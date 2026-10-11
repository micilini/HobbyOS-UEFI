#include "cmd_irqlatency.h"
#include "../../memory/irq_latency_test.h"
#include "../../libc/string.h"
#include "../diagnostic_result.h"

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
#include "cmd_mem.h"
#include "../../memory/guard_probe.h"
#include "../../drivers/serial.h"
#include "../../smp/smp_topology.h"
#include "../../smp/cpu_request.h"
#include "../../core/runtime_ready.h"
#include "../../core/clock.h"
#include "../../core/scheduler.h"
#if !defined(HOBBYOS_SERIAL_PROOF_ID) || HOBBYOS_SERIAL_PROOF_ID == 0
#error Serial diagnostic requires a nonzero immutable build identity
#endif

static bool physical_serial_wait_boot(void)
{
    if (!cpu_request_context_allowed()) return false;
    if (runtime_ready_boot_diagnostics_finished()) return true;
    serial_write_all("\n[PHYSICAL_LATENCY][WAIT] stage=boot_diagnostics timeout_ns=30000000000\n");
    uint64_t start,previous,now;
    if (!clock_monotonic_read_ns(&start) || start>UINT64_MAX-UINT64_C(30000000000)) return false;
    previous=start;unsigned stalled=0;
    while (!runtime_ready_boot_diagnostics_finished()) {
        if (!clock_monotonic_read_ns(&now) || now<previous || now>=start+UINT64_C(30000000000)) return false;
        stalled=now==previous ? stalled+1u : 0;
        if (stalled>=1024u) return false;
        previous=now;schedule();
    }
    return clock_monotonic_read_ns(&now) && now>=previous && now<start+UINT64_C(30000000000);
}

static int physical_serial_start_failure(uint64_t session,const char *stage,bool reference)
{
    (void)stack_test_emit_checked("[PHYSICAL_LATENCY][END] version=%u build=0x%llx stage=%s status=INCOMPLETE timing_acceptance=NOT_GRANTED\n",
                                 reference ? 5u : 4u,(unsigned long long)session,stage);
    return 1;
}

static int physical_serial_run(bool reference)
{
    uint64_t session=HOBBYOS_SERIAL_PROOF_ID;
    if (!physical_serial_wait_boot()) return physical_serial_start_failure(session,"boot_wait",reference);
    if (!runtime_ready_is_test_ready()) return physical_serial_start_failure(session,"boot_not_ready",reference);
    if (!stack_test_emit_checked("[PHYSICAL_LATENCY][BEGIN] version=%u build=0x%llx cpus=%u role=PHYSICAL_DIAGNOSTIC transport=uart-115200-8n1 proof_output=3 budget_ns=50000 boot_ready=%u%s\n",
                        reference ? 5u : 4u,(unsigned long long)session,g_cpu_count,runtime_ready_is_test_ready()?1u:0u,
                        reference ? " budget_role=REFERENCE_ONLY policy=linux-comparative-v1" : ""))
        return physical_serial_start_failure(session,"opening_transport",reference);
    int prepared=irq_latency_test_proof(true,session,1);
    int begun=prepared ? 1 : irq_latency_test_begin();
    if (prepared || begun) {
        (void)stack_test_emit_checked("[PHYSICAL_LATENCY][END] version=%u build=0x%llx prepared=%d begin=%d status=INCOMPLETE timing_acceptance=NOT_GRANTED\n",
                            reference ? 5u : 4u,(unsigned long long)session,prepared,begun);
        return 1;
    }
    char *args[]={"mem"};
    int memory=cmd_mem(1,args);
    int stress=irq_latency_test_stress();
    int finish=irq_latency_test_finish();
    /* A rejected timing postcheck still needs the terminal audit and proof. */
    int audit=irq_latency_test_audit();
    int exported=audit ? 1 : irq_latency_test_export();
    bool complete=!memory && !stress && !audit && !exported;
    irq_latency_test_summary_t summary;
    bool available=irq_latency_test_summary(&summary);
    bool qualified=available && !finish && !summary.missing && !summary.invalid && !summary.unqualified;
    bool below=qualified && !summary.over_budget;
    uint32_t uart_failed_ports=serial_proof_failure_mask();
    uint64_t aux_records,aux_bytes;serial_proof_aux_totals(&aux_records,&aux_bytes);
    bool emitted=stack_test_emit_checked("[PHYSICAL_LATENCY][END] version=%u build=0x%llx prepared=%d begin=%d memory=%d stress=%d finish=%d audit=%d export=%d status=%s aux_records=%llu aux_bytes=%llu uart_failed_ports=0x%x summary_available=%u cells=%u missing=%u invalid=%u unqualified=%u over_budget=%u maximum_ns=%llu maximum_cpu=%u maximum_path=%u measurement=%s%s timing_acceptance=NOT_GRANTED\n",
        reference ? 5u : 4u,(unsigned long long)session,prepared,begun,memory,stress,finish,audit,exported,
        complete ? "CAPTURE_COMPLETE" : "INCOMPLETE",(unsigned long long)aux_records,(unsigned long long)aux_bytes,
        uart_failed_ports,available,summary.cells,summary.missing,summary.invalid,summary.unqualified,summary.over_budget,
        (unsigned long long)summary.maximum_ns,summary.maximum_cpu,summary.maximum_path,
        !available ? "UNAVAILABLE" : reference ? (qualified ? "QUALIFIED" : "UNQUALIFIED") :
            below ? "BELOW_BUDGET_OBSERVED" : "REJECTED",
        reference ? (!qualified ? " policy=linux-comparative-v1 reference=UNAVAILABLE" :
                     summary.over_budget ? " policy=linux-comparative-v1 reference=EXCEEDED" :
                                          " policy=linux-comparative-v1 reference=BELOW") : "");
    if (!emitted) return physical_serial_start_failure(session,"closing_transport",reference);
    return complete && emitted && (reference ? qualified : below) && !serial_proof_failure_mask() ? 0 : 1;
}
#endif

int cmd_irqlatency(int argc, char **argv)
{
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    if (argc==2 && !strcmp(argv[1],"serial"))
        return diagnostic_result_complete("irqlatency serial",physical_serial_run(false));
    if (argc==2 && !strcmp(argv[1],"reference"))
        return diagnostic_result_complete("irqlatency reference",physical_serial_run(true));
#endif
    if (argc == 5 && !strcmp(argv[1], "proof") &&
        (!strcmp(argv[2], "deferred") || !strcmp(argv[2], "immediate"))) {
        uint64_t session=0;
        if (strlen(argv[3])!=16 || strlen(argv[4])!=1 || argv[4][0]<'1' || argv[4][0]>'3') return 1;
        for (unsigned i=0;i<16;++i) {
            char c=argv[3][i];
            if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) return 1;
            session=(session<<4)|(uint64_t)(c<='9'?c-'0':c-'a'+10);
        }
        return diagnostic_result_complete("irqlatency proof",
            irq_latency_test_proof(!strcmp(argv[2],"deferred"),session,(unsigned)(argv[4][0]-'0')));
    }
    if (argc == 2 && !strcmp(argv[1], "export"))
        return diagnostic_result_complete("irqlatency export", irq_latency_test_export());
    if (argc == 2 && !strcmp(argv[1], "audit"))
        return diagnostic_result_complete("irqlatency audit", irq_latency_test_audit());
    if (argc == 2 && !strcmp(argv[1], "begin"))
        return diagnostic_result_complete("irqlatency begin", irq_latency_test_begin());
    if (argc == 2 && !strcmp(argv[1], "finish"))
        return diagnostic_result_complete("irqlatency finish", irq_latency_test_finish());
    if (argc == 2 && !strcmp(argv[1], "stress"))
        return diagnostic_result_complete("irqlatency stress", irq_latency_test_stress());
    return diagnostic_result_complete("irqlatency", 1);
}
