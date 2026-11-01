// Sonido: cada voz de la PS1 (core/spu.c) suena en uno de los 16 canales de hardware de la DS, con su
// muestra ya en PCM de 16 bits. El ARM7 (driver de sonido de calico) hace la mezcla; aquí solo se reparten
// los canales y se traducen tono, volumen y panorama.
#include <nds.h>
#include <malloc.h>
#include <stdio.h>
#include <calico/nds/arm9/sound.h>
#include <calico/system/thread.h>
#include <calico/system/mutex.h>
#include <calico/system/condvar.h>
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
	int valid, active, ended, stereo;   // valid: el tramo desde first_lba lo toca el hilo (hasta que el juego pare o salte)
	u32 rate, timer, t0;          // t0: fast_ticks al arrancar los canales
	u32 written;                  // muestras por canal escritas desde el arranque
	u32 first_lba, next_lba, emu_lba;   // next_lba: tras el último que sonó; emu_lba: el último que leyó la emulación
	u8 file, chan;
	XaDecoder dec;
} xa;
static int xa_muted;
static s16 xa_pcm[4032];
extern u32 fast_ticks(void);

// ---- lector del disco para el audio (hilo) ----
// Lo que va a sonar del CD lo lee de la SD un hilo propio, por delante, a un anillo de sectores: las pistas de
// audio (CD-DA) en bloques de DA_CHUNK sectores, y el XA sector a sector, solo los de su archivo y canal (en
// cuanto se sabe cada cuántos sectores viene uno suyo, directo a ellos: la música va entrelazada). Mientras el
// ARM7 trae los datos (calico espera su respuesta en un buzón), la emulación sigue. Leído en el hilo principal
// costaba en la DSi ~9% la música de CD de Tomb Raider (v18) y ~13% el XA de Castlevania («otros» del log).
#define DA_SLOTS 32     // sectores en el anillo (0,43 s de CD-DA; de XA, 1,7-3,4 s)
#define DA_CHUNK 8      // sectores por lectura de la SD (CD-DA)
static u32 (*da_read)(void *user, u32 lba, u32 n, u8 *out);
static u8 *da_ring;
static u32 ring_lba[DA_SLOTS];   // XA: sector del disco de cada hueco
static Thread da_thread;
alignas(8) static u8 da_stack[16 * 1024];
static Mutex da_mtx;
static CondVar da_cv;
enum { RD_OFF, RD_DA, RD_XA };
// Con da_mtx. head/tail cuentan sectores del anillo; pos = siguiente sector del disco a leer. XA: emu = el último
// que leyó la emulación (no adelantarse más de XA_MAX_AHEAD), last_match/stride = el último del canal y cada
// cuántos vienen.
static struct { u32 gen, pos, end, head, tail, emu, last_match, stride; int mode, eof; u8 file, chan; } dr;

static int da_reader(void *arg)
{
	(void)arg;
	mutexLock(&da_mtx);
	for (;;) {
		u32 gen = dr.gen, slot = dr.head % DA_SLOTS, room = DA_SLOTS - (dr.head - dr.tail), lba, n, got;
		if (dr.mode == RD_OFF || dr.eof || room < (dr.mode == RD_XA ? 1u : DA_CHUNK) ||
		    (dr.mode == RD_XA && dr.pos - dr.emu >= XA_MAX_AHEAD)) {
			condvarWait(&da_cv, &da_mtx);
			continue;
		}
		if (dr.mode == RD_XA) {
			u8 *s = da_ring + slot * 2352;   // hueco libre: si el sector no es del canal, lo pisa el siguiente
			lba = dr.pos;
			if (dr.stride && lba < dr.last_match + dr.stride) lba = dr.last_match + dr.stride;   // directo al siguiente del canal
			mutexUnlock(&da_mtx);
			got = xa_read && xa_read(NULL, lba, s);
			mutexLock(&da_mtx);
			if (gen != dr.gen) continue;   // otro tramo (o Stop) mientras se leía: se tira
			if (!got) { dr.eof = 1; continue; }
			dr.pos = lba + 1;
			if (s[15] == 2 && (s[18] & 0x04) && s[16] == dr.file && s[17] == dr.chan) {
				u32 d = lba - dr.last_match;
				dr.stride = d >= 2 && d <= 32 ? d : 0;
				dr.last_match = lba;
				ring_lba[slot] = lba;
				dr.head++;
				if (s[18] & 0x80) dr.eof = 1;   // fin de archivo: no leer más
			} else if (dr.stride && lba == dr.last_match + dr.stride) {   // el patrón cambió: buscar sector a sector
				dr.stride = 0;
				dr.pos = dr.last_match + 1;
			}
			continue;
		}
		lba = dr.pos;
		n = DA_CHUNK;
		if (n > dr.end - lba) n = dr.end - lba;
		if (n > DA_SLOTS - slot) n = DA_SLOTS - slot;   // sin dar la vuelta al anillo
		mutexUnlock(&da_mtx);
		got = da_read(NULL, lba, n, da_ring + slot * 2352);
		mutexLock(&da_mtx);
		if (gen != dr.gen) continue;   // otro Play (o Stop) mientras se leía: se tira
		dr.head += got;
		dr.pos += got;
		if (!got || dr.pos >= dr.end) dr.eof = 1;
	}
	return 0;
}

// Cambia lo que lee el hilo (desde la emulación: el consumidor es siempre el hilo principal). XA: lba es el
// siguiente al primer sector del tramo (xa_first, que ya sonó); CD-DA: [lba, end)
static void rd_request(int mode, u32 lba, u32 end, const u8 *xa_first)
{
	mutexLock(&da_mtx);
	dr.gen++;
	dr.mode = da_ring ? mode : RD_OFF;
	dr.pos = lba;
	dr.end = end;
	dr.head = dr.tail = 0;
	dr.eof = 0;
	dr.stride = 0;
	dr.emu = dr.last_match = lba - 1;
	if (xa_first) { dr.file = xa_first[16]; dr.chan = xa_first[17]; }
	condvarSignal(&da_cv);
	mutexUnlock(&da_mtx);
}

static u32 xa_played(void) { return (fast_ticks() - xa.t0) / (2 * xa.timer); }   // fast_ticks va a 33,5 MHz

static void xa_halt(void)
{
	if (xa.active) {
		soundStop((1u << CH_XL) | (1u << CH_XR));
		rd_request(RD_OFF, 0, 0, NULL);
	}
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
	if (!xa_buf[0] || !da_ring) return;
	if (xa.valid && raw[16] == xa.file && raw[17] == xa.chan && lba >= xa.first_lba) {   // mismo tramo (aunque ya acabara)
		if (lba > xa.emu_lba) xa.emu_lba = lba;   // lo lee y lo toca el hilo, por delante
		return;
	}
	// empieza (o vuelve a empezar) aquí: este sector suena ya y el hilo sigue desde el siguiente
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
	rd_request(RD_XA, lba + 1, 0, raw);
	snd_xa_pump();
}

// Una vez por frame: llena el anillo leyendo el disco por delante; para los canales al acabarse
static void da_fill(void);
static struct { int active, started, ended; u32 timer, t0, written; } da;   // started: canales en marcha

void snd_xa_pump(void)
{
	u32 played, head, tail;
	int eof;
	if (da.active) { da_fill(); return; }   // una pista de audio de CD en vez del XA
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
	mutexLock(&da_mtx);
	head = dr.head;
	tail = dr.tail;
	eof = dr.eof;
	mutexUnlock(&da_mtx);
	while (!xa.ended && xa.written - played < XA_LEAD && tail != head) {   // lo que ya trajo el hilo
		u32 slot = tail % DA_SLOTS;
		xa_put(da_ring + slot * 2352);
		xa.next_lba = ring_lba[slot] + 1;
		tail++;
	}
	if (tail == head && eof) xa.ended = 1;   // el hilo no puede leer más (fin del archivo o del disco)
	mutexLock(&da_mtx);   // sitio libre en el anillo y hasta dónde va la emulación: que el hilo siga
	dr.tail = tail;
	dr.emu = xa.emu_lba;
	condvarSignal(&da_cv);
	mutexUnlock(&da_mtx);
}

// ---- audio de CD (pistas de audio) ----
// Con Play, la pista suena desde aquí a su velocidad real (sectores de 588 pares L/R a 44100 Hz) en el mismo
// anillo y los mismos canales que el XA (el lector del CD solo toca una cosa a la vez); la lee el hilo de arriba.
static u32 da_played(void) { return (fast_ticks() - da.t0) / (2 * da.timer); }

static void da_halt(void)
{
	if (da.active) {
		if (da.started) soundStop((1u << CH_XL) | (1u << CH_XR));
		rd_request(RD_OFF, 0, 0, NULL);
	}
	da.active = da.started = 0;
}

// Los canales arrancan cuando ya hay música en el anillo (si no, el principio de la pista salía cortado)
static void da_start(void)
{
	soundPreparePcm(CH_XL, 2047, 0, da.timer, SoundMode_Repeat, SoundFmt_Pcm16, xa_buf[0], 0, XR / 2);
	soundPreparePcm(CH_XR, 2047, 127, da.timer, SoundMode_Repeat, SoundFmt_Pcm16, xa_buf[1], 0, XR / 2);
	soundStart((1u << CH_XL) | (1u << CH_XR));
	da.t0 = fast_ticks();
	da.started = 1;
	if (xa_muted) xa_volume();
}

// n muestras estéreo intercaladas (L, R) a los dos anillos desde la posición w, de dos en dos: una palabra de 32
// bits por canal y pareja (en la DSi, con escrituras de 16 bits a la RAM, la música de CD era el 1,2% del tiempo)
static void da_copy(u32 w, const u32 *src, u32 n)
{
	u32 *l = (u32 *)&xa_buf[0][w], *r = (u32 *)&xa_buf[1][w];
	for (u32 i = 0; i < n; i += 2) {
		u32 a = src[i], b = src[i + 1];   // L0 R0, L1 R1
		*l++ = (a & 0xffff) | (b << 16);
		*r++ = (a >> 16) | (b & 0xffff0000u);
	}
}

static void da_fill(void)
{
	u32 played = da.started ? da_played() : 0, head, tail;
	int eof;
	if (da.ended && played >= da.written) { da_halt(); return; }
	if (da.written < played) {   // se vació (un frame muy largo): lo que queda en el anillo es viejo
		dmaFillWords(0, xa_buf[0], XR * 2);
		dmaFillWords(0, xa_buf[1], XR * 2);
		DC_FlushRange(xa_buf[0], XR * 2);
		DC_FlushRange(xa_buf[1], XR * 2);
		da.written = (played + 512) & ~1u;   // par: da_copy escribe de dos en dos
	}
	mutexLock(&da_mtx);
	head = dr.head;
	tail = dr.tail;
	eof = dr.eof;
	mutexUnlock(&da_mtx);
	while (!da.ended && da.written - played < XA_LEAD) {
		const u32 *smp;
		u32 w = da.written % XR, n1;
		if (tail == head) { if (eof) da.ended = 1; break; }   // el hilo aún no lo trajo: en el frame que viene
		smp = (const u32 *)(da_ring + (tail % DA_SLOTS) * 2352);
		n1 = XR - w < 588 ? XR - w : 588;   // hasta el final del anillo y, si da la vuelta, el resto desde el principio
		da_copy(w, smp, n1);
		if (n1 < 588) da_copy(0, smp + n1, 588 - n1);
		if (w + 588 <= XR) { DC_FlushRange(&xa_buf[0][w], 588 * 2); DC_FlushRange(&xa_buf[1][w], 588 * 2); }
		else { DC_FlushRange(xa_buf[0], XR * 2); DC_FlushRange(xa_buf[1], XR * 2); }
		da.written += 588;
		tail++;
	}
	if (!da.started && da.written) da_start();
	if (tail != dr.tail) {   // sitio libre en el anillo: que el hilo siga leyendo
		mutexLock(&da_mtx);
		dr.tail = tail;
		condvarSignal(&da_cv);
		mutexUnlock(&da_mtx);
	}
}

static void da_play(u32 lba, u32 end)
{
	xa_halt();
	xa.valid = 0;
	da_halt();
	if (!da_read) return;
	memset(&da, 0, sizeof da);
	da.timer = soundTimerFromHz(44100);
	dmaFillWords(0, xa_buf[0], XR * 2);
	dmaFillWords(0, xa_buf[1], XR * 2);
	DC_FlushRange(xa_buf[0], XR * 2);
	DC_FlushRange(xa_buf[1], XR * 2);
	rd_request(RD_DA, lba, end, NULL);   // el hilo trae el primer bloque en unos ms; los canales arrancan con él (da_fill)
	da.active = 1;
}

static void xa_stop(void) { xa_halt(); xa.valid = 0; da_halt(); }
static void xa_mute(int on) { xa_muted = on; if (xa.active || da.active) xa_volume(); }
static const CdAudioHw snd_cd = { xa_sector, xa_stop, xa_mute, da_play };

void snd_set_disc(int (*read)(void *user, u32 lba, u8 *out)) { xa_read = read; }
void snd_set_cdda(u32 (*read)(void *user, u32 lba, u32 n, u8 *out)) { da_read = da_ring ? read : NULL; }

void snd_init(int dsi)
{
	for (int v = 0; v < 24; v++) ch_of[v] = -1;
	for (int c = 0; c < NCH; c++) voice_of[c] = -1;
	soundInit();
	soundPowerOn();
	soundSetMixerVolume(127);
	if (!spu_cache_mem) {   // caché de muestras en PCM: en modo DS (4 MB) solo cabe una pequeña
		spu_cache_size = dsi ? 1024 * 1024 : SND_DS_CACHE_KB * 1024;
		while (spu_cache_size >= 32 * 1024 && !(spu_cache_mem = memalign(32, spu_cache_size))) spu_cache_size /= 2;
		if (!spu_cache_mem) spu_cache_size = 0;
		printf("Sonido: cache %lu KB\n", spu_cache_size / 1024);
	}
	spu_hw = &snd_hw;
	xa_buf[0] = memalign(32, XR * 2);
	xa_buf[1] = memalign(32, XR * 2);
	if (xa_buf[0] && xa_buf[1]) cd_audio_hw = &snd_cd;
	if (!da_ring && (da_ring = memalign(32, DA_SLOTS * 2352))) {   // el lector de la música, por encima de la emulación
		threadPrepare(&da_thread, da_reader, NULL, &da_stack[sizeof da_stack], MAIN_THREAD_PRIO - 4);
		threadAttachLocalStorage(&da_thread, NULL);
		threadStart(&da_thread);
	}
}

void snd_pause(int on)
{
	soundSetMixerVolume(on ? 0 : 127);
}

// Sonido apagado desde la lista de juegos: la SPU sigue emulándose (el juego la necesita), pero sin tocar nada
void snd_enable(int on)
{
	spu_hw = on ? &snd_hw : NULL;
	if (!on) cd_audio_hw = NULL;
}
