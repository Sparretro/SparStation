// Lo frío de la GTE en la RAM principal: el despacho general (intérprete y operaciones raras) y el estado guardado.
// Con las entradas directas del dynarec no se usa en partida; en la ITCM ocupaba 5,3 KB (ver gte.itcm.c).
#pragma GCC diagnostic ignored "-Wunused-function"
#define GTE_COLD_ONLY
#include "../../core/gte.c"
