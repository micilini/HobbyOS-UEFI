#include "../shared/protocol.h"
#include "src/graphics/graphics.h"
#include "src/core/kernel_init.h"
#include "src/core/panic.h"
#include "src/graphics/splash.h"
#include "src/graphics/terminal.h"
#include "src/core/shell.h"
#include "src/memory/heap.h"
#include "src/libc/memory.h"
#include "src/graphics/console.h" 


#define DESKTOP_BG_COLOR 0xFF0000AA 

void _start(BootInfo* boot_info) {
    if (!boot_info) return;

    
    init_graphics(boot_info->framebuffer, NULL);
    
    
    init_system_core(boot_info);

    
    play_splash_screen(boot_info);
    discard_splash_memory(boot_info);

    
    
    console_write("\n[DEBUG] Splash finalizado. Alocando 8MB para Video...\n");

    
    uint64_t fb_size = boot_info->framebuffer->Width * boot_info->framebuffer->Height * 4;
    void* back_buffer = kmalloc(fb_size);
    bool buffer_ok = false;

    if (back_buffer != NULL) {
        console_write("[DEBUG] Memoria alocada! Testando acesso...\n");
        
        
        uint8_t* test_ptr = (uint8_t*)back_buffer;
        test_ptr[0] = 0; 
        test_ptr[fb_size - 1] = 0;
        
        console_write("[DEBUG] Memoria segura. Pintando fundo...\n");

        
        uint32_t* pixel_ptr = (uint32_t*)back_buffer;
        uint64_t total_pixels = fb_size / 4;
        for(uint64_t i=0; i < total_pixels; i++) {
             pixel_ptr[i] = DESKTOP_BG_COLOR;
        }

        init_graphics(boot_info->framebuffer, back_buffer);
        graphics_enable_buffering(true);
        buffer_ok = true;
    } else {
        console_write("[ERRO] Falha no kmalloc! Heap cheio?\n");
        
        for(volatile int i=0; i<100000000; i++); 
    }

    
    if (buffer_ok) {
        swap_buffers(); 
    } else {
        clear_screen(DESKTOP_BG_COLOR);
    }
    
    
    init_terminal();
    shell_init();

    while(1) {
        __asm__("hlt");
    }
}