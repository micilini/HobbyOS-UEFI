#include "cmd_taskman.h"
#include "taskman_view.h"
#include "cmd_killtest.h"
#include "cmd_reaptest.h"
#include "../../core/modal_session.h"
#include "../../core/modal_ui.h"
#include "../../core/task_metrics.h"
#include "../../core/task_format.h"
#include "../../core/spinlock.h"
#include "../../core/clock.h"
#include "../../drivers/timer.h"
#include "../../drivers/serial.h"
#include "../../memory/heap.h"
#include "../../libc/memory.h"
#include "../../libc/string.h"
#include "../../smp/smp_topology.h"

typedef struct {
    ConsoleCell *row_cells;
    char *text_scratch;
    char *left_scratch;
    char *right_scratch;
    uint32_t row_capacity;
    uint32_t text_capacity;
    taskman_table_schema_t schema;
    uint64_t allocations;
    uint64_t reallocations;
} taskman_visual_workspace_t;

typedef struct {
    uint32_t refresh_ms,cursor_x,cursor_y; uint8_t cursor_valid;
    taskman_layout_t layout; console_region_t previous_region;
    uint8_t previous_valid; taskman_model_t model; taskman_navigation_t nav;
    uint32_t auto_exit_target_full_frames; uint8_t auto_exit_reached,cleanup_recorded;
    uint64_t scroll_before,clipped_before;
    uint64_t full_frames,fallback_frames;
    uint64_t first_full_frame_ns,last_full_frame_ns,max_full_frame_gap_ns;
    uint64_t modal_scroll_delta,shell_exit_scroll_delta;
    uint64_t inputs,ignored,render_failures;
    taskman_visual_workspace_t *visual;
    uint32_t previous_task_rows;
    uint64_t present_calls,rows_examined,rows_changed,rows_unchanged;
    uint64_t cells_examined,cells_changed,cells_unchanged;
    uint64_t glyph_changes,style_changes;
    uint64_t first_frame_full_presents,cleanup_clears,geometry_clears;
    uint64_t stable_frame_full_clears,tail_rows_cleared_by_present;
    uint64_t schema_builds,schema_failures,separator_mismatches;
    uint64_t field_overflows,field_truncations,split_overlaps;
} taskman_ui_context_t;

static task_cpu_sampler_t g_taskman_sampler;
static taskman_stats_t g_taskman_stats;
static spinlock_t g_taskman_stats_lock;
static volatile uint32_t g_test_cap=TASKMAN_V1_MAX_ENTRIES;
static volatile uint8_t g_fail_next_allocation;
static volatile uint32_t g_test_cols,g_test_rows;
static volatile uint32_t g_test_auto_exit_after_full_frames;
static uint32_t taskman_real_cols(void){return console_get_max_cols();}
static uint32_t taskman_real_rows(void){return console_get_max_rows();}
static uint32_t taskman_console_cols(void){uint32_t v=__atomic_load_n(&g_test_cols,__ATOMIC_ACQUIRE);return v?v:taskman_real_cols();}
static uint32_t taskman_console_rows(void){uint32_t v=__atomic_load_n(&g_test_rows,__ATOMIC_ACQUIRE);return v?v:taskman_real_rows();}
#define console_get_max_cols taskman_console_cols
#define console_get_max_rows taskman_console_rows

static uint64_t monotonic_delta(uint64_t end,uint64_t begin)
{return end>=begin?end-begin:0;}
static const char *layout_mode_name(taskman_layout_mode_t mode)
{return mode==TASKMAN_LAYOUT_WIDE?"WIDE":mode==TASKMAN_LAYOUT_COMPACT?"COMPACT":mode==TASKMAN_LAYOUT_TOO_NARROW?"TOO_NARROW":"TOO_SHORT";}
static void serial_dec(uint64_t value)
{char digits[21];uint32_t count=0;do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);while(count)serial_putc_all(digits[--count]);}
static void record_full_frame(taskman_ui_context_t *c)
{uint64_t now=clock_monotonic_ns();if(!c->full_frames)c->first_full_frame_ns=now;else{uint64_t gap=monotonic_delta(now,c->last_full_frame_ns);if(gap>c->max_full_frame_gap_ns)c->max_full_frame_gap_ns=gap;}c->last_full_frame_ns=now;c->full_frames++;}

bool taskman_build_header(taskman_layout_mode_t mode,uint32_t name_width,char*out,uint32_t size)
{
 taskman_table_schema_t schema;taskman_view_diagnostics_t diag;
 uint32_t width=taskman_table_fixed_width(mode)+name_width;
 return taskman_table_schema_build(mode,width,&schema)&&
  taskman_view_build_header_text(&schema,out,size,&diag);
}

bool taskman_layout_compute(uint32_t cols,uint32_t rows,uint32_t x,uint32_t y,taskman_layout_t *o)
{
    if(!o)return false;
    memset(o,0,sizeof(*o));o->console_cols=cols;o->console_rows=rows;o->anchor_x=x;o->anchor_y=y;
    o->region_x=x;o->region_y=y;o->title_rows=1;o->column_header_rows=1;
    uint32_t compact_min=taskman_table_minimum_width(TASKMAN_LAYOUT_COMPACT);
    uint32_t wide_min=taskman_table_minimum_width(TASKMAN_LAYOUT_WIDE);
    o->minimum_cols=compact_min;
    if(x>=cols||cols-x<compact_min){o->mode=TASKMAN_LAYOUT_TOO_NARROW;o->summary_rows=2;o->footer_rows=2;o->minimum_rows=o->title_rows+o->summary_rows+o->column_header_rows+o->footer_rows+1;o->region_width=x<cols?cols-x:0;o->region_height=y<rows?rows-y:0;return true;}
    o->mode=cols-x>=wide_min?TASKMAN_LAYOUT_WIDE:TASKMAN_LAYOUT_COMPACT;
    o->summary_rows=2;o->footer_rows=2;
    uint32_t fixed=o->title_rows+o->summary_rows+o->column_header_rows+o->footer_rows;
    o->minimum_rows=fixed+1;
    if(y>=rows||rows-y<o->minimum_rows){o->mode=TASKMAN_LAYOUT_TOO_SHORT;o->region_width=cols-x;o->region_height=y<rows?rows-y:0;return true;}
    o->region_width=cols-x;o->region_height=rows-y;o->visible_task_rows=o->region_height-fixed;
    taskman_table_schema_t schema;
    if(!taskman_table_schema_build(o->mode,o->region_width,&schema))return false;
    o->name_width=schema.name_width;
    return true;
}

bool taskman_format_name(const char *name,uint32_t width,char *out,uint32_t size,bool *tr)
{
    return task_format_name(name,width,out,size,tr);
}

bool taskman_format_mem(uint64_t bytes,char *out,uint32_t size)
{
    return task_format_memory(bytes,out,size);
}

void taskman_sort_by_pid(task_snapshot_t *a,uint32_t n){task_snapshot_sort_by_pid(a,n);}

void taskman_summary_compute(const task_snapshot_t *a,uint32_t n,taskman_summary_t *o)
{if(!o)return;memset(o,0,sizeof(*o));o->total_captured=n;for(uint32_t i=0;i<n;i++){switch(a[i].state){case TASK_RUNNING:o->running++;break;case TASK_READY:o->ready++;break;case TASK_BLOCKED:o->blocked++;break;case TASK_SLEEPING:o->sleeping++;break;case TASK_ZOMBIE:o->zombies++;break;default:break;}if(a[i].is_idle)o->idle++;if(a[i].kill_pending)o->kill_pending++;if(a[i].flags&TASK_FLAG_KILL_PROTECTED)o->protected_tasks++;else if(a[i].flags&TASK_FLAG_KILLABLE)o->killable++;}}

void taskman_navigation_reconcile(taskman_navigation_t *v,const task_snapshot_t *a,uint32_t n,uint32_t rows)
{if(!v)return;if(!rows)rows=1;v->page_count=n?(n+rows-1)/rows:1;uint32_t found=UINT32_MAX;if(v->selected_id)for(uint32_t i=0;i<n;i++)if(a[i].id==v->selected_id){found=i;break;}if(!n){v->page_index=0;v->selected_index=0;v->selected_id=0;}else{if(found!=UINT32_MAX)v->selected_index=found;
#ifndef HOBBYOS_TASKMAN_NEGATIVE_NO_PAGE_CLAMP
else if(v->selected_index>=n){v->selected_index=n-1;v->clamps++;}
if(v->page_index>=v->page_count){v->page_index=v->page_count-1;v->clamps++;}
#endif
if(v->selected_index<n){if(v->selected_index/rows!=v->page_index)v->page_index=v->selected_index/rows;v->selected_id=a[v->selected_index].id;}}v->first_visible=v->page_index*rows;v->last_visible=n?v->first_visible+rows-1:0;if(v->last_visible>=n&&n)v->last_visible=n-1;}

taskman_input_action_t taskman_navigation_input(taskman_navigation_t *v,input_event_t e,uint32_t n,uint32_t rows)
{if(!v)return TASKMAN_ACTION_NONE;if(e.type==INPUT_EVENT_CHAR){
#ifdef HOBBYOS_TASKMAN_NEGATIVE_EXIT_ANY_KEY
return TASKMAN_ACTION_EXIT;
#else
return e.value==27?TASKMAN_ACTION_EXIT:TASKMAN_ACTION_NONE;
#endif
}if(e.type!=INPUT_EVENT_SPECIAL||!n)return TASKMAN_ACTION_NONE;if(!rows)rows=1;uint32_t old=v->selected_index,oldp=v->page_index;switch(e.value){case KEY_SPECIAL_UP:if(v->selected_index)v->selected_index--;break;case KEY_SPECIAL_DOWN:if(v->selected_index+1<n)v->selected_index++;break;case KEY_SPECIAL_PAGE_UP:v->selected_index=v->selected_index>=rows?v->selected_index-rows:0;break;case KEY_SPECIAL_PAGE_DOWN:v->selected_index=(v->selected_index+rows<n)?v->selected_index+rows:n-1;break;case KEY_SPECIAL_HOME:v->selected_index=0;break;case KEY_SPECIAL_END:v->selected_index=n-1;break;default:
#ifdef HOBBYOS_TASKMAN_NEGATIVE_EXIT_ANY_KEY
return TASKMAN_ACTION_EXIT;
#else
return TASKMAN_ACTION_NONE;
#endif
}v->selected_id=0;v->page_index=v->selected_index/rows;if(old!=v->selected_index)v->selection_changes++;if(oldp!=v->page_index)v->page_changes++;return oldp!=v->page_index?TASKMAN_ACTION_PAGE:TASKMAN_ACTION_SELECTION;}

bool taskman_refresh_parse(const char *s,uint32_t *out){if(!s||!*s||!out)return false;uint64_t v=0;for(;*s;s++){if(*s<'0'||*s>'9')return false;v=v*10+(uint64_t)(*s-'0');if(v>UINT32_MAX)return false;}if(v<TASKMAN_MIN_REFRESH_MS||v>TASKMAN_MAX_REFRESH_MS)return false;*out=(uint32_t)v;return true;}

void taskman_model_release(taskman_model_t *m){if(!m)return;if(m->tasks)kfree(m->tasks);if(m->samples)kfree(m->samples);memset(m,0,sizeof(*m));}
static void *model_alloc(uint64_t bytes){if(__atomic_exchange_n(&g_fail_next_allocation,0,__ATOMIC_ACQ_REL))return NULL;if(bytes>SIZE_MAX)return NULL;return kmalloc((size_t)bytes);}
static bool model_grow(taskman_model_t *m,uint32_t need,uint32_t limit)
{uint32_t cap=m->capacity?m->capacity:TASKMAN_INITIAL_CAPACITY;
#ifdef HOBBYOS_TASKMAN_NEGATIVE_FIXED_128
limit=128;
#endif
if(!limit||limit>TASKMAN_V1_MAX_ENTRIES)limit=TASKMAN_V1_MAX_ENTRIES;
while(cap<need&&cap<limit){if(cap>limit/2){cap=limit;break;}cap*=2;}if(cap>limit)cap=limit;if(cap<=m->capacity)return need<=m->capacity;task_snapshot_t *t=model_alloc((uint64_t)cap*sizeof(*t));task_cpu_sample_t *s=t?model_alloc((uint64_t)cap*sizeof(*s)):NULL;if(!t||!s){if(t)kfree(t);if(s)kfree(s);m->allocation_failed=1;return false;}if(m->tasks&&m->count)memcpy(t,m->tasks,m->count*sizeof(*t));if(m->tasks)kfree(m->tasks);if(m->samples)kfree(m->samples);m->tasks=t;m->samples=s;m->capacity=cap;m->reallocations++;return true;}
uint32_t taskman_rows_requested(uint32_t count,uint32_t visible){
#ifdef HOBBYOS_TASKMAN_NEGATIVE_RENDER_ALL_ROWS
return count;
#else
return count<visible?count:visible;
#endif
}
bool taskman_model_capture_from_source(taskman_model_t*m,taskman_snapshot_source_fn source,
 void*ctx,uint32_t max_entries)
{if(!m||!source)return false;m->allocation_failed=0;uint32_t prior_cap=__atomic_load_n(&g_test_cap,__ATOMIC_ACQUIRE);uint32_t effective=max_entries&&max_entries<prior_cap?max_entries:prior_cap;for(uint32_t retry=0;retry<4;retry++){task_snapshot_result_t q=source(NULL,0,0,ctx);uint32_t want=q.total;if(want>effective)want=effective;if(!model_grow(m,want,effective)){m->total=q.total;m->truncated=1;if(!m->capacity)return false;task_snapshot_result_t f=source(m->tasks,m->capacity,0,ctx);m->count=f.written;m->total=f.total;m->registry_generation=f.registry_generation;m->sample_time_ns=f.sample_time_ns;break;}task_snapshot_result_t r=source(m->tasks,m->capacity,0,ctx);m->count=r.written;m->total=r.total;m->registry_generation=r.registry_generation;m->sample_time_ns=r.sample_time_ns;m->truncated=r.truncated||r.total>m->capacity;if(!m->truncated||m->capacity>=effective)break;m->snapshot_retries++;}taskman_sort_by_pid(m->tasks,m->count);m->sort_passes++;m->frames++;
#ifdef HOBBYOS_TASKMAN_NEGATIVE_FIXED_128
m->truncated=0;
#endif
return true;}
static task_snapshot_result_t scheduler_source(task_snapshot_t*b,uint32_t cap,uint32_t off,void*ctx){(void)ctx;return scheduler_snapshot_tasks(b,cap,off);}
static bool model_capture(taskman_model_t*m){return taskman_model_capture_from_source(m,scheduler_source,NULL,TASKMAN_V1_MAX_ENTRIES);}

bool taskman_format_row(const taskman_layout_t*l,const task_snapshot_t*t,
 const task_cpu_sample_t*s,bool selected,char*out,uint32_t cap,
 taskman_row_diagnostics_t*d)
{if(!l||!t||!out||!cap)return false;taskman_table_schema_t schema;
 taskman_view_diagnostics_t vd;if(!taskman_table_schema_build(l->mode,l->region_width,&schema)||
 !taskman_view_build_row_text(&schema,t,s,selected,out,cap,&vd))return false;
 if(d){memset(d,0,sizeof(*d));d->length=vd.length;d->field_truncations=vd.field_truncations;
 d->name_truncated=vd.name_truncated;d->anomalous_cpu_marked=s&&s->valid&&s->anomalous&&strchr(out,'!');
 d->mandatory_columns_present=vd.mandatory_columns_present;}return true;}

static bool region_equal(const console_region_t *a,const console_region_t *b)
{return a->x==b->x&&a->y==b->y&&a->width==b->width&&a->height==b->height;}

static void visual_workspace_release(taskman_ui_context_t *c)
{if(!c||!c->visual)return;taskman_visual_workspace_t*w=c->visual;
 if(w->row_cells)kfree(w->row_cells);
 if(w->text_scratch)kfree(w->text_scratch);
 if(w->left_scratch)kfree(w->left_scratch);
 if(w->right_scratch)kfree(w->right_scratch);
 kfree(w);c->visual=NULL;irq_flags_t f=spin_lock_irqsave(&g_taskman_stats_lock);
 g_taskman_stats.visual_workspace_frees++;
 if(g_taskman_stats.visual_workspace_live)g_taskman_stats.visual_workspace_live--;
 spin_unlock_irqrestore(&g_taskman_stats_lock,f);}

static bool visual_workspace_ensure(taskman_ui_context_t *c)
{if(c->visual)return true;taskman_visual_workspace_t*w=kmalloc(sizeof(*w));
 if(w)memset(w,0,sizeof(*w));
 uint32_t text_cap=CONSOLE_MAX_COLS_STORAGE+1u;
 if(w){w->row_cells=kmalloc(CONSOLE_MAX_COLS_STORAGE*sizeof(*w->row_cells));
  w->text_scratch=kmalloc(2u*text_cap);w->left_scratch=kmalloc(text_cap);
  w->right_scratch=kmalloc(text_cap);}
 if(!w||!w->row_cells||!w->text_scratch||!w->left_scratch||!w->right_scratch){
  if(w){if(w->row_cells)kfree(w->row_cells);
   if(w->text_scratch)kfree(w->text_scratch);
   if(w->left_scratch)kfree(w->left_scratch);
   if(w->right_scratch)kfree(w->right_scratch);
   kfree(w);}
  irq_flags_t f=spin_lock_irqsave(&g_taskman_stats_lock);g_taskman_stats.visual_workspace_failures++;
  spin_unlock_irqrestore(&g_taskman_stats_lock,f);return false;}
 w->row_capacity=CONSOLE_MAX_COLS_STORAGE;w->text_capacity=2u*text_cap;w->allocations=1;
 c->visual=w;irq_flags_t f=spin_lock_irqsave(&g_taskman_stats_lock);
 g_taskman_stats.visual_workspace_allocations++;g_taskman_stats.visual_workspace_live++;
 spin_unlock_irqrestore(&g_taskman_stats_lock,f);return true;}

static void cells_empty(taskman_visual_workspace_t*w,uint32_t width)
{const taskman_view_palette_t*p=taskman_view_palette();for(uint32_t i=0;i<width;i++){
 w->row_cells[i].c=' ';w->row_cells[i].fg=p->row_text;w->row_cells[i].bg=p->background;}}

static void text_line(char*out,uint32_t width,const char*text)
{memset(out,' ',width);uint32_t n=0;while(text&&text[n]&&n<width){char v=text[n];
 out[n]=(v=='\n'||v=='\r'||v=='\t')?' ':v;n++;}out[width]=0;}

static bool present_row(taskman_ui_context_t*c,const console_region_t*r,uint32_t row,
 bool tail)
{console_region_present_result_t result;if(!console_region_present_row(r,row,
 c->visual->row_cells,r->width,(ConsoleCell){' ',CONSOLE_COLOR_WHITE,
 CONSOLE_COLOR_HOBBYOS_BLUE},&result))return false;
 c->present_calls++;c->rows_examined++;
 if(result.row_dirty)c->rows_changed++;else c->rows_unchanged++;
 c->cells_examined+=result.cells_examined;c->cells_changed+=result.cells_changed;
 c->cells_unchanged+=result.cells_unchanged;c->glyph_changes+=result.glyph_changes;
 c->style_changes+=result.style_changes;
 if(tail&&result.cells_changed)c->tail_rows_cleared_by_present++;
 return true;}

static void account_view_diag(taskman_ui_context_t*c,
 const taskman_view_diagnostics_t*d)
{c->separator_mismatches+=d->separator_mismatches;c->field_overflows+=d->field_overflows;
 c->field_truncations+=d->field_truncations;c->split_overlaps+=d->split_overlaps;}

static bool render(taskman_ui_context_t *c)
{
    taskman_layout_t l;
    if (!taskman_layout_compute(console_get_max_cols(), console_get_max_rows(),
                                c->cursor_x, c->cursor_y, &l)) return false;
    console_region_t r={l.region_x,l.region_y,l.region_width,l.region_height};
    if(!r.width||!r.height||r.width>CONSOLE_MAX_COLS_STORAGE)return false;
    if(!visual_workspace_ensure(c))return false;
#ifdef HOBBYOS_TASKMAN_NEGATIVE_CLEAR_EVERY_FRAME
    if(c->previous_valid){if(!console_region_clear(&c->previous_region,
       CONSOLE_COLOR_HOBBYOS_BLUE))return false;c->stable_frame_full_clears++;}
#else
    bool geometry_changed=c->previous_valid&&!region_equal(&c->previous_region,&r);
    if(geometry_changed){
       if(!console_region_clear(&c->previous_region,CONSOLE_COLOR_HOBBYOS_BLUE))return false;
       c->geometry_clears++;
    }
#endif
    c->layout=l;c->previous_region=r;c->previous_valid=1;
    char*line=c->visual->text_scratch;
    taskman_view_diagnostics_t diag;
    if(l.mode==TASKMAN_LAYOUT_TOO_NARROW||l.mode==TASKMAN_LAYOUT_TOO_SHORT){
        text_line(line,r.width,"HobbyOS TASKMAN");
        if(!taskman_view_text_cells(line,r.width,TASKMAN_VIEW_TEXT_TITLE,r.width,
          c->visual->row_cells,c->visual->row_capacity)||!present_row(c,&r,0,false))return false;
        for(uint32_t row=1;row<r.height;row++){
            if(row==1)text_line(line,r.width,l.mode==TASKMAN_LAYOUT_TOO_NARROW?
              "Console too narrow: need cols >= 71 | ESC exits":
              "Console too short | ESC exits");else text_line(line,r.width,"");
            if(!taskman_view_text_cells(line,r.width,row==1?TASKMAN_VIEW_TEXT_FALLBACK:
              TASKMAN_VIEW_TEXT_EMPTY,r.width,c->visual->row_cells,c->visual->row_capacity)||
              !present_row(c,&r,row,false))return false;
        }
        c->previous_task_rows=0;c->fallback_frames++;return true;
    }
    c->schema_builds++;
    if(!taskman_table_schema_build(l.mode,r.width,&c->visual->schema)){
        c->schema_failures++;return false;
    }
    if (!model_capture(&c->model)) return false;
    memset(c->model.samples, 0, c->model.capacity * sizeof(*c->model.samples));
    if (!g_taskman_sampler.initialized ||
        c->model.sample_time_ns > g_taskman_sampler.sample_time_ns)
        task_cpu_sampler_sample(&g_taskman_sampler,c->model.tasks,c->model.count,
          c->model.sample_time_ns,c->model.samples,c->model.capacity);
    for(uint32_t i=0;i<c->model.count;i++)if(c->model.tasks[i].state==TASK_ZOMBIE)
        reaptest_zombie_mem_observe_taskman(c->model.tasks[i].id,
                                            c->model.tasks[i].kernel_mem_est_bytes);
    for(uint32_t i=0;i<c->model.count;i++)task_view_trace_emit("TASKMAN",
        &c->model.tasks[i],&c->model.samples[i]);
    taskman_navigation_reconcile(&c->nav,c->model.tasks,c->model.count,
                                 l.visible_task_rows);
    uint64_t page_bad=c->nav.page_index>=c->nav.page_count;
    uint64_t selection_bad=c->model.count&&c->nav.selected_index>=c->model.count;
    uint64_t duplicates=0;for(uint32_t i=1;i<c->model.count;i++)
        if(c->model.tasks[i-1].id==c->model.tasks[i].id)duplicates++;
    uint64_t hidden=c->model.count<c->model.total&&!c->model.truncated;
    if(page_bad||selection_bad||duplicates||hidden){irq_flags_t sf=spin_lock_irqsave(&g_taskman_stats_lock);
      g_taskman_stats.page_range_violations+=page_bad;g_taskman_stats.selection_range_violations+=selection_bad;
      g_taskman_stats.duplicate_row_ids+=duplicates;g_taskman_stats.hidden_truncations+=hidden;
      spin_unlock_irqrestore(&g_taskman_stats_lock,sf);}

    if(!taskman_view_build_title_text(&l,c->refresh_ms,&c->nav,line,
       CONSOLE_MAX_COLS_STORAGE+1u,c->visual->left_scratch,
       CONSOLE_MAX_COLS_STORAGE+1u,c->visual->right_scratch,
       CONSOLE_MAX_COLS_STORAGE+1u,&diag))return false;
    account_view_diag(c,&diag);
    if(!taskman_view_text_cells(line,r.width,TASKMAN_VIEW_TEXT_TITLE,
       diag.split_right_offset,c->visual->row_cells,c->visual->row_capacity)||
       !present_row(c,&r,0,false))return false;

    taskman_summary_t sum;taskman_summary_compute(c->model.tasks,c->model.count,&sum);
    char(*formatted)[CONSOLE_MAX_COLS_STORAGE+1u]=(void*)c->visual->text_scratch;
    uint8_t format_truncated=0;uint32_t summary_count=taskman_format_summary_lines(&l,
      &sum,&c->model,c->refresh_ms,c->model.sample_time_ns,&c->nav,formatted,2,&format_truncated);
    if(summary_count!=2u||format_truncated){irq_flags_t sf=spin_lock_irqsave(&g_taskman_stats_lock);
      g_taskman_stats.summary_format_failures++;g_taskman_stats.builder_truncations+=format_truncated;
      spin_unlock_irqrestore(&g_taskman_stats_lock,sf);return false;}
    for(uint32_t i=0;i<2u;i++){if(!taskman_view_text_cells(formatted[i],r.width,
      TASKMAN_VIEW_TEXT_SUMMARY,r.width,c->visual->row_cells,c->visual->row_capacity)||
      !present_row(c,&r,1u+i,false))return false;}

    uint32_t header_row=l.title_rows+l.summary_rows;
    if(!taskman_view_build_header(&c->visual->schema,c->visual->row_cells,
       c->visual->row_capacity,&diag))return false;
    account_view_diag(c,&diag);
    if(!present_row(c,&r,header_row,false))return false;
    uint32_t remaining=c->model.count>c->nav.first_visible?
                       c->model.count-c->nav.first_visible:0;
    uint32_t requested=taskman_rows_requested(remaining,l.visible_task_rows);
    if(requested>l.visible_task_rows)return false;
    for(uint32_t shown=0;shown<l.visible_task_rows;shown++){
        bool tail=shown>=requested&&shown<c->previous_task_rows;
        if(shown<requested){uint32_t i=c->nav.first_visible+shown;
          if(!taskman_view_build_row(&c->visual->schema,&c->model.tasks[i],
            &c->model.samples[i],i==c->nav.selected_index,c->visual->row_cells,
            c->visual->row_capacity,&diag)){irq_flags_t sf=spin_lock_irqsave(&g_taskman_stats_lock);
            g_taskman_stats.row_overflows++;spin_unlock_irqrestore(&g_taskman_stats_lock,sf);return false;}
          account_view_diag(c,&diag);
        }else cells_empty(c->visual,r.width);
        if(!present_row(c,&r,header_row+1u+shown,tail))return false;
    }
    c->previous_task_rows=requested;

    uint32_t footer_count=taskman_format_footer_lines(&l,&c->model,&c->nav,
      formatted,2,&format_truncated);
    if(footer_count!=2u||format_truncated){irq_flags_t sf=spin_lock_irqsave(&g_taskman_stats_lock);
      g_taskman_stats.footer_format_failures++;g_taskman_stats.builder_truncations+=format_truncated;
      spin_unlock_irqrestore(&g_taskman_stats_lock,sf);return false;}
    uint32_t footer_row=r.height-l.footer_rows;
    for(uint32_t i=0;i<2u;i++){if(!taskman_view_text_cells(formatted[i],r.width,
      TASKMAN_VIEW_TEXT_FOOTER,r.width,c->visual->row_cells,c->visual->row_capacity)||
      !present_row(c,&r,footer_row+i,false))return false;}
    if(!c->full_frames)c->first_frame_full_presents++;
    record_full_frame(c);return true;
}

static taskman_input_action_t process_input(taskman_ui_context_t *c,modal_session_token_t token)
{input_event_t e;taskman_input_action_t last=TASKMAN_ACTION_NONE;while(modal_session_try_pop(token,&e)==MODAL_INPUT_OK){c->inputs++;taskman_input_action_t a=taskman_navigation_input(&c->nav,e,c->model.count,c->layout.visible_task_rows);if(a==TASKMAN_ACTION_EXIT)return a;if(a==TASKMAN_ACTION_NONE)c->ignored++;else last=a;}return last;}
static int ui_entry(modal_session_token_t token,void *arg)
{taskman_ui_context_t *c=arg;if(!c)return 1;console_get_cursor(&c->cursor_x,&c->cursor_y);c->cursor_valid=1;c->scroll_before=console_scroll_count();console_region_stats_t before;console_region_stats_snapshot(&before);c->clipped_before=before.clipped_writes;task_cpu_sampler_reset(&g_taskman_sampler);for(;;){task_cancel_point();if(process_input(c,token)==TASKMAN_ACTION_EXIT)break;console_begin_batch();bool rendered=render(c);if(!rendered)c->render_failures++;console_end_batch();if(rendered&&c->auto_exit_target_full_frames&&c->full_frames>=c->auto_exit_target_full_frames){c->auto_exit_reached=1;break;}timer_sleep(c->refresh_ms);}return 0;}

static void auto_exit_sentinel(const taskman_ui_context_t *c,bool pass)
{serial_write_all(pass?"[TASKMAN][AUTO_EXIT] PASS target=":"[TASKMAN][AUTO_EXIT] FAIL target=");serial_dec(c->auto_exit_target_full_frames);serial_write_all(" full_frames=");serial_dec(c->full_frames);serial_write_all(" fallback_frames=");serial_dec(c->fallback_frames);serial_write_all(" mode=");serial_write_all(layout_mode_name(c->layout.mode));serial_write_all(" pages=");serial_dec(c->nav.page_count);serial_write_all(" captured=");serial_dec(c->model.count);serial_write_all(" render_failures=");serial_dec(c->render_failures);serial_write_all(" modal_scroll=");serial_dec(c->modal_scroll_delta);serial_write_all(" shell_exit_scroll=");serial_dec(c->shell_exit_scroll_delta);serial_write_all(" max_gap_ms=");serial_dec(c->max_full_frame_gap_ns/1000000ULL);serial_write_all("\n");}

static void ui_cleanup(void *arg,int result,bool completed)
{(void)result;(void)completed;taskman_ui_context_t*c=arg;if(!c)return;
 c->cleanup_recorded=1;uint32_t stale=0;if(c->previous_valid){
  if(console_region_clear(&c->previous_region,CONSOLE_COLOR_HOBBYOS_BLUE))
   c->cleanup_clears++;
  else c->render_failures++;
  console_region_count_nonblank(&c->previous_region,&stale);}
 uint64_t modal_scroll_end=console_scroll_count();
 c->modal_scroll_delta=monotonic_delta(modal_scroll_end,c->scroll_before);
 if(c->cursor_valid){console_set_cursor(c->cursor_x,c->cursor_y);console_write("TASKMAN exited.\n");}
 c->shell_exit_scroll_delta=monotonic_delta(console_scroll_count(),modal_scroll_end);
 console_region_stats_t rs;console_region_stats_snapshot(&rs);
 uint64_t session_clipped=monotonic_delta(rs.clipped_writes,c->clipped_before);
 bool armed=c->auto_exit_target_full_frames!=0;
 bool shortfall=armed&&(!c->auto_exit_reached||c->full_frames<c->auto_exit_target_full_frames);
 bool auto_pass=armed&&c->auto_exit_reached&&c->full_frames>=c->auto_exit_target_full_frames&&
  !c->fallback_frames&&!c->render_failures&&!c->modal_scroll_delta&&
  !c->stable_frame_full_clears&&!c->separator_mismatches&&!c->field_overflows&&
  !c->split_overlaps&&(c->layout.mode==TASKMAN_LAYOUT_WIDE||
  c->layout.mode==TASKMAN_LAYOUT_COMPACT)&&c->nav.page_count>=1&&c->model.count>0;
 irq_flags_t f=spin_lock_irqsave(&g_taskman_stats_lock);
#define ADD_STAT(field) g_taskman_stats.field+=c->field
 g_taskman_stats.sessions++;g_taskman_stats.frames+=c->full_frames;
 g_taskman_stats.full_frames+=c->full_frames;g_taskman_stats.fallback_frames+=c->fallback_frames;
 g_taskman_stats.auto_exit_sessions+=armed;g_taskman_stats.auto_exit_shortfalls+=shortfall;
 if(c->max_full_frame_gap_ns>g_taskman_stats.max_full_frame_gap_ns)
  g_taskman_stats.max_full_frame_gap_ns=c->max_full_frame_gap_ns;
 g_taskman_stats.inputs+=c->inputs;g_taskman_stats.ignored_inputs+=c->ignored;
 g_taskman_stats.render_failures+=c->render_failures;g_taskman_stats.page_changes+=c->nav.page_changes;
 g_taskman_stats.selection_changes+=c->nav.selection_changes;g_taskman_stats.clamps+=c->nav.clamps;
 g_taskman_stats.model_reallocations+=c->model.reallocations;
 g_taskman_stats.scroll_delta+=c->modal_scroll_delta;g_taskman_stats.last_session_scroll_delta=c->modal_scroll_delta;
 g_taskman_stats.shell_exit_scroll_delta+=c->shell_exit_scroll_delta;
 g_taskman_stats.last_session_shell_exit_scroll_delta=c->shell_exit_scroll_delta;
 g_taskman_stats.last_session_full_frames=c->full_frames;
 g_taskman_stats.last_session_fallback_frames=c->fallback_frames;
 g_taskman_stats.last_session_clipped_writes=session_clipped;
 g_taskman_stats.total_clipped_writes+=session_clipped;g_taskman_stats.stale_cells+=stale;
 g_taskman_stats.model_allocation_failures+=c->model.allocation_failed;
 g_taskman_stats.region_writes=rs.writes;g_taskman_stats.region_clears=rs.clears;
 ADD_STAT(present_calls);ADD_STAT(rows_examined);ADD_STAT(rows_changed);ADD_STAT(rows_unchanged);
 ADD_STAT(cells_examined);ADD_STAT(cells_changed);ADD_STAT(cells_unchanged);
 ADD_STAT(glyph_changes);ADD_STAT(style_changes);ADD_STAT(first_frame_full_presents);
 ADD_STAT(cleanup_clears);g_taskman_stats.geometry_invalidation_clears+=c->geometry_clears;
 ADD_STAT(stable_frame_full_clears);ADD_STAT(tail_rows_cleared_by_present);
 ADD_STAT(schema_builds);ADD_STAT(schema_failures);ADD_STAT(separator_mismatches);
 ADD_STAT(field_overflows);ADD_STAT(field_truncations);ADD_STAT(split_overlaps);
 g_taskman_stats.last_session_present_calls=c->present_calls;
 g_taskman_stats.last_session_rows_examined=c->rows_examined;
 g_taskman_stats.last_session_rows_changed=c->rows_changed;
 g_taskman_stats.last_session_rows_unchanged=c->rows_unchanged;
 g_taskman_stats.last_session_cells_examined=c->cells_examined;
 g_taskman_stats.last_session_cells_changed=c->cells_changed;
 g_taskman_stats.last_session_cells_unchanged=c->cells_unchanged;
 g_taskman_stats.last_session_glyph_changes=c->glyph_changes;
 g_taskman_stats.last_session_style_changes=c->style_changes;
 g_taskman_stats.last_session_first_frame_full_presents=c->first_frame_full_presents;
 g_taskman_stats.last_session_cleanup_clears=c->cleanup_clears;
 g_taskman_stats.last_session_geometry_clears=c->geometry_clears;
 g_taskman_stats.last_session_stable_frame_full_clears=c->stable_frame_full_clears;
 g_taskman_stats.last_session_tail_rows_cleared_by_present=c->tail_rows_cleared_by_present;
 g_taskman_stats.last_session_separator_mismatches=c->separator_mismatches;
 g_taskman_stats.last_session_field_overflows=c->field_overflows;
 g_taskman_stats.last_session_split_overlaps=c->split_overlaps;
 g_taskman_stats.last_captured=c->model.count;g_taskman_stats.last_total=c->model.total;
 g_taskman_stats.last_pages=c->nav.page_count;g_taskman_stats.last_visible_rows=c->layout.visible_task_rows;
 g_taskman_stats.last_selected_id=c->nav.selected_id;
 g_taskman_stats.last_layout_mode=c->layout.mode;g_taskman_stats.last_truncated=c->model.truncated;
#undef ADD_STAT
 spin_unlock_irqrestore(&g_taskman_stats_lock,f);
 if(armed)auto_exit_sentinel(c,auto_pass);
 taskman_model_release(&c->model);
 visual_workspace_release(c);f=spin_lock_irqsave(&g_taskman_stats_lock);
 g_taskman_stats.model_live=0;spin_unlock_irqrestore(&g_taskman_stats_lock,f);}

void taskman_stats_snapshot(taskman_stats_t *o){if(!o)return;irq_flags_t f=spin_lock_irqsave(&g_taskman_stats_lock);*o=g_taskman_stats;spin_unlock_irqrestore(&g_taskman_stats_lock,f);}
void taskman_test_set_cap(uint32_t cap){__atomic_store_n(&g_test_cap,cap?cap:TASKMAN_V1_MAX_ENTRIES,__ATOMIC_RELEASE);}
void taskman_test_fail_next_allocation(void){__atomic_store_n(&g_fail_next_allocation,1,__ATOMIC_RELEASE);}
bool taskman_test_set_geometry(uint32_t cols,uint32_t rows){uint32_t rc=taskman_real_cols(),rr=taskman_real_rows();if(!cols||!rows||cols>rc||rows>rr)return false;__atomic_store_n(&g_test_cols,cols,__ATOMIC_RELEASE);__atomic_store_n(&g_test_rows,rows,__ATOMIC_RELEASE);return true;}
void taskman_test_clear_geometry(void){__atomic_store_n(&g_test_cols,0,__ATOMIC_RELEASE);__atomic_store_n(&g_test_rows,0,__ATOMIC_RELEASE);}
void taskman_test_reset_controls(void){taskman_test_set_cap(TASKMAN_V1_MAX_ENTRIES);taskman_test_clear_geometry();__atomic_store_n(&g_fail_next_allocation,0,__ATOMIC_RELEASE);__atomic_store_n(&g_test_auto_exit_after_full_frames,0,__ATOMIC_RELEASE);}
uint32_t taskman_test_auto_exit_pending(void){return __atomic_load_n(&g_test_auto_exit_after_full_frames,__ATOMIC_ACQUIRE);}
bool taskman_test_controls_default(void){return __atomic_load_n(&g_test_cap,__ATOMIC_ACQUIRE)==TASKMAN_V1_MAX_ENTRIES&&!__atomic_load_n(&g_test_cols,__ATOMIC_ACQUIRE)&&!__atomic_load_n(&g_test_rows,__ATOMIC_ACQUIRE)&&!__atomic_load_n(&g_fail_next_allocation,__ATOMIC_ACQUIRE)&&!taskman_test_auto_exit_pending();}
bool taskman_test_arm_auto_exit_full_frames(uint32_t frames)
{if(!frames||frames>100000||modal_session_is_active())return false;irq_flags_t f=spin_lock_irqsave(&g_taskman_stats_lock);bool idle=!g_taskman_stats.model_live;spin_unlock_irqrestore(&g_taskman_stats_lock,f);if(!idle)return false;uint32_t expected=0;return __atomic_compare_exchange_n(&g_test_auto_exit_after_full_frames,&expected,frames,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
bool taskman_test_stats_reset(void)
{if(modal_session_is_active()||!taskman_test_controls_default())return false;irq_flags_t f=spin_lock_irqsave(&g_taskman_stats_lock);if(g_taskman_stats.model_live){spin_unlock_irqrestore(&g_taskman_stats_lock,f);return false;}memset(&g_taskman_stats,0,sizeof(g_taskman_stats));spin_unlock_irqrestore(&g_taskman_stats_lock,f);return true;}
int cmd_taskman(int argc,char **argv){uint32_t refresh=TASKMAN_DEFAULT_REFRESH_MS;if(argc>2){console_write("Usage: taskman [refresh_ms]\n");serial_write_all("Usage: taskman [refresh_ms]\n");return 1;}if(argc==2&&!taskman_refresh_parse(argv[1],&refresh)){console_write("taskman: refresh_ms must be 50..2000\n");serial_write_all("taskman: refresh_ms must be 50..2000\n");return 1;}taskman_ui_context_t c;memset(&c,0,sizeof(c));c.refresh_ms=refresh;c.auto_exit_target_full_frames=__atomic_exchange_n(&g_test_auto_exit_after_full_frames,0,__ATOMIC_ACQ_REL);irq_flags_t f=spin_lock_irqsave(&g_taskman_stats_lock);g_taskman_stats.model_live=1;spin_unlock_irqrestore(&g_taskman_stats_lock,f);modal_ui_run_result_t r;modal_ui_run_status_t s=modal_ui_run_sync("taskman-ui",ui_entry,ui_cleanup,&c,&r);if(s!=MODAL_UI_RUN_OK){taskman_model_release(&c.model);visual_workspace_release(&c);f=spin_lock_irqsave(&g_taskman_stats_lock);g_taskman_stats.model_live=0;if(c.auto_exit_target_full_frames&&!c.cleanup_recorded){g_taskman_stats.auto_exit_sessions++;g_taskman_stats.auto_exit_shortfalls++;}spin_unlock_irqrestore(&g_taskman_stats_lock,f);console_write("taskman: modal UI failed\n");return 1;}return 0;}
