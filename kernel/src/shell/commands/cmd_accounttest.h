#ifndef CMD_ACCOUNTTEST_H
#define CMD_ACCOUNTTEST_H
#include <stdbool.h>
#include <stdint.h>
#include "../../core/scheduler.h"

#define ACCOUNT_SLEEP_EARLY_TOLERANCE_NS 1000000ULL
#define ACCOUNT_SLEEP_PROFILE_MAX_SAMPLES 256u

typedef struct {
 uint32_t requested_ms;
 task_wait_result_t wait_result;
 uint64_t elapsed_ns;
 uint64_t minimum_ns;
 uint64_t overshoot_ns;
 uint64_t early_by_ns;
 uint8_t early;
 uint8_t late_soft;
 uint8_t wait_ok;
} account_sleep_sample_t;

typedef struct {
 uint32_t target_ms;
 uint32_t samples;
 uint32_t early_count;
 uint32_t wait_errors;
 uint32_t late_soft_count;
 uint64_t min_ns;
 uint64_t p50_ns;
 uint64_t p95_ns;
 uint64_t p99_ns;
 uint64_t max_ns;
 uint64_t max_overshoot_ns;
 uint64_t api_delta;
 uint64_t local_delta;
 uint64_t retry_delta;
 uint64_t saturation_delta;
 uint64_t state_delta;
 uint64_t not_ready_delta;
 uint8_t hard_valid;
 uint8_t quality_good;
} account_sleep_profile_t;

typedef enum {
 ACCOUNT_CLOCK_FAIL_NONE             = 0,
 ACCOUNT_CLOCK_FAIL_START_ZERO       = 1u << 0,
 ACCOUNT_CLOCK_FAIL_MONOTONIC        = 1u << 1,
 /* Bits 2..4 belonged to the legacy late-or-early delay contract. */
 ACCOUNT_CLOCK_FAIL_SLEEP_EARLY_10   = 1u << 2,
 ACCOUNT_CLOCK_FAIL_SLEEP_EARLY_100  = 1u << 3,
 ACCOUNT_CLOCK_FAIL_SLEEP_EARLY_1000 = 1u << 4,
 ACCOUNT_CLOCK_FAIL_API_REGRESSION   = 1u << 5,
 ACCOUNT_CLOCK_FAIL_LOCAL_REGRESSION = 1u << 6,
 ACCOUNT_CLOCK_FAIL_RETRY            = 1u << 7,
 ACCOUNT_CLOCK_FAIL_SATURATION       = 1u << 8,
 ACCOUNT_CLOCK_FAIL_STATE            = 1u << 9,
 ACCOUNT_CLOCK_FAIL_NOT_READY        = 1u << 10,
 ACCOUNT_CLOCK_FAIL_UPTIME_WRAPPER   = 1u << 11,
 ACCOUNT_CLOCK_FAIL_SLEEP_WAIT_10    = 1u << 12,
 ACCOUNT_CLOCK_FAIL_SLEEP_WAIT_100   = 1u << 13,
 ACCOUNT_CLOCK_FAIL_SLEEP_WAIT_1000  = 1u << 14
} account_clock_failure_t;

typedef struct {
 uint64_t reads;
 uint64_t monotonic_failures;
 uint64_t d10_ns;
 uint64_t d100_ns;
 uint64_t d1000_ns;
 uint64_t api_delta;
 uint64_t local_delta;
 uint64_t cross_lag_delta;
 uint64_t retry_delta;
 uint64_t saturation_delta;
 uint64_t state_delta;
 uint64_t not_ready_delta;
 uint64_t wrapper_before_ms;
 uint64_t wrapper_ms;
 uint64_t wrapper_after_ms;
 account_sleep_sample_t sleep10;
 account_sleep_sample_t sleep100;
 account_sleep_sample_t sleep1000;
 uint32_t sleep_early_mask;
 uint32_t sleep_late_mask;
 uint32_t sleep_wait_error_mask;
 uint8_t legacy_exact_equal;
 uint8_t wrapper_interval_valid;
 uint32_t failed_mask;
} account_clock_result_t;

typedef struct {
 uint32_t expected,handles_gone,active_reap_calls,active_reaped,stable_passes;
 uint32_t ready,running,blocked,sleeping,zombies,exiting;
 uint64_t global_reaped_delta,free_inflight;
} accounttest_worker_quiescence_t;
bool accounttest_wait_workers_reaped(const task_handle_t *workers,uint32_t worker_count,
 uint64_t timeout_ms,accounttest_worker_quiescence_t *out);
bool accounttest_wait_reaper_idle(uint64_t timeout_ms,uint32_t stable_passes,
 task_reaper_stats_t *out);
bool accounttest_uptime_wrapper_sample_valid(uint64_t before_ms,
 uint64_t uptime_ms,uint64_t after_ms);
bool accounttest_uptime_wrapper_contract_selftest(void);
bool accounttest_sleep_sample_classify(uint32_t requested_ms,
 task_wait_result_t wait_result,uint64_t elapsed_ns,
 account_sleep_sample_t *out);
bool accounttest_sleep_percentiles_selftest(void);
int cmd_accounttest(int argc,char **argv);
#endif
