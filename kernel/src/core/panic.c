#include "panic.h"
#include "../graphics/console.h" 


#define COLOR_BSOD_BG 0xFF0000AA 
#define COLOR_BSOD_FG 0xFFFFFFFF

void kpanic(char* message) {
    
    __asm__ volatile ("cli");

    
    
    console_clear(COLOR_BSOD_BG);
    console_set_color(COLOR_BSOD_FG, COLOR_BSOD_BG);

    
    console_write("\n\n");
    console_write("  :(  HobbyOS Kernel Panic\n");
    console_write("  ========================\n\n");

    
    console_write("  Error: ");
    console_write(message);
    console_write("\n\n");
    
    console_write("  System Halted. Please restart your computer manually.");

    
    while(1) {
        __asm__ volatile ("hlt");
    }
}