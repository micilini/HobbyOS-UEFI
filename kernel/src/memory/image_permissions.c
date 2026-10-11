#include "image_permissions.h"

static uint64_t down(uint64_t n) { return n & ~UINT64_C(4095); }
static uint64_t up(uint64_t n) { return (n + 4095) & ~UINT64_C(4095); }
static bool mutable(image_role_t role)
{ return role == IMAGE_DATA || role == IMAGE_BSS || role == IMAGE_BOOT_DATA; }
static bool intersects(uint64_t first, uint64_t end, uint64_t a, uint64_t b)
{ return first < b && a < end; }

bool image_permissions_create(image_permissions_t *out, const physical_contract_t *c,
                              const image_permission_input_t *in)
{
    if (!out || !c || !c->ready || !in || c->image_first < KERNEL_IMAGE_OFFSET ||
        c->image_end <= c->image_first || !physical_canonical_range(c->image_first, c->image_end-c->image_first) ||
        (c->image_first & 4095) || c->image_end > UINT64_MAX-4095) return false;
    uint64_t first = c->image_first-KERNEL_IMAGE_OFFSET;
    uint64_t end = c->image_end-KERNEL_IMAGE_OFFSET;
    if (end > PHYSICAL_ADDRESS_LIMIT-4095 || !in->boot_first ||
        (in->boot_first & 4095) || (in->boot_end & 4095) ||
        in->boot_first >= in->boot_end || in->boot_end > first) return false;
    for (unsigned i=0; i<IMAGE_ROLE_COUNT; ++i) {
        const image_section_t *s=&in->sections[i];
        uint64_t low=i<IMAGE_BOOT_TEXT ? first : in->boot_first;
        uint64_t high=i<IMAGE_BOOT_TEXT ? end : in->boot_end;
        /* Data/BSS may be empty in a declared link profile; executable and
         * immutable protocol sections are required by this kernel ABI. */
        if (s->role!=i || s->first<low || s->end>high || s->end<s->first ||
            (s->first==s->end && i!=IMAGE_DATA && i!=IMAGE_BSS)) return false;
        if (i && i!=IMAGE_BOOT_TEXT && s->first<in->sections[i-1].end) return false;
        if (s->first==s->end) continue;
        for (unsigned j=0; j<i; ++j) {
            const image_section_t *t=&in->sections[j];
            if (t->first==t->end) continue;
            if (intersects(down(s->first),up(s->end),down(t->first),up(t->end)) &&
                (mutable(s->role)!=mutable(t->role) ||
                 (s->role==IMAGE_TEXT)!=(t->role==IMAGE_TEXT))) return false;
        }
    }
    if (in->sections[IMAGE_TEXT].first!=first || in->sections[IMAGE_BSS].end!=end ||
        in->sections[IMAGE_BOOT_TEXT].first!=in->boot_first ||
        up(in->sections[IMAGE_BOOT_DATA].end)!=in->boot_end) return false;
    if (in->bsp_guard_first || in->bsp_guard_end || in->bsp_stack_first || in->bsp_stack_end) {
        if ((in->bsp_guard_first&4095) || in->bsp_guard_first<in->sections[IMAGE_BSS].first ||
            in->bsp_guard_first>PHYSICAL_ADDRESS_LIMIT-20480 ||
            in->bsp_guard_end!=in->bsp_guard_first+4096 ||
            in->bsp_stack_first!=in->bsp_guard_end ||
            in->bsp_stack_end!=in->bsp_stack_first+16384 || in->bsp_stack_end!=end) return false;
    }
    *out=(image_permissions_t){.input=*in, .first=first, .end=up(end),
        .linked_first=c->image_first, .linked_end=up(c->image_end), .ready=true};
    return true;
}

bool image_permissions_verify(const image_permissions_t *p, const physical_contract_t *c)
{
    image_permissions_t expected;
    return p && p->ready && image_permissions_create(&expected,c,&p->input) &&
        p->first==expected.first && p->end==expected.end &&
        p->linked_first==expected.linked_first && p->linked_end==expected.linked_end;
}

bool image_permissions_linked_range(const image_permissions_t *p,uint64_t address,uint64_t bytes)
{
    if (!p || !p->ready || !bytes || address<p->linked_first || address>=p->linked_end ||
        bytes>p->linked_end-address) return false;
    uint64_t first=p->input.bsp_guard_first;
    return !first || !intersects(address,address+bytes,first+KERNEL_IMAGE_OFFSET,
                                 p->input.bsp_guard_end+KERNEL_IMAGE_OFFSET);
}

uint64_t image_permissions_linked_bytes(const image_permissions_t *p)
{ return p && p->ready ? p->linked_end-p->linked_first-(p->input.bsp_guard_end-p->input.bsp_guard_first) : 0; }

bool image_permissions_overlaps(const image_permissions_t *p, uint64_t physical, uint64_t bytes)
{
    if (!p || !p->ready || !bytes || physical>=PHYSICAL_ADDRESS_LIMIT ||
        bytes>PHYSICAL_ADDRESS_LIMIT-physical) return true;
    return intersects(physical,physical+bytes,p->first,p->end) ||
        intersects(physical,physical+bytes,p->input.boot_first,p->input.boot_end);
}

/* Advance between permission boundaries, never once per page of huge RAM. */
static uint64_t boundary(const image_permissions_t *p, uint64_t at, uint64_t end)
{
    const uint64_t outer[]={p->first,p->end,p->input.boot_first,p->input.boot_end};
    for (unsigned i=0;i<4;++i) if (outer[i]>at && outer[i]<end) end=outer[i];
    for (unsigned i=0;i<IMAGE_ROLE_COUNT;++i) {
        const image_section_t *s=&p->input.sections[i];
        if (s->first==s->end) continue;
        uint64_t a=down(s->first),b=up(s->end);
        if (a>at && a<end) end=a;
        if (b>at && b<end) end=b;
    }
    return end;
}

bool image_permissions_rights(const image_permissions_t *p, uint64_t physical,
                              uint64_t bytes, bool linked, image_rights_t *out)
{
    if (!p || !p->ready || !out || !bytes || ((physical|bytes)&4095) ||
        physical>=PHYSICAL_ADDRESS_LIMIT || bytes>PHYSICAL_ADDRESS_LIMIT-physical ||
        (linked && (physical<p->first || bytes>p->end-physical || physical>=p->end))) return false;
    if (linked && !image_permissions_linked_range(p,physical+KERNEL_IMAGE_OFFSET,bytes)) return false;
    image_rights_t result={0};
    uint64_t end=physical+bytes,at=physical;
    while (at<end) {
        image_rights_t current={.writable=!image_permissions_overlaps(p,at,4096)};
        for (unsigned i=0;i<IMAGE_ROLE_COUNT;++i) {
            const image_section_t *s=&p->input.sections[i];
            if (s->first!=s->end && at>=down(s->first) && at<up(s->end)) {
                current.writable=mutable(s->role);
                current.executable=linked && s->role==IMAGE_TEXT;
            }
        }
        if (at!=physical && (result.writable!=current.writable || result.executable!=current.executable)) return false;
        result=current; at=boundary(p,at,end);
    }
    *out=result; return true;
}

bool image_permissions_check_leaf(const image_permissions_t *p, uint64_t va,
                                 uint64_t physical, uint64_t bytes, bool writable,
                                 bool nx, bool supported)
{
    if (!p || !p->ready || !bytes || ((physical|bytes)&4095) ||
        physical>=PHYSICAL_ADDRESS_LIMIT || bytes>PHYSICAL_ADDRESS_LIMIT-physical || (!supported && nx)) return false;
    bool linked=va>=p->linked_first && va<p->linked_end;
    if (linked && (!image_permissions_linked_range(p,va,bytes) || physical!=va-KERNEL_IMAGE_OFFSET)) return false;
    if (!linked && (va<RAM_DIRECT_BASE || va>=RAM_DIRECT_END || bytes>RAM_DIRECT_END-va || physical!=va-RAM_DIRECT_BASE)) return false;
    if (bytes!=4096 && image_permissions_overlaps(p,physical,bytes)) return false;
    uint64_t end=physical+bytes;
    for (uint64_t at=physical;at<end;at=boundary(p,at,end)) {
        bool expected_write=true, expected_execute=false;
        if ((at>=p->first && at<p->end) || (at>=p->input.boot_first && at<p->input.boot_end)) expected_write=false;
        for (unsigned i=0;i<IMAGE_ROLE_COUNT;++i) {
            const image_section_t *s=&p->input.sections[i];
            if (s->first==s->end || at<down(s->first) || at>=up(s->end)) continue;
            switch (s->role) {
            case IMAGE_DATA: case IMAGE_BSS: case IMAGE_BOOT_DATA: expected_write=true; break;
            case IMAGE_TEXT: expected_execute=linked; break;
            default: break;
            }
        }
        if (writable!=expected_write || (supported && nx==expected_execute)) return false;
    }
    return true;
}

unsigned image_permissions_leaf_shift(const image_permissions_t *p, const ram_map_plan_t *ram,
                                     const ram_interval_t *interval, uint64_t physical)
{
    unsigned shift=ram_map_leaf_shift(ram,interval,physical);
    while (shift>12 && image_permissions_overlaps(p,physical,UINT64_C(1)<<shift)) shift-=9;
    return shift;
}
