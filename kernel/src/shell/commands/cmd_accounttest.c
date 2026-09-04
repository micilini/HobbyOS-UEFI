#include "cmd_accounttest.h"
#include "../../core/clock.h"
#include "../../core/task_metrics.h"
#include "../../core/scheduler.h"
#include "../../core/semaphore.h"
#include "../../core/idt.h"
#include "../../timer/hpet.h"
#include "../../apic/lapic.h"
#include "../../smp/smp_topology.h"
#include "../../drivers/timer.h"
#include "../../drivers/serial.h"
#include "../../libc/memory.h"
#include "../../libc/string.h"
#define ACCT_MAX_SNAP 256u
#define ACCT_MAX_WORKERS 32u
#define ACCOUNT_CLOCK_RECORD_MAX 1024u
#define ACCOUNT_CLOCK_READS 1000000u
static task_snapshot_t g_before[ACCT_MAX_SNAP],g_after[ACCT_MAX_SNAP];
static task_cpu_sampler_t g_lifecycle_sampler; static task_cpu_sample_t g_lifecycle_samples[ACCT_MAX_SNAP];
static task_cpu_sample_t g_samples[ACCT_MAX_SNAP];static task_cpu_sampler_t g_sampler;
static uint64_t g_sleep_profile_samples[ACCOUNT_SLEEP_PROFILE_MAX_SAMPLES];
static volatile uint8_t g_stop;static volatile uint64_t g_migrations;
typedef struct{char bytes[ACCOUNT_CLOCK_RECORD_MAX];uint32_t length;uint8_t overflow;}account_clock_record_t;
static account_clock_record_t g_account_clock_record;
typedef struct{uint32_t mode,index;volatile uint8_t started,finished;task_id_t id;task_handle_t handle;}worker_ctx_t;
static worker_ctx_t g_workers[ACCT_MAX_WORKERS];
static void u64(uint64_t v){char b[21];uint32_t n=0;do{b[n++]=(char)('0'+v%10);v/=10;}while(v);while(n)serial_putc_all(b[--n]);}
static uint32_t parse(const char*s,uint32_t f){uint64_t v=0;if(!s||!*s)return f;while(*s){if(*s<'0'||*s>'9')return f;v=v*10+(uint32_t)(*s++-'0');if(v>180000)return f;}return(uint32_t)v;}
static uint32_t account_current_slot(void){scheduler_cpu_pin_t pin;if(!scheduler_cpu_pin(&pin))return TASK_CPU_SLOT_NONE;uint32_t slot=pin.slot;scheduler_cpu_unpin(&pin);return slot;}
static void worker(void*arg){worker_ctx_t*c=arg;uint32_t last=TASK_CPU_SLOT_NONE;__atomic_store_n(&c->started,1,__ATOMIC_RELEASE);while(!__atomic_load_n(&g_stop,__ATOMIC_ACQUIRE)){uint64_t begin=clock_monotonic_ns(),end=begin+1000000ULL;volatile uint64_t x=c->index+1;while(clock_monotonic_ns()<end)x=x*6364136223846793005ULL+1;(void)x;uint32_t slot=account_current_slot();if(slot!=TASK_CPU_SLOT_NONE&&last!=TASK_CPU_SLOT_NONE&&slot!=last)__atomic_add_fetch(&g_migrations,1,__ATOMIC_RELAXED);if(slot!=TASK_CPU_SLOT_NONE)last=slot;if(c->mode==1)timer_sleep(9);else if(c->mode==2)schedule_voluntary();}__atomic_store_n(&c->finished,1,__ATOMIC_RELEASE);}
static uint32_t snapshot(task_snapshot_t*out){return scheduler_snapshot_tasks(out,ACCT_MAX_SNAP,0).written;}
static bool wait_workers(uint32_t count,bool started,uint32_t timeout){for(uint32_t elapsed=0;elapsed<timeout;elapsed++){uint32_t done=0;for(uint32_t i=0;i<count;i++)done+=started?__atomic_load_n(&g_workers[i].started,__ATOMIC_ACQUIRE):__atomic_load_n(&g_workers[i].finished,__ATOMIC_ACQUIRE);if(done==count)return true;timer_sleep(1);}return false;}
static bool start_workers(uint32_t count,uint32_t mode){if(count>ACCT_MAX_WORKERS)return false;g_stop=0;g_migrations=0;for(uint32_t i=0;i<count;i++){g_workers[i]=(worker_ctx_t){.mode=mode,.index=i};task_handle_t h;if(!thread_create_named_handle(worker,&g_workers[i],"account-worker",&h))return false;g_workers[i].id=h.id;g_workers[i].handle=h;}return wait_workers(count,true,2000);}
static int workload(const char*kind,uint32_t window,uint32_t workers,uint32_t mode){task_cpu_sampler_reset(&g_sampler);if(!start_workers(workers,mode)){serial_write_all("[ACCOUNT][WORKLOAD] FAIL reason=worker-start-timeout\n");return 1;}timer_sleep(10);uint32_t n=snapshot(g_before);uint64_t start=clock_monotonic_ns();task_cpu_sampler_sample(&g_sampler,g_before,n,start,g_samples,ACCT_MAX_SNAP);timer_sleep(window);uint32_t m=snapshot(g_after);uint64_t end=clock_monotonic_ns();bool sampled=task_cpu_sampler_sample(&g_sampler,g_after,m,end,g_samples,ACCT_MAX_SNAP);__atomic_store_n(&g_stop,1,__ATOMIC_RELEASE);bool finished=wait_workers(workers,false,3000);uint64_t total=0,nonidle=0,max=0,anomalies=0;for(uint32_t i=0;i<m;i++)if(g_samples[i].valid){total+=g_samples[i].cpu_x10;if(!g_after[i].is_idle)nonidle+=g_samples[i].cpu_x10;if(g_samples[i].cpu_x10>max)max=g_samples[i].cpu_x10;if(g_samples[i].anomalous)anomalies++;}uint64_t expected=(uint64_t)g_cpu_count*1000ULL,tolerance=expected/10+100;bool ok=sampled&&finished&&!anomalies&&total+tolerance>=expected&&total<=expected+tolerance;if(!strcmp(kind,"BUSY")&&g_cpu_count==1)ok=ok&&nonidle>=900;if(!strcmp(kind,"SHARE")&&g_cpu_count==1)ok=ok&&nonidle>=900&&max>=350&&max<=650;if(!strcmp(kind,"SLEEP")&&g_cpu_count==1)ok=ok&&nonidle>=30&&nonidle<=300;if(!strcmp(kind,"MIGRATE")&&g_cpu_count>1)ok=ok&&g_migrations>0;serial_write_all("[ACCOUNT][");serial_write_all(kind);serial_write_all(ok?"] PASS":"] FAIL");serial_write_all(" cpus=");u64(g_cpu_count);serial_write_all(" aggregate_x10=");u64(total);serial_write_all(" nonidle_x10=");u64(nonidle);serial_write_all(" expected_x10=");u64(expected);serial_write_all(" max_x10=");u64(max);serial_write_all(" anomalies=");u64(anomalies);serial_write_all(" finished=");u64(finished?workers:0);if(mode==2){serial_write_all(" migrations=");u64(g_migrations);}serial_write_all("\n");
#ifdef HOBBYOS_ACCOUNT_NEGATIVE_IRQ_TICKS
if(!ok)serial_write_all("[ACCOUNT][NEGATIVE] IRQ_TICK_MODEL_DETECTED\n");
#endif
return ok?0:1;}
static int stats(void){clock_stats_t c;scheduler_accounting_stats_t a;task_metrics_stats_t m;clock_stats_snapshot(&c);scheduler_accounting_stats_snapshot(&a);task_metrics_stats_snapshot(&m);serial_write_all("[ACCOUNT][STATS] clock_reads=");u64(c.reads);serial_write_all(" api_regressions=");u64(c.api_regressions);serial_write_all(" local_regressions=");u64(c.source_local_regressions);serial_write_all(" cross_lag=");u64(c.cross_cpu_lag_clamps);serial_write_all(" retry_exhaustions=");u64(c.source_retry_exhaustions);serial_write_all(" max_cross_lag_ns=");u64(c.max_cross_cpu_lag_ns);serial_write_all(" clock_saturations=");u64(c.saturations);serial_write_all(" state_corruptions=");u64(c.state_corruptions);serial_write_all(" not_ready=");u64(c.not_ready);serial_write_all(" events=");u64(a.accounting_events);serial_write_all(" runtime_ns=");u64(a.runtime_ns_accounted);serial_write_all(" overflows=");u64(a.runtime_overflows);serial_write_all(" metric_regressions=");u64(m.runtime_regressions);serial_write_all(" over100_marked=");u64(m.over_100_samples);serial_write_all(" drops=");u64(m.dropped_baselines);serial_write_all("\n");return(c.api_regressions||c.source_local_regressions||c.source_retry_exhaustions||c.state_corruptions||c.saturations||c.not_ready||a.clock_regressions||a.runtime_overflows||m.runtime_regressions||m.dropped_baselines)?1:0;}

bool accounttest_uptime_wrapper_sample_valid(uint64_t before_ms,
                                             uint64_t uptime_ms,
                                             uint64_t after_ms)
{
    return before_ms <= uptime_ms && uptime_ms <= after_ms;
}

bool accounttest_uptime_wrapper_contract_selftest(void)
{
    uint64_t legacy_wrapper = 100;
    uint64_t legacy_after = 101;
    return accounttest_uptime_wrapper_sample_valid(100, 100, 100) &&
           accounttest_uptime_wrapper_sample_valid(100, 100, 101) &&
           accounttest_uptime_wrapper_sample_valid(100, 101, 101) &&
           accounttest_uptime_wrapper_sample_valid(UINT64_MAX, UINT64_MAX,
                                                   UINT64_MAX) &&
           !accounttest_uptime_wrapper_sample_valid(101, 100, 101) &&
           !accounttest_uptime_wrapper_sample_valid(100, 102, 101) &&
           legacy_wrapper != legacy_after &&
           accounttest_uptime_wrapper_sample_valid(100, legacy_after,
                                                   legacy_after);
}

static uint64_t account_sleep_target_ns(uint32_t requested_ms)
{
    return (uint64_t)requested_ms * 1000000ULL;
}

static uint64_t account_sleep_soft_upper_ns(uint32_t requested_ms)
{
    return account_sleep_target_ns(requested_ms) * 2u + 50000000ULL;
}

bool accounttest_sleep_sample_classify(uint32_t requested_ms,
                                        task_wait_result_t wait_result,
                                        uint64_t elapsed_ns,
                                        account_sleep_sample_t *out)
{
    uint64_t target_ns, minimum_ns;
    if (!requested_ms || !out)
        return false;
    target_ns = account_sleep_target_ns(requested_ms);
    minimum_ns = target_ns > ACCOUNT_SLEEP_EARLY_TOLERANCE_NS ?
        target_ns - ACCOUNT_SLEEP_EARLY_TOLERANCE_NS : 0;
    memset(out, 0, sizeof(*out));
    out->requested_ms = requested_ms;
    out->wait_result = wait_result;
    out->elapsed_ns = elapsed_ns;
    out->minimum_ns = minimum_ns;
    out->overshoot_ns = elapsed_ns > target_ns ? elapsed_ns - target_ns : 0;
    out->early_by_ns = elapsed_ns < target_ns ? target_ns - elapsed_ns : 0;
    out->early = elapsed_ns < minimum_ns;
    out->late_soft = elapsed_ns > account_sleep_soft_upper_ns(requested_ms);
    out->wait_ok = wait_result == TASK_WAIT_RESULT_OK ||
                   wait_result == TASK_WAIT_RESULT_TIMEOUT;
    return out->wait_ok && !out->early;
}

static bool account_sleep_sample_run(uint32_t requested_ms,
                                     account_sleep_sample_t *out)
{
    uint64_t start_ns, end_ns;
    task_wait_result_t result;
    if (!out)
        return false;
    start_ns = clock_monotonic_ns();
    result = timer_sleep_interruptible(requested_ms);
    end_ns = clock_monotonic_ns();
    if (end_ns < start_ns)
    {
        (void)accounttest_sleep_sample_classify(requested_ms, result, 0, out);
        out->early = 1;
        return false;
    }
    return accounttest_sleep_sample_classify(requested_ms, result,
                                             end_ns - start_ns, out);
}

static void account_sleep_sort(uint64_t *values, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++)
    {
        uint64_t value = values[i];
        uint32_t j = i;
        while (j && values[j - 1u] > value)
        {
            values[j] = values[j - 1u];
            j--;
        }
        values[j] = value;
    }
}

static uint32_t account_sleep_percentile_index(uint32_t count,
                                                uint32_t percentile)
{
    return (count * percentile + 99u) / 100u - 1u;
}

bool accounttest_sleep_percentiles_selftest(void)
{
    uint64_t values[20];
    for (uint32_t i = 0; i < 20; i++)
        values[i] = 20u - i;
    account_sleep_sort(values, 20);
    return values[account_sleep_percentile_index(20, 50)] == 10 &&
           values[account_sleep_percentile_index(20, 95)] == 19 &&
           values[account_sleep_percentile_index(20, 99)] == 20;
}

static void account_clock_record_reset(void)
{
    g_account_clock_record.length = 0;
    g_account_clock_record.overflow = 0;
    g_account_clock_record.bytes[0] = '\0';
}

static bool account_clock_record_char(char value)
{
    if (g_account_clock_record.overflow ||
        g_account_clock_record.length >= ACCOUNT_CLOCK_RECORD_MAX - 1u)
    {
        g_account_clock_record.overflow = 1;
        return false;
    }
    g_account_clock_record.bytes[g_account_clock_record.length++] = value;
    g_account_clock_record.bytes[g_account_clock_record.length] = '\0';
    return true;
}

static bool account_clock_record_text(const char *text)
{
    if (!text)
        return false;
    while (*text)
        if (!account_clock_record_char(*text++))
            return false;
    return true;
}

static bool account_clock_record_u64(uint64_t value)
{
    char reverse[20];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        if (!account_clock_record_char(reverse[--count]))
            return false;
    return true;
}

static bool account_clock_record_field(const char *name, uint64_t value)
{
    return account_clock_record_text(name) && account_clock_record_u64(value);
}

static bool account_clock_record_emit(const account_clock_result_t *result)
{
    bool ok;
    if (!result)
        return false;
    account_clock_record_reset();
    ok = account_clock_record_text("\n[ACCOUNT][CLOCK] ") &&
         account_clock_record_text(result->failed_mask ? "FAIL" : "PASS") &&
         account_clock_record_field(" reads=", result->reads) &&
         account_clock_record_field(" failed_mask=", result->failed_mask) &&
         account_clock_record_field(" monotonic_failures=", result->monotonic_failures) &&
         account_clock_record_field(" d10=", result->d10_ns) &&
         account_clock_record_field(" d100=", result->d100_ns) &&
         account_clock_record_field(" d1000=", result->d1000_ns) &&
         account_clock_record_field(" sleep_early_mask=", result->sleep_early_mask) &&
         account_clock_record_field(" sleep_late_mask=", result->sleep_late_mask) &&
         account_clock_record_field(" sleep_wait_error_mask=", result->sleep_wait_error_mask) &&
         account_clock_record_field(" d10_overshoot=", result->sleep10.overshoot_ns) &&
         account_clock_record_field(" d100_overshoot=", result->sleep100.overshoot_ns) &&
         account_clock_record_field(" d1000_overshoot=", result->sleep1000.overshoot_ns) &&
         account_clock_record_field(" d10_early_by=", result->sleep10.early_by_ns) &&
         account_clock_record_field(" d100_early_by=", result->sleep100.early_by_ns) &&
         account_clock_record_field(" d1000_early_by=", result->sleep1000.early_by_ns) &&
         account_clock_record_field(" api_delta=", result->api_delta) &&
         account_clock_record_field(" local_delta=", result->local_delta) &&
         account_clock_record_field(" cross_delta=", result->cross_lag_delta) &&
         account_clock_record_field(" retry_delta=", result->retry_delta) &&
         account_clock_record_field(" saturation_delta=", result->saturation_delta) &&
         account_clock_record_field(" state_delta=", result->state_delta) &&
         account_clock_record_field(" not_ready_delta=", result->not_ready_delta) &&
         account_clock_record_field(" wrapper_before_ms=", result->wrapper_before_ms) &&
         account_clock_record_field(" wrapper_ms=", result->wrapper_ms) &&
         account_clock_record_field(" wrapper_after_ms=", result->wrapper_after_ms) &&
         account_clock_record_field(" legacy_exact_equal=", result->legacy_exact_equal) &&
         account_clock_record_field(" wrapper_interval_valid=", result->wrapper_interval_valid) &&
         account_clock_record_char('\n');
    if (!ok || g_account_clock_record.overflow)
    {
        serial_write_all("\n[ACCOUNT][CLOCK_RECORD] FAIL reason=overflow\n");
        return false;
    }
    serial_write_all(g_account_clock_record.bytes);
    return true;
}

static uint64_t account_clock_delta(uint64_t before, uint64_t after,
                                    bool *underflow)
{
    if (after < before)
    {
        *underflow = true;
        return 0;
    }
    return after - before;
}

static bool account_sleep_profile_run(uint32_t target_ms, uint32_t samples,
                                      account_sleep_profile_t *out)
{
    clock_stats_t before, after;
    bool underflow = false;
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!target_ms || target_ms > 10000u || !samples ||
        samples > ACCOUNT_SLEEP_PROFILE_MAX_SAMPLES)
        return false;
    out->target_ms = target_ms;
    out->samples = samples;
    clock_stats_snapshot(&before);
    for (uint32_t i = 0; i < samples; i++)
    {
        account_sleep_sample_t sample;
        (void)account_sleep_sample_run(target_ms, &sample);
        g_sleep_profile_samples[i] = sample.elapsed_ns;
        if (sample.early)
            out->early_count++;
        if (!sample.wait_ok)
            out->wait_errors++;
        if (sample.late_soft)
            out->late_soft_count++;
        if (sample.overshoot_ns > out->max_overshoot_ns)
            out->max_overshoot_ns = sample.overshoot_ns;
    }
    clock_stats_snapshot(&after);
    out->api_delta = account_clock_delta(before.api_regressions,
                                          after.api_regressions, &underflow);
    out->local_delta = account_clock_delta(before.source_local_regressions,
                                            after.source_local_regressions,
                                            &underflow);
    out->retry_delta = account_clock_delta(before.source_retry_exhaustions,
                                            after.source_retry_exhaustions,
                                            &underflow);
    out->saturation_delta = account_clock_delta(before.saturations,
                                                 after.saturations,
                                                 &underflow);
    out->state_delta = account_clock_delta(before.state_corruptions,
                                            after.state_corruptions,
                                            &underflow);
    out->not_ready_delta = account_clock_delta(before.not_ready,
                                                after.not_ready, &underflow);
    if (underflow && !out->state_delta)
        out->state_delta = 1;
    account_sleep_sort(g_sleep_profile_samples, samples);
    out->min_ns = g_sleep_profile_samples[0];
    out->p50_ns = g_sleep_profile_samples[
        account_sleep_percentile_index(samples, 50)];
    out->p95_ns = g_sleep_profile_samples[
        account_sleep_percentile_index(samples, 95)];
    out->p99_ns = g_sleep_profile_samples[
        account_sleep_percentile_index(samples, 99)];
    out->max_ns = g_sleep_profile_samples[samples - 1u];
    out->hard_valid = !out->early_count && !out->wait_errors &&
        !out->api_delta && !out->local_delta && !out->retry_delta &&
        !out->saturation_delta && !out->state_delta && !out->not_ready_delta;
    out->quality_good = out->p95_ns <= account_sleep_soft_upper_ns(target_ms);
    return out->hard_valid;
}

static void account_sleep_profile_emit(const account_sleep_profile_t *profile)
{
    const char *status = !profile->hard_valid ? "FAIL" :
        (profile->quality_good ? "PASS" : "DEGRADED");
    serial_write_all("[ACCOUNT][SLEEP_PROFILE] ");serial_write_all(status);
    serial_write_all(" target_ms=");u64(profile->target_ms);
    serial_write_all(" samples=");u64(profile->samples);
    serial_write_all(" early=");u64(profile->early_count);
    serial_write_all(" wait_errors=");u64(profile->wait_errors);
    serial_write_all(" late_soft=");u64(profile->late_soft_count);
    serial_write_all(" min=");u64(profile->min_ns);
    serial_write_all(" p50=");u64(profile->p50_ns);
    serial_write_all(" p95=");u64(profile->p95_ns);
    serial_write_all(" p99=");u64(profile->p99_ns);
    serial_write_all(" max=");u64(profile->max_ns);
    serial_write_all(" max_overshoot=");u64(profile->max_overshoot_ns);
    serial_write_all(" soft_upper=");
    u64(account_sleep_soft_upper_ns(profile->target_ms));
    serial_write_all(" api_delta=");u64(profile->api_delta);
    serial_write_all(" local_delta=");u64(profile->local_delta);
    serial_write_all(" retry_delta=");u64(profile->retry_delta);
    serial_write_all(" saturation_delta=");u64(profile->saturation_delta);
    serial_write_all(" state_delta=");u64(profile->state_delta);
    serial_write_all(" not_ready_delta=");u64(profile->not_ready_delta);
    serial_write_all("\n");
}

static int sleep_profile_test(uint32_t target_ms, uint32_t samples)
{
    account_sleep_profile_t profile;
    (void)account_sleep_profile_run(target_ms, samples, &profile);
    account_sleep_profile_emit(&profile);
    return profile.hard_valid ? 0 : 1;
}

static int sleep_profile_set_test(void)
{
    static const uint32_t targets[] = {10u, 100u, 1000u};
    static const uint32_t samples[] = {128u, 32u, 20u};
    account_sleep_profile_t profile;
    uint32_t hard_failures = 0, degraded_profiles = 0;
    uint64_t late_samples = 0;
    for (uint32_t i = 0; i < 3; i++)
    {
        (void)account_sleep_profile_run(targets[i], samples[i], &profile);
        account_sleep_profile_emit(&profile);
        if (!profile.hard_valid)
            hard_failures++;
        if (profile.hard_valid && !profile.quality_good)
            degraded_profiles++;
        late_samples += profile.late_soft_count;
    }
    serial_write_all("[ACCOUNT][SLEEP_PROFILE_SET] ");
    serial_write_all(hard_failures ? "FAIL" :
        (degraded_profiles ? "DEGRADED" : "PASS"));
    serial_write_all(" profiles=3 hard_failures=");u64(hard_failures);
    serial_write_all(" degraded_profiles=");u64(degraded_profiles);
    serial_write_all(" late_samples=");u64(late_samples);
    serial_write_all("\n");
    return hard_failures ? 1 : 0;
}

static uint32_t clock_test_once(account_clock_result_t *out)
{
    clock_stats_t before, after;
    bool underflow = false;
    if (!out)
        return ACCOUNT_CLOCK_FAIL_STATE;
    memset(out, 0, sizeof(*out));
    out->reads = ACCOUNT_CLOCK_READS;
    clock_stats_snapshot(&before);
    uint64_t start = clock_monotonic_ns();
    uint64_t last = start;
    if (!start)
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_START_ZERO;
    for (uint32_t i = 0; i < ACCOUNT_CLOCK_READS; i++)
    {
        uint64_t now = clock_monotonic_ns();
        if (now < last)
        {
            out->monotonic_failures++;
            out->failed_mask |= ACCOUNT_CLOCK_FAIL_MONOTONIC;
        }
        last = now;
    }
    (void)account_sleep_sample_run(10, &out->sleep10);
    (void)account_sleep_sample_run(100, &out->sleep100);
    (void)account_sleep_sample_run(1000, &out->sleep1000);
    out->d10_ns = out->sleep10.elapsed_ns;
    out->d100_ns = out->sleep100.elapsed_ns;
    out->d1000_ns = out->sleep1000.elapsed_ns;
    if (out->sleep10.early)
    {
        out->sleep_early_mask |= 1u << 0;
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_SLEEP_EARLY_10;
    }
    if (out->sleep100.early)
    {
        out->sleep_early_mask |= 1u << 1;
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_SLEEP_EARLY_100;
    }
    if (out->sleep1000.early)
    {
        out->sleep_early_mask |= 1u << 2;
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_SLEEP_EARLY_1000;
    }
    if (out->sleep10.late_soft)
        out->sleep_late_mask |= 1u << 0;
    if (out->sleep100.late_soft)
        out->sleep_late_mask |= 1u << 1;
    if (out->sleep1000.late_soft)
        out->sleep_late_mask |= 1u << 2;
    if (!out->sleep10.wait_ok)
    {
        out->sleep_wait_error_mask |= 1u << 0;
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_SLEEP_WAIT_10;
    }
    if (!out->sleep100.wait_ok)
    {
        out->sleep_wait_error_mask |= 1u << 1;
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_SLEEP_WAIT_100;
    }
    if (!out->sleep1000.wait_ok)
    {
        out->sleep_wait_error_mask |= 1u << 2;
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_SLEEP_WAIT_1000;
    }
    clock_stats_snapshot(&after);
    out->api_delta = account_clock_delta(before.api_regressions,
                                          after.api_regressions, &underflow);
    out->local_delta = account_clock_delta(before.source_local_regressions,
                                            after.source_local_regressions,
                                            &underflow);
    out->cross_lag_delta = account_clock_delta(before.cross_cpu_lag_clamps,
                                               after.cross_cpu_lag_clamps,
                                               &underflow);
    out->retry_delta = account_clock_delta(before.source_retry_exhaustions,
                                            after.source_retry_exhaustions,
                                            &underflow);
    out->saturation_delta = account_clock_delta(before.saturations,
                                                after.saturations, &underflow);
    out->state_delta = account_clock_delta(before.state_corruptions,
                                           after.state_corruptions, &underflow);
    out->not_ready_delta = account_clock_delta(before.not_ready,
                                               after.not_ready, &underflow);
    if (out->api_delta)
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_API_REGRESSION;
    if (out->local_delta)
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_LOCAL_REGRESSION;
    if (out->retry_delta)
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_RETRY;
    if (out->saturation_delta)
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_SATURATION;
    if (out->state_delta || underflow)
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_STATE;
    if (out->not_ready_delta)
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_NOT_READY;
    out->wrapper_before_ms = clock_monotonic_ms();
    out->wrapper_ms = timer_get_uptime_ms();
    out->wrapper_after_ms = clock_monotonic_ms();
    out->legacy_exact_equal = out->wrapper_ms == out->wrapper_after_ms;
    out->wrapper_interval_valid = accounttest_uptime_wrapper_sample_valid(
        out->wrapper_before_ms, out->wrapper_ms, out->wrapper_after_ms);
    if (!out->wrapper_interval_valid)
        out->failed_mask |= ACCOUNT_CLOCK_FAIL_UPTIME_WRAPPER;
    return out->failed_mask;
}

static int clock_test(void)
{
    account_clock_result_t result;
    (void)clock_test_once(&result);
    bool emitted = account_clock_record_emit(&result);
    return result.failed_mask || !emitted ? 1 : 0;
}

static int clock_contract_test(void)
{
    uint64_t legacy_wrapper = 100, legacy_after = 101;
    bool false_rejection = legacy_wrapper != legacy_after &&
        accounttest_uptime_wrapper_sample_valid(100, legacy_after, legacy_after);
    bool ok = false_rejection && accounttest_uptime_wrapper_contract_selftest();
    serial_write_all(ok ? "[ACCOUNT][CLOCK_CONTRACT] PASS cases=6 legacy_false_rejection=1\n"
                        : "[ACCOUNT][CLOCK_CONTRACT] FAIL cases=6 legacy_false_rejection=0\n");
    return ok ? 0 : 1;
}

static int clock_wrapper_loop_test(uint32_t samples)
{
    uint64_t interval_failures = 0, legacy_mismatches = 0, max_span_ms = 0;
    for (uint32_t i = 0; i < samples; i++)
    {
        uint64_t before = clock_monotonic_ms();
        uint64_t wrapper = timer_get_uptime_ms();
        uint64_t after = clock_monotonic_ms();
        if (!accounttest_uptime_wrapper_sample_valid(before, wrapper, after))
            interval_failures++;
        if (wrapper != after)
            legacy_mismatches++;
        if (after >= before && after - before > max_span_ms)
            max_span_ms = after - before;
    }
    serial_write_all(interval_failures ? "[ACCOUNT][CLOCK_WRAPPER_LOOP] FAIL samples="
                                       : "[ACCOUNT][CLOCK_WRAPPER_LOOP] PASS samples=");
    u64(samples);serial_write_all(" interval_failures=");u64(interval_failures);
    serial_write_all(" legacy_exact_mismatches=");u64(legacy_mismatches);
    serial_write_all(" max_span_ms=");u64(max_span_ms);serial_write_all("\n");
    return interval_failures ? 1 : 0;
}

static int clock_loop_test(uint32_t rounds)
{
    uint64_t legacy_mismatches = 0, cross_lag_total = 0;
    uint64_t late_rounds = 0, late10 = 0, late100 = 0, late1000 = 0;
    uint64_t max_d10 = 0, max_d100 = 0, max_d1000 = 0;
    uint64_t max_overshoot10 = 0, max_overshoot100 = 0;
    uint64_t max_overshoot1000 = 0;
    for (uint32_t round = 1; round <= rounds; round++)
    {
        account_clock_result_t result;
        (void)clock_test_once(&result);
        legacy_mismatches += result.legacy_exact_equal ? 0u : 1u;
        cross_lag_total += result.cross_lag_delta;
        late_rounds += result.sleep_late_mask ? 1u : 0u;
        late10 += (result.sleep_late_mask & (1u << 0)) ? 1u : 0u;
        late100 += (result.sleep_late_mask & (1u << 1)) ? 1u : 0u;
        late1000 += (result.sleep_late_mask & (1u << 2)) ? 1u : 0u;
        if (result.d10_ns > max_d10) max_d10 = result.d10_ns;
        if (result.d100_ns > max_d100) max_d100 = result.d100_ns;
        if (result.d1000_ns > max_d1000) max_d1000 = result.d1000_ns;
        if (result.sleep10.overshoot_ns > max_overshoot10)
            max_overshoot10 = result.sleep10.overshoot_ns;
        if (result.sleep100.overshoot_ns > max_overshoot100)
            max_overshoot100 = result.sleep100.overshoot_ns;
        if (result.sleep1000.overshoot_ns > max_overshoot1000)
            max_overshoot1000 = result.sleep1000.overshoot_ns;
        if (result.failed_mask)
        {
            (void)account_clock_record_emit(&result);
            serial_write_all("[ACCOUNT][CLOCK_LOOP] FAIL round=");u64(round);
            serial_write_all(" failed_mask=");u64(result.failed_mask);
            serial_write_all("\n");
            return 1;
        }
    }
    serial_write_all("[ACCOUNT][CLOCK_LOOP] PASS rounds=");u64(rounds);
    serial_write_all(" failed_mask=0 late_rounds=");u64(late_rounds);
    serial_write_all(" late10=");u64(late10);
    serial_write_all(" late100=");u64(late100);
    serial_write_all(" late1000=");u64(late1000);
    serial_write_all(" max_d10=");u64(max_d10);
    serial_write_all(" max_d100=");u64(max_d100);
    serial_write_all(" max_d1000=");u64(max_d1000);
    serial_write_all(" max_overshoot10=");u64(max_overshoot10);
    serial_write_all(" max_overshoot100=");u64(max_overshoot100);
    serial_write_all(" max_overshoot1000=");u64(max_overshoot1000);
    serial_write_all(" legacy_exact_mismatches=");u64(legacy_mismatches);
    serial_write_all(" cross_lag_total=");u64(cross_lag_total);
    serial_write_all(" sleep_early_mask=0 sleep_wait_error_mask=0\n");
    return 0;
}
static int clock_classifier_test(void)
{
    bool detected = clock_sample_classifier_selftest();
#ifdef HOBBYOS_CLOCK_NEGATIVE_GLOBAL_ONLY_REGRESSION
    serial_write_all(detected
        ? "[CLOCK][NEGATIVE] GLOBAL_ONLY_FALSE_REGRESSION_DETECTED\n"
        : "[CLOCK][NEGATIVE] GLOBAL_ONLY_FALSE_REGRESSION_MISSED\n");
    serial_write_all(detected
        ? "[ACCOUNT][CLOCK_CLASSIFIER] NEGATIVE_DETECTED kind=global-only\n"
        : "[ACCOUNT][CLOCK_CLASSIFIER] FAIL kind=global-only\n");
#elif defined(HOBBYOS_CLOCK_NEGATIVE_LOCAL_BACKWARD)
    serial_write_all(detected
        ? "[CLOCK][NEGATIVE] LOCAL_SOURCE_REGRESSION_DETECTED\n"
        : "[CLOCK][NEGATIVE] LOCAL_SOURCE_REGRESSION_MISSED\n");
    serial_write_all(detected
        ? "[ACCOUNT][CLOCK_CLASSIFIER] NEGATIVE_DETECTED kind=local-backward\n"
        : "[ACCOUNT][CLOCK_CLASSIFIER] FAIL kind=local-backward\n");
#else
    uint64_t begin = 0, end = 0, protected_bytes = 0;
    clock_state_layout(&begin, &end, &protected_bytes);
    serial_write_all(detected ? "[ACCOUNT][CLOCK_CLASSIFIER] PASS cases="
                              : "[ACCOUNT][CLOCK_CLASSIFIER] FAIL cases=");
    u64(clock_sample_classifier_case_count());
    serial_write_all(" canary_begin=");
    u64(begin);
    serial_write_all(" canary_end=");
    u64(end);
    serial_write_all(" protected_bytes=");
    u64(protected_bytes);
    serial_write_all("\n");
#endif
    return detected ? 0 : 1;
}
static int lapic_config_test(void)
{
    if(!g_cpu_count||g_cpu_count>ACCT_MAX_WORKERS)return 1;
    uint64_t min_tpm=UINT64_MAX,max_tpm=0;bool ok=true;
    for(uint32_t slot=0;slot<g_cpu_count;slot++){
        lapic_timer_state_t state;lapic_timer_validation_t validation;
        bool valid=lapic_timer_state_snapshot(slot,&state)&&
            lapic_timer_validate_configuration(slot,&validation);
        if(!valid)ok=false;
        if(state.calibration.ticks_per_ms<min_tpm)min_tpm=state.calibration.ticks_per_ms;
        if(state.calibration.ticks_per_ms>max_tpm)max_tpm=state.calibration.ticks_per_ms;
        serial_write_all("[ACCOUNT][LAPIC_CONFIG_CPU] slot=");u64(slot);
        serial_write_all(" failed_mask=");u64(validation.failed_mask);
        serial_write_all(" vector=");u64(state.programmed_vector);
        serial_write_all(" divisor=");u64(state.programmed_divisor);
        serial_write_all(" initial=");u64(state.programmed_initial_count);
        serial_write_all(" periodic=");u64(state.periodic);
        serial_write_all(" masked=");u64(state.masked);serial_write_all("\n");
    }
    uint64_t spread=(min_tpm&&min_tpm!=UINT64_MAX)?
        ((max_tpm-min_tpm)*1000ULL)/min_tpm:UINT64_MAX;
    if(spread>200)ok=false;
#ifdef HOBBYOS_LAPIC_NEGATIVE_WRONG_PERIOD
    if(!ok){serial_write_all("[ACCOUNT][NEGATIVE] LAPIC_WRONG_PERIOD_DETECTED\n");return 0;}
#endif
    serial_write_all(ok?"[ACCOUNT][LAPIC_CONFIG] PASS cpus=":"[ACCOUNT][LAPIC_CONFIG] FAIL cpus=");
    u64(g_cpu_count);serial_write_all(" spread_x10=");u64(spread);serial_write_all("\n");
    return ok?0:1;
}

static lapic_timer_state_t g_lapic_before[ACCT_MAX_WORKERS];
static lapic_timer_state_t g_lapic_after[ACCT_MAX_WORKERS];
static uint64_t g_lapic_ratios[ACCT_MAX_WORKERS][9];

static int lapic_liveness_test(uint32_t window)
{
    if(!window||g_cpu_count>ACCT_MAX_WORKERS)return 1;
    bool ok=true;for(uint32_t s=0;s<g_cpu_count;s++)
        ok=lapic_timer_state_snapshot(s,&g_lapic_before[s])&&ok;
    uint64_t begin=clock_monotonic_ns();timer_sleep(window);uint64_t end=clock_monotonic_ns();
    uint64_t max_gap=0,zero=0;
    for(uint32_t s=0;s<g_cpu_count;s++){
        lapic_timer_validation_t validation;
        bool valid=lapic_timer_state_snapshot(s,&g_lapic_after[s])&&
            lapic_timer_validate_configuration(s,&validation);
        uint64_t delta=g_lapic_after[s].irq_count-g_lapic_before[s].irq_count;
        uint64_t observed_now=clock_monotonic_ns();
        bool advanced=delta&&g_lapic_after[s].last_irq_ns>g_lapic_before[s].last_irq_ns&&
            g_lapic_after[s].last_irq_ns<=observed_now&&
            observed_now-g_lapic_after[s].last_irq_ns<=(uint64_t)window*1000000ULL;
        if(!advanced)zero++;
        if(g_lapic_after[s].max_irq_gap_ns>max_gap)max_gap=g_lapic_after[s].max_irq_gap_ns;
        if(!valid||!advanced)ok=false;
        serial_write_all("[ACCOUNT][LAPIC_LIVENESS_CPU] slot=");u64(s);
        serial_write_all(" delta=");u64(delta);serial_write_all(" masked=");
        u64(g_lapic_after[s].masked);serial_write_all("\n");
    }
#ifdef HOBBYOS_LAPIC_NEGATIVE_MASK_ONE_CPU
    if(!ok&&zero){serial_write_all("[ACCOUNT][NEGATIVE] LAPIC_MASKED_CPU_DETECTED\n");return 0;}
#endif
    serial_write_all(ok?"[ACCOUNT][LAPIC_LIVENESS] PASS cpus=":"[ACCOUNT][LAPIC_LIVENESS] FAIL cpus=");
    u64(g_cpu_count);serial_write_all(" window_ms=");u64(window);
    serial_write_all(" all_advanced=");u64(zero==0);serial_write_all(" max_gap_ns=");u64(max_gap);
    serial_write_all(" zero_irq_cpus=");u64(zero);serial_write_all(" elapsed_ns=");u64(end-begin);serial_write_all("\n");
    return ok?0:1;
}

static void quiescence_count(const task_snapshot_t*s,accounttest_worker_quiescence_t*q)
{if(s->exit_started&&s->state!=TASK_ZOMBIE){q->exiting++;return;}switch(s->state){case TASK_READY:q->ready++;break;case TASK_RUNNING:q->running++;break;case TASK_BLOCKED:q->blocked++;break;case TASK_SLEEPING:q->sleeping++;break;case TASK_ZOMBIE:q->zombies++;break;default:break;}}
static void quiescence_task(const task_snapshot_t*s)
{serial_write_all("[ACCOUNT][QUIESCENCE_TASK] id=");u64(s->id);serial_write_all(" state=");u64(s->state);serial_write_all(" on_cpu=");u64(s->on_cpu);serial_write_all(" cpu_slot=");u64(s->current_cpu_slot);serial_write_all(" queue=");u64(s->queue_membership);serial_write_all(" wait_kind=");u64(s->wait_kind);serial_write_all(" exit_started=");u64(s->exit_started);serial_write_all(" cleanup_done=");u64(s->cleanup_done);serial_write_all(" notify_done=");u64(s->lifecycle_notify_completed);serial_write_all(" reap_claimed=");u64(s->reap_claimed);serial_write_all("\n");}
bool accounttest_wait_workers_reaped(const task_handle_t*w,uint32_t n,uint64_t timeout_ms,accounttest_worker_quiescence_t*out)
{accounttest_worker_quiescence_t q={.expected=n};if(!w||!n||!out)return false;for(uint32_t i=0;i<n;i++)if(!w[i].id||!w[i].lifecycle_generation)return false;task_reaper_stats_t begin;scheduler_reaper_stats_snapshot(&begin);uint64_t deadline=timer_get_uptime_ms()+timeout_ms;while(timer_get_uptime_ms()<deadline){q.active_reap_calls++;q.active_reaped+=scheduler_reap_zombies(0);q.handles_gone=0;q.ready=q.running=q.blocked=q.sleeping=q.zombies=q.exiting=0;for(uint32_t i=0;i<n;i++){task_snapshot_t s;if(!scheduler_snapshot_task_by_handle(w[i],&s))q.handles_gone++;else quiescence_count(&s,&q);}task_reaper_stats_t r;scheduler_reaper_stats_snapshot(&r);q.free_inflight=r.free_inflight;q.global_reaped_delta=r.reaped-begin.reaped;if(q.handles_gone==n&&!q.free_inflight){if(++q.stable_passes>=3){*out=q;return true;}}else q.stable_passes=0;timer_sleep(10);}serial_write_all("[ACCOUNT][QUIESCENCE_TIMEOUT] expected=");u64(n);serial_write_all(" gone=");u64(q.handles_gone);serial_write_all(" ready=");u64(q.ready);serial_write_all(" running=");u64(q.running);serial_write_all(" blocked=");u64(q.blocked);serial_write_all(" sleeping=");u64(q.sleeping);serial_write_all(" zombies=");u64(q.zombies);serial_write_all(" exiting=");u64(q.exiting);serial_write_all(" free_inflight=");u64(q.free_inflight);serial_write_all(" active_reap_calls=");u64(q.active_reap_calls);serial_write_all(" active_reaped=");u64(q.active_reaped);serial_write_all("\n");for(uint32_t i=0;i<n;i++){task_snapshot_t s;if(scheduler_snapshot_task_by_handle(w[i],&s))quiescence_task(&s);}*out=q;return false;}
bool accounttest_wait_reaper_idle(uint64_t timeout_ms,uint32_t stable_passes,task_reaper_stats_t*out)
{if(!stable_passes)return false;uint32_t stable=0;uint64_t deadline=timer_get_uptime_ms()+timeout_ms;task_reaper_stats_t r={0};while(timer_get_uptime_ms()<deadline){(void)scheduler_reap_zombies(0);scheduler_reaper_stats_snapshot(&r);if(!r.current_zombies&&!r.free_inflight){if(++stable>=stable_passes){if(out)*out=r;return true;}}else stable=0;timer_sleep(10);}if(out)*out=r;return false;}

static int lapic_rate_test(uint32_t window,uint32_t rounds)
{
    if(window<100||!rounds||rounds>9||g_cpu_count>ACCT_MAX_WORKERS)return 1;
    if(!accounttest_wait_reaper_idle(10000,3,NULL)||lapic_config_test())return 1;
    bool ok=true;uint64_t min_round=UINT64_MAX,max_round=0;
    for(uint32_t r=0;r<rounds;r++){
        if(!accounttest_wait_reaper_idle(10000,3,NULL))return 1;
        for(uint32_t s=0;s<g_cpu_count;s++)
            if(!lapic_timer_state_snapshot(s,&g_lapic_before[s]))ok=false;
        uint64_t begin=clock_monotonic_ns();timer_sleep(window);uint64_t end=clock_monotonic_ns();
        uint64_t expected=(end-begin)/1000000ULL;if(!expected)expected=1;
        for(uint32_t s=0;s<g_cpu_count;s++){
            if(!lapic_timer_state_snapshot(s,&g_lapic_after[s]))ok=false;
            uint64_t delta=g_lapic_after[s].irq_count-g_lapic_before[s].irq_count;
            uint64_t ratio=delta*1000ULL/expected;g_lapic_ratios[s][r]=ratio;
            if(!delta)ok=false;
            if(ratio<min_round)min_round=ratio;
            if(ratio>max_round)max_round=ratio;
            serial_write_all("[ACCOUNT][LAPIC_RATE_CPU] round=");u64(r+1);
            serial_write_all(" slot=");u64(s);serial_write_all(" delta=");u64(delta);
            serial_write_all(" expected=");u64(expected);serial_write_all(" ratio_x1000=");u64(ratio);serial_write_all("\n");
        }
    }
    uint64_t worst=UINT64_MAX;serial_write_all("[ACCOUNT][LAPIC_RATE_MEDIANS]");
    for(uint32_t s=0;s<g_cpu_count;s++){
        for(uint32_t i=1;i<rounds;i++){uint64_t v=g_lapic_ratios[s][i];uint32_t j=i;
            while(j&&g_lapic_ratios[s][j-1]>v){g_lapic_ratios[s][j]=g_lapic_ratios[s][j-1];j--;}
            g_lapic_ratios[s][j]=v;}
        uint64_t median=g_lapic_ratios[s][rounds/2];if(median<worst)worst=median;
        serial_write_all(" cpu");u64(s);serial_write_all("=");u64(median);
    }serial_write_all("\n");
    if(worst<700||worst>1300)ok=false;
    serial_write_all(ok?"[ACCOUNT][LAPIC_RATE] PASS":"[ACCOUNT][LAPIC_RATE] FAIL");
    serial_write_all(" window_ms=");u64(window);serial_write_all(" rounds=");u64(rounds);
    serial_write_all(" worst_median_x1000=");u64(worst);serial_write_all(" min_round_x1000=");u64(min_round);
    serial_write_all(" max_round_x1000=");u64(max_round);serial_write_all("\n");return ok?0:1;
}
static bool lifecycle_matches(const task_snapshot_t *task,
                              task_handle_t handle)
{
    return task->id == handle.id &&
           task->lifecycle_generation == handle.lifecycle_generation;
}

static int lifecycle(void)
{
    task_cpu_sampler_t *sampler = &g_lifecycle_sampler;
    task_cpu_sample_t *samples = g_lifecycle_samples;
    task_cpu_sampler_reset(sampler);
    uint32_t count = snapshot(g_before);
    bool first = task_cpu_sampler_sample(sampler, g_before, count,
                                         clock_monotonic_ns(), samples,
                                         ACCT_MAX_SNAP);
    for (uint32_t i = 0; i < count; i++)
        if (g_before[i].state != TASK_ZOMBIE && samples[i].valid)
            first = false;

    if (!start_workers(1, 2))
        return 1;
    task_handle_t worker = g_workers[0].handle;
    count = snapshot(g_after);
    bool sampled = task_cpu_sampler_sample(sampler, g_after, count,
                                            clock_monotonic_ns(), samples,
                                            ACCT_MAX_SNAP);
    bool new_ok = false;
    for (uint32_t i = 0; i < count; i++)
        if (lifecycle_matches(&g_after[i], worker))
            new_ok = sampled && !samples[i].valid;

    timer_sleep(100);
    count = snapshot(g_after);
    sampled = task_cpu_sampler_sample(sampler, g_after, count,
                                      clock_monotonic_ns(), samples,
                                      ACCT_MAX_SNAP);
    for (uint32_t i = 0; i < count; i++)
        if (lifecycle_matches(&g_after[i], worker))
            new_ok = new_ok && sampled && samples[i].valid;

    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    bool finished = wait_workers(1, false, 2000);
    bool zombie = false, frozen = false;
    uint64_t zombie_runtime = 0;
    for (uint32_t tries = 0; tries < 2000 && !zombie; tries++) {
        task_snapshot_t task = {0};
        if (scheduler_snapshot_task_by_handle(worker, &task) &&
            task.state == TASK_ZOMBIE) {
            zombie = true;
            zombie_runtime = task.runtime_ns_total;
        } else {
            timer_sleep(1);
        }
    }
    if (zombie) {
        timer_sleep(20);
        count = snapshot(g_after);
        for (uint32_t i = 0; i < count; i++)
            if (lifecycle_matches(&g_after[i], worker) &&
                g_after[i].state == TASK_ZOMBIE) {
                (void)task_cpu_sampler_sample(
                    sampler, g_after, count, clock_monotonic_ns(), samples,
                    ACCT_MAX_SNAP);
                frozen = g_after[i].runtime_ns_total == zombie_runtime &&
                         samples[i].valid && samples[i].cpu_x10 == 0;
            }
    }

    accounttest_worker_quiescence_t quiescence = {0};
    bool removed = accounttest_wait_workers_reaped(
        &worker, 1, 30000, &quiescence);
    bool ok = first && new_ok && finished && zombie && frozen && removed &&
              quiescence.handles_gone == 1 && !quiescence.free_inflight;
    serial_write_all("[ACCOUNT][LIFECYCLE] ");
    serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" first="); u64(first);
    serial_write_all(" new="); u64(new_ok);
    serial_write_all(" zombie="); u64(zombie && frozen);
    serial_write_all(" removed="); u64(removed);
    serial_write_all(" active_reap_calls="); u64(quiescence.active_reap_calls);
    serial_write_all(" free_inflight="); u64(quiescence.free_inflight);
    serial_write_all("\n");
    return ok ? 0 : 1;
}
typedef struct{uint32_t reads;volatile uint8_t done;volatile uint64_t failures,migrations;} clock_smp_ctx_t;static clock_smp_ctx_t g_clock_smp[ACCT_MAX_WORKERS];static semaphore_t g_clock_smp_completion;static clock_anomaly_event_t g_clock_events[32];static void clock_smp_worker(void*arg){clock_smp_ctx_t*c=arg;uint64_t last=0;uint32_t slot=TASK_CPU_SLOT_NONE;for(uint32_t i=0;i<c->reads;i++){uint64_t now=clock_monotonic_ns();if(now<last)__atomic_add_fetch(&c->failures,1,__ATOMIC_RELAXED);last=now;uint32_t next=account_current_slot();if(slot!=TASK_CPU_SLOT_NONE&&next!=TASK_CPU_SLOT_NONE&&slot!=next)__atomic_add_fetch(&c->migrations,1,__ATOMIC_RELAXED);slot=next;if(!(i&255u))schedule_voluntary();}__atomic_store_n(&c->done,1,__ATOMIC_RELEASE);sem_signal(&g_clock_smp_completion);}static uint32_t parse_large(const char*s,uint32_t fallback){task_id_t v;return s&&task_id_parse_decimal(s,&v)&&v&&v<=1000000?(uint32_t)v:fallback;}static int clock_events_test(void){uint32_t n=clock_events_snapshot(g_clock_events,32);for(uint32_t i=0;i<n;i++){serial_write_all("[ACCOUNT][CLOCK_EVENT] seq=");u64(g_clock_events[i].sequence);serial_write_all(" kind=");u64(g_clock_events[i].kind);serial_write_all(" cpu=");u64(g_clock_events[i].cpu_slot);serial_write_all(" delta_ns=");u64(g_clock_events[i].delta_ns);serial_write_all("\n");}serial_write_all("[ACCOUNT][CLOCK_EVENTS] PASS count=");u64(n);serial_write_all("\n");return 0;}
static task_handle_t g_clock_smp_handles[ACCT_MAX_WORKERS];
static int clock_smp_quiescent_test(uint32_t workers,uint32_t reads)
{
    if(!workers||workers>ACCT_MAX_WORKERS)return 1;
    clock_stats_t before,after;task_reaper_stats_t rb,ra;
    clock_stats_snapshot(&before);scheduler_reaper_stats_snapshot(&rb);
    uint32_t base=reads/workers,extra=reads%workers,made=0;sem_init(&g_clock_smp_completion,0);scheduler_test_set_lifecycle_log_quiet(true);
    for(uint32_t i=0;i<workers;i++){
        g_clock_smp[i]=(clock_smp_ctx_t){.reads=base+(i<extra)};
        if(!thread_create_named_handle(clock_smp_worker,&g_clock_smp[i],
                                       "clock-smp",&g_clock_smp_handles[i]))break;
        made++;
    }
    bool done=made==workers;
    if(done)for(uint32_t i=0;i<workers;i++)sem_wait(&g_clock_smp_completion);
    accounttest_worker_quiescence_t q={0};bool quiet=done&&made==workers&&accounttest_wait_workers_reaped(g_clock_smp_handles,workers,30000,&q);scheduler_test_set_lifecycle_log_quiet(false);
    uint64_t failures=0,migrations=0;for(uint32_t i=0;i<made;i++){
        failures+=__atomic_load_n(&g_clock_smp[i].failures,__ATOMIC_RELAXED);
        migrations+=__atomic_load_n(&g_clock_smp[i].migrations,__ATOMIC_RELAXED);}
    clock_stats_snapshot(&after);scheduler_reaper_stats_snapshot(&ra);
    bool ok=made==workers&&done&&quiet&&!failures&&
        after.api_regressions==before.api_regressions&&
        after.source_local_regressions==before.source_local_regressions&&
        after.cross_cpu_lag_clamps==before.cross_cpu_lag_clamps&&
        after.source_retry_exhaustions==before.source_retry_exhaustions&&
        after.state_corruptions==before.state_corruptions&&q.handles_gone==workers&&!q.free_inflight;
    serial_write_all("[ACCOUNT][CLOCK_SMP] ");serial_write_all(ok?"PASS":"FAIL");
    serial_write_all(" reads=");u64(reads);serial_write_all(" migrations=");u64(migrations);
    serial_write_all(" api_regressions=");u64(after.api_regressions-before.api_regressions);
    serial_write_all(" local_regressions=");u64(after.source_local_regressions-before.source_local_regressions);
    serial_write_all(" cross_lag=");u64(after.cross_cpu_lag_clamps-before.cross_cpu_lag_clamps);
    serial_write_all(" worker_handles_gone=");u64(q.handles_gone);serial_write_all(" global_reaped_delta=");u64(ra.reaped-rb.reaped);
    serial_write_all(" active_reaped=");u64(q.active_reaped);serial_write_all(" active_reap_calls=");u64(q.active_reap_calls);
    serial_write_all(" free_inflight=");u64(q.free_inflight);serial_write_all("\n");return ok?0:1;
}

static int reaper_quiescence_test(uint32_t workers)
{if(!workers||workers>ACCT_MAX_WORKERS)return 1;sem_init(&g_clock_smp_completion,0);scheduler_test_set_lifecycle_log_quiet(true);uint32_t made=0;for(uint32_t i=0;i<workers;i++){g_clock_smp[i]=(clock_smp_ctx_t){.reads=1};if(!thread_create_named_handle(clock_smp_worker,&g_clock_smp[i],"account-reap",&g_clock_smp_handles[i]))break;made++;}bool done=made==workers;if(done)for(uint32_t i=0;i<workers;i++)sem_wait(&g_clock_smp_completion);accounttest_worker_quiescence_t q={0};bool quiet=done&&accounttest_wait_workers_reaped(g_clock_smp_handles,workers,30000,&q);scheduler_test_set_lifecycle_log_quiet(false);bool ok=quiet&&q.handles_gone==workers&&q.active_reap_calls&&q.free_inflight==0;serial_write_all(ok?"[ACCOUNT][REAPER_QUIESCENCE] PASS workers=":"[ACCOUNT][REAPER_QUIESCENCE] FAIL workers=");u64(workers);serial_write_all(" gone=");u64(q.handles_gone);serial_write_all(" active_reap_calls=");u64(q.active_reap_calls);serial_write_all(" active_reaped=");u64(q.active_reaped);serial_write_all(" free_inflight=");u64(q.free_inflight);serial_write_all("\n");return ok?0:1;}

static int lapic_after_load_test(void)
{
    int rc=clock_smp_quiescent_test(32,1000000);int config=0,liveness=0,rate=0;
    if(!rc){config=lapic_config_test()==0;liveness=lapic_liveness_test(500)==0;
        rate=lapic_rate_test(2000,5)==0;rc|=!config||!liveness||!rate;}
    serial_write_all(rc?"[ACCOUNT][LAPIC_AFTER_LOAD] FAIL":"[ACCOUNT][LAPIC_AFTER_LOAD] PASS");
    serial_write_all(" workers=32 reads=1000000 reaped=32 config=");u64(config);
    serial_write_all(" liveness=");u64(liveness);serial_write_all(" rate=");u64(rate);serial_write_all("\n");
    return rc?1:0;
}

typedef struct{volatile uint8_t stop,done;volatile uint64_t failures,migrations;uint64_t last_ticks,last_ns;}hpet_reader_ctx_t;
static hpet_reader_ctx_t g_hpet_readers[ACCT_MAX_WORKERS];
static void hpet_rollover_reader(void*arg){hpet_reader_ctx_t*c=arg;uint32_t slot=TASK_CPU_SLOT_NONE;while(!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE)){hpet_counter_sample_t s;uint64_t ns=clock_monotonic_ns();if(ns<c->last_ns)__atomic_add_fetch(&c->failures,1,__ATOMIC_RELAXED);c->last_ns=ns;if(!hpet_read_counter_sample(&s)||!s.valid||s.ticks<c->last_ticks)__atomic_add_fetch(&c->failures,1,__ATOMIC_RELAXED);else c->last_ticks=s.ticks;uint32_t next=account_current_slot();if(slot!=TASK_CPU_SLOT_NONE&&next!=TASK_CPU_SLOT_NONE&&slot!=next)__atomic_add_fetch(&c->migrations,1,__ATOMIC_RELAXED);slot=next;schedule_voluntary();}__atomic_store_n(&c->done,1,__ATOMIC_RELEASE);}
static bool hpet_readers_start(uint32_t count){if(!count||count>ACCT_MAX_WORKERS)return false;for(uint32_t i=0;i<count;i++){g_hpet_readers[i]=(hpet_reader_ctx_t){0};if(!thread_create_named(hpet_rollover_reader,&g_hpet_readers[i],"hpet-reader"))return false;}return true;}
static bool hpet_readers_stop(uint32_t count,uint64_t*failures,uint64_t*migrations){for(uint32_t i=0;i<count;i++)__atomic_store_n(&g_hpet_readers[i].stop,1,__ATOMIC_RELEASE);uint64_t deadline=timer_get_uptime_ms()+5000;bool done=false;while(timer_get_uptime_ms()<deadline){done=true;for(uint32_t i=0;i<count;i++)if(!__atomic_load_n(&g_hpet_readers[i].done,__ATOMIC_ACQUIRE))done=false;if(done)break;timer_sleep(1);}*failures=0;*migrations=0;for(uint32_t i=0;i<count;i++){*failures+=__atomic_load_n(&g_hpet_readers[i].failures,__ATOMIC_RELAXED);*migrations+=__atomic_load_n(&g_hpet_readers[i].migrations,__ATOMIC_RELAXED);}return done;}
static const char*hpet_mode_name(hpet_counter_read_mode_t mode){return mode==HPET_COUNTER_READ_SPLIT64_STABLE?"SPLIT64_STABLE":mode==HPET_COUNTER_READ_EXTENDED32?"EXTENDED32":"NONE";}
static int hpet_stats_test(void){hpet_counter_stats_t s;hpet_counter_stats_snapshot(&s);uint64_t wrap_ns=hpet_counter_to_ns(1ULL<<32);serial_write_all("[ACCOUNT][HPET_STATS]\ncounter_width=");u64(s.counter_width_bits);serial_write_all("\nread_mode=");serial_write_all(hpet_mode_name(s.read_mode));serial_write_all("\nreads=");u64(s.reads);serial_write_all("\nsplit_retries=");u64(s.split_retries);serial_write_all("\nretry_exhaustion=");u64(s.split_retry_exhaustions);serial_write_all("\nlow32_rollovers=");u64(s.low32_rollovers);serial_write_all("\nmax_retries=");u64(s.max_split_retries);serial_write_all("\nperiod_fs=");u64(hpet_period_fs());serial_write_all("\nlow32_wrap_ns=");u64(wrap_ns);serial_write_all("\n");return hpet_is_available()&&s.counter_width_bits&&(s.read_mode!=HPET_COUNTER_READ_NONE)&&!s.split_retry_exhaustions?0:1;}
static int hpet_rollover_test(uint32_t wraps){if(!hpet_is_available()||!wraps)return 1;hpet_counter_sample_t initial,near,after;if(!hpet_read_counter_sample(&initial))return 1;uint64_t until=(1ULL<<32)-(uint32_t)initial.ticks;uint64_t until_ms=hpet_counter_to_ns(until)/1000000ULL;if(until_ms>100)timer_sleep((uint32_t)(until_ms-100));if(!hpet_read_counter_sample(&near))return 1;clock_stats_t ca,cb;clock_stats_snapshot(&ca);hpet_counter_test_stats_reset();uint32_t readers=g_cpu_count*4;if(readers>ACCT_MAX_WORKERS)readers=ACCT_MAX_WORKERS;if(!hpet_readers_start(readers))return 1;uint32_t target=(uint32_t)(near.ticks>>32)+wraps;uint64_t deadline=timer_get_uptime_ms()+(uint64_t)wraps*50000ULL+5000;bool observed=false;after=near;while(timer_get_uptime_ms()<deadline){if(!hpet_read_counter_sample(&after))break;if((uint32_t)(after.ticks>>32)>=target){observed=true;break;}timer_sleep(1);}uint64_t failures,migrations;bool stopped=hpet_readers_stop(readers,&failures,&migrations);clock_stats_snapshot(&cb);hpet_counter_stats_t hs;hpet_counter_stats_snapshot(&hs);bool clean=cb.api_regressions==ca.api_regressions&&cb.source_local_regressions==ca.source_local_regressions&&cb.cross_cpu_lag_clamps==ca.cross_cpu_lag_clamps&&cb.source_retry_exhaustions==ca.source_retry_exhaustions&&cb.state_corruptions==ca.state_corruptions&&!hs.split_retry_exhaustions;bool ok=observed&&stopped&&!failures&&clean&&(uint32_t)near.ticks>(uint32_t)after.ticks;serial_write_all(ok?"[ACCOUNT][HPET_ROLLOVER] PASS":"[ACCOUNT][HPET_ROLLOVER] FAIL");serial_write_all(" wraps=");u64(wraps);serial_write_all(" high_before=");u64(near.ticks>>32);serial_write_all(" high_after=");u64(after.ticks>>32);serial_write_all(" low_before=");u64((uint32_t)near.ticks);serial_write_all(" low_after=");u64((uint32_t)after.ticks);serial_write_all(" api=");u64(cb.api_regressions-ca.api_regressions);serial_write_all(" local=");u64(cb.source_local_regressions-ca.source_local_regressions);serial_write_all(" cross=");u64(cb.cross_cpu_lag_clamps-ca.cross_cpu_lag_clamps);serial_write_all(" split_retries=");u64(hs.split_retries);serial_write_all(" retry_exhaustion=");u64(hs.split_retry_exhaustions);serial_write_all(" reader_failures=");u64(failures);serial_write_all(" migrations=");u64(migrations);serial_write_all("\n");return ok?0:1;}
static int hpet_wrap_soak_test(uint32_t duration){if(duration<130000||!hpet_is_available())return 1;clock_stats_t ca,cb;clock_stats_snapshot(&ca);hpet_counter_test_stats_reset();uint32_t readers=g_cpu_count*4;if(readers>ACCT_MAX_WORKERS)readers=ACCT_MAX_WORKERS;if(!hpet_readers_start(readers))return 1;uint64_t deadline=timer_get_uptime_ms()+duration;while(timer_get_uptime_ms()<deadline)timer_sleep(100);uint64_t failures,migrations;bool stopped=hpet_readers_stop(readers,&failures,&migrations);clock_stats_snapshot(&cb);hpet_counter_stats_t hs;hpet_counter_stats_snapshot(&hs);bool ok=stopped&&!failures&&hs.low32_rollovers>=3&&!hs.split_retry_exhaustions&&cb.api_regressions==ca.api_regressions&&cb.source_local_regressions==ca.source_local_regressions&&cb.cross_cpu_lag_clamps==ca.cross_cpu_lag_clamps&&cb.source_retry_exhaustions==ca.source_retry_exhaustions&&cb.state_corruptions==ca.state_corruptions;serial_write_all(ok?"[ACCOUNT][HPET_WRAP_SOAK] PASS":"[ACCOUNT][HPET_WRAP_SOAK] FAIL");serial_write_all(" duration_ms=");u64(duration);serial_write_all(" wraps=");u64(hs.low32_rollovers);serial_write_all(" split_retries=");u64(hs.split_retries);serial_write_all(" retry_exhaustion=");u64(hs.split_retry_exhaustions);serial_write_all(" api=");u64(cb.api_regressions-ca.api_regressions);serial_write_all(" local=");u64(cb.source_local_regressions-ca.source_local_regressions);serial_write_all(" cross=");u64(cb.cross_cpu_lag_clamps-ca.cross_cpu_lag_clamps);serial_write_all(" migrations=");u64(migrations);serial_write_all("\n");return ok?0:1;}
static int check(void){int rc=stats();rc|=hpet_validation_selftest()?0:1;rc|=hpet_counter_access_selftest()?0:1;hpet_counter_stats_t hs;hpet_counter_stats_snapshot(&hs);hpet_runtime_snapshot_t hpet;timer_clockevent_snapshot_t event;bool hpet_ok=hpet_runtime_snapshot(&hpet)&&hpet.main_counter_enabled&&!hpet.legacy_replacement_enabled&&!hpet.timer0_interrupt_enabled&&!hpet.timer0_periodic_enabled&&!hpet.timer0_fsb_enabled&&!hpet.timer0_route_enabled&&!hpet.timer0_pending&&!hpet.stray_irqs;bool event_ok=timer_clockevent_snapshot(&event)&&timer_clockevent_validate()&&event.source==TIMER_CLOCKEVENT_BSP_LAPIC&&event.period_us==1000u;rc|=hs.split_retry_exhaustions?1:0;rc|=(hpet_is_available()&&hpet_period_fs()&&hpet_ok&&event_ok)?0:1;int config=lapic_config_test()==0;int live=lapic_liveness_test(250)==0;rc|=!config||!live;scheduler_accounting_stats_t a;scheduler_accounting_stats_snapshot(&a);if(!a.accounting_events||!a.runtime_ns_accounted)rc=1;serial_write_all(rc?"[ACCOUNT][CHECK] FAIL\n":"[ACCOUNT][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC hpet_timer0=QUIESCENT lapic_config=1 lapic_liveness=1 accounting=1 metrics=1\n");return rc;}
int cmd_accounttest(int argc,char**argv){const char*sub=argc>1?argv[1]:"check";uint32_t window=parse(argc>2?argv[2]:0,!strcmp(sub,"lapic")?2000:3000);if(!strcmp(sub,"clock"))return clock_test();if(!strcmp(sub,"clock-contract"))return clock_contract_test();if(!strcmp(sub,"clock-wrapper-loop"))return clock_wrapper_loop_test(parse_large(argc>2?argv[2]:NULL,100000));if(!strcmp(sub,"clock-loop")){uint32_t rounds=parse_large(argc>2?argv[2]:NULL,10);if(rounds>100)rounds=10;return clock_loop_test(rounds);}if(!strcmp(sub,"sleep-profile")){uint32_t target=parse_large(argc>2?argv[2]:NULL,10),samples=parse_large(argc>3?argv[3]:NULL,128);if(target>10000)target=10;if(samples>ACCOUNT_SLEEP_PROFILE_MAX_SAMPLES)samples=128;return sleep_profile_test(target,samples);}if(!strcmp(sub,"sleep-profile-set"))return sleep_profile_set_test();if(!strcmp(sub,"clock-classifier"))return clock_classifier_test();if(!strcmp(sub,"clock-smp"))return clock_smp_quiescent_test(parse_large(argc>2?argv[2]:NULL,g_cpu_count*4),parse_large(argc>3?argv[3]:NULL,1000000));if(!strcmp(sub,"reaper-quiescence"))return reaper_quiescence_test(parse_large(argc>2?argv[2]:NULL,32));if(!strcmp(sub,"clock-events"))return clock_events_test();if(!strcmp(sub,"hpet-stats"))return hpet_stats_test();if(!strcmp(sub,"hpet-rollover"))return hpet_rollover_test(parse_large(argc>2?argv[2]:NULL,1));if(!strcmp(sub,"hpet-wrap-soak"))return hpet_wrap_soak_test(parse(argc>2?argv[2]:NULL,130000));if(!strcmp(sub,"clock-stats"))return stats();if(!strcmp(sub,"clock-reset-test-stats")){clock_test_stats_reset();hpet_counter_test_stats_reset();serial_write_all("[ACCOUNT][CLOCK_RESET] PASS\n");return 0;}if(!strcmp(sub,"check-hard"))return stats();if(!strcmp(sub,"lapic-config"))return lapic_config_test();if(!strcmp(sub,"lapic-liveness"))return lapic_liveness_test(parse(argc>2?argv[2]:NULL,500));if(!strcmp(sub,"lapic-rate")||!strcmp(sub,"lapic"))return lapic_rate_test(parse(argc>2?argv[2]:NULL,2000),parse_large(argc>3?argv[3]:NULL,5));if(!strcmp(sub,"lapic-after-load"))return lapic_after_load_test();if(!strcmp(sub,"sampler"))return task_metrics_sampler_selftest()?0:1;if(!strcmp(sub,"format"))return task_metrics_format_selftest()?0:1;if(!strcmp(sub,"long"))return task_metrics_long_selftest()?0:1;if(!strcmp(sub,"lifecycle"))return lifecycle();if((!strcmp(sub,"ui-telemetry")||!strcmp(sub,"ui"))){bool on=argc>2&&!strcmp(argv[2],"on");task_metrics_ui_telemetry_set(on);serial_write_all(on?"[ACCOUNT][UI_TELEMETRY] ON\n":"[ACCOUNT][UI_TELEMETRY] OFF\n");return 0;}if(!strcmp(sub,"stats"))return stats();if(!strcmp(sub,"idle"))return workload("IDLE",window,0,0);if(!strcmp(sub,"busy"))return workload("BUSY",window,1,0);if(!strcmp(sub,"sleep"))return workload("SLEEP",window,1,1);if(!strcmp(sub,"share"))return workload("SHARE",window,2,0);if(!strcmp(sub,"allcpu"))return workload("ALLCPU",window,g_cpu_count*2,0);if(!strcmp(sub,"migrate"))return workload("MIGRATE",window,g_cpu_count>1?g_cpu_count*2:1,2);if(!strcmp(sub,"check"))return check();if(!strcmp(sub,"all")||!strcmp(sub,"core")){int rc=clock_test();rc|=lapic_config_test();rc|=lapic_liveness_test(500);rc|=lapic_rate_test(2000,5);rc|=task_metrics_sampler_selftest()?0:1;rc|=task_metrics_format_selftest()?0:1;rc|=task_metrics_long_selftest()?0:1;rc|=lifecycle();rc|=check();serial_write_all(rc?"[ACCOUNT][CORE] FAIL\n":"[ACCOUNT][CORE] PASS\n");return rc;}serial_write_all("[ACCOUNT][TEST] FAIL reason=unknown-subcommand\n");return 1;}
