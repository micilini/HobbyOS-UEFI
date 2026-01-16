#include "sleep.h"
#include "acpi.h"

#include <stdint.h>
#include <stdbool.h>

static int aml_pkg_length(const uint8_t *p, uint32_t max, uint32_t *out_len, uint32_t *out_consumed)
{
    if (!p || max == 0)
        return 0;

    uint8_t lead = p[0];
    uint8_t byte_count = (lead >> 6) & 0x3;
    uint32_t len = 0;

    if (byte_count == 0)
    {
        len = (uint32_t)(lead & 0x3F);
        *out_len = len;
        *out_consumed = 1;
        return 1;
    }

    if (max < (uint32_t)(1 + byte_count))
        return 0;

    len = (uint32_t)(lead & 0x0F);

    for (uint8_t i = 0; i < byte_count; i++)
    {
        len |= ((uint32_t)p[1 + i]) << (4 + (i * 8));
    }

    *out_len = len;
    *out_consumed = 1 + byte_count;
    return 1;
}

static int aml_parse_integer(const uint8_t *p, uint32_t max, uint32_t *out_value, uint32_t *out_consumed)
{
    if (!p || max == 0)
        return 0;

    uint8_t op = p[0];

    if (op == 0x00)
    {
        *out_value = 0;
        *out_consumed = 1;
        return 1;
    }
    if (op == 0x01)
    {
        *out_value = 1;
        *out_consumed = 1;
        return 1;
    }
    if (op == 0xFF)
    {
        *out_value = 0xFFFFFFFFu;
        *out_consumed = 1;
        return 1;
    }

    if (op == 0x0A)
    {
        if (max < 2)
            return 0;
        *out_value = p[1];
        *out_consumed = 2;
        return 1;
    }
    if (op == 0x0B)
    {
        if (max < 3)
            return 0;
        *out_value = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
        *out_consumed = 3;
        return 1;
    }
    if (op == 0x0C)
    {
        if (max < 5)
            return 0;
        *out_value = (uint32_t)p[1] |
                     ((uint32_t)p[2] << 8) |
                     ((uint32_t)p[3] << 16) |
                     ((uint32_t)p[4] << 24);
        *out_consumed = 5;
        return 1;
    }

    if (op == 0x0E)
    {
        if (max < 9)
            return 0;

        *out_value = (uint32_t)p[1] |
                     ((uint32_t)p[2] << 8) |
                     ((uint32_t)p[3] << 16) |
                     ((uint32_t)p[4] << 24);
        *out_consumed = 9;
        return 1;
    }

    return 0;
}

bool acpi_get_s5_sleep_types(uint8_t *out_typa, uint8_t *out_typb)
{
    if (!out_typa || !out_typb)
        return false;

    *out_typa = 0;
    *out_typb = 0;

    const AcpiSdtHeader *dsdt = acpi_get_dsdt();
    if (!dsdt)
        return false;

    const uint8_t *aml = (const uint8_t *)dsdt + sizeof(AcpiSdtHeader);
    uint32_t aml_len = dsdt->length - (uint32_t)sizeof(AcpiSdtHeader);

    for (uint32_t i = 0; i + 8 < aml_len; i++)
    {
        if (aml[i] != 0x08)
            continue;

        uint32_t j = i + 1;

        while (j < aml_len && (aml[j] == 0x5C || aml[j] == 0x5E))
            j++;

        if (j + 4 >= aml_len)
            continue;

        if (!(aml[j + 0] == '_' && aml[j + 1] == 'S' && aml[j + 2] == '5' && aml[j + 3] == '_'))
            continue;

        uint32_t k = j + 4;
        if (k >= aml_len)
            continue;

        if (aml[k] != 0x12)
            continue;
        k++;

        uint32_t pkg_len = 0, pkg_len_cons = 0;
        if (!aml_pkg_length(&aml[k], aml_len - k, &pkg_len, &pkg_len_cons))
            continue;
        k += pkg_len_cons;

        if (k >= aml_len)
            continue;

        uint8_t elem_count = aml[k];
        k++;

        if (elem_count < 2)
            continue;

        uint32_t v0 = 0, c0 = 0;
        if (!aml_parse_integer(&aml[k], aml_len - k, &v0, &c0))
            continue;
        k += c0;

        uint32_t v1 = 0, c1 = 0;
        if (!aml_parse_integer(&aml[k], aml_len - k, &v1, &c1))
            continue;

        *out_typa = (uint8_t)(v0 & 0xFF);
        *out_typb = (uint8_t)(v1 & 0xFF);

        return true;
    }

    return false;
}