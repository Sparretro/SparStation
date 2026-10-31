// Sonido: cada voz de la PS1 (core/spu.c) suena en uno de los 16 canales de hardware de la DS, con su
// muestra ya en PCM de 16 bits. El ARM7 (driver de sonido de calico) hace la mezcla; aquí solo se reparten
// los canales y se traducen tono, volumen y panorama.
#include <nds.h>
#include <malloc.h>
#include <stdio.h>
#include <calico/nds/arm9/sound.h>
#include "psx.h"
#include "snd.h"


#ifndef SND_DS_CACHE_KB
#define SND_DS_CACHE_KB 64   // modo DS (banco de pruebas en melonDS): poca memoria
#endif
#define NCH 14         // canales para las voces; el 14 y el 15 son del audio XA del CD

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

// ---- audio XA del CD ----
// El juego lee el CD al ritmo de la emulación (lenta), pero los sectores de audio no los mira la CPU: van
// directos a la SPU. Así que, en cuanto llega el primero, aquí se sigue leyendo el disco por delante y se
// toca a su velocidad real. Cuando la emulación «lee» después esos mismos sectores, se ignoran.
#define XR 16384                  // muestras por canal en el anillo (0,43 s a 37800 Hz)
#define XA_LEAD (XR - 4096)       // cuánto se llena por delante de lo que suena
#define XA_MAX_AHEAD 2000         // sectores como mucho por delante de la emulación
#define CH_XL 14
#define CH_XR 15
static s16 *xa_buf[2];
static int (*xa_read)(void *user, u32 lba, u8 *out);
static struct {
	int valid, active, ended, stereo;   // valid: el tramo [first_lba, next_lba) ya se tocó (hasta que el juego pare o salte)
	u32 rate, timer, t0;          // t0: fast_ticks al arrancar los canales
	u32 written;                  // muestras por canal escritas desde el arranque
	u32 first_lba, next_lba, emu_lba;
	u8 file, chan;
	XaDecoder dec;
} xa;
static int xa_muted;
static u8 xa_sector_buf[2352];
static s16 xa_pcm[4032];
extern u32 fast_ticks(void);

static u32 xa_played(void) { return (fast_ticks() - xa.t0) / (2 * xa.timer); }   // fast_ticks va a 33,5 MHz

static void xa_halt(void)
{
	if (xa.active) soundStop((1u << CH_XL) | (1u << CH_XR));
	xa.active = 0;
}

static void xa_put(const u8 *raw)
{
	int st, rate, n = xa_decode(raw, xa_pcm, &xa.dec, &st, &rate);
	u32 w = xa.written % XR;
	for (int i = 0; i < n; i++) {
		xa_buf[0][(w + i) % XR] = xa_pcm[st ? i * 2 : i];
		xa_buf[1][(w + i) % XR] = xa_pcm[st ? i * 2 + 1 : i];
	}
	if (w + n <= XR) { DC_FlushRange(&xa_buf[0][w], n * 2); DC_FlushRange(&xa_buf[1][w], n * 2); }
	else { DC_FlushRange(xa_buf[0], XR * 2); DC_FlushRange(xa_buf[1], XR * 2); }
	xa.written += n;
	if (raw[18] & 0x80) xa.ended = 1;   // fin de archivo: no leer más
}

static void xa_volume(void)
{
	unsigned v = xa_muted ? 0 : 2047;
	soundChSetVolume(CH_XL, v);
	soundChSetVolume(CH_XR, v);
}

static void xa_sector(u32 lba, const u8 *raw)
{
	int st, rate;
	if (!xa_buf[0]) return;
	if (xa.valid && raw[16] == xa.file && raw[17] == xa.chan && lba >= xa.first_lba) {   // mismo tramo (aunque ya acabara)
		if (lba > xa.emu_lba) xa.emu_lba = lba;
		if (lba < xa.next_lba) return;   // ya sonó (se leyó por delante)
		if (xa.active && lba == xa.next_lba && !xa.ended && xa.written - xa_played() < XR - 4032) {   // la emulación alcanzó a la lectura
			xa_put(raw);
			xa.next_lba++;
			return;
		}
	}
	// empieza (o vuelve a empezar) aquí
	xa_halt();
	memset(&xa, 0, sizeof xa);
	st = raw[19] & 1;
	rate = (raw[19] & 4) ? 18900 : 37800;
	xa.stereo = st;
	xa.rate = (u32)rate;
	xa.timer = soundTimerFromHz(rate);
	xa.file = raw[16];
	xa.chan = raw[17];
	xa.first_lba = xa.emu_lba = lba;
	xa.next_lba = lba + 1;
	dmaFillWords(0, xa_buf[0], XR * 2);
	dmaFillWords(0, xa_buf[1], XR * 2);
	xa_put(raw);
	DC_FlushRange(xa_buf[0], XR * 2);
	DC_FlushRange(xa_buf[1], XR * 2);
	soundPreparePcm(CH_XL, 2047, 0, xa.timer, SoundMode_Repeat, SoundFmt_Pcm16, xa_buf[0], 0, XR / 2);
	soundPreparePcm(CH_XR, 2047, 127, xa.timer, SoundMode_Repeat, SoundFmt_Pcm16, xa_buf[1], 0, XR / 2);
	soundStart((1u << CH_XL) | (1u << CH_XR));
	if (xa_muted) xa_volume();
	xa.t0 = fast_ticks();
	xa.active = xa.valid = 1;
	snd_xa_pump();
}

// Una vez por frame: llena el anillo leyendo el disco por delante; para los canales al acabarse
void snd_xa_pump(void)
{
	u32 played;
	if (!xa.active) return;
	played = xa_played();
	if (xa.ended && played >= xa.written) { xa_halt(); return; }
	if (xa.written < played) {   // se vació: lo que queda en el anillo es viejo, fuera
		dmaFillWords(0, xa_buf[0], XR * 2);
		dmaFillWords(0, xa_buf[1], XR * 2);
		DC_FlushRange(xa_buf[0], XR * 2);
		DC_FlushRange(xa_buf[1], XR * 2);
		xa.written = played + 512;
	}
	while (!xa.ended && xa.written - played < XA_LEAD && xa.next_lba - xa.emu_lba < XA_MAX_AHEAD) {
		const u8 *s = xa_sector_buf;
		if (!xa_read || !xa_read(NULL, xa.next_lba, xa_sector_buf)) { xa.ended = 1; break; }
		xa.next_lba++;
		if (s[15] == 2 && (s[18] & 0x04) && s[16] == xa.file && s[17] == xa.chan) xa_put(s);
	}
}

static void xa_stop(void) { xa_halt(); xa.valid = 0; }
static void xa_mute(int on) { xa_muted = on; if (xa.active) xa_volume(); }
static const CdAudioHw snd_cd = { xa_sector, xa_stop, xa_mute };

void snd_set_disc(int (*read)(void *user, u32 lba, u8 *out)) { xa_read = read; }

void snd_init(int dsi)
{
	for (int v = 0; v < 24; v++) ch_of[v] = -1;
	for (int c = 0; c < NCH; c++) voice_of[c] = -1;
	soundInit();
	soundPowerOn();
	soundSetMixerVolume(127);
	if (!spu_cache_mem) {   // caché de muestras en PCM: en modo DS (4 MB) solo cabe una pequeña
		spu_cache_size = dsi ? 1536 * 1024 : SND_DS_CACHE_KB * 1024;
		while (spu_cache_size >= 32 * 1024 && !(spu_cache_mem = memalign(32, spu_cache_size))) spu_cache_size /= 2;
		if (!spu_cache_mem) spu_cache_size = 0;
		printf("Sonido: cache %lu KB\n", spu_cache_size / 1024);
	}
	spu_hw = &snd_hw;
	xa_buf[0] = memalign(32, XR * 2);
	xa_buf[1] = memalign(32, XR * 2);
	if (xa_buf[0] && xa_buf[1]) cd_audio_hw = &snd_cd;
}

void snd_pause(int on)
{
	soundSetMixerVolume(on ? 0 : 127);
}
