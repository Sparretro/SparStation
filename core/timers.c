// Los tres contadores de la PS1. El 1 puede contar líneas (hblank); el 2, ciclos/8.
#include "psx.h"

typedef struct { u32 count, mode, target; int fired; u32 frac; } Timer;
static Timer tm[3];
// Perezosos: se ponen al día (timers_sync) al leerlos o escribirlos y al final de cada tramo de ejecución;
// sus interrupciones se programan como eventos (timers_next_event)
static u32 t_last;        // psx_cycles hasta el que están al día
static u32 line_origin;   // ciclo del principio del frame (para contar líneas)

void timers_reset(void)
{
	memset(tm, 0, sizeof tm);
	t_last = line_origin = psx_cycles;
	for (int i = 0; i < 3; i++) tm[i].mode = 1 << 10;
}

static void tm_irq(int i)
{
	Timer *t = &tm[i];
	if (t->fired && !(t->mode & 0x40)) return;   // modo «una vez»
	t->fired = 1;
	if (t->mode & 0x80) t->mode ^= 1 << 10;       // modo alterno
	else t->mode &= ~(1 << 10);
	if (!(t->mode & (1 << 10)) || !(t->mode & 0x80)) psx_irq(PSX_IRQ_TMR0 + i);
	if (!(t->mode & 0x80)) t->mode |= 1 << 10;    // el pulso vuelve a 1 enseguida
}

static void tm_add(int i, u32 n)
{
	Timer *t = &tm[i];
	u32 old = t->count, c = old + n, tgt = t->target;
	int hit_t = 0, hit_f = 0;
	if (!n) return;
	// llega al objetivo al pasar de debajo a >= objetivo; en modo «reinicia» cuenta 0..objetivo
	hit_t = (old < tgt && c >= tgt) || (tgt == 0 && (t->mode & 8));
	if (t->mode & 8) {
		if (old <= tgt && c > tgt) {
			c -= tgt + 1;
			if (c > tgt) c %= tgt + 1;   // solo si da más de una vuelta (la división es cara en la DS)
		}
		else if (c > 0xffff) { hit_f = 1; c &= 0xffff; }
	} else if (c > 0xffff) {
		hit_f = 1;
		c &= 0xffff;
	}
	t->count = c;
	if (hit_t) { t->mode |= 1 << 11; if (t->mode & 0x10) tm_irq(i); }
	if (hit_f) { t->mode |= 1 << 12; if (t->mode & 0x20) tm_irq(i); }
}

static int stopped(int i)
{
	u32 m = tm[i].mode;
	if (!(m & 1)) return 0;
	if (i == 2) { u32 s = (m >> 1) & 3; return s == 0 || s == 3; }
	return 0;
}

static void timers_advance(u32 cycles)
{
	// timer 0: reloj de puntos aproximado como reloj de sistema
	if (!stopped(0)) tm_add(0, cycles);
	if (!(tm[1].mode & 0x100) && !stopped(1)) tm_add(1, cycles);
	if (!stopped(2)) {
		if (tm[2].mode & 0x200) {
			tm[2].frac += cycles;
			tm_add(2, tm[2].frac >> 3);
			tm[2].frac &= 7;
		} else tm_add(2, cycles);
	}
}

// Líneas: el temporizador 1 en modo hblank cuenta los finales de línea que hay entre t_last y psx_cycles
// (las líneas van de PSX_LINE_CYCLES en PSX_LINE_CYCLES desde line_origin, el principio del frame)

static u32 lines_at(u32 c) { return (s32)(c - line_origin) > 0 ? (c - line_origin) / PSX_LINE_CYCLES : 0; }

void timers_sync(void)
{
	u32 n = psx_cycles - t_last;
	if (!n) return;
	if ((tm[1].mode & 0x100) && !stopped(1)) tm_add(1, lines_at(psx_cycles) - lines_at(t_last));
	t_last = psx_cycles;
	timers_advance(n);
}

void timers_line_origin(u32 c)
{
	timers_sync();
	line_origin = c;
}

// Cuándo puede saltar la próxima interrupción de un temporizador (objetivo o desbordamiento)
int timers_next_event(u32 *when)
{
	int have = 0;
	for (int i = 0; i < 3; i++) {
		Timer *t = &tm[i];
		u32 left, c;
		if (!(t->mode & 0x30) || (t->fired && !(t->mode & 0x40)) || stopped(i) || !(irq_mask & (1u << (PSX_IRQ_TMR0 + i)))) continue;
		if ((t->mode & 0x10) && t->count < t->target) left = t->target - t->count;
		else left = 0x10000 - t->count;
		if (i == 1 && (t->mode & 0x100)) c = line_origin + (lines_at(t_last) + left) * PSX_LINE_CYCLES - t_last;   // en líneas
		else if (i == 2 && (t->mode & 0x200)) c = left * 8 - t->frac;
		else c = left;
		if (!have || TIME_BEFORE(t_last + c, *when)) { *when = t_last + c; have = 1; }
	}
	return have;
}


u32 timer_read(u32 off)
{
	int i = (off >> 4) & 3;
	Timer *t;
	u32 v;
	if (i > 2) return 0;
	timers_sync();
	t = &tm[i];
	switch (off & 0xc) {
	case 0: return t->count & 0xffff;
	case 4:
		v = t->mode;
		t->mode &= ~((1 << 11) | (1 << 12));
		return v;
	case 8: return t->target;
	}
	return 0;
}

void timer_write(u32 off, u32 v)
{
	int i = (off >> 4) & 3;
	Timer *t;
	if (i > 2) return;
	timers_sync();
	t = &tm[i];
	switch (off & 0xc) {
	case 0: t->count = v & 0xffff; break;
	case 4:
		t->mode = (v & 0x3ff) | (1 << 10);
		t->count = 0;
		t->fired = 0;
		t->frac = 0;
		break;
	case 8: t->target = v & 0xffff; break;
	}
}

void timers_state(PsxState *s)
{
	if (!s->load) timers_sync();
	STATE(s, tm);
	if (s->load) t_last = line_origin = psx_cycles;
}
