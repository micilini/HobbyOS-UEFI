# TMV1-CL-11-FIX5 — canary de generation

Base: `88645901d9393f4796af84cecd9e13d3d6671c70`, branch `feat/taskman`.

O canary antigo correlacionava a generation do registry com `reaped`, embora a
generation avance durante o remove sob o scheduler lock e `reaped` somente
depois do free fora do lock. Um target `cc-boundary`, protegido por hold e
identificado por `task_handle_t`, passou a ser observado antes da pagina 1,
removido entre paginas e confirmado ausente antes da pagina 2. Os loops SMP=1
e SMP=8 e os cinco reprodutores pesados confirmaram generation diferente,
`forced_generation_changes=1`, 2000 children gone e zero duplicates/partial.

## Correção posterior do timer-ref

A execução FIX5 corrigiu o boundary entre páginas, mas o timer-ref ainda
publicava e liberava o worker antes de instalar o hold CLAIMED. Em SMP, o timer
de 20 ms podia expirar e a task sair NORMAL antes do hook. O failure cleanup
sobrescrevia o estágio original com `reap`. A correção e certificação final
estão em `TMV1-CL-11-FIX6-final-certification.md`.
