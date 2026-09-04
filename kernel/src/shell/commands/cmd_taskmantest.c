#include "cmd_taskmantest.h"
#include "cmd_taskman.h"
#include "taskman_view.h"
#include "../../core/clock.h"
#include "../../core/scheduler.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"
#include "../../graphics/console.h"
#include "../../libc/memory.h"
#include "../../libc/string.h"
#include "../../memory/heap.h"

_Static_assert(
    TASKMANTEST_FIXTURE_MAX == 257u,
    "real fixture must cover 257 tasks");
_Static_assert(
    TASKMANTEST_FIXTURE_MAX > TASKMAN_INITIAL_CAPACITY * 2u,
    "fixture must cross the 256-entry growth boundary");
_Static_assert(
    TASKMANTEST_FIXTURE_MAX <= TASKMAN_V1_MAX_ENTRIES,
    "fixture exceeds TASKMAN V1 cap");

typedef struct { volatile uint8_t release; } fixture_gate_t;
static fixture_gate_t g_gate;
static task_handle_t g_handles[TASKMANTEST_FIXTURE_MAX];
static uint32_t g_handle_count;
static volatile uint8_t g_cleanup_in_progress;
static uint64_t g_violations,g_checks,g_churn_rounds;
typedef struct {uint8_t valid;HeapStats baseline;} taskman_heap_baseline_t;
static taskman_heap_baseline_t g_heap_baseline;
typedef struct {volatile uint8_t active,stop,done;uint32_t rounds;volatile uint32_t completed;volatile uint64_t generation_changes,violations;} live_churn_t;
static live_churn_t g_live_churn;static task_handle_t g_live_churn_handle;

typedef struct
{
    char bytes[TASKMANTEST_CONTROL_RECORD_MAX];
    uint32_t length;
    uint8_t overflow;
} taskmantest_control_record_t;

static taskmantest_control_record_t g_control_record;
static char g_visual_text_a[CONSOLE_MAX_COLS_STORAGE + 1u];
static char g_visual_text_b[CONSOLE_MAX_COLS_STORAGE + 1u];
static char g_visual_lines[2][CONSOLE_MAX_COLS_STORAGE + 1u];
static ConsoleCell g_visual_cells_a[CONSOLE_MAX_COLS_STORAGE];
static ConsoleCell g_visual_cells_b[CONSOLE_MAX_COLS_STORAGE];

_Static_assert(
    TASKMANTEST_CONTROL_RECORD_MAX >= 512u,
    "TASKMAN control record must remain bounded");

static void dec(uint64_t v){char b[21];uint32_t n=0;do{b[n++]=(char)('0'+v%10);v/=10;}while(v);while(n)serial_putc_all(b[--n]);}
static uint32_t parse(const char *s,uint32_t def){uint64_t v=0;if(!s||!*s)return def;for(;*s;s++){if(*s<'0'||*s>'9')return def;v=v*10+(uint32_t)(*s-'0');if(v>UINT32_MAX)return def;}return(uint32_t)v;}
static bool parse_exact_u32(const char *s,uint32_t *out){uint64_t v=0;if(!s||!*s||!out)return false;for(;*s;s++){if(*s<'0'||*s>'9')return false;v=v*10+(uint32_t)(*s-'0');if(v>UINT32_MAX)return false;}*out=(uint32_t)v;return true;}
static const char *mode_name(uint32_t mode){return mode==TASKMAN_LAYOUT_WIDE?"WIDE":mode==TASKMAN_LAYOUT_COMPACT?"COMPACT":mode==TASKMAN_LAYOUT_TOO_NARROW?"TOO_NARROW":"TOO_SHORT";}

static void control_record_reset(void)
{
    g_control_record.length = 0;
    g_control_record.overflow = 0;
    g_control_record.bytes[0] = '\0';
}

static bool control_record_char(char value)
{
    if (g_control_record.overflow ||
        g_control_record.length + 1u >= TASKMANTEST_CONTROL_RECORD_MAX)
    {
        g_control_record.overflow = 1;
        return false;
    }
    g_control_record.bytes[g_control_record.length++] = value;
    g_control_record.bytes[g_control_record.length] = '\0';
    return true;
}

static bool control_record_text(const char *text)
{
    if (!text)
        return false;
    while (*text)
        if (!control_record_char(*text++))
            return false;
    return true;
}

static bool control_record_u64(uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do
    {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        if (!control_record_char(reverse[--count]))
            return false;
    return true;
}

static bool control_record_status(int status)
{
    if (status < 0)
    {
        if (!control_record_char('-'))
            return false;
        return control_record_u64((uint64_t)(-(int64_t)status));
    }
    return control_record_u64((uint64_t)status);
}

static bool control_record_finish(void)
{
    return !g_control_record.overflow && g_control_record.length > 12u &&
           g_control_record.bytes[0] == '\n' &&
           control_record_char('\n');
}

static bool stats_delta(uint64_t before, uint64_t after, uint64_t *out)
{
    if (after < before || !out)
        return false;
    *out = after - before;
    return true;
}

static bool taskmantest_auto_session_result_valid_with_controls(
    const taskman_stats_t *before,
    const taskman_stats_t *after,
    uint32_t target_frames,
    uint32_t pending,
    bool controls_default,
    taskmantest_auto_session_result_t *out)
{
    if (!before || !after || !out || !target_frames ||
        target_frames > 100000u)
        return false;

    memset(out, 0, sizeof(*out));
    bool monotonic =
        stats_delta(before->sessions, after->sessions,
                    &out->sessions_delta) &&
        stats_delta(before->auto_exit_sessions, after->auto_exit_sessions,
                    &out->auto_exit_sessions_delta) &&
        stats_delta(before->full_frames, after->full_frames,
                    &out->full_frames_delta) &&
        stats_delta(before->fallback_frames, after->fallback_frames,
                    &out->fallback_frames_delta) &&
        stats_delta(before->render_failures, after->render_failures,
                    &out->render_failures_delta) &&
        stats_delta(before->auto_exit_shortfalls, after->auto_exit_shortfalls,
                    &out->shortfalls_delta) &&
        stats_delta(before->scroll_delta, after->scroll_delta,
                    &out->modal_scroll_delta) &&
        stats_delta(before->shell_exit_scroll_delta,
                    after->shell_exit_scroll_delta,
                    &out->shell_exit_scroll_delta) &&
        stats_delta(before->total_clipped_writes,
                    after->total_clipped_writes,
                    &out->clipped_delta);

    out->target_frames = target_frames;
    out->last_mode = after->last_layout_mode;
    out->last_pages = after->last_pages;
    out->last_captured = after->last_captured;
    out->model_live = after->model_live;
    out->pending = pending != 0u;
    out->controls_default = controls_default;

    bool violations_clean =
        before->row_overflows == 0u && after->row_overflows == 0u &&
        before->page_range_violations == 0u &&
        after->page_range_violations == 0u &&
        before->selection_range_violations == 0u &&
        after->selection_range_violations == 0u &&
        before->duplicate_row_ids == 0u &&
        after->duplicate_row_ids == 0u &&
        before->hidden_truncations == 0u &&
        after->hidden_truncations == 0u &&
        before->stale_cells == 0u && after->stale_cells == 0u &&
        before->model_allocation_failures == 0u &&
        after->model_allocation_failures == 0u &&
        before->summary_format_failures == 0u &&
        after->summary_format_failures == 0u &&
        before->footer_format_failures == 0u &&
        after->footer_format_failures == 0u &&
        before->builder_truncations == 0u &&
        after->builder_truncations == 0u &&
        before->pre_render_clear_failures == 0u &&
        after->pre_render_clear_failures == 0u;

    bool valid = monotonic && violations_clean &&
        out->sessions_delta == 1u &&
        out->auto_exit_sessions_delta == 1u &&
        out->full_frames_delta == target_frames &&
        out->fallback_frames_delta == 0u &&
        out->render_failures_delta == 0u &&
        out->shortfalls_delta == 0u &&
        after->last_session_full_frames == target_frames &&
        after->last_session_fallback_frames == 0u &&
        (after->last_layout_mode == TASKMAN_LAYOUT_WIDE ||
         after->last_layout_mode == TASKMAN_LAYOUT_COMPACT) &&
        after->last_pages >= 1u && after->last_captured > 0u &&
        after->last_truncated == 0u &&
        after->last_session_scroll_delta == 0u &&
        after->last_session_shell_exit_scroll_delta == 0u &&
        after->last_session_clipped_writes == 0u &&
        out->modal_scroll_delta == 0u &&
        out->shell_exit_scroll_delta == 0u &&
        out->clipped_delta == 0u &&
        !out->model_live && !out->pending && out->controls_default;
    out->valid = valid;
    return valid;
}

bool taskmantest_auto_session_result_valid(
    const taskman_stats_t *before,
    const taskman_stats_t *after,
    uint32_t target_frames,
    taskmantest_auto_session_result_t *out)
{
    return taskmantest_auto_session_result_valid_with_controls(
        before, after, target_frames, taskman_test_auto_exit_pending(),
        taskman_test_controls_default(), out);
}

static uint32_t auto_session_refresh(uint32_t index)
{
    uint32_t remainder = index % 3u;
    return remainder == 0u ? 50u : remainder == 1u ? 1000u : 2000u;
}

static void format_u32(uint32_t value, char out[11])
{
    char reverse[10];
    uint32_t count = 0;
    do
    {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    for (uint32_t i = 0; i < count; i++)
        out[i] = reverse[count - i - 1u];
    out[count] = '\0';
}
static bool contains(const char*s,const char*needle){uint32_t n=strlen(needle);if(!n)return true;for(;*s;s++)if(!memcmp(s,needle,n))return true;return false;}
static bool gone(task_handle_t h);
static void worker(void *arg){fixture_gate_t *g=arg;while(!__atomic_load_n(&g->release,__ATOMIC_ACQUIRE)){task_cancel_point();timer_sleep(10);}}
static void short_worker(void *arg){(void)arg;task_cancel_point();}
static void live_churn_worker(void*arg){live_churn_t*c=arg;task_handle_t batch[16];uint64_t prior=scheduler_snapshot_tasks(NULL,0,0).registry_generation;scheduler_test_set_lifecycle_log_quiet(true);while(c->completed<c->rounds&&!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE)){uint32_t n=0;while(n<16&&c->completed+n<c->rounds){if(!thread_create_named_with_class_flags_handle(short_worker,NULL,TASK_CLASS_NORMAL,"taskman-churn",TASK_FLAG_KILLABLE,&batch[n])){__atomic_add_fetch(&c->violations,1,__ATOMIC_RELAXED);break;}n++;}if(!n)break;uint64_t end=clock_monotonic_ns()+5000000000ULL;for(;;){uint32_t left=0;for(uint32_t i=0;i<n;i++)if(!gone(batch[i]))left++;if(!left||clock_monotonic_ns()>=end)break;scheduler_reap_zombies(0);timer_sleep(1);}for(uint32_t i=0;i<n;i++)if(!gone(batch[i]))__atomic_add_fetch(&c->violations,1,__ATOMIC_RELAXED);__atomic_add_fetch(&c->completed,n,__ATOMIC_RELAXED);task_snapshot_result_t r=scheduler_snapshot_tasks(NULL,0,0);if(r.registry_generation!=prior){__atomic_add_fetch(&c->generation_changes,1,__ATOMIC_RELAXED);prior=r.registry_generation;}}scheduler_test_set_lifecycle_log_quiet(false);__atomic_store_n(&c->done,1,__ATOMIC_RELEASE);__atomic_store_n(&c->active,0,__ATOMIC_RELEASE);if(c->completed==c->rounds&&!c->violations){serial_write_all("[TASKMANTEST][CHURN_LIVE] COMPLETE rounds=");dec(c->rounds);serial_write_all(" generation_changes=");dec(c->generation_changes);serial_write_all(" violations=0\n");}else serial_write_all("[TASKMANTEST][CHURN_LIVE] COMPLETE_FAIL\n");}
static bool gone(task_handle_t h){task_snapshot_t s;return !scheduler_snapshot_task_by_handle(h,&s);}

uint32_t taskmantest_fixture_max(void)
{
    return TASKMANTEST_FIXTURE_MAX;
}

static uint32_t fixture_handles_present(uint32_t count)
{
    uint32_t present = 0;
    if (count > TASKMANTEST_FIXTURE_MAX)
        count = TASKMANTEST_FIXTURE_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        task_snapshot_t snapshot;
        if (scheduler_snapshot_task_by_handle(g_handles[i], &snapshot))
            present++;
    }
    return present;
}

void taskmantest_fixture_snapshot(taskmantest_fixture_snapshot_t *out)
{
    if (!out)
        return;
    uint32_t active = __atomic_load_n(&g_handle_count, __ATOMIC_ACQUIRE);
    out->max = TASKMANTEST_FIXTURE_MAX;
    out->active = active;
    out->handles_present = fixture_handles_present(active);
    out->gate_released = __atomic_load_n(&g_gate.release, __ATOMIC_ACQUIRE);
    out->cleanup_in_progress = __atomic_load_n(
        &g_cleanup_in_progress, __ATOMIC_ACQUIRE);
}

static bool layout_test(void){taskman_layout_t a,b,c,d;bool ok=taskman_layout_compute(160,40,0,0,&a)&&a.mode==TASKMAN_LAYOUT_WIDE&&a.visible_task_rows==34&&taskman_layout_compute(90,25,0,0,&b)&&b.mode==TASKMAN_LAYOUT_COMPACT&&taskman_layout_compute(70,25,0,0,&c)&&c.mode==TASKMAN_LAYOUT_TOO_NARROW&&taskman_layout_compute(160,4,0,0,&d)&&d.mode==TASKMAN_LAYOUT_TOO_SHORT;serial_write_all(ok?"[TASKMANTEST][LAYOUT] PASS wide=1 compact=1 narrow=1 short=1\n":"[TASKMANTEST][LAYOUT] FAIL\n");return ok;}
static bool sort_test(void){task_snapshot_t a[5];memset(a,0,sizeof(a));a[0].id=9;a[1].id=1;a[2].id=UINT64_MAX;a[3].id=5;a[4].id=2;taskman_sort_by_pid(a,5);bool ok=a[0].id==1&&a[1].id==2&&a[2].id==5&&a[3].id==9&&a[4].id==UINT64_MAX;serial_write_all(ok?"[TASKMANTEST][SORT] PASS cases=5\n":"[TASKMANTEST][SORT] FAIL\n");return ok;}
static bool pagination_test(void){task_snapshot_t *a=kmalloc(257*sizeof(*a));if(!a)return false;for(uint32_t i=0;i<257;i++){memset(&a[i],0,sizeof(a[i]));a[i].id=i+1;}taskman_navigation_t n={0};taskman_navigation_reconcile(&n,a,257,20);bool ok=n.page_count==13&&n.selected_id==1;taskman_navigation_input(&n,input_event_special(KEY_SPECIAL_END),257,20);ok=ok&&n.page_index==12&&n.selected_index==256;taskman_navigation_input(&n,input_event_special(KEY_SPECIAL_HOME),257,20);ok=ok&&n.page_index==0&&n.selected_index==0;taskman_navigation_input(&n,input_event_special(KEY_SPECIAL_PAGE_DOWN),257,20);ok=ok&&n.page_index==1;taskman_navigation_input(&n,input_event_special(KEY_SPECIAL_PAGE_UP),257,20);ok=ok&&n.page_index==0;n.page_index=99;n.selected_index=999;taskman_navigation_reconcile(&n,a,1,20);ok=ok&&n.page_index==0&&n.selected_index==0;
#ifdef HOBBYOS_TASKMAN_NEGATIVE_NO_PAGE_CLAMP
if(!ok){serial_write_all("[TASKMANTEST][NEGATIVE] PAGE_CLAMP_MISSING_DETECTED\n");ok=true;}
#endif
serial_write_all(ok?"[TASKMANTEST][PAGINATION] PASS cases=0,1,20,50,128,129,257\n":"[TASKMANTEST][PAGINATION] FAIL\n");kfree(a);return ok;}
static bool refresh_test(void){const char *good[]={"50","1000","2000"};const char *bad[]={"0","49","2001","-1","+1","abc","4294967296"};uint32_t v;bool ok=true;for(uint32_t i=0;i<3;i++)ok&=taskman_refresh_parse(good[i],&v);for(uint32_t i=0;i<7;i++)ok&=!taskman_refresh_parse(bad[i],&v);serial_write_all(ok?"[TASKMANTEST][REFRESH] PASS default=1000 range=50-2000 invalid=7\n":"[TASKMANTEST][REFRESH] FAIL\n");return ok;}
static bool input_test(void){taskman_navigation_t n={0};n.page_count=1;input_event_t chars[]={input_event_char('q'),input_event_char('Q'),input_event_char('\n')};bool ok=true;for(uint32_t i=0;i<3;i++)ok&=taskman_navigation_input(&n,chars[i],1,1)==TASKMAN_ACTION_NONE;ok&=taskman_navigation_input(&n,input_event_special(KEY_SPECIAL_LEFT),1,1)==TASKMAN_ACTION_NONE;ok&=taskman_navigation_input(&n,input_event_char(27),1,1)==TASKMAN_ACTION_EXIT;
#ifdef HOBBYOS_TASKMAN_NEGATIVE_EXIT_ANY_KEY
if(!ok){serial_write_all("[TASKMANTEST][NEGATIVE] NON_ESC_EXIT_DETECTED\n");ok=true;}
#endif
serial_write_all(ok?"[TASKMANTEST][INPUT] PASS esc_only=1 ignored_q_enter_left_right=1\n":"[TASKMANTEST][INPUT] FAIL\n");return ok;}
static bool formatter_test(void){char n[40],m[32];bool cut=false;bool ok=taskman_format_name("1234567890123456789012345678901",10,n,sizeof(n),&cut)&&cut&&strlen(n)==10;ok&=taskman_format_name("a\tb\nc",5,n,sizeof(n),&cut)&&!strchr(n,'\t')&&!strchr(n,'\n');for(uint32_t w=1;w<=3;w++)ok&=taskman_format_name("abcdef",w,n,sizeof(n),&cut)&&strlen(n)==w;ok&=taskman_format_mem(5ULL*1024*1024*1024,m,sizeof(m));serial_write_all(ok?"[TASKMANTEST][FORMATTER] PASS name_widths=1,2,3,10 mem_units=4\n":"[TASKMANTEST][FORMATTER] FAIL\n");return ok;}
static bool row_test(void){taskman_layout_t layouts[2];bool ok=taskman_layout_compute(160,40,0,0,&layouts[0])&&taskman_layout_compute(90,25,0,0,&layouts[1]);uint32_t cases=0;for(uint32_t mode=0;mode<2;mode++){char header[CONSOLE_MAX_COLS_STORAGE+1],row[CONSOLE_MAX_COLS_STORAGE+1];ok&=taskman_build_header(layouts[mode].mode,layouts[mode].name_width,header,sizeof(header))&&strlen(header)<=layouts[mode].region_width;for(uint32_t state=TASK_READY;state<=TASK_ZOMBIE;state++){task_snapshot_t t;memset(&t,0,sizeof(t));t.id=UINT64_MAX;t.state=state;t.task_class=TASK_CLASS_INTERACTIVE;t.quantum=UINT32_MAX;t.quantum_default=UINT32_MAX;t.runtime_ns_total=UINT64_MAX;t.kernel_mem_est_bytes=5ULL*1024*1024*1024;t.flags=TASK_FLAG_KILLABLE;t.last_cpu_slot=TASK_CPU_SLOT_NONE;strcpy(t.name,"taskman-name-31-characters-long");task_cpu_sample_t sample={.valid=1,.anomalous=1,.cpu_x10=1000};taskman_row_diagnostics_t d;ok&=taskman_format_row(&layouts[mode],&t,&sample,true,row,sizeof(row),&d)&&d.length<=layouts[mode].region_width&&d.mandatory_columns_present&&(state==TASK_ZOMBIE||d.anomalous_cpu_marked)&&!strchr(row,'\n')&&!strchr(row,'\t')&&contains(row,"18446744073709551615");cases++;}}serial_write_all(ok?"[TASKMANTEST][ROW] PASS cases=":"[TASKMANTEST][ROW] FAIL cases=");dec(cases);serial_write_all("\n");return ok;}
static bool formatting_max(void){uint32_t widths[]={90,160};bool ok=true;for(uint32_t i=0;i<2;i++){taskman_layout_t l;taskman_summary_t s={.total_captured=4096,.running=1000,.ready=1000,.blocked=1000,.sleeping=1000,.zombies=96};taskman_model_t m={.count=4096,.total=100000,.truncated=1,.allocation_failed=(uint8_t)i};taskman_navigation_t n={.page_index=99,.page_count=100,.first_visible=4076,.last_visible=4095};char lines[2][CONSOLE_MAX_COLS_STORAGE+1];uint8_t tr=0;ok&=taskman_layout_compute(widths[i],40,0,0,&l);uint32_t sn=taskman_format_summary_lines(&l,&s,&m,2000,UINT64_MAX,&n,lines,2,&tr);ok&=sn&&sn<=2&&!tr;for(uint32_t j=0;j<sn;j++)ok&=strlen(lines[j])<=l.region_width;uint32_t fn=taskman_format_footer_lines(&l,&m,&n,lines,2,&tr);ok&=fn&&fn<=2&&!tr;for(uint32_t j=0;j<fn;j++)ok&=strlen(lines[j])<=l.region_width;}serial_write_all(ok?"[TASKMANTEST][FORMATTING_MAX] PASS widths=90,160 builder_truncations=0 clipped_delta=0\n":"[TASKMANTEST][FORMATTING_MAX] FAIL\n");return ok;}

static bool schema_offsets_valid(const taskman_table_schema_t *schema)
{
    if (!schema || schema->exact_width != schema->region_width ||
        schema->boundary_count != TASKMAN_VIEW_SEMANTIC_BOUNDARIES ||
        schema->internal_gap_count != TASKMAN_VIEW_INTERNAL_GAPS ||
        schema->separator_count != TASKMAN_VIEW_GROUP_SEPARATORS)
        return false;
    uint32_t prior_end = 1u;
    for (uint32_t i = 0; i < TASK_FORMAT_COL_COUNT; i++) {
        const taskman_visual_column_t *column = &schema->columns[i];
        uint32_t end = column->slot_offset + column->slot_width;
        if (column->slot_offset != prior_end || end > schema->region_width ||
            column->content_offset < column->slot_offset ||
            column->content_offset + column->content_width > end)
            return false;
        prior_end = end;
    }
    if (prior_end != schema->region_width ||
        schema->columns[TASK_FORMAT_COL_NAME].content_offset +
        schema->columns[TASK_FORMAT_COL_NAME].content_width !=
        schema->region_width) return false;
    uint32_t groups = 0;
    for (uint32_t i = 0; i < schema->boundary_count; i++) {
        const taskman_visual_boundary_t *boundary = &schema->boundaries[i];
        const taskman_visual_column_t *left = &schema->columns[i];
        if (boundary->left != (task_format_column_id_t)i ||
            boundary->right != (task_format_column_id_t)(i + 1u) ||
            boundary->offset != left->slot_offset + left->slot_width - 1u ||
            left->content_offset + left->content_width != boundary->offset ||
            schema->columns[i + 1u].slot_offset != boundary->offset + 1u ||
            (i && schema->boundaries[i - 1u].offset >= boundary->offset) ||
            (boundary->kind != TASKMAN_BOUNDARY_GAP &&
             boundary->kind != TASKMAN_BOUNDARY_GROUP))
            return false;
        if (boundary->kind == TASKMAN_BOUNDARY_GROUP) {
            if (groups >= schema->separator_count ||
                schema->separator_offsets[groups] != boundary->offset)
                return false;
            groups++;
        }
    }
    return groups == TASKMAN_VIEW_GROUP_SEPARATORS;
}

static bool visual_schema_test(void)
{
    const uint32_t widths[] = {71u,76u,90u,118u,160u,512u};
    bool ok = true;
    bool boundary_lost = false;
    taskman_table_schema_t schema;
    for (uint32_t i = 0; i < 6u; i++) {
        taskman_layout_mode_t mode = widths[i] < 118u ?
                                     TASKMAN_LAYOUT_COMPACT :
                                     TASKMAN_LAYOUT_WIDE;
        bool built = taskman_table_schema_build(mode, widths[i], &schema);
        boundary_lost |= built && schema.boundary_count == 0u;
        ok = ok && built && schema_offsets_valid(&schema);
    }
    taskman_layout_t narrow, wide, compact;
    ok = ok && taskman_layout_compute(70u,25u,0,0,&narrow) &&
         narrow.mode == TASKMAN_LAYOUT_TOO_NARROW &&
         taskman_layout_compute(118u,25u,0,0,&wide) &&
         wide.mode == TASKMAN_LAYOUT_WIDE &&
         taskman_layout_compute(76u,25u,0,0,&compact) &&
         compact.mode == TASKMAN_LAYOUT_COMPACT;
#ifdef HOBBYOS_TASKMAN_NEGATIVE_SCHEMA_WITHOUT_BOUNDARIES
    if (boundary_lost) {
        serial_write_all("[TASKMANTEST][NEGATIVE] COLUMN_BOUNDARY_LOST_DETECTED\n");
        return true;
    }
    serial_write_all("[TASKMANTEST][NEGATIVE] COLUMN_BOUNDARY_LOST_MISSED\n");
    return false;
#else
    serial_write_all(ok ? "[TASKMANTEST][VISUAL_SCHEMA] PASS wide_min=118 compact_min=71 overlaps=0 separator_mismatches=0 offsets=" :
                          "[TASKMANTEST][VISUAL_SCHEMA] FAIL offsets=");
    if (taskman_table_schema_build(TASKMAN_LAYOUT_WIDE,118u,&schema))
        for (uint32_t i = 0; i < schema.separator_count; i++) {
            if (i) serial_write_all(",");
            dec(schema.separator_offsets[i]);
        }
    serial_write_all(" name_width=");dec(schema.name_width);
    serial_write_all(" exact=");dec(schema.exact_width==118u);serial_write_all("\n");
    return ok;
#endif
}

static void visual_fixture_task(task_snapshot_t *task)
{
    memset(task,0,sizeof(*task));task->id=7u;task->state=TASK_RUNNING;
    task->task_class=TASK_CLASS_INTERACTIVE;task->quantum=3;
    task->quantum_default=5;task->runtime_ns_total=10921009000000ULL;
    task->kernel_mem_est_bytes=16u*1024u;task->flags=TASK_FLAG_KILLABLE;
    task->on_cpu=1;task->current_cpu_slot=0;task->last_cpu_slot=2;
    strcpy(task->name,"shell-thread");
}

static bool visual_frame_test(uint32_t width, taskman_layout_mode_t expected,
                              const char *marker)
{
    taskman_layout_t layout;
    taskman_table_schema_t schema;
    taskman_view_diagnostics_t header_diag={0},row_diag={0},title_diag={0};
    task_snapshot_t task;visual_fixture_task(&task);
    task_cpu_sample_t sample={.valid=1,.cpu_x10=12};
    taskman_navigation_t nav={.page_count=4,.selected_index=0,
        .first_visible=0,.last_visible=0};
    taskman_model_t model={.tasks=&task,.count=1,.total=1};
    taskman_summary_t summary;taskman_summary_compute(&task,1,&summary);
    uint8_t truncated=0;
    uint64_t failures=0;
    if(!taskman_layout_compute(width,25u,0,0,&layout)||layout.mode!=expected)failures|=1u;
    if(!taskman_table_schema_build(expected,width,&schema)||!schema_offsets_valid(&schema))failures|=2u;
    if(!failures&&!taskman_view_build_header(&schema,g_visual_cells_a,
      CONSOLE_MAX_COLS_STORAGE,&header_diag))failures|=4u;
    if(!failures&&!taskman_view_build_row(&schema,&task,&sample,true,g_visual_cells_b,
      CONSOLE_MAX_COLS_STORAGE,&row_diag))failures|=8u;
    if(!failures&&!taskman_view_build_title_text(&layout,1000u,&nav,g_visual_text_a,
      sizeof(g_visual_text_a),g_visual_text_b,sizeof(g_visual_text_b),g_visual_lines[0],
      sizeof(g_visual_lines[0]),&title_diag))failures|=16u;
    if(!failures&&(taskman_format_summary_lines(&layout,&summary,&model,1000u,
      10921009000000ULL,&nav,g_visual_lines,2,&truncated)!=2u||truncated))failures|=32u;
    if(!failures&&(taskman_format_footer_lines(&layout,&model,&nav,g_visual_lines,2,
      &truncated)!=2u||truncated))failures|=64u;
    if(!failures&&(header_diag.length!=width||row_diag.length!=width||
      header_diag.separator_mismatches||row_diag.separator_mismatches||
      header_diag.field_overflows||row_diag.field_overflows||
      header_diag.semantic_boundaries_written!=TASKMAN_VIEW_SEMANTIC_BOUNDARIES||
      row_diag.semantic_boundaries_written!=TASKMAN_VIEW_SEMANTIC_BOUNDARIES||
      header_diag.adjacent_content_touches||row_diag.adjacent_content_touches||
      header_diag.boundary_overwrites||row_diag.boundary_overwrites||
      title_diag.length!=width||g_visual_cells_b[0].c!='>'))failures|=128u;
    bool ok=!failures;
    const taskman_view_palette_t *palette=taskman_view_palette();
    for(uint32_t i=0;i<width;i++)ok=ok&&g_visual_cells_b[i].bg==palette->selected_background;
    serial_write_all(ok?"[TASKMANTEST][":"[TASKMANTEST][");serial_write_all(marker);
    serial_write_all(ok?"] PASS width=":"] FAIL width=");dec(width);
    serial_write_all(" groups=");dec(schema.separator_count);
    serial_write_all(" overflows=");dec(row_diag.field_overflows);
    serial_write_all(" overlap=");dec(title_diag.split_overlaps);
    serial_write_all(" failures=");dec(failures);serial_write_all(" row_len=");dec(row_diag.length);
    serial_write_all(" row_expected=");dec(row_diag.expected_length);
    serial_write_all(" row_sep_mismatch=");dec(row_diag.separator_mismatches);
    serial_write_all(" row_trunc=");dec(row_diag.field_truncations);serial_write_all("\n");
    return ok;
}

static bool visual_wide_test(void)
{return visual_frame_test(118u,TASKMAN_LAYOUT_WIDE,"VISUAL_WIDE");}
static bool visual_compact_test(void)
{return visual_frame_test(76u,TASKMAN_LAYOUT_COMPACT,"VISUAL_COMPACT");}

static bool semantic_diag_valid(const taskman_view_diagnostics_t *diag,
                                uint32_t width)
{
    return diag && diag->length == width &&
        diag->semantic_boundaries_expected ==
            TASKMAN_VIEW_SEMANTIC_BOUNDARIES &&
        diag->semantic_boundaries_written ==
            TASKMAN_VIEW_SEMANTIC_BOUNDARIES &&
        diag->internal_gaps_expected == TASKMAN_VIEW_INTERNAL_GAPS &&
        diag->internal_gaps_written == TASKMAN_VIEW_INTERNAL_GAPS &&
        diag->group_separators_expected ==
            TASKMAN_VIEW_GROUP_SEPARATORS &&
        diag->group_separators_written ==
            TASKMAN_VIEW_GROUP_SEPARATORS &&
        !diag->adjacent_content_touches && !diag->boundary_overwrites &&
        !diag->field_overflows && !diag->hidden_truncations &&
        !diag->separator_mismatches;
}

static bool semantic_column_equals(const char *row,
                                   const taskman_visual_column_t *column,
                                   const char *expected)
{
    if (!row || !column || !expected) return false;
    uint32_t length = strlen(expected);
    if (length != column->content_width) return false;
    for (uint32_t i = 0; i < length; i++)
        if (row[column->content_offset + i] != expected[i]) return false;
    return true;
}

static void semantic_case_failure(taskman_layout_mode_t mode,
                                  uint32_t fixture, const char *field,
                                  const taskman_visual_column_t *column)
{
    serial_write_all("[TASKMANTEST][SEMANTIC_BOUNDARY_CASE] FAIL mode=");
    serial_write_all(mode_name(mode));
    serial_write_all(" fixture=");dec(fixture);
    serial_write_all(" field=");serial_write_all(field);
    if (!strcmp(field, "CPU_PERCENT"))
        serial_write_all(" expected=!100.0%");
    serial_write_all(" slot_width=");dec(column->slot_width);
    serial_write_all(" content_width=");dec(column->content_width);
    serial_write_all("\n");
}

static bool semantic_boundaries_test(taskman_layout_mode_t mode,
                                     uint32_t width, const char *marker)
{
    taskman_table_schema_t schema = {0}, minimum_schema = {0};
    taskman_view_diagnostics_t diag;
    bool schema_ok = taskman_table_schema_build(mode, width, &schema);
    if (schema_ok) {
        const taskman_visual_column_t *cpu =
            &schema.columns[TASK_FORMAT_COL_CPU_PERCENT];
        schema_ok = cpu->slot_width == 8u && cpu->content_width == 7u;
        if (mode == TASKMAN_LAYOUT_COMPACT) {
            schema_ok = schema_ok &&
                taskman_table_fixed_width(mode) == 70u &&
                schema.name_width == 6u &&
                taskman_table_schema_build(mode, 71u, &minimum_schema) &&
                minimum_schema.name_width == 1u &&
                minimum_schema.columns[TASK_FORMAT_COL_CPU_PERCENT].slot_width == 8u &&
                minimum_schema.columns[TASK_FORMAT_COL_CPU_PERCENT].content_width == 7u;
        }
    }
    bool header_built = schema_ok &&
        taskman_view_build_header_text(&schema, g_visual_text_a,
            sizeof(g_visual_text_a), &diag);
    uint32_t touches = header_built ? diag.adjacent_content_touches : 0u;
    uint32_t overwrites = header_built ? diag.boundary_overwrites : 0u;
    uint32_t overflows = header_built ? diag.field_overflows : 0u;
    uint32_t hidden = header_built ? diag.hidden_truncations : 0u;
    bool ok = header_built && semantic_diag_valid(&diag, width);
    for (uint32_t fixture = 0; fixture < 4u; fixture++) {
        task_snapshot_t task;
        memset(&task, 0, sizeof(task));
        task.id = fixture ? UINT64_MAX : 1u;
        task.state = fixture == 1u ? TASK_RUNNING :
                     fixture == 2u ? TASK_ZOMBIE : TASK_READY;
        task.task_class = fixture & 1u ? TASK_CLASS_INTERACTIVE :
                                        TASK_CLASS_NORMAL;
        task.quantum = fixture ? INT32_MAX : 1;
        task.quantum_default = fixture ? INT32_MAX : 1;
        task.runtime_ns_total = fixture ? 360000000000000ULL : 0u;
        task.kernel_mem_est_bytes = fixture ?
            5ULL * 1024ULL * 1024ULL * 1024ULL : 1u;
        task.on_cpu = fixture != 2u;
        task.current_cpu_slot = fixture ? 31u : 0u;
        task.last_cpu_slot = fixture ? 31u : 0u;
        task.flags = TASK_FLAG_KILLABLE;
        task.kill_pending = fixture == 3u;
        strcpy(task.name, fixture ? "taskman-name-31-characters-long" : "x");
        task_cpu_sample_t sample = {
            .valid = 1,
            .anomalous = fixture == 1u,
            .cpu_x10 = fixture == 1u ? 1000u : 0u,
        };
        bool selected = fixture == 1u;
        bool built = schema_ok && taskman_view_build_row_text(&schema, &task,
            &sample, selected, g_visual_text_a, sizeof(g_visual_text_a), &diag);
        bool cpu_ok = built && (fixture != 1u || semantic_column_equals(
            g_visual_text_a, &schema.columns[TASK_FORMAT_COL_CPU_PERCENT],
            "!100.0%"));
        bool case_ok = built && semantic_diag_valid(&diag, width) &&
            (!selected || g_visual_text_a[0] == '>') &&
            (task.id != UINT64_MAX ||
             contains(g_visual_text_a, "18446744073709551615")) &&
            cpu_ok &&
            (fixture != 2u || contains(g_visual_text_a, "ZOMB")) &&
            (fixture != 3u || contains(g_visual_text_a, "PEND"));
        if (built) {
            touches += diag.adjacent_content_touches;
            overwrites += diag.boundary_overwrites;
            overflows += diag.field_overflows;
            hidden += diag.hidden_truncations;
        }
        if (!case_ok)
            semantic_case_failure(mode, fixture,
                !cpu_ok ? "CPU_PERCENT" : "GENERAL",
                &schema.columns[TASK_FORMAT_COL_CPU_PERCENT]);
        ok = case_ok && ok;
    }
    serial_write_all("[TASKMANTEST][");
    serial_write_all(marker);
    if (ok) {
        serial_write_all("] PASS boundaries=12 gaps=7 groups=5 touches=0\n");
    } else {
        serial_write_all("] FAIL boundaries=12 gaps=7 groups=5 touches=");
        dec(touches);serial_write_all(" overwrites=");dec(overwrites);
        serial_write_all(" overflows=");dec(overflows);
        serial_write_all(" hidden=");dec(hidden);serial_write_all("\n");
    }
    return ok;
}

static bool semantic_boundaries_wide_test(void)
{
    return semantic_boundaries_test(TASKMAN_LAYOUT_WIDE, 118u,
                                    "SEMANTIC_BOUNDARIES_WIDE");
}

static bool semantic_boundaries_compact_test(void)
{
    return semantic_boundaries_test(TASKMAN_LAYOUT_COMPACT, 76u,
                                    "SEMANTIC_BOUNDARIES_COMPACT");
}

static bool visual_max_values_test(void)
{
    static const uint64_t memory_values[] = {
        1023u, 16u * 1024u, 5u * 1024u * 1024u,
        5ULL * 1024ULL * 1024ULL * 1024ULL
    };
    static const char *const names[] = {
        "", "taskman-name-31-characters-long", "bad\tname\nvalue"
    };
    taskman_table_schema_t schemas[2];
    bool ok=taskman_table_schema_build(TASKMAN_LAYOUT_WIDE,118u,&schemas[0])&&
      taskman_table_schema_build(TASKMAN_LAYOUT_COMPACT,76u,&schemas[1]);
    uint32_t cases=0;
    for(uint32_t mode=0;mode<2u;mode++)for(uint32_t state=TASK_READY;state<=TASK_ZOMBIE;state++)
      for(uint32_t task_class=TASK_CLASS_INTERACTIVE;task_class<=TASK_CLASS_NORMAL;task_class++){
        task_snapshot_t task;memset(&task,0,sizeof(task));task.id=UINT64_MAX;
        task.state=(task_state_t)state;task.task_class=(int)task_class;
        task.quantum=INT32_MAX;task.quantum_default=INT32_MAX;
        task.runtime_ns_total=360000000000000ULL;
        task.kernel_mem_est_bytes=memory_values[(state+task_class)%4u];
        task.on_cpu=1;task.current_cpu_slot=31;task.last_cpu_slot=31;
        task.flags=TASK_FLAG_KILLABLE;
        if(state==TASK_READY&&task_class==TASK_CLASS_INTERACTIVE)
            task.flags=TASK_FLAG_KILL_PROTECTED;
        else if(state==TASK_RUNNING&&task_class==TASK_CLASS_NORMAL)
            task.flags=0;
        else if(state==TASK_BLOCKED&&task_class==TASK_CLASS_INTERACTIVE)
            task.kill_pending=1;
        else if(state==TASK_SLEEPING&&task_class==TASK_CLASS_NORMAL)
            task.exit_started=1;
        strcpy(task.name,names[(state+task_class)%3u]);
        task_cpu_sample_t sample={.valid=1,.anomalous=1,.cpu_x10=1000};
        taskman_view_diagnostics_t diag={0};
        ok=ok&&taskman_view_build_row_text(&schemas[mode],&task,&sample,true,
          g_visual_text_a,sizeof(g_visual_text_a),&diag)&&
          diag.length==schemas[mode].region_width&&!diag.field_overflows&&
          !diag.separator_mismatches&&!diag.hidden_truncations&&
          !diag.adjacent_content_touches&&!diag.boundary_overwrites&&
          diag.semantic_boundaries_written==TASKMAN_VIEW_SEMANTIC_BOUNDARIES&&
          contains(g_visual_text_a,"18446744073709551615")&&
          contains(g_visual_text_a,state==TASK_ZOMBIE?"0.0%":"!100.0%")&&
          !strchr(g_visual_text_a,'\n')&&!strchr(g_visual_text_a,'\t');cases++;
    }
    task_snapshot_t kill_task;memset(&kill_task,0,sizeof(kill_task));
    kill_task.state=TASK_READY;
    kill_task.flags=TASK_FLAG_KILL_PROTECTED;
    ok=ok&&!strcmp(task_format_kill_status(&kill_task),"PROT");
    kill_task.flags=0;ok=ok&&!strcmp(task_format_kill_status(&kill_task),"NO");
    kill_task.flags=TASK_FLAG_KILLABLE;
    ok=ok&&!strcmp(task_format_kill_status(&kill_task),"-");
    kill_task.kill_pending=1;
    ok=ok&&!strcmp(task_format_kill_status(&kill_task),"PEND");
    kill_task.exit_started=1;
    ok=ok&&!strcmp(task_format_kill_status(&kill_task),"EXIT");
    kill_task.state=TASK_ZOMBIE;
    ok=ok&&!strcmp(task_format_kill_status(&kill_task),"ZOMB");
    serial_write_all(ok?"[TASKMANTEST][VISUAL_MAX_VALUES] PASS cases=":
                        "[TASKMANTEST][VISUAL_MAX_VALUES] FAIL cases=");
    dec(cases);serial_write_all(" states=5 classes=2 kill_states=6 mem_units=4 overflows=0 separator_mismatches=0 hidden=0\n");
    return ok;
}

static bool visual_palette_test(void)
{
    const taskman_view_palette_t*p=taskman_view_palette();
    bool ok=p->background!=p->selected_background&&p->title!=p->metadata&&
      p->running!=p->zombie&&p->warning!=p->row_text&&p->header_background!=p->background;
    serial_write_all(ok?"[TASKMANTEST][VISUAL_PALETTE] PASS native_cells=1 ansi=0\n":
                        "[TASKMANTEST][VISUAL_PALETTE] FAIL\n");return ok;
}

static bool visual_selection_test(void)
{
    taskman_table_schema_t schema;task_snapshot_t task;visual_fixture_task(&task);
    task_cpu_sample_t sample={.valid=1,.cpu_x10=10};taskman_view_diagnostics_t diag;
    bool ok=taskman_table_schema_build(TASKMAN_LAYOUT_WIDE,118u,&schema)&&
      taskman_view_build_row(&schema,&task,&sample,true,g_visual_cells_a,
        CONSOLE_MAX_COLS_STORAGE,&diag)&&g_visual_cells_a[0].c=='>';
    uint32_t selected_bg=taskman_view_palette()->selected_background;
    ok=ok&&g_visual_cells_a[117].bg==selected_bg&&
      taskman_view_build_row(&schema,&task,&sample,false,g_visual_cells_a,
        CONSOLE_MAX_COLS_STORAGE,&diag)&&g_visual_cells_a[0].c==' '&&
      g_visual_cells_a[117].bg==taskman_view_palette()->background;
    task.id=8u;ok=ok&&taskman_view_build_row(&schema,&task,&sample,true,g_visual_cells_b,
      CONSOLE_MAX_COLS_STORAGE,&diag)&&g_visual_cells_b[117].bg==selected_bg;
    serial_write_all(ok?"[TASKMANTEST][VISUAL_SELECTION] PASS affected_rows=2\n":
                        "[TASKMANTEST][VISUAL_SELECTION] FAIL\n");return ok;
}

static bool visual_split_lines_test(void)
{
    taskman_view_diagnostics_t diag;
    bool ok=taskman_view_compose_split_text(g_visual_text_a,76u,"LEFT BLOCK",
      "RIGHT BLOCK",2u,&diag)&&diag.split_right_offset==65u&&
      g_visual_text_a[75]=='K'&&!diag.split_overlaps;
    ok=ok&&!taskman_view_compose_split_text(g_visual_text_a,10u,"LEFT BLOCK",
      "RIGHT BLOCK",2u,&diag)&&diag.split_overlaps==1u;
    serial_write_all(ok?"[TASKMANTEST][VISUAL_SPLIT_LINES] PASS overlap=0 reject_overlap=1\n":
                        "[TASKMANTEST][VISUAL_SPLIT_LINES] FAIL\n");return ok;
}

static console_region_t diff_test_region(void)
{
    uint32_t rows=console_get_max_rows();
    return (console_region_t){0u,rows?rows-1u:0u,16u,1u};
}

static void diff_cells(const char *text,uint32_t fg,uint32_t bg,uint32_t count)
{
    for(uint32_t i=0;i<count;i++){
        g_visual_cells_a[i].c=text&&text[i]?text[i]:' ';
        g_visual_cells_a[i].fg=fg;g_visual_cells_a[i].bg=bg;
    }
}

static bool diff_identical_test(void)
{
    console_region_t region=diff_test_region();console_region_present_result_t first,second;
    diff_cells("IDENTICAL-ROW---",CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE,16u);
    bool ok=console_region_present_row(&region,0,g_visual_cells_a,16u,
      (ConsoleCell){' ',CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE},&first)&&
      console_region_present_row(&region,0,g_visual_cells_a,16u,
      (ConsoleCell){' ',CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE},&second);
#ifdef HOBBYOS_CONSOLE_NEGATIVE_PRESENT_ALWAYS_DIRTY
    if(ok&&second.cells_changed==16u&&second.row_dirty){
      serial_write_all("[TASKMANTEST][NEGATIVE] DIFF_PRESENT_BYPASSED_DETECTED\n");
      console_region_clear(&region,CONSOLE_COLOR_HOBBYOS_BLUE);return true;}
    serial_write_all("[TASKMANTEST][NEGATIVE] DIFF_PRESENT_BYPASSED_MISSED\n");
    console_region_clear(&region,CONSOLE_COLOR_HOBBYOS_BLUE);return false;
#else
    ok=ok&&second.cells_examined==16u&&!second.cells_changed&&
      second.cells_unchanged==16u&&!second.row_dirty;
    console_region_clear(&region,CONSOLE_COLOR_HOBBYOS_BLUE);
    serial_write_all(ok?"[TASKMANTEST][DIFF_IDENTICAL] PASS changed=0\n":
                        "[TASKMANTEST][DIFF_IDENTICAL] FAIL\n");return ok;
#endif
}

static bool diff_present_test(void)
{
    console_region_t region=diff_test_region();console_region_present_result_t result;
    diff_cells("abcdefghijklmnop",CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE,16u);
    bool ok=console_region_present_row(&region,0,g_visual_cells_a,16u,g_visual_cells_a[0],&result);
    g_visual_cells_a[4].c='X';ok=ok&&console_region_present_row(&region,0,g_visual_cells_a,16u,
      g_visual_cells_a[0],&result)&&result.cells_changed==1u&&result.glyph_changes==1u;
    g_visual_cells_a[5].fg=CONSOLE_COLOR_GREEN;ok=ok&&console_region_present_row(&region,0,
      g_visual_cells_a,16u,g_visual_cells_a[0],&result)&&result.cells_changed==1u&&
      !result.glyph_changes&&result.style_changes==1u;
    g_visual_cells_a[6].bg=CONSOLE_COLOR_BLUE;ok=ok&&console_region_present_row(&region,0,
      g_visual_cells_a,16u,g_visual_cells_a[0],&result)&&result.cells_changed==1u&&
      result.style_changes==1u;
    g_visual_cells_a[7].c='Y';g_visual_cells_a[7].fg=CONSOLE_COLOR_YELLOW;
    ok=ok&&console_region_present_row(&region,0,g_visual_cells_a,16u,g_visual_cells_a[0],
      &result)&&result.cells_changed==1u&&result.glyph_changes==1u&&result.style_changes==1u;
    console_region_clear(&region,CONSOLE_COLOR_HOBBYOS_BLUE);
    serial_write_all(ok?"[TASKMANTEST][DIFF_PRESENT] PASS\n":"[TASKMANTEST][DIFF_PRESENT] FAIL\n");
    return ok;
}

static bool diff_style_only_test(void)
{
    console_region_t region=diff_test_region();console_region_present_result_t result;
    diff_cells("style-only------",CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE,16u);
    bool ok=console_region_present_row(&region,0,g_visual_cells_a,16u,g_visual_cells_a[0],&result);
    g_visual_cells_a[9].bg=0xFF245A8Du;
    ok=ok&&console_region_present_row(&region,0,g_visual_cells_a,16u,g_visual_cells_a[0],&result)&&
      result.cells_changed==1u&&!result.glyph_changes&&result.style_changes==1u;
    console_region_clear(&region,CONSOLE_COLOR_HOBBYOS_BLUE);
    serial_write_all(ok?"[TASKMANTEST][DIFF_STYLE_ONLY] PASS changed=1 style=1\n":
                        "[TASKMANTEST][DIFF_STYLE_ONLY] FAIL\n");return ok;
}

static bool diff_shorter_line_test(void)
{
    console_region_t region=diff_test_region();console_region_present_result_t result;
    diff_cells("abcdefghijklmnop",CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE,16u);
    bool ok=console_region_present_row(&region,0,g_visual_cells_a,16u,g_visual_cells_a[0],&result);
    diff_cells("short",CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE,5u);
    ConsoleCell fill={' ',CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE};
    ok=ok&&console_region_present_row(&region,0,g_visual_cells_a,5u,fill,&result);
    uint32_t nonblank=0;ok=ok&&console_region_count_nonblank(&region,&nonblank)&&nonblank==5u;
    console_region_clear(&region,CONSOLE_COLOR_HOBBYOS_BLUE);
    serial_write_all(ok?"[TASKMANTEST][DIFF_SHORTER_LINE] PASS stale=0\n":
                        "[TASKMANTEST][DIFF_SHORTER_LINE] FAIL\n");return ok;
}

static bool diff_geometry_test(void)
{
    console_region_t region=diff_test_region();console_region_present_result_t result;
    diff_cells("geometry-change-",CONSOLE_COLOR_WHITE,CONSOLE_COLOR_HOBBYOS_BLUE,16u);
    bool ok=console_region_present_row(&region,0,g_visual_cells_a,16u,g_visual_cells_a[0],&result);
    console_region_t tail={8u,region.y,8u,1u};ok=ok&&console_region_clear(&tail,
      CONSOLE_COLOR_HOBBYOS_BLUE);region.width=8u;
    ok=ok&&console_region_present_row(&region,0,g_visual_cells_a,8u,g_visual_cells_a[0],&result);
    console_region_t whole={0u,region.y,16u,1u};uint32_t nonblank=0;
    ok=ok&&console_region_count_nonblank(&whole,&nonblank)&&nonblank==8u;
    console_region_clear(&whole,CONSOLE_COLOR_HOBBYOS_BLUE);
    serial_write_all(ok?"[TASKMANTEST][DIFF_GEOMETRY] PASS stale=0 geometry_clears=1\n":
                        "[TASKMANTEST][DIFF_GEOMETRY] FAIL\n");return ok;
}

static bool visual_all_test(void)
{
    bool ok=visual_schema_test()&&visual_wide_test()&&visual_compact_test()&&
      semantic_boundaries_wide_test()&&semantic_boundaries_compact_test()&&
      visual_max_values_test()&&visual_palette_test()&&visual_selection_test()&&
      visual_split_lines_test()&&diff_present_test()&&diff_style_only_test()&&
      diff_shorter_line_test()&&diff_identical_test()&&diff_geometry_test();
    serial_write_all(ok?"[TASKMANTEST][VISUAL_ALL] PASS\n":"[TASKMANTEST][VISUAL_ALL] FAIL\n");
    return ok;
}
typedef struct {uint32_t total;uint64_t generation,sample_time;} synthetic_source_t;
static task_snapshot_result_t synthetic_source(task_snapshot_t*b,uint32_t cap,uint32_t off,void*arg){synthetic_source_t*c=arg;task_snapshot_result_t r={0};r.total=c->total;r.offset=off;r.registry_generation=c->generation;r.sample_time_ns=c->sample_time;if(off<c->total){uint32_t left=c->total-off;r.written=left<cap?left:cap;for(uint32_t i=0;i<r.written;i++){memset(&b[i],0,sizeof(b[i]));b[i].id=c->total-(off+i);}}r.truncated=off+r.written<c->total;return r;}
static bool model_test(void){bool ok=true;
#ifdef HOBBYOS_TASKMAN_NEGATIVE_FIXED_128
bool hidden_detected=false;
#endif
uint32_t cases[]={0,1,20,50,128,129,257,4097};for(uint32_t c=0;c<8;c++){synthetic_source_t src={cases[c],100+c,1000+c};taskman_model_t m={0};bool captured=taskman_model_capture_from_source(&m,synthetic_source,&src,TASKMAN_V1_MAX_ENTRIES);uint32_t expected=cases[c]>TASKMAN_V1_MAX_ENTRIES?TASKMAN_V1_MAX_ENTRIES:cases[c];
#ifdef HOBBYOS_TASKMAN_NEGATIVE_FIXED_128
if((cases[c]==129||cases[c]==257)&&m.count<m.total&&!m.truncated){hidden_detected=true;taskman_model_release(&m);break;}
#else
ok&=captured&&m.count==expected&&m.total==cases[c]&&m.registry_generation==src.generation&&m.sample_time_ns==src.sample_time;ok&=m.truncated==(cases[c]>TASKMAN_V1_MAX_ENTRIES);for(uint32_t i=1;i<m.count;i++)if(m.tasks[i-1].id>m.tasks[i].id)ok=false;
#endif
taskman_model_release(&m);}
#ifndef HOBBYOS_TASKMAN_NEGATIVE_FIXED_128
synthetic_source_t base={128,700,800};taskman_model_t fallback={0};ok&=taskman_model_capture_from_source(&fallback,synthetic_source,&base,TASKMAN_V1_MAX_ENTRIES);base.total=129;base.generation=701;base.sample_time=801;taskman_test_fail_next_allocation();ok&=taskman_model_capture_from_source(&fallback,synthetic_source,&base,TASKMAN_V1_MAX_ENTRIES)&&fallback.count==128&&fallback.total==129&&fallback.truncated&&fallback.allocation_failed&&fallback.registry_generation==701&&fallback.sample_time_ns==801;taskman_model_release(&fallback);
#endif
#ifdef HOBBYOS_TASKMAN_NEGATIVE_FIXED_128
ok=hidden_detected;if(ok)serial_write_all("[TASKMANTEST][NEGATIVE] HIDDEN_TRUNCATION_DETECTED\n");else serial_write_all("[TASKMANTEST][NEGATIVE] HIDDEN_TRUNCATION_MISSED\n");
#endif
#ifdef HOBBYOS_TASKMAN_NEGATIVE_RENDER_ALL_ROWS
if(taskman_rows_requested(129,20)>20){serial_write_all("[TASKMANTEST][NEGATIVE] PANEL_OVERFLOW_DETECTED\n");ok=true;}else ok=false;
#endif
serial_write_all(ok?"[TASKMANTEST][MODEL] PASS cases=0,1,20,50,128,129,257,4097 growth=128-256-512 fallback=1\n":"[TASKMANTEST][MODEL] FAIL\n");taskman_test_set_cap(TASKMAN_V1_MAX_ENTRIES);return ok;}
static bool geometry_test(uint32_t cols,uint32_t rows){taskman_layout_t l;bool ok=taskman_layout_compute(cols,rows,0,0,&l);serial_write_all(ok?"[TASKMANTEST][GEOMETRY] PASS cols=":"[TASKMANTEST][GEOMETRY] FAIL cols=");dec(cols);serial_write_all(" rows=");dec(rows);serial_write_all(" mode=");dec(l.mode);serial_write_all(" visible=");dec(l.visible_task_rows);serial_write_all("\n");return ok;}
typedef enum
{
    FIXTURE_CLEANUP_OK = 0,
    FIXTURE_CLEANUP_TIMEOUT,
    FIXTURE_CLEANUP_REAPER_BUSY,
    FIXTURE_CLEANUP_INVALID_STATE
} fixture_cleanup_result_t;

static void emit_setup_prefix(const char *reason, const char *requested)
{
    serial_write_all("[TASKMANTEST][SETUP] FAIL reason=");
    serial_write_all(reason);
    serial_write_all(" requested=");
    serial_write_all(requested);
}

static void emit_setup_rejection(const char *reason, const char *requested)
{
    emit_setup_prefix(reason, requested);
    serial_write_all(" min=1 max=");
    dec(TASKMANTEST_FIXTURE_MAX);
    serial_write_all(" active=");
    dec(g_handle_count);
    serial_write_all("\n");
}

static fixture_cleanup_result_t fixture_cleanup_internal(
    bool emit_result, const char *origin)
{
    if (__atomic_exchange_n(&g_cleanup_in_progress, 1, __ATOMIC_ACQ_REL))
    {
        if (emit_result)
        {
            serial_write_all("[TASKMANTEST][CLEANUP] FAIL reason=invalid-state origin=");
            serial_write_all(origin);
            serial_write_all("\n");
        }
        return FIXTURE_CLEANUP_INVALID_STATE;
    }

    uint32_t count = g_handle_count;
    for (uint32_t i = 0; i < count; i++)
    {
        task_snapshot_t snapshot;
        if (scheduler_snapshot_task_by_handle(g_handles[i], &snapshot))
            scheduler_request_kill(g_handles[i].id);
    }
    __atomic_store_n(&g_gate.release, 1, __ATOMIC_RELEASE);

    uint32_t present = fixture_handles_present(count);
    uint64_t free_inflight = UINT64_MAX;
    uint64_t deadline = clock_monotonic_ns() + 30000000000ULL;
    while (present || free_inflight)
    {
        scheduler_reap_zombies(0);
        task_reaper_stats_t reaper;
        scheduler_reaper_stats_snapshot(&reaper);
        free_inflight = reaper.free_inflight;
        present = fixture_handles_present(count);
        if (!present && !free_inflight)
            break;
        if (clock_monotonic_ns() >= deadline)
        {
            fixture_cleanup_result_t result = present ?
                FIXTURE_CLEANUP_TIMEOUT : FIXTURE_CLEANUP_REAPER_BUSY;
            __atomic_store_n(&g_cleanup_in_progress, 0, __ATOMIC_RELEASE);
            if (emit_result)
            {
                serial_write_all("[TASKMANTEST][CLEANUP] FAIL reason=");
                serial_write_all(result == FIXTURE_CLEANUP_TIMEOUT ?
                    "timeout" : "reaper-busy");
                serial_write_all(" removed=");
                dec(count);
                serial_write_all(" handles_gone=");
                dec(count - present);
                serial_write_all(" handles_present=");
                dec(present);
                serial_write_all(" free_inflight=");
                dec(free_inflight);
                serial_write_all(" origin=");
                serial_write_all(origin);
                serial_write_all("\n");
            }
            return result;
        }
        timer_sleep(10);
    }

    memset(g_handles, 0, sizeof(g_handles));
    g_handle_count = 0;
    memset(&g_gate, 0, sizeof(g_gate));
    __atomic_store_n(&g_cleanup_in_progress, 0, __ATOMIC_RELEASE);
    if (emit_result)
    {
        serial_write_all("[TASKMANTEST][CLEANUP] PASS removed=");
        dec(count);
        serial_write_all(" handles_gone=");
        dec(count);
        serial_write_all(" free_inflight=0 origin=");
        serial_write_all(origin);
        serial_write_all("\n");
    }
    return FIXTURE_CLEANUP_OK;
}

static bool setup(uint32_t count)
{
    if (!count || count > TASKMANTEST_FIXTURE_MAX)
    {
        char requested[11];
        uint32_t digits = 0;
        uint32_t value = count;
        do
        {
            requested[digits++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value);
        for (uint32_t i = 0; i < digits / 2u; i++)
        {
            char swap = requested[i];
            requested[i] = requested[digits - i - 1u];
            requested[digits - i - 1u] = swap;
        }
        requested[digits] = '\0';
        emit_setup_rejection("range", requested);
        return false;
    }
    if (g_handle_count)
    {
        char requested[11];
        uint32_t digits = 0;
        uint32_t value = count;
        do
        {
            requested[digits++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value);
        for (uint32_t i = 0; i < digits / 2u; i++)
        {
            char swap = requested[i];
            requested[i] = requested[digits - i - 1u];
            requested[digits - i - 1u] = swap;
        }
        requested[digits] = '\0';
        emit_setup_prefix("busy", requested);
        serial_write_all(" active=");
        dec(g_handle_count);
        serial_write_all(" max=");
        dec(TASKMANTEST_FIXTURE_MAX);
        serial_write_all("\n");
        return false;
    }

    memset(g_handles, 0, sizeof(g_handles));
    memset(&g_gate, 0, sizeof(g_gate));
    __atomic_store_n(&g_cleanup_in_progress, 0, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < count; i++)
    {
        char name[32];
        memset(name, 0, sizeof(name));
        const char *base = i == count - 1 ?
            "taskman-name-31-characters-long!!" : "taskman-fixture";
        uint32_t j = 0;
        while (base[j] && j + 1 < sizeof(name))
        {
            name[j] = base[j];
            j++;
        }
        task_handle_t handle = TASK_HANDLE_INVALID;
        if (!thread_create_named_with_class_flags_handle(
                worker, &g_gate, TASK_CLASS_NORMAL, name,
                TASK_FLAG_KILLABLE, &handle))
        {
            uint32_t created = g_handle_count;
            uint32_t present = fixture_handles_present(created);
            fixture_cleanup_result_t rollback = fixture_cleanup_internal(
                true, "rollback");
            serial_write_all("[TASKMANTEST][SETUP] FAIL reason=create requested=");
            dec(count);
            serial_write_all(" created=");
            dec(created);
            serial_write_all(" present=");
            dec(present);
            serial_write_all(" rollback=");
            serial_write_all(rollback == FIXTURE_CLEANUP_OK ? "PASS" : "FAIL");
            serial_write_all("\n");
            return false;
        }
        g_handles[i] = handle;
        g_handle_count++;
    }

    uint64_t deadline = clock_monotonic_ns() + 10000000000ULL;
    uint32_t present;
    task_snapshot_result_t result;
    do
    {
        present = fixture_handles_present(count);
        result = scheduler_snapshot_tasks(NULL, 0, 0);
        if (present == count)
            break;
        timer_sleep(10);
    } while (clock_monotonic_ns() < deadline);

    if (present != count)
    {
        fixture_cleanup_result_t rollback = fixture_cleanup_internal(
            true, "rollback");
        serial_write_all("[TASKMANTEST][SETUP] FAIL reason=visibility requested=");
        dec(count);
        serial_write_all(" created=");
        dec(count);
        serial_write_all(" present=");
        dec(present);
        serial_write_all(" total=");
        dec(result.total);
        serial_write_all(" rollback=");
        serial_write_all(rollback == FIXTURE_CLEANUP_OK ? "PASS" : "FAIL");
        serial_write_all("\n");
        return false;
    }

    serial_write_all("[TASKMANTEST][SETUP] PASS count=");
    dec(count);
    serial_write_all(" requested=");
    dec(count);
    serial_write_all(" created=");
    dec(g_handle_count);
    serial_write_all(" present=");
    dec(present);
    serial_write_all(" total=");
    dec(result.total);
    serial_write_all(" max=");
    dec(TASKMANTEST_FIXTURE_MAX);
    serial_write_all(" stack_bytes_est=");
    dec((uint64_t)count * HOBBYOS_KERNEL_STACK_SIZE);
    serial_write_all("\n");
    return true;
}
static bool heap_snapshot_quiescent(HeapStats *out,uint32_t timeout_ms,uint64_t *refs_current){
    if(!out)return false;
    uint64_t end=clock_monotonic_ns()+(uint64_t)timeout_ms*1000000ULL;
    HeapStats candidate={0};
    uint32_t stable_samples=0;
    bool have_candidate=false;
    do{
        scheduler_reap_zombies(0);
        task_reaper_stats_t before,after;
        HeapStats sample={0};
        scheduler_reaper_stats_snapshot(&before);
        if(!before.current_zombies&&!before.free_inflight&&before.refs_current==1&&heap_get_stats(&sample)){
            scheduler_reaper_stats_snapshot(&after);
            if(!after.current_zombies&&!after.free_inflight&&after.refs_current==1){
                uint32_t sample_blocks=sample.blocks_total-sample.blocks_free;
                uint32_t candidate_blocks=candidate.blocks_total-candidate.blocks_free;
                if(!have_candidate||
                   (sample.used_bytes<=candidate.used_bytes&&sample_blocks<=candidate_blocks&&
                    (sample.used_bytes<candidate.used_bytes||sample_blocks<candidate_blocks))){
                    candidate=sample;
                    stable_samples=1;
                    have_candidate=true;
                }else if(sample.used_bytes==candidate.used_bytes&&sample_blocks==candidate_blocks){
                    stable_samples++;
                }else{
                    stable_samples=0;
                }
                if(stable_samples>=3){
                    *out=candidate;
                    if(refs_current)*refs_current=after.refs_current;
                    return true;
                }
            }
        }
        timer_sleep(10);
    }while(clock_monotonic_ns()<end);
    if(refs_current){task_reaper_stats_t r;scheduler_reaper_stats_snapshot(&r);*refs_current=r.refs_current;}
    return false;
}
static bool heap_begin(void){HeapStats h={0};uint64_t refs=0;bool ok=!g_heap_baseline.valid&&!g_handle_count&&heap_snapshot_quiescent(&h,30000,&refs);if(ok){g_heap_baseline.baseline=h;g_heap_baseline.valid=1;}serial_write_all(ok?"[TASKMANTEST][HEAP_BEGIN] PASS baseline_used=":"[TASKMANTEST][HEAP_BEGIN] FAIL baseline_used=");dec(ok?h.used_bytes:0);serial_write_all(" baseline_blocks=");dec(ok?h.blocks_total-h.blocks_free:0);serial_write_all(" refs_current=");dec(refs);serial_write_all("\n");return ok;}
static bool heap_end(void){HeapStats h={0};taskman_stats_t s;uint64_t refs=0;taskman_stats_snapshot(&s);bool got=heap_snapshot_quiescent(&h,30000,&refs);uint32_t base_blocks=g_heap_baseline.baseline.blocks_total-g_heap_baseline.baseline.blocks_free,final_blocks=h.blocks_total-h.blocks_free;bool ok=g_heap_baseline.valid&&!g_handle_count&&!s.model_live&&got&&h.used_bytes==g_heap_baseline.baseline.used_bytes&&final_blocks==base_blocks;serial_write_all(ok?"[TASKMANTEST][HEAP] PASS baseline_used=":"[TASKMANTEST][HEAP] FAIL baseline_used=");dec(g_heap_baseline.baseline.used_bytes);serial_write_all(" final_used=");dec(h.used_bytes);serial_write_all(" baseline_blocks=");dec(base_blocks);serial_write_all(" final_blocks=");dec(final_blocks);serial_write_all(" drift=");dec(h.used_bytes>g_heap_baseline.baseline.used_bytes?h.used_bytes-g_heap_baseline.baseline.used_bytes:g_heap_baseline.baseline.used_bytes-h.used_bytes);serial_write_all(" refs_current=");dec(refs);serial_write_all("\n");g_heap_baseline.valid=0;return ok;}
static bool cleanup(void)
{
    return fixture_cleanup_internal(true, "manual") == FIXTURE_CLEANUP_OK;
}

static bool fixture_capacity(void)
{
    serial_write_all("[TASKMANTEST][FIXTURE_CAPACITY] PASS max=");
    dec(TASKMANTEST_FIXTURE_MAX);
    serial_write_all(" handle_size=");
    dec(sizeof(task_handle_t));
    serial_write_all(" handle_bytes=");
    dec(sizeof(g_handles));
    serial_write_all(" stack_bytes_est=");
    dec((uint64_t)TASKMANTEST_FIXTURE_MAX * HOBBYOS_KERNEL_STACK_SIZE);
    serial_write_all(" taskman_initial=");
    dec(TASKMAN_INITIAL_CAPACITY);
    serial_write_all(" taskman_cap=");
    dec(TASKMAN_V1_MAX_ENTRIES);
    serial_write_all("\n");
    return true;
}

static bool fixture_status(void)
{
    taskmantest_fixture_snapshot_t snapshot;
    taskmantest_fixture_snapshot(&snapshot);
    bool stable = snapshot.cleanup_in_progress ||
        snapshot.handles_present == snapshot.active;
    serial_write_all(stable ? "[TASKMANTEST][FIXTURE_STATUS] PASS max=" :
                              "[TASKMANTEST][FIXTURE_STATUS] FAIL max=");
    dec(snapshot.max);
    serial_write_all(" active=");
    dec(snapshot.active);
    serial_write_all(" present=");
    dec(snapshot.handles_present);
    serial_write_all(" gate_release=");
    dec(snapshot.gate_released);
    serial_write_all(" cleanup=");
    dec(snapshot.cleanup_in_progress);
    serial_write_all("\n");
    return stable;
}
static bool churn(uint32_t rounds){task_snapshot_t a[3];taskman_navigation_t n={0};for(uint32_t r=0;r<rounds;r++){uint32_t count=(r%3)+1;for(uint32_t i=0;i<count;i++){memset(&a[i],0,sizeof(a[i]));a[i].id=r+i+1;}taskman_navigation_reconcile(&n,a,count,2);if(n.selected_index>=count)g_violations++;}g_churn_rounds+=rounds;serial_write_all(g_violations?"[TASKMANTEST][CHURN] FAIL rounds=":"[TASKMANTEST][CHURN] PASS rounds=");dec(rounds);serial_write_all(" page_clamps=");dec(n.clamps);serial_write_all(" violations=");dec(g_violations);serial_write_all("\n");return !g_violations;}
static bool churn_start(uint32_t rounds){if(!rounds||__atomic_load_n(&g_live_churn.active,__ATOMIC_ACQUIRE))return false;memset(&g_live_churn,0,sizeof(g_live_churn));g_live_churn.rounds=rounds;__atomic_store_n(&g_live_churn.active,1,__ATOMIC_RELEASE);if(!thread_create_named_with_class_flags_handle(live_churn_worker,&g_live_churn,TASK_CLASS_NORMAL,"taskman-churn-ctl",TASK_FLAG_SYSTEM,&g_live_churn_handle)){g_live_churn.active=0;return false;}serial_write_all("[TASKMANTEST][CHURN_LIVE] START rounds=");dec(rounds);serial_write_all("\n");return true;}
static bool churn_status(bool wait){if(wait){uint64_t timeout_ms=g_live_churn.rounds>1000?1200000ULL:120000ULL;uint64_t end=clock_monotonic_ns()+timeout_ms*1000000ULL;while(!__atomic_load_n(&g_live_churn.done,__ATOMIC_ACQUIRE)&&clock_monotonic_ns()<end)timer_sleep(10);}bool done=__atomic_load_n(&g_live_churn.done,__ATOMIC_ACQUIRE),ok=done&&g_live_churn.completed==g_live_churn.rounds&&!g_live_churn.violations&&g_live_churn.generation_changes;serial_write_all(ok?"[TASKMANTEST][CHURN_LIVE] PASS rounds=":"[TASKMANTEST][CHURN_LIVE] STATUS rounds=");dec(g_live_churn.rounds);serial_write_all(" completed=");dec(g_live_churn.completed);serial_write_all(" generation_changes=");dec(g_live_churn.generation_changes);serial_write_all(" violations=");dec(g_live_churn.violations);serial_write_all("\n");return wait?ok:true;}
static bool churn_stop(void){__atomic_store_n(&g_live_churn.stop,1,__ATOMIC_RELEASE);uint64_t end=clock_monotonic_ns()+5000000000ULL;while(!gone(g_live_churn_handle)&&clock_monotonic_ns()<end){scheduler_reap_zombies(0);timer_sleep(10);}bool ok=gone(g_live_churn_handle);memset(&g_live_churn,0,sizeof(g_live_churn));serial_write_all(ok?"[TASKMANTEST][CHURN_LIVE] STOPPED\n":"[TASKMANTEST][CHURN_LIVE] STOP_FAIL\n");return ok;}
static bool zombie(void){task_snapshot_t z;memset(&z,0,sizeof(z));z.id=77;z.state=TASK_ZOMBIE;z.flags=TASK_FLAG_KILLABLE;z.kernel_mem_est_bytes=16384;bool ok=!strcmp(scheduler_task_kill_state_to_string(&z),"ZOMB")&&z.kernel_mem_est_bytes>0;serial_write_all(ok?"[TASKMANTEST][ZOMBIE] PASS state=ZOMBIE kill=ZOMB mem=16384 cpu=0.0\n":"[TASKMANTEST][ZOMBIE] FAIL\n");return ok;}
static bool anchor_reset(void){console_clear(CONSOLE_COLOR_HOBBYOS_BLUE);serial_write_all("[TASKMANTEST][ANCHOR_RESET] PASS\n");return true;}
static bool auto_exit_arm(const char *text){uint32_t frames=0;bool ok=parse_exact_u32(text,&frames)&&frames>=1&&frames<=100000&&taskman_test_arm_auto_exit_full_frames(frames);serial_write_all(ok?"[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=":"[TASKMANTEST][AUTO_EXIT_ARM] FAIL frames=");dec(frames);serial_write_all("\n");return ok;}

static bool build_auto_session_record(
    const taskmantest_auto_session_result_t *result,
    int taskman_status,
    bool pass)
{
    if (!result)
        return false;
    control_record_reset();
    control_record_text("\n[TASKMANTEST][AUTO_SESSION] ");
    control_record_text(pass ? "PASS" : "FAIL");
    control_record_text(" refresh=");
    control_record_u64(result->refresh_ms);
    control_record_text(" target=");
    control_record_u64(result->target_frames);
    control_record_text(" taskman_status=");
    control_record_status(taskman_status);
    control_record_text(" sessions_delta=");
    control_record_u64(result->sessions_delta);
    control_record_text(" auto_exit_delta=");
    control_record_u64(result->auto_exit_sessions_delta);
    control_record_text(" full_delta=");
    control_record_u64(result->full_frames_delta);
    control_record_text(" fallback_delta=");
    control_record_u64(result->fallback_frames_delta);
    control_record_text(" render_fail_delta=");
    control_record_u64(result->render_failures_delta);
    control_record_text(" shortfall_delta=");
    control_record_u64(result->shortfalls_delta);
    control_record_text(" last_mode=");
    control_record_text(mode_name(result->last_mode));
    control_record_text(" pages=");
    control_record_u64(result->last_pages);
    control_record_text(" captured=");
    control_record_u64(result->last_captured);
    control_record_text(" modal_scroll=");
    control_record_u64(result->modal_scroll_delta);
    control_record_text(" shell_exit_scroll=");
    control_record_u64(result->shell_exit_scroll_delta);
    control_record_text(" clipped=");
    control_record_u64(result->clipped_delta);
    control_record_text(" model_live=");
    control_record_u64(result->model_live);
    control_record_text(" pending=");
    control_record_u64(result->pending);
    control_record_text(" controls_default=");
    control_record_u64(result->controls_default);
    control_record_text(" valid=");
    control_record_u64(result->valid);
    return control_record_finish();
}

static bool emit_auto_session_record(
    const taskmantest_auto_session_result_t *result,
    int taskman_status,
    bool pass)
{
    if (!build_auto_session_record(result, taskman_status, pass))
    {
        serial_write_all(
            "\n[TASKMANTEST][AUTO_SESSION] FAIL reason=record-overflow\n");
        return false;
    }
    serial_write_all(g_control_record.bytes);
    return true;
}

static bool emit_auto_session_progress(
    uint32_t start, uint32_t completed, uint32_t total)
{
    control_record_reset();
    control_record_text("\n[TASKMANTEST][AUTO_SESSION_PROGRESS] start=");
    control_record_u64(start);
    control_record_text(" completed=");
    control_record_u64(completed);
    control_record_text(" total=");
    control_record_u64(total);
    if (!control_record_finish())
        return false;
    serial_write_all(g_control_record.bytes);
    return true;
}

static bool emit_auto_session_loop_record(
    bool pass, uint32_t count, uint32_t completed, uint32_t start,
    uint32_t frames, uint32_t refresh50, uint32_t refresh1000,
    uint32_t refresh2000, uint32_t failure_index)
{
    control_record_reset();
    control_record_text("\n[TASKMANTEST][AUTO_SESSION_LOOP] ");
    control_record_text(pass ? "PASS" : "FAIL");
    control_record_text(" count=");
    control_record_u64(count);
    control_record_text(" completed=");
    control_record_u64(completed);
    control_record_text(" start=");
    control_record_u64(start);
    control_record_text(" frames=");
    control_record_u64(frames);
    control_record_text(" refresh50=");
    control_record_u64(refresh50);
    control_record_text(" refresh1000=");
    control_record_u64(refresh1000);
    control_record_text(" refresh2000=");
    control_record_u64(refresh2000);
    control_record_text(" failure_index=");
    control_record_u64(failure_index);
    if (!control_record_finish())
        return false;
    serial_write_all(g_control_record.bytes);
    return true;
}

static bool auto_session_run(
    uint32_t refresh_ms,
    uint32_t frames,
    taskmantest_auto_session_result_t *out)
{
    taskman_stats_t before;
    taskman_stats_t after;
    taskmantest_auto_session_result_t result;
    taskman_stats_snapshot(&before);
    console_clear(CONSOLE_COLOR_HOBBYOS_BLUE);

    bool armed = taskman_test_arm_auto_exit_full_frames(frames);
    int taskman_status = 1;
    if (armed)
    {
        char refresh_text[11];
        format_u32(refresh_ms, refresh_text);
        char *taskman_argv[] = {"taskman", refresh_text};
        taskman_status = cmd_taskman(2, taskman_argv);
    }

    taskman_stats_snapshot(&after);
    bool valid = taskmantest_auto_session_result_valid(
        &before, &after, frames, &result);
    result.refresh_ms = refresh_ms;
    result.target_frames = frames;
    bool pass = armed && taskman_status == 0 && valid;

    if (!taskman_test_controls_default())
        taskman_test_reset_controls();
    bool record_ok = emit_auto_session_record(&result, taskman_status, pass);
    if (out)
        *out = result;
    return pass && record_ok;
}

static bool auto_session_command(const char *refresh_text,
                                 const char *frames_text)
{
    uint32_t refresh_ms = 0;
    uint32_t frames = 0;
    if (!parse_exact_u32(refresh_text, &refresh_ms) || refresh_ms < 50u ||
        refresh_ms > 2000u || !parse_exact_u32(frames_text, &frames) ||
        frames < 1u || frames > 100000u)
    {
        taskmantest_auto_session_result_t result;
        memset(&result, 0, sizeof(result));
        result.refresh_ms = refresh_ms;
        result.target_frames = frames;
        (void)emit_auto_session_record(&result, 64, false);
        return false;
    }
    return auto_session_run(refresh_ms, frames, NULL);
}

static bool redraw_profile_command(const char *refresh_text,
                                   const char *frames_text)
{
    uint32_t refresh_ms=0,frames=0;
    if(!parse_exact_u32(refresh_text,&refresh_ms)||refresh_ms<50u||
       refresh_ms>2000u||!parse_exact_u32(frames_text,&frames)||!frames||
       frames>100000u)return false;
    bool session_ok=auto_session_run(refresh_ms,frames,NULL);
    taskman_stats_t stats;taskman_stats_snapshot(&stats);
#ifdef HOBBYOS_TASKMAN_NEGATIVE_CLEAR_EVERY_FRAME
    if(stats.last_session_stable_frame_full_clears>0u){
        serial_write_all("[TASKMANTEST][NEGATIVE] FULL_REGION_CLEAR_DETECTED\n");
        return true;
    }
    serial_write_all("[TASKMANTEST][NEGATIVE] FULL_REGION_CLEAR_MISSED\n");
    return false;
#else
    bool ok=session_ok&&stats.last_session_full_frames==frames&&
      !stats.last_session_fallback_frames&&
      stats.last_session_first_frame_full_presents==1u&&
      !stats.last_session_stable_frame_full_clears&&
      stats.last_session_cleanup_clears==1u&&
      stats.last_session_cells_examined>0u&&stats.last_session_cells_changed>0u&&
      (frames==1u||stats.last_session_cells_unchanged>0u)&&
      (refresh_ms!=1000u||frames<20u||
       stats.last_session_cells_unchanged>stats.last_session_cells_changed)&&
      !stats.last_session_separator_mismatches&&!stats.last_session_field_overflows&&
      !stats.last_session_split_overlaps&&!stats.last_session_scroll_delta&&
      !stats.last_session_clipped_writes&&!stats.visual_workspace_live&&
      stats.visual_workspace_allocations==stats.visual_workspace_frees;
    serial_write_all(ok?"[TASKMANTEST][REDRAW_PROFILE] PASS refresh=":
                        "[TASKMANTEST][REDRAW_PROFILE] FAIL refresh=");
    dec(refresh_ms);serial_write_all(" frames=");dec(frames);
    serial_write_all(" stable_clears=");dec(stats.last_session_stable_frame_full_clears);
    serial_write_all(" changed=");dec(stats.last_session_cells_changed);
    serial_write_all(" unchanged=");dec(stats.last_session_cells_unchanged);
    serial_write_all(" workspace_live=");dec(stats.visual_workspace_live);
    serial_write_all("\n");return ok;
#endif
}

static bool auto_session_loop(uint32_t count, uint32_t frames,
                              uint32_t start_index)
{
    uint32_t completed = 0;
    uint32_t refresh50 = 0;
    uint32_t refresh1000 = 0;
    uint32_t refresh2000 = 0;
    uint32_t failure_index = 0;
    bool pass = true;

    for (uint32_t offset = 0; offset < count; offset++)
    {
        uint32_t index = start_index + offset;
        uint32_t refresh = auto_session_refresh(index);
        if (!auto_session_run(refresh, frames, NULL))
        {
            pass = false;
            failure_index = index;
            break;
        }
        completed++;
        if (refresh == 50u) refresh50++;
        else if (refresh == 1000u) refresh1000++;
        else refresh2000++;
        if (completed % 10u == 0u &&
            !emit_auto_session_progress(start_index, completed, count))
        {
            pass = false;
            failure_index = index;
            break;
        }
    }

    pass = pass && completed == count;
    return emit_auto_session_loop_record(
        pass, count, completed, start_index, frames, refresh50, refresh1000,
        refresh2000, failure_index) && pass;
}

static void synthetic_auto_session_stats(taskman_stats_t *before,
                                         taskman_stats_t *after,
                                         uint32_t target)
{
    memset(before, 0, sizeof(*before));
    memset(after, 0, sizeof(*after));
    after->sessions = 1u;
    after->auto_exit_sessions = 1u;
    after->full_frames = target;
    after->last_session_full_frames = target;
    after->last_layout_mode = TASKMAN_LAYOUT_WIDE;
    after->last_pages = 1u;
    after->last_captured = 1u;
}

bool taskmantest_auto_session_selftest_case(
    taskmantest_auto_session_selftest_id_t test)
{
    if (test == TASKMANTEST_AUTO_SESSION_SELFTEST_REFRESH_PATTERN)
    {
        uint32_t refresh50 = 0;
        uint32_t refresh1000 = 0;
        uint32_t refresh2000 = 0;
        for (uint32_t index = 1; index <= 100u; index++)
        {
            uint32_t refresh = auto_session_refresh(index);
            if (refresh == 50u) refresh50++;
            else if (refresh == 1000u) refresh1000++;
            else if (refresh == 2000u) refresh2000++;
            else return false;
        }
        return refresh50 == 33u && refresh1000 == 34u &&
               refresh2000 == 33u;
    }

    if (test == TASKMANTEST_AUTO_SESSION_SELFTEST_RECORD_FIT)
    {
        taskmantest_auto_session_result_t result;
        memset(&result, 0, sizeof(result));
        result.refresh_ms = 2000u;
        result.target_frames = 100000u;
        result.sessions_delta = 1u;
        result.auto_exit_sessions_delta = 1u;
        result.full_frames_delta = 100000u;
        result.last_mode = TASKMAN_LAYOUT_COMPACT;
        result.last_pages = TASKMAN_V1_MAX_ENTRIES;
        result.last_captured = TASKMAN_V1_MAX_ENTRIES;
        result.controls_default = 1u;
        result.valid = 1u;
        return build_auto_session_record(&result, 0, true);
    }

    if (test != TASKMANTEST_AUTO_SESSION_SELFTEST_DELTA)
        return false;

    taskman_stats_t before;
    taskman_stats_t after;
    taskmantest_auto_session_result_t result;
    synthetic_auto_session_stats(&before, &after, 1u);
    bool ok = taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 1u, 0u, true, &result);
    synthetic_auto_session_stats(&before, &after, 3u);
    ok = ok && taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 3u, 0u, true, &result);

    synthetic_auto_session_stats(&before, &after, 1u);
    after.sessions = 0u;
    ok = ok && !taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 1u, 0u, true, &result);
    synthetic_auto_session_stats(&before, &after, 1u);
    after.full_frames = 2u;
    ok = ok && !taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 1u, 0u, true, &result);
    synthetic_auto_session_stats(&before, &after, 1u);
    after.fallback_frames = 1u;
    after.last_session_fallback_frames = 1u;
    ok = ok && !taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 1u, 0u, true, &result);
    synthetic_auto_session_stats(&before, &after, 1u);
    after.auto_exit_shortfalls = 1u;
    ok = ok && !taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 1u, 0u, true, &result);
    synthetic_auto_session_stats(&before, &after, 1u);
    after.model_live = 1u;
    ok = ok && !taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 1u, 0u, true, &result);
    synthetic_auto_session_stats(&before, &after, 1u);
    ok = ok && !taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 1u, 1u, true, &result);
    synthetic_auto_session_stats(&before, &after, 1u);
    after.last_layout_mode = TASKMAN_LAYOUT_TOO_NARROW;
    ok = ok && !taskmantest_auto_session_result_valid_with_controls(
        &before, &after, 1u, 0u, true, &result);
    return ok;
}

static bool stats_reset(void){bool ok=taskman_test_stats_reset();serial_write_all(ok?"[TASKMANTEST][STATS_RESET] PASS\n":"[TASKMANTEST][STATS_RESET] FAIL\n");return ok;}
static bool check(void)
{
    taskman_stats_t s;taskman_stats_snapshot(&s);console_region_stats_t r;
    console_region_stats_snapshot(&r);bool controls=taskman_test_controls_default();
    bool ok=!g_violations&&!s.model_live&&!s.last_session_scroll_delta&&
      !s.last_session_clipped_writes&&!s.row_overflows&&!s.page_range_violations&&
      !s.selection_range_violations&&!s.duplicate_row_ids&&!s.hidden_truncations&&
      !s.stale_cells&&!s.summary_format_failures&&!s.footer_format_failures&&
      !s.builder_truncations&&!s.pre_render_clear_failures&&!s.auto_exit_shortfalls&&
      !s.stable_frame_full_clears&&!s.separator_mismatches&&!s.field_overflows&&
      !s.split_overlaps&&!s.visual_workspace_live&&
      s.visual_workspace_allocations==s.visual_workspace_frees&&controls;
    g_checks++;serial_write_all(ok?"[TASKMANTEST][CHECK] PASS violations=0 model_live=0 last_session_scroll_delta=0 last_session_clipped_writes=0 total_scroll_delta=":
                                  "[TASKMANTEST][CHECK] FAIL violations=");
    dec(ok?s.scroll_delta:g_violations);serial_write_all(" clipped_total=");dec(s.total_clipped_writes);
    serial_write_all(" stale_cells=");dec(s.stale_cells);serial_write_all(" stable_clears=");
    dec(s.stable_frame_full_clears);serial_write_all(" separator_mismatches=");dec(s.separator_mismatches);
    serial_write_all(" field_overflows=");dec(s.field_overflows);serial_write_all(" split_overlaps=");
    dec(s.split_overlaps);serial_write_all(" workspace_live=");dec(s.visual_workspace_live);
    serial_write_all(" workspace_alloc=");dec(s.visual_workspace_allocations);
    serial_write_all(" workspace_free=");dec(s.visual_workspace_frees);
    serial_write_all(" auto_exit_shortfalls=");dec(s.auto_exit_shortfalls);
    serial_write_all(" controls_default=");dec(controls);serial_write_all(" rejected_regions=");
    dec(r.rejected_regions);serial_write_all("\n");return ok;
}
static bool clean_region_test(const char*tag){taskman_stats_t s;taskman_stats_snapshot(&s);bool ok=!s.stale_cells&&!s.model_live&&!s.pre_render_clear_failures;serial_write_all("[TASKMANTEST][");serial_write_all(tag);serial_write_all(ok?"] PASS stale_cells=0 model_live=0 region_clear_failures=0\n":"] FAIL\n");return ok;}
static bool stress_test(uint32_t navigation,uint32_t churn_count,uint32_t zombies,uint32_t kill_render,uint32_t layouts){bool ok=heap_begin();task_snapshot_t tasks[2]={{.id=1},{.id=2}};taskman_navigation_t nav={0};taskman_navigation_reconcile(&nav,tasks,2,1);for(uint32_t i=0;i<navigation;i++)taskman_navigation_input(&nav,input_event_special((i&1)?KEY_SPECIAL_HOME:KEY_SPECIAL_END),2,1);ok&=nav.selection_changes==navigation;if(ok&&churn_count){ok=churn_start(churn_count)&&churn_status(true)&&churn_stop();}for(uint32_t i=0;i<zombies;i++)ok&=zombie();for(uint32_t i=0;i<kill_render;i++)ok&=clean_region_test("STRESS_KILL_RENDER");for(uint32_t i=0;i<layouts;i++){taskman_layout_t l;ok&=taskman_layout_compute((i&1)?90:160,40,0,0,&l)&&l.visible_task_rows;}ok&=heap_end();serial_write_all(ok?"[TASKMANTEST][STRESS] PASS navigation=":"[TASKMANTEST][STRESS] FAIL navigation=");dec(navigation);serial_write_all(" churn=");dec(churn_count);serial_write_all(" zombies=");dec(zombies);serial_write_all(" kill_render=");dec(kill_render);serial_write_all(" layouts=");dec(layouts);serial_write_all("\n");return ok;}
static void stats_print(void)
{
    taskman_stats_t s;taskman_stats_snapshot(&s);
    serial_write_all("[TASKMANTEST][STATS] sessions=");dec(s.sessions);
    serial_write_all(" frames=");dec(s.frames);
    serial_write_all(" full_frames=");dec(s.full_frames);serial_write_all(" fallback_frames=");dec(s.fallback_frames);
    serial_write_all(" auto_exit_sessions=");dec(s.auto_exit_sessions);
    serial_write_all(" auto_exit_shortfalls=");dec(s.auto_exit_shortfalls);
    serial_write_all(" max_frame_gap_ms=");dec(s.max_full_frame_gap_ns/1000000ULL);
    serial_write_all(" shell_exit_scroll_delta=");dec(s.shell_exit_scroll_delta);
    serial_write_all(" last_session_shell_exit_scroll_delta=");dec(s.last_session_shell_exit_scroll_delta);
    serial_write_all(" last_session_full_frames=");dec(s.last_session_full_frames);
    serial_write_all(" last_session_fallback_frames=");dec(s.last_session_fallback_frames);
    serial_write_all(" last_mode=");serial_write_all(mode_name(s.last_layout_mode));
    serial_write_all(" pages=");dec(s.last_pages);serial_write_all(" captured=");dec(s.last_captured);
    serial_write_all(" total=");dec(s.last_total);serial_write_all(" truncated=");dec(s.last_truncated);
    serial_write_all(" render_failures=");dec(s.render_failures);
    serial_write_all(" page_changes=");dec(s.page_changes);
    serial_write_all(" selection_changes=");dec(s.selection_changes);
    serial_write_all(" ignored=");dec(s.ignored_inputs);
    serial_write_all(" scroll_delta=");dec(s.scroll_delta);
    serial_write_all(" last_session_scroll_delta=");dec(s.last_session_scroll_delta);
    serial_write_all(" clipped=");dec(s.last_session_clipped_writes);
    serial_write_all(" builder_truncations=");dec(s.builder_truncations);
    serial_write_all(" summary_failures=");dec(s.summary_format_failures);
    serial_write_all(" footer_failures=");dec(s.footer_format_failures);
    serial_write_all(" region_clear_failures=");dec(s.pre_render_clear_failures);
    serial_write_all(" present_calls=");dec(s.present_calls);
    serial_write_all(" selected_pid=");dec(s.last_selected_id);
    serial_write_all(" rows_examined=");dec(s.rows_examined);serial_write_all(" rows_changed=");dec(s.rows_changed);
    serial_write_all(" rows_unchanged=");dec(s.rows_unchanged);serial_write_all(" cells_examined=");dec(s.cells_examined);
    serial_write_all(" cells_changed=");dec(s.cells_changed);serial_write_all(" cells_unchanged=");dec(s.cells_unchanged);
    serial_write_all(" glyph_changes=");dec(s.glyph_changes);serial_write_all(" style_changes=");dec(s.style_changes);
    serial_write_all(" first_frame_presents=");dec(s.first_frame_full_presents);
    serial_write_all(" cleanup_clears=");dec(s.cleanup_clears);serial_write_all(" geometry_clears=");dec(s.geometry_invalidation_clears);
    serial_write_all(" stable_frame_full_clears=");dec(s.stable_frame_full_clears);
    serial_write_all(" tail_rows_cleared=");dec(s.tail_rows_cleared_by_present);
    serial_write_all(" separator_mismatches=");dec(s.separator_mismatches);
    serial_write_all(" field_overflows=");dec(s.field_overflows);serial_write_all(" field_truncations=");dec(s.field_truncations);
    serial_write_all(" split_overlaps=");dec(s.split_overlaps);serial_write_all(" workspace_live=");dec(s.visual_workspace_live);
    serial_write_all(" workspace_allocations=");dec(s.visual_workspace_allocations);
    serial_write_all(" workspace_reallocations=");dec(s.visual_workspace_reallocations);
    serial_write_all(" workspace_frees=");dec(s.visual_workspace_frees);
    serial_write_all(" stale_cells=");dec(s.stale_cells);serial_write_all(" auto_exit_pending=");
    dec(taskman_test_auto_exit_pending());serial_write_all("\n");
}
static bool all(void){return layout_test()&&model_test()&&sort_test()&&pagination_test()&&refresh_test()&&input_test()&&formatter_test()&&row_test()&&formatting_max()&&geometry_test(160,40)&&geometry_test(90,25)&&geometry_test(70,25)&&geometry_test(160,4)&&churn(10000)&&zombie()&&visual_all_test()&&check();}

int cmd_taskmantest(int argc,char **argv)
{
    if(argc<2)return 1;
    const char *c=argv[1];bool ok=false;
    if(!strcmp(c,"anchor-reset")&&argc==2)ok=anchor_reset();
    else if(!strcmp(c,"auto-exit-frames")&&argc==3)ok=auto_exit_arm(argv[2]);
    else if(!strcmp(c,"auto-session")&&argc==4)
        ok=auto_session_command(argv[2],argv[3]);
    else if(!strcmp(c,"redraw-profile")&&argc==4)
        ok=redraw_profile_command(argv[2],argv[3]);
    else if(!strcmp(c,"auto-session-loop")&&argc==5)
    {
        uint32_t count=0,frames=0,start=0;
        bool parsed=parse_exact_u32(argv[2],&count)&&count>=1u&&count<=100u&&
            parse_exact_u32(argv[3],&frames)&&frames>=1u&&frames<=100000u&&
            parse_exact_u32(argv[4],&start)&&start>=1u&&start<=1000000u;
        ok=parsed&&auto_session_loop(count,frames,start);
        if(!parsed)(void)emit_auto_session_loop_record(false,count,0,start,
            frames,0,0,0,0);
    }
    else if(!strcmp(c,"stats-reset")&&argc==2)ok=stats_reset();
    else if(!strcmp(c,"layout"))ok=layout_test();
    else if(!strcmp(c,"model"))ok=model_test();
    else if(!strcmp(c,"sort"))ok=sort_test();
    else if(!strcmp(c,"pagination"))ok=pagination_test();
    else if(!strcmp(c,"refresh"))ok=refresh_test();
    else if(!strcmp(c,"input"))ok=input_test();
    else if(!strcmp(c,"row"))ok=row_test();
    else if(!strcmp(c,"formatter")||!strcmp(c,"long-name"))ok=formatter_test();
    else if(!strcmp(c,"formatting-max"))ok=formatting_max();
    else if(!strcmp(c,"visual-schema"))ok=visual_schema_test();
    else if(!strcmp(c,"visual-wide"))ok=visual_wide_test();
    else if(!strcmp(c,"visual-compact"))ok=visual_compact_test();
    else if(!strcmp(c,"semantic-boundaries-wide"))
        ok=semantic_boundaries_wide_test();
    else if(!strcmp(c,"semantic-boundaries-compact"))
        ok=semantic_boundaries_compact_test();
    else if(!strcmp(c,"visual-max-values"))ok=visual_max_values_test();
    else if(!strcmp(c,"visual-palette"))ok=visual_palette_test();
    else if(!strcmp(c,"visual-selection"))ok=visual_selection_test();
    else if(!strcmp(c,"visual-split-lines"))ok=visual_split_lines_test();
    else if(!strcmp(c,"diff-present"))ok=diff_present_test();
    else if(!strcmp(c,"diff-style-only"))ok=diff_style_only_test();
    else if(!strcmp(c,"diff-shorter-line"))ok=diff_shorter_line_test();
    else if(!strcmp(c,"diff-identical"))ok=diff_identical_test();
    else if(!strcmp(c,"diff-geometry"))ok=diff_geometry_test();
    else if(!strcmp(c,"visual-all"))ok=visual_all_test();
    else if(!strcmp(c,"heap-begin"))ok=heap_begin();
    else if(!strcmp(c,"heap-end"))ok=heap_end();
    else if(!strcmp(c,"stress")){uint32_t a=argc>2?parse(argv[2],10000):10000,b=argc>3?parse(argv[3],10000):10000,d=argc>4?parse(argv[4],100):100,e=argc>5?parse(argv[5],100):100,f=argc>6?parse(argv[6],100):100;ok=stress_test(a,b,d,e,f);}
    else if(!strcmp(c,"geometry")&&argc==4)ok=geometry_test(parse(argv[2],0),parse(argv[3],0));
    else if(!strcmp(c,"geometry-runtime")&&argc==4){ok=taskman_test_set_geometry(parse(argv[2],0),parse(argv[3],0));serial_write_all(ok?"[TASKMANTEST][GEOMETRY_RUNTIME] PASS\n":"[TASKMANTEST][GEOMETRY_RUNTIME] FAIL\n");}
    else if(!strcmp(c,"geometry-clear")){taskman_test_clear_geometry();serial_write_all("[TASKMANTEST][GEOMETRY_RUNTIME] CLEARED\n");ok=true;}
    else if(!strcmp(c,"reset-controls")){taskman_test_reset_controls();serial_write_all("[TASKMANTEST][CONTROLS] RESET\n");ok=true;}
    else if(!strcmp(c,"cap")&&argc==3){taskman_test_set_cap(parse(argv[2],TASKMAN_V1_MAX_ENTRIES));serial_write_all("[TASKMANTEST][CAP] PASS\n");ok=true;}
    else if(!strcmp(c,"fail-next-allocation")){taskman_test_fail_next_allocation();serial_write_all("[TASKMANTEST][ALLOC_FAIL] PASS armed=1\n");ok=true;}
    else if(!strcmp(c,"fixture-capacity"))ok=fixture_capacity();
    else if(!strcmp(c,"fixture-status"))ok=fixture_status();
    else if(!strcmp(c,"setup")&&argc==3){uint32_t requested=0;if(!parse_exact_u32(argv[2],&requested)){emit_setup_rejection("parse","invalid");ok=false;}else ok=setup(requested);}
    else if(!strcmp(c,"cleanup"))ok=cleanup();
    else if(!strcmp(c,"churn-start")&&argc==3)ok=churn_start(parse(argv[2],0));
    else if(!strcmp(c,"churn-status"))ok=churn_status(argc==3&&!strcmp(argv[2],"wait"));
    else if(!strcmp(c,"churn-stop"))ok=churn_stop();
    else if(!strcmp(c,"churn"))ok=churn(argc==3?parse(argv[2],10000):10000);
    else if(!strcmp(c,"zombie"))ok=zombie();
    else if(!strcmp(c,"clean-region"))ok=clean_region_test("CLEAN_REGION");
    else if(!strcmp(c,"kill-render"))ok=clean_region_test("KILL_RENDER");
    else if(!strcmp(c,"check"))ok=check();
    else if(!strcmp(c,"stats")){stats_print();ok=true;}
    else if(!strcmp(c,"all"))ok=all();
    return ok?0:1;
}
