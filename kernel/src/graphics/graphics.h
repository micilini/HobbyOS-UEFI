#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "../../../shared/protocol.h"
#include <stdint.h>
#include <stdbool.h>

#define COLOR_BLACK 0x00000000
#define COLOR_WHITE 0x00FFFFFF
#define COLOR_BLUE 0x00111133
#define COLOR_GREEN 0x0000FF00

void init_graphics(Framebuffer *fb, void *back_buffer);
bool graphics_bind_framebuffer(Framebuffer *fb, void *back_buffer);
bool graphics_framebuffer_is_bound(void);
Framebuffer *graphics_framebuffer(void);
bool graphics_binding_selftest(void);

void *get_draw_buffer();

void swap_buffers();

void graphics_enable_buffering(bool enable);

void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void clear_screen(uint32_t color);
void draw_overlay_image(SimpleImage *img, uint32_t x, uint32_t y, uint8_t opacity);

#endif
