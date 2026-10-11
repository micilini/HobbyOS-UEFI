#include "paging_build.h"
#include "../libc/memory.h"

static int table_index(const paging_build_t *b, uint64_t physical)
{
    for (unsigned i = 0; i < b->count; ++i)
        if (b->tables[i].table.physical == physical) return (int)i;
    return -1;
}

static int allocate(paging_build_t *b, unsigned level)
{
    if (b->count == PAGING_BUILD_TABLES) return -1;
    mapping_table_t table = {0};
    if (!b->ops.allocate(b->context, &table)) return -1;
    if (!table.cpu || !table.physical || (table.physical & 4095) ||
        table.physical > PHYSICAL_ENTRY_LIMIT - 4096) {
        if (table.cpu) b->ops.release(b->context, table);
        return -1;
    }
    /* A faulty allocator must not cause an already owned frame to be freed. */
    if (table_index(b, table.physical) >= 0) return -1;
    for (unsigned i = 0; i < b->count; ++i)
        if (table.cpu == b->tables[i].table.cpu) return -1;
    memset(table.cpu, 0, 4096);
    unsigned index = b->count++;
    b->tables[index] = (paging_build_table_t){.table=table, .level=level};
    return (int)index;
}

static bool map_interval(paging_build_t *b, uint64_t va, uint64_t physical,
                         uint64_t bytes, unsigned shift, bool writable, bool nx)
{
    unsigned leaf_level = (shift - 12) / 9 + 1;
    uint64_t size = UINT64_C(1) << shift;
    if (!bytes || ((va | physical | bytes) & (size - 1)) ||
        !physical_canonical_range(va, bytes)) return false;
    /* Image siblings have mixed rights. Their links allow both operations;
     * only leaves restrict them. The direct subtree is data-only. */
    bool link_nx=va>=RAM_DIRECT_BASE && va<RAM_DIRECT_END && b->plan->features.nx;
    while (bytes) {
        unsigned index = 0;
        for (unsigned level = 4; level > leaf_level; --level) {
            uint64_t *entry = &b->tables[index].table.cpu->entries[(va >> (12 + 9 * (level-1))) & 511];
            if (!*entry) {
                int child = allocate(b, level - 1);
                if (child < 0 || !paging_entry_encode(&b->plan->features, level, false,
                    b->tables[child].table.physical, 0, true, link_nx, entry)) return false;
                index = (unsigned)child;
            } else {
                paging_entry_view_t view;
                if (!paging_entry_decode(&b->plan->features, level, *entry, &view) ||
                    view.leaf || !view.writable || view.user || view.nx != link_nx) return false;
                int child = table_index(b, view.physical);
                if (child < 0) return false;
                index = (unsigned)child;
            }
        }
        unsigned at = (va >> shift) & 511;
        uint64_t leaves = bytes / size;
        if (leaves > 512 - at) leaves = 512 - at;
        for (unsigned i = 0; i < leaves; ++i) {
            uint64_t *entry = &b->tables[index].table.cpu->entries[at + i];
            if (*entry || !paging_entry_encode(&b->plan->features, leaf_level, true,
                physical + i * size, 0, writable, nx, entry)) return false;
        }
        uint64_t chunk = leaves * size;
        va += chunk; physical += chunk; bytes -= chunk;
    }
    return true;
}

static bool window_allocate(void *context, mapping_table_t *out)
{
    paging_build_t *b = context;
    unsigned n = b->window.allocated;
    int index = allocate(b, n == 0 ? 3 : n == 1 ? 2 : 1);
    if (index < 0) return false;
    *out = b->tables[index].table;
    return true;
}
static void window_release(void *context, mapping_table_t table)
{
    /* mapping_window_construct unwinds in reverse allocation order. */
    paging_build_t *b = context;
    if (b->count && b->tables[b->count-1].table.physical == table.physical) {
        b->ops.release(b->context, table);
        b->tables[--b->count] = (paging_build_table_t){0};
    }
}

static bool stack_table_allocate(void *context,mapping_table_t *out)
{
    paging_build_t *b=context;
    unsigned n=b->stacks.allocated;
    int index=allocate(b,n==0 ? 3 : n==1 ? 2 : 1);
    if (index<0) return false;
    *out=b->tables[index].table;return true;
}

void paging_build_abort(paging_build_t *b)
{
    if (!b || b->activated) return;
    while (b->count) b->ops.release(b->context, b->tables[--b->count].table);
    memset(&b->window, 0, sizeof(b->window));
    memset(&b->stacks, 0, sizeof(b->stacks));
    b->built = b->verified = false;
}

bool paging_build_create(paging_build_t *b, const physical_contract_t *c,
                         const ram_map_plan_t *p, const image_permissions_t *permissions,
                         const stack_layout_t *stacks,
                         const mapping_table_ops_t *ops,
                         void *context)
{
    if (!b || b->count || b->activated || !c || !c->ready || !p || !p->ready ||
        !ops || !ops->allocate || !ops->release || !c->identity_limit ||
        (c->identity_limit & 4095) || c->identity_limit > PHYSICAL_ENTRY_LIMIT ||
        !image_permissions_verify(permissions,c)) return false;
    b->ops = *ops; b->context = context; b->plan = p;
    b->permissions=permissions;
    b->stack_layout=stacks;
    b->identity_limit = c->identity_limit;
    b->image_first = c->image_first & ~UINT64_C(4095);
    b->image_end = (c->image_end + 4095) & ~UINT64_C(4095);
    if (allocate(b, 4) < 0) goto fail;
    for (uint64_t va=b->image_first;va<b->image_end;) {
        if (permissions->input.bsp_guard_first && va==permissions->input.bsp_guard_first+KERNEL_IMAGE_OFFSET) {
            va+=4096;continue;
        }
        image_rights_t rights,next;
        if (!image_permissions_rights(permissions,va-KERNEL_IMAGE_OFFSET,4096,true,&rights)) goto fail;
        uint64_t end=va+4096;
        while (end<b->image_end && image_permissions_rights(permissions,end-KERNEL_IMAGE_OFFSET,4096,true,&next) &&
            next.writable==rights.writable && next.executable==rights.executable) end+=4096;
        if (!map_interval(b,va,va-KERNEL_IMAGE_OFFSET,end-va,12,rights.writable,
                          p->features.nx && !rights.executable)) goto fail;
        va=end;
    }
    for (unsigned i = 0; i < p->count; ++i) {
        const ram_interval_t *r = &p->intervals[i];
        uint64_t physical = r->first;
        while (physical < r->end) {
            unsigned shift = image_permissions_leaf_shift(permissions,p,r,physical);
            uint64_t size = UINT64_C(1) << shift, end = physical + size;
            image_rights_t rights,next;
            if (!image_permissions_rights(permissions,physical,size,false,&rights)) goto fail;
            /* Coalesce equal-size leaves to fill table arrays once per run. */
            while (end < r->end && image_permissions_leaf_shift(permissions,p,r,end)==shift &&
                image_permissions_rights(permissions,end,size,false,&next) && next.writable==rights.writable) end+=size;
            if (!map_interval(b, RAM_DIRECT_BASE + physical, physical, end-physical, shift,
                              rights.writable,p->features.nx)) goto fail;
            physical = end;
        }
    }
    const mapping_table_ops_t window_ops = {window_allocate, window_release};
    if (mapping_window_construct(&b->window, b->tables[0].table.cpu, &window_ops, b,p->features.nx) != MAPPING_OK) goto fail;
    const mapping_table_ops_t stack_ops={stack_table_allocate,window_release};
    uint64_t stack_bytes;
    if (!stack_layout_expected(stacks,permissions,p,&stack_bytes) ||
        !stack_window_construct(&b->stacks,b->tables[0].table.cpu,&p->features,&stack_ops,b)) goto fail;
    if (stacks) for (unsigned i=0;i<STACK_REGION_SLOTS;++i) {
        stack_owner_view_t owner;
        if (!stack_owner_read(&stacks->owners[i],&owner)) goto fail;
        if (owner.state==STACK_PREPARED || owner.state==STACK_ALLOCATED || owner.state==STACK_BOUND || owner.state==STACK_LIVE)
            if (!stack_window_install(&b->stacks,&owner)) goto fail;
    }
    b->built = true;
    return true;
fail:
    paging_build_abort(b);
    return false;
}

bool paging_build_rights(const paging_build_t *b, uint64_t address, uint64_t *physical, bool *writable, bool *nx)
{
    if (!b || !b->built || !physical || !writable || !nx || !physical_canonical_range(address, 1)) return false;
    unsigned index = 0;
    bool blocked = false, write=true;
    for (unsigned level = 4; level; --level) {
        uint64_t entry = b->tables[index].table.cpu->entries[(address >> (12+9*(level-1))) & 511];
        paging_entry_view_t view;
        if (!paging_entry_decode(&b->plan->features, level, entry, &view)) return false;
        blocked |= view.nx;
        write &= view.writable;
        if (view.user) return false;
        if (view.leaf) { *physical = view.physical + (address & (view.bytes-1)); *nx = blocked; *writable=write; return true; }
        int child = table_index(b, view.physical);
        if (child < 0) return false;
        index = (unsigned)child;
    }
    return false;
}

bool paging_build_lookup(const paging_build_t *b, uint64_t address, uint64_t *physical, bool *nx)
{ bool writable; return paging_build_rights(b,address,physical,&writable,nx); }

static bool verify_table(paging_build_t *b, unsigned index, unsigned level,
                         uint64_t address, bool inherited_write, bool inherited_nx, uint64_t *image)
{
    paging_build_table_t *record = &b->tables[index];
    if (record->seen || record->level != level || !record->table.cpu ||
        !ram_map_contains(b->plan, record->table.physical, 4096)) return false;
    record->seen = true; ++b->verified_tables;
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t raw = record->table.cpu->entries[i];
        if (level == 4 && i < 256 && raw) return false;
        if (!raw) continue;
        uint64_t va = address | ((uint64_t)i << (12 + 9*(level-1)));
        if (level == 4 && (i & 256)) va |= UINT64_C(0xffff000000000000);
        paging_entry_view_t v;
        if (!paging_entry_decode(&b->plan->features, level, raw, &v) || v.user || v.pat_slot) return false;
        bool direct = va >= RAM_DIRECT_BASE && va < RAM_DIRECT_END;
        bool nx = inherited_nx || v.nx, writable=inherited_write && v.writable;
        if (!v.leaf) {
            uint64_t span=UINT64_C(1) << (12+9*(level-1));
            bool image_path=va<=b->image_end-1 && (va>=b->image_first || b->image_first-va<span);
            bool window_path=va<=DEVICE_WINDOW_BASE+DEVICE_WINDOW_BYTES-1 &&
                (va>=DEVICE_WINDOW_BASE || DEVICE_WINDOW_BASE-va<span);
            bool stack_path=va<=STACK_REGION_END-1 && (va>=STACK_REGION_BASE || STACK_REGION_BASE-va<span);
            if (!direct && !image_path && !window_path && !stack_path) return false;
            int child = table_index(b, v.physical);
            if (child < 0 || !verify_table(b, (unsigned)child, level-1, va, writable,nx, image)) return false;
            continue;
        }
        if (va>=STACK_REGION_BASE && va<STACK_REGION_END) {
            if (!stack_layout_leaf(b->stack_layout,b->permissions,b->plan,va,v.physical,v.bytes,writable,nx)) return false;
            continue;
        }
        if (!image_permissions_check_leaf(b->permissions,va,v.physical,v.bytes,writable,nx,b->plan->features.nx)) return false;
        if (direct) {
            uint64_t expected = va - RAM_DIRECT_BASE;
            const ram_interval_t *r = ram_map_interval(b->plan, expected);
            if (v.physical != expected || !r || v.bytes > r->end - expected ||
                nx != b->plan->features.nx ||
                (12 + 9*(level-1)) != image_permissions_leaf_shift(b->permissions,b->plan,r,expected)) return false;
            ++b->leaves[level-1]; b->direct_bytes += v.bytes;
        } else if (va >= b->image_first && va < b->image_end) {
            if (level != 1 || v.physical != va-KERNEL_IMAGE_OFFSET || v.bytes > b->image_end-va) return false;
            *image += v.bytes;
        } else return false;
    }
    return true;
}

bool paging_build_verify(paging_build_t *b, const physical_contract_t *c,
                         const ram_mtrr_inputs_t *m, ram_extent_t authorized_framebuffer,
                         const paging_required_path_t *paths,
                         unsigned count)
{
    if (b) b->verified = false;
    if (!b || !b->built || b->activated || !b->count || b->count > PAGING_BUILD_TABLES ||
        !ram_map_plan_verify(b->plan, c, m, authorized_framebuffer) ||
        !image_permissions_verify(b->permissions,c) ||
        b->identity_limit!=c->identity_limit ||
        b->image_first!=(c->image_first & ~UINT64_C(4095)) ||
        b->image_end!=((c->image_end+4095) & ~UINT64_C(4095)) ||
        (count && !paths) || count > 16) return false;
    b->verified = false; b->verified_tables = 0; b->direct_bytes = 0;
    memset(b->leaves, 0, sizeof(b->leaves));
    for (unsigned i = 0; i < b->count; ++i) {
        b->tables[i].seen = false;
        uint64_t physical = b->tables[i].table.physical;
        if (!physical || (physical & 4095) || physical > c->identity_limit-4096) return false;
        for (unsigned j = 0; j < i; ++j)
            if (physical == b->tables[j].table.physical ||
                b->tables[i].table.cpu == b->tables[j].table.cpu) return false;
    }
    uint64_t image = 0;
    if (!verify_table(b, 0, 4, 0, true,false, &image) || b->verified_tables != b->count ||
        b->direct_bytes != b->plan->bytes ||
        image != image_permissions_linked_bytes(b->permissions) || !b->window.ready || b->window.allocated != MAPPING_WINDOW_TABLES ||
        !stack_window_verify(&b->stacks,b->tables[0].table.cpu,b->stack_layout,b->permissions,b->plan)) return false;
    for (unsigned i = 0; i < MAPPING_WINDOW_TABLES; ++i) {
        int index = table_index(b, b->window.tables[i].physical);
        if (index < 0 || !b->tables[index].seen || b->tables[index].table.cpu != b->window.tables[i].cpu) return false;
    }
    uint64_t window_flags=PAGE_PRESENT|PAGE_RW|(b->plan->features.nx ? PAGE_NX : 0);
    if (b->window.nx!=b->plan->features.nx ||
        b->tables[0].table.cpu->entries[(DEVICE_WINDOW_BASE >> 39) & 511] !=
        (b->window.tables[0].physical | window_flags) ||
        b->window.tables[0].cpu->entries[0] != (b->window.tables[1].physical | window_flags)) return false;
    for (unsigned i = 0; i < 512; ++i)
        if (b->window.tables[1].cpu->entries[i] != (b->window.tables[i+2].physical | window_flags)) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (paths[i].address < UINT64_C(0xffff800000000000) ||
            !physical_canonical_range(paths[i].address, paths[i].bytes)) return false;
        uint64_t cursor = paths[i].address, left = paths[i].bytes;
        while (left) {
            uint64_t physical; bool nx,writable;
            if (!paging_build_rights(b, cursor, &physical,&writable, &nx) || (nx && paths[i].executable) ||
                (!writable && paths[i].writable)) return false;
            uint64_t chunk = 4096-(cursor & 4095);
            if (chunk > left) chunk = left;
            cursor += chunk; left -= chunk;
        }
    }
    for (unsigned i = 0; i < b->count; ++i)
        b->tables[i].verified_hash = paging_table_hash(b->tables[i].table.cpu);
    b->verified = true;
    return true;
}

uint64_t paging_table_hash(const PageTable *table)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 512; ++i) {
        /* Hardware may set accessed/dirty bits after the verified root load. */
        uint64_t entry = table->entries[i] & ~UINT64_C(0x60);
        for (unsigned byte = 0; byte < 8; ++byte) {
            hash ^= (entry >> (byte * 8)) & 255;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

bool paging_build_activate(paging_build_t *b, void (*load)(void *, uint64_t), void *context)
{
    if (!b || !b->verified || b->activated || !load) return false;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    load(context, b->tables[0].table.physical);
    b->activated = true;
    return true;
}
