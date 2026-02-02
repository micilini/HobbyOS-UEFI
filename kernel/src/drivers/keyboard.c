#include "keyboard.h"
#include "ps2.h"
#include "../libc/string.h"
#include "../shell/shell.h"
#include "../core/io.h"
#include "../core/spinlock.h"
#include "../core/semaphore.h"

static bool g_shift = false;
static bool g_capslock = false;
static bool g_ctrl = false;
static bool g_alt = false;
static bool g_e0_prefix = false;

static semaphore_t g_sem_kbd;

static bool g_stop_repeating = false;

typedef struct
{
    uint8_t value;
    uint8_t is_special;
} kbd_evt_t;

#define USB_KBD_FIFO_SIZE 512

static volatile kbd_evt_t g_usb_fifo[USB_KBD_FIFO_SIZE];
static volatile uint32_t g_usb_fifo_head = 0;
static volatile uint32_t g_usb_fifo_tail = 0;
static volatile uint32_t g_usb_fifo_drops = 0;

static spinlock_t g_usb_fifo_lock;

static inline void usb_fifo_push(uint8_t value, uint8_t is_special)
{
    irq_flags_t flags = spin_lock_irqsave(&g_usb_fifo_lock);

    uint32_t head = g_usb_fifo_head;
    uint32_t next = (head + 1) % USB_KBD_FIFO_SIZE;

    if (next == g_usb_fifo_tail)
    {
        g_usb_fifo_drops++;
        spin_unlock_irqrestore(&g_usb_fifo_lock, flags);
        return;
    }

    g_usb_fifo[head].value = value;
    g_usb_fifo[head].is_special = is_special;
    __asm__ volatile("" ::: "memory");

    g_usb_fifo_head = next;

    spin_unlock_irqrestore(&g_usb_fifo_lock, flags);

    sem_signal(&g_sem_kbd);
}

static void drain_keyboard_buffer(void)
{
    while (1)
    {
        kbd_evt_t ev;
        irq_flags_t flags = spin_lock_irqsave(&g_usb_fifo_lock);

        if (g_usb_fifo_tail == g_usb_fifo_head)
        {
            spin_unlock_irqrestore(&g_usb_fifo_lock, flags);
            break; 
        }

        ev = g_usb_fifo[g_usb_fifo_tail];
        g_usb_fifo_tail = (g_usb_fifo_tail + 1) % USB_KBD_FIFO_SIZE;

        spin_unlock_irqrestore(&g_usb_fifo_lock, flags);

        
        if (ev.is_special)
            shell_receive_special(ev.value);
        else
            shell_receive_char((char)ev.value);
    }
}

void input_thread_entry(void *arg)
{
    (void)arg;
    console_write_debug("[INPUT] Input Thread Started (Waiting for keys).\n");

    while (1)
    {        
        sem_wait(&g_sem_kbd);

        drain_keyboard_buffer();
    }
}

uint32_t keyboard_usb_fifo_drops(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_usb_fifo_lock);
    uint32_t v = (uint32_t)g_usb_fifo_drops;
    spin_unlock_irqrestore(&g_usb_fifo_lock, flags);
    return v;
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
};

void keyboard_init()
{
    spinlock_init(&g_usb_fifo_lock);

    sem_init(&g_sem_kbd, 0);

    g_usb_fifo_head = 0;
    g_usb_fifo_tail = 0;
    g_usb_fifo_drops = 0;

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
        if (ascii >= KEY_SPECIAL_LEFT && ascii <= KEY_SPECIAL_DOWN)
        {

            usb_fifo_push((uint8_t)ascii, 1);
        }
        else
        {

            usb_fifo_push((uint8_t)ascii, 0);
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