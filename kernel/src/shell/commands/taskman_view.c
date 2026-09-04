#include "taskman_view.h"

#include "../../libc/memory.h"
#include "../../libc/string.h"
#include "../../smp/smp_topology.h"

#define TASKMAN_WIDE_MINIMUM 118u
#define TASKMAN_COMPACT_MINIMUM 71u

typedef struct
{
    char *out;
    uint32_t capacity;
    uint32_t length;
    uint8_t failed;
} view_builder_t;

static const taskman_view_palette_t g_palette = {
    .background = CONSOLE_COLOR_HOBBYOS_BLUE,
    .title = CONSOLE_COLOR_WHITE,
    .metadata = 0xFF65D9FFu,
    .summary_label = 0xFF75B7E8u,
    .summary_value = CONSOLE_COLOR_WHITE,
    .header_background = 0xFF102A48u,
    .header_text = CONSOLE_COLOR_WHITE,
    .separator = 0xFF86A6C4u,
    .row_text = 0xFFE8EEF5u,
    .running = 0xFF63E67Au,
    .dim = 0xFF9AA7B4u,
    .zombie = 0xFFFF6B6Bu,
    .warning = 0xFFFFD166u,
    .selected_background = 0xFF245A8Du,
    .selected_text = CONSOLE_COLOR_WHITE,
    .footer = 0xFFFFD166u,
};

static void vb_init(view_builder_t *b, char *out, uint32_t capacity)
{
    b->out = out;
    b->capacity = capacity;
    b->length = 0;
    b->failed = !out || !capacity;
    if (out && capacity) out[0] = 0;
}

static void vb_char(view_builder_t *b, char value)
{
    if (b->failed) return;
    if (value == '\n' || value == '\r' || value == '\t') value = ' ';
    if (b->length + 1u >= b->capacity) {
        b->failed = 1;
        return;
    }
    b->out[b->length++] = value;
    b->out[b->length] = 0;
}

static void vb_text(view_builder_t *b, const char *text)
{
    if (!text) {
        b->failed = 1;
        return;
    }
    while (*text) vb_char(b, *text++);
}

static void vb_u64(view_builder_t *b, uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(reverse));
    while (count) vb_char(b, reverse[--count]);
}

static uint32_t table_slot_width(taskman_layout_mode_t mode,
                                 task_format_column_id_t id,
                                 const task_format_column_spec_t *spec)
{
    uint32_t width = mode == TASKMAN_LAYOUT_WIDE ? spec->long_width :
                                                   spec->compact_width;
    /* Boundaries consume the last cell of their left-hand slots.  Borrow
     * only from the bounded NAME remainder to keep anomaly and compact
     * control values intact. */
    if (id == TASK_FORMAT_COL_CPU_PERCENT)
        width += mode == TASKMAN_LAYOUT_COMPACT ? 2u : 1u;
    if (mode == TASKMAN_LAYOUT_COMPACT && id == TASK_FORMAT_COL_MEMORY)
        width++;
    return width;
}

uint32_t taskman_table_fixed_width(taskman_layout_mode_t mode)
{
    uint32_t width = 1u;
    for (uint32_t i = 0; i < TASK_FORMAT_COL_NAME; i++) {
        const task_format_column_spec_t *spec =
            task_format_column_spec((task_format_column_id_t)i);
        if (!spec) return 0;
        width += table_slot_width(mode, (task_format_column_id_t)i, spec);
    }
    return width;
}

uint32_t taskman_table_minimum_width(taskman_layout_mode_t mode)
{
    if (mode == TASKMAN_LAYOUT_WIDE) return TASKMAN_WIDE_MINIMUM;
    if (mode == TASKMAN_LAYOUT_COMPACT) return TASKMAN_COMPACT_MINIMUM;
    return 0;
}

static taskman_boundary_kind_t boundary_kind_after(
    task_format_column_id_t id)
{
    if (id == TASK_FORMAT_COL_USER || id == TASK_FORMAT_COL_AFF ||
        id == TASK_FORMAT_COL_QUANTUM ||
        id == TASK_FORMAT_COL_CPU_PERCENT ||
        id == TASK_FORMAT_COL_MEMORY)
        return TASKMAN_BOUNDARY_GROUP;
    if (id < TASK_FORMAT_COL_NAME)
        return TASKMAN_BOUNDARY_GAP;
    return TASKMAN_BOUNDARY_NONE;
}

static bool alignment_right(task_format_column_id_t id)
{
    return id == TASK_FORMAT_COL_PID || id == TASK_FORMAT_COL_CPU ||
           id == TASK_FORMAT_COL_LCPU || id == TASK_FORMAT_COL_QUANTUM ||
           id == TASK_FORMAT_COL_TIME ||
           id == TASK_FORMAT_COL_CPU_PERCENT ||
           id == TASK_FORMAT_COL_MEMORY;
}

bool taskman_table_schema_build(taskman_layout_mode_t mode,
                                uint32_t region_width,
                                taskman_table_schema_t *out)
{
    if (!out || (mode != TASKMAN_LAYOUT_WIDE &&
                 mode != TASKMAN_LAYOUT_COMPACT) ||
        region_width < taskman_table_minimum_width(mode) ||
        region_width > CONSOLE_MAX_COLS_STORAGE)
        return false;
    memset(out, 0, sizeof(*out));
    out->mode = mode;
    out->region_width = region_width;
    uint32_t offset = 1u;
    for (uint32_t i = 0; i < TASK_FORMAT_COL_COUNT; i++) {
        const task_format_column_spec_t *spec =
            task_format_column_spec((task_format_column_id_t)i);
        if (!spec) return false;
        taskman_visual_column_t *column = &out->columns[i];
        column->id = (task_format_column_id_t)i;
        column->alignment = alignment_right(column->id) ?
                            TASKMAN_ALIGN_RIGHT : TASKMAN_ALIGN_LEFT;
        column->slot_offset = (uint16_t)offset;
        if (i == TASK_FORMAT_COL_NAME)
            column->slot_width = (uint16_t)(region_width - offset);
        else
            column->slot_width = (uint16_t)table_slot_width(
                mode, column->id, spec);
        column->content_offset = column->slot_offset;
        column->content_width = column->slot_width;
        offset += column->slot_width;
    }
    out->name_width = out->columns[TASK_FORMAT_COL_NAME].content_width;
    out->exact_width = offset;
    if (out->exact_width != region_width ||
        taskman_table_fixed_width(mode) >= region_width)
        return false;

#ifndef HOBBYOS_TASKMAN_NEGATIVE_SCHEMA_WITHOUT_BOUNDARIES
    for (uint32_t i = 0; i < TASK_FORMAT_COL_NAME; i++) {
        taskman_visual_column_t *column = &out->columns[i];
        taskman_boundary_kind_t kind = boundary_kind_after(column->id);
        taskman_visual_boundary_t *boundary =
            &out->boundaries[out->boundary_count++];
        boundary->left = column->id;
        boundary->right = (task_format_column_id_t)(i + 1u);
        boundary->offset = (uint16_t)(column->slot_offset +
                                      column->slot_width - 1u);
        boundary->kind = kind;
        column->boundary_after = kind;
        column->content_width--;
        if (kind == TASKMAN_BOUNDARY_GROUP)
            out->separator_offsets[out->separator_count++] = boundary->offset;
        else
            out->internal_gap_count++;
    }
#endif

    for (uint32_t i = 0; i < TASK_FORMAT_COL_COUNT; i++) {
        taskman_visual_column_t *column = &out->columns[i];
        if (!column->content_width) return false;
    }
#ifndef HOBBYOS_TASKMAN_NEGATIVE_SCHEMA_WITHOUT_BOUNDARIES
    if (out->boundary_count != TASKMAN_VIEW_SEMANTIC_BOUNDARIES ||
        out->internal_gap_count != TASKMAN_VIEW_INTERNAL_GAPS ||
        out->separator_count != TASKMAN_VIEW_GROUP_SEPARATORS)
        return false;
#endif
    out->name_width = out->columns[TASK_FORMAT_COL_NAME].content_width;
    return true;
}

const taskman_view_palette_t *taskman_view_palette(void)
{
    return &g_palette;
}

static void canvas_init(char *out, uint32_t width)
{
    memset(out, ' ', width);
    out[width] = 0;
}

static uint32_t text_length(const char *text)
{
    uint32_t length = 0;
    if (text) while (text[length]) length++;
    return length;
}

static void canvas_field(char *canvas, const taskman_visual_column_t *column,
                         const char *text, bool name,
                         taskman_view_diagnostics_t *diag)
{
    uint32_t source_length = text_length(text);
    uint32_t width = column->content_width;
    uint32_t copy_length = source_length;
    uint32_t source_start = 0;
    uint32_t target = column->content_offset;
    bool truncated = source_length > width;
    if (truncated) {
        diag->field_truncations++;
        if (column->alignment == TASKMAN_ALIGN_RIGHT) {
            copy_length = width ? width - 1u : 0u;
            source_start = source_length - copy_length;
            target++;
        } else {
            copy_length = width;
        }
    }
    if (!truncated && column->alignment == TASKMAN_ALIGN_RIGHT &&
        copy_length < width)
        target += width - copy_length;
    for (uint32_t i = 0; i < copy_length; i++) {
        char value = text[source_start + i];
        if (value == '\n' || value == '\r' || value == '\t') value = ' ';
        canvas[target + i] = value;
    }
    if (truncated && width) {
        if (name && width >= 3u) {
            canvas[column->content_offset + width - 3u] = '.';
            canvas[column->content_offset + width - 2u] = '.';
            canvas[column->content_offset + width - 1u] = '.';
        } else if (column->alignment == TASKMAN_ALIGN_RIGHT) {
            canvas[column->content_offset] = '~';
        } else {
            canvas[column->content_offset + width - 1u] = '~';
        }
    }
}

static void diag_finish(const taskman_table_schema_t *schema,
                        const char *canvas,
                        taskman_view_diagnostics_t *diag)
{
    diag->length = text_length(canvas);
    diag->expected_length = schema->region_width;
    diag->semantic_boundaries_expected = TASKMAN_VIEW_SEMANTIC_BOUNDARIES;
    diag->internal_gaps_expected = TASKMAN_VIEW_INTERNAL_GAPS;
    diag->group_separators_expected = TASKMAN_VIEW_GROUP_SEPARATORS;
    for (uint32_t i = 0; i < schema->boundary_count; i++) {
        const taskman_visual_boundary_t *boundary = &schema->boundaries[i];
        char expected = boundary->kind == TASKMAN_BOUNDARY_GROUP ? '|' : ' ';
        if (boundary->offset >= schema->region_width ||
            canvas[boundary->offset] != expected) {
            diag->boundary_overwrites++;
            diag->adjacent_content_touches++;
            if (boundary->kind == TASKMAN_BOUNDARY_GROUP)
                diag->separator_mismatches++;
            continue;
        }
        diag->semantic_boundaries_written++;
        if (boundary->kind == TASKMAN_BOUNDARY_GROUP) {
            diag->group_separators_written++;
            diag->separators_written++;
        } else {
            diag->internal_gaps_written++;
        }
    }
    if (diag->semantic_boundaries_written !=
            diag->semantic_boundaries_expected ||
        diag->internal_gaps_written != diag->internal_gaps_expected ||
        diag->group_separators_written != diag->group_separators_expected)
        diag->boundary_overwrites++;
    diag->mandatory_columns_present =
        diag->length == schema->region_width &&
        !diag->field_overflows && !diag->hidden_truncations &&
        !diag->adjacent_content_touches && !diag->boundary_overwrites;
}

static bool canvas_begin(const taskman_table_schema_t *schema, char *out,
                         uint32_t capacity,
                         taskman_view_diagnostics_t *diag)
{
    if (!schema || !out || capacity <= schema->region_width || !diag)
        return false;
    memset(diag, 0, sizeof(*diag));
    canvas_init(out, schema->region_width);
    for (uint32_t i = 0; i < schema->boundary_count; i++) {
        const taskman_visual_boundary_t *boundary = &schema->boundaries[i];
        if (boundary->offset >= schema->region_width) {
            diag->field_overflows++;
            return false;
        }
        out[boundary->offset] =
            boundary->kind == TASKMAN_BOUNDARY_GROUP ? '|' : ' ';
    }
    return true;
}

bool taskman_view_build_header_text(const taskman_table_schema_t *schema,
                                    char *out, uint32_t capacity,
                                    taskman_view_diagnostics_t *diag)
{
    if (!canvas_begin(schema, out, capacity, diag)) return false;
    for (uint32_t i = 0; i < TASK_FORMAT_COL_COUNT; i++) {
        const task_format_column_spec_t *spec =
            task_format_column_spec((task_format_column_id_t)i);
        const char *header = schema->mode == TASKMAN_LAYOUT_WIDE ?
                             spec->long_header : spec->compact_header;
        canvas_field(out, &schema->columns[i], header, false, diag);
    }
    diag_finish(schema, out, diag);
    return !diag->separator_mismatches && !diag->field_overflows &&
           !diag->adjacent_content_touches && !diag->boundary_overwrites &&
           diag->length == schema->region_width;
}

static const char *field_value(const task_format_fields_t *fields,
                               task_format_column_id_t id)
{
    switch (id) {
    case TASK_FORMAT_COL_PID: return fields->pid;
    case TASK_FORMAT_COL_USER: return fields->user;
    case TASK_FORMAT_COL_CPU: return fields->cpu;
    case TASK_FORMAT_COL_LCPU: return fields->last_cpu;
    case TASK_FORMAT_COL_AFF: return fields->affinity;
    case TASK_FORMAT_COL_STATE: return fields->state;
    case TASK_FORMAT_COL_CLASS: return fields->task_class;
    case TASK_FORMAT_COL_QUANTUM: return fields->quantum;
    case TASK_FORMAT_COL_TIME: return fields->runtime;
    case TASK_FORMAT_COL_CPU_PERCENT: return fields->cpu_percent;
    case TASK_FORMAT_COL_KILL: return fields->kill_status;
    case TASK_FORMAT_COL_MEMORY: return fields->memory;
    case TASK_FORMAT_COL_NAME: return fields->name;
    default: return "";
    }
}

bool taskman_view_build_row_text(const taskman_table_schema_t *schema,
                                 const task_snapshot_t *task,
                                 const task_cpu_sample_t *sample,
                                 bool selected, char *out,
                                 uint32_t capacity,
                                 taskman_view_diagnostics_t *diag)
{
    if (!task || !canvas_begin(schema, out, capacity, diag)) return false;
    task_format_fields_t fields;
    task_format_style_t style = schema->mode == TASKMAN_LAYOUT_WIDE ?
                                TASK_FORMAT_LONG : TASK_FORMAT_COMPACT;
    if (!task_format_snapshot_fields(task, sample, style, &fields))
        return false;
    out[0] = selected ? '>' : ' ';
    for (uint32_t i = 0; i < TASK_FORMAT_COL_COUNT; i++) {
        bool name = i == TASK_FORMAT_COL_NAME;
        canvas_field(out, &schema->columns[i], field_value(&fields,
                     (task_format_column_id_t)i), name, diag);
    }
    uint32_t source_name_length = text_length(task->name[0] ? task->name :
                                                               "(unnamed)");
    diag->name_truncated = source_name_length > schema->name_width;
    diag_finish(schema, out, diag);
    return !diag->separator_mismatches && !diag->field_overflows &&
           !diag->adjacent_content_touches && !diag->boundary_overwrites &&
           diag->length == schema->region_width;
}

static void cells_from_canvas(const char *text, uint32_t width,
                              uint32_t fg, uint32_t bg,
                              ConsoleCell *out_cells)
{
    for (uint32_t i = 0; i < width; i++) {
        out_cells[i].c = text[i] ? text[i] : ' ';
        out_cells[i].fg = fg;
        out_cells[i].bg = bg;
    }
}

bool taskman_view_build_header(const taskman_table_schema_t *schema,
                               ConsoleCell *out_cells, uint32_t capacity,
                               taskman_view_diagnostics_t *diag)
{
    if (!schema || !out_cells || capacity < schema->region_width) return false;
    char text[CONSOLE_MAX_COLS_STORAGE + 1u];
    if (!taskman_view_build_header_text(schema, text, sizeof(text), diag))
        return false;
    cells_from_canvas(text, schema->region_width, g_palette.header_text,
                      g_palette.header_background, out_cells);
    for (uint32_t i = 0; i < schema->separator_count; i++)
        out_cells[schema->separator_offsets[i]].fg = g_palette.separator;
    return true;
}

static void style_column(ConsoleCell *cells,
                         const taskman_visual_column_t *column,
                         uint32_t color)
{
    for (uint32_t i = 0; i < column->content_width; i++)
        cells[column->content_offset + i].fg = color;
}

bool taskman_view_build_row(const taskman_table_schema_t *schema,
                            const task_snapshot_t *task,
                            const task_cpu_sample_t *sample, bool selected,
                            ConsoleCell *out_cells, uint32_t capacity,
                            taskman_view_diagnostics_t *diag)
{
    if (!schema || !out_cells || capacity < schema->region_width) return false;
    char text[CONSOLE_MAX_COLS_STORAGE + 1u];
    if (!taskman_view_build_row_text(schema, task, sample, selected, text,
                                     sizeof(text), diag))
        return false;
    uint32_t background = selected ? g_palette.selected_background :
                                     g_palette.background;
    cells_from_canvas(text, schema->region_width,
                      selected ? g_palette.selected_text : g_palette.row_text,
                      background, out_cells);
    uint32_t state_color = g_palette.row_text;
    if (task->state == TASK_RUNNING) state_color = g_palette.running;
    else if (task->state == TASK_BLOCKED || task->state == TASK_SLEEPING)
        state_color = g_palette.dim;
    else if (task->state == TASK_ZOMBIE) state_color = g_palette.zombie;
    style_column(out_cells, &schema->columns[TASK_FORMAT_COL_STATE],
                 state_color);
    if (task->state == TASK_ZOMBIE)
        style_column(out_cells, &schema->columns[TASK_FORMAT_COL_KILL],
                     g_palette.zombie);
    else if (task->kill_pending || task->exit_started)
        style_column(out_cells, &schema->columns[TASK_FORMAT_COL_KILL],
                     g_palette.warning);
    if (sample && sample->valid && sample->anomalous &&
        task->state != TASK_ZOMBIE)
        style_column(out_cells,
                     &schema->columns[TASK_FORMAT_COL_CPU_PERCENT],
                     g_palette.warning);
    for (uint32_t i = 0; i < schema->separator_count; i++)
        out_cells[schema->separator_offsets[i]].fg = g_palette.separator;
    out_cells[0].fg = selected ? CONSOLE_COLOR_WHITE : g_palette.dim;
    return true;
}

static bool text_is_clean(const char *text)
{
    if (!text) return false;
    for (; *text; text++)
        if (*text == '\n' || *text == '\r' || *text == '\t') return false;
    return true;
}

bool taskman_view_compose_split_text(char *out, uint32_t width,
                                     const char *left, const char *right,
                                     uint32_t minimum_gap,
                                     taskman_view_diagnostics_t *diag)
{
    if (!out || !width || !left || !right || !diag ||
        !text_is_clean(left) || !text_is_clean(right))
        return false;
    memset(diag, 0, sizeof(*diag));
    uint32_t left_length = text_length(left);
    uint32_t right_length = text_length(right);
    diag->expected_length = width;
    if (left_length + minimum_gap + right_length > width) {
        diag->split_overlaps = 1;
        return false;
    }
    canvas_init(out, width);
    memcpy(out, left, left_length);
    uint32_t right_offset = width - right_length;
    memcpy(out + right_offset, right, right_length);
    diag->split_right_offset = right_offset;
    diag->length = width;
    diag->mandatory_columns_present = 1;
    return true;
}

bool taskman_view_build_title_text(const taskman_layout_t *layout,
                                   uint32_t refresh_ms,
                                   const taskman_navigation_t *nav,
                                   char *out, uint32_t capacity,
                                   char *left, uint32_t left_capacity,
                                   char *right, uint32_t right_capacity,
                                   taskman_view_diagnostics_t *diag)
{
    if (!layout || !nav || !out || capacity <= layout->region_width)
        return false;
    view_builder_t l;
    view_builder_t r;
    vb_init(&l, left, left_capacity);
    vb_text(&l, "HobbyOS TASKMAN");
    vb_init(&r, right, right_capacity);
    if (layout->mode == TASKMAN_LAYOUT_COMPACT) {
        vb_text(&r, "C | ");
        vb_u64(&r, refresh_ms);
        vb_text(&r, "ms | P ");
    } else {
        vb_text(&r, "WIDE | ");
        vb_u64(&r, refresh_ms);
        vb_text(&r, " ms | Page ");
    }
    vb_u64(&r, nav->page_index + 1u);
    vb_char(&r, '/');
    vb_u64(&r, nav->page_count);
    if (l.failed || r.failed) return false;
    return taskman_view_compose_split_text(out, layout->region_width,
                                            left, right, 2u, diag);
}

bool taskman_view_text_cells(const char *text, uint32_t width,
                             taskman_view_text_role_t role,
                             uint32_t split_right_offset,
                             ConsoleCell *out_cells, uint32_t capacity)
{
    if (!text || !out_cells || capacity < width || text_length(text) != width)
        return false;
    uint32_t fg = g_palette.row_text;
    uint32_t bg = g_palette.background;
    if (role == TASKMAN_VIEW_TEXT_TITLE) fg = g_palette.title;
    else if (role == TASKMAN_VIEW_TEXT_SUMMARY) fg = g_palette.summary_label;
    else if (role == TASKMAN_VIEW_TEXT_FOOTER) fg = g_palette.footer;
    else if (role == TASKMAN_VIEW_TEXT_FALLBACK) fg = g_palette.warning;
    cells_from_canvas(text, width, fg, bg, out_cells);
    if (role == TASKMAN_VIEW_TEXT_TITLE && split_right_offset < width) {
        for (uint32_t i = split_right_offset; i < width; i++)
            out_cells[i].fg = g_palette.metadata;
    } else if (role == TASKMAN_VIEW_TEXT_SUMMARY) {
        for (uint32_t i = 0; i < width; i++)
            if ((text[i] >= '0' && text[i] <= '9') || text[i] == '-' ||
                text[i] == '/')
                out_cells[i].fg = g_palette.summary_value;
    }
    return true;
}

static void vb_reset(view_builder_t *builder)
{
    vb_init(builder, builder->out, builder->capacity);
}

static void vb_u64_padded(view_builder_t *builder, uint64_t value,
                          uint32_t minimum)
{
    char reverse[21];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(reverse));
    while (count < minimum) reverse[count++] = '0';
    while (count) vb_char(builder, reverse[--count]);
}

static void vb_uptime(view_builder_t *builder, uint64_t uptime_ns)
{
    uint64_t seconds = uptime_ns / 1000000000ULL;
    vb_u64_padded(builder, seconds / 3600ULL, 2u);
    vb_char(builder, ':');
    vb_u64_padded(builder, (seconds / 60ULL) % 60ULL, 2u);
    vb_char(builder, ':');
    vb_u64_padded(builder, seconds % 60ULL, 2u);
}

static void vb_range(view_builder_t *builder,
                     const taskman_model_t *model,
                     const taskman_navigation_t *nav, bool words)
{
    if (words) vb_text(builder, "Showing ");
    if (model->count) {
        vb_u64(builder, nav->first_visible + 1u);
        vb_char(builder, '-');
        vb_u64(builder, nav->last_visible + 1u);
    } else {
        vb_text(builder, "0-0");
    }
    if (words) vb_text(builder, " of ");
    else vb_char(builder, '/');
    vb_u64(builder, model->total);
}

static bool summary_line_one(const taskman_layout_t *layout,
                             const taskman_summary_t *summary,
                             const taskman_model_t *model,
                             const taskman_navigation_t *nav,
                             char *out, char *left, char *right,
                             bool compact,
                             taskman_view_diagnostics_t *diag)
{
    view_builder_t l;
    view_builder_t r;
    vb_init(&l, left, 160u);
    vb_init(&r, right, 160u);
    if (compact) {
        vb_text(&l, "T "); vb_u64(&l, model->total);
        vb_text(&l, " R "); vb_u64(&l, summary->running);
        vb_text(&l, " Q "); vb_u64(&l, summary->ready);
        vb_text(&l, " B "); vb_u64(&l, summary->blocked);
        vb_text(&l, " S "); vb_u64(&l, summary->sleeping);
        vb_text(&l, " Z "); vb_u64(&l, summary->zombies);
        vb_range(&r, model, nav, false);
    } else {
        vb_text(&l, "TASKS Total "); vb_u64(&l, model->total);
        vb_text(&l, " Run "); vb_u64(&l, summary->running);
        vb_text(&l, " Ready "); vb_u64(&l, summary->ready);
        vb_text(&l, " Block "); vb_u64(&l, summary->blocked);
        vb_text(&l, " Sleep "); vb_u64(&l, summary->sleeping);
        vb_text(&l, " Zomb "); vb_u64(&l, summary->zombies);
        vb_text(&r, "VIEW "); vb_range(&r, model, nav, true);
    }
    return !l.failed && !r.failed &&
        taskman_view_compose_split_text(out, layout->region_width,
                                        left, right, 2u, diag);
}

static bool summary_line_two(const taskman_layout_t *layout,
                             const taskman_model_t *model,
                             uint64_t uptime_ns, char *out,
                             char *left, char *right, bool compact,
                             taskman_view_diagnostics_t *diag)
{
    view_builder_t l;
    view_builder_t r;
    vb_init(&l, left, 160u);
    vb_init(&r, right, 160u);
    if (compact) vb_text(&l, "CPU ");
    else vb_text(&l, "SYSTEM CPUs ");
    vb_u64(&l, smp_online_cpu_count());
    vb_text(&l, compact ? " UP " : " Up ");
    vb_uptime(&l, uptime_ns);
    if (!compact) vb_text(&r, "CAPTURED ");
    vb_u64(&r, model->count);
    vb_char(&r, '/');
    vb_u64(&r, model->total);
    if (model->truncated) vb_text(&r, " TRUNCATED");
    if (model->allocation_failed) vb_text(&r, " ALLOC FALLBACK");
    return !l.failed && !r.failed &&
        taskman_view_compose_split_text(out, layout->region_width,
                                        left, right, 2u, diag);
}

uint32_t taskman_format_summary_lines(const taskman_layout_t *layout,
                        const taskman_summary_t *summary,
                        const taskman_model_t *model,uint32_t refresh_ms,
                        uint64_t uptime_ns,const taskman_navigation_t *nav,
                        char lines[][CONSOLE_MAX_COLS_STORAGE + 1],
                        uint32_t max_lines,uint8_t *truncated)
{
    (void)refresh_ms;
    if (truncated) *truncated = 0;
    if (!layout || !summary || !model || !nav || !lines || max_lines < 2u)
        return 0;
    char left[160];
    char right[160];
    taskman_view_diagnostics_t diag;
    bool compact = layout->mode == TASKMAN_LAYOUT_COMPACT;
    bool first = summary_line_one(layout, summary, model, nav, lines[0],
                                  left, right, compact, &diag);
    if (!first && !compact)
        first = summary_line_one(layout, summary, model, nav, lines[0],
                                 left, right, true, &diag);
    bool second = summary_line_two(layout, model, uptime_ns, lines[1],
                                   left, right, compact, &diag);
    if (!second && !compact)
        second = summary_line_two(layout, model, uptime_ns, lines[1],
                                  left, right, true, &diag);
    if (!first || !second) {
        if (truncated) *truncated = 1;
        return 0;
    }
    return 2u;
}

static bool footer_selected(view_builder_t *builder,
                            const taskman_layout_t *layout,
                            const taskman_model_t *model,
                            const taskman_navigation_t *nav,
                            uint32_t available, bool compact)
{
    if (!model->tasks || !model->count || nav->selected_index >= model->count) {
        vb_text(builder, compact ? "Sel none" : "Selected none");
        return !builder->failed;
    }
    const task_snapshot_t *task = &model->tasks[nav->selected_index];
    vb_text(builder, compact ? "Sel " : "Selected PID ");
    vb_u64(builder, task->id);
    if (builder->failed || builder->length >= available) return false;
    uint32_t name_width = available - builder->length;
    if (name_width <= 1u) return true;
    vb_char(builder, ' ');
    name_width--;
    if (name_width > TASK_FORMAT_NAME_CAP - 1u)
        name_width = TASK_FORMAT_NAME_CAP - 1u;
    char name[TASK_FORMAT_NAME_CAP];
    bool ignored = false;
    if (!task_format_name(task->name, name_width, name, sizeof(name), &ignored))
        return false;
    vb_text(builder, name);
    return !builder->failed;
}

uint32_t taskman_format_footer_lines(const taskman_layout_t *layout,
                        const taskman_model_t *model,
                        const taskman_navigation_t *nav,
                        char lines[][CONSOLE_MAX_COLS_STORAGE + 1],
                        uint32_t max_lines,uint8_t *truncated)
{
    if (truncated) *truncated = 0;
    if (!layout || !model || !nav || !lines || max_lines < 2u) return 0;
    char left[160];
    char right[160];
    view_builder_t r;
    bool compact = layout->mode == TASKMAN_LAYOUT_COMPACT;
    vb_init(&r, right, sizeof(right));
    if (compact) vb_text(&r, "P ");
    else vb_text(&r, "Page ");
    vb_u64(&r, nav->page_index + 1u);
    vb_char(&r, '/');
    vb_u64(&r, nav->page_count);
    vb_text(&r, " | ");
    if (!compact) vb_text(&r, "Showing ");
    vb_range(&r, model, nav, false);
    if (r.failed || r.length + 2u >= layout->region_width) goto fail;
    uint32_t available = layout->region_width - r.length - 2u;
    view_builder_t l;
    vb_init(&l, left, sizeof(left));
    if (!footer_selected(&l, layout, model, nav, available, compact)) goto fail;
    taskman_view_diagnostics_t diag;
    if (!taskman_view_compose_split_text(lines[0], layout->region_width,
                                          left, right, 2u, &diag))
        goto fail;
    vb_reset(&l);
    vb_text(&l, compact ?
        "ESC Exit | Up/Dn Select | PgUp/PgDn Page | Home/End Jump" :
        "ESC Exit | Up/Down Select | PgUp/PgDn Page | Home/End Jump");
    if (l.failed || !taskman_view_compose_split_text(lines[1],
        layout->region_width, left, "", 0u, &diag))
        goto fail;
    return 2u;
fail:
    if (truncated) *truncated = 1;
    return 0;
}
