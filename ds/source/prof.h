// Perfilador por muestreo (solo en la ROM de perfil: make PERFIL=1)
#pragma once
#include <nds.h>

void prof_start(const char *game);
void prof_second(u32 second);
void prof_mark(u32 mark);
void prof_sync(void);
