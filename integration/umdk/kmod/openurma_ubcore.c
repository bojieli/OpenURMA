// SPDX-License-Identifier: GPL-2.0
//
// openurma_ubcore — OpenURMA provider for the OFFICIAL openEuler ubcore kernel
// subsystem (drivers/ub/urma/ubcore). This is the kernel-side analogue of the
// HiSilicon hns3_udma provider: it registers a ubcore_device and implements the
// ubcore_ops vtable, so the *unmodified* official stack —
//     stock liburma (UMDK) → /dev/uburma (uburma.ko) → ubcore.ko → THIS driver
// — runs on OpenURMA. With this loaded, stock urma_admin / urma_perftest /
// URPC run in-guest with no userspace shims (cf. the Tier-S LD_PRELOAD path).
//
// The data path drives the OpenURMA NIC MMIO aperture (the gem5 NICTopologySC
// SimObject at OPENURMA_APERTURE, the same aperture the gem5 scaffold's minimal
// uburma.ko uses) by assembling 64-byte UB doorbell flits and reading CQE flits
// — identical wire/flit layout to the userspace provider
// (integration/umdk/provider/openurma_provider.c) and ub_flit.hpp. On the U50
// the same driver binds the PCIe BAR instead (bus HAL, future).
//
// STATUS: RUNS IN-GUEST. Built against the real openEuler OLK-5.10 ubcore and
// loaded in a cross-built OLK-5.10 gem5 ARM guest: the official stack
// liburma → uburma.ko → ubcore.ko → THIS driver enumerates the OpenURMA UB
// device (stock urma_admin show). It implements alloc_ucontext/mmap so a
// userspace provider (provider/openurma_provider_kernel.c) can mmap the doorbell/
// CQ aperture for the data path. NOTE: the in-guest verbs DATA path additionally
// needs the kernel uburma ioctl ABI to match the UMDK userspace: UMDK (v25.12.0+,
// our pin) uses TLV-encoded ioctls, which OLK-5.10's uburma predates — so the
// data plane needs a TLV-capable kernel (OLK-6.6/newer). See gem5/README.md.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <net/net_namespace.h>

#include <urma/ubcore_types.h>
#include <urma/ubcore_api.h>

#define OPENURMA_DRV_NAME   "openurma"
#define OPENURMA_ABI_VER    1
#define OPENURMA_APERTURE   0x2D000000UL   /* matches single_node_fs_clean.py */
#define OPENURMA_APER_SZ    0x10000UL
#define OPENURMA_DB_OFFSET  0x0            /* doorbell: 64-B WR flit */
#define OPENURMA_CQ_OFFSET  0x40           /* CQ poll slot: 64-B CQE flit */

/* TAOpcode subset (ub_flit.hpp) */
#define TAOP_SEND  0x00
#define TAOP_WRITE 0x03
#define TAOP_READ  0x06

struct openurma_dev {
	struct ubcore_device ubc;     /* embedded ubcore device */
	void __iomem        *aper;    /* ioremap'd NIC aperture */
	u32                  cna;     /* local 24-bit UB CNA */
	atomic_t             jetty_seq;
	atomic_t             token_seq;
};

static struct openurma_dev *g_dev;

static inline struct openurma_dev *to_ou(struct ubcore_device *d)
{
	return container_of(d, struct openurma_dev, ubc);
}

/* ---- assemble + ring a 64-byte doorbell WR flit (lane layout per ub_flit.hpp) */
static void ou_lane_set(u64 *flit, int lane, int lo, int w, u64 v)
{
	u64 mask = ((w >= 64) ? ~0ULL : ((1ULL << w) - 1)) << lo;
	flit[lane] = (flit[lane] & ~mask) | ((v & ((w >= 64) ? ~0ULL : ((1ULL << w) - 1))) << lo);
}

static void ou_ring_wr(struct openurma_dev *od, u8 taop, u32 dcna, u16 tassn,
		       u64 remote_va, u32 token_id, u32 len)
{
	u64 meta[8] = {0}, ext[8] = {0};
	int i;

	ou_lane_set(meta, 0, 0, 24, dcna);        /* NTH.dcna   */
	ou_lane_set(meta, 0, 60, 3, 0x2);         /* NTH.nlp = RTPH */
	ou_lane_set(meta, 0, 63, 1, 1);           /* valid      */
	ou_lane_set(meta, 2, 58, 2, 0);           /* svc = ROI  */
	ou_lane_set(meta, 2, 61, 1, 1);           /* last_pkt   */
	ou_lane_set(meta, 3, 0, 8, taop);         /* BTAH.taop  */
	ou_lane_set(meta, 3, 12, 1, 1);           /* tv_en      */
	ou_lane_set(meta, 3, 16, 16, tassn);      /* ini_tassn  */
	ou_lane_set(meta, 3, 43, 20, 7);          /* ini_rc_id  */
	((u8 *)meta)[32] = 0x01;                  /* sop        */

	ext[0] = remote_va;
	ext[1] = (u64)token_id | ((u64)len << 32);
	ext[2] = 0xDEADBEEFULL;                   /* token value */
	((u8 *)ext)[32] = 0x02;                   /* eop        */

	/* CPU issues 8x 8-byte stores per 64-B doorbell slot; NICTopologySC
	 * assembles them into a flit and fires submit_wr when complete. */
	for (i = 0; i < 8; i++)
		writeq(meta[i], od->aper + OPENURMA_DB_OFFSET + i * 8);
	wmb();
	for (i = 0; i < 8; i++)
		writeq(ext[i], od->aper + OPENURMA_DB_OFFSET + i * 8);
	wmb();
}

/* ============================ ubcore_ops ============================ */

/* Userspace context: uburma_cmd_create_ctx → ubcore_alloc_ucontext → here. */
static struct ubcore_ucontext *ou_alloc_ucontext(struct ubcore_device *dev,
						 uint32_t eid_index,
						 struct ubcore_udrv_priv *udrv)
{
	struct ubcore_ucontext *uc = kzalloc(sizeof(*uc), GFP_KERNEL);
	(void)udrv;
	if (!uc)
		return NULL;
	uc->ub_dev = dev;
	uc->eid_index = eid_index;
	pr_info("openurma: alloc_ucontext eid_index=%u\n", eid_index);
	return uc;
}
static int ou_free_ucontext(struct ubcore_ucontext *uc) { kfree(uc); return 0; }

/* Map the NIC doorbell + CQ aperture into userspace so the userspace provider
 * can ring WR flits and read CQE flits directly (the RDMA fast path). uburma's
 * file mmap forwards here. We map the single 64-KiB aperture uncached. */
static int ou_mmap(struct ubcore_ucontext *ctx, struct vm_area_struct *vma)
{
	size_t sz = vma->vm_end - vma->vm_start;
	(void)ctx;
	if (sz > OPENURMA_APER_SZ)
		return -EINVAL;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	pr_info("openurma: mmap aperture sz=%zu -> user\n", sz);
	return remap_pfn_range(vma, vma->vm_start,
			       OPENURMA_APERTURE >> PAGE_SHIFT, sz,
			       vma->vm_page_prot);
}

static int ou_config_device(struct ubcore_device *dev, struct ubcore_device_cfg *cfg)
{
	(void)to_ou(dev); (void)cfg;
	return 0;
}

static int ou_query_device_attr(struct ubcore_device *dev, struct ubcore_device_attr *attr)
{
	/* ubcore copies dev->attr (populated at register time) by default; the
	 * driver may refine here. Generous caps mirror the Tier-S sysfs. */
	*attr = dev->attr;
	return 0;
}

static struct ubcore_token_id *ou_alloc_token_id(struct ubcore_device *dev,
						 union ubcore_token_id_flag flag,
						 struct ubcore_udata *udata)
{
	struct openurma_dev *od = to_ou(dev);
	struct ubcore_token_id *t = kzalloc(sizeof(*t), GFP_KERNEL);
	(void)flag; (void)udata;
	if (!t)
		return NULL;
	t->token_id = atomic_inc_return(&od->token_seq) & 0x3F; /* 64 MR slots */
	t->ub_dev = dev;
	return t;
}
/* ubcore_alloc_token_id requires BOTH alloc + free ops to be set. */
static int ou_free_token_id(struct ubcore_token_id *t) { kfree(t); return 0; }

static struct ubcore_target_seg *ou_register_seg(struct ubcore_device *dev,
						 struct ubcore_seg_cfg *cfg,
						 struct ubcore_udata *udata)
{
	struct openurma_dev *od = to_ou(dev);
	struct ubcore_target_seg *s = kzalloc(sizeof(*s), GFP_KERNEL);
	(void)udata;
	if (!s)
		return NULL;
	s->ub_dev = dev;
	s->seg.ubva.va = cfg->va;
	s->seg.len = cfg->len;
	s->seg.token_id = atomic_inc_return(&od->token_seq) & 0x3F;
	pr_info("openurma: register_seg va=0x%llx len=%llu (via ubcore)\n", (unsigned long long)cfg->va, (unsigned long long)cfg->len);
	return s;
}
static int ou_unregister_seg(struct ubcore_target_seg *s) { kfree(s); return 0; }

static struct ubcore_target_seg *ou_import_seg(struct ubcore_device *dev,
					       struct ubcore_target_seg_cfg *cfg,
					       struct ubcore_udata *udata)
{
	struct ubcore_target_seg *s = kzalloc(sizeof(*s), GFP_KERNEL);
	(void)udata;
	if (!s)
		return NULL;
	s->ub_dev = dev;
	s->seg.ubva = cfg->seg.ubva;
	s->seg.len = cfg->seg.len;
	s->seg.token_id = cfg->seg.token_id;
	return s;
}
static int ou_unimport_seg(struct ubcore_target_seg *s) { kfree(s); return 0; }

static struct ubcore_jfc *ou_create_jfc(struct ubcore_device *dev,
					struct ubcore_jfc_cfg *cfg,
					struct ubcore_udata *udata)
{
	struct ubcore_jfc *jfc = kzalloc(sizeof(*jfc), GFP_KERNEL);
	(void)udata;
	if (!jfc)
		return NULL;
	jfc->ub_dev = dev;
	jfc->jfc_cfg = *cfg;
	return jfc;
}
static int ou_destroy_jfc(struct ubcore_jfc *jfc) { kfree(jfc); return 0; }

static struct ubcore_jfr *ou_create_jfr(struct ubcore_device *dev,
					struct ubcore_jfr_cfg *cfg,
					struct ubcore_udata *udata)
{
	struct ubcore_jfr *jfr = kzalloc(sizeof(*jfr), GFP_KERNEL);
	(void)udata;
	if (!jfr)
		return NULL;
	jfr->ub_dev = dev;
	jfr->jfr_cfg = *cfg;
	return jfr;
}
static int ou_destroy_jfr(struct ubcore_jfr *jfr) { kfree(jfr); return 0; }

static struct ubcore_jetty *ou_create_jetty(struct ubcore_device *dev,
					    struct ubcore_jetty_cfg *cfg,
					    struct ubcore_udata *udata)
{
	struct openurma_dev *od = to_ou(dev);
	struct ubcore_jetty *j = kzalloc(sizeof(*j), GFP_KERNEL);
	(void)udata;
	if (!j)
		return NULL;
	j->ub_dev = dev;
	j->jetty_cfg = *cfg;
	j->jetty_id.id = atomic_inc_return(&od->jetty_seq);
	pr_info("openurma: create_jetty id=%u (via ubcore)\n", j->jetty_id.id);
	return j;
}
static int ou_destroy_jetty(struct ubcore_jetty *j) { kfree(j); return 0; }

static struct ubcore_tjetty *ou_import_jetty(struct ubcore_device *dev,
					     struct ubcore_tjetty_cfg *cfg,
					     struct ubcore_udata *udata)
{
	struct ubcore_tjetty *t = kzalloc(sizeof(*t), GFP_KERNEL);
	(void)udata;
	if (!t)
		return NULL;
	t->ub_dev = dev;
	t->cfg = *cfg;
	return t;
}
static int ou_unimport_jetty(struct ubcore_tjetty *t) { kfree(t); return 0; }

static int ou_post_jetty_send_wr(struct ubcore_jetty *jetty, struct ubcore_jfs_wr *wr,
				 struct ubcore_jfs_wr **bad_wr)
{
	struct openurma_dev *od = to_ou(jetty->ub_dev);
	u8 taop = TAOP_WRITE;
	u64 rva = 0; u32 tid = 0, len = 0;

	for (; wr; wr = wr->next) {
		switch (wr->opcode) {
		case UBCORE_OPC_WRITE:
			taop = TAOP_WRITE;
			rva = wr->rw.dst.sge ? wr->rw.dst.sge[0].addr : 0;
			len = wr->rw.src.sge ? wr->rw.src.sge[0].len : 0;
			break;
		case UBCORE_OPC_READ:
			taop = TAOP_READ;
			rva = wr->rw.src.sge ? wr->rw.src.sge[0].addr : 0;
			len = wr->rw.dst.sge ? wr->rw.dst.sge[0].len : 0;
			break;
		case UBCORE_OPC_SEND:
		default:
			taop = TAOP_SEND;
			len = wr->send.src.sge ? wr->send.src.sge[0].len : 0;
			break;
		}
		ou_ring_wr(od, taop, jetty->jetty_id.id, 0, rva, tid, len);
	}
	if (bad_wr)
		*bad_wr = NULL;
	return 0;
}

static int ou_post_jetty_recv_wr(struct ubcore_jetty *jetty, struct ubcore_jfr_wr *wr,
				 struct ubcore_jfr_wr **bad_wr)
{
	(void)jetty; (void)wr;
	if (bad_wr)
		*bad_wr = NULL;
	return 0;
}

static int ou_poll_jfc(struct ubcore_jfc *jfc, int cr_cnt, struct ubcore_cr *cr)
{
	struct openurma_dev *od = to_ou(jfc->ub_dev);
	u64 cqe[8];
	int i, n = 0;

	while (n < cr_cnt) {
		for (i = 0; i < 8; i++)
			cqe[i] = readq(od->aper + OPENURMA_CQ_OFFSET + i * 8);
		if (cqe[0] == 0)              /* empty slot */
			break;
		memset(&cr[n], 0, sizeof(cr[n]));
		cr[n].status = UBCORE_CR_SUCCESS;
		cr[n].completion_len = (u32)(cqe[0] >> 32);
		n++;
	}
	return n;
}

static struct ubcore_ops g_openurma_ubcore_ops = {
	.owner             = THIS_MODULE,
	.driver_name       = OPENURMA_DRV_NAME,
	.abi_version       = OPENURMA_ABI_VER,
	.alloc_ucontext    = ou_alloc_ucontext,
	.free_ucontext     = ou_free_ucontext,
	.mmap              = ou_mmap,
	.config_device     = ou_config_device,
	.query_device_attr = ou_query_device_attr,
	.alloc_token_id    = ou_alloc_token_id,
	.free_token_id     = ou_free_token_id,
	.register_seg      = ou_register_seg,
	.unregister_seg    = ou_unregister_seg,
	.import_seg        = ou_import_seg,
	.unimport_seg      = ou_unimport_seg,
	.create_jfc        = ou_create_jfc,
	.destroy_jfc       = ou_destroy_jfc,
	.create_jfr        = ou_create_jfr,
	.destroy_jfr       = ou_destroy_jfr,
	.create_jetty      = ou_create_jetty,
	.destroy_jetty     = ou_destroy_jetty,
	.import_jetty      = ou_import_jetty,
	.unimport_jetty    = ou_unimport_jetty,
	.post_jetty_send_wr = ou_post_jetty_send_wr,
	.post_jetty_recv_wr = ou_post_jetty_recv_wr,
	.poll_jfc          = ou_poll_jfc,
	/* TP/TPG/VTP/UTP/jetty-group/atomic ops: ubcore tolerates NULL or the
	 * driver returns -EOPNOTSUPP, matching the OpenURMA MVP cuts. */
};

static int __init openurma_ubcore_init(void)
{
	struct openurma_dev *od;
	int ret;

	od = kzalloc(sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	od->aper = ioremap(OPENURMA_APERTURE, OPENURMA_APER_SZ);
	if (!od->aper) {
		kfree(od);
		return -ENOMEM;
	}
	od->cna = 0xABC123;
	atomic_set(&od->jetty_seq, 1);
	atomic_set(&od->token_seq, 1);

	strscpy(od->ubc.dev_name, "openurma0", UBCORE_MAX_DEV_NAME);
	od->ubc.ops = &g_openurma_ubcore_ops;
	od->ubc.transport_type = UBCORE_TRANSPORT_UB;

	/* Generous caps (mirror the Tier-S sysfs) so liburma's client-side checks
	 * accept whatever the apps request. max_eid_cnt=1 makes ubcore create the
	 * eids/eid0 sysfs that liburma's create_context reads. */
	od->ubc.attr.dev_cap.max_eid_cnt          = 1;
	od->ubc.attr.dev_cap.max_jfc              = 1u << 20;
	od->ubc.attr.dev_cap.max_jfs              = 1u << 20;
	od->ubc.attr.dev_cap.max_jfr              = 1u << 20;
	od->ubc.attr.dev_cap.max_jetty            = 1u << 20;
	od->ubc.attr.dev_cap.max_jfc_depth        = 1u << 16;
	od->ubc.attr.dev_cap.max_jfs_depth        = 1u << 16;
	od->ubc.attr.dev_cap.max_jfr_depth        = 1u << 16;
	od->ubc.attr.dev_cap.max_jfs_inline_size  = 912;
	od->ubc.attr.dev_cap.max_jfs_sge          = 8;
	od->ubc.attr.dev_cap.max_jfs_rsge         = 8;
	od->ubc.attr.dev_cap.max_jfr_sge          = 8;
	od->ubc.attr.dev_cap.max_msg_size         = 1ull << 31;
	od->ubc.attr.dev_cap.trans_mode           = 0x7; /* RM|RC|UM */
	od->ubc.attr.reserved_jetty_id_min        = 0;
	od->ubc.attr.reserved_jetty_id_max        = 0;

	ret = ubcore_register_device(&od->ubc);
	if (ret) {
		pr_err("openurma: ubcore_register_device failed: %d\n", ret);
		iounmap(od->aper);
		kfree(od);
		return ret;
	}

	/* Populate a static EID at index 0 (ubcore allocated eid_entries for
	 * max_eid_cnt at register). liburma reads /sys/class/ubcore/openurma0/
	 * eids/eid0 during create_context. EID = fe80::1 (non-zero → valid). */
	if (od->ubc.eid_table.eid_entries && od->ubc.eid_table.eid_cnt >= 1) {
		spin_lock(&od->ubc.eid_table.lock);
		od->ubc.eid_table.eid_entries[0].eid.raw[0]  = 0xfe;
		od->ubc.eid_table.eid_entries[0].eid.raw[1]  = 0x80;
		od->ubc.eid_table.eid_entries[0].eid.raw[15] = 0x01;
		od->ubc.eid_table.eid_entries[0].eid_index   = 0;
		od->ubc.eid_table.eid_entries[0].net         = &init_net;
		od->ubc.eid_table.eid_entries[0].valid       = true;
		spin_unlock(&od->ubc.eid_table.lock);
		pr_info("openurma: eid[0] = fe80::1 set\n");
	}
	g_dev = od;
	pr_info("openurma: registered ubcore device 'openurma0' (UB), aperture 0x%lx\n",
		OPENURMA_APERTURE);
	return 0;
}

static void __exit openurma_ubcore_exit(void)
{
	if (g_dev) {
		ubcore_unregister_device(&g_dev->ubc);
		if (g_dev->aper)
			iounmap(g_dev->aper);
		kfree(g_dev);
		g_dev = NULL;
	}
}

module_init(openurma_ubcore_init);
module_exit(openurma_ubcore_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("OpenURMA contributors");
MODULE_DESCRIPTION("OpenURMA provider for the official openEuler ubcore subsystem");
