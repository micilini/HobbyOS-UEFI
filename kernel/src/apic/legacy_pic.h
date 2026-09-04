#ifndef HOBBYOS_LEGACY_PIC_H
#define HOBBYOS_LEGACY_PIC_H

#include <stdbool.h>
#include <stdint.h>

#define LEGACY_PIC_MASTER_COMMAND 0x20u
#define LEGACY_PIC_MASTER_IMR 0x21u
#define LEGACY_PIC_SLAVE_COMMAND 0xA0u
#define LEGACY_PIC_SLAVE_IMR 0xA1u

typedef struct
{
    uint8_t master_imr;
    uint8_t slave_imr;
    uint8_t pcat_declared;
    uint8_t defensive_attempt;
    uint8_t quiescent;
} legacy_pic_snapshot_t;

bool legacy_pic_quiesce(bool pcat_declared);
bool legacy_pic_snapshot(legacy_pic_snapshot_t *out);
bool legacy_pic_is_quiescent(void);
bool legacy_pic_contract_selftest(void);

#endif
