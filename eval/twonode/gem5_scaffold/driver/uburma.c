// SPDX-License-Identifier: GPL-2.0
//
// uburma — minimal Linux char-device driver for the OpenURMA UBController
// gem5 SimObject. Exposes /dev/uburma0 with the following ioctls:
//
//   UBURMA_CREATE_JETTY    — allocate a Jetty handle
//   UBURMA_CREATE_TPCH     — allocate a TP Channel to a peer host
//   UBURMA_REG_MR          — register a memory region (kva → MR token)
//   UBURMA_POST_WR         — post a Work Request via the doorbell
//   UBURMA_POLL_CQ         — poll the CQ for completions
//   UBURMA_REQ_NOTIFY_CQ   — arm CQ event notification (MSI-X bridge)
//
// mmap support:
//   - first 4 KB page  → doorbell BAR (one Jetty per doorbell page in a
//     fuller impl; this scaffold serves a single Jetty)
//   - next 4 KB page   → CQ ring buffer (cacheable WB)
//
// MSI-X path:
//   On CQ-notify-request, the UBController fires an interrupt routed to
//   our driver's ISR, which wakes any process blocked on poll()/read().
//
// Status:
//   This file is a structural skeleton. It compiles against a recent
//   ARM64 mainline kernel via the standalone driver Makefile in the
//   same directory. End-to-end exercise requires:
//     1. A booted gem5 FS-mode ARM Linux image (Phase 4.1 below).
//     2. The UBController exposing an IntSourcePin gem5 wires to the
//        ARM GIC so the driver's ISR fires (Phase 4.2 patch).
//     3. A userspace liburma.so that issues these ioctls.
//   None of those are yet present in the artifact; this file is the
//   first of three components, the one with the cleanest scope.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/poll.h>

#define UBURMA_DRV_NAME    "uburma"
#define UBURMA_CLASS_NAME  "uburma"

// gem5 UBController aperture; for the production driver this would come
// from the DT or PCI BAR but for the gem5 scaffold we hard-code.
#define UBURMA_PHYS_BASE   0x2D000000UL  // matches dual_node_fs_clean.py aperture
#define UBURMA_DB_OFFSET   0x0
// NICTopologySC.mmio_b decodes accesses by local offset:
//   - 0x00..0x3F : doorbell WR (CPU's 8-byte stores assembled into a 64-B flit)
//   - 0x40..0x7F : CQ poll slot (one 64-B flit popped from cqe_queue_)
// The CQ slot is contiguous with the doorbell, NOT page-aligned — older
// versions of this driver used 0x1000 which falls into the "unmapped"
// region NICTopologySC pads with zero bytes, so POLL_CQ always returned
// an empty CQE. Keep the offset in sync with NICTopologySC::CQ_OFFSET.
#define UBURMA_CQ_OFFSET   0x40
#define UBURMA_APERTURE_SZ 0x10000UL

struct uburma_dev {
    struct cdev cdev;
    dev_t       devt;
    struct class *class;
    struct device *dev;
    void __iomem *iomem;
    int          irq;
    wait_queue_head_t cq_wait;
    atomic_t     cq_ready;
};

static struct uburma_dev *udev;

// ---- ioctl interface ------------------------------------------------------
#define UBURMA_IOC_MAGIC 'u'
struct uburma_create_jetty_arg {
    __u32 svc_modes_mask;  // bitmask of allowed SVC modes
    __u32 jetty_id;        // returned
};
struct uburma_post_wr_arg {
    __u64 meta[8];         // metadata flit (64 B)
    __u64 ext[8];          // extension flit (64 B)
};
struct uburma_poll_cq_arg {
    __u64 cqe[8];          // returned CQE flit
    __u32 valid;
};
#define UBURMA_IOC_CREATE_JETTY  _IOWR(UBURMA_IOC_MAGIC, 1, struct uburma_create_jetty_arg)
#define UBURMA_IOC_POST_WR       _IOW (UBURMA_IOC_MAGIC, 2, struct uburma_post_wr_arg)
#define UBURMA_IOC_POLL_CQ       _IOR (UBURMA_IOC_MAGIC, 3, struct uburma_poll_cq_arg)
#define UBURMA_IOC_REQ_NOTIFY_CQ _IO  (UBURMA_IOC_MAGIC, 4)

static long uburma_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case UBURMA_IOC_CREATE_JETTY: {
        struct uburma_create_jetty_arg ka;
        if (copy_from_user(&ka, (void __user *)arg, sizeof(ka)))
            return -EFAULT;
        // Scaffold: always return jetty_id 0 for the first call.
        ka.jetty_id = 0;
        if (copy_to_user((void __user *)arg, &ka, sizeof(ka)))
            return -EFAULT;
        return 0;
    }
    case UBURMA_IOC_POST_WR: {
        struct uburma_post_wr_arg ka;
        if (copy_from_user(&ka, (void __user *)arg, sizeof(ka)))
            return -EFAULT;
        // Two consecutive 64-B writes to the doorbell page.
        memcpy_toio(udev->iomem + UBURMA_DB_OFFSET, ka.meta, 64);
        wmb();
        memcpy_toio(udev->iomem + UBURMA_DB_OFFSET, ka.ext, 64);
        wmb();
        // Synthetic ISR: under loopback_ack the UBController has
        // already pushed a CQE into the queue. Pretend we got an
        // interrupt — wake the poll() waiter. This captures the
        // syscall + sleep + wake + return overhead that FastWake
        // characterises, without the MSI-X delivery itself (which
        // requires DT-based irqdomain wiring that the gem5
        // miscdevice-mode driver cannot construct without DT).
        atomic_set(&udev->cq_ready, 1);
        wake_up_interruptible(&udev->cq_wait);
        return 0;
    }
    case UBURMA_IOC_POLL_CQ: {
        struct uburma_poll_cq_arg ka;
        memcpy_fromio(ka.cqe, udev->iomem + UBURMA_CQ_OFFSET, 64);
        ka.valid = (ka.cqe[0] != 0);
        if (copy_to_user((void __user *)arg, &ka, sizeof(ka)))
            return -EFAULT;
        return 0;
    }
    case UBURMA_IOC_REQ_NOTIFY_CQ:
        atomic_set(&udev->cq_ready, 0);
        // Real driver writes an "arm" bit to the NIC; for the scaffold
        // this is a no-op; ISR sets cq_ready and wakes cq_wait.
        return 0;
    default:
        return -ENOTTY;
    }
}

// ---- mmap: expose the doorbell + CQ pages to userspace -------------------
//
// The vm_pgoff (mmap "offset" / PAGE_SIZE) is overloaded as a policy
// selector so userspace can request a specific page-protection
// attribute on the NIC aperture. This lets the cache-policy
// sensitivity sweep (Exp 12 in the paper) measure §8.3 LD/ST under
// each policy without rebuilding the kernel module.
//
//   pgoff 0           → uncached (pgprot_noncached) — default
//   pgoff 0x10000     → write-through (pgprot_writecombine is the
//                       closest ARM64 mapping; spec-level WT)
//   pgoff 0x20000     → write-back (vm_page_prot unmodified)
//
// Regardless of pgoff, the mapping always covers the same physical
// aperture starting at UBURMA_PHYS_BASE.
#define UBURMA_PGPROT_UC_PGOFF 0x00000UL
#define UBURMA_PGPROT_WT_PGOFF 0x10000UL
#define UBURMA_PGPROT_WB_PGOFF 0x20000UL

static int uburma_mmap(struct file *f, struct vm_area_struct *vma)
{
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long pol = vma->vm_pgoff;
    if (size > UBURMA_APERTURE_SZ) return -EINVAL;
    if (pol == UBURMA_PGPROT_WB_PGOFF) {
        // Leave vma->vm_page_prot at its default (cacheable WB).
    } else if (pol == UBURMA_PGPROT_WT_PGOFF) {
        vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    } else {
        // Default: uncached (UC).
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    }
    // Clear vm_pgoff so remap_pfn_range maps from physical base, not
    // an offset into the aperture.
    vma->vm_pgoff = 0;
    return remap_pfn_range(vma, vma->vm_start,
                           UBURMA_PHYS_BASE >> PAGE_SHIFT, size,
                           vma->vm_page_prot);
}

// ---- poll(): block until CQ-ready (ISR fires) ----------------------------
// Use unsigned int (not __poll_t) for kernel 4.14 compat; 5.0+ aliases.
static unsigned int uburma_poll(struct file *f, poll_table *wait)
{
    poll_wait(f, &udev->cq_wait, wait);
    return atomic_read(&udev->cq_ready) ? (POLLIN | POLLRDNORM) : 0;
}

static const struct file_operations uburma_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = uburma_ioctl,
    .mmap           = uburma_mmap,
    .poll           = uburma_poll,
};

// ---- ISR: woken by UBController IntSourcePin -----------------------------
static irqreturn_t uburma_isr(int irq, void *dev_id)
{
    struct uburma_dev *d = dev_id;
    atomic_set(&d->cq_ready, 1);
    wake_up_interruptible(&d->cq_wait);
    return IRQ_HANDLED;
}

// ---- miscdevice registration ---------------------------------------------
// Use miscdevice so /dev/uburma0 appears as soon as the module loads,
// without needing a platform DT node. The NIC aperture is a build-time
// constant (UBURMA_PHYS_BASE); ioremap'd at init.

// Hardcode a fixed minor so /dev/uburma0 mknod from tiny_init can use
// it without discovery (sysfs / /proc/misc lookup in this minimal
// initramfs has been unreliable). 222 is well away from standard
// kernel-reserved minors and inside the MISC dynamic range.
#define UBURMA_FIXED_MINOR 222

static struct miscdevice uburma_misc = {
    .minor = UBURMA_FIXED_MINOR,
    .name  = "uburma0",
    .fops  = &uburma_fops,
};

static int __init uburma_init(void)
{
    int err;
    udev = kzalloc(sizeof(*udev), GFP_KERNEL);
    if (!udev) return -ENOMEM;

    udev->iomem = ioremap(UBURMA_PHYS_BASE, UBURMA_APERTURE_SZ);
    if (!udev->iomem) { kfree(udev); return -ENOMEM; }

    init_waitqueue_head(&udev->cq_wait);
    atomic_set(&udev->cq_ready, 0);
    udev->irq = -1;

    err = misc_register(&uburma_misc);
    if (err) {
        iounmap(udev->iomem);
        kfree(udev);
        return err;
    }
    pr_info("uburma: registered /dev/uburma0, iomem=%p (phys 0x%lx)\n",
            udev->iomem, (unsigned long)UBURMA_PHYS_BASE);
    return 0;
}

static void __exit uburma_exit(void)
{
    misc_deregister(&uburma_misc);
    if (udev) {
        if (udev->iomem) iounmap(udev->iomem);
        kfree(udev);
    }
}

module_init(uburma_init);
module_exit(uburma_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("OpenURMA contributors");
MODULE_DESCRIPTION("uburma: char-device driver for gem5 UBController");
