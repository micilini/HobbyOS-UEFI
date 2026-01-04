#include "splash.h"
#include "graphics.h"
#include "../timer/hpet.h"
#include "../memory/pmem.h"


#ifndef COLOR_BLACK
#define COLOR_BLACK 0x00000000
#endif

void play_splash_screen(BootInfo* boot_info) {
    if (!boot_info->logo) return;

    
    
    clear_screen(COLOR_BLACK);

    SimpleImage* logo = boot_info->logo;
    uint32_t sx = (boot_info->framebuffer->Width - logo->Width) / 2;
    uint32_t sy = (boot_info->framebuffer->Height - logo->Height) / 2;

    
    for (int op = 0; op <= 100; op += 2) {
        draw_overlay_image(logo, sx, sy, op);
        hpet_usleep(10000); 
    }

    hpet_usleep(2000000); 

    
    for (int op = 100; op >= 0; op -= 2) {
        draw_overlay_image(logo, sx, sy, op);
        hpet_usleep(10000);
    }
}

void discard_splash_memory(BootInfo* boot_info) {
    if (!boot_info->logo) return;

    SimpleImage* logo = boot_info->logo;
    
    
    uint64_t start_addr = (uint64_t)logo->PixelBuffer;
    uint64_t size = logo->Size;
    uint64_t end_addr = start_addr + size;

    
    
    
    for (uint64_t addr = start_addr; addr < end_addr; addr += 4096) {
        pmm_free_frame((void*)addr);
    }

    
    boot_info->logo = NULL;
}