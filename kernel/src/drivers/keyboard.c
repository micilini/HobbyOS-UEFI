#include "keyboard.h"
#include "ps2.h"
#include "../libc/string.h" 
#include "../core/shell.h"
#include "../core/io.h"


static bool g_shift = false;
static bool g_capslock = false;
static bool g_ctrl = false;
static bool g_alt = false;
static bool g_e0_prefix = false;



static bool g_stop_repeating = false;


static char apply_modifiers_letter(char lower, char upper) {
    bool upper_mode = g_shift;
    if (g_capslock) upper_mode = !upper_mode; 
    return upper_mode ? upper : lower;
}


static char apply_modifiers_symbol(char normal, char shifted) {
    return g_shift ? shifted : normal;
}

void keyboard_init() {
    
    while(ps2_has_data()) {
        ps2_read_data();
    }
    
    g_shift = false;
    g_capslock = false;
    g_ctrl = false;
    g_alt = false;
    g_e0_prefix = false;
    g_stop_repeating = false;
}


static char scancode_to_ascii(uint8_t code) {
    if (code == 0xE0) {
        g_e0_prefix = true;
        return 0;
    }

    bool is_extended = g_e0_prefix;
    g_e0_prefix = false; 

    
    if (code & 0x80) {
        uint8_t released_code = code & 0x7F; 

        
        g_stop_repeating = true;

        if (released_code == 0x2A || released_code == 0x36) g_shift = false;
        if (released_code == 0x1D) g_ctrl = false; 
        if (released_code == 0x38) g_alt = false;  
        
        return 0; 
    }

    
    
    g_stop_repeating = false;

    if (code == 0x2A || code == 0x36) { g_shift = true; return 0; }
    if (code == 0x3A) { g_capslock = !g_capslock; return 0; }
    if (code == 0x1D) { g_ctrl = true; return 0; }
    if (code == 0x38) { g_alt = true; return 0; }

    if (is_extended) {
        switch (code) {
            case 0x4B: return KEY_SPECIAL_LEFT;
            case 0x4D: return KEY_SPECIAL_RIGHT;
            case 0x48: return KEY_SPECIAL_UP;
            case 0x50: return KEY_SPECIAL_DOWN;
        }
        return 0; 
    }

    switch (code) {
        case 0x29: return apply_modifiers_symbol('`', '~');
        case 0x02: return apply_modifiers_symbol('1', '!');
        case 0x03: return apply_modifiers_symbol('2', '@');
        case 0x04: return apply_modifiers_symbol('3', '#');
        case 0x05: return apply_modifiers_symbol('4', '$');
        case 0x06: return apply_modifiers_symbol('5', '%');
        case 0x07: return apply_modifiers_symbol('6', '^');
        case 0x08: return apply_modifiers_symbol('7', '&');
        case 0x09: return apply_modifiers_symbol('8', '*');
        case 0x0A: return apply_modifiers_symbol('9', '(');
        case 0x0B: return apply_modifiers_symbol('0', ')');
        case 0x0C: return apply_modifiers_symbol('-', '_');
        case 0x0D: return apply_modifiers_symbol('=', '+');
        case 0x0E: return '\b'; 
        case 0x0F: return '\t'; 

        case 0x10: return apply_modifiers_letter('q', 'Q');
        case 0x11: return apply_modifiers_letter('w', 'W');
        case 0x12: return apply_modifiers_letter('e', 'E');
        case 0x13: return apply_modifiers_letter('r', 'R');
        case 0x14: return apply_modifiers_letter('t', 'T');
        case 0x15: return apply_modifiers_letter('y', 'Y');
        case 0x16: return apply_modifiers_letter('u', 'U');
        case 0x17: return apply_modifiers_letter('i', 'I');
        case 0x18: return apply_modifiers_letter('o', 'O');
        case 0x19: return apply_modifiers_letter('p', 'P');
        case 0x1A: return apply_modifiers_symbol('[', '{');
        case 0x1B: return apply_modifiers_symbol(']', '}');
        case 0x2B: return apply_modifiers_symbol('\\', '|');

        case 0x1E: return apply_modifiers_letter('a', 'A');
        case 0x1F: return apply_modifiers_letter('s', 'S');
        case 0x20: return apply_modifiers_letter('d', 'D');
        case 0x21: return apply_modifiers_letter('f', 'F');
        case 0x22: return apply_modifiers_letter('g', 'G');
        case 0x23: return apply_modifiers_letter('h', 'H');
        case 0x24: return apply_modifiers_letter('j', 'J');
        case 0x25: return apply_modifiers_letter('k', 'K');
        case 0x26: return apply_modifiers_letter('l', 'L');
        case 0x27: return apply_modifiers_symbol(';', ':');
        case 0x28: return apply_modifiers_symbol('\'', '"');
        case 0x1C: return '\n'; 

        case 0x2C: return apply_modifiers_letter('z', 'Z');
        case 0x2D: return apply_modifiers_letter('x', 'X');
        case 0x2E: return apply_modifiers_letter('c', 'C');
        case 0x2F: return apply_modifiers_letter('v', 'V');
        case 0x30: return apply_modifiers_letter('b', 'B');
        case 0x31: return apply_modifiers_letter('n', 'N');
        case 0x32: return apply_modifiers_letter('m', 'M');
        case 0x33: return apply_modifiers_symbol(',', '<');
        case 0x34: return apply_modifiers_symbol('.', '>');
        case 0x35: return apply_modifiers_symbol('/', '?');
        case 0x39: return ' '; 

        default: return 0;
    }
}

void keyboard_handle_interrupt() {
    
    while (ps2_has_data()) {
        uint8_t scancode = inb(PS2_DATA_PORT); 
        
        
        unsigned char ascii = (unsigned char)scancode_to_ascii(scancode);
        
        
        
        
        if (g_stop_repeating && (ascii != 0)) {
            continue; 
        }
        
        if (ascii != 0) {
            if (ascii >= 0xF1 && ascii <= 0xF4) {
                shell_receive_special(ascii);
            } else {
                shell_receive_char((char)ascii);
            }
        }
    }
}