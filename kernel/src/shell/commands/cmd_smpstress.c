#include "cmd_smpstress.h"
#include "../../apic/lapic.h"
#include "../../core/scheduler.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"
#include "../../memory/heap.h"
#include "../../graphics/console.h"

static uint64_t timer_get_ms(void)
{
    return timer_get_uptime_ms();
}

static void serial_write_hex_u64(uint64_t v)
{
    serial_write_hex64_all(v);
}

typedef struct
{
    uint32_t worker_id;
    uint32_t panic_pct;
    uint32_t period_ms;
    uint32_t rng_state;
    uint64_t iterations;

    uint64_t warmup_until_ms;
    uint32_t yield_ms;
    uint32_t panic_check_ms;
    uint64_t next_panic_check_ms;
} smpstress_ctx_t;

static uint32_t parse_u32(const char *s, uint32_t def)
{
    if (!s || !*s)
        return def;

    uint32_t v = 0;
    while (*s)
    {
        char c = *s++;
        if (c < '0' || c > '9')
            return def;
        v = (v * 10u) + (uint32_t)(c - '0');
    }
    return v;
}

static uint32_t rng_next_u32(smpstress_ctx_t *c)
{

    uint32_t x = c->rng_state;
    if (x == 0)
        x = 0xA341316Cu ^ (c->worker_id * 2654435761u);

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    c->rng_state = x;
    return x;
}

static void serial_write_u32_dec_all(uint32_t v)
{
    char buf[11];
    int i = 0;

    if (v == 0)
    {
        serial_write_all("0");
        return;
    }

    while (v > 0 && i < 10)
    {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    while (i--)
    {
        char s[2] = {buf[i], 0};
        serial_write_all(s);
    }
}

static uint64_t stress_job_A(uint64_t x)
{

    for (int i = 0; i < 250000; i++)
    {
        x ^= (x << 7);
        x ^= (x >> 9);
        x *= 11400714819323198485ULL;
        x += (uint64_t)i;
    }
    return x;
}

static uint64_t stress_job_B(uint64_t x)
{

    uint64_t acc = 0;
    uint64_t n = (x | 1ULL);

    for (int k = 0; k < 6000; k++)
    {
        uint64_t v = n + (uint64_t)k * 2ULL;
        int is_prime = 1;

        for (uint64_t d = 3; d * d <= v && d < 2000; d += 2)
        {
            if ((v % d) == 0)
            {
                is_prime = 0;
                break;
            }
        }

        acc += (uint64_t)is_prime;
    }

    return acc ^ n;
}

static uint64_t stress_job_C(uint64_t x)
{

    uint64_t a = (x ^ 0x9E3779B97F4A7C15ULL);
    uint64_t b = (x + 0xD1B54A32D192ED03ULL);
    uint64_t sum = 0;

    for (int i = 0; i < 200000; i++)
    {
        a = a * 6364136223846793005ULL + 1;
        b = b * 1442695040888963407ULL + 1;
        sum += (a ^ (b >> 1)) + (uint64_t)i;
    }

    return sum;
}

static void smpstress_worker(void *arg)
{
    smpstress_ctx_t *c = (smpstress_ctx_t *)arg;
    if (!c)
        return;

    const uint32_t test_kind = (c->worker_id % 3u);
    const char *test_name = (test_kind == 0) ? "A" : (test_kind == 1) ? "B"
                                                                      : "C";

    uint64_t last_log = timer_get_ms();

    serial_write_all("[SMP] Task ");
    serial_write_u32_dec_all(c->worker_id);
    serial_write_all(" started on CPU ");
    serial_write_u32_dec_all(lapic_get_id());
    serial_write_all(" test=");
    serial_write_all(test_name);
    serial_write_all("\n");

    while (1)
    {
        uint64_t seed = ((uint64_t)rng_next_u32(c) << 32) | (uint64_t)rng_next_u32(c);
        seed ^= ((uint64_t)c->worker_id * 0x9E3779B97F4A7C15ULL);

        volatile uint64_t acc = 0;
        if (test_kind == 0)
            acc = stress_job_A(seed);
        else if (test_kind == 1)
            acc = stress_job_B(seed);
        else
            acc = stress_job_C(seed);

        c->iterations++;

        uint64_t now = timer_get_ms();

        if ((now - last_log) >= (uint64_t)c->period_ms)
        {
            last_log = now;
            serial_write_all("[SMP] Task ");
            serial_write_u32_dec_all(c->worker_id);
            serial_write_all(" cpu=");
            serial_write_u32_dec_all(lapic_get_id());
            serial_write_all(" test=");
            serial_write_all(test_name);
            serial_write_all(" iter=");
            serial_write_u32_dec_all((uint32_t)(c->iterations & 0xFFFFFFFFu));
            serial_write_all(" acc=0x");
            serial_write_hex_u64((uint64_t)acc);
            serial_write_all("\n");
        }

        if (c->panic_pct > 0 && now >= c->warmup_until_ms)
        {
            if (now >= c->next_panic_check_ms)
            {
                c->next_panic_check_ms = now + (uint64_t)c->panic_check_ms;

                uint32_t r = rng_next_u32(c) % 100;
                if (r < c->panic_pct)
                {
                    serial_write_all("[SMP] Triggering #DE (div0) from Task ");
                    serial_write_u32_dec_all(c->worker_id);
                    serial_write_all(" cpu=");
                    serial_write_u32_dec_all(lapic_get_id());
                    serial_write_all(" test=");
                    serial_write_all(test_name);
                    serial_write_all("\n");

                    volatile uint64_t z = 0;
                    volatile uint64_t crash = acc / z;
                    (void)crash;
                }
            }
        }

        if (c->yield_ms == 0)
        {
        }
        else
        {
            if (c->yield_ms > 0)
                timer_sleep(c->yield_ms);
        }
    }
}

int cmd_smpstress(int argc, char **argv)
{

    uint32_t workers = 3;
    uint32_t panic_pct = 30;
    uint32_t period_ms = 250;

    uint32_t warmup_s = 5;
    uint32_t yield_ms = 1;
    uint32_t panic_check_ms = 250;

    if (argc >= 2)
        workers = parse_u32(argv[1], workers);
    if (argc >= 3)
        panic_pct = parse_u32(argv[2], panic_pct);
    if (argc >= 4)
        period_ms = parse_u32(argv[3], period_ms);

    if (argc >= 5)
        warmup_s = parse_u32(argv[4], warmup_s);
    if (argc >= 6)
        yield_ms = parse_u32(argv[5], yield_ms);
    if (argc >= 7)
        panic_check_ms = parse_u32(argv[6], panic_check_ms);

    if (workers == 0)
        workers = 1;
    if (panic_pct > 100)
        panic_pct = 100;

    if (period_ms < 50)
        period_ms = 50;
    if (yield_ms > 20)
        yield_ms = 20;
    if (panic_check_ms < 50)
        panic_check_ms = 50;

    uint64_t now = timer_get_ms();
    uint64_t warmup_until = now + (uint64_t)warmup_s * 1000ULL;

    serial_write_all("[SMP] smpstress spawning workers=");
    serial_write_u32_dec_all(workers);
    serial_write_all(" panic_pct=");
    serial_write_u32_dec_all(panic_pct);
    serial_write_all(" period_ms=");
    serial_write_u32_dec_all(period_ms);
    serial_write_all(" warmup_s=");
    serial_write_u32_dec_all(warmup_s);
    serial_write_all(" yield_ms=");
    serial_write_u32_dec_all(yield_ms);
    serial_write_all(" panic_check_ms=");
    serial_write_u32_dec_all(panic_check_ms);
    serial_write_all("\n");

    for (uint32_t i = 0; i < workers; i++)
    {
        smpstress_ctx_t *ctx = (smpstress_ctx_t *)kmalloc(sizeof(smpstress_ctx_t));
        if (!ctx)
        {
            serial_write_all("[SMP] smpstress kmalloc failed\n");
            return -1;
        }

        ctx->worker_id = i;
        ctx->panic_pct = panic_pct;
        ctx->period_ms = period_ms;
        ctx->rng_state = 0xC001D00Du ^ (i * 0x9E3779B9u);
        ctx->iterations = 0;

        ctx->warmup_until_ms = warmup_until;
        ctx->yield_ms = yield_ms;
        ctx->panic_check_ms = panic_check_ms;
        ctx->next_panic_check_ms = warmup_until;

        thread_create(smpstress_worker, ctx);
    }

    console_write("[SMP] Stress Started (interactive). Check SERIAL.\n");

    return 0;
}