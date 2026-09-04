#include "splash.h"
#include "graphics.h"
#include "../memory/pmem.h"
#include "console.h"
#include "../core/clock.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifndef COLOR_BLACK
#define COLOR_BLACK 0x00000000
#endif

#define SPLASH_FADE_MS 500u
#define SPLASH_HOLD_MS 2000u
#define SPLASH_TOTAL_BUDGET_MS 10000u
#define SPLASH_STALL_SAMPLE_LIMIT 20000u
#define SPLASH_ITERATION_LIMIT 10000000u
#define SPLASH_RENDER_CALL_LIMIT 32u
#define SPLASH_MAX_IMAGE_PIXELS 4194304u
#define SPLASH_OPACITY_BUCKET 10u
#define SPLASH_CADENCE_MIN_MONOTONIC_MS 10000u
#define SPLASH_CADENCE_MIN_FRACTION 4u

typedef struct {
    SimpleImage *logo;
    Framebuffer *framebuffer;
    uint32_t x;
    uint32_t y;
    uint8_t bytes_per_pixel;
} splash_plan_t;

typedef struct {
    uint64_t monotonic_start_ms;
    uint64_t ticks_start;
    uint64_t last_monotonic_ms;
    uint64_t last_ticks;
    uint64_t elapsed_ms;
    uint32_t stagnant_samples;
    uint32_t iterations;
    uint32_t renders;
    uint32_t period_us;
} splash_timing_t;

typedef enum {
    SPLASH_WAIT_CONTINUE = 0,
    SPLASH_WAIT_CLOCK_STALL,
    SPLASH_WAIT_RENDER_BUDGET,
} splash_wait_decision_t;

static char *splash_append_text(char *out, const char *text)
{
    while (*text)
        *out++ = *text++;
    return out;
}

static char *splash_append_u64(char *out, uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        *out++ = reverse[--count];
    return out;
}

static void splash_marker(const char *event, int progress,
                          const char *suffix)
{
    char line[224];
    char *p = splash_append_text(line, "[GRAPHICS][SPLASH] ");
    p = splash_append_text(p, event);
    if (progress >= 0) {
        p = splash_append_text(p, " progress=");
        p = splash_append_u64(p, (uint64_t)progress);
    }
    if (suffix && *suffix) {
        p = splash_append_text(p, " ");
        p = splash_append_text(p, suffix);
    }
    p = splash_append_text(p, " stage=");
    p = splash_append_text(p, event);
    p = splash_append_text(p, " monotonic_ms=");
    p = splash_append_u64(p, clock_monotonic_ms());
    timer_clockevent_snapshot_t snapshot = {0};
    (void)timer_clockevent_snapshot(&snapshot);
    p = splash_append_text(p, " clockevent_ticks=");
    p = splash_append_u64(p, snapshot.total_ticks);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

const char *splash_result_name(splash_result_t result)
{
    switch (result) {
        case SPLASH_RESULT_COMPLETE:
            return "COMPLETE";
        case SPLASH_RESULT_SKIPPED_NO_LOGO:
            return "SKIPPED_NO_LOGO";
        case SPLASH_RESULT_SKIPPED_INVALID_IMAGE:
            return "SKIPPED_INVALID_IMAGE";
        case SPLASH_RESULT_ABORTED_CLOCK_STALL:
            return "ABORTED_CLOCK_STALL";
        case SPLASH_RESULT_ABORTED_RENDER_BUDGET:
            return "ABORTED_RENDER_BUDGET";
        case SPLASH_RESULT_ABORTED_CONSOLE_BUSY:
            return "ABORTED_CONSOLE_BUSY";
    }
    return "ABORTED_RENDER_BUDGET";
}

static splash_result_t splash_validate(const BootInfo *boot_info,
                                       splash_plan_t *out)
{
    if (!boot_info || !out || !boot_info->framebuffer)
        return SPLASH_RESULT_SKIPPED_INVALID_IMAGE;
    Framebuffer *fb = boot_info->framebuffer;
    if (!fb->BaseAddress || !fb->Width || !fb->Height ||
        !fb->PixelsPerScanLine || fb->PixelsPerScanLine < fb->Width)
        return SPLASH_RESULT_SKIPPED_INVALID_IMAGE;
    uint64_t framebuffer_pixels =
        (uint64_t)fb->Height * (uint64_t)fb->PixelsPerScanLine;
    if (!framebuffer_pixels || framebuffer_pixels > UINT32_MAX ||
        framebuffer_pixels > UINT64_MAX / sizeof(uint32_t) ||
        fb->BufferSize < framebuffer_pixels * sizeof(uint32_t))
        return SPLASH_RESULT_SKIPPED_INVALID_IMAGE;
    if (!boot_info->logo)
        return SPLASH_RESULT_SKIPPED_NO_LOGO;
    SimpleImage *logo = boot_info->logo;
    if (!logo->PixelBuffer || !logo->Width || !logo->Height ||
        logo->Width > fb->Width || logo->Height > fb->Height)
        return SPLASH_RESULT_SKIPPED_INVALID_IMAGE;
    uint64_t pixels = (uint64_t)logo->Width * (uint64_t)logo->Height;
    if (!pixels || pixels > SPLASH_MAX_IMAGE_PIXELS ||
        pixels > UINT64_MAX / 4u || pixels * 3u > logo->Size)
        return pixels > SPLASH_MAX_IMAGE_PIXELS
            ? SPLASH_RESULT_ABORTED_RENDER_BUDGET
            : SPLASH_RESULT_SKIPPED_INVALID_IMAGE;
    out->logo = logo;
    out->framebuffer = fb;
    out->x = (fb->Width - logo->Width) / 2u;
    out->y = (fb->Height - logo->Height) / 2u;
    out->bytes_per_pixel = logo->Size >= pixels * 4u ? 4u : 3u;
    return SPLASH_RESULT_COMPLETE;
}

static splash_wait_decision_t splash_wait_decide(uint64_t elapsed_ms,
                                                  uint32_t stagnant_samples,
                                                  uint32_t iterations,
                                                  uint32_t renders)
{
    if (elapsed_ms > SPLASH_TOTAL_BUDGET_MS ||
        iterations >= SPLASH_ITERATION_LIMIT ||
        renders >= SPLASH_RENDER_CALL_LIMIT)
        return SPLASH_WAIT_RENDER_BUDGET;
    if (stagnant_samples >= SPLASH_STALL_SAMPLE_LIMIT)
        return SPLASH_WAIT_CLOCK_STALL;
    return SPLASH_WAIT_CONTINUE;
}

static bool splash_clockevent_cadence_degraded(
    uint64_t monotonic_ms, const timer_clockevent_snapshot_t *event)
{
    if (!event || !event->active || event->period_us != 1000u)
        return true;
    if (monotonic_ms < SPLASH_CADENCE_MIN_MONOTONIC_MS)
        return false;
    return event->total_ticks < monotonic_ms / SPLASH_CADENCE_MIN_FRACTION;
}

static splash_wait_decision_t splash_sample(splash_timing_t *timing)
{
    uint64_t now_ms = clock_monotonic_ms();
    timer_clockevent_snapshot_t event = {0};
    bool have_event = timer_clockevent_snapshot(&event) && event.active &&
                      event.period_us == 1000u;
    uint64_t ticks = have_event ? event.total_ticks : timing->last_ticks;
    bool progressed = now_ms > timing->last_monotonic_ms ||
                      ticks > timing->last_ticks;
    timing->stagnant_samples = progressed ? 0u : timing->stagnant_samples + 1u;
    timing->last_monotonic_ms = now_ms;
    timing->last_ticks = ticks;
    timing->iterations++;

    uint64_t monotonic_elapsed = now_ms >= timing->monotonic_start_ms
        ? now_ms - timing->monotonic_start_ms : 0u;
    uint64_t tick_delta = ticks >= timing->ticks_start
        ? ticks - timing->ticks_start : 0u;
    uint64_t tick_elapsed = tick_delta > UINT64_MAX / timing->period_us
        ? UINT64_MAX
        : (tick_delta * timing->period_us) / 1000u;
    timing->elapsed_ms = monotonic_elapsed > tick_elapsed
        ? monotonic_elapsed : tick_elapsed;
    return splash_wait_decide(timing->elapsed_ms, timing->stagnant_samples,
                              timing->iterations, timing->renders);
}

static uint8_t splash_opacity(uint64_t elapsed_ms, bool fade_in)
{
    uint64_t bounded = elapsed_ms > SPLASH_FADE_MS
        ? SPLASH_FADE_MS : elapsed_ms;
    uint32_t value = (uint32_t)((bounded * 100u) / SPLASH_FADE_MS);
    value = (value / SPLASH_OPACITY_BUCKET) * SPLASH_OPACITY_BUCKET;
    if (!fade_in)
        value = 100u - value;
    return (uint8_t)value;
}

static splash_result_t splash_wait_phase(splash_timing_t *timing,
                                         const splash_plan_t *plan,
                                         uint64_t duration_ms, bool render,
                                         bool fade_in)
{
    uint64_t phase_start = timing->elapsed_ms;
    int last_opacity = -1;
    uint8_t midpoint_reported = 0;
    if (render)
        splash_marker(fade_in ? "FADE_IN" : "FADE_OUT",
                      fade_in ? 0 : 100, NULL);
    else
        splash_marker("HOLD", -1, NULL);

    for (;;) {
        splash_wait_decision_t decision = splash_sample(timing);
        if (decision == SPLASH_WAIT_CLOCK_STALL)
            return SPLASH_RESULT_ABORTED_CLOCK_STALL;
        if (decision == SPLASH_WAIT_RENDER_BUDGET)
            return SPLASH_RESULT_ABORTED_RENDER_BUDGET;
        uint64_t phase_elapsed = timing->elapsed_ms >= phase_start
            ? timing->elapsed_ms - phase_start : 0u;
        if (render) {
            uint8_t opacity = splash_opacity(phase_elapsed, fade_in);
            if ((int)opacity != last_opacity) {
                if (timing->renders >= SPLASH_RENDER_CALL_LIMIT)
                    return SPLASH_RESULT_ABORTED_RENDER_BUDGET;
                draw_overlay_image(plan->logo, plan->x, plan->y, opacity);
                timing->renders++;
                last_opacity = opacity;
            }
            if (!midpoint_reported && phase_elapsed >= SPLASH_FADE_MS / 2u) {
                splash_marker(fade_in ? "FADE_IN" : "FADE_OUT", 50, NULL);
                midpoint_reported = 1;
            }
        }
        if (phase_elapsed >= duration_ms)
            break;
        for (uint32_t pause = 0; pause < 64u; pause++)
            __asm__ volatile("pause");
    }
    if (render)
        splash_marker(fade_in ? "FADE_IN" : "FADE_OUT",
                      fade_in ? 100 : 0, NULL);
    return SPLASH_RESULT_COMPLETE;
}

static void splash_model_cleanup(uint8_t *suspended)
{
    if (suspended)
        *suspended = 0;
}

bool splash_model_selftest(void)
{
    uint8_t pixel = 0;
    Framebuffer fb = {
        .BaseAddress = &pixel,
        .BufferSize = 640u * 480u * 4u,
        .Width = 640,
        .Height = 480,
        .PixelsPerScanLine = 640,
    };
    SimpleImage image = {
        .Width = 10,
        .Height = 10,
        .Size = 300,
        .PixelBuffer = &pixel,
    };
    BootInfo info = {.framebuffer = &fb, .logo = NULL};
    splash_plan_t plan = {0};
    uint8_t suspended = 1;
    bool null_logo = splash_validate(&info, &plan) ==
                     SPLASH_RESULT_SKIPPED_NO_LOGO;
    splash_model_cleanup(&suspended);
    bool null_restores = suspended == 0;
    info.logo = &image;
    image.Size = 299;
    bool invalid_size = splash_validate(&info, &plan) ==
                        SPLASH_RESULT_SKIPPED_INVALID_IMAGE;
    image.Size = 300;
    bool valid = splash_validate(&info, &plan) == SPLASH_RESULT_COMPLETE;
    bool clock_stall = splash_wait_decide(1, SPLASH_STALL_SAMPLE_LIMIT,
                                          1, 0) == SPLASH_WAIT_CLOCK_STALL;
    bool lateness_skips = splash_opacity(400, true) == 80;
    bool render_budget = splash_wait_decide(SPLASH_TOTAL_BUDGET_MS + 1u,
                                            0, 1, 0) ==
                         SPLASH_WAIT_RENDER_BUDGET;
    timer_clockevent_snapshot_t healthy_event = {
        .active = 1,
        .period_us = 1000,
        .total_ticks = 3000,
    };
    timer_clockevent_snapshot_t degraded_event = healthy_event;
    degraded_event.total_ticks = 1000;
    bool cadence_policy =
        !splash_clockevent_cadence_degraded(11000, &healthy_event) &&
        splash_clockevent_cadence_degraded(11000, &degraded_event);
    suspended = 1;
    splash_model_cleanup(&suspended);
    bool complete_restores = suspended == 0;
    bool all_results_terminal = true;
    for (int result = SPLASH_RESULT_COMPLETE;
         result <= SPLASH_RESULT_ABORTED_CONSOLE_BUSY; result++)
        all_results_terminal = all_results_terminal &&
            splash_result_name((splash_result_t)result) != NULL;
    bool ok = null_logo && null_restores && invalid_size && valid &&
              clock_stall && lateness_skips && render_budget &&
              cadence_policy &&
              complete_restores && all_results_terminal;
    serial_write_all(ok
        ? "[GRAPHICS][SPLASH_SELFTEST] PASS tests=8\n"
        : "[GRAPHICS][SPLASH_SELFTEST] FAIL\n");
    return ok;
}

splash_result_t play_splash_screen(BootInfo *boot_info)
{
    splash_result_t result = SPLASH_RESULT_COMPLETE;
    splash_plan_t plan = {0};
    splash_timing_t timing = {.period_us = 1000u};
    uint8_t suspended = 0;
    splash_marker("BEGIN", -1, NULL);
    console_set_render_suspended(1);
    suspended = 1;
    splash_marker("SUSPEND", -1, NULL);

    result = splash_validate(boot_info, &plan);
    if (result != SPLASH_RESULT_COMPLETE) {
        char detail[96];
        char *detail_p = splash_append_text(
            detail, result == SPLASH_RESULT_ABORTED_RENDER_BUDGET
                        ? "reason=" : "result=");
        detail_p = splash_append_text(detail_p, splash_result_name(result));
        *detail_p = 0;
        splash_marker(result == SPLASH_RESULT_ABORTED_RENDER_BUDGET
                          ? "ABORT" : "SKIP",
                      -1, detail);
        goto cleanup;
    }

    timer_clockevent_snapshot_t event = {0};
    timing.monotonic_start_ms = clock_monotonic_ms();
    timing.last_monotonic_ms = timing.monotonic_start_ms;
    if (timer_clockevent_snapshot(&event) && event.active &&
        event.period_us == 1000u) {
        timing.ticks_start = event.total_ticks;
        timing.last_ticks = event.total_ticks;
    }

    if (splash_clockevent_cadence_degraded(timing.monotonic_start_ms,
                                           &event)) {
        result = SPLASH_RESULT_ABORTED_RENDER_BUDGET;
        splash_marker(
            "ABORT", -1,
            "reason=ABORTED_RENDER_BUDGET detail=clockevent_cadence_degraded");
        goto cleanup;
    }

    clear_screen(COLOR_BLACK);
    splash_marker("CLEAR", -1, NULL);
    result = splash_wait_phase(&timing, &plan, SPLASH_FADE_MS, true, true);
    if (result != SPLASH_RESULT_COMPLETE)
        goto abort;
    result = splash_wait_phase(&timing, &plan, SPLASH_HOLD_MS, false, true);
    if (result != SPLASH_RESULT_COMPLETE)
        goto abort;
    result = splash_wait_phase(&timing, &plan, SPLASH_FADE_MS, true, false);
    if (result != SPLASH_RESULT_COMPLETE)
        goto abort;
    goto cleanup;

abort:
    {
        char detail[96];
        char *detail_p = splash_append_text(detail, "reason=");
        detail_p = splash_append_text(detail_p, splash_result_name(result));
        *detail_p = 0;
        splash_marker("ABORT", -1, detail);
    }

cleanup:
    if (suspended) {
        console_set_render_suspended(0);
        suspended = 0;
    }
    console_render_full();
    splash_marker("RESUME", -1, NULL);
    char suffix[96];
    char *p = splash_append_text(suffix, "result=");
    p = splash_append_text(p, splash_result_name(result));
    p = splash_append_text(p, " console_suspended=");
    p = splash_append_u64(p, console_is_render_suspended());
    *p = 0;
    splash_marker("COMPLETE", -1, suffix);
    return result;
}

void discard_splash_memory(BootInfo *boot_info)
{
    if (!boot_info->logo)
        return;

    SimpleImage *logo = boot_info->logo;

    uint64_t start_addr = (uint64_t)logo->PixelBuffer;
    uint64_t size = logo->Size;
    uint64_t end_addr = start_addr + size;

    for (uint64_t addr = start_addr; addr < end_addr; addr += 4096)
    {
        pmm_free_frame((void *)addr);
    }

    boot_info->logo = NULL;
}
