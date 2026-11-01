// Compilado en ITCM (memoria interna rápida del ARM9): el linker de calico mete aquí los *.itcm.o.
// Solo lo caliente: gte_exec, gte_command y gte_state van a la RAM principal (gte.cold.c) y dejan sitio en la ITCM.
#pragma GCC diagnostic ignored "-Wunused-function"
#define GTE_HOT_ONLY
#include "../../core/gte.c"
