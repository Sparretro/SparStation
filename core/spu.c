// SPU de la PS1: registros, RAM de sonido, envolventes ADSR y posición de cada voz en el tiempo de la PS1.
//
// No se mezcla audio aquí: cada vez que el juego da «key on» a una voz, su muestra ADPCM se descodifica
// una sola vez a PCM de 16 bits (caché en anillo, se invalida si el juego reescribe esa zona de la RAM de
// sonido) y el frontend la toca con un canal de sonido de hardware (spu_hw). Después solo se le pasan los
// cambios de volumen (envolvente x volumen de la voz x volumen general) y de tono. Así el sonido va a su
// velocidad real aunque la emulación vaya lenta, y casi no gasta CPU de la DS.
//
// Para el juego, la voz avanza en el tiempo de la PS1 (cada 32 muestras): ENDX, ENVX y la IRQ de la SPU
// se calculan con esa posición. Sin reverberación, ruido ni modulación de tono (de momento).
#include "psx.h"
#include <stdlib.h>
#ifdef PSX_SPULOG
#include <stdio.h>
#endif

u16 *spu_mem;   // RAM de sonido (512 KB): si el frontend no la pone antes de psx_reset, se reserva aquí
const SpuHw *spu_hw;
u8 *spu_cache_mem;   // caché de muestras en PCM (el frontend la puede poner antes de psx_reset)
u32 spu_cache_size = 1024 * 1024;

static u16 spu_regs[256];
static u32 spu_addr;

#define TICK_SAMPLES 32
#define TICK_CYCLES (TICK_SAMPLES * 768)   // 33,8688 MHz / 44100 Hz = 768 ciclos por muestra
#define HW_EVERY 4                          // al hardware, uno de cada 4 pasos (~345 veces por segundo de la PS1)

enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct {
	u8 phase, hw_on;
	s32 env;          // 0..0x7fff
	u32 env_cnt;      // muestras acumuladas hacia el siguiente paso de la envolvente
	u32 pos;          // muestra actual en 1/4096
	s32 smp;          // entrada de la caché (-1 = ninguna)
	u32 start;        // dirección de inicio en bytes
	u16 sent_l, sent_r, sent_pitch;
} Voice;

static Voice voice[24];
static u32 endx, tick_acc, tick_n, irq_flag;

// ---- caché de muestras descodificadas ----
#define MAX_SMP 256
typedef struct {
	u32 addr, end;     // bytes de la RAM de sonido que ocupa el ADPCM: [addr, end)
	u32 stamp;         // escrituras vistas al descodificar
	u32 off, len;      // en el anillo (bytes) y longitud en muestras
	s32 loop;          // muestra donde vuelve el bucle, -1 = suena una vez
	u32 loop_addr;     // dirección de la vuelta (para la IRQ)
	u8 used;
} Smp;
static Smp smp[MAX_SMP];
static u32 smp_next;   // siguiente entrada que se reutiliza (en orden circular)
static u32 ring_head;
static u32 write_seq = 1, page_stamp[256];   // páginas de 2 KB

static void mark_written(u32 addr) { page_stamp[(addr >> 11) & 255] = write_seq; }

static int smp_valid(const Smp *s)
{
	if (!s->used) return 0;
	for (u32 p = s->addr >> 11; p <= ((s->end - 1) >> 11) && p < 256; p++)
		if (page_stamp[p] >= s->stamp) return 0;
	return 1;
}

#define NO_PCM 0xffffffffu   // entrada sin PCM (no cabía): solo cuenta el tiempo

static s16 *ring_alloc(u32 bytes, u32 *off)
{
	u32 o;
	if (bytes > spu_cache_size) return NULL;
	if (ring_head + bytes > spu_cache_size) ring_head = 0;
	o = ring_head;
	ring_head += bytes;
	for (int i = 0; i < MAX_SMP; i++)   // lo que había en ese trozo del anillo se pierde
		if (smp[i].used && smp[i].off != NO_PCM && smp[i].off < o + bytes && smp[i].off + smp[i].len * 2 > o) smp[i].used = 0;
	*off = o;
	return (s16 *)(spu_cache_mem + o);
}

#define MAX_SAMPLES (256 * 1024)

// ADPCM de la SPU: bloques de 16 bytes = 28 muestras. Bandera 1 = fin, 2 = repetir, 4 = inicio del bucle.
static s32 decode(u32 addr)
{
	static const s32 f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
	const u16 *ram = spu_mem;   // en medias palabras: en el banco de pruebas la RAM de sonido está en el Expansion Pak
	u32 a = addr & 0x7fff0, blocks = 0, loop_blk = ~0u, loop_addr = addr;
	int repeat = 0;
	s32 old = 0, older = 0;
	Smp *s;
	s16 *out;
	u32 off;
	// primero la longitud: hasta el bloque con la bandera de fin
	for (;;) {
		u8 fl = (u8)(ram[((a + blocks * 16) & 0x7ffff) >> 1] >> 8);
		if (fl & 4) { loop_blk = blocks; loop_addr = (a + blocks * 16) & 0x7ffff; }
		blocks++;
		if (fl & 1) { repeat = fl & 2; break; }
		if (blocks * 28 >= MAX_SAMPLES) break;
	}
	// sin sitio para el PCM la entrada vale igual: el juego necesita la longitud y el bucle (ENDX, fin de la voz)
	out = ring_alloc(blocks * 56, &off);
	if (!out) off = NO_PCM;
	s = &smp[smp_next];
	smp_next = (smp_next + 1) % MAX_SMP;
	s->addr = a;
	s->end = a + blocks * 16;
	s->stamp = ++write_seq;   // lo escrito antes queda por debajo; lo que se escriba ahora, no
	s->off = off;
	s->len = blocks * 28;
	s->loop = repeat ? (s32)(loop_blk != ~0u ? loop_blk * 28 : 0) : -1;
	s->loop_addr = loop_addr;
	s->used = 1;
	if (!out) return (s32)(s - smp);
	for (u32 b = 0; b < blocks; b++) {
		const u16 *p = ram + (((a + b * 16) & 0x7ffff) >> 1);
		int shift = p[0] & 15, filt = (p[0] >> 4) & 7;
		s32 k0, k1;
		if (shift > 12) shift = 9;
		if (filt > 4) filt = 4;
		k0 = f0[filt];
		k1 = f1[filt];
		for (int i = 0; i < 28; i++) {
			s32 nib = (p[1 + i / 4] >> ((i & 3) * 4)) & 15;
			s32 v = ((s32)((u32)nib << 28) >> 16) >> shift;   // nibble con signo en los 4 bits altos de 16
			v += (old * k0 + older * k1 + 32) >> 6;
			if (v > 32767) v = 32767;
			if (v < -32768) v = -32768;
			*out++ = (s16)v;
			older = old;
			old = v;
		}
	}
	return (s32)(s - smp);
}

static s32 find_sample(u32 addr)
{
	addr &= 0x7fff0;
	for (int i = 0; i < MAX_SMP; i++)
		if (smp[i].used && smp[i].addr == addr && smp_valid(&smp[i])) return i;
	return decode(addr);
}

// ---- volúmenes ----
static s32 vol_of(u16 r)   // modo fijo: 15 bits con signo x2; barrido: se toma como volumen máximo
{
	s32 v;
	if (r & 0x8000) return 0x7fff;
	v = (s32)((u32)r << 17) >> 16;
	return v < 0 ? -v : v;
}

static void voice_out(int n, u16 *l, u16 *r)
{
	const u16 *vr = &spu_regs[n * 8];
	s32 env = voice[n].env;
	u32 cnt = spu_regs[0x1aa / 2];
	if (!(cnt & 0x8000) || !(cnt & 0x4000)) { *l = *r = 0; return; }   // SPU apagada o silenciada
	*l = (u16)((((vol_of(vr[0]) * env) >> 15) * vol_of(spu_regs[0x180 / 2])) >> 15);
	*r = (u16)((((vol_of(vr[1]) * env) >> 15) * vol_of(spu_regs[0x182 / 2])) >> 15);
}

// ---- envolvente ADSR (en pasos de TICK_SAMPLES muestras) ----
static void env_run(Voice *v, int shift, int step, int exp, int dec, u32 n)
{
	// shift 0..31: como mucho 1 << 20 (en ARM un desplazamiento de 32 o más da 0 y el bucle no acabaría)
	u32 cycles = shift > 11 ? 1u << (shift - 11) : 1;
	s32 st = dec ? step - 8 : 7 - step;
	if (shift < 11) st <<= 11 - shift;
	if (exp && !dec && v->env > 0x6000) cycles <<= 2;
	v->env_cnt += n;
	while (v->env_cnt >= cycles) {
		s32 d = st;
		v->env_cnt -= cycles;
		if (exp && dec) d = (st * v->env) >> 15;
		v->env += d;
		if (v->env > 0x7fff) { v->env = 0x7fff; v->env_cnt = 0; break; }
		if (v->env < 0) { v->env = 0; v->env_cnt = 0; break; }
		if (exp && !dec && v->env > 0x6000 && cycles < 4) cycles <<= 2;
	}
}

static void env_step(int n, u32 samples)
{
	Voice *v = &voice[n];
	u16 a1 = spu_regs[n * 8 + 4], a2 = spu_regs[n * 8 + 5];
	switch (v->phase) {
	case PH_ATTACK:
		env_run(v, (a1 >> 10) & 31, (a1 >> 8) & 3, a1 >> 15, 0, samples);
		if (v->env >= 0x7fff) { v->phase = PH_DECAY; v->env_cnt = 0; }
		break;
	case PH_DECAY: {
		s32 sus = ((a1 & 15) + 1) * 0x800;
		env_run(v, (a1 >> 4) & 15, 0, 1, 1, samples);
		if (v->env <= sus) { v->phase = PH_SUSTAIN; v->env_cnt = 0; }
		break;
	}
	case PH_SUSTAIN:
		env_run(v, (a2 >> 8) & 31, (a2 >> 6) & 3, a2 >> 15, (a2 >> 14) & 1, samples);
		break;
	case PH_RELEASE:
		env_run(v, a2 & 31, 0, (a2 >> 5) & 1, 1, samples);
		if (v->env <= 0) v->phase = PH_OFF;
		break;
	}
}

static void hw_stop(int n)
{
	if (voice[n].hw_on && spu_hw) spu_hw->stop(n);
	voice[n].hw_on = 0;
}

static void key_on(int n)
{
	Voice *v = &voice[n];
	v->start = (u32)spu_regs[n * 8 + 3] << 3;
	v->phase = PH_ATTACK;
	v->env = 0;
	v->env_cnt = 0;
	v->pos = 0;
	endx &= ~(1u << n);
	v->smp = spu_mem ? find_sample(v->start) : -1;
	if (v->smp >= 0) spu_regs[n * 8 + 7] = (u16)(smp[v->smp].loop_addr >> 3);
#ifdef PSX_SPULOG
	if (v->smp >= 0) printf("KON v%02d addr=%05x len=%u loop=%d pitch=%04x adsr=%04x/%04x pcm=%s\n", n, v->start, smp[v->smp].len, smp[v->smp].loop, spu_regs[n * 8 + 2], spu_regs[n * 8 + 4], spu_regs[n * 8 + 5], smp[v->smp].off == NO_PCM ? "NO" : "si");
#endif
	hw_stop(n);
	if (spu_hw && v->smp >= 0 && smp[v->smp].off != NO_PCM) {
		const Smp *s = &smp[v->smp];
		u16 pitch = spu_regs[n * 8 + 2];
		v->sent_l = v->sent_r = 0;
		v->sent_pitch = pitch;
		spu_hw->key_on(n, (const s16 *)(spu_cache_mem + s->off), s->len, s->loop, pitch);
		v->hw_on = 1;
	}
}

// IRQ de la SPU: salta cuando una voz lee la dirección marcada (0x1A4) con SPUCNT bit 6
static void check_irq(u32 from, u32 to)
{
	u32 cnt = spu_regs[0x1aa / 2], ia = (u32)spu_regs[0x1a4 / 2] << 3;
	if (!(cnt & 0x40) || irq_flag) return;
	if (ia >= (from & ~15u) && ia < ((to + 15) & ~15u)) {
		irq_flag = 1;
		psx_irq(PSX_IRQ_SPU);
	}
}

static void tick(void)
{
	int to_hw = ++tick_n % HW_EVERY == 0;
	for (int n = 0; n < 24; n++) {
		Voice *v = &voice[n];
		if (v->phase == PH_OFF) continue;
		env_step(n, TICK_SAMPLES);
		if (v->smp >= 0) {
			const Smp *s = &smp[v->smp];
			u32 pitch = spu_regs[n * 8 + 2], end = s->len << 12, p0 = v->pos;
			if (pitch > 0x3fff) pitch = 0x3fff;
			v->pos += pitch * TICK_SAMPLES;
			check_irq(s->addr + (p0 >> 12) / 28 * 16, s->addr + (v->pos >> 12) / 28 * 16);
			if (v->pos >= end) {
				endx |= 1u << n;
				if (s->loop >= 0 && (u32)s->loop < s->len) v->pos = ((u32)s->loop << 12) + (v->pos - end) % ((s->len - s->loop) << 12);
				else { v->phase = PH_OFF; v->env = 0; }
			}
		} else if (v->phase != PH_RELEASE) {
			endx |= 1u << n;
		}
		if (v->phase == PH_OFF) { v->env = 0; hw_stop(n); continue; }
		if (to_hw && v->hw_on) {
			u16 l, r, pitch = spu_regs[n * 8 + 2];
			voice_out(n, &l, &r);
			// solo si cambia de verdad (cada orden al ARM7 cuesta): volumen en pasos de 1/256
			if ((l >> 7) != (v->sent_l >> 7) || (r >> 7) != (v->sent_r >> 7) || pitch != v->sent_pitch) {
				v->sent_l = l;
				v->sent_r = r;
				v->sent_pitch = pitch;
				spu_hw->update(n, pitch, l, r);
			}
		}
	}
	if (spu_hw && spu_hw->mix) spu_hw->mix(TICK_SAMPLES);
}

PSX_ITCM void spu_advance(u32 cycles)   // en la ITCM (1% en la DSi real con Tomb Raider)
{
	tick_acc += cycles;
	while (tick_acc >= TICK_CYCLES) {
		tick_acc -= TICK_CYCLES;
		tick();
	}
}

void spu_reset(void)
{
	if (!spu_mem) spu_mem = malloc(512 * 1024);
	if (spu_mem) memset(spu_mem, 0, 512 * 1024);
	if (!spu_cache_mem) spu_cache_mem = malloc(spu_cache_size);
	memset(spu_regs, 0, sizeof spu_regs);
	spu_addr = 0;
	for (int n = 0; n < 24; n++) {
		hw_stop(n);
		memset(&voice[n], 0, sizeof voice[n]);
		voice[n].smp = -1;
	}
	memset(smp, 0, sizeof smp);
	smp_next = ring_head = 0;
	endx = tick_acc = tick_n = irq_flag = 0;
	write_seq++;
	if (!spu_cache_mem) spu_cache_size = 0;
}

u32 spu_read16(u32 off)
{
	off &= 0x1fe;
	if (off < 0x180) {
		int n = off >> 4;
		if ((off & 15) == 0xc) return (u16)voice[n].env;
		return spu_regs[off / 2];
	}
	switch (off) {
	case 0x19c: return endx & 0xffff;
	case 0x19e: return endx >> 16;
	case 0x1a6: return spu_addr >> 3;
	case 0x1ae: return (spu_regs[0x1aa / 2] & 0x3f) | (irq_flag << 6);   // SPUSTAT refleja el modo de SPUCNT
	case 0x1b8: return spu_regs[0x180 / 2] & 0x7fff;   // volumen general actual
	case 0x1ba: return spu_regs[0x182 / 2] & 0x7fff;
	}
	return spu_regs[off / 2];
}

static void write_ram(u16 v)
{
	if (spu_mem) spu_mem[(spu_addr >> 1) & 0x3ffff] = v;
	mark_written(spu_addr);
	if ((spu_regs[0x1aa / 2] & 0x40) && !irq_flag && (spu_addr & ~15u) == ((u32)spu_regs[0x1a4 / 2] << 3)) {
		irq_flag = 1;
		psx_irq(PSX_IRQ_SPU);
	}
	spu_addr = (spu_addr + 2) & 0x7ffff;
}

void spu_write16(u32 off, u32 v)
{
	off &= 0x1fe;
	spu_regs[off / 2] = (u16)v;
	switch (off) {
	case 0x188: case 0x18a: {   // key on (voces 0-15 / 16-23)
		u32 bits = (v & 0xffff) << (off == 0x18a ? 16 : 0);
		for (int n = 0; n < 24; n++) if (bits & (1u << n)) key_on(n);
		break;
	}
	case 0x18c: case 0x18e: {   // key off
		u32 bits = (v & 0xffff) << (off == 0x18e ? 16 : 0);
		for (int n = 0; n < 24; n++)
			if ((bits & (1u << n)) && voice[n].phase != PH_OFF) { voice[n].phase = PH_RELEASE; voice[n].env_cnt = 0; }
		break;
	}
	case 0x1a6: spu_addr = (v & 0xffff) << 3; write_seq++; break;
	case 0x1a8: write_ram((u16)v); break;
	case 0x1aa: if (!(v & 0x40)) irq_flag = 0; break;
	}
}

void spu_dma_write(u32 v)
{
	write_ram((u16)v);
	write_ram((u16)(v >> 16));
}

u32 spu_dma_read(void)
{
	u32 v = 0;
	if (spu_mem) v = spu_mem[(spu_addr >> 1) & 0x3ffff] | (spu_mem[((spu_addr + 2) >> 1) & 0x3ffff] << 16);
	spu_addr = (spu_addr + 4) & 0x7ffff;
	return v;
}

void spu_state(PsxState *s)
{
	STATE(s, spu_regs);
	STATE(s, spu_addr);
	if (spu_mem) psx_state_io(s, spu_mem, 512 * 1024);

	if (s->load) {   // las voces no van en el estado: calladas hasta su próximo key on
		for (int n = 0; n < 24; n++) {
			hw_stop(n);
			memset(&voice[n], 0, sizeof voice[n]);
			voice[n].smp = -1;
		}
		memset(smp, 0, sizeof smp);
		endx = irq_flag = 0;
		write_seq++;
	}
}
