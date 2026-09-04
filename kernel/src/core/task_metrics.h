#ifndef TASK_METRICS_H
#define TASK_METRICS_H
#include "scheduler.h"
#define TASK_CPU_BASELINE_MAX 4096u
typedef struct { task_id_t id; uint64_t runtime_ns; uint8_t valid; } task_cpu_baseline_entry_t;
typedef struct {
 task_cpu_baseline_entry_t entries[TASK_CPU_BASELINE_MAX]; task_cpu_baseline_entry_t scratch[TASK_CPU_BASELINE_MAX]; uint32_t count;
 uint64_t sample_time_ns, anomalies, dropped_baselines; uint8_t initialized;
} task_cpu_sampler_t;
typedef struct { uint32_t cpu_x10; uint8_t valid, anomalous; } task_cpu_sample_t;
typedef struct { uint64_t runtime_regressions, over_100_samples, dropped_baselines; } task_metrics_stats_t;
bool task_metrics_muldiv_u64(uint64_t value,uint64_t multiplier,uint64_t divisor,uint64_t *out);
void task_cpu_sampler_reset(task_cpu_sampler_t *sampler);
bool task_cpu_sampler_sample(task_cpu_sampler_t *sampler,const task_snapshot_t *snap,
 uint32_t count,uint64_t now_ns,task_cpu_sample_t *out,uint32_t out_capacity);
bool task_metrics_format_runtime(uint64_t runtime_ns,char *out,uint32_t out_size);
void task_metrics_stats_snapshot(task_metrics_stats_t *out);
void task_metrics_ui_telemetry_set(bool enabled);
bool task_metrics_ui_telemetry_enabled(void);
bool task_metrics_sampler_selftest(void);
bool task_metrics_format_selftest(void);
bool task_metrics_long_selftest(void);
#endif
