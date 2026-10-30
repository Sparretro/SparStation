// GPU de la PS1 con el motor 3D de la DS (ver gx.c)
#pragma once
#include "psx.h"

extern const GpuHw gx_hw;
int gx_need_copy(void);        // ¿hay que copiar la zona mostrada de la VRAM al fondo BG3? (la marca como copiada)
extern int gx_bg_hidden;       // el fondo del 3D es opaco este frame: el BG3 no se ve
extern volatile u32 gx_underflows, gx_late, gx_rdlines_min;   // diagnóstico por segundo (los pone a cero hud.c)
void gx_vblank(void);          // en la vblank: lleva a la VRAM las texturas y paletas del frame que se acaba de mostrar
void gx_init(void);            // pone el vídeo en modo 3D; los bancos A (fondo), B, C y D (texturas), E (paletas)
typedef struct { char magic[8]; volatile u32 stage, n, vbl, dyn; } GxDbg;   // depuración: paso en curso (tools/dsbench lo enseña)
extern GxDbg gx_dbg;
