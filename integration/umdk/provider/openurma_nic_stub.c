// SPDX-License-Identifier: Apache-2.0
//
// M1 stub for openurma_nic. Lets the provider link and lets discovery +
// context-create work before the SystemC pipeline is wired in (M2/M3 replace
// this with openurma_nic.cpp built against runtime/openurma's SC facade).
#include "openurma_nic.h"

struct openurma_nic *openurma_nic_create(uint32_t local_cna) { (void)local_cna; return 0; }
void openurma_nic_destroy(struct openurma_nic *nic) { (void)nic; }
int openurma_nic_submit_wr(struct openurma_nic *nic, const uint8_t flit[64]) { (void)nic;(void)flit; return 0; }
int openurma_nic_poll_cqe(struct openurma_nic *nic, uint8_t flit_out[64]) { (void)nic;(void)flit_out; return 0; }
void openurma_nic_pump(struct openurma_nic *nic, uint64_t budget_ns) { (void)nic;(void)budget_ns; }
