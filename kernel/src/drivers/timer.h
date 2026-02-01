#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// Inicializa o timer (HPET/PIT)
void timer_init();

// O ISR real - agora deve ser leve
void timer_handler();

// Retorna tempo em ms (usado pelo xHCI e Hotplug)
uint64_t timer_get_uptime_ms();

// Pausa bloqueante (busy wait inteligente)
void timer_sleep(uint64_t ms);

// NOVO: Função para ser chamada no loop infinito do kernel (Bottom Half)
void timer_run_deferred();

#endif