#ifndef SMP_BOOT_H
#define SMP_BOOT_H

#include <stdint.h>

// Endereço fixo na memória baixa onde o trampoline vai morar
// 0x8000 é seguro na maioria dos PCs (fica acima do boot sector real mode 0x7C00)
#define TRAMPOLINE_ADDR 0x8000

// Inicia o processo de acordar todos os APs
void smp_boot_aps();

#endif