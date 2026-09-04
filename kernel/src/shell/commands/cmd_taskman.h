#ifndef CMD_TASKMAN_H
#define CMD_TASKMAN_H

#include <stdbool.h>
#include <stdint.h>
#include "../../core/scheduler.h"
#include "../../core/task_metrics.h"
#include "../../core/task_format.h"
#include "../../core/input_event.h"
#include "../../graphics/console.h"

#define TASKMAN_DEFAULT_REFRESH_MS 1000u
#define TASKMAN_MIN_REFRESH_MS 50u
#define TASKMAN_MAX_REFRESH_MS 2000u
#define TASKMAN_INITIAL_CAPACITY 128u
#define TASKMAN_V1_MAX_ENTRIES 4096u

typedef enum { TASKMAN_LAYOUT_WIDE=0, TASKMAN_LAYOUT_COMPACT,
    TASKMAN_LAYOUT_TOO_NARROW, TASKMAN_LAYOUT_TOO_SHORT } taskman_layout_mode_t;
typedef struct {
    taskman_layout_mode_t mode;
    uint32_t console_cols,console_rows,anchor_x,anchor_y;
    uint32_t region_x,region_y,region_width,region_height;
    uint32_t title_rows,summary_rows,column_header_rows,footer_rows;
    uint32_t visible_task_rows,name_width,minimum_cols,minimum_rows;
} taskman_layout_t;
typedef task_format_column_id_t taskman_column_id_t;
typedef task_format_column_spec_t taskman_column_spec_t;
#define TASKMAN_COL_PID TASK_FORMAT_COL_PID
#define TASKMAN_COL_USER TASK_FORMAT_COL_USER
#define TASKMAN_COL_CPU TASK_FORMAT_COL_CPU
#define TASKMAN_COL_LCPU TASK_FORMAT_COL_LCPU
#define TASKMAN_COL_AFF TASK_FORMAT_COL_AFF
#define TASKMAN_COL_STATE TASK_FORMAT_COL_STATE
#define TASKMAN_COL_CLASS TASK_FORMAT_COL_CLASS
#define TASKMAN_COL_QUANTUM TASK_FORMAT_COL_QUANTUM
#define TASKMAN_COL_TIME TASK_FORMAT_COL_TIME
#define TASKMAN_COL_CPU_PERCENT TASK_FORMAT_COL_CPU_PERCENT
#define TASKMAN_COL_KILL TASK_FORMAT_COL_KILL
#define TASKMAN_COL_MEMORY TASK_FORMAT_COL_MEMORY
#define TASKMAN_COL_NAME TASK_FORMAT_COL_NAME
#define TASKMAN_COL_COUNT TASK_FORMAT_COL_COUNT
typedef struct {
    task_snapshot_t *tasks; task_cpu_sample_t *samples;
    uint32_t capacity,count,total; uint64_t registry_generation,sample_time_ns;
    uint8_t truncated,allocation_failed;
    uint64_t frames,reallocations,snapshot_retries,sort_passes;
} taskman_model_t;
typedef task_snapshot_result_t (*taskman_snapshot_source_fn)(
    task_snapshot_t *buffer,uint32_t capacity,uint32_t offset,void *ctx);
typedef struct {
    uint32_t total_captured,running,ready,blocked,sleeping,zombies,idle;
    uint32_t kill_pending,killable,protected_tasks;
} taskman_summary_t;
typedef struct {
    uint32_t page_index,page_count,selected_index;
    task_id_t selected_id; uint32_t first_visible,last_visible;
    uint64_t page_changes,selection_changes,clamps;
} taskman_navigation_t;
typedef enum { TASKMAN_ACTION_NONE=0,TASKMAN_ACTION_EXIT,TASKMAN_ACTION_SELECTION,
    TASKMAN_ACTION_PAGE } taskman_input_action_t;
typedef struct {uint32_t length;uint64_t field_truncations;uint8_t name_truncated,
 anomalous_cpu_marked,mandatory_columns_present;} taskman_row_diagnostics_t;
typedef struct {
    uint64_t sessions,frames,inputs,ignored_inputs,render_failures;
    uint64_t full_frames,fallback_frames;
    uint64_t auto_exit_sessions,auto_exit_shortfalls;
    uint64_t max_full_frame_gap_ns;
    uint64_t page_changes,selection_changes,clamps,model_reallocations;
    uint64_t scroll_delta,last_session_scroll_delta,region_writes,region_clears;
    uint64_t shell_exit_scroll_delta,last_session_shell_exit_scroll_delta;
    uint64_t last_session_full_frames,last_session_fallback_frames;
    uint64_t last_session_clipped_writes,total_clipped_writes,row_overflows;
    uint64_t page_range_violations,selection_range_violations,duplicate_row_ids;
    uint64_t hidden_truncations,stale_cells,model_allocation_failures;
    uint64_t summary_format_failures,footer_format_failures,builder_truncations;
    uint64_t pre_render_clear_failures;
    uint64_t present_calls,rows_examined,rows_changed,rows_unchanged;
    uint64_t cells_examined,cells_changed,cells_unchanged;
    uint64_t glyph_changes,style_changes;
    uint64_t first_frame_full_presents,cleanup_clears;
    uint64_t geometry_invalidation_clears,stable_frame_full_clears;
    uint64_t tail_rows_cleared_by_present;
    uint64_t schema_builds,schema_failures,separator_mismatches;
    uint64_t field_overflows,field_truncations,split_overlaps;
    uint64_t visual_workspace_live,visual_workspace_allocations;
    uint64_t visual_workspace_reallocations,visual_workspace_frees;
    uint64_t visual_workspace_failures;
    uint64_t last_session_present_calls,last_session_rows_examined;
    uint64_t last_session_rows_changed,last_session_rows_unchanged;
    uint64_t last_session_cells_examined,last_session_cells_changed;
    uint64_t last_session_cells_unchanged,last_session_glyph_changes;
    uint64_t last_session_style_changes,last_session_first_frame_full_presents;
    uint64_t last_session_cleanup_clears,last_session_geometry_clears;
    uint64_t last_session_stable_frame_full_clears;
    uint64_t last_session_tail_rows_cleared_by_present;
    uint64_t last_session_separator_mismatches;
    uint64_t last_session_field_overflows,last_session_split_overlaps;
    uint32_t last_captured,last_total,last_pages,last_visible_rows;
    task_id_t last_selected_id;
    uint32_t last_layout_mode;
    uint8_t last_truncated,model_live;
} taskman_stats_t;

bool taskman_layout_compute(uint32_t cols,uint32_t rows,uint32_t anchor_x,
                            uint32_t anchor_y,taskman_layout_t *out);
bool taskman_format_name(const char *name,uint32_t width,char *out,
                         uint32_t out_size,bool *truncated);
bool taskman_format_mem(uint64_t bytes,char *out,uint32_t out_size);
bool taskman_build_header(taskman_layout_mode_t mode,uint32_t name_width,
                          char *out,uint32_t out_size);
bool taskman_format_row(const taskman_layout_t *layout,const task_snapshot_t *task,
                        const task_cpu_sample_t *sample,bool selected,char *out,
                        uint32_t out_size,taskman_row_diagnostics_t *diag);
uint32_t taskman_format_summary_lines(const taskman_layout_t *layout,
                        const taskman_summary_t *summary,
                        const taskman_model_t *model,uint32_t refresh_ms,
                        uint64_t uptime_ns,const taskman_navigation_t *nav,
                        char lines[][CONSOLE_MAX_COLS_STORAGE + 1],
                        uint32_t max_lines,uint8_t *truncated);
uint32_t taskman_format_footer_lines(const taskman_layout_t *layout,
                        const taskman_model_t *model,
                        const taskman_navigation_t *nav,
                        char lines[][CONSOLE_MAX_COLS_STORAGE + 1],
                        uint32_t max_lines,uint8_t *truncated);
bool taskman_model_capture_from_source(taskman_model_t *model,
                        taskman_snapshot_source_fn source,void *source_ctx,
                        uint32_t max_entries);
void taskman_model_release(taskman_model_t *model);
void taskman_sort_by_pid(task_snapshot_t *tasks,uint32_t count);
void taskman_summary_compute(const task_snapshot_t *tasks,uint32_t count,
                             taskman_summary_t *out);
void taskman_navigation_reconcile(taskman_navigation_t *nav,
                                  const task_snapshot_t *tasks,uint32_t count,
                                  uint32_t visible_rows);
taskman_input_action_t taskman_navigation_input(taskman_navigation_t *nav,
                                                input_event_t event,
                                                uint32_t count,
                                                uint32_t visible_rows);
bool taskman_refresh_parse(const char *text,uint32_t *out);
void taskman_stats_snapshot(taskman_stats_t *out);
void taskman_test_set_cap(uint32_t cap);
void taskman_test_fail_next_allocation(void);
uint32_t taskman_rows_requested(uint32_t count,uint32_t visible_rows);
bool taskman_test_set_geometry(uint32_t cols,uint32_t rows);
void taskman_test_clear_geometry(void);
void taskman_test_reset_controls(void);
bool taskman_test_controls_default(void);
bool taskman_test_arm_auto_exit_full_frames(uint32_t frames);
uint32_t taskman_test_auto_exit_pending(void);
bool taskman_test_stats_reset(void);
int cmd_taskman(int argc,char **argv);
#endif
