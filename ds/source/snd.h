// Sonido de la PS1 con los canales de hardware de la DS (ver snd.c)
#pragma once
extern volatile u32 snd_vblanks;
void snd_init(int dsi);   // antes de psx_reset: reserva la caché de muestras y conecta spu_hw
void snd_pause(int on);   // silencio mientras está el menú de pausa
void snd_set_disc(int (*read)(void *user, u32 lba, u8 *out));   // para leer por delante el audio XA
void snd_set_cdda(int (*read)(void *user, u32 lba, u8 *out));   // y las pistas de audio del CD
void snd_xa_pump(void);   // una vez por frame: mantiene lleno el audio XA
void snd_enable(int on);   // 0 = sin sonido (la SPU se sigue emulando)
