#include "legacy_pic.h"

#include "../core/idt.h"
#include "../core/io.h"
#include "../drivers/serial.h"

static legacy_pic_snapshot_t g_legacy_pic;

static bool legacy_pic_values_quiescent(uint8_t master, uint8_t slave)
{
    return master == 0xFFu && slave == 0xFFu;
}

bool legacy_pic_quiesce(bool pcat_declared)
{
    if (irq_is_enabled())
        return false;

    g_legacy_pic = (legacy_pic_snapshot_t){
        .pcat_declared = pcat_declared ? 1u : 0u,
        .defensive_attempt = pcat_declared ? 0u : 1u};

    outb(LEGACY_PIC_MASTER_IMR, 0xFFu);
    outb(LEGACY_PIC_SLAVE_IMR, 0xFFu);
#ifdef HOBBYOS_IRQ_NEGATIVE_PIC_UNMASKED
    outb(LEGACY_PIC_MASTER_IMR, 0xFEu);
#endif
    g_legacy_pic.master_imr = inb(LEGACY_PIC_MASTER_IMR);
    g_legacy_pic.slave_imr = inb(LEGACY_PIC_SLAVE_IMR);
    g_legacy_pic.quiescent = legacy_pic_values_quiescent(
        g_legacy_pic.master_imr, g_legacy_pic.slave_imr);

    char line[] = "[IRQ][PIC] QUIESCENT master=00 slave=00 pcat=0\n";
    const char hex[] = "0123456789abcdef";
    line[28] = hex[g_legacy_pic.master_imr >> 4];
    line[29] = hex[g_legacy_pic.master_imr & 0xFu];
    line[37] = hex[g_legacy_pic.slave_imr >> 4];
    line[38] = hex[g_legacy_pic.slave_imr & 0xFu];
    line[45] = pcat_declared ? '1' : '0';
    if (g_legacy_pic.quiescent)
        serial_write_all(line);
#ifdef HOBBYOS_IRQ_NEGATIVE_PIC_UNMASKED
    else
        serial_write_all("[IRQ][NEGATIVE] PIC_NOT_QUIESCENT_DETECTED\n");
#endif
    return g_legacy_pic.quiescent;
}

bool legacy_pic_snapshot(legacy_pic_snapshot_t *out)
{
    if (!out)
        return false;
    *out = g_legacy_pic;
    return true;
}

bool legacy_pic_is_quiescent(void)
{
    return g_legacy_pic.quiescent &&
           legacy_pic_values_quiescent(g_legacy_pic.master_imr,
                                       g_legacy_pic.slave_imr);
}

bool legacy_pic_contract_selftest(void)
{
    return legacy_pic_values_quiescent(0xFFu, 0xFFu) &&
           !legacy_pic_values_quiescent(0xFEu, 0xFFu) &&
           !legacy_pic_values_quiescent(0xFFu, 0x7Fu);
}
