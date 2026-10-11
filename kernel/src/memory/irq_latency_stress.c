#include "irq_latency_stress.h"
#include "heap.h"
#include "pmem.h"
#include "dma_buffer.h"
#include "mapping_probe.h"
#include "tlb_fixture.h"
#include "kernel_tlb.h"
#include "kernel_stack.h"
#include "guard_probe.h"
#include "../core/clock.h"
#include "../core/irq_latency.h"
#include "../core/idt.h"
#include "../core/scheduler.h"
#include "../drivers/timer.h"
#include "../smp/smp_topology.h"
#include "../libc/memory.h"

#define STRESS_WORKERS (2u * HOBBYOS_MAX_CPUS)
#define STRESS_OPERATIONS 1024u
#define STRESS_LIVE 64u
#define STRESS_FRAMES 32u
#define STRESS_DMA_BYTES 65536u
#define STRESS_MAP_CYCLES 192u
#define STRESS_GROWTH (33u * 1024u * 1024u)
#define STRESS_HEADROOM (128u * 1024u * 1024u)
#define STRESS_MAPPING_ATTEMPTS 1024u
#define STRESS_MAPPING_DEADLINE_NS UINT64_C(1000000000)
#define U64(v) ((unsigned long long)(v))

typedef struct {
    uint64_t begin, end, maximum_ns;
    uint32_t operation, count;
} api_elapsed_t;
typedef struct {
    task_handle_t handle;
    stack_handle_t stack;
    void *live[STRESS_LIVE];
    size_t bytes[STRESS_LIVE];
    uint64_t frames[STRESS_FRAMES];
    uint32_t touches[HOBBYOS_MAX_CPUS];
    api_elapsed_t api[3];
    uint64_t started, ended;
    uint64_t heap_allocated, heap_released, frames_allocated, frames_released;
    uint64_t dma_allocated, dma_released;
    uint32_t views_acquired, views_released;
    uint32_t acquire_busy, release_busy, acquire_status, release_status;
    uint32_t index, operations, failed, done, entered, overlap;
} stress_worker_t;
static stress_worker_t g_workers[STRESS_WORKERS];
static uint32_t g_stress_used, g_release, g_stop, g_active;
static const size_t g_sizes[] = {16, 64, 256, 1024, 4096};

typedef struct {
    task_handle_t handle;
    stack_handle_t stack;
    uint32_t cpu, done, failed;
} coverage_task_t;
typedef struct {
    coverage_task_t worker, child;
    task_handle_t created_handle;
    uint32_t target, before_cpu, after_cpu, child_retired;
} stack_coverage_t;
static stack_coverage_t g_stack_coverage[HOBBYOS_MAX_CPUS];

static bool coverage_identity(coverage_task_t *record, uint32_t target)
{
    if (!irq_is_enabled()) return false;
    scheduler_cpu_pin_t pin;
    if (!scheduler_cpu_pin(&pin)) return false;
    bool ok = pin.slot == target && pin.task && scheduler_cpu_pin_validate(&pin);
    if (ok) {
        record->cpu = pin.slot;
        record->handle = (task_handle_t){pin.task->id, pin.task->lifecycle_generation};
        record->stack = pin.task->stack_owner;
    }
    scheduler_cpu_unpin(&pin);
    return ok;
}

static bool coverage_retired(const coverage_task_t *record)
{
    if (!__atomic_load_n(&record->done, __ATOMIC_ACQUIRE)) return false;
    task_snapshot_t snapshot;
    stack_owner_view_t owner;
    return record->handle.id && record->stack.generation &&
        !scheduler_snapshot_task_by_handle(record->handle, &snapshot) &&
        kernel_stack_read(record->stack.slot, &owner) &&
        (owner.generation > record->stack.generation ||
         (owner.generation == record->stack.generation && owner.state == STACK_FREE &&
          owner.retirement_generation));
}

static void coverage_child(void *arg)
{
    stack_coverage_t *record = arg;
    record->child.failed = !coverage_identity(&record->child, record->target);
    __atomic_store_n(&record->child.done, 1, __ATOMIC_RELEASE);
}

static void coverage_worker(void *arg)
{
    stack_coverage_t *record = arg;
    bool ok = coverage_identity(&record->worker, record->target);
    record->before_cpu = record->worker.cpu;
    task_create_options_t options = {.name="irq-stack-child", .task_class=TASK_CLASS_NORMAL,
        .flags=TASK_FLAG_SYSTEM, .test_cpu_plus_one=record->target + 1};
    task_handle_t child = TASK_HANDLE_INVALID;
    uint64_t start = 0, now = 0;
    ok = ok && clock_monotonic_read_ns(&start) && start <= UINT64_MAX - UINT64_C(120000000000);
    bool created = ok && thread_create_ex_handle(coverage_child, record, &options, &child);
    coverage_task_t after = {0};
    ok = coverage_identity(&after, record->target) && ok && created;
    record->after_cpu = after.cpu;
    if (created) {
        do {
            if (coverage_retired(&record->child)) {
                record->child_retired = 1;
                break;
            }
            if (!clock_monotonic_read_ns(&now) || now < start || now - start >= UINT64_C(120000000000)) break;
            timer_sleep(1);
        } while (true);
    }
    ok = ok && record->child_retired && !record->child.failed &&
        record->child.handle.id == child.id &&
        record->child.handle.lifecycle_generation == child.lifecycle_generation;
    record->worker.failed = !ok;
    __atomic_store_n(&record->worker.done, 1, __ATOMIC_RELEASE);
}

static bool deterministic_stack_coverage(void)
{
    if (!irq_is_enabled()) return false;
    uint64_t start, now;
    if (!clock_monotonic_read_ns(&start)) return false;
    unsigned created = 0, retired = 0;
    for (; created < g_cpu_count; ++created) {
        stack_coverage_t *record = &g_stack_coverage[created];
        record->target = created;
        task_create_options_t options = {.name="irq-stack-owner", .task_class=TASK_CLASS_NORMAL,
            .flags=TASK_FLAG_SYSTEM, .test_cpu_plus_one=created + 1};
        if (!thread_create_ex_handle(coverage_worker, record, &options, &record->created_handle)) break;
    }
    do {
        retired = 0;
        for (unsigned cpu = 0; cpu < created; ++cpu)
            retired += coverage_retired(&g_stack_coverage[cpu].worker);
        if (retired == created) break;
        if (!clock_monotonic_read_ns(&now) || now < start || now - start >= UINT64_C(120000000000)) break;
        timer_sleep(1);
    } while (true);
    bool ok = created == g_cpu_count && retired == created;
    for (unsigned cpu = 0; cpu < created; ++cpu) {
        const stack_coverage_t *r = &g_stack_coverage[cpu];
        if (!__atomic_load_n(&r->worker.done, __ATOMIC_ACQUIRE)) { ok = false; continue; }
        bool complete = coverage_retired(&r->worker) && r->child_retired && !r->worker.failed &&
            r->worker.handle.id == r->created_handle.id &&
            r->worker.handle.lifecycle_generation == r->created_handle.lifecycle_generation;
        ok = complete && ok;
        if (!complete) {
            (void)stack_test_emit("[IRQ_LATENCY][STACK_COVERAGE_FAILURE] cpu=%u status=INCOMPLETE\n", cpu);
            continue;
        }
        ok = stack_test_emit("[IRQ_LATENCY][STACK_COVERAGE] version=1 cpu=%u before=%u after=%u child_cpu=%u worker=%llu worker_life=%llu worker_slot=%u worker_generation=%llu child=%llu child_life=%llu child_slot=%u child_generation=%llu child_retired=%u worker_retired=%u status=%s\n",
            cpu, r->before_cpu, r->after_cpu, r->child.cpu,
            U64(r->worker.handle.id), U64(r->worker.handle.lifecycle_generation),
            r->worker.stack.slot, U64(r->worker.stack.generation),
            U64(r->child.handle.id), U64(r->child.handle.lifecycle_generation),
            r->child.stack.slot, U64(r->child.stack.generation),
            r->child_retired, complete, complete ? "PASS" : "FAIL") && ok;
    }
    return ok;
}

static mapping_status_t acquire_view(const dma_buffer_t *buffer, mapping_view_t *view,
                                    uint32_t *busy)
{
    uint64_t deadline = clock_monotonic_ns() + STRESS_MAPPING_DEADLINE_NS;
    mapping_status_t status = MAPPING_BUSY;
    for (unsigned attempt = 0; attempt < STRESS_MAPPING_ATTEMPTS; ++attempt) {
        status = owned_mapping_stress_buffer(buffer, view);
        if (status != MAPPING_BUSY) return status;
        ++*busy;
        if (clock_monotonic_ns() >= deadline || __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) break;
        timer_sleep(1);
    }
    return status;
}

static mapping_status_t release_view(mapping_handle_t handle, uint32_t *busy)
{
    uint64_t deadline = clock_monotonic_ns() + STRESS_MAPPING_DEADLINE_NS;
    mapping_status_t status = MAPPING_BUSY;
    for (unsigned attempt = 0; attempt < STRESS_MAPPING_ATTEMPTS; ++attempt) {
        status = owned_mapping_release(handle);
        if (status != MAPPING_BUSY) return status;
        ++*busy;
        /* Cleanup keeps trying within its original bound even after stop. */
        if (clock_monotonic_ns() >= deadline) break;
        timer_sleep(1);
    }
    return status;
}

static void release_heap(stress_worker_t *worker, void **pointer)
{
    if (!*pointer) return;
    if (heap_test_free(*pointer)) { ++worker->heap_released; *pointer = NULL; }
    else worker->failed = 1;
}

static void elapsed(stress_worker_t *worker, unsigned kind, unsigned operation, uint64_t begin)
{
    uint64_t end = clock_monotonic_ns();
    api_elapsed_t *sample = &worker->api[kind];
    if (end < begin) { worker->failed = 1; return; }
    ++sample->count;
    if (end - begin > sample->maximum_ns)
        *sample = (api_elapsed_t){begin, end, end - begin, operation, sample->count};
}

/* A local payload access, not a creation/placement counter. No allocation runs
 * in this bounded identity-pinned region. Each worker owns its counters. */
static bool touch(stress_worker_t *worker, void *pointer, size_t bytes, uint8_t value)
{
    uint64_t switches, flags = irq_save();
    uint32_t cpu;
    bool ok = irq_latency_identity_sample(&cpu, &switches) && cpu < g_cpu_count && bytes;
    if (ok) {
        volatile uint8_t *data = pointer;
        data[0] = value; data[bytes-1] = value;
        ok = data[0] == value && data[bytes-1] == value;
        ++worker->touches[cpu];
    }
    irq_restore(flags);
    return ok;
}

static bool dma_cycle(stress_worker_t *worker, unsigned operation)
{
    dma_buffer_t buffer = {0};
    uint64_t begin = clock_monotonic_ns();
    if (dma_buffer_allocate(STRESS_DMA_BYTES, PAGE_SIZE, UINT32_MAX, &buffer) != MAPPING_OK) return false;
    worker->dma_allocated += buffer.backing_bytes;
    bool ok = buffer.cpu && !(buffer.device_address & (PAGE_SIZE-1)) &&
        buffer.backing_bytes == STRESS_DMA_BYTES && buffer.device_address <= UINT32_MAX - (STRESS_DMA_BYTES-1);
    const uint8_t *bytes = buffer.cpu;
    for (size_t i = 0; i < buffer.backing_bytes; ++i) if (bytes[i]) ok = false;
    ok = touch(worker, buffer.cpu, buffer.backing_bytes, (uint8_t)operation) && ok;
    mapping_view_t view;
    mapping_status_t acquired = acquire_view(&buffer, &view, &worker->acquire_busy);
    worker->acquire_status = acquired;
    if (acquired == MAPPING_QUARANTINED) { elapsed(worker, 2, operation, begin); return false; }
    bool mapped = acquired == MAPPING_OK;
    bool unmapped = !mapped;
    if (mapped) {
        ++worker->views_acquired;
        volatile uint64_t *alias = view.cpu;
        uint64_t marker = UINT64_C(0x4952514c00000000) | ((uint64_t)worker->index << 16) | operation;
        *alias = marker;
        ok = *(volatile uint64_t *)buffer.cpu == marker && ok;
        void *subview = NULL;
        uint64_t physical = 0;
        ok = owned_mapping_subview(view.handle, 0, 8, &subview) && subview == view.cpu &&
            owned_mapping_physical(view.cpu, 8, &physical) && physical == buffer.device_address && ok;
        worker->release_status = release_view(view.handle, &worker->release_busy);
        unmapped = worker->release_status == MAPPING_OK;
        if (unmapped) ++worker->views_released;
    }
    ok = mapped && unmapped && ok;
    if (!unmapped) { elapsed(worker, 2, operation, begin); return false; }
    /* This buffer is never published to a controller. The legal simulated
     * lifecycle exercises retirement without device activity or IRQ allocation. */
    bool in_flight = dma_buffer_state(buffer.handle, DMA_IN_FLIGHT);
    bool completed = in_flight && dma_buffer_state(buffer.handle, DMA_COMPLETED);
    ok = completed && ok;
    if (!in_flight || completed) {
        bool released = dma_buffer_release(buffer.handle) == MAPPING_OK;
        if (released) worker->dma_released += buffer.backing_bytes;
        ok = released && ok;
    }
    elapsed(worker, 2, operation, begin);
    return ok;
}

static void stress_worker(void *argument)
{
    stress_worker_t *worker = argument;
    scheduler_cpu_pin_t pin = {0};
    if (scheduler_cpu_pin(&pin)) {
        const task_t *self = get_current_task();
        if (self) worker->stack = self->stack_owner;
        scheduler_cpu_unpin(&pin);
    }
    if (!worker->stack.generation) worker->failed = 1;
    __atomic_store_n(&worker->entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) schedule_voluntary();
    worker->started = clock_monotonic_ns();
    __atomic_add_fetch(&g_active, 1, __ATOMIC_ACQ_REL);
    uint32_t random = UINT32_C(1230131532) ^ worker->index;
    for (unsigned operation = 0; operation < STRESS_OPERATIONS &&
         !__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE); ++operation) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        unsigned slot = operation % STRESS_LIVE;
        if (__atomic_load_n(&g_active, __ATOMIC_ACQUIRE) > 1) ++worker->overlap;
        uint64_t begin = clock_monotonic_ns();
        if (worker->live[slot]) {
            release_heap(worker, &worker->live[slot]);
            if (worker->live[slot]) break;
        }
        size_t size = g_sizes[random % (sizeof(g_sizes)/sizeof(g_sizes[0]))];
        size_t alignment = operation & 1 ? 64 : PAGE_SIZE;
        void *pointer = operation % 3 ? kmalloc(size) : kmalloc_aligned(size, alignment);
        if (pointer) ++worker->heap_allocated;
        elapsed(worker, 0, operation, begin);
        if (!pointer || (!(operation % 3) && ((uintptr_t)pointer & (alignment-1)))) {
            if (pointer) {
                worker->live[slot] = pointer;
                release_heap(worker, &worker->live[slot]);
            }
            worker->failed = 1; break;
        }
        worker->live[slot] = pointer; worker->bytes[slot] = size;
        if (!touch(worker, pointer, size, (uint8_t)random)) { worker->failed = 1; break; }
        unsigned frame = operation % STRESS_FRAMES;
        begin = clock_monotonic_ns();
        if (worker->frames[frame]) {
            pmm_free_frame((void *)(uintptr_t)worker->frames[frame]); ++worker->frames_released;
        }
        worker->frames[frame] = (uint64_t)(uintptr_t)pmm_alloc_frame();
        if (worker->frames[frame]) ++worker->frames_allocated;
        elapsed(worker, 1, operation, begin);
        if (!worker->frames[frame]) { worker->failed = 1; break; }
        if (!(operation % 64) && !dma_cycle(worker, operation)) { worker->failed = 1; break; }
        ++worker->operations;
        if (!(operation % 128)) {
            PmmReachability reach;
            if (!pmm_validate_integrity() || !pmm_get_reachability(&reach) || !heap_validate_integrity()) {
                worker->failed = 1; break;
            }
        }
        if (!(operation % 16)) schedule_voluntary();
    }
    for (unsigned i = 0; i < STRESS_LIVE; ++i) {
        release_heap(worker, &worker->live[i]);
    }
    for (unsigned i = 0; i < STRESS_FRAMES; ++i) {
        if (worker->frames[i]) {
            pmm_free_frame((void *)(uintptr_t)worker->frames[i]); ++worker->frames_released;
        }
        worker->frames[i] = 0;
    }
    __atomic_sub_fetch(&g_active, 1, __ATOMIC_ACQ_REL);
    worker->ended = clock_monotonic_ns();
    __atomic_store_n(&worker->done, 1, __ATOMIC_RELEASE);
}

static bool fragmented_frames(void)
{
    uint64_t first = (uint64_t)(uintptr_t)pmm_alloc_contiguous_frames(STRESS_FRAMES);
    if (!first) return false;
    for (unsigned i = 1; i < STRESS_FRAMES; i += 2) pmm_free_frame((void *)(uintptr_t)(first + i*PAGE_SIZE));
    uint64_t refused = UINT64_MAX;
    bool allocated = pmm_test_alloc_range(2, 1, first, first + STRESS_FRAMES*PAGE_SIZE, &refused);
    bool ok = !allocated && refused == UINT64_MAX;
    if (allocated) for (unsigned i = 0; i < 2; ++i) pmm_free_frame((void *)(uintptr_t)(refused + i*PAGE_SIZE));
    for (unsigned i = 0; i < STRESS_FRAMES; i += 2) pmm_free_frame((void *)(uintptr_t)(first + i*PAGE_SIZE));
    return stack_test_emit("[IRQ_LATENCY][FRAGMENTATION] first=%llu frames=32 isolated_free=16 request=2 allocated=%u output=%llu status=%s\n",
                           U64(first), allocated, U64(refused), ok ? "PASS" : "FAIL") && ok;
}

static bool mapping_cycles(void)
{
    dma_buffer_t buffer;
    if (dma_buffer_allocate(STRESS_DMA_BYTES, PAGE_SIZE, UINT32_MAX, &buffer) != MAPPING_OK) return false;
    unsigned completed = 0;
    uint32_t acquire_busy = 0, release_busy = 0;
    mapping_status_t acquired = MAPPING_OK, released_view = MAPPING_OK;
    bool safe = true;
    for (; completed < STRESS_MAP_CYCLES; ++completed) {
        mapping_view_t view;
        acquired = acquire_view(&buffer, &view, &acquire_busy);
        if (acquired != MAPPING_OK) { safe = acquired != MAPPING_QUARANTINED; break; }
        volatile uint64_t *alias = view.cpu;
        uint64_t value = UINT64_C(0x4952514c00000000) | completed;
        *alias = value;
        bool same = *(volatile uint64_t *)buffer.cpu == value;
        released_view = release_view(view.handle, &release_busy);
        safe = released_view == MAPPING_OK;
        if (!safe || !same) break;
    }
    bool released = safe && dma_buffer_release(buffer.handle) == MAPPING_OK;
    return stack_test_emit("[IRQ_LATENCY][MAPPING] completed=%u required=192 backing_released=%u acquire_busy=%u release_busy=%u acquire_status=%u release_status=%u status=%s\n",
                           completed, released, acquire_busy, release_busy, acquired, released_view,
                           completed == STRESS_MAP_CYCLES && released ? "PASS" : "FAIL") &&
        completed == STRESS_MAP_CYCLES && released;
}

static bool controlled_refusals(void)
{
    void *oversized = kmalloc(SIZE_MAX);
    void *misaligned = kmalloc_aligned(16, 3);
    bool heap = !oversized && !misaligned;
    if (oversized) kfree(oversized);
    if (misaligned) kfree(misaligned);
    dma_buffer_t buffer = {0};
    mapping_status_t dma = dma_buffer_allocate((size_t)UINT32_MAX + 2, PAGE_SIZE, UINT32_MAX, &buffer);
    bool dma_ok = dma == MAPPING_INVALID && !buffer.handle.generation && !buffer.cpu;
    if (dma == MAPPING_OK) (void)dma_buffer_release(buffer.handle);
    uint64_t frame = UINT64_MAX;
    bool pmm = pmm_alloc_constrained_frames(1, 1, UINT64_C(0x100000), &frame);
    bool pmm_ok = !pmm && frame == UINT64_MAX;
    if (pmm) pmm_free_frame((void *)(uintptr_t)frame);
    return stack_test_emit("[IRQ_LATENCY][REFUSALS] heap=%u dma32=%u dma_status=%u pmm=%u pmm_output=%llu status=%s\n",
        heap, dma_ok, dma, pmm_ok, U64(frame), heap && dma_ok && pmm_ok ? "PASS" : "FAIL") && heap && dma_ok && pmm_ok;
}

int irq_latency_stress_run(void)
{
    uint32_t unused = 0;
    if (!irq_is_enabled() || !g_cpu_count || g_cpu_count > HOBBYOS_MAX_CPUS ||
        !__atomic_compare_exchange_n(&g_stress_used, &unused, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 1;
    PmmReachability reach = {0};
    HeapStats before = {0}, grown = {0};
    bool ok = pmm_get_reachability(&reach) && heap_get_stats(&before) &&
        reach.eligible_free_frames >= STRESS_HEADROOM/PAGE_SIZE;
    void *growth = ok ? kmalloc(STRESS_GROWTH) : NULL;
    ok = growth && heap_get_stats(&grown) && grown.total_bytes > before.total_bytes && ok;
    if (growth) kfree(growth);
    bool emitted = stack_test_emit("[IRQ_LATENCY][GROWTH] request=34603008 headroom=134217728 heap_before=%llu heap_after=%llu eligible_frames=%llu status=%s\n",
        U64(before.total_bytes), U64(growth ? grown.total_bytes : 0), U64(reach.eligible_free_frames), ok ? "PASS" : "FAIL");
    ok = emitted && ok;
    if (ok) ok = controlled_refusals();
    if (ok) ok = fragmented_frames();
    unsigned created = 0;
    while (ok && created < 2*g_cpu_count) {
        stress_worker_t *worker = &g_workers[created]; worker->index = created;
        task_create_options_t options = {.name="irq-memory", .task_class=TASK_CLASS_NORMAL, .flags=TASK_FLAG_SYSTEM};
        ok = thread_create_ex_handle(stress_worker, worker, &options, &worker->handle);
        if (ok) ++created;
    }
    uint64_t deadline = clock_monotonic_ns() + UINT64_C(120000000000);
    unsigned entered = 0;
    do {
        entered = 0;
        for (unsigned i = 0; i < created; ++i) entered += __atomic_load_n(&g_workers[i].entered, __ATOMIC_ACQUIRE) != 0;
        if (entered == created) break;
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);
    ok = ok && entered == created && created == 2*g_cpu_count;
    if (!ok) __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
    if (ok) ok = mapping_cycles();
    if (ok) ok = pmm_validate_integrity() && pmm_get_reachability(&reach) && heap_validate_integrity();
    if (!ok) __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    deadline = clock_monotonic_ns() + UINT64_C(120000000000);
    unsigned gone;
    do {
        gone = 0;
        task_snapshot_t snapshot;
        for (unsigned i = 0; i < created; ++i) {
            if (!__atomic_load_n(&g_workers[i].done, __ATOMIC_ACQUIRE)) continue;
            stack_owner_view_t owner;
            stack_handle_t stack = g_workers[i].stack;
            if (!scheduler_snapshot_task_by_handle(g_workers[i].handle, &snapshot) &&
                stack.generation && kernel_stack_read(stack.slot, &owner) &&
                (owner.generation > stack.generation || (owner.generation == stack.generation &&
                    owner.state == STACK_FREE && owner.retirement_generation))) ++gone;
        }
        if (gone == created) break;
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);
    if (gone != created) {
        (void)stack_test_emit("[IRQ_LATENCY][STRESS] created=%u retired=%u status=INCOMPLETE\n", created, gone);
        return 1;
    }
    if (ok) ok = deterministic_stack_coverage();
    /* Its complete owner/table scans reject a moving stack inventory. The
     * allocator workers and coordinator above remain concurrent; the inherited
     * fixture now owns its two concurrent initiators and their real retirement. */
    if (ok) ok = tlb_fixture_run() == 0;
    if (ok) { cpu_request_result_t cache = kernel_cache_writeback(); ok = cache.status == CPU_REQUEST_OK; }
    uint32_t participating = 0;
    for (unsigned i = 0; i < created; ++i) {
        const stress_worker_t *worker = &g_workers[i];
        ok = worker->operations == STRESS_OPERATIONS && !worker->failed && worker->overlap && ok;
        bool resources = worker->heap_allocated == worker->heap_released &&
            worker->frames_allocated == worker->frames_released && worker->dma_allocated == worker->dma_released &&
            worker->views_acquired == worker->views_released;
        ok = resources && ok;
        ok = stack_test_emit("[IRQ_LATENCY][WORKER_RESOURCES] worker=%u heap_allocated=%llu heap_released=%llu frames_allocated=%llu frames_released=%llu dma_allocated=%llu dma_released=%llu views_acquired=%u views_released=%u status=%s\n",
            i, U64(worker->heap_allocated), U64(worker->heap_released), U64(worker->frames_allocated),
            U64(worker->frames_released), U64(worker->dma_allocated), U64(worker->dma_released),
            worker->views_acquired, worker->views_released, resources ? "PASS" : "FAIL") && ok;
        ok = stack_test_emit("[IRQ_LATENCY][WORKER_MAPPING] worker=%u acquire_busy=%u release_busy=%u acquire_status=%u release_status=%u\n",
            i, worker->acquire_busy, worker->release_busy, worker->acquire_status, worker->release_status) && ok;
        ok = stack_test_emit("[IRQ_LATENCY][WORKER] index=%u task=%llu lifecycle=%llu start_ns=%llu end_ns=%llu operations=%u overlap=%u failed=%u retired=1\n",
            i, U64(worker->handle.id), U64(worker->handle.lifecycle_generation), U64(worker->started), U64(worker->ended),
            worker->operations, worker->overlap, worker->failed) && ok;
        for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu) {
            if (worker->touches[cpu]) participating |= 1u << cpu;
            ok = stack_test_emit("[IRQ_LATENCY][PARTICIPATION] worker=%u cpu=%u local_payload_accesses=%u\n", i, cpu, worker->touches[cpu]) && ok;
        }
        for (unsigned kind = 0; kind < 3; ++kind) {
            const api_elapsed_t *sample = &worker->api[kind];
            ok = stack_test_emit("[IRQ_LATENCY][API_ELAPSED] worker=%u kind=%u count=%u maximum_ns=%llu begin_ns=%llu end_ns=%llu operation=%u authority=MONOTONIC_CLOCK includes_enabled_time=1\n",
                i, kind, sample->count, U64(sample->maximum_ns), U64(sample->begin), U64(sample->end), sample->operation) && ok;
        }
    }
    uint32_t required = g_cpu_count == 32 ? UINT32_MAX : (1u << g_cpu_count)-1;
    ok = participating == required && ok;
    ok = pmm_validate_integrity() && heap_validate_integrity() && ok;
    ok = stack_test_emit("[IRQ_LATENCY][STRESS] created=%u retired=%u participating=0x%x required=0x%x status=%s\n",
                        created, gone, participating, required, ok ? "PASS" : "FAIL") && ok;
    return ok ? 0 : 1;
}
