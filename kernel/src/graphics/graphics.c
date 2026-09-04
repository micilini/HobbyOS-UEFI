#include "graphics.h"
#include "../libc/memory.h"

static Framebuffer *g_fb = NULL;
static uint32_t *g_back_buffer = NULL;
static bool g_buffering_enabled = false;

extern volatile int g_panic_in_progress;

bool graphics_bind_framebuffer(Framebuffer *fb, void *back_buffer)
{
    if (!fb || !fb->BaseAddress || !fb->Width || !fb->Height ||
        !fb->PixelsPerScanLine)
        return false;
    if (g_fb && g_fb != fb)
        return false;
    g_fb = fb;
    if (back_buffer || !g_back_buffer)
        g_back_buffer = (uint32_t *)back_buffer;
    return true;
}

void init_graphics(Framebuffer *fb, void *back_buffer)
{
    (void)graphics_bind_framebuffer(fb, back_buffer);
}

bool graphics_framebuffer_is_bound(void)
{
    return g_fb != NULL;
}

Framebuffer *graphics_framebuffer(void)
{
    return g_fb;
}

bool graphics_binding_selftest(void)
{
    Framebuffer *bound = g_fb;
    return bound && graphics_bind_framebuffer(bound, NULL) &&
           g_fb == bound && !graphics_bind_framebuffer(NULL, NULL);
}

void graphics_enable_buffering(bool enable)
{
    if (enable && g_back_buffer)
    {
        g_buffering_enabled = true;
    }
    else
    {
        g_buffering_enabled = false;
    }
}

void *get_draw_buffer()
{
    return (void *)g_back_buffer;
}

void swap_buffers()
{

    if (g_panic_in_progress)
        return;

    if (!g_buffering_enabled)
        return;

    if (!g_fb || !g_back_buffer)
        return;

    uint64_t buffer_size = g_fb->Height * g_fb->PixelsPerScanLine * 4;
    memcpy((void *)g_fb->BaseAddress, (void *)g_back_buffer, buffer_size);
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!g_fb)
        return;

    uint32_t max_x = g_fb->Width;
    if (g_fb->PixelsPerScanLine && g_fb->PixelsPerScanLine < max_x)
    {
        max_x = g_fb->PixelsPerScanLine;
    }

    if (x >= max_x || y >= g_fb->Height)
        return;

    if (g_back_buffer && g_buffering_enabled)
    {
        g_back_buffer[y * g_fb->PixelsPerScanLine + x] = color;
    }
    else
    {
        uint32_t *screen = (uint32_t *)g_fb->BaseAddress;
        screen[y * g_fb->PixelsPerScanLine + x] = color;
    }
}

void clear_screen(uint32_t color)
{
    if (!g_fb)
        return;

    if (g_back_buffer && g_buffering_enabled && !g_panic_in_progress)
    {
        uint64_t total = g_fb->Height * g_fb->PixelsPerScanLine;
        for (uint64_t i = 0; i < total; i++)
            g_back_buffer[i] = color;
    }
    else
    {
        uint32_t *screen = (uint32_t *)g_fb->BaseAddress;
        uint64_t total = g_fb->Height * g_fb->PixelsPerScanLine;
        for (uint64_t i = 0; i < total; i++)
            screen[i] = color;
    }
}

void draw_overlay_image(SimpleImage *img, uint32_t x, uint32_t y, uint8_t opacity)
{
    if (!g_fb || !img || !img->PixelBuffer)
        return;

    uint8_t *fileData = (uint8_t *)img->PixelBuffer;
    int bytesPerPixel = 3;
    if (img->Size >= (img->Width * img->Height * 4))
        bytesPerPixel = 4;

    for (uint32_t iy = 0; iy < img->Height; iy++)
    {
        for (uint32_t ix = 0; ix < img->Width; ix++)
        {

            uint32_t fileOffset = ((img->Height - 1 - iy) * img->Width + ix) * bytesPerPixel;

            uint8_t b = fileData[fileOffset + 0];
            uint8_t g = fileData[fileOffset + 1];
            uint8_t r = fileData[fileOffset + 2];

            uint8_t final_r = (r * opacity) / 100;
            uint8_t final_g = (g * opacity) / 100;
            uint8_t final_b = (b * opacity) / 100;

            uint32_t finalColor = (final_r << 16) | (final_g << 8) | final_b;

            put_pixel(x + ix, y + iy, finalColor);
        }
    }
}
