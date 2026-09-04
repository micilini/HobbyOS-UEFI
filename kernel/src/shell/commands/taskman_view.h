#ifndef TASKMAN_VIEW_H
#define TASKMAN_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "cmd_taskman.h"

#define TASKMAN_VIEW_SEMANTIC_BOUNDARIES 12u
#define TASKMAN_VIEW_INTERNAL_GAPS 7u
#define TASKMAN_VIEW_GROUP_SEPARATORS 5u

typedef enum
{
    TASKMAN_ALIGN_LEFT = 0,
    TASKMAN_ALIGN_RIGHT
} taskman_alignment_t;

typedef enum
{
    TASKMAN_BOUNDARY_NONE = 0,
    TASKMAN_BOUNDARY_GAP,
    TASKMAN_BOUNDARY_GROUP
} taskman_boundary_kind_t;

typedef struct
{
    task_format_column_id_t left;
    task_format_column_id_t right;
    uint16_t offset;
    taskman_boundary_kind_t kind;
} taskman_visual_boundary_t;

typedef struct
{
    task_format_column_id_t id;
    uint16_t slot_offset;
    uint16_t slot_width;
    uint16_t content_offset;
    uint16_t content_width;
    taskman_alignment_t alignment;
    taskman_boundary_kind_t boundary_after;
} taskman_visual_column_t;

typedef struct
{
    taskman_layout_mode_t mode;
    uint32_t region_width;
    uint32_t name_width;
    taskman_visual_column_t columns[TASK_FORMAT_COL_COUNT];
    taskman_visual_boundary_t boundaries[TASKMAN_VIEW_SEMANTIC_BOUNDARIES];
    uint32_t boundary_count;
    uint32_t internal_gap_count;
    uint32_t separator_offsets[TASKMAN_VIEW_GROUP_SEPARATORS];
    uint32_t separator_count;
    uint32_t exact_width;
} taskman_table_schema_t;

typedef struct
{
    uint32_t length;
    uint32_t expected_length;
    uint32_t separators_written;
    uint32_t separator_mismatches;
    uint32_t field_overflows;
    uint32_t field_truncations;
    uint32_t hidden_truncations;
    uint32_t semantic_boundaries_expected;
    uint32_t semantic_boundaries_written;
    uint32_t internal_gaps_expected;
    uint32_t internal_gaps_written;
    uint32_t group_separators_expected;
    uint32_t group_separators_written;
    uint32_t adjacent_content_touches;
    uint32_t boundary_overwrites;
    uint32_t split_overlaps;
    uint32_t split_right_offset;
    uint8_t name_truncated;
    uint8_t mandatory_columns_present;
} taskman_view_diagnostics_t;

typedef struct
{
    uint32_t background;
    uint32_t title;
    uint32_t metadata;
    uint32_t summary_label;
    uint32_t summary_value;
    uint32_t header_background;
    uint32_t header_text;
    uint32_t separator;
    uint32_t row_text;
    uint32_t running;
    uint32_t dim;
    uint32_t zombie;
    uint32_t warning;
    uint32_t selected_background;
    uint32_t selected_text;
    uint32_t footer;
} taskman_view_palette_t;

typedef enum
{
    TASKMAN_VIEW_TEXT_TITLE = 0,
    TASKMAN_VIEW_TEXT_SUMMARY,
    TASKMAN_VIEW_TEXT_FOOTER,
    TASKMAN_VIEW_TEXT_FALLBACK,
    TASKMAN_VIEW_TEXT_EMPTY
} taskman_view_text_role_t;

uint32_t taskman_table_minimum_width(taskman_layout_mode_t mode);
uint32_t taskman_table_fixed_width(taskman_layout_mode_t mode);
bool taskman_table_schema_build(taskman_layout_mode_t mode,
                                uint32_t region_width,
                                taskman_table_schema_t *out);
const taskman_view_palette_t *taskman_view_palette(void);

bool taskman_view_build_header_text(const taskman_table_schema_t *schema,
                                    char *out, uint32_t capacity,
                                    taskman_view_diagnostics_t *diag);
bool taskman_view_build_row_text(const taskman_table_schema_t *schema,
                                 const task_snapshot_t *task,
                                 const task_cpu_sample_t *sample,
                                 bool selected, char *out,
                                 uint32_t capacity,
                                 taskman_view_diagnostics_t *diag);
bool taskman_view_build_header(const taskman_table_schema_t *schema,
                               ConsoleCell *out_cells, uint32_t capacity,
                               taskman_view_diagnostics_t *diag);
bool taskman_view_build_row(const taskman_table_schema_t *schema,
                            const task_snapshot_t *task,
                            const task_cpu_sample_t *sample, bool selected,
                            ConsoleCell *out_cells, uint32_t capacity,
                            taskman_view_diagnostics_t *diag);

bool taskman_view_compose_split_text(char *out, uint32_t width,
                                     const char *left, const char *right,
                                     uint32_t minimum_gap,
                                     taskman_view_diagnostics_t *diag);
bool taskman_view_build_title_text(const taskman_layout_t *layout,
                                   uint32_t refresh_ms,
                                   const taskman_navigation_t *nav,
                                   char *out, uint32_t capacity,
                                   char *left, uint32_t left_capacity,
                                   char *right, uint32_t right_capacity,
                                   taskman_view_diagnostics_t *diag);
bool taskman_view_text_cells(const char *text, uint32_t width,
                             taskman_view_text_role_t role,
                             uint32_t split_right_offset,
                             ConsoleCell *out_cells, uint32_t capacity);

#endif
