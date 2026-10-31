// Sonido de la PS1 con los canales de hardware de la DS (ver snd.c)
#pragma once
extern volatile u32 snd_vblanks;
void snd_init(int dsi);   // antes de psx_reset: reserva la caché de muestras y conecta spu_hw
void snd_pause(int on);   // silencio mientras está el menú de pausa
