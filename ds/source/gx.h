// GPU de la PS1 con el motor 3D de la DS (ver gx.c)
#pragma once
#include "psx.h"

extern const GpuHw gx_hw;
extern int gx_display_dirty;   // la zona mostrada de la VRAM cambió desde la última copia al fondo
extern int gx_bg_hidden;       // el fondo del 3D es opaco este frame: el BG3 no se ve
void gx_init(void);            // pone el vídeo en modo 3D; los bancos A (fondo), B, C y D (texturas), E (paletas)
