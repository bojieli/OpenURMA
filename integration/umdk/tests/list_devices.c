// SPDX-License-Identifier: Apache-2.0
//
// Tier-S discovery gate (gate 1). Uses ONLY the stock liburma public API —
// exactly the path urma_perftest/urma_sample take (urma_init →
// urma_get_device_list → urma_get_device_by_name → urma_create_context) — to
// prove a stock liburma client discovers and opens the OpenURMA device with no
// kernel module present. (urma_admin's `show` goes over netlink to ubcore and
// belongs to the kernel-backed Tier G, not here.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "urma_api.h"
#include "urma_types.h"

int main(int argc, char **argv)
{
    const char *want = (argc > 1) ? argv[1] : "openurma0";

    urma_init_attr_t init = {0};
    if (urma_init(&init) != URMA_SUCCESS) {
        fprintf(stderr, "FAIL: urma_init\n");
        return 2;
    }

    int n = 0;
    urma_device_t **list = urma_get_device_list(&n);
    printf("urma_get_device_list: %d device(s)\n", n);
    for (int i = 0; i < n; i++)
        printf("  [%d] name=%s type=%d path=%s\n", i, list[i]->name, list[i]->type, list[i]->path);

    urma_device_t *dev = urma_get_device_by_name((char *)want);
    if (!dev) {
        fprintf(stderr, "FAIL: device '%s' not found\n", want);
        return 3;
    }

    urma_device_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    if (urma_query_device(dev, &attr) == URMA_SUCCESS) {
        printf("query_device '%s': max_jetty=%u max_jfc=%u max_jfs_depth=%u "
               "max_msg_size=%lu trans_mode=0x%x\n",
               want, attr.dev_cap.max_jetty, attr.dev_cap.max_jfc,
               attr.dev_cap.max_jfs_depth,
               (unsigned long)attr.dev_cap.max_msg_size,
               attr.dev_cap.trans_mode);
    }

    urma_context_t *ctx = urma_create_context(dev, 0);
    if (!ctx) {
        fprintf(stderr, "FAIL: urma_create_context('%s')\n", want);
        return 4;
    }
    printf("create_context OK: eid_index=%u dev_fd=%d\n", ctx->eid_index, ctx->dev_fd);

    urma_delete_context(ctx);
    urma_free_device_list(list);
    urma_uninit();
    printf("PASS: discovered + opened '%s' on stock liburma (Tier S, no kernel)\n", want);
    return 0;
}
