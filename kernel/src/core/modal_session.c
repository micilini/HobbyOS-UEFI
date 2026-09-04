#include "modal_session.h"
#include "spinlock.h"
#include "../shell/shell.h"
#include "../drivers/serial.h"
#include "../libc/memory.h"

typedef struct {
    spinlock_t lock;
    modal_session_state_t state;
    uint64_t next_generation, active_generation, active_route_generation;
    task_handle_t owner, last_closed_owner;
    char owner_name[32];
    modal_session_token_t last_closed_token;
    uint64_t opens, closes, close_idempotent, busy_rejections;
    uint64_t invalid_token_rejections, wrong_owner_rejections;
    uint64_t stale_token_rejections, begin_failures, end_failures;
    uint64_t owner_death_recoveries, cleanup_recoveries;
    uint64_t input_token_rejections, stale_events_dropped;
} modal_session_runtime_t;

static modal_session_runtime_t g_modal;
static volatile uint8_t g_modal_quiet;

static bool handle_valid(task_handle_t h)
{ return h.id != TASK_ID_INVALID && h.lifecycle_generation != 0; }
static bool handle_equal(task_handle_t a, task_handle_t b)
{ return a.id == b.id && a.lifecycle_generation == b.lifecycle_generation; }
static void copy_name(char *dst, const char *src)
{
    uint32_t i = 0;
    if (src) {
        for (; i < 31 && src[i]; i++)
            dst[i] = src[i];
    }
    dst[i] = 0;
}
bool modal_session_token_is_valid(modal_session_token_t t)
{ return t.session_generation && t.route_generation && t.owner_task_id &&
         t.owner_lifecycle_generation; }
bool modal_session_token_equal(modal_session_token_t a,modal_session_token_t b)
{ return a.session_generation==b.session_generation&&a.route_generation==b.route_generation&&
         a.owner_task_id==b.owner_task_id&&a.owner_lifecycle_generation==b.owner_lifecycle_generation; }
static modal_session_token_t active_token_locked(void)
{
    return (modal_session_token_t){g_modal.active_generation,
        g_modal.active_route_generation,g_modal.owner.id,
        g_modal.owner.lifecycle_generation};
}

void modal_session_init(void)
{
    memset(&g_modal,0,sizeof(g_modal));spinlock_init(&g_modal.lock);
    g_modal.state=MODAL_SESSION_INACTIVE;g_modal.next_generation=1;
    g_modal.owner=TASK_HANDLE_INVALID;g_modal.last_closed_owner=TASK_HANDLE_INVALID;
    __atomic_store_n(&g_modal_quiet,0,__ATOMIC_RELEASE);
}
void modal_session_test_set_quiet(bool q)
{__atomic_store_n(&g_modal_quiet,q?1:0,__ATOMIC_RELEASE);}

modal_begin_result_t modal_session_begin(const char *name,modal_session_token_t*out)
{
    if(out)*out=MODAL_SESSION_TOKEN_INVALID;
    if(!out)return MODAL_BEGIN_INTERNAL_ERROR;
    task_handle_t owner;
    if(!scheduler_current_task_handle(&owner)||!handle_valid(owner))return MODAL_BEGIN_INVALID_OWNER;
    irq_flags_t f=spin_lock_irqsave(&g_modal.lock);
    if(g_modal.state!=MODAL_SESSION_INACTIVE){g_modal.busy_rejections++;spin_unlock_irqrestore(&g_modal.lock,f);return MODAL_BEGIN_BUSY;}
    if(!g_modal.next_generation){g_modal.begin_failures++;spin_unlock_irqrestore(&g_modal.lock,f);return MODAL_BEGIN_GENERATION_EXHAUSTED;}
    uint64_t generation=g_modal.next_generation++;
    if(!g_modal.next_generation)g_modal.next_generation=0;
    g_modal.state=MODAL_SESSION_OPENING;g_modal.owner=owner;
    g_modal.active_generation=generation;copy_name(g_modal.owner_name,name);
    shell_pause_input_for_modal_ui();
    input_router_transition_result_t transition;
    if(!input_router_begin_modal(&transition)){
        shell_resume_input_from_modal_ui();g_modal.state=MODAL_SESSION_INACTIVE;
        g_modal.owner=TASK_HANDLE_INVALID;g_modal.active_generation=0;
        g_modal.owner_name[0]=0;g_modal.begin_failures++;
        spin_unlock_irqrestore(&g_modal.lock,f);return MODAL_BEGIN_ROUTER_REFUSED;
    }
    g_modal.active_route_generation=transition.generation;
    g_modal.state=MODAL_SESSION_ACTIVE;g_modal.opens++;
    *out=active_token_locked();spin_unlock_irqrestore(&g_modal.lock,f);
    if(!__atomic_load_n(&g_modal_quiet,__ATOMIC_ACQUIRE))serial_write_all("[MODAL] session_begin OK\n");
    return MODAL_BEGIN_OK;
}

modal_end_result_t modal_session_end(modal_session_token_t token)
{
    task_handle_t caller=TASK_HANDLE_INVALID;(void)scheduler_current_task_handle(&caller);
    irq_flags_t f=spin_lock_irqsave(&g_modal.lock);
    if(!modal_session_token_is_valid(token)){g_modal.invalid_token_rejections++;spin_unlock_irqrestore(&g_modal.lock,f);return MODAL_END_INVALID_TOKEN;}
    if(g_modal.state==MODAL_SESSION_INACTIVE){
        bool same=modal_session_token_equal(token,g_modal.last_closed_token);
        if(same&&!handle_equal(caller,g_modal.last_closed_owner)){
            g_modal.wrong_owner_rejections++;
            spin_unlock_irqrestore(&g_modal.lock,f);
            return MODAL_END_WRONG_OWNER;
        }
        if(same)g_modal.close_idempotent++;else g_modal.stale_token_rejections++;
        spin_unlock_irqrestore(&g_modal.lock,f);
        return same?MODAL_END_ALREADY_CLOSED:MODAL_END_INVALID_TOKEN;
    }
    if(g_modal.state!=MODAL_SESSION_ACTIVE){spin_unlock_irqrestore(&g_modal.lock,f);return MODAL_END_NOT_ACTIVE;}
    bool token_matches = modal_session_token_equal(token,
                                                    active_token_locked());
#ifdef HOBBYOS_MODAL_NEGATIVE_ACCEPT_STALE_TOKEN
    token_matches = modal_session_token_is_valid(token);
#endif
    if (!token_matches) {
        g_modal.stale_token_rejections++;
        spin_unlock_irqrestore(&g_modal.lock, f);
        return MODAL_END_INVALID_TOKEN;
    }
    if(!handle_equal(caller,g_modal.owner)){g_modal.wrong_owner_rejections++;spin_unlock_irqrestore(&g_modal.lock,f);return MODAL_END_WRONG_OWNER;}
    g_modal.state=MODAL_SESSION_CLOSING;input_router_transition_result_t tr;
    if(!input_router_end_modal(&tr)){g_modal.state=MODAL_SESSION_ACTIVE;g_modal.end_failures++;spin_unlock_irqrestore(&g_modal.lock,f);return MODAL_END_ROUTER_REFUSED;}
    shell_resume_input_from_modal_ui();g_modal.last_closed_token=token;
    g_modal.last_closed_owner=g_modal.owner;g_modal.owner=TASK_HANDLE_INVALID;
    g_modal.owner_name[0]=0;g_modal.active_generation=0;g_modal.active_route_generation=0;
    g_modal.state=MODAL_SESSION_INACTIVE;g_modal.closes++;
    spin_unlock_irqrestore(&g_modal.lock,f);
    if(!__atomic_load_n(&g_modal_quiet,__ATOMIC_ACQUIRE))serial_write_all("[MODAL] session_end OK\n");
    return MODAL_END_OK;
}

bool modal_session_recover_owner(task_handle_t owner,modal_recovery_reason_t reason)
{
    if(!handle_valid(owner))return false;
    irq_flags_t f=spin_lock_irqsave(&g_modal.lock);
    if(g_modal.state==MODAL_SESSION_INACTIVE){bool same=handle_equal(owner,g_modal.last_closed_owner);spin_unlock_irqrestore(&g_modal.lock,f);return same;}
    if(g_modal.state!=MODAL_SESSION_ACTIVE||!handle_equal(owner,g_modal.owner)){spin_unlock_irqrestore(&g_modal.lock,f);return false;}
    modal_session_token_t token=active_token_locked();g_modal.state=MODAL_SESSION_CLOSING;
    input_router_transition_result_t tr;
    if(input_router_route_state()==INPUT_ROUTE_MODAL&&!input_router_end_modal(&tr)){g_modal.state=MODAL_SESSION_ACTIVE;spin_unlock_irqrestore(&g_modal.lock,f);return false;}
    if(input_router_route_state()!=INPUT_ROUTE_DEFAULT){g_modal.state=MODAL_SESSION_ACTIVE;spin_unlock_irqrestore(&g_modal.lock,f);return false;}
    shell_resume_input_from_modal_ui();g_modal.last_closed_token=token;
    g_modal.last_closed_owner=owner;g_modal.owner=TASK_HANDLE_INVALID;
    g_modal.owner_name[0]=0;g_modal.active_generation=0;g_modal.active_route_generation=0;
    g_modal.state=MODAL_SESSION_INACTIVE;g_modal.closes++;
    if(reason==MODAL_RECOVERY_LIFECYCLE)g_modal.owner_death_recoveries++;else g_modal.cleanup_recoveries++;
    spin_unlock_irqrestore(&g_modal.lock,f);return true;
}

static modal_input_result_t validate_input_locked(modal_session_token_t token,task_handle_t caller)
{
    if(!modal_session_token_is_valid(token)){g_modal.input_token_rejections++;return MODAL_INPUT_INVALID_TOKEN;}
    if(g_modal.state!=MODAL_SESSION_ACTIVE)return MODAL_INPUT_SESSION_CLOSED;
    if(!modal_session_token_equal(token,active_token_locked())){g_modal.input_token_rejections++;return MODAL_INPUT_INVALID_TOKEN;}
    if(!handle_equal(caller,g_modal.owner)){g_modal.wrong_owner_rejections++;return MODAL_INPUT_WRONG_OWNER;}
    return MODAL_INPUT_OK;
}
static modal_input_result_t modal_input(modal_session_token_t token,input_event_t*out,bool wait)
{
    if (!out)
        return MODAL_INPUT_ERROR;
    task_handle_t caller = TASK_HANDLE_INVALID;
    (void)scheduler_current_task_handle(&caller);
    irq_flags_t f = spin_lock_irqsave(&g_modal.lock);
    modal_input_result_t v = validate_input_locked(token, caller);
    spin_unlock_irqrestore(&g_modal.lock, f);
    if (v != MODAL_INPUT_OK)
        return v;
    input_queue_entry_t e;
    input_queue_pop_result_t p = wait ?
        input_router_modal_wait_entry(&e) :
        input_router_modal_try_pop_entry(&e);
    if (p == INPUT_QUEUE_POP_EMPTY)
        return MODAL_INPUT_EMPTY;
    if (p == INPUT_QUEUE_POP_CANCELLED)
        return MODAL_INPUT_CANCELLED;
    if (p != INPUT_QUEUE_POP_OK)
        return MODAL_INPUT_ERROR;
    if (e.destination != INPUT_DEST_MODAL ||
        e.route_generation != token.route_generation) {
        f = spin_lock_irqsave(&g_modal.lock);
        g_modal.stale_events_dropped++;
        spin_unlock_irqrestore(&g_modal.lock, f);
        return MODAL_INPUT_STALE_EVENT;
    }
    f = spin_lock_irqsave(&g_modal.lock);
    v = validate_input_locked(token, caller);
    spin_unlock_irqrestore(&g_modal.lock, f);
    if (v != MODAL_INPUT_OK)
        return MODAL_INPUT_SESSION_CLOSED;
    *out = e.event;
    return MODAL_INPUT_OK;
}
modal_input_result_t modal_session_try_pop(modal_session_token_t t,input_event_t*out){return modal_input(t,out,false);}
modal_input_result_t modal_session_wait(modal_session_token_t t,input_event_t*out){return modal_input(t,out,true);}

bool modal_session_snapshot(modal_session_snapshot_t*out)
{
    if (!out)
        return false;
    irq_flags_t f = spin_lock_irqsave(&g_modal.lock);
    memset(out, 0, sizeof(*out));
    out->state=g_modal.state;out->active_generation=g_modal.active_generation;out->route_generation=g_modal.active_route_generation;out->owner=g_modal.owner;copy_name(out->owner_name,g_modal.owner_name);out->last_closed_token=g_modal.last_closed_token;
    out->opens=g_modal.opens;out->closes=g_modal.closes;out->rejected=g_modal.busy_rejections;out->invalid_tokens=g_modal.invalid_token_rejections+g_modal.stale_token_rejections;out->owner_death_recoveries=g_modal.owner_death_recoveries;out->cleanup_recoveries=g_modal.cleanup_recoveries;
    out->shell_paused=shell_is_input_paused_for_modal_ui();
    out->router_state=input_router_route_state();
    spin_unlock_irqrestore(&g_modal.lock,f);
    return true;
}
bool modal_session_validate(uint64_t*outv)
{
    modal_session_snapshot_t s;uint64_t v=0;if(!modal_session_snapshot(&s))return false;
    if(s.state==MODAL_SESSION_INACTIVE&&(handle_valid(s.owner)||s.router_state!=INPUT_ROUTE_DEFAULT||s.shell_paused))v++;
    if(s.state==MODAL_SESSION_ACTIVE&&(!handle_valid(s.owner)||!s.active_generation||!s.route_generation||s.router_state!=INPUT_ROUTE_MODAL||!s.shell_paused))v++;
    if(s.state==MODAL_SESSION_OPENING||s.state==MODAL_SESSION_CLOSING)v++;
    if (outv)
        *outv = v;
    return v == 0;
}
bool modal_session_is_active(void){return modal_session_state()!=MODAL_SESSION_INACTIVE;}
modal_session_state_t modal_session_state(void){irq_flags_t f=spin_lock_irqsave(&g_modal.lock);modal_session_state_t s=g_modal.state;spin_unlock_irqrestore(&g_modal.lock,f);return s;}

bool modal_session_test_begin(const char*name){modal_session_token_t t;return modal_session_begin(name,&t)==MODAL_BEGIN_OK;}
bool modal_session_test_end(void){modal_session_snapshot_t s;if(!modal_session_snapshot(&s)||!handle_valid(s.owner))return false;return modal_session_recover_owner(s.owner,MODAL_RECOVERY_TEST);}
bool modal_session_test_try_pop(input_event_t*out){input_queue_entry_t e;if(input_router_modal_try_pop_entry(&e)!=INPUT_QUEUE_POP_OK)return false;if(out)*out=e.event;return true;}
