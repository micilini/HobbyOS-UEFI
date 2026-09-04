#include "keyboard.h"
#include "ps2.h"
#include "../libc/string.h"
#include "../shell/shell.h"
#include "../core/io.h"
#include "../core/input_queue.h"
#include "../core/input_router.h"
#include "../core/scheduler.h"
#include "../graphics/console.h"
#include "serial.h"

static bool g_shift = false;
static bool g_capslock = false;
static bool g_ctrl = false;
static bool g_alt = false;
static bool g_e0_prefix = false;

static bool g_stop_repeating = false;

#define USB_KBD_FIFO_SIZE 512

static input_queue_entry_t g_keyboard_storage[USB_KBD_FIFO_SIZE];
static input_queue_t g_keyboard_queue;

static void kbd_trace_char_event(const char *stage, uint8_t raw, uint8_t ascii, uint8_t is_special)
{
    if (!(input_debug_get_trace_flags() & INPUT_TRACE_KEYBOARD))
        return;
    serial_write_all("[INPUT-TRACE][KBD]");
    serial_write_all(stage ? stage : "");
    serial_write_all(" raw=0x");
    serial_write_hex64_all((uint64_t)raw);

    serial_write_all(" type=");
    serial_write_all(is_special ? "SPECIAL" : "CHAR");

    serial_write_all(" val=");
    if (!is_special)
    {
        if (ascii == '\n')
            serial_write_all("<ENTER>");
        else if (ascii == '\r')
            serial_write_all("<CR>");
        else if (ascii == '\b')
            serial_write_all("<BKSP>");
        else if (ascii == '\t')
            serial_write_all("<TAB>");
        else if (ascii == 27)
            serial_write_all("<ESC>");
        else if (ascii < 32 || ascii > 126)
        {
            serial_write_all("0x");
            serial_write_hex64_all((uint64_t)ascii);
        }
        else
        {
            serial_putc_all('\'');
            serial_putc_all((char)ascii);
            serial_putc_all('\'');
        }
    }
    else
    {
        serial_write_all("0x");
        serial_write_hex64_all((uint64_t)ascii);
    }

    serial_write_all("\n");
}

static inline void usb_fifo_push(uint8_t value, uint8_t is_special)
{
    input_event_t event = is_special ? input_event_special(value)
                                     : input_event_char((char)value);
    (void)input_queue_push(&g_keyboard_queue, event, 1,
                           INPUT_DEST_KEYBOARD_FIFO, NULL);
}

void input_thread_entry(void *arg)
{
    (void)arg;
    console_write_debug("[INPUT] Input Thread Started (Waiting for keys).\n");

    while (1)
    {
        input_event_t event;
        input_queue_pop_result_t result = input_queue_wait_pop(&g_keyboard_queue,
                                                               &event);
        if (result == INPUT_QUEUE_POP_CANCELLED)
            task_cancel_point();
        if (result != INPUT_QUEUE_POP_OK)
            continue;
        input_router_dispatch_event(event);
        while (input_queue_try_pop(&g_keyboard_queue, &event) == INPUT_QUEUE_POP_OK)
            input_router_dispatch_event(event);
    }
}

uint32_t keyboard_usb_fifo_drops(void)
{
    input_queue_stats_t stats;
    return input_queue_snapshot(&g_keyboard_queue, &stats)
               ? (uint32_t)stats.drops : 0;
}

bool keyboard_input_queue_snapshot(input_queue_stats_t *out)
{
    return input_queue_snapshot(&g_keyboard_queue, out);
}

bool keyboard_input_queue_validate(input_queue_validation_t *out)
{
    return input_queue_validate(&g_keyboard_queue, out);
}

bool keyboard_test_enqueue_event(input_event_t event)
{
    return input_queue_push(&g_keyboard_queue, event, 1,
                            INPUT_DEST_KEYBOARD_FIFO, NULL) == INPUT_QUEUE_PUSH_OK;
}

static char apply_modifiers_letter(char lower, char upper)
{
    bool upper_mode = g_shift;
    if (g_capslock)
        upper_mode = !upper_mode;
    return upper_mode ? upper : lower;
}

static char apply_modifiers_symbol(char normal, char shifted)
{
    return g_shift ? shifted : normal;
}

static char ps2_scancode_to_ascii(uint8_t code)
{

    if (code == 0x1E)
        return 'a';
    if (code == 0x30)
        return 'b';
    if (code == 0x1C)
        return '\n';
    if (code == 0x39)
        return ' ';
    if (code == 0x0E)
        return '\b';

    return 0;
}

static const uint8_t usb_hid_map[128][2] = {
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {'a', 'A'},
    {'b', 'B'},
    {'c', 'C'},
    {'d', 'D'},
    {'e', 'E'},
    {'f', 'F'},
    {'g', 'G'},
    {'h', 'H'},
    {'i', 'I'},
    {'j', 'J'},
    {'k', 'K'},
    {'l', 'L'},
    {'m', 'M'},
    {'n', 'N'},
    {'o', 'O'},
    {'p', 'P'},
    {'q', 'Q'},
    {'r', 'R'},
    {'s', 'S'},
    {'t', 'T'},
    {'u', 'U'},
    {'v', 'V'},
    {'w', 'W'},
    {'x', 'X'},
    {'y', 'Y'},
    {'z', 'Z'},
    {'1', '!'},
    {'2', '@'},
    {'3', '#'},
    {'4', '$'},
    {'5', '%'},
    {'6', '^'},
    {'7', '&'},
    {'8', '*'},
    {'9', '('},
    {'0', ')'},
    {'\n', '\n'},
    {0x1B, 0x1B},
    {'\b', '\b'},
    {'\t', '\t'},
    {' ', ' '},
    {'-', '_'},
    {'=', '+'},
    {'[', '{'},
    {']', '}'},
    {'\\', '|'},
    {'#', '~'},
    {';', ':'},
    {'\'', '"'},
    {'`', '~'},
    {',', '<'},
    {'.', '>'},
    {'/', '?'},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {KEY_SPECIAL_RIGHT, KEY_SPECIAL_RIGHT},
    {KEY_SPECIAL_LEFT, KEY_SPECIAL_LEFT},
    {KEY_SPECIAL_DOWN, KEY_SPECIAL_DOWN},
    {KEY_SPECIAL_UP, KEY_SPECIAL_UP},
    [0x4A] = {KEY_SPECIAL_HOME, KEY_SPECIAL_HOME},
    [0x4B] = {KEY_SPECIAL_PAGE_UP, KEY_SPECIAL_PAGE_UP},
    [0x4D] = {KEY_SPECIAL_END, KEY_SPECIAL_END},
    [0x4E] = {KEY_SPECIAL_PAGE_DOWN, KEY_SPECIAL_PAGE_DOWN},
};

void keyboard_init()
{
    input_queue_init(&g_keyboard_queue, g_keyboard_storage,
                     USB_KBD_FIFO_SIZE, "keyboard-fifo");

    g_shift = false;
    g_capslock = false;
    g_ctrl = false;
    g_alt = false;
    g_e0_prefix = false;
    g_stop_repeating = false;
}

void keyboard_push_usb_event(uint8_t modifiers, uint8_t keycode)
{
    if (keycode == 0 || keycode > 127)
        return;

    bool is_shift = (modifiers & (1 << 1)) || (modifiers & (1 << 5));

    if (keycode == 0x39)
    {
        g_capslock = !g_capslock;
        if (input_debug_get_trace_flags() & INPUT_TRACE_KEYBOARD)
            serial_write_all("[INPUT-TRACE][KBD] usb capslock toggle\n");
        return;
    }

    bool is_upper = is_shift;

    if (keycode >= 0x04 && keycode <= 0x1D)
    {
        if (g_capslock)
            is_upper = !is_upper;
    }

    uint8_t ascii = usb_hid_map[keycode][is_upper ? 1 : 0];

    if (ascii != 0)
    {
        if (ascii >= KEY_SPECIAL_LEFT && ascii <= KEY_SPECIAL_END)
        {
            kbd_trace_char_event(" usb->fifo", keycode, ascii, 1);
            usb_fifo_push((uint8_t)ascii, 1);
        }
        else
        {
            kbd_trace_char_event(" usb->fifo", keycode, ascii, 0);
            usb_fifo_push((uint8_t)ascii, 0);
        }
    }
    else
    {
        if (input_debug_get_trace_flags() & INPUT_TRACE_KEYBOARD) {
            serial_write_all("[INPUT-TRACE][KBD] usb unmapped keycode=0x");
            serial_write_hex64_all((uint64_t)keycode);
            serial_write_all("\n");
        }
    }
}

static char scancode_to_ascii(uint8_t code)
{
    if (code == 0xE0)
    {
        g_e0_prefix = true;
        return 0;
    }

    bool is_extended = g_e0_prefix;
    g_e0_prefix = false;

    if (code & 0x80)
    {
        uint8_t released_code = code & 0x7F;

        g_stop_repeating = true;

        if (released_code == 0x2A || released_code == 0x36)
            g_shift = false;
        if (released_code == 0x1D)
            g_ctrl = false;
        if (released_code == 0x38)
            g_alt = false;

        return 0;
    }

    g_stop_repeating = false;

    if (code == 0x2A || code == 0x36)
    {
        g_shift = true;
        return 0;
    }
    if (code == 0x3A)
    {
        g_capslock = !g_capslock;
        return 0;
    }
    if (code == 0x1D)
    {
        g_ctrl = true;
        return 0;
    }
    if (code == 0x38)
    {
        g_alt = true;
        return 0;
    }

    if (is_extended)
    {
        switch (code)
        {
        case 0x4B:
            return KEY_SPECIAL_LEFT;
        case 0x4D:
            return KEY_SPECIAL_RIGHT;
        case 0x48:
            return KEY_SPECIAL_UP;
        case 0x50:
            return KEY_SPECIAL_DOWN;
        case 0x47:
            return KEY_SPECIAL_HOME;
        case 0x4F:
            return KEY_SPECIAL_END;
        case 0x49:
            return KEY_SPECIAL_PAGE_UP;
        case 0x51:
            return KEY_SPECIAL_PAGE_DOWN;
        }
        return 0;
    }

    switch (code)
    {
    case 0x29:
        return apply_modifiers_symbol('`', '~');
    case 0x02:
        return apply_modifiers_symbol('1', '!');
    case 0x03:
        return apply_modifiers_symbol('2', '@');
    case 0x04:
        return apply_modifiers_symbol('3', '#');
    case 0x05:
        return apply_modifiers_symbol('4', '$');
    case 0x06:
        return apply_modifiers_symbol('5', '%');
    case 0x07:
        return apply_modifiers_symbol('6', '^');
    case 0x08:
        return apply_modifiers_symbol('7', '&');
    case 0x09:
        return apply_modifiers_symbol('8', '*');
    case 0x0A:
        return apply_modifiers_symbol('9', '(');
    case 0x0B:
        return apply_modifiers_symbol('0', ')');
    case 0x0C:
        return apply_modifiers_symbol('-', '_');
    case 0x0D:
        return apply_modifiers_symbol('=', '+');
    case 0x0E:
        return '\b';
    case 0x0F:
        return '\t';

    case 0x10:
        return apply_modifiers_letter('q', 'Q');
    case 0x11:
        return apply_modifiers_letter('w', 'W');
    case 0x12:
        return apply_modifiers_letter('e', 'E');
    case 0x13:
        return apply_modifiers_letter('r', 'R');
    case 0x14:
        return apply_modifiers_letter('t', 'T');
    case 0x15:
        return apply_modifiers_letter('y', 'Y');
    case 0x16:
        return apply_modifiers_letter('u', 'U');
    case 0x17:
        return apply_modifiers_letter('i', 'I');
    case 0x18:
        return apply_modifiers_letter('o', 'O');
    case 0x19:
        return apply_modifiers_letter('p', 'P');
    case 0x1A:
        return apply_modifiers_symbol('[', '{');
    case 0x1B:
        return apply_modifiers_symbol(']', '}');
    case 0x2B:
        return apply_modifiers_symbol('\\', '|');

    case 0x1E:
        return apply_modifiers_letter('a', 'A');
    case 0x1F:
        return apply_modifiers_letter('s', 'S');
    case 0x20:
        return apply_modifiers_letter('d', 'D');
    case 0x21:
        return apply_modifiers_letter('f', 'F');
    case 0x22:
        return apply_modifiers_letter('g', 'G');
    case 0x23:
        return apply_modifiers_letter('h', 'H');
    case 0x24:
        return apply_modifiers_letter('j', 'J');
    case 0x25:
        return apply_modifiers_letter('k', 'K');
    case 0x26:
        return apply_modifiers_letter('l', 'L');
    case 0x27:
        return apply_modifiers_symbol(';', ':');
    case 0x28:
        return apply_modifiers_symbol('\'', '"');
    case 0x1C:
        return '\n';

    case 0x2C:
        return apply_modifiers_letter('z', 'Z');
    case 0x2D:
        return apply_modifiers_letter('x', 'X');
    case 0x2E:
        return apply_modifiers_letter('c', 'C');
    case 0x2F:
        return apply_modifiers_letter('v', 'V');
    case 0x30:
        return apply_modifiers_letter('b', 'B');
    case 0x31:
        return apply_modifiers_letter('n', 'N');
    case 0x32:
        return apply_modifiers_letter('m', 'M');
    case 0x33:
        return apply_modifiers_symbol(',', '<');
    case 0x34:
        return apply_modifiers_symbol('.', '>');
    case 0x35:
        return apply_modifiers_symbol('/', '?');
    case 0x39:
        return ' ';

    default:
        return 0;
    }
}

void keyboard_handle_interrupt()
{
    while (inb(0x64) & 1)
    {
        uint8_t scancode = inb(0x60);
        char c = scancode_to_ascii(scancode);

        if (c)
        {
            if ((uint8_t)c >= 0xF1 && (uint8_t)c <= 0xF4)
                usb_fifo_push((uint8_t)c, 1);
            else
                usb_fifo_push((uint8_t)c, 0);
        }
    }
}
