#include "usb_hub.h"
#include "usb_hotplug.h"
#include "xhci.h"
#include "../../../memory/paging.h"
#include "../../../memory/heap.h"
#include "../../../graphics/console.h"
#include "../../../libc/string.h"
#include "../../../drivers/timer.h"
#include "../../../libc/memory.h"
#include "../../keyboard.h"
#include "../../../memory/pmem.h"

static volatile uint64_t g_cmd_last_ptr = 0;
static volatile uint32_t g_cmd_last_cc = 0;
static volatile uint32_t g_cmd_last_slot = 0;
static volatile int g_cmd_waiting = 0;
static volatile int g_ep0_waiting = 0;
static volatile int g_ep0_target_slot = 0;
static volatile int g_ep0_result_cc = 0;

uint64_t xhci_read64(uint64_t addr);
void xhci_write64(uint64_t addr, uint64_t value);

int xhci_port_reset_hs_style(volatile xhci_port_regs_t *port, int port_index, int is_usb3);

static void xhci_queue_kbd_request_buf(uint8_t slot, uint8_t *buf);
void xhci_parse_config(void *config_buffer, uint16_t len, usb_device_info_t *info);

static volatile uint64_t xhci_dbg_isr_count = 0;
static volatile uint64_t xhci_dbg_last_progress_ms = 0;

static volatile uint32_t xhci_dbg_last_evt_processed = 0;
static volatile uint32_t xhci_dbg_last_kbd_processed = 0;

static volatile uint32_t xhci_dbg_no_progress_streak = 0;

static volatile uint64_t xhci_dbg_last_kbd_ms[64] = {0};
static volatile uint32_t xhci_dbg_repeat_zombie_hits[64] = {0};

static volatile int xhci_processing_events = 0;

static volatile uint64_t xhci_total_events_processed = 0;
static volatile uint64_t xhci_total_kbd_events = 0;
static volatile uint64_t xhci_last_successful_process_ms = 0;

static volatile int xhci_driver_ready = 0;

static volatile uint32_t xhci_lock_flag = 0;

static inline int xhci_try_lock(void)
{
    uint32_t old = 1;
    __asm__ volatile(
        "xchgl %0, %1"
        : "=r"(old), "+m"(xhci_lock_flag)
        : "0"(old)
        : "memory");
    return (old == 0) ? 1 : 0;
}

static inline void xhci_unlock(void)
{
    __asm__ volatile("mfence" ::: "memory");
    xhci_lock_flag = 0;
    __asm__ volatile("mfence" ::: "memory");
}

volatile uint32_t xhci_debug_flags = 0;

void xhci_set_debug_flags(uint32_t flags)
{
    xhci_debug_flags = flags;
}

uint32_t xhci_get_debug_flags(void)
{
    return xhci_debug_flags;
}

static inline int xhci_dbg_on(uint32_t f)
{
    return (xhci_debug_flags & f) != 0;
}

void xhci_write64(uint64_t addr, uint64_t val);
uint32_t xhci_read32(volatile uint32_t *reg);
void xhci_write32(volatile uint32_t *reg, uint32_t val);

uint8_t xhci_send_command_wait(uint32_t type, uint64_t param, uint32_t control_bits);
int xhci_control_transfer(uint8_t slot_id, usb_setup_packet_t *setup, void *data);
void xhci_ring_command_doorbell(void);
void xhci_ring_ep_doorbell(uint8_t slot_id, uint8_t endpoint_id);

int xhci_evaluate_context(uint8_t slot_id, uint16_t new_mps, int port_id, int speed_id);
int xhci_get_descriptor_device(uint8_t slot_id, usb_device_descriptor_t *out_desc, uint16_t len);
void *xhci_get_config_descriptor(uint8_t slot_id, uint16_t *out_len);
void xhci_parse_config(void *config_buffer, uint16_t len, usb_device_info_t *info);

int xhci_set_configuration(uint8_t slot_id, uint8_t config_value);
int xhci_set_protocol(uint8_t slot_id, uint8_t interface_num, uint8_t protocol);
int xhci_configure_endpoint_irq(uint8_t slot_id, int port_id, int speed_id, uint8_t ep_addr, uint16_t mps, uint8_t interval);
void xhci_queue_kbd_request(uint8_t slot_id);
void xhci_poll_keyboard_test(uint8_t slot_id);
void xhci_process_events();

static void xhci_ep0_wait_begin(uint8_t slot)
{
    static volatile uint8_t active = 0;
    static volatile uint8_t wait_slot = 0;
    static volatile uint8_t done = 0;
    static volatile uint8_t cc = 0;

    wait_slot = slot;
    done = 0;
    cc = 0;
    active = 1;
}

static int xhci_ep0_wait_is_active_for(uint8_t slot)
{
    static volatile uint8_t active = 0;
    static volatile uint8_t wait_slot = 0;
    static volatile uint8_t done = 0;
    static volatile uint8_t cc = 0;

    (void)done;
    (void)cc;
    return (active && wait_slot == slot);
}

static void xhci_ep0_wait_complete(uint8_t slot, uint8_t cc_value)
{
    static volatile uint8_t active = 0;
    static volatile uint8_t wait_slot = 0;
    static volatile uint8_t done = 0;
    static volatile uint8_t cc = 0;

    if (active && wait_slot == slot)
    {
        cc = cc_value;
        done = 1;
    }
}

static int xhci_ep0_wait_take_result(uint8_t *out_cc)
{
    static volatile uint8_t active = 0;
    static volatile uint8_t wait_slot = 0;
    static volatile uint8_t done = 0;
    static volatile uint8_t cc = 0;

    (void)wait_slot;

    if (!active)
        return 0;
    if (!done)
        return 0;

    if (out_cc)
        *out_cc = cc;

    active = 0;
    done = 0;
    return 1;
}

static volatile int *xhci_kbd_led_pending_ptr(void)
{
    static volatile int pending = 0;
    return &pending;
}

static volatile uint8_t *xhci_kbd_led_slot_ptr(void)
{
    static volatile uint8_t slot = 0;
    return &slot;
}

static volatile uint8_t *xhci_kbd_led_iface_ptr(void)
{
    static volatile uint8_t iface = 0;
    return &iface;
}

static volatile uint8_t *xhci_kbd_led_value_ptr(void)
{
    static volatile uint8_t leds = 0;
    return &leds;
}

static void xhci_kbd_led_schedule(uint8_t slot, uint8_t interface_num, uint8_t leds_bitmap)
{
    volatile int *p_pending = xhci_kbd_led_pending_ptr();
    volatile uint8_t *p_slot = xhci_kbd_led_slot_ptr();
    volatile uint8_t *p_iface = xhci_kbd_led_iface_ptr();
    volatile uint8_t *p_leds = xhci_kbd_led_value_ptr();

    *p_slot = slot;
    *p_iface = interface_num;
    *p_leds = leds_bitmap;

    __asm__ volatile("mfence" ::: "memory");
    *p_pending = 1;
    __asm__ volatile("mfence" ::: "memory");
}

static int xhci_kbd_led_take(uint8_t *out_slot, uint8_t *out_iface, uint8_t *out_leds)
{
    volatile int *p_pending = xhci_kbd_led_pending_ptr();
    volatile uint8_t *p_slot = xhci_kbd_led_slot_ptr();
    volatile uint8_t *p_iface = xhci_kbd_led_iface_ptr();
    volatile uint8_t *p_leds = xhci_kbd_led_value_ptr();

    __asm__ volatile("mfence" ::: "memory");
    if (!(*p_pending))
        return 0;

    *p_pending = 0;
    __asm__ volatile("mfence" ::: "memory");

    if (out_slot)
        *out_slot = *p_slot;
    if (out_iface)
        *out_iface = *p_iface;
    if (out_leds)
        *out_leds = *p_leds;
    return 1;
}

static volatile int *xhci_kbd_recover_pending_ptr(void)
{
    static volatile int pending = 0;
    return &pending;
}

static volatile uint8_t *xhci_kbd_recover_slot_ptr(void)
{
    static volatile uint8_t slot = 0;
    return &slot;
}

static volatile uint8_t *xhci_kbd_recover_ep_id_ptr(void)
{
    static volatile uint8_t ep_id = 0;
    return &ep_id;
}

static void xhci_kbd_recover_schedule(uint8_t slot, uint8_t ep_id)
{
    volatile int *p_pending = xhci_kbd_recover_pending_ptr();
    volatile uint8_t *p_slot = xhci_kbd_recover_slot_ptr();
    volatile uint8_t *p_epid = xhci_kbd_recover_ep_id_ptr();

    *p_slot = slot;
    *p_epid = ep_id;

    __asm__ volatile("mfence" ::: "memory");
    *p_pending = 1;
    __asm__ volatile("mfence" ::: "memory");
}

static int xhci_kbd_recover_take(uint8_t *out_slot, uint8_t *out_ep_id)
{
    volatile int *p_pending = xhci_kbd_recover_pending_ptr();
    volatile uint8_t *p_slot = xhci_kbd_recover_slot_ptr();
    volatile uint8_t *p_epid = xhci_kbd_recover_ep_id_ptr();

    __asm__ volatile("mfence" ::: "memory");
    if (!(*p_pending))
        return 0;

    *p_pending = 0;
    __asm__ volatile("mfence" ::: "memory");

    if (out_slot)
        *out_slot = *p_slot;
    if (out_ep_id)
        *out_ep_id = *p_epid;
    return 1;
}

xhci_controller_t xhci_driver;

#define CONSOLE_COLOR_CYAN 0xFF00FFFF
#define CONSOLE_COLOR_ORANGE 0xFFFFA500
#define XHCI_RING_SIZE 256

#define MK_SLOT_CTX_DW0(entries, speed) \
    (((entries & 0x1F) << 27) | ((speed & 0xF) << 20))

#define MK_SLOT_CTX_DW1(root_port) \
    (((root_port & 0xFF) << 16))

#define MK_EP_CTX_DW0(type, max_packet, interval) \
    (((interval & 0xFF) << 16) | ((type & 0x7) << 3))

#define MK_EP_CTX_DW1(max_packet, error_count, type) \
    (((max_packet & 0xFFFF) << 16) | ((type & 0x7) << 3) | ((error_count & 0x3) << 1))

void xhci_delay(uint32_t count)
{
    for (volatile uint32_t i = 0; i < count * 10000; i++)
    {
        __asm__ volatile("pause");
    }
}

static uint32_t g_diag_counter = 0;

static void xhci_diag_print(const char *where)
{
    g_diag_counter++;

    if (g_diag_counter % 50 != 0)
        return;

    volatile xhci_trb_t *evt = &xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx];
    __asm__ volatile("mfence" ::: "memory");

    uint32_t ctrl = evt->Control;
    uint8_t evt_cycle = ctrl & 1;
    uint8_t our_cycle = xhci_driver.event_ring_cycle_bit;

    console_write_debug("[DIAG:");
    console_write_debug(where);
    console_write_debug("] ev_idx=");
    console_print_dec_debug(xhci_driver.event_ring_dequeue_idx);
    console_write_debug(" ev_cyc=");
    console_print_dec_debug(evt_cycle);
    console_write_debug("/");
    console_print_dec_debug(our_cycle);
    console_write_debug(" lock=");
    console_print_dec_debug(xhci_processing_events);
    console_write_debug("\n");
}

static uint8_t floor_log2_u32(uint32_t x)
{
    uint8_t r = 0;
    while (x > 1)
    {
        x >>= 1;
        r++;
    }
    return r;
}

static uint8_t calc_xhci_interval(uint8_t usb_speed, uint8_t bInterval)
{
    if (bInterval == 0)
        bInterval = 1;

    if (usb_speed == 1 || usb_speed == 2)
    {
        uint32_t period_uf = (uint32_t)bInterval * 8u;

        uint8_t ilog = floor_log2_u32(period_uf);

        if (ilog < 3)
            ilog = 3;
        if (ilog > 15)
            ilog = 15;

        return ilog;
    }

    uint8_t v = bInterval;
    if (v < 1)
        v = 1;
    if (v > 16)
        v = 16;
    return (uint8_t)(v - 1);
}

int xhci_set_configuration(uint8_t slot_id, uint8_t config_value)
{
    usb_setup_packet_t setup;
    setup.RequestType = 0x00;
    setup.Request = 0x09;
    setup.Value = config_value;
    setup.Index = 0;
    setup.Length = 0;

    if (xhci_control_transfer(slot_id, &setup, 0))
    {
        console_write_debug(" -> SetConfig(");
        console_print_dec_debug(config_value);
        console_write_debug(") OK.\n");
        return 1;
    }
    else
    {
        console_write_debug(" -> SetConfig Failed.\n");
        return 0;
    }
}

int xhci_clear_endpoint_halt(uint8_t slot_id, uint8_t ep_addr)
{
    usb_setup_packet_t setup;

    setup.RequestType = 0x02;
    setup.Request = 0x01;
    setup.Value = 0x0000;
    setup.Index = ep_addr;
    setup.Length = 0;

    return xhci_control_transfer(slot_id, &setup, 0);
}

int xhci_set_protocol(uint8_t slot_id, uint8_t interface_num, uint8_t protocol)
{
    usb_setup_packet_t setup;

    setup.RequestType = 0x21;
    setup.Request = USB_REQ_SET_PROTOCOL;
    setup.Value = protocol;
    setup.Index = interface_num;
    setup.Length = 0;

    if (xhci_control_transfer(slot_id, &setup, 0))
    {
        console_write_debug(" -> SetProto(");
        console_print_dec_debug(protocol);
        console_write_debug(") OK.\n");
        return 1;
    }
    else
    {
        console_write_debug(" -> SetProto Failed.\n");
        return 0;
    }
}

int xhci_configure_endpoint_irq(uint8_t slot_id, int port_id, int speed_id,
                                uint8_t ep_addr, uint16_t mps, uint8_t interval)
{
    (void)port_id;

    uint8_t ep_num = ep_addr & 0x0F;
    uint8_t dir_in = (ep_addr & 0x80) ? 1 : 0;
    uint8_t dci = (ep_num == 0) ? 1 : (uint8_t)(2 * ep_num + (dir_in ? 1 : 0));

    xhci_driver.slot_kbd_dci[slot_id] = dci;

    void *ring_phys_ptr = pmm_alloc_contiguous_frames(1);
    if (!ring_phys_ptr)
        return -1;

    uint64_t ring_phys = (uint64_t)ring_phys_ptr;
    uint64_t ring_virt = ring_phys;

    paging_map(ring_virt, ring_phys, PAGE_PRESENT | PAGE_RW);
    __asm__ volatile("invlpg (%0)" ::"r"(ring_virt) : "memory");

    memset((void *)ring_virt, 0, 4096);

    xhci_driver.slot_kbd_rings[slot_id] = (volatile xhci_trb_t *)ring_virt;
    xhci_driver.slot_kbd_enqueue[slot_id] = 0;
    xhci_driver.slot_kbd_cycle[slot_id] = 1;

    xhci_driver.slot_kbd_ring_phys[slot_id] = ring_phys;
    for (int i = 0; i < 256; i++)
    {
        xhci_driver.slot_kbd_trb_buf[slot_id][i] = NULL;
    }

    volatile xhci_trb_t *link = &xhci_driver.slot_kbd_rings[slot_id][255];
    link->Parameter = ring_phys;
    link->Status = 0;

    uint32_t pcs = (xhci_driver.slot_kbd_cycle[slot_id] & 1u);
    link->Control = (TRB_TYPE_LINK << 10) | (1u << 1) | pcs;

    __asm__ __volatile__("clflush (%0)" ::"r"(link) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");

    for (int q = 0; q < XHCI_KBD_PIPE_DEPTH; q++)
    {
        void *buf_phys_ptr = pmm_alloc_contiguous_frames(1);
        if (!buf_phys_ptr)
            return -1;

        uint64_t buf_phys = (uint64_t)buf_phys_ptr;
        uint64_t buf_virt = buf_phys;

        paging_map(buf_virt, buf_phys, PAGE_PRESENT | PAGE_RW);
        __asm__ volatile("invlpg (%0)" ::"r"(buf_virt) : "memory");

        memset((void *)buf_virt, 0, 64);

        xhci_driver.slot_kbd_buffers[slot_id][q] = (uint8_t *)buf_virt;
        xhci_driver.slot_kbd_buffers_phys[slot_id][q] = buf_phys;
    }

    xhci_driver.slot_kbd_buffer[slot_id] = xhci_driver.slot_kbd_buffers[slot_id][0];

    uint8_t interval_log = calc_xhci_interval(speed_id, interval);
    uint8_t ep_type = dir_in ? 7 : 3;

    uint32_t ctx_size = 32;
    if ((xhci_driver.cap_regs->HccParams1 >> 2) & 1)
        ctx_size = 64;

    uint64_t in_bytes = ctx_size * 33;
    if (in_bytes < 4096)
        in_bytes = 4096;

    void *input_ctx = kmalloc_aligned(in_bytes, 4096);
    if (!input_ctx)
        return -1;

    uint64_t inp_phys = paging_get_physical_address((uint64_t)input_ctx);
    paging_map((uint64_t)input_ctx, inp_phys, PAGE_PRESENT | PAGE_RW);
    memset(input_ctx, 0, in_bytes);

    uint32_t *icc = (uint32_t *)input_ctx;

    icc[1] = (1u << dci) | 1u;

    uint64_t out_phys = xhci_driver.dcbaa[slot_id];
    if (out_phys)
    {
        uint64_t out_bytes = ctx_size * 32;
        if (out_bytes < 4096)
            out_bytes = 4096;

        for (uint64_t off = 0; off < out_bytes; off += 4096)
        {
            paging_map(out_phys + off, out_phys + off, PAGE_PRESENT | PAGE_RW);
            __asm__ volatile("invlpg (%0)" ::"r"(out_phys + off) : "memory");
        }

        uint8_t *out_ctx = (uint8_t *)(uint64_t)out_phys;

        uint32_t *slot_ctx = (uint32_t *)((uint8_t *)input_ctx + ctx_size);
        uint32_t *ep0_ctx = (uint32_t *)((uint8_t *)input_ctx + (ctx_size * 2));

        memcpy(slot_ctx, out_ctx + (ctx_size * 0), ctx_size);
        memcpy(ep0_ctx, out_ctx + (ctx_size * 1), ctx_size);

        uint32_t current_entries = (slot_ctx[0] >> 27) & 0x1F;
        if (dci > current_entries)
        {
            slot_ctx[0] &= ~(0x1Fu << 27);
            slot_ctx[0] |= ((uint32_t)dci << 27);
        }

        if (ctx_size == 64)
        {

            for (int i = 8; i < 16; i++)
            {
                slot_ctx[i] = 0;
                ep0_ctx[i] = 0;
            }
        }
    }

    uint32_t *ep_ctx = (uint32_t *)((uint8_t *)input_ctx + (ctx_size * (dci + 1)));

    uint32_t max_esit_payload = (uint32_t)mps;
    uint32_t avg_trb_len = 0x400;

    uint32_t dw0 = ((uint32_t)interval_log << 16);
    dw0 |= ((max_esit_payload >> 16) & 0xFFu) << 24;
    ep_ctx[0] = dw0;

    ep_ctx[1] = ((uint32_t)mps << 16) | ((uint32_t)ep_type << 3) | (3u << 1);

    ep_ctx[2] = (uint32_t)(ring_phys & 0xFFFFFFFF) | 1u;
    ep_ctx[3] = (uint32_t)(ring_phys >> 32);

    ep_ctx[4] = (avg_trb_len & 0xFFFFu) | ((max_esit_payload & 0xFFFFu) << 16);

    for (uint64_t off = 0; off < in_bytes; off += 64)
    {
        __asm__ __volatile__("clflush (%0)" ::"r"((uint8_t *)input_ctx + off) : "memory");
    }
    __asm__ __volatile__("mfence" ::: "memory");

    uint8_t res = xhci_send_command_wait(TRB_TYPE_CONFIG_EP, inp_phys, (slot_id << 24));

    if (res != slot_id)
    {
        console_write_debug("[XHCI] CONFIGURE_ENDPOINT failed.\n");
        return -1;
    }

    console_write_debug("[XHCI] Endpoint configured SUCCESS: slot=");
    console_print_dec_debug(slot_id);
    console_write_debug(" DCI=");
    console_print_dec_debug(dci);
    console_write_debug("\n");

    return 0;
}

void xhci_disable_slot(uint8_t slot_id)
{
    if (slot_id == 0 || slot_id > xhci_driver.max_slots)
        return;

    console_write_debug("[XHCI] Disabling slot ");
    console_print_dec_debug(slot_id);
    console_write_debug("... ");

    if (xhci_driver.slot_ep0_rings[slot_id] != NULL)
    {

        uint8_t res = xhci_send_command_wait(TRB_TYPE_DISABLE_SLOT, 0, (slot_id << 24));
        if (res == 0)
            console_write_debug("(Hardware cmd failed) ");
        else
            console_write_debug("(Hardware disable OK) ");
    }

    xhci_driver.dcbaa[slot_id] = 0;
    __asm__ volatile("sfence" ::: "memory");

    xhci_driver.slot_ep0_rings[slot_id] = NULL;
    xhci_driver.slot_ep0_enqueue[slot_id] = 0;
    xhci_driver.slot_ep0_cycle[slot_id] = 0;

    xhci_driver.slot_kbd_rings[slot_id] = NULL;
    xhci_driver.slot_kbd_enqueue[slot_id] = 0;
    xhci_driver.slot_kbd_cycle[slot_id] = 0;
    xhci_driver.slot_kbd_dci[slot_id] = 0;
    xhci_driver.slot_kbd_pending[slot_id] = 0;
    xhci_driver.slot_kbd_buffer[slot_id] = NULL;

    console_write_debug("done\n");
}

static void xhci_drain_events(void)
{
    int drained = 0;
    int max_drain = 256;

    while (max_drain-- > 0)
    {
        volatile xhci_trb_t *evt = &xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx];

        __asm__ volatile("clflush (%0)" ::"r"(evt));
        __asm__ volatile("mfence" ::: "memory");

        uint32_t ctrl = evt->Control;
        uint8_t evt_cycle = (uint8_t)(ctrl & 1);

        if (evt_cycle != xhci_driver.event_ring_cycle_bit)
        {
            break;
        }

        uint32_t trb_type = (ctrl >> 10) & 0x3F;
        drained++;

        xhci_driver.event_ring_dequeue_idx++;
        if (xhci_driver.event_ring_dequeue_idx >= xhci_driver.event_ring_size)
        {
            xhci_driver.event_ring_dequeue_idx = 0;
            xhci_driver.event_ring_cycle_bit ^= 1;
        }
    }

    if (drained > 0)
    {

        uint64_t erdp_phys = paging_get_physical_address(
            (uint64_t)&xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx]);

        uint64_t ir0 = (uint64_t)xhci_driver.run_regs + 0x20;
        xhci_write64(ir0 + 0x18, erdp_phys | (1ULL << 3));

        xhci_write32(&xhci_driver.op_regs->UsbSts, USBSTS_EINT);
        volatile uint32_t *iman = (volatile uint32_t *)(ir0 + 0x00);
        xhci_write32(iman, xhci_read32(iman) | XHCI_IMAN_IP);

        console_write_debug("[XHCI] Drained ");
        console_print_dec_debug(drained);
        console_write_debug(" stale events\n");
    }
}

static void xhci_write64_split(uint64_t addr, uint64_t val)
{
    volatile uint32_t *lo = (volatile uint32_t *)addr;
    volatile uint32_t *hi = (volatile uint32_t *)(addr + 4);

    *lo = (uint32_t)(val & 0xFFFFFFFF);
    __asm__ volatile("mfence" ::: "memory");
    *hi = (uint32_t)(val >> 32);
    __asm__ volatile("mfence" ::: "memory");
}

static void xhci_busy_wait(uint32_t iterations)
{
    for (volatile uint32_t i = 0; i < iterations; i++)
    {
        __asm__ volatile("pause");
    }
}

static int xhci_recover_command_ring(void)
{
    uint64_t op_base = (uint64_t)xhci_driver.op_regs;

#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38

    uint64_t crcr = xhci_read64(op_base + OP_CRCR);
    uint64_t crcr_addr = crcr & ~0x3FULL;

    uint64_t expected_base = paging_get_physical_address((uint64_t)xhci_driver.cmd_ring_base);

    if (crcr_addr >= expected_base && crcr_addr < (expected_base + 4096))
    {
        return 1;
    }

    console_write_debug("[XHCI] Command ring lost! Full recovery...\n");

    __asm__ volatile("cli");

    uint32_t cmd = xhci_read32((volatile uint32_t *)(op_base + OP_USBCMD));
    xhci_write32((volatile uint32_t *)(op_base + OP_USBCMD), cmd & ~(USBCMD_RUN_STOP | USBCMD_INTE));

    for (int timeout = 10000; timeout > 0; timeout--)
    {
        uint32_t sts = xhci_read32((volatile uint32_t *)(op_base + OP_USBSTS));
        if (sts & USBSTS_HC_HALTED)
            break;
        xhci_busy_wait(10000);
    }

    console_write_debug("[XHCI] HC Reset...\n");

    cmd = xhci_read32((volatile uint32_t *)(op_base + OP_USBCMD));
    xhci_write32((volatile uint32_t *)(op_base + OP_USBCMD), cmd | USBCMD_HC_RESET);

    for (int timeout = 100000; timeout > 0; timeout--)
    {
        cmd = xhci_read32((volatile uint32_t *)(op_base + OP_USBCMD));
        if (!(cmd & USBCMD_HC_RESET))
            break;
        xhci_busy_wait(1000);
    }

    for (int timeout = 100000; timeout > 0; timeout--)
    {
        uint32_t sts = xhci_read32((volatile uint32_t *)(op_base + OP_USBSTS));
        if (!(sts & (1 << 11)))
            break;
        xhci_busy_wait(1000);
    }

    console_write_debug("[XHCI] Reset done\n");

    xhci_busy_wait(100000);

    for (int i = 0; i < 64; i++)
    {
        xhci_driver.dcbaa[i] = 0;
        xhci_driver.slot_ep0_rings[i] = NULL;
        xhci_driver.slot_ep0_enqueue[i] = 0;
        xhci_driver.slot_ep0_cycle[i] = 0;
        xhci_driver.slot_kbd_rings[i] = NULL;
        xhci_driver.slot_kbd_enqueue[i] = 0;
        xhci_driver.slot_kbd_cycle[i] = 0;
        xhci_driver.slot_kbd_dci[i] = 0;
        xhci_driver.slot_kbd_pending[i] = 0;
    }
    __asm__ volatile("sfence" ::: "memory");

    xhci_driver.cmd_ring_enqueue_idx = 0;
    xhci_driver.cmd_ring_cycle_bit = 1;

    memset(xhci_driver.cmd_ring_base, 0, 4096);

    xhci_trb_t *link = &xhci_driver.cmd_ring_base[xhci_driver.cmd_ring_size - 1];
    link->Parameter = expected_base;
    link->Status = 0;
    link->Control = (TRB_TYPE_LINK << 10) | (1 << 1) | 1;

    for (uint64_t off = 0; off < 4096; off += 64)
        __asm__ volatile("clflush (%0)" ::"r"((uint64_t)xhci_driver.cmd_ring_base + off));
    __asm__ volatile("mfence" ::: "memory");

    xhci_driver.event_ring_dequeue_idx = 0;
    xhci_driver.event_ring_cycle_bit = 1;

    memset(xhci_driver.event_ring, 0, xhci_driver.event_ring_size * sizeof(xhci_trb_t));

    for (uint64_t off = 0; off < xhci_driver.event_ring_size * sizeof(xhci_trb_t); off += 64)
        __asm__ volatile("clflush (%0)" ::"r"((uint64_t)xhci_driver.event_ring + off));
    __asm__ volatile("mfence" ::: "memory");

    xhci_write32((volatile uint32_t *)(op_base + OP_CONFIG), xhci_driver.max_slots);

    uint64_t dcbaap_phys = paging_get_physical_address((uint64_t)xhci_driver.dcbaa);
    xhci_write64_split(op_base + OP_DCBAAP, dcbaap_phys);

    uint64_t ir0 = (uint64_t)xhci_driver.run_regs + 0x20;

    *(volatile uint32_t *)(ir0 + 0x08) = 1;

    uint64_t ev_ring_phys = paging_get_physical_address((uint64_t)xhci_driver.event_ring);
    xhci_driver.erst[0].BaseAddress = ev_ring_phys;
    xhci_driver.erst[0].Size = xhci_driver.event_ring_size;
    __asm__ volatile("clflush (%0)" ::"r"(xhci_driver.erst));
    __asm__ volatile("mfence" ::: "memory");

    uint64_t erst_phys = paging_get_physical_address((uint64_t)xhci_driver.erst);
    xhci_write64_split(ir0 + 0x10, erst_phys);
    xhci_write64_split(ir0 + 0x18, ev_ring_phys | (1ULL << 3));

    *(volatile uint32_t *)(ir0 + 0x00) = XHCI_IMAN_IP | XHCI_IMAN_IE;

    uint64_t crcr_val = (expected_base & ~0x3FULL) | 1ULL;
    xhci_write64_split(op_base + OP_CRCR, crcr_val);
    __asm__ volatile("mfence" ::: "memory");

    console_write_debug("[XHCI] Starting...\n");

    cmd = xhci_read32((volatile uint32_t *)(op_base + OP_USBCMD));
    xhci_write32((volatile uint32_t *)(op_base + OP_USBCMD), cmd | USBCMD_RUN_STOP | USBCMD_INTE);

    for (int timeout = 10000; timeout > 0; timeout--)
    {
        uint32_t sts = xhci_read32((volatile uint32_t *)(op_base + OP_USBSTS));
        if (!(sts & USBSTS_HC_HALTED))
            break;
        xhci_busy_wait(10000);
    }

    xhci_busy_wait(500000);

    __asm__ volatile("sti");

    xhci_driver.event_ring_dequeue_idx = 0;
    xhci_driver.event_ring_cycle_bit = 1;

    console_write_debug("[XHCI] Recovery complete!\n");
    return 1;
}

void xhci_hotplug_enumerate_port(uint8_t port_0based)
{
    if (port_0based >= xhci_driver.max_ports)
        return;

    xhci_drain_events();

    uint32_t status = xhci_read32(&xhci_driver.op_regs->UsbSts);
    if (status & USBSTS_HC_HALTED)
    {
        console_write_debug("[XHCI][HOTPLUG] Controller HALTED! Recovery needed.\n");
        if (!xhci_recover_command_ring())
            return;
    }

    volatile xhci_port_regs_t *port = &xhci_driver.port_regs[port_0based];

    const uint32_t CCS = (1u << 0);
    const uint32_t PED = (1u << 1);
    const uint32_t PORT_POWER = (1u << 9);
    const uint32_t SPEED_MASK = (0xFu << 10);
    const uint32_t PR = (1u << 4);
    const uint32_t PRC = (1u << 21);
    const uint32_t CHG_BITS = (1u << 17) | (1u << 18) | (1u << 19) |
                              (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23);

    timer_sleep(150);

    uint32_t sc = port->PortSC;
    if ((sc & CCS) == 0)
        return;

    port->PortSC = (sc & PORT_POWER) | CHG_BITS;
    (void)port->PortSC;
    timer_sleep(10);

    sc = port->PortSC;

    if (sc & PED)
    {
        uint8_t speed = (uint8_t)((sc & SPEED_MASK) >> 10);
        xhci_configure_device(port_0based, speed);
        return;
    }

    console_write_debug("[XHCI][HOTPLUG] Resetting port ");
    console_print_dec_debug(port_0based + 1);
    console_write_debug("... ");

    port->PortSC = (sc & PORT_POWER) | PR;
    (void)port->PortSC;

    int ok = 0;
    for (int i = 0; i < 100; i++)
    {
        timer_sleep(10);
        sc = port->PortSC;
        if ((sc & PRC) || (sc & PED))
        {
            ok = 1;
            break;
        }
    }

    if (!ok)
        console_write_debug("timeout! ");
    else
        console_write_debug("OK! ");

    port->PortSC = (sc & PORT_POWER) | CHG_BITS;
    (void)port->PortSC;
    timer_sleep(30);

    sc = port->PortSC;
    if ((sc & PED) && (sc & CCS))
    {
        uint8_t speed = (uint8_t)((sc & SPEED_MASK) >> 10);
        console_write_debug("Configuring (speed=");
        console_print_dec_debug(speed);
        console_write_debug(")...\n");
        xhci_configure_device(port_0based, speed);
    }
    else
    {
        console_write_debug("Failed to enable port.\n");
    }
}

extern volatile int g_xhci_need_bh;

void xhci_bottom_half(void)
{
    if (!g_xhci_need_bh)
        return;

    g_xhci_need_bh = 0;

    xhci_process_events();

    usb_hotplug_process_pending();

    keyboard_usb_pump_to_shell(256);
}

static void xhci_queue_kbd_request_buf(uint8_t slot, uint8_t *buf)
{
    volatile xhci_trb_t *ring = xhci_driver.slot_kbd_rings[slot];
    if (!ring || !buf)
        return;

    uint8_t dci = xhci_driver.slot_kbd_dci[slot];
    if (dci == 0)
        return;

    uint64_t buf_phys = 0;
    for (int q = 0; q < XHCI_KBD_PIPE_DEPTH; q++)
    {
        if (xhci_driver.slot_kbd_buffers[slot][q] == buf)
        {
            buf_phys = xhci_driver.slot_kbd_buffers_phys[slot][q];
            break;
        }
    }

    if (buf_phys == 0)
    {
        buf_phys = paging_get_physical_address((uint64_t)buf);
    }

    uint32_t idx = xhci_driver.slot_kbd_enqueue[slot];
    uint32_t cycle = xhci_driver.slot_kbd_cycle[slot] & 1;

    uint32_t inflight_before = xhci_driver.slot_kbd_pending[slot];
    int need_db = (inflight_before <= 1);

    if (idx >= 255)
    {
        uint64_t ring_phys = xhci_driver.slot_kbd_ring_phys[slot];

        uint32_t old_cycle = cycle;

        xhci_driver.slot_kbd_cycle[slot] ^= 1;
        cycle = xhci_driver.slot_kbd_cycle[slot] & 1;
        idx = 0;

        xhci_driver.slot_kbd_trb_buf[slot][0] = buf;

        ring[0].Parameter = buf_phys;
        ring[0].Status = (8u << 0);

        ring[0].Control = (TRB_TYPE_NORMAL << 10) | (1u << 5) | (cycle & 1u);

        ring[255].Parameter = ring_phys;
        ring[255].Status = 0;
        ring[255].Control = (TRB_TYPE_LINK << 10) | (1u << 1) | (old_cycle & 1u);

        xhci_driver.slot_kbd_trb_buf[slot][255] = NULL;
        xhci_driver.slot_kbd_enqueue[slot] = 1;

        xhci_driver.slot_kbd_pending[slot] = inflight_before + 1;

        __asm__ volatile("sfence" ::: "memory");

        if (need_db)
        {
            xhci_ring_ep_doorbell(slot, dci);
            (void)xhci_read32(&xhci_driver.op_regs->UsbSts);
        }

        return;
    }

    xhci_driver.slot_kbd_trb_buf[slot][idx] = buf;

    ring[idx].Parameter = buf_phys;
    ring[idx].Status = (8u << 0);

    ring[idx].Control = (TRB_TYPE_NORMAL << 10) | (1u << 5) | (cycle & 1u);

    xhci_driver.slot_kbd_enqueue[slot] = idx + 1;

    xhci_driver.slot_kbd_pending[slot] = inflight_before + 1;

    __asm__ volatile("sfence" ::: "memory");

    if (need_db)
    {
        xhci_ring_ep_doorbell(slot, dci);
        (void)xhci_read32(&xhci_driver.op_regs->UsbSts);
    }
}

void xhci_queue_kbd_request(uint8_t slot_id)
{
    xhci_queue_kbd_request_buf(slot_id, xhci_driver.slot_kbd_buffer[slot_id]);
}

int xhci_set_leds(uint8_t slot_id, uint8_t interface_num, uint8_t leds_bitmap)
{

    if (!xhci_driver.slot_led_buffer[slot_id])
    {
        uint8_t *buf = kmalloc_aligned(64, 64);
        if (!buf)
            return 0;

        uint64_t buf_phys = paging_get_physical_address((uint64_t)buf);
        paging_map((uint64_t)buf, buf_phys, PAGE_PRESENT | PAGE_RW);

        xhci_driver.slot_led_buffer[slot_id] = buf;
    }

    uint8_t *buf = xhci_driver.slot_led_buffer[slot_id];
    *buf = leds_bitmap;

    __asm__ volatile("clflush (%0)" ::"r"(buf));
    __asm__ volatile("mfence" ::: "memory");

    usb_setup_packet_t setup;
    setup.RequestType = 0x21;
    setup.Request = 0x09;
    setup.Value = (2 << 8) | 0;
    setup.Index = interface_num;
    setup.Length = 1;

    return xhci_control_transfer(slot_id, &setup, buf);
}

static uint8_t caps_lock_state = 0;

static uint8_t kbd_led_state = 0;

void xhci_poll_keyboard_test(uint8_t ignored_slot_id)
{
    (void)ignored_slot_id;

    volatile xhci_trb_t *evt = &xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx];

    if ((evt->Control & 1) == xhci_driver.event_ring_cycle_bit)
    {
        __asm__ volatile("mfence" ::: "memory");

        uint32_t type = (evt->Control >> 10) & 0x3F;
        uint8_t slot = (evt->Control >> 24) & 0xFF;

        if (type == TRB_TYPE_TRANSFER_EVENT && xhci_driver.slot_kbd_buffer[slot] != NULL)
        {

            uint8_t *buf = xhci_driver.slot_kbd_buffer[slot];

            uint8_t modifiers = buf[0];
            if (modifiers != 0)
            {
                console_set_color_debug(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
                console_write_debug("[MOD] Slot ");
                console_print_dec_debug(slot);
                console_write_debug(": ");

                if (modifiers & (1 << 0))
                    console_write_debug("L-CTRL ");
                if (modifiers & (1 << 1))
                    console_write_debug("L-SHIFT ");
                if (modifiers & (1 << 2))
                    console_write_debug("L-ALT ");
                if (modifiers & (1 << 3))
                    console_write_debug("L-WIN ");
                if (modifiers & (1 << 4))
                    console_write_debug("R-CTRL ");
                if (modifiers & (1 << 5))
                    console_write_debug("R-SHIFT ");
                if (modifiers & (1 << 6))
                    console_write_debug("R-ALT ");
                console_write_debug("\n");
            }

            uint8_t keycode = buf[2];
            if (keycode != 0)
            {
                console_set_color_debug(CONSOLE_COLOR_ORANGE, CONSOLE_COLOR_BLACK);
                console_write_debug("[KEY] Slot ");
                console_print_dec_debug(slot);
                console_write_debug(" Code: 0x");
                console_print_hex_debug(keycode);
                console_write_debug("\n");

                static uint8_t leds = 0;
                if (keycode == 0x53)
                    leds ^= 1;
                if (keycode == 0x39)
                    leds ^= 2;
                if (keycode == 0x47)
                    leds ^= 4;

                if (keycode == 0x53 || keycode == 0x39 || keycode == 0x47)
                {
                    xhci_set_leds(slot, 0, leds);
                    console_write_debug(" -> LED Toggled!\n");
                }
            }

            console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

            xhci_queue_kbd_request(slot);
        }

        xhci_driver.event_ring_dequeue_idx++;
        if (xhci_driver.event_ring_dequeue_idx == xhci_driver.event_ring_size)
        {
            xhci_driver.event_ring_dequeue_idx = 0;
            xhci_driver.event_ring_cycle_bit ^= 1;
        }

        uint64_t erdp = paging_get_physical_address((uint64_t)&xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx]);
        xhci_write64((uint64_t)&xhci_driver.run_regs->Interrupters[0].Erdp, erdp | (1 << 3));
    }
}

void xhci_configure_device(int port_id, int speed_id)
{
    console_write_debug("[XHCI-DBG] configure_device: port=");
    console_print_dec_debug(port_id);
    console_write_debug(" speed=");
    console_print_dec_debug(speed_id);
    console_write_debug("\n");

    int found_sync = 0;
    for (uint64_t i = 0; i < xhci_driver.event_ring_size; i++)
    {
        volatile xhci_trb_t *evt = &xhci_driver.event_ring[i];
        uint8_t cycle = evt->Control & 1;

        if (i > 0)
        {
            volatile xhci_trb_t *prev = &xhci_driver.event_ring[i - 1];
            uint8_t prev_cycle = prev->Control & 1;
            if (prev_cycle == 1 && cycle == 0)
            {
                xhci_driver.event_ring_dequeue_idx = i;
                xhci_driver.event_ring_cycle_bit = 1;
                found_sync = 1;
                break;
            }
        }
    }

    if (!found_sync)
    {

        xhci_driver.event_ring_dequeue_idx = 0;
        xhci_driver.event_ring_cycle_bit = 1;
    }

    console_write_debug("[XHCI-DBG] EVT synced to idx=");
    console_print_dec_debug(xhci_driver.event_ring_dequeue_idx);
    console_write_debug("\n");

    console_write_debug("[XHCI-DBG] CMD Ring: enq_idx=");
    console_print_dec_debug(xhci_driver.cmd_ring_enqueue_idx);
    console_write_debug(" cycle=");
    console_print_dec_debug(xhci_driver.cmd_ring_cycle_bit);
    console_write_debug("\n");

    console_write_debug("[XHCI-DBG] EVT Ring: deq_idx=");
    console_print_dec_debug(xhci_driver.event_ring_dequeue_idx);
    console_write_debug(" cycle=");
    console_print_dec_debug(xhci_driver.event_ring_cycle_bit);
    console_write_debug("\n");

    uint32_t sts = xhci_read32(&xhci_driver.op_regs->UsbSts);
    uint32_t cmd = xhci_read32(&xhci_driver.op_regs->UsbCmd);
    console_write_debug("[XHCI-DBG] STS=0x");
    console_print_hex_debug(sts);
    console_write_debug(" CMD=0x");
    console_print_hex_debug(cmd);
    console_write_debug("\n");

    if (sts & USBSTS_HC_HALTED)
    {
        console_write_debug("[XHCI-DBG] ERROR: Controller is HALTED!\n");
        return;
    }

    console_write_debug("[XHCI-DBG] Sending NOOP...\n");
    if (xhci_send_command_wait(TRB_TYPE_NOOP, 0, 0) == 0)
    {
        console_write_debug("[XHCI-DBG] NOOP FAILED!\n");
        return;
    }
    console_write_debug("[XHCI-DBG] NOOP OK\n");

    console_write_debug("[XHCI-DBG] Sending ENABLE_SLOT...\n");
    uint8_t slot_id = xhci_send_command_wait(TRB_TYPE_ENABLE_SLOT, 0, 0);
    if (slot_id == 0)
    {
        console_write_debug("[XHCI-DBG] ENABLE_SLOT FAILED!\n");
        return;
    }
    console_write_debug("[XHCI-DBG] ENABLE_SLOT OK, slot=");
    console_print_dec_debug(slot_id);
    console_write_debug("\n");

    console_write_debug(" [XHCI] Device at Slot ");
    console_print_dec_debug(slot_id);

    uint32_t hcc1 = xhci_driver.cap_regs->HccParams1;
    uint32_t ctx_size = ((hcc1 >> 2) & 1) ? 64 : 32;

    uint64_t out_bytes = ctx_size * 32;
    if (out_bytes < 4096)
        out_bytes = 4096;
    void *dev_ctx = kmalloc_aligned(out_bytes, 4096);
    uint64_t dev_phys = paging_get_physical_address((uint64_t)dev_ctx);
    paging_map((uint64_t)dev_ctx, dev_phys, PAGE_PRESENT | PAGE_RW);
    memset(dev_ctx, 0, out_bytes);
    xhci_driver.dcbaa[slot_id] = dev_phys;
    __asm__ volatile("clflush (%0)" ::"r"(&xhci_driver.dcbaa[slot_id]));
    __asm__ volatile("mfence" ::: "memory");

    uint64_t in_bytes = ctx_size * 33;
    if (in_bytes < 4096)
        in_bytes = 4096;
    void *input_ctx = kmalloc_aligned(in_bytes, 4096);
    uint64_t inp_phys = paging_get_physical_address((uint64_t)input_ctx);
    paging_map((uint64_t)input_ctx, inp_phys, PAGE_PRESENT | PAGE_RW);
    memset(input_ctx, 0, in_bytes);

    uint32_t *icc_add = (uint32_t *)((uint8_t *)input_ctx + 4);
    *icc_add = (1u << 0) | (1u << 1);

    uint32_t *slot_dw = (uint32_t *)((uint8_t *)input_ctx + ctx_size);
    uint32_t *ep0_dw = (uint32_t *)((uint8_t *)input_ctx + (ctx_size * 2));

    slot_dw[0] = ((1u & 0x1Fu) << 27) | (((uint32_t)speed_id & 0xFu) << 20);
    slot_dw[1] = (((uint32_t)((port_id + 1) & 0xFF)) << 16);

    void *tr_ring = kmalloc_aligned(4096, 4096);
    uint64_t tr_phys = paging_get_physical_address((uint64_t)tr_ring);
    paging_map((uint64_t)tr_ring, tr_phys, PAGE_PRESENT | PAGE_RW);
    memset(tr_ring, 0, 4096);

    xhci_trb_t *ring = (xhci_trb_t *)tr_ring;
    ring[255].Parameter = tr_phys;
    ring[255].Status = 0;
    ring[255].Control = (TRB_TYPE_LINK << 10) | (1 << 1) | 1;

    uint16_t initial_mps = (speed_id <= 2) ? 8 : 64;
    ep0_dw[1] = (initial_mps << 16) | (4u << 3) | (3u << 1);
    ep0_dw[2] = (uint32_t)tr_phys | 1u;
    ep0_dw[3] = (uint32_t)(tr_phys >> 32);
    ep0_dw[4] = 8;

    for (uint64_t off = 0; off < in_bytes; off += 64)
        __asm__ volatile("clflush (%0)" ::"r"((uint64_t)input_ctx + off));
    __asm__ volatile("mfence" ::: "memory");

    if (speed_id <= 3)
        timer_sleep(20);

    console_write_debug("[XHCI-DBG] Sending ADDRESS_DEVICE...\n");
    uint8_t res = xhci_send_command_wait(TRB_TYPE_ADDRESS_DEVICE, inp_phys, (slot_id << 24));
    if (res != slot_id)
    {
        console_write_debug("-> Addr Fail.\n");
        return;
    }
    console_write_debug("[XHCI-DBG] ADDRESS_DEVICE OK\n");

    xhci_driver.slot_ep0_rings[slot_id] = (xhci_trb_t *)tr_ring;
    xhci_driver.slot_ep0_enqueue[slot_id] = 0;
    xhci_driver.slot_ep0_cycle[slot_id] = 1;

    usb_hotplug_notify_root_device_configured((uint8_t)(port_id + 1), slot_id);

    if (speed_id < 3)
    {
        usb_device_descriptor_t desc_short;
        if (xhci_get_descriptor_device(slot_id, &desc_short, 8))
        {
            int real_mps = desc_short.MaxPacketSize0;
            if (real_mps > 0 && real_mps != initial_mps)
            {
                xhci_evaluate_context(slot_id, (uint16_t)real_mps, port_id, speed_id);
            }
        }
    }

    usb_device_descriptor_t desc;
    if (xhci_get_descriptor_device(slot_id, &desc, 18))
    {
        console_write_debug(" (VID=");
        console_print_hex_debug(desc.VendorID);
        console_write_debug(" PID=");
        console_print_hex_debug(desc.ProductID);
        console_write_debug(" Class=");
        console_print_hex_debug(desc.DeviceClass);
        console_write_debug(")");

        if (desc.DeviceClass == USB_CLASS_HUB)
        {
            console_set_color_debug(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
            console_write_debug("\n[XHCI] USB HUB detected on root port!\n");
            console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

            int is_usb3 = (desc.DeviceProtocol >= 3) || (speed_id >= 4);

            usb_hub_enumerate(
                slot_id,
                (uint8_t)(port_id + 1),
                0,
                0,
                0,
                0,
                is_usb3);

            return;
        }

        uint16_t conf_len = 0;
        void *conf_buf = xhci_get_config_descriptor(slot_id, &conf_len);

        if (conf_buf)
        {

            usb_device_info_t info;
            memset(&info, 0, sizeof(info));
            xhci_parse_config(conf_buf, conf_len, &info);

            if (info.found)
            {
                console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
                console_write_debug(" -> KEYBOARD DETECTED.\n");
                console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

                if (xhci_set_configuration(slot_id, info.config_value))
                {
                    console_write_debug("[XHCI][KBD] Using parsed endpoint: IF=");
                    console_print_dec_debug(info.interface_num);
                    console_write_debug(" EP=0x");
                    console_print_hex_debug(info.endpoint_addr);
                    console_write_debug(" MPS=");
                    console_print_dec_debug(info.endpoint_mps);
                    console_write_debug(" INT=");
                    console_print_dec_debug(info.endpoint_interval);
                    console_write_debug("\n");

                    xhci_set_protocol(slot_id, info.interface_num, 0);
                    xhci_set_idle(slot_id, info.interface_num, 0, 0);

                    int result = xhci_configure_endpoint_irq(
                        slot_id,
                        port_id,
                        speed_id,
                        info.endpoint_addr,
                        info.endpoint_mps,
                        info.endpoint_interval);

                    if (result == 0)
                    {
                        console_write_debug("[XHCI] Endpoint configured successfully!\n");

                        xhci_driver.slot_kbd_pending[slot_id] = 0;

                        for (int q = 0; q < XHCI_KBD_PIPE_DEPTH; q++)
                        {
                            xhci_queue_kbd_request_buf(slot_id, xhci_driver.slot_kbd_buffers[slot_id][q]);
                        }

                        console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
                        console_write_debug("[XHCI] Keyboard ready! Press keys to test.\n");
                        console_write_debug("       Slot=");
                        console_print_dec_debug(slot_id);
                        console_write_debug(" DCI=");
                        console_print_dec_debug(xhci_driver.slot_kbd_dci[slot_id]);
                        console_write_debug("\n");
                        console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
                    }
                }
            }
            else
            {
                console_write_debug(" [Not a keyboard]\n");
            }
        }
    }
    else
    {
        console_write_debug(" (Desc Fail)\n");
    }
}

uint32_t xhci_read32(volatile uint32_t *reg)
{
    return *reg;
}

void xhci_write32(volatile uint32_t *reg, uint32_t val)
{
    *reg = val;
}

void xhci_write64(uint64_t addr, uint64_t val)
{
    *(volatile uint64_t *)addr = val;
}

uint64_t xhci_read64(uint64_t addr)
{
    return *(volatile uint64_t *)addr;
}

static inline void xhci_port_clear_changes(volatile xhci_port_regs_t *port, uint32_t mask)
{

    port->PortSC = port->PortSC | mask;
    (void)port->PortSC;
}

static void xhci_build_port_protocol_lists(
    uint8_t *proto_major,
    uint8_t *usb2_ports, int *usb2_n,
    uint8_t *usb3_ports, int *usb3_n)
{
    memset(proto_major, 0, 256);
    *usb2_n = 0;
    *usb3_n = 0;

    uint32_t hcc = xhci_driver.cap_regs->HccParams1;
    uint32_t xecp = (hcc >> 16) & 0xFFFF;
    if (xecp == 0)
        return;

    uint64_t cap_addr = xhci_driver.pci_base_address + ((uint64_t)xecp * 4);

    while (1)
    {
        volatile uint32_t *dw0p = (volatile uint32_t *)cap_addr;
        uint32_t dw0 = *dw0p;

        uint8_t cap_id = (uint8_t)(dw0 & 0xFF);
        uint8_t next = (uint8_t)((dw0 >> 8) & 0xFF);

        if (cap_id == 2)
        {

            uint8_t major = (uint8_t)((dw0 >> 24) & 0xFF);

            volatile uint32_t *dw2p = (volatile uint32_t *)(cap_addr + 8);
            uint32_t dw2 = *dw2p;

            uint8_t port_off_1based = (uint8_t)(dw2 & 0xFF);
            uint8_t port_count = (uint8_t)((dw2 >> 8) & 0xFF);

            if (port_off_1based != 0 && port_count != 0)
            {
                uint8_t start = (uint8_t)(port_off_1based - 1);
                for (uint8_t i = 0; i < port_count; i++)
                {
                    uint8_t p = (uint8_t)(start + i);
                    if (p >= xhci_driver.max_ports)
                        break;

                    proto_major[p] = major;

                    if (major == 2 && *usb2_n < 256)
                        usb2_ports[(*usb2_n)++] = p;
                    if (major == 3 && *usb3_n < 256)
                        usb3_ports[(*usb3_n)++] = p;
                }
            }
        }

        if (next == 0)
            break;
        cap_addr += ((uint64_t)next * 4);
    }
}

int xhci_port_reset_hs_style(volatile xhci_port_regs_t *port, int port_index, int is_usb3)
{
    const uint32_t PORT_POWER = (1u << 9);

    const uint32_t CHG = (1u << 17) | (1u << 18) | (1u << 20) | (1u << 21) | (1u << 22);

    const uint32_t CCS = (1u << 0);
    const uint32_t PED = (1u << 1);

    uint32_t sc = port->PortSC;
    if ((sc & CCS) == 0)
        return 0;

    if ((sc & PORT_POWER) == 0)
    {
        port->PortSC = PORT_POWER;
        (void)port->PortSC;
        timer_sleep(20);
        sc = port->PortSC;
        if ((sc & PORT_POWER) == 0)
            return 0;
    }

    port->PortSC = PORT_POWER | CHG;
    (void)port->PortSC;

    uint32_t resetBit = is_usb3 ? (1u << 31) : (1u << 4);
    uint32_t resetChange = is_usb3 ? (1u << 19) : (1u << 21);

    port->PortSC = PORT_POWER | resetBit;
    (void)port->PortSC;

    int timeout = 500;
    while (timeout-- > 0)
    {
        if (port->PortSC & resetChange)
            break;
        timer_sleep(1);
    }
    if (timeout <= 0)
        return 0;

    timer_sleep(is_usb3 ? 20 : 50);

    sc = port->PortSC;
    if (sc & PED)
    {

        port->PortSC = PORT_POWER | CHG;
        (void)port->PortSC;
        return 1;
    }

    return 0;
}

void xhci_bios_handoff(uint64_t mmio_base)
{
    console_write_debug("[XHCI] Checking for BIOS Handoff...\n");
    xhci_cap_regs_t *caps = (xhci_cap_regs_t *)mmio_base;
    uint32_t hccparams1 = caps->HccParams1;
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFF;

    if (xecp == 0)
    {
        console_write_debug(" -> No Extended Capabilities found.\n");
        return;
    }

    uint64_t cap_addr = mmio_base + (xecp * 4);

    while (1)
    {
        volatile uint32_t *cap_reg = (volatile uint32_t *)cap_addr;
        uint32_t cap_val = *cap_reg;
        uint8_t cap_id = cap_val & 0xFF;

        if (cap_id == 1)
        {
            if (cap_val & (1 << 16))
            {
                *cap_reg |= (1 << 24);
                int timeout = 100;
                while ((*cap_reg & (1 << 16)) && timeout > 0)
                {
                    timer_sleep(10);
                    timeout--;
                }
                if (*cap_reg & (1 << 16))
                    console_write_debug(" -> BIOS Refused Handoff.\n");
                else
                    console_write_debug(" -> BIOS Handoff Success.\n");
            }
            else
            {
                *cap_reg |= (1 << 24);
                console_write_debug(" -> BIOS Handoff Clean.\n");
            }

            volatile uint32_t *leg_ctl = (volatile uint32_t *)(cap_addr + 4);
            *leg_ctl &= 0xFFFF0000;
            return;
        }

        uint8_t next = (cap_val >> 8) & 0xFF;
        if (next == 0)
            break;
        cap_addr += (next * 4);
    }
}

void xhci_alloc_dcbaa()
{
    console_write_debug("[XHCI] Allocating DCBAA... ");

    uint64_t size = (xhci_driver.max_slots + 1) * sizeof(uint64_t);
    void *dcbaa_virt = kmalloc_aligned(size, 4096);
    uint64_t dcbaa_phys = paging_get_physical_address((uint64_t)dcbaa_virt);

    const uint64_t dma_flags = PAGE_PRESENT | PAGE_RW;
    paging_map((uint64_t)dcbaa_virt, dcbaa_phys, dma_flags);
    __asm__ volatile("invlpg (%0)" ::"r"(dcbaa_virt) : "memory");

    memset(dcbaa_virt, 0, size);

    xhci_driver.dcbaa = (uint64_t *)dcbaa_virt;

    uint32_t hcs2 = xhci_driver.cap_regs->HcsParams2;
    uint32_t sp_hi = (hcs2 >> 27) & 0x1F;
    uint32_t sp_lo = (hcs2 >> 21) & 0x1F;
    uint32_t sp_count = (sp_hi << 5) | sp_lo;

    if (sp_count > 0)
    {
        console_write_debug("\n[XHCI] Scratchpad Buffers Required: ");
        console_print_dec_debug(sp_count);
        console_write_debug("\n");

        uint64_t sp_array_bytes = (uint64_t)sp_count * sizeof(uint64_t);
        void *sp_array_virt = kmalloc_aligned(sp_array_bytes, 64);
        uint64_t sp_array_phys = paging_get_physical_address((uint64_t)sp_array_virt);

        paging_map((uint64_t)sp_array_virt, sp_array_phys, dma_flags);
        __asm__ volatile("invlpg (%0)" ::"r"(sp_array_virt) : "memory");

        memset(sp_array_virt, 0, sp_array_bytes);

        for (uint32_t i = 0; i < sp_count; i++)
        {
            void *buf_virt = kmalloc_aligned(4096, 4096);
            uint64_t buf_phys = paging_get_physical_address((uint64_t)buf_virt);

            paging_map((uint64_t)buf_virt, buf_phys, dma_flags);
            __asm__ volatile("invlpg (%0)" ::"r"(buf_virt) : "memory");

            memset(buf_virt, 0, 4096);

            ((uint64_t *)sp_array_virt)[i] = buf_phys;

            for (uint64_t off = 0; off < 4096; off += 64)
            {
                __asm__ volatile("clflush (%0)" ::"r"((uint64_t)buf_virt + off));
            }
        }

        for (uint64_t off = 0; off < sp_array_bytes; off += 64)
        {
            __asm__ volatile("clflush (%0)" ::"r"((uint64_t)sp_array_virt + off));
        }

        __asm__ volatile("mfence" ::: "memory");

        xhci_driver.dcbaa[0] = sp_array_phys;

        __asm__ volatile("clflush (%0)" ::"r"(&xhci_driver.dcbaa[0]));
        __asm__ volatile("mfence" ::: "memory");
    }

    for (uint64_t off = 0; off < size; off += 64)
    {
        __asm__ volatile("clflush (%0)" ::"r"((uint64_t)dcbaa_virt + off));
    }
    __asm__ volatile("mfence" ::: "memory");

    xhci_write64((uint64_t)&xhci_driver.op_regs->Dcbaap, (dcbaa_phys & ~0x3FULL));
    (void)xhci_read64((uint64_t)&xhci_driver.op_regs->Dcbaap);

    console_write_debug("OK.\n");
}

void xhci_init_command_ring()
{
    console_write_debug("[XHCI] Initializing Command Ring... ");

    xhci_driver.cmd_ring_size = 32;

    uint64_t size = xhci_driver.cmd_ring_size * sizeof(xhci_trb_t);
    void *ptr = kmalloc_aligned(size, 4096);
    uint64_t phys = paging_get_physical_address((uint64_t)ptr);

    paging_map((uint64_t)ptr, phys, PAGE_PRESENT | PAGE_RW);
    __asm__ volatile("invlpg (%0)" ::"r"(ptr) : "memory");
    memset(ptr, 0, size);

    xhci_driver.cmd_ring_base = (xhci_trb_t *)ptr;
    xhci_driver.cmd_ring_enqueue_idx = 0;
    xhci_driver.cmd_ring_cycle_bit = 1;

    xhci_trb_t *link = &xhci_driver.cmd_ring_base[xhci_driver.cmd_ring_size - 1];
    link->Parameter = phys;
    link->Status = 0;
    link->Control = (TRB_TYPE_LINK << 10) | (1 << 1) | (xhci_driver.cmd_ring_cycle_bit & 1);

    for (uint64_t off = 0; off < size; off += 64)
    {
        __asm__ volatile("clflush (%0)" ::"r"((uint64_t)ptr + off));
    }
    __asm__ volatile("mfence" ::: "memory");

    uint64_t crcr_val = (phys & ~0x3FULL) | 1ULL;
    xhci_write64((uint64_t)&xhci_driver.op_regs->Crcr, crcr_val);

    (void)xhci_read64((uint64_t)&xhci_driver.op_regs->Crcr);

    __asm__ volatile("mfence" ::: "memory");

    console_write_debug("OK (RCS=1).\n");
}

void xhci_init_event_ring()
{
    console_write_debug("[XHCI] Initializing Event Ring... ");

    xhci_driver.event_ring_size = 256;
    uint64_t ring_bytes = xhci_driver.event_ring_size * sizeof(xhci_trb_t);

    const uint64_t dma_flags = PAGE_PRESENT | PAGE_RW;

    void *ring_virt = kmalloc_aligned(ring_bytes, 4096);
    uint64_t ring_phys = paging_get_physical_address((uint64_t)ring_virt);
    paging_map((uint64_t)ring_virt, ring_phys, dma_flags);
    __asm__ volatile("invlpg (%0)" ::"r"(ring_virt) : "memory");
    memset(ring_virt, 0, ring_bytes);

    xhci_driver.event_ring = (xhci_trb_t *)ring_virt;
    xhci_driver.event_ring_dequeue_idx = 0;
    xhci_driver.event_ring_cycle_bit = 1;

    void *erst_virt = kmalloc_aligned(sizeof(xhci_erst_entry_t), 64);
    uint64_t erst_phys = paging_get_physical_address((uint64_t)erst_virt);
    paging_map((uint64_t)erst_virt, erst_phys, dma_flags);
    __asm__ volatile("invlpg (%0)" ::"r"(erst_virt) : "memory");
    memset(erst_virt, 0, sizeof(xhci_erst_entry_t));

    xhci_driver.erst = (xhci_erst_entry_t *)erst_virt;
    xhci_driver.erst[0].BaseAddress = ring_phys;
    xhci_driver.erst[0].Size = xhci_driver.event_ring_size;

    volatile xhci_interrupter_regs_t *intr = &xhci_driver.run_regs->Interrupters[0];

    intr->Erstsz = 1;

    intr->Erstba = erst_phys;

    intr->Erdp = ring_phys | (1ULL << 3);

    intr->Imod = 0;

    intr->Iman = XHCI_IMAN_IE | XHCI_IMAN_IP;

    __asm__ volatile("mfence" ::: "memory");

    console_write_debug("OK (IMOD=0).\n");
}

int xhci_reset_port(volatile xhci_port_regs_t *port, int port_id)
{
    const uint32_t CCS = (1u << 0);
    const uint32_t PED = (1u << 1);
    const uint32_t PR = (1u << 4);
    const uint32_t PP = (1u << 9);
    const uint32_t WPR = (1u << 31);

    const uint32_t CHG_BASE = (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21);

    uint32_t sc = port->PortSC;
    if ((sc & CCS) == 0)
        return 0;

    uint8_t speed = (sc >> 10) & 0xF;
    uint8_t pls = (sc >> 5) & 0xF;

    uint32_t CHG = CHG_BASE;
    if (speed == 4)
        CHG |= (1u << 22) | (1u << 23);

    port->PortSC = port->PortSC | CHG;
    (void)port->PortSC;

    sc = port->PortSC;
    if ((sc & PP) == 0)
    {
        port->PortSC = sc | PP;
        (void)port->PortSC;
        timer_sleep(20);
    }

    sc = port->PortSC;
    pls = (sc >> 5) & 0xF;
    if (pls == 7)
        timer_sleep(50);

    console_write_debug(" -> Reset(PR)... ");

    sc = port->PortSC;
    port->PortSC = sc | PP | PR | CHG;
    (void)port->PortSC;

    int t = 3000;
    while (t--)
    {
        sc = port->PortSC;
        if ((sc & PR) == 0)
            break;
        xhci_delay(1);
    }

    port->PortSC = port->PortSC | (1u << 21);
    (void)port->PortSC;

    t = 3000;
    while (t--)
    {
        sc = port->PortSC;
        if (sc & PED)
        {
            timer_sleep(200);
            port->PortSC = port->PortSC | CHG;
            (void)port->PortSC;
            console_write_debug("OK! (Enabled)\n");
            return 1;
        }
        xhci_delay(1);
    }

    sc = port->PortSC;
    speed = (sc >> 10) & 0xF;
    if (speed == 4)
    {
        console_write_debug("Failed (PED=0). -> Reset(WPR)... ");

        port->PortSC = port->PortSC | WPR | CHG;
        (void)port->PortSC;

        t = 4000;
        while (t--)
        {
            sc = port->PortSC;
            if ((sc & WPR) == 0)
                break;
            xhci_delay(1);
        }

        t = 4000;
        while (t--)
        {
            sc = port->PortSC;
            if (sc & PED)
            {
                timer_sleep(50);
                port->PortSC = port->PortSC | CHG;
                (void)port->PortSC;
                console_write_debug("OK! (Enabled)\n");
                return 1;
            }
            xhci_delay(1);
        }
    }

    console_write_debug("Failed (PED never set). PortSC=0x");
    console_print_hex_debug(port->PortSC);
    console_write_debug("\n");
    return 0;
}

void xhci_probe_ports()
{
    console_write_debug("\n[XHCI] Probing Ports ...\n");

    const uint32_t CCS = (1u << 0);
    const uint32_t PED = (1u << 1);
    const uint32_t SPEED_MASK = (0xFu << 10);

    uint8_t proto_major[256];
    uint8_t usb2_ports[256];
    int usb2_n = 0;
    uint8_t usb3_ports[256];
    int usb3_n = 0;

    xhci_build_port_protocol_lists(proto_major, usb2_ports, &usb2_n, usb3_ports, &usb3_n);

    uint8_t paired_usb2_of_usb3[256];
    memset(paired_usb2_of_usb3, 0xFF, 256);
    uint8_t paired_usb3_of_usb2[256];
    memset(paired_usb3_of_usb2, 0xFF, 256);

    int pair_n = (usb2_n < usb3_n) ? usb2_n : usb3_n;
    for (int i = 0; i < pair_n; i++)
    {
        uint8_t u2 = usb2_ports[i];
        uint8_t u3 = usb3_ports[i];
        paired_usb2_of_usb3[u3] = u2;
        paired_usb3_of_usb2[u2] = u3;
    }

    uint8_t usb2_active[256];
    memset(usb2_active, 1, 256);

    console_write_debug("[XHCI] Ports detected: USB2=");
    console_print_dec_debug(usb2_n);
    console_write_debug(" USB3=");
    console_print_dec_debug(usb3_n);
    console_write_debug("\n");

    for (int pi = 0; pi < usb3_n; pi++)
    {
        uint8_t p = usb3_ports[pi];
        volatile xhci_port_regs_t *port = &xhci_driver.port_regs[p];

        uint32_t sc = port->PortSC;
        if ((sc & CCS) == 0)
            continue;

        console_set_color_debug(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
        console_write_debug("[PORT ");
        console_print_dec_debug(p + 1);
        console_write_debug("] CONNECTED. [USB3] ");

        int ok = 0;
        if ((sc & PED) == 0)
        {
            console_write_debug("-> Reset... ");
            ok = xhci_port_reset_hs_style(port, p, 1);
            console_write_debug(ok ? "OK! (Enabled)\n" : "FAIL.\n");
        }
        else
        {
            console_write_debug("ALREADY ENABLED.\n");
            ok = 1;
        }

        if (ok)
        {

            uint8_t u2 = paired_usb2_of_usb3[p];
            if (u2 != 0xFF)
                usb2_active[u2] = 0;

            sc = port->PortSC;
            uint8_t speed = (uint8_t)((sc & SPEED_MASK) >> 10);
            console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
            xhci_configure_device(p, speed);
        }

        console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }

    for (int pi = 0; pi < usb2_n; pi++)
    {
        uint8_t p = usb2_ports[pi];
        if (!usb2_active[p])
            continue;

        volatile xhci_port_regs_t *port = &xhci_driver.port_regs[p];
        uint32_t sc = port->PortSC;
        if ((sc & CCS) == 0)
            continue;

        console_set_color_debug(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
        console_write_debug("[PORT ");
        console_print_dec_debug(p + 1);
        console_write_debug("] CONNECTED. [USB2] ");

        int ok = 0;
        if ((sc & PED) == 0)
        {
            console_write_debug("-> Reset... ");
            ok = xhci_port_reset_hs_style(port, p, 0);
            console_write_debug(ok ? "OK! (Enabled)\n" : "FAIL.\n");
        }
        else
        {
            console_write_debug("ALREADY ENABLED.\n");
            ok = 1;
        }

        if (ok)
        {
            sc = port->PortSC;
            uint8_t speed = (uint8_t)((sc & SPEED_MASK) >> 10);
            console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
            xhci_configure_device(p, speed);
        }

        console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
}

static volatile int kbd_need_recover = 0;
static volatile uint8_t kbd_recover_slot = 0;
static volatile uint8_t kbd_recover_ep_id = 0;

void xhci_kbd_recover_poll(void)
{
    uint8_t slot = 0, ep_id = 0;
    if (!xhci_kbd_recover_take(&slot, &ep_id))
    {
        return;
    }

    uint8_t ep_num = (ep_id - 1) / 2;
    uint8_t dir_in = (ep_id & 1);
    uint8_t ep_addr = ep_num | (dir_in ? 0x80 : 0x00);

    console_write_debug("[XHCI][KBD] RECOVER start slot=");
    console_print_dec_debug(slot);
    console_write_debug(" ep_id=");
    console_print_dec_debug(ep_id);
    console_write_debug(" ep_addr=0x");
    console_print_hex_debug(ep_addr);
    console_write_debug("\n");

    console_write_debug("[XHCI][KBD] CLEAR_HALT (device)...\n");
    if (!xhci_clear_endpoint_halt(slot, ep_addr))
    {
        console_write_debug("[XHCI][KBD] CLEAR_HALT failed (device). going with no xHC.\n");
    }
    else
    {
        console_write_debug("[XHCI][KBD] CLEAR_HALT ok (device)\n");
    }

    console_write_debug("[XHCI][KBD] STOP_ENDPOINT (xHC)...\n");
    if (!xhci_send_command_wait(15, 0, ((uint32_t)slot << 24) | ((uint32_t)ep_id << 16)))
    {
        console_write_debug("[XHCI][KBD] STOP_ENDPOINT failed. Reagendando.\n");
        xhci_kbd_recover_schedule(slot, ep_id);
        return;
    }

    xhci_trb_t *ring = xhci_driver.slot_kbd_rings[slot];
    if (!ring)
    {
        console_write_debug("[XHCI][KBD] ring NULL. is not possible to dequeue.\n");
        return;
    }

    xhci_driver.slot_kbd_enqueue[slot] = 0;
    xhci_driver.slot_kbd_cycle[slot] = 1;

    uint64_t ring_phys = paging_get_physical_address((uint64_t)&ring[0]);
    uint64_t deq = (ring_phys & ~0xFULL) | 1ULL;

    console_write_debug("[XHCI][KBD] SET_TR_DEQ_PTR (xHC)...\n");
    if (!xhci_send_command_wait(16, deq, ((uint32_t)slot << 24) | ((uint32_t)ep_id << 16)))
    {
        console_write_debug("[XHCI][KBD] SET_TR_DEQ_PTR failed. Rescheduled.\n");
        xhci_kbd_recover_schedule(slot, ep_id);
        return;
    }

    console_write_debug("[XHCI][KBD] RESTART (doorbell) + REARM...\n");
    xhci_ring_ep_doorbell(slot, ep_id);
    xhci_queue_kbd_request(slot);
}

static int xhci_event_ring_try_resync(uint64_t ev_ring_phys_base)
{
    if (!xhci_driver.event_ring || !xhci_driver.event_ring_size)
        return 0;

    uint64_t ir0 = (uint64_t)xhci_driver.run_regs + 0x20;
    volatile uint64_t *erdp_reg = (volatile uint64_t *)(ir0 + 0x18);

    uint64_t erdpv = xhci_read64((uint64_t)erdp_reg);

    uint64_t erdp_ptr = (erdpv & ~0xFULL);

    uint64_t ring_bytes = (uint64_t)xhci_driver.event_ring_size * sizeof(xhci_trb_t);
    uint64_t ring_start = ev_ring_phys_base;
    uint64_t ring_end = ev_ring_phys_base + ring_bytes;

    if (erdp_ptr < ring_start || erdp_ptr >= ring_end)
    {
        return 0;
    }

    uint32_t new_idx = (uint32_t)((erdp_ptr - ring_start) / sizeof(xhci_trb_t));
    if (new_idx >= xhci_driver.event_ring_size)
        return 0;

    xhci_driver.event_ring_dequeue_idx = new_idx;

    volatile xhci_trb_t *evt = &xhci_driver.event_ring[new_idx];
    __asm__ volatile("mfence" ::: "memory");

    xhci_driver.event_ring_cycle_bit = (evt->Control & 1);

    uint64_t erdp_write = (erdp_ptr & ~0xFULL) | (1ULL << 3);
    xhci_write64((uint64_t)erdp_reg, erdp_write);
    __asm__ volatile("mfence" ::: "memory");

    return 1;
}

static void xhci_dbg_dump_state(const char *tag)
{
    console_write_debug("\n====================\n");
    console_write_debug("ERROR BUG: ");
    console_write_debug(tag);
    console_write_debug("\n");
    console_write_debug("ISR#=");
    console_print_dec_debug((uint32_t)xhci_dbg_isr_count);
    console_write_debug("\n");
    console_write_debug("evt_processed=");
    console_print_dec_debug(xhci_dbg_last_evt_processed);
    console_write_debug(" kbd_processed=");
    console_print_dec_debug(xhci_dbg_last_kbd_processed);
    console_write_debug("\n");

    uint32_t usbsts = xhci_read32(&xhci_driver.op_regs->UsbSts);
    uint32_t usbcmd = xhci_read32(&xhci_driver.op_regs->UsbCmd);
    console_write_debug("UsbSts=0x");
    console_print_hex_debug(usbsts);
    console_write_debug(" UsbCmd=0x");
    console_print_hex_debug(usbcmd);
    console_write_debug("\n");

    uint64_t ir0 = (uint64_t)xhci_driver.run_regs + 0x20;
    volatile uint32_t *iman = (volatile uint32_t *)(ir0 + 0x00);
    volatile uint32_t *imod = (volatile uint32_t *)(ir0 + 0x04);
    volatile uint64_t *erdp = (volatile uint64_t *)(ir0 + 0x18);

    console_write_debug("IMAN=0x");
    console_print_hex_debug(xhci_read32(iman));
    console_write_debug(" IMOD=0x");
    console_print_hex_debug(xhci_read32(imod));
    console_write_debug("\n");

    uint64_t erdpv = xhci_read64((uint64_t)erdp);
    console_write_debug("ERDP=0x");
    console_print_hex_debug((uint32_t)(erdpv >> 32));
    console_write_debug("_");
    console_print_hex_debug((uint32_t)(erdpv & 0xFFFFFFFF));
    console_write_debug("\n");

    console_write_debug("deq_idx=");
    console_print_dec_debug(xhci_driver.event_ring_dequeue_idx);
    console_write_debug(" cycle=");
    console_print_dec_debug(xhci_driver.event_ring_cycle_bit);
    console_write_debug("\n");
    console_write_debug("====================\n");
}

void xhci_process_events(void)
{
    if (xhci_processing_events)
        return;
    xhci_processing_events = 1;

    uint32_t processed = 0;
    uint32_t kbd_processed = 0;
    uint64_t ev_ring_phys_base = paging_get_physical_address((uint64_t)xhci_driver.event_ring);

    uint32_t safety = 0;
    uint32_t safety_limit = xhci_driver.event_ring_size ? (xhci_driver.event_ring_size * 2) : 512;

    while (safety < safety_limit)
    {
        safety++;
        volatile xhci_trb_t *evt = &xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx];
        __asm__ volatile("lfence" ::: "memory");

        uint32_t ctrl = evt->Control;
        uint8_t evt_cycle = ctrl & 1;

        if (evt_cycle != xhci_driver.event_ring_cycle_bit)
            break;

        uint32_t sts = evt->Status;
        uint64_t param = evt->Parameter;
        uint32_t type = (ctrl >> 10) & 0x3F;
        uint8_t slot = (ctrl >> 24) & 0xFF;
        uint8_t ep_id = (ctrl >> 16) & 0x1F;
        uint8_t cc = (sts >> 24) & 0xFF;

        processed++;

        if (type == TRB_TYPE_CMD_COMPLETE)
        {

            if (g_cmd_waiting)
            {
                g_cmd_last_ptr = (param & ~0xFULL);
                g_cmd_last_cc = cc;
                g_cmd_last_slot = slot;
            }
        }
        else if (type == TRB_TYPE_TRANSFER_EVENT)
        {

            if (ep_id == 1)
            {
                if (g_ep0_waiting && slot == g_ep0_target_slot)
                {
                    g_ep0_result_cc = cc;
                    g_ep0_waiting = 0;
                }
            }

            else if (slot < 64 && xhci_driver.slot_kbd_dci[slot] && ep_id == xhci_driver.slot_kbd_dci[slot])
            {
                if (xhci_driver.slot_kbd_pending[slot] > 0)
                    xhci_driver.slot_kbd_pending[slot]--;

                if (cc == 1 || cc == 13)
                {
                    kbd_processed++;
                    xhci_dbg_last_kbd_ms[slot] = timer_get_uptime_ms();
                    uint64_t trb_phys = (param & ~0xFULL);

                    volatile xhci_trb_t *ring = xhci_driver.slot_kbd_rings[slot];
                    if (ring)
                    {
                        uint64_t ring_phys = xhci_driver.slot_kbd_ring_phys[slot];
                        uint32_t trb_idx = 0xFFFF;

                        if (ring_phys && trb_phys >= ring_phys && trb_phys < (ring_phys + 4096))
                            trb_idx = (uint32_t)((trb_phys - ring_phys) / 16);

                        uint8_t *buf = NULL;
                        if (trb_idx < 255)
                        {
                            buf = xhci_driver.slot_kbd_trb_buf[slot][trb_idx];
                            xhci_driver.slot_kbd_trb_buf[slot][trb_idx] = NULL;
                        }

                        if (!buf)
                        {
                            static uint8_t rr_fallback[64] = {0};
                            uint8_t pick = rr_fallback[slot]++ % XHCI_KBD_PIPE_DEPTH;
                            buf = xhci_driver.slot_kbd_buffers[slot][pick];
                        }

                        if (buf)
                        {

                            uint8_t modifiers = buf[0];
                            uint8_t keys[6];
                            for (int i = 0; i < 6; i++)
                                keys[i] = buf[2 + i];

                            memset(buf, 0, 8);
                            xhci_queue_kbd_request_buf(slot, buf);

                            int any_key_down = 0;
                            for (int i = 0; i < 6; i++)
                            {
                                if (keys[i] != 0 && keys[i] != 0x01)
                                {
                                    any_key_down = 1;
                                    break;
                                }
                            }

                            if (!any_key_down)
                            {

                                xhci_driver.slot_kbd_repeat_active[slot] = 0;
                                xhci_driver.slot_kbd_repeat_key[slot] = 0;
                                xhci_driver.slot_kbd_repeat_mods[slot] = 0;
                                xhci_driver.slot_kbd_repeat_next_ms[slot] = 0;
                            }

                            for (int i = 0; i < 6; i++)
                            {
                                uint8_t keycode = keys[i];
                                if (keycode == 0 || keycode == 0x01)
                                    continue;

                                int dup_in_report = 0;
                                for (int j = 0; j < i; j++)
                                {
                                    if (keys[j] == keycode)
                                    {
                                        dup_in_report = 1;
                                        break;
                                    }
                                }
                                if (dup_in_report)
                                    continue;

                                int already_pressed = 0;
                                for (int p = 0; p < 6; p++)
                                {
                                    if (xhci_driver.slot_kbd_prev_keys[slot][p] == keycode)
                                    {
                                        already_pressed = 1;
                                        break;
                                    }
                                }
                                if (already_pressed)
                                    continue;

                                keyboard_push_usb_event(modifiers, keycode);

                                xhci_driver.slot_kbd_repeat_key[slot] = keycode;
                                xhci_driver.slot_kbd_repeat_mods[slot] = modifiers;
                                xhci_driver.slot_kbd_repeat_active[slot] = 1;
                                xhci_driver.slot_kbd_repeat_next_ms[slot] = timer_get_uptime_ms() + 500;
                            }

                            for (int i = 0; i < 6; i++)
                                xhci_driver.slot_kbd_prev_keys[slot][i] = keys[i];
                            xhci_driver.slot_kbd_prev_mods[slot] = modifiers;
                        }
                    }
                }
                else
                {

                    static uint8_t rr_err[64] = {0};
                    uint8_t pick = rr_err[slot]++ % XHCI_KBD_PIPE_DEPTH;
                    xhci_queue_kbd_request_buf(slot, xhci_driver.slot_kbd_buffers[slot][pick]);
                }
            }
        }
        else if (type == TRB_TYPE_PORT_STATUS)
        {

            uint8_t port_id = (uint8_t)((param >> 24) & 0xFF);
            if (port_id >= 1 && port_id <= xhci_driver.max_ports)
            {
                volatile xhci_port_regs_t *port = &xhci_driver.port_regs[port_id - 1];
                uint32_t portsc = port->PortSC;
                usb_hotplug_handle_root_port_status(port_id, portsc);

                uint32_t preserve = (1u << 9);
                uint32_t change_bits = (1u << 17) | (1u << 18) | (1u << 19) |
                                       (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23);
                port->PortSC = (portsc & preserve) | change_bits;
            }
        }

        xhci_driver.event_ring_dequeue_idx++;
        if (xhci_driver.event_ring_dequeue_idx >= xhci_driver.event_ring_size)
        {
            xhci_driver.event_ring_dequeue_idx = 0;
            xhci_driver.event_ring_cycle_bit ^= 1;
        }
    }

    if (processed > 0)
    {
        uint64_t erdp_phys = ev_ring_phys_base +
                             (xhci_driver.event_ring_dequeue_idx * sizeof(xhci_trb_t));

        xhci_write64((uint64_t)&xhci_driver.run_regs->Interrupters[0].Erdp, erdp_phys | (1ULL << 3));
        __asm__ volatile("sfence" ::: "memory");

        xhci_total_events_processed += processed;
        xhci_total_kbd_events += kbd_processed;
        xhci_last_successful_process_ms = timer_get_uptime_ms();
    }

    xhci_dbg_last_evt_processed = processed;
    xhci_dbg_last_kbd_processed = kbd_processed;
    xhci_processing_events = 0;
}

static volatile uint32_t xhci_isr_in_progress = 0;

void xhci_handle_interrupt(void)
{
    xhci_dbg_isr_count++;

    if (!xhci_driver_ready)
        return;
    if (!xhci_driver.op_regs || !xhci_driver.run_regs)
        return;

    if (xhci_isr_in_progress)
        return;
    xhci_isr_in_progress = 1;

    uint64_t ir0 = (uint64_t)xhci_driver.run_regs + 0x20;
    volatile uint32_t *iman = (volatile uint32_t *)(ir0 + 0x00);

    uint32_t iman_val = xhci_read32(iman);
    if (!(iman_val & XHCI_IMAN_IE))
    {
        xhci_write32(iman, iman_val | XHCI_IMAN_IE);
        (void)xhci_read32(iman);
        iman_val = xhci_read32(iman);
    }

    int have_work = 0;

    if (iman_val & XHCI_IMAN_IP)
    {
        have_work = 1;
    }
    else
    {

        volatile xhci_trb_t *evt = &xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx];
        uint32_t ctrl = evt->Control;
        uint8_t evt_cycle = (uint8_t)(ctrl & 1u);
        if (evt_cycle == xhci_driver.event_ring_cycle_bit)
            have_work = 1;
    }

    if (have_work)
    {

        xhci_process_events();

        g_xhci_need_bh = 1;

        iman_val = xhci_read32(iman);
        if (iman_val & XHCI_IMAN_IP)
        {
            xhci_write32(iman, iman_val | XHCI_IMAN_IP);
            (void)xhci_read32(iman);
        }

        uint32_t usbsts = xhci_read32(&xhci_driver.op_regs->UsbSts);
        if (usbsts & USBSTS_EINT)
        {
            xhci_write32(&xhci_driver.op_regs->UsbSts, USBSTS_EINT);
            (void)xhci_read32(&xhci_driver.op_regs->UsbSts);
        }
    }

    xhci_isr_in_progress = 0;
}

static uint32_t xhci_get_ctx_size(void)
{

    uint32_t ctx_size = 32;
    if ((xhci_driver.cap_regs->HccParams1 >> 2) & 1u)
        ctx_size = 64;
    return ctx_size;
}

static void xhci_clflush_range(const void *ptr, uint64_t bytes)
{

    const uint8_t *p = (const uint8_t *)ptr;
    for (uint64_t off = 0; off < bytes; off += 64)
    {
        __asm__ __volatile__("clflush (%0)" ::"r"(p + off) : "memory");
    }
    __asm__ __volatile__("mfence" ::: "memory");
}

static void xhci_map_identity_range(uint64_t phys, uint64_t bytes)
{
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + bytes + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t p = start; p < end; p += 0x1000)
    {
        paging_map(p, p, PAGE_PRESENT | PAGE_RW);
        __asm__ __volatile__("invlpg (%0)" ::"r"(p) : "memory");
    }
}

static void xhci_print_hex64(uint64_t v)
{

    console_write_debug("0x");
    console_print_hex_debug((uint32_t)(v >> 32));
    console_write_debug("_");
    console_print_hex_debug((uint32_t)(v & 0xFFFFFFFFu));
}

static void xhci_dump_trb_line(uint32_t idx, volatile xhci_trb_t *trb)
{
    console_write_debug("    TRB[");
    console_print_dec_debug(idx);
    console_write_debug("] P=");
    xhci_print_hex64(trb->Parameter);
    console_write_debug(" S=0x");
    console_print_hex_debug(trb->Status);
    console_write_debug(" C=0x");
    console_print_hex_debug(trb->Control);

    uint32_t type = (trb->Control >> 10) & 0x3F;
    console_write_debug(" (type=");
    console_print_dec_debug(type);
    console_write_debug(" cyc=");
    console_print_dec_debug(trb->Control & 1u);
    console_write_debug(")\n");
}

static volatile uint8_t *xhci_get_output_ctx(uint8_t slot)
{
    if (!xhci_driver.dcbaa)
        return NULL;

    uint64_t out_phys = xhci_driver.dcbaa[slot];

    out_phys &= ~0x3FULL;
    if (!out_phys)
        return NULL;

    uint32_t ctx_size = xhci_get_ctx_size();
    uint64_t out_bytes = (uint64_t)ctx_size * 32;
    if (out_bytes < 4096)
        out_bytes = 4096;

    xhci_map_identity_range(out_phys, out_bytes);

    return (volatile uint8_t *)(uint64_t)out_phys;
}

static volatile uint32_t *xhci_get_output_ep_ctx_dw(uint8_t slot, uint8_t dci)
{
    volatile uint8_t *out = xhci_get_output_ctx(slot);
    if (!out)
        return NULL;

    uint32_t ctx_size = xhci_get_ctx_size();
    return (volatile uint32_t *)(out + (uint64_t)ctx_size * (uint64_t)dci);
}

static void xhci_dump_kbd_endpoint_state(uint8_t slot)
{

    if (!xhci_dbg_on(XHCI_DBG_DUMP))
        return;

    uint8_t dci = xhci_driver.slot_kbd_dci[slot];
    if (!dci)
        return;

    console_write_debug("\n==================== [XHCI DUMP] Slot ");
    console_print_dec_debug(slot);
    console_write_debug(" dci=");
    console_print_dec_debug(dci);
    console_write_debug(" ====================\n");

    console_write_debug("USBSTS=0x");
    console_print_hex_debug(xhci_read32(&xhci_driver.op_regs->UsbSts));
    console_write_debug(" USBCMD=0x");
    console_print_hex_debug(xhci_read32(&xhci_driver.op_regs->UsbCmd));
    console_write_debug("\n");

    if (xhci_driver.run_regs)
    {
        volatile xhci_interrupter_regs_t *intr = &xhci_driver.run_regs->Interrupters[0];

        console_write_debug("IMAN=0x");
        console_print_hex_debug(intr->Iman);
        console_write_debug(" IMOD=0x");
        console_print_hex_debug(intr->Imod);
        console_write_debug(" ERDP=0x");
        console_print_hex_debug((uint32_t)(intr->Erdp >> 32));
        console_write_debug("_");
        console_print_hex_debug((uint32_t)(intr->Erdp & 0xFFFFFFFFu));
        console_write_debug(" ERSTBA=0x");
        console_print_hex_debug((uint32_t)(intr->Erstba >> 32));
        console_write_debug("_");
        console_print_hex_debug((uint32_t)(intr->Erstba & 0xFFFFFFFFu));
        console_write_debug(" ERSTSZ=0x");
        console_print_hex_debug(intr->Erstsz);
        console_write_debug("\n");
    }

    console_write_debug("SW: enq=");
    console_print_dec_debug(xhci_driver.slot_kbd_enqueue[slot]);
    console_write_debug(" cyc=");
    console_print_dec_debug(xhci_driver.slot_kbd_cycle[slot] & 1u);
    console_write_debug(" ring_phys=0x");
    console_print_hex_debug((uint32_t)(xhci_driver.slot_kbd_ring_phys[slot] >> 32));
    console_write_debug("_");
    console_print_hex_debug((uint32_t)(xhci_driver.slot_kbd_ring_phys[slot] & 0xFFFFFFFFu));
    console_write_debug("\n");

    volatile uint32_t *ep = xhci_get_output_ep_ctx_dw(slot, dci);
    if (!ep)
    {
        console_write_debug("EPCTX: <NULL>\n");
        console_write_debug("===============================================================\n");
        return;
    }

    xhci_clflush_range((const void *)ep, (uint64_t)xhci_get_ctx_size());

    uint32_t dw0 = ep[0];
    uint32_t dw1 = ep[1];
    uint32_t dw2 = ep[2];
    uint32_t dw3 = ep[3];

    uint8_t ep_state = (uint8_t)(dw0 & 0x7u);
    uint8_t dcs = (uint8_t)(dw2 & 1u);
    uint64_t deq_raw = ((uint64_t)dw3 << 32) | (uint64_t)dw2;
    uint64_t deq_ptr = deq_raw & ~0xFULL;

    console_write_debug("EPCTX: DW0=0x");
    console_print_hex_debug(dw0);
    console_write_debug(" DW1=0x");
    console_print_hex_debug(dw1);
    console_write_debug(" DW2=0x");
    console_print_hex_debug(dw2);
    console_write_debug(" DW3=0x");
    console_print_hex_debug(dw3);
    console_write_debug("\n");

    console_write_debug("HW: ep_state=");
    console_print_dec_debug(ep_state);
    console_write_debug(" DCS=");
    console_print_dec_debug(dcs);
    console_write_debug(" TR_deq=0x");
    console_print_hex_debug((uint32_t)(deq_ptr >> 32));
    console_write_debug("_");
    console_print_hex_debug((uint32_t)(deq_ptr & 0xFFFFFFFFu));
    console_write_debug("\n");

    volatile xhci_trb_t *ring = xhci_driver.slot_kbd_rings[slot];
    if (!ring)
    {
        console_write_debug("TRBs: <NULL>\n");
        console_write_debug("===============================================================\n");
        return;
    }

    console_write_debug("TRBs (wrap):\n");
    for (uint32_t i = 252; i <= 255; i++)
        xhci_dump_trb_line(i, &ring[i]);
    for (uint32_t i = 0; i <= 3; i++)
        xhci_dump_trb_line(i, &ring[i]);

    console_write_debug("===============================================================\n\n");
}

void xhci_kbd_repeat_poll(void)
{
    if (!xhci_driver_ready)
        return;

    uint64_t now = timer_get_uptime_ms();

    const uint64_t REPEAT_RATE_MS = 30;

    for (int slot = 1; slot < 64; slot++)
    {

        if (!xhci_driver.slot_kbd_dci[slot])
            continue;
        if (!xhci_driver.slot_kbd_repeat_active[slot])
            continue;

        uint8_t key = xhci_driver.slot_kbd_repeat_key[slot];
        if (key == 0)
        {
            xhci_driver.slot_kbd_repeat_active[slot] = 0;
            continue;
        }

        uint64_t next = xhci_driver.slot_kbd_repeat_next_ms[slot];
        if (next == 0 || now < next)
            continue;

        int still_down = 0;
        for (int i = 0; i < 6; i++)
        {
            if (xhci_driver.slot_kbd_prev_keys[slot][i] == key)
            {
                still_down = 1;
                break;
            }
        }

        if (!still_down)
        {
            xhci_driver.slot_kbd_repeat_active[slot] = 0;
            xhci_driver.slot_kbd_repeat_key[slot] = 0;
            xhci_driver.slot_kbd_repeat_mods[slot] = 0;
            xhci_driver.slot_kbd_repeat_next_ms[slot] = 0;
            continue;
        }

        keyboard_push_usb_event(xhci_driver.slot_kbd_repeat_mods[slot], key);

        xhci_driver.slot_kbd_repeat_next_ms[slot] = now + REPEAT_RATE_MS;
    }
}

volatile int g_xhci_need_bh = 0;

void xhci_poll_events(void)
{
    if (!xhci_driver_ready)
        return;
    if (!xhci_driver.op_regs || !xhci_driver.run_regs)
        return;
    if (!xhci_driver.event_ring)
        return;

    if (g_xhci_need_bh)
        return;

    volatile xhci_trb_t *evt = &xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx];
    uint32_t ctrl = evt->Control;
    uint8_t evt_cycle = (uint8_t)(ctrl & 1u);

    if (evt_cycle == xhci_driver.event_ring_cycle_bit)
    {

        g_xhci_need_bh = 1;
    }
}

void xhci_reset_controller()
{
    console_write_debug("[XHCI] Stopping... ");

    uint32_t cmd = xhci_read32(&xhci_driver.op_regs->UsbCmd);
    cmd &= ~USBCMD_RUN_STOP;
    xhci_write32(&xhci_driver.op_regs->UsbCmd, cmd);

    while (!(xhci_read32(&xhci_driver.op_regs->UsbSts) & USBSTS_HC_HALTED))
        xhci_delay(1);

    console_write_debug("Resetting... ");
    cmd = xhci_read32(&xhci_driver.op_regs->UsbCmd);
    cmd |= USBCMD_HC_RESET;
    xhci_write32(&xhci_driver.op_regs->UsbCmd, cmd);

    while (xhci_read32(&xhci_driver.op_regs->UsbCmd) & USBCMD_HC_RESET)
        xhci_delay(1);
    while (xhci_read32(&xhci_driver.op_regs->UsbSts) & (1 << 11))
        xhci_delay(1);

    console_write_debug("Ready.\n");
}

void xhci_init(uint64_t base_address)
{
    console_set_color_debug(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
    console_write_debug("\n=== XHCI DRIVER FASE 5 (INVERTED CYCLE) ===\n");

    xhci_driver.pci_base_address = base_address;

    const uint64_t mmio_flags = PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT;

    for (uint64_t i = 0; i < 256; i++)
    {
        uint64_t page = base_address + (i * 4096);
        paging_map(page, page, mmio_flags);
    }

    xhci_driver.virtual_base_address = base_address;

    xhci_bios_handoff(base_address);

    xhci_driver.cap_regs = (xhci_cap_regs_t *)base_address;

    xhci_driver.op_regs = (xhci_op_regs_t *)(base_address + xhci_driver.cap_regs->CapLength);

    uint32_t rtsoff_raw = xhci_driver.cap_regs->RtsOff;
    uint32_t dboff_raw = xhci_driver.cap_regs->DbOff;

    uint32_t rtsoff = rtsoff_raw & ~0x1FULL;
    uint32_t dboff = dboff_raw & ~0x3U;

    xhci_driver.run_regs = (xhci_runtime_regs_t *)(base_address + rtsoff);
    xhci_driver.db_regs = (xhci_doorbell_regs_t *)(base_address + dboff);

    xhci_driver.port_regs = (xhci_port_regs_t *)((uint64_t)xhci_driver.op_regs + 0x400);

    uint32_t params = xhci_driver.cap_regs->HcsParams1;
    xhci_driver.max_slots = XHCI_MAX_SLOTS(params);
    xhci_driver.max_ports = XHCI_MAX_PORTS(params);

    console_write_debug("[XHCI] Initializing keyboard arrays...\n");
    for (int i = 0; i < 64; i++)
    {
        xhci_driver.slot_kbd_rings[i] = NULL;
        xhci_driver.slot_kbd_enqueue[i] = 0;
        xhci_driver.slot_kbd_cycle[i] = 0;
        xhci_driver.slot_kbd_buffer[i] = NULL;
        xhci_driver.slot_led_buffer[i] = NULL;
        xhci_driver.slot_kbd_dci[i] = 0;

        for (int q = 0; q < XHCI_KBD_PIPE_DEPTH; q++)
        {
            xhci_driver.slot_kbd_buffers[i][q] = NULL;
            xhci_driver.slot_kbd_buffers_phys[i][q] = 0;
        }

        for (int k = 0; k < 6; k++)
            xhci_driver.slot_kbd_prev_keys[i][k] = 0;
        xhci_driver.slot_kbd_prev_mods[i] = 0;
        xhci_driver.slot_kbd_repeat_key[i] = 0;
        xhci_driver.slot_kbd_repeat_mods[i] = 0;
        xhci_driver.slot_kbd_repeat_active[i] = 0;
        xhci_driver.slot_kbd_repeat_next_ms[i] = 0;

        xhci_driver.slot_ep0_rings[i] = NULL;
        xhci_driver.slot_ep0_enqueue[i] = 0;
        xhci_driver.slot_ep0_cycle[i] = 0;
    }

    console_write_debug(" -> Base: 0x");
    console_print_hex_debug(base_address);
    console_write_debug("\n");
    console_write_debug(" -> RTSOFF raw: 0x");
    console_print_hex_debug(rtsoff_raw);
    console_write_debug(" masked: 0x");
    console_print_hex_debug(rtsoff);
    console_write_debug("\n");
    console_write_debug(" -> DBOFF  raw: 0x");
    console_print_hex_debug(dboff_raw);
    console_write_debug(" masked: 0x");
    console_print_hex_debug(dboff);
    console_write_debug("\n");

    xhci_reset_controller();

    xhci_write32(&xhci_driver.op_regs->UsbSts, 0xFFFFFFFF);

    xhci_write32(&xhci_driver.op_regs->Config, xhci_driver.max_slots);

    xhci_alloc_dcbaa();
    xhci_init_command_ring();
    xhci_init_event_ring();

    console_write_debug("[XHCI] Starting Controller... ");
    uint32_t cmd = xhci_read32(&xhci_driver.op_regs->UsbCmd);
    cmd |= (USBCMD_INTE | USBCMD_RUN_STOP);
    xhci_write32(&xhci_driver.op_regs->UsbCmd, cmd);

    int timeout = 100;
    while ((xhci_read32(&xhci_driver.op_regs->UsbSts) & USBSTS_HC_HALTED) && timeout > 0)
    {
        timer_sleep(1);
        timeout--;
    }

    if (xhci_read32(&xhci_driver.op_regs->UsbSts) & USBSTS_HC_HALTED)
    {
        console_write_debug("FAILED (Halted).\n");
    }
    else
    {
        console_write_debug("OK (Running).\n");
    }

    console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
    console_write_debug("[SUCCESS] XHCI Init Done.\n");

    usb_hub_init();

    xhci_probe_ports();

    usb_hub_print_tree();

    console_write_debug("==================================\n");
    console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

    xhci_driver_ready = 1;
}

void xhci_ring_ep_doorbell(uint8_t slot_id, uint8_t endpoint_id)
{
    volatile uint32_t *db = (volatile uint32_t *)&xhci_driver.db_regs[slot_id].Target;

    __asm__ volatile("sfence" ::: "memory");

    *db = (uint32_t)endpoint_id;
}

int xhci_evaluate_context(uint8_t slot_id, uint16_t new_mps, int port_id, int speed_id)
{
    (void)port_id;
    (void)speed_id;

    uint32_t ctx_size = 32;
    if ((xhci_driver.cap_regs->HccParams1 & 0x4) != 0)
        ctx_size = 64;

    uint64_t out_phys = xhci_driver.dcbaa[slot_id];
    if (out_phys == 0)
    {
        console_write_debug(" -> Eval Ctx: No OUT ctx.\n");
        return 0;
    }

    uint64_t out_bytes = ctx_size * 32;
    if (out_bytes < 4096)
        out_bytes = 4096;

    for (uint64_t off = 0; off < out_bytes; off += 4096)
    {
        paging_map(out_phys + off, out_phys + off, PAGE_PRESENT | PAGE_RW);
        __asm__ volatile("invlpg (%0)" ::"r"(out_phys + off) : "memory");
    }

    uint8_t *out_ctx = (uint8_t *)(uint64_t)out_phys;

    void *input_ctx = kmalloc_aligned(4096, 4096);
    if (!input_ctx)
        return 0;

    uint64_t inp_phys = paging_get_physical_address((uint64_t)input_ctx);
    paging_map((uint64_t)input_ctx, inp_phys, PAGE_PRESENT | PAGE_RW);
    __asm__ volatile("invlpg (%0)" ::"r"(input_ctx) : "memory");

    memset(input_ctx, 0, 4096);

    uint32_t *icc_drop = (uint32_t *)((uint8_t *)input_ctx + 0);
    uint32_t *icc_add = (uint32_t *)((uint8_t *)input_ctx + 4);
    *icc_drop = 0;
    *icc_add = (1u << 1);

    uint8_t *in_slot = (uint8_t *)input_ctx + ctx_size;
    uint8_t *in_ep0 = (uint8_t *)input_ctx + (ctx_size * 2);

    uint8_t *out_slot = out_ctx + (ctx_size * 0);
    uint8_t *out_ep0 = out_ctx + (ctx_size * 1);

    memcpy(in_slot, out_slot, ctx_size);
    memcpy(in_ep0, out_ep0, ctx_size);

    if (ctx_size == 64)
    {
        memset(in_slot + 32, 0, 32);
        memset(in_ep0 + 32, 0, 32);
    }

    ((xhci_slot_context_t *)in_slot)->SlotState = 0;

    ((xhci_endpoint_context_t *)in_ep0)->EpState = 0;

    ((xhci_endpoint_context_t *)in_ep0)->MaxPacketSize = new_mps;

    for (uint64_t off = 0; off < 4096; off += 64)
    {
        __asm__ volatile("clflush (%0)" ::"r"((uint64_t)input_ctx + off));
    }
    __asm__ volatile("mfence" ::: "memory");

    timer_sleep(1);

    console_write_debug(" -> Eval Ctx (MPS=");
    console_print_dec_debug(new_mps);
    console_write_debug(")... ");

    uint8_t res = xhci_send_command_wait(TRB_TYPE_EVALUATE_CONTEXT, inp_phys, (slot_id << 24));
    if (res == slot_id)
    {
        console_write_debug("OK.\n");
        return 1;
    }
    else
    {
        console_write_debug("Fail.\n");
        return 0;
    }
}

void xhci_queue_trb(uint8_t slot_id, uint32_t p_low, uint32_t p_high, uint32_t status, uint32_t control)
{
    xhci_trb_t *ring = xhci_driver.slot_ep0_rings[slot_id];
    uint16_t idx = xhci_driver.slot_ep0_enqueue[slot_id];
    uint8_t cycle = xhci_driver.slot_ep0_cycle[slot_id];

    ring[idx].Parameter = ((uint64_t)p_high << 32) | p_low;
    ring[idx].Status = status;

    ring[idx].Control = control | cycle;

    __asm__ volatile("clflush (%0)" ::"r"(&ring[idx]));

    idx++;

    if (idx >= 255)
    {

        xhci_trb_t *link = &ring[255];
        link->Control = (TRB_TYPE_LINK << 10) | (1 << 1) | cycle;
        __asm__ volatile("clflush (%0)" ::"r"(link));

        idx = 0;
        xhci_driver.slot_ep0_cycle[slot_id] ^= 1;
    }

    xhci_driver.slot_ep0_enqueue[slot_id] = idx;
}

void xhci_ring_command_doorbell(void)
{

    volatile uint32_t *db = (volatile uint32_t *)&xhci_driver.db_regs[0].Target;

    __asm__ volatile("mfence" ::: "memory");
    *db = 0;
    __asm__ volatile("mfence" ::: "memory");
}

uint8_t xhci_send_command_wait(uint32_t type, uint64_t param, uint32_t control_bits)
{
    if (xhci_read32(&xhci_driver.op_regs->UsbSts) & USBSTS_HC_HALTED)
    {
        console_write_debug("[XHCI] HC Halted before cmd.\n");
        return 0;
    }

    g_cmd_last_ptr = 0;
    g_cmd_last_cc = 0;
    g_cmd_last_slot = 0;
    g_cmd_waiting = 1;

    const uint32_t TRB_CTRL_IOC = (1u << 5);
    uint64_t idx = xhci_driver.cmd_ring_enqueue_idx;
    uint8_t cycle = xhci_driver.cmd_ring_cycle_bit & 1;

    volatile xhci_trb_t *cmd = &xhci_driver.cmd_ring_base[idx];

    cmd->Parameter = param;
    cmd->Status = 0;
    __asm__ volatile("" ::: "memory");
    cmd->Control = (type << 10) | control_bits | TRB_CTRL_IOC | cycle;

    uint64_t cmd_phys = paging_get_physical_address((uint64_t)cmd) & ~0xFULL;

    __asm__ volatile("clflush (%0)" ::"r"(cmd));
    __asm__ volatile("mfence" ::: "memory");

    idx++;
    if (idx >= (xhci_driver.cmd_ring_size - 1))
    {
        volatile xhci_trb_t *link = &xhci_driver.cmd_ring_base[idx];

        link->Control = (TRB_TYPE_LINK << 10) | (1u << 1) | cycle;

        __asm__ volatile("clflush (%0)" ::"r"(link));
        __asm__ volatile("mfence" ::: "memory");

        xhci_driver.cmd_ring_enqueue_idx = 0;
        xhci_driver.cmd_ring_cycle_bit ^= 1;
    }
    else
    {
        xhci_driver.cmd_ring_enqueue_idx = idx;
    }

    xhci_ring_command_doorbell();

    int timeout_ms = 3000;
    while (timeout_ms-- > 0)
    {
        timer_sleep(1);

        xhci_process_events();

        if (g_cmd_last_ptr == cmd_phys)
        {
            g_cmd_waiting = 0;

            if (g_cmd_last_cc != 1)
            {
                console_write_debug("[XHCI] CMD FAILED. CC=");
                console_print_dec_debug(g_cmd_last_cc);
                console_write_debug("\n");
                return 0;
            }

            if (type == TRB_TYPE_ENABLE_SLOT || type == TRB_TYPE_ADDRESS_DEVICE ||
                type == TRB_TYPE_CONFIG_EP || type == TRB_TYPE_EVALUATE_CONTEXT)
            {
                return (uint8_t)g_cmd_last_slot;
            }
            return 1;
        }
    }

    g_cmd_waiting = 0;
    console_write_debug("[XHCI] CMD TIMEOUT! Phys=0x");
    console_print_hex_debug((uint32_t)cmd_phys);
    console_write_debug("\n");
    return 0;
}

int xhci_get_descriptor_device(uint8_t slot_id, usb_device_descriptor_t *out_desc, uint16_t len)
{

    void *buffer = kmalloc_aligned(64, 64);
    if (!buffer)
        return 0;

    uint64_t phys = paging_get_physical_address((uint64_t)buffer);
    paging_map((uint64_t)buffer, phys, PAGE_PRESENT | PAGE_RW);
    __asm__ volatile("invlpg (%0)" ::"r"(buffer) : "memory");
    memset(buffer, 0, 64);

    usb_setup_packet_t setup;
    setup.RequestType = 0x80;
    setup.Request = 0x06;
    setup.Value = (1 << 8) | 0;
    setup.Index = 0;
    setup.Length = len;

    int success = xhci_control_transfer(slot_id, &setup, buffer);

    if (success)
    {

        uint16_t copy_len = (len > sizeof(usb_device_descriptor_t)) ? sizeof(usb_device_descriptor_t) : len;
        memcpy(out_desc, buffer, copy_len);
    }

    return success;
}

void *xhci_get_config_descriptor(uint8_t slot_id, uint16_t *out_len)
{

    usb_config_descriptor_t head;

    void *buf_head = kmalloc_aligned(64, 64);
    uint64_t head_phys = paging_get_physical_address((uint64_t)buf_head);
    paging_map((uint64_t)buf_head, head_phys, PAGE_PRESENT | PAGE_RW);
    memset(buf_head, 0, 64);

    usb_setup_packet_t setup;
    setup.RequestType = 0x80;
    setup.Request = 0x06;
    setup.Value = (2 << 8) | 0;
    setup.Index = 0;
    setup.Length = 9;

    if (!xhci_control_transfer(slot_id, &setup, buf_head))
    {
        console_write_debug(" -> GetConfig(Head) Failed.\n");
        return 0;
    }

    memcpy(&head, buf_head, 9);
    uint16_t total_len = head.wTotalLength;

    uint64_t alloc_len = (total_len + 63) & ~63;
    void *buf_full = kmalloc_aligned(alloc_len, 4096);
    uint64_t full_phys = paging_get_physical_address((uint64_t)buf_full);

    for (uint64_t off = 0; off < alloc_len; off += 4096)
    {
        paging_map((uint64_t)buf_full + off, full_phys + off, PAGE_PRESENT | PAGE_RW);
        __asm__ volatile("invlpg (%0)" ::"r"((uint64_t)buf_full + off) : "memory");
    }
    memset(buf_full, 0, alloc_len);

    setup.Length = total_len;

    if (!xhci_control_transfer(slot_id, &setup, buf_full))
    {
        console_write_debug(" -> GetConfig(Full) Failed.\n");
        return 0;
    }

    *out_len = total_len;
    return buf_full;
}

void xhci_parse_config(void *config_buffer, uint16_t total_len, usb_device_info_t *info)
{

    info->found = 0;
    info->cand_count = 0;

    usb_config_descriptor_t *cfg = (usb_config_descriptor_t *)config_buffer;
    info->config_value = cfg->bConfigurationValue;

    uint8_t *ptr = (uint8_t *)config_buffer;
    uint16_t offset = 0;

    uint8_t cur_iface = 0xFF;
    uint8_t cur_alt = 0;
    int in_kbd_iface = 0;

    int best_idx = -1;

    while (offset + 2 <= total_len)
    {
        uint8_t len = ptr[offset + 0];
        uint8_t type = ptr[offset + 1];
        if (len < 2)
            break;
        if (offset + len > total_len)
            break;

        if (type == USB_DESC_INTERFACE)
        {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)(ptr + offset);

            cur_iface = iface->bInterfaceNumber;
            cur_alt = iface->bAlternateSetting;

            if (iface->bInterfaceClass == 3 && iface->bInterfaceSubClass == 1 && iface->bInterfaceProtocol == 1)
            {
                in_kbd_iface = 1;

                console_write_debug("[XHCI][CFG] HID Boot Keyboard IF=");
                console_print_dec_debug(cur_iface);
                console_write_debug(" ALT=");
                console_print_dec_debug(cur_alt);
                console_write_debug("\n");
            }
            else
            {
                in_kbd_iface = 0;
            }
        }
        else if (type == USB_DESC_ENDPOINT)
        {
            if (in_kbd_iface)
            {
                usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)(ptr + offset);

                uint8_t ep_addr = ep->bEndpointAddress;
                uint8_t attr = ep->bmAttributes;
                uint8_t ep_type = (attr & 0x3);
                uint16_t mps = ep->wMaxPacketSize & 0x7FF;
                uint8_t interval = ep->bInterval;

                int is_in = (ep_addr & 0x80) != 0;
                int is_intr = (ep_type == 3);

                if (is_in && is_intr)
                {
                    if (info->cand_count < XHCI_KBD_EP_CANDIDATES)
                    {
                        int i = info->cand_count++;

                        info->cand_iface[i] = cur_iface;
                        info->cand_alt[i] = cur_alt;
                        info->cand_epaddr[i] = ep_addr;
                        info->cand_mps[i] = mps;
                        info->cand_interval[i] = interval;

                        console_write_debug("[XHCI][CFG]  EP cand: IF=");
                        console_print_dec_debug(cur_iface);
                        console_write_debug(" ALT=");
                        console_print_dec_debug(cur_alt);
                        console_write_debug(" EP=0x");
                        console_print_hex_debug(ep_addr);
                        console_write_debug(" MPS=");
                        console_print_dec_debug(mps);
                        console_write_debug(" INT=");
                        console_print_dec_debug(interval);
                        console_write_debug("\n");

                        if (best_idx < 0 || (info->cand_alt[i] == 0 && info->cand_alt[best_idx] != 0))
                        {
                            best_idx = i;
                        }
                    }
                }
            }
        }

        offset += len;
    }

    if (info->cand_count > 0)
    {
        if (best_idx < 0)
            best_idx = 0;

        info->interface_num = info->cand_iface[best_idx];
        info->interface_alt = info->cand_alt[best_idx];
        info->endpoint_addr = info->cand_epaddr[best_idx];
        info->endpoint_mps = info->cand_mps[best_idx];
        info->endpoint_interval = info->cand_interval[best_idx];

        info->found = 1;
    }
}

int xhci_set_interface(uint8_t slot_id, uint8_t interface_num, uint8_t alt_setting)
{
    usb_setup_packet_t setup;
    setup.RequestType = 0x01;
    setup.Request = USB_REQ_SET_INTERFACE;
    setup.Value = alt_setting;
    setup.Index = interface_num;
    setup.Length = 0;

    if (xhci_control_transfer(slot_id, &setup, 0))
    {
        console_write_debug("[XHCI][IFACE] SET_INTERFACE OK if=");
        console_print_dec_debug(interface_num);
        console_write_debug(" alt=");
        console_print_dec_debug(alt_setting);
        console_write_debug("\n");
        return 1;
    }

    console_write_debug("[XHCI][IFACE] SET_INTERFACE FAIL if=");
    console_print_dec_debug(interface_num);
    console_write_debug(" alt=");
    console_print_dec_debug(alt_setting);
    console_write_debug("\n");
    return 0;
}

int xhci_get_endpoint_status(uint8_t slot_id, uint8_t ep_addr, uint16_t *out_status)
{
    uint8_t buf[2] = {0, 0};

    usb_setup_packet_t setup;
    setup.RequestType = 0x82;
    setup.Request = USB_REQ_GET_STATUS;
    setup.Value = 0;
    setup.Index = ep_addr;
    setup.Length = 2;

    if (!xhci_control_transfer(slot_id, &setup, buf))
    {
        console_write_debug("[XHCI][EP] GET_STATUS FAIL ep=0x");
        console_print_hex_debug(ep_addr);
        console_write_debug("\n");
        return 0;
    }

    uint16_t st = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (out_status)
        *out_status = st;

    console_write_debug("[XHCI][EP] GET_STATUS OK ep=0x");
    console_print_hex_debug(ep_addr);
    console_write_debug(" status=0x");
    console_print_hex_debug(st);
    console_write_debug("\n");

    return 1;
}

int xhci_set_idle(uint8_t slot_id, uint8_t interface_num, uint8_t duration, uint8_t report_id)
{
    usb_setup_packet_t setup;
    setup.RequestType = 0x21;
    setup.Request = 0x0A;
    setup.Value = ((uint16_t)duration << 8) | report_id;
    setup.Index = interface_num;
    setup.Length = 0;

    if (xhci_control_transfer(slot_id, &setup, 0))
    {
        console_write_debug("[XHCI][HID] SET_IDLE OK if=");
        console_print_dec_debug(interface_num);
        console_write_debug("\n");
        return 1;
    }

    console_write_debug("[XHCI][HID] SET_IDLE FAIL if=");
    console_print_dec_debug(interface_num);
    console_write_debug("\n");
    return 0;
}

int xhci_control_transfer(uint8_t slot_id, usb_setup_packet_t *setup, void *data)
{
    if (xhci_driver.slot_ep0_rings[slot_id] == 0)
    {
        console_write_debug("[XHCI] Erro: Ring EP0 not initialized for slot ");
        console_print_dec_debug(slot_id);
        console_write_debug("\n");
        return 0;
    }

    g_ep0_target_slot = slot_id;
    g_ep0_result_cc = 0;
    g_ep0_waiting = 1;

    uint8_t is_in = (setup->RequestType & USB_DIR_IN) ? 1 : 0;
    uint32_t trt = 0;

    if (setup->Length > 0)
    {
        trt = is_in ? 3 : 2;
    }

    uint32_t setup_param_low = *(uint32_t *)setup;
    uint32_t setup_param_high = *(((uint32_t *)setup) + 1);

    xhci_queue_trb(slot_id,
                   setup_param_low,
                   setup_param_high,
                   8,
                   (TRB_TYPE_SETUP_STAGE << 10) | (1u << 6) | (trt << 16));

    if (setup->Length > 0 && data != 0)
    {
        uint64_t data_phys = paging_get_physical_address((uint64_t)data);
        uint32_t dir_bit = is_in ? (1u << 16) : 0u;

        xhci_queue_trb(slot_id,
                       (uint32_t)data_phys,
                       (uint32_t)(data_phys >> 32),
                       setup->Length,
                       (TRB_TYPE_DATA_STAGE << 10) | dir_bit);
    }

    uint32_t status_dir = 1;
    if (setup->Length > 0 && is_in)
    {
        status_dir = 0;
    }

    xhci_queue_trb(slot_id,
                   0, 0, 0,
                   (TRB_TYPE_STATUS_STAGE << 10) | (1u << 5) | (status_dir << 16));

    xhci_ring_ep_doorbell(slot_id, 1);

    int timeout = 3000;
    while (timeout-- > 0)
    {
        timer_sleep(1);

        xhci_process_events();

        if (g_ep0_waiting == 0)
        {
            if (g_ep0_result_cc == 1)
                return 1;

            console_write_debug("[USB] Control Transfer Fail. CC=");
            console_print_dec_debug(g_ep0_result_cc);
            console_write_debug("\n");
            return 0;
        }
    }

    g_ep0_waiting = 0;
    console_write_debug("[USB] Control Transfer Timeout (No Event).\n");
    return 0;
}

int xhci_configure_device_with_context(int port_id, int speed_id, usb_device_context_t *ctx)
{

    if (ctx == NULL)
    {
        xhci_configure_device(port_id, speed_id);
        return 0;
    }

    if (xhci_send_command_wait(TRB_TYPE_NOOP, 0, 0) == 0)
        return 0;

    uint8_t slot_id = xhci_send_command_wait(TRB_TYPE_ENABLE_SLOT, 0, 0);
    if (slot_id == 0)
        return 0;

    console_write_debug(" [XHCI] Device (via hub) at Slot ");
    console_print_dec_debug(slot_id);
    console_write_debug("\n");

    ctx->slot_id = slot_id;

    uint32_t hcc1 = xhci_driver.cap_regs->HccParams1;
    uint32_t ctx_size = ((hcc1 >> 2) & 1) ? 64 : 32;

    uint64_t out_bytes = ctx_size * 32;
    if (out_bytes < 4096)
        out_bytes = 4096;
    void *dev_ctx = kmalloc_aligned(out_bytes, 4096);
    uint64_t dev_phys = paging_get_physical_address((uint64_t)dev_ctx);
    paging_map((uint64_t)dev_ctx, dev_phys, PAGE_PRESENT | PAGE_RW);
    memset(dev_ctx, 0, out_bytes);
    xhci_driver.dcbaa[slot_id] = dev_phys;
    __asm__ volatile("clflush (%0)" ::"r"(&xhci_driver.dcbaa[slot_id]));
    __asm__ volatile("mfence" ::: "memory");

    uint64_t in_bytes = ctx_size * 33;
    if (in_bytes < 4096)
        in_bytes = 4096;
    void *input_ctx = kmalloc_aligned(in_bytes, 4096);
    uint64_t inp_phys = paging_get_physical_address((uint64_t)input_ctx);
    paging_map((uint64_t)input_ctx, inp_phys, PAGE_PRESENT | PAGE_RW);
    memset(input_ctx, 0, in_bytes);

    uint32_t *icc_add = (uint32_t *)((uint8_t *)input_ctx + 4);
    *icc_add = (1u << 0) | (1u << 1);

    uint32_t *slot_dw = (uint32_t *)((uint8_t *)input_ctx + ctx_size);

    slot_dw[0] = ((1u & 0x1Fu) << 27) |
                 (((uint32_t)speed_id & 0xFu) << 20) |
                 (ctx->route_string & 0xFFFFF);

    slot_dw[1] = ((uint32_t)ctx->root_port << 16);

    if (ctx->tt_hub_slot_id != 0)
    {
        slot_dw[2] = ((uint32_t)ctx->tt_hub_slot_id) |
                     ((uint32_t)ctx->tt_port_num << 8);

        console_write_debug("[XHCI] TT configured: hub_slot=");
        console_print_dec_debug(ctx->tt_hub_slot_id);
        console_write_debug(" port=");
        console_print_dec_debug(ctx->tt_port_num);
        console_write_debug("\n");
    }

    uint32_t *ep0_dw = (uint32_t *)((uint8_t *)input_ctx + (ctx_size * 2));

    void *tr_ring = kmalloc_aligned(4096, 4096);
    uint64_t tr_phys = paging_get_physical_address((uint64_t)tr_ring);
    paging_map((uint64_t)tr_ring, tr_phys, PAGE_PRESENT | PAGE_RW);
    memset(tr_ring, 0, 4096);

    xhci_trb_t *ring = (xhci_trb_t *)tr_ring;
    ring[255].Parameter = tr_phys;
    ring[255].Status = 0;
    ring[255].Control = (TRB_TYPE_LINK << 10) | (1 << 1) | 1;

    uint16_t initial_mps;
    switch (speed_id)
    {
    case 1:
        initial_mps = 8;
        break;
    case 2:
        initial_mps = 8;
        break;
    case 3:
        initial_mps = 64;
        break;
    case 4:
        initial_mps = 512;
        break;
    case 5:
        initial_mps = 512;
        break;
    default:
        initial_mps = 8;
        break;
    }

    ep0_dw[1] = (initial_mps << 16) | (4u << 3) | (3u << 1);
    ep0_dw[2] = (uint32_t)tr_phys | 1u;
    ep0_dw[3] = (uint32_t)(tr_phys >> 32);
    ep0_dw[4] = 8;

    for (uint64_t off = 0; off < in_bytes; off += 64)
        __asm__ volatile("clflush (%0)" ::"r"((uint64_t)input_ctx + off));
    __asm__ volatile("mfence" ::: "memory");

    if (speed_id <= 3)
        timer_sleep(20);

    uint8_t res = xhci_send_command_wait(TRB_TYPE_ADDRESS_DEVICE, inp_phys, (slot_id << 24));
    if (res != slot_id)
    {
        console_write_debug("-> Address Device Failed.\n");
        return 0;
    }

    xhci_driver.slot_ep0_rings[slot_id] = (xhci_trb_t *)tr_ring;
    xhci_driver.slot_ep0_enqueue[slot_id] = 0;
    xhci_driver.slot_ep0_cycle[slot_id] = 1;

    usb_hotplug_notify_root_device_configured((uint8_t)(port_id + 1), slot_id);

    if (speed_id < 3)
    {
        usb_device_descriptor_t desc_short;
        if (xhci_get_descriptor_device(slot_id, &desc_short, 8))
        {
            int real_mps = desc_short.MaxPacketSize0;
            if (real_mps > 0 && real_mps != initial_mps)
            {
                xhci_evaluate_context(slot_id, (uint16_t)real_mps, port_id, speed_id);
            }
        }
    }

    usb_device_descriptor_t desc;
    if (!xhci_get_descriptor_device(slot_id, &desc, 18))
    {
        console_write_debug(" -> Device Descriptor Failed.\n");
        return slot_id;
    }

    console_write_debug(" (VID=");
    console_print_hex_debug(desc.VendorID);
    console_write_debug(" PID=");
    console_print_hex_debug(desc.ProductID);
    console_write_debug(" Class=");
    console_print_hex_debug(desc.DeviceClass);
    console_write_debug(")\n");

    if (desc.DeviceClass == USB_CLASS_HUB)
    {
        console_set_color_debug(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
        console_write_debug("[XHCI] USB HUB detected! Enumerating...\n");
        console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

        int is_usb3 = (desc.DeviceProtocol >= 3) || (speed_id >= 4);

        usb_hub_enumerate(
            slot_id,
            ctx->root_port,
            ctx->route_string,
            ctx->hub_depth,
            ctx->parent_hub_slot,
            ctx->port_on_parent,
            is_usb3);

        return slot_id;
    }

    uint16_t conf_len = 0;
    void *conf_buf = xhci_get_config_descriptor(slot_id, &conf_len);

    if (conf_buf)
    {
        usb_device_info_t info;
        memset(&info, 0, sizeof(info));
        xhci_parse_config(conf_buf, conf_len, &info);

        if (info.found)
        {
            console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
            console_write_debug(" -> KEYBOARD DETECTED (via Hub).\n");
            console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

            if (xhci_set_configuration(slot_id, info.config_value))
            {
                console_write_debug("[XHCI][KBD] Using parsed endpoint: IF=");
                console_print_dec_debug(info.interface_num);
                console_write_debug(" EP=0x");
                console_print_hex_debug(info.endpoint_addr);
                console_write_debug(" MPS=");
                console_print_dec_debug(info.endpoint_mps);
                console_write_debug(" INT=");
                console_print_dec_debug(info.endpoint_interval);
                console_write_debug("\n");

                xhci_set_protocol(slot_id, info.interface_num, 0);
                xhci_set_idle(slot_id, info.interface_num, 0, 0);

                int result = xhci_configure_endpoint_irq(
                    slot_id,
                    port_id,
                    speed_id,
                    info.endpoint_addr,
                    info.endpoint_mps,
                    info.endpoint_interval);

                if (result == 0)
                {
                    console_write_debug("[XHCI] Endpoint configured successfully!\n");

                    xhci_driver.slot_kbd_pending[slot_id] = 0;

                    for (int q = 0; q < XHCI_KBD_PIPE_DEPTH; q++)
                    {
                        xhci_queue_kbd_request_buf(slot_id, xhci_driver.slot_kbd_buffers[slot_id][q]);
                    }

                    console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
                    console_write_debug("[XHCI] Keyboard ready (via Hub)!\n");
                    console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
                }
            }
        }
        else
        {
            console_write_debug(" -> Not a boot keyboard.\n");
        }
    }

    return slot_id;
}

void xhci_diag_latency(void)
{
    console_write_debug("\n========== XHCI LATENCY DIAGNOSTIC ==========\n");

    uint32_t usbsts = xhci_read32(&xhci_driver.op_regs->UsbSts);
    uint32_t usbcmd = xhci_read32(&xhci_driver.op_regs->UsbCmd);

    console_write_debug("UsbCmd = 0x");
    console_print_hex_debug(usbcmd);
    console_write_debug(" (RS=");
    console_print_dec_debug((usbcmd >> 0) & 1);
    console_write_debug(" INTE=");
    console_print_dec_debug((usbcmd >> 2) & 1);
    console_write_debug(")\n");

    console_write_debug("UsbSts = 0x");
    console_print_hex_debug(usbsts);
    console_write_debug(" (HCH=");
    console_print_dec_debug((usbsts >> 0) & 1);
    console_write_debug(" EINT=");
    console_print_dec_debug((usbsts >> 3) & 1);
    console_write_debug(")\n");

    volatile xhci_interrupter_regs_t *intr = &xhci_driver.run_regs->Interrupters[0];

    console_write_debug("IMAN  = 0x");
    console_print_hex_debug(intr->Iman);
    console_write_debug(" (IP=");
    console_print_dec_debug((intr->Iman >> 0) & 1);
    console_write_debug(" IE=");
    console_print_dec_debug((intr->Iman >> 1) & 1);
    console_write_debug(")\n");

    console_write_debug("IMOD  = 0x");
    console_print_hex_debug(intr->Imod);
    if (intr->Imod != 0)
    {
        console_write_debug(" !!! IMOD should be 0 for minimum latency !!!\n");
    }
    else
    {
        console_write_debug(" (OK)\n");
    }

    console_write_debug("\n--- Event Processing Stats ---\n");
    console_write_debug("ISR count:        ");
    console_print_dec_debug(xhci_dbg_isr_count);
    console_write_debug("\n");
    console_write_debug("Total events:     ");
    console_print_dec_debug(xhci_total_events_processed);
    console_write_debug("\n");
    console_write_debug("Total kbd events: ");
    console_print_dec_debug(xhci_total_kbd_events);
    console_write_debug("\n");

    static uint64_t last_diag_isr = 0;
    static uint64_t last_diag_kbd = 0;
    static uint64_t last_diag_ms = 0;

    uint64_t now = timer_get_uptime_ms();
    if (last_diag_ms > 0 && (now - last_diag_ms) > 0)
    {
        uint64_t delta_ms = now - last_diag_ms;
        uint64_t delta_isr = xhci_dbg_isr_count - last_diag_isr;
        uint64_t delta_kbd = xhci_total_kbd_events - last_diag_kbd;

        uint64_t isr_per_sec = (delta_isr * 1000) / delta_ms;
        uint64_t kbd_per_sec = (delta_kbd * 1000) / delta_ms;

        console_write_debug("ISR/sec (recent): ");
        console_print_dec_debug(isr_per_sec);
        console_write_debug("\n");

        console_write_debug("KBD/sec (recent): ");
        console_print_dec_debug(kbd_per_sec);
        console_write_debug("\n");

        if (isr_per_sec < 50 && xhci_total_kbd_events > 0)
        {
            console_write_debug("!!! WARNING: Low ISR rate - MSI may not be working!\n");
        }
    }

    last_diag_isr = xhci_dbg_isr_count;
    last_diag_kbd = xhci_total_kbd_events;
    last_diag_ms = now;

    console_write_debug("\n--- Keyboard Slots ---\n");
    for (int slot = 1; slot < 16; slot++)
    {
        if (xhci_driver.slot_kbd_dci[slot] == 0)
            continue;

        console_write_debug("Slot ");
        console_print_dec_debug(slot);
        console_write_debug(": DCI=");
        console_print_dec_debug(xhci_driver.slot_kbd_dci[slot]);
        console_write_debug(" enq=");
        console_print_dec_debug(xhci_driver.slot_kbd_enqueue[slot]);
        console_write_debug(" cyc=");
        console_print_dec_debug(xhci_driver.slot_kbd_cycle[slot]);
        console_write_debug(" pending=");
        console_print_dec_debug(xhci_driver.slot_kbd_pending[slot]);
        console_write_debug("\n");
    }

    console_write_debug("\n--- Event Ring ---\n");
    console_write_debug("Dequeue idx: ");
    console_print_dec_debug(xhci_driver.event_ring_dequeue_idx);
    console_write_debug(" / ");
    console_print_dec_debug(xhci_driver.event_ring_size);
    console_write_debug("\n");
    console_write_debug("Cycle bit:   ");
    console_print_dec_debug(xhci_driver.event_ring_cycle_bit);
    console_write_debug("\n");

    uint64_t erdp = intr->Erdp;
    console_write_debug("ERDP = 0x");
    console_print_hex_debug((uint32_t)(erdp >> 32));
    console_print_hex_debug((uint32_t)(erdp & 0xFFFFFFFF));
    console_write_debug("\n");

    volatile xhci_trb_t *evt = &xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx];
    __asm__ volatile("mfence" ::: "memory");

    uint8_t evt_cycle = evt->Control & 1;
    uint32_t evt_type = (evt->Control >> 10) & 0x3F;

    console_write_debug("Next event: cycle=");
    console_print_dec_debug(evt_cycle);
    console_write_debug(" type=");
    console_print_dec_debug(evt_type);

    if (evt_cycle == xhci_driver.event_ring_cycle_bit)
    {
        console_write_debug(" [PENDING!]\n");
    }
    else
    {
        console_write_debug(" [none]\n");
    }

    console_write_debug("==========================================\n");
}