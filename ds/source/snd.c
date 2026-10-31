// Sonido: cada voz de la PS1 (core/spu.c) suena en uno de los 16 canales de hardware de la DS, con su
// muestra ya en PCM de 16 bits. El ARM7 (driver de sonido de calico) hace la mezcla; aquí solo se reparten
// los canales y se traducen tono, volumen y panorama.
#include <nds.h>
#include <malloc.h>
#include <stdio.h>
#include <calico/nds/arm9/sound.h>
#include "psx.h"
#include "snd.h"
#include "gx.h"

#define NCH 16

static s8 ch_of[24];          // canal de la DS de cada voz (-1 = ninguno)
static s8 voice_of[NCH];      // voz de la PS1 en cada canal (-1 = libre)
static u32 ch_until[NCH];     // vblank en que acaba una muestra sin bucle (el canal queda libre solo)
static u16 ch_level[NCH];     // volumen enviado (para robar el canal más bajo)
static u32 ch_age[NCH], age;
volatile u32 snd_vblanks;   // lo sube main.c en cada vblank

static unsigned timer_for(u32 pitch)
{
	u32 t;
	if (pitch < 32) pitch = 32;
	if (pitch > 0x3fff) pitch = 0x3fff;
	t = (u32)(((u64)SOUND_CLOCK * 4096 + (u64)44100 * pitch / 2) / ((u64)44100 * pitch));
	return t > 0xffff ? 0xffff : t;
}

static void vol_pan(u32 l, u32 r, unsigned *vol, unsigned *pan)
{
	u32 v = l + r;
	*vol = v >> 5;   // 0..0xfffe -> 0..2047
	*pan = v ? (r * 127 + v / 2) / v : 64;
}

static void release(int c)
{
	int v = voice_of[c];
	if (v >= 0) ch_of[v] = -1;
	voice_of[c] = -1;
}

static int pick_channel(void)
{
	int best = -1;
	u32 now = snd_vblanks;
	for (int c = 0; c < NCH; c++) {   // libres, o con una muestra sin bucle que ya ha acabado
		if (voice_of[c] < 0) return c;
		if (ch_until[c] && (s32)(now - ch_until[c]) >= 0) { release(c); return c; }
	}
	for (int c = 0; c < NCH; c++)   // si no, el más bajo (y entre iguales, el más viejo)
		if (best < 0 || ch_level[c] < ch_level[best] || (ch_level[c] == ch_level[best] && ch_age[c] < ch_age[best])) best = c;
	soundStop(1u << best);
	release(best);
	return best;
}

static void snd_key_on(int v, const s16 *pcm, u32 len, s32 loop, u32 pitch)
{
	int c;
	unsigned t = timer_for(pitch);
	if (ch_of[v] >= 0) { soundStop(1u << ch_of[v]); release(ch_of[v]); }
	if (len < 2) return;
	c = pick_channel();
	ch_of[v] = (s8)c;
	voice_of[c] = (s8)v;
	ch_level[c] = 0;
	ch_age[c] = ++age;
	DC_FlushRange(pcm, len * 2);   // el ARM7 lee la RAM principal, no la caché del ARM9
	if (loop >= 0 && loop / 2 <= 0xffff && (u32)loop + 2 <= len) {
		ch_until[c] = 0;
		soundPreparePcm(c | SOUND_START, 0, 64, t, SoundMode_Repeat, SoundFmt_Pcm16, pcm, loop / 2, (len - loop) / 2);
	} else {
		// duración real con este tono, en vblanks (para dar el canal por libre aunque la PS1 siga)
		ch_until[c] = snd_vblanks + 1 + (u32)((u64)len * t * 60 / SOUND_CLOCK);
		if (!ch_until[c]) ch_until[c] = 1;
		soundPreparePcm(c | SOUND_START, 0, 64, t, SoundMode_OneShot, SoundFmt_Pcm16, pcm, 0, len / 2);
	}
}

static void snd_update(int v, u32 pitch, u32 l, u32 r)
{
	int c = ch_of[v];
	unsigned vol, pan;
	if (c < 0) return;
	vol_pan(l, r, &vol, &pan);
	ch_level[c] = (u16)vol;
	soundChSetVolume(c, vol);
	soundChSetPan(c, pan);
	soundChSetTimer(c, timer_for(pitch));
}

static void snd_stop(int v)
{
	int c = ch_of[v];
	if (c < 0) return;
	soundStop(1u << c);
	release(c);
}

static const SpuHw snd_hw = { snd_key_on, snd_update, snd_stop, NULL };

void snd_init(int dsi)
{
	for (int v = 0; v < 24; v++) ch_of[v] = -1;
	for (int c = 0; c < NCH; c++) voice_of[c] = -1;
	soundInit();
	soundPowerOn();
	soundSetMixerVolume(127);
	if (!spu_cache_mem) {   // caché de muestras en PCM: en modo DS (4 MB) solo cabe una pequeña
		spu_cache_size = dsi ? 1536 * 1024 : 192 * 1024;
		while (spu_cache_size >= 32 * 1024 && !(spu_cache_mem = memalign(32, spu_cache_size))) spu_cache_size /= 2;
		if (!spu_cache_mem) spu_cache_size = 0;
		printf("Sonido: cache %lu KB\n", spu_cache_size / 1024);
	}
	spu_hw = &snd_hw;
}

void snd_pause(int on)
{
	soundSetMixerVolume(on ? 0 : 127);
}
