#ifndef HOBBYOS_IRQ_LATENCY_TEST_H
#define HOBBYOS_IRQ_LATENCY_TEST_H
#include <stdbool.h>
#include <stdint.h>
bool irq_latency_test_reporting(void);
int irq_latency_test_begin(void);
int irq_latency_test_stress(void);
int irq_latency_test_finish(void);
int irq_latency_test_audit(void);
int irq_latency_test_proof(bool deferred, uint64_t session, unsigned invocations);
int irq_latency_test_export(void);
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
typedef struct {
    uint64_t maximum_ns;
    uint32_t cells, missing, invalid, unqualified, over_budget, maximum_cpu, maximum_path;
} irq_latency_test_summary_t;
bool irq_latency_test_summary(irq_latency_test_summary_t *out);
#endif
#endif
