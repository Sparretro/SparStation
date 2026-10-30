// SIO0: mando digital en el puerto 1 y tarjeta de memoria en la ranura 1 (128 KB que da el frontend en
// psx_memcard; NULL = sin tarjeta). El puerto 2 está vacío.
// Respeta los tiempos del hardware: el byte tarda en transmitirse, luego llega la respuesta y después
// el pulso de ACK del mando (con su interrupción). Algunas librerías de mando fallan si todo es instantáneo.
#include "psx.h"
#ifdef PSX_TRACE_IO
#include <stdio.h>
#define SIO_LOG(...) do { if (psx_pad_buttons != 0xffff) printf(__VA_ARGS__); } while (0)
u32 sio_polls;   // consultas al mando (comando 0x42) desde el arranque
#else
#define SIO_LOG(...) do { } while (0)
#endif

u16 psx_pad_buttons = 0xffff;
u8 *psx_memcard;
u32 psx_memcard_writes;

// Tarjeta: comando en curso ('R', 'W', 'S'), paso dentro de él, sector y suma de comprobación.
// No va en los estados guardados (una transferencia dura milisegundos y el juego reintenta).
static u8 mc_cmd, mc_flag = 0x08, mc_msb, mc_lsb, mc_chk, mc_prev;
static int mc_step;
static u8 mc_buf[128];

static u32 ctrl, mode, baud, stat_irq, ack_line;
static u8 rx, rx_next;
static int rx_full, seq, transferring, ack_pending, will_ack;
static u32 xfer_end, ack_time, ack_end;

#define ACK_DELAY 170    // ciclos entre el fin del byte y el ACK
#define ACK_LEN 100      // duración del pulso de ACK

void sio_reset(void)
{
	ctrl = mode = baud = stat_irq = ack_line = 0;
	rx = 0xff;
	rx_full = seq = transferring = ack_pending = will_ack = 0;
	mc_step = 0;
}

void sio_memcard_reset(void) { mc_flag = 0x08; mc_step = 0; }

// Tarjeta vacía formateada: cabecera «MC», 15 bloques libres, sin sectores rotos
void psx_memcard_format(u8 *card)
{
	for (int i = 0; i < 128 * 1024; i++) card[i] = 0;
	for (int f = 0; f < 64; f++) {
		u8 *p = card + f * 128, x = 0;
		if (f == 0 || f == 63) { p[0] = 'M'; p[1] = 'C'; }
		else if (f < 16) { p[0] = 0xa0; p[8] = p[9] = 0xff; }
		else if (f < 36) { p[0] = p[1] = p[2] = p[3] = 0xff; p[8] = p[9] = 0xff; }
		else continue;
		for (int i = 0; i < 127; i++) x ^= p[i];
		p[127] = x;
	}
}

// Un byte de la conversación con la tarjeta (tras el 81h). *ack = 0 en el último byte.
static u8 memcard_byte(u8 v, int *ack)
{
	int st = mc_step++;
	u32 addr = (u32)mc_msb << 8 | mc_lsb, sector = addr & 0x3ff;
	*ack = 1;
	if (st == 0) {   // comando
		mc_cmd = v;
		if (v != 'R' && v != 'W' && v != 'S') { *ack = 0; return 0xff; }
		return mc_flag;
	}
	if (st == 1) return 0x5a;
	if (st == 2) return 0x5d;
	if (mc_cmd == 'S') {
		static const u8 id[6] = { 0x5c, 0x5d, 0x04, 0x00, 0x00, 0x80 };
		if (st >= 8) *ack = 0;
		return st < 9 ? id[st - 3] : 0xff;
	}
	if (st == 3) { mc_msb = v; mc_chk = v; return 0x00; }
	if (st == 4) { mc_lsb = v; mc_chk ^= v; mc_prev = v; return mc_msb; }
	if (mc_cmd == 'R') {
		int bad = addr > 0x3ff;
		switch (st) {
		case 5: return 0x5c;
		case 6: return 0x5d;
		case 7: return bad ? 0xff : mc_msb;
		case 8: if (bad) *ack = 0; return bad ? 0xff : mc_lsb;
		}
		if (st < 9 + 128) {
			u8 d = psx_memcard[sector * 128 + st - 9];
			mc_chk ^= d;
			return d;
		}
		if (st == 9 + 128) return mc_chk;
		*ack = 0;
		return 0x47;
	}
	// 'W': 128 datos y la suma (cada respuesta es el byte anterior), luego 5Ch, 5Dh y el estado final
	if (st < 5 + 128) {
		u8 r = mc_prev;
		mc_buf[st - 5] = v;
		mc_chk ^= v;
		mc_prev = v;
		return r;
	}
	if (st == 5 + 128) {
		u8 r = mc_prev;
		if (addr > 0x3ff) mc_prev = 0xff;
		else if (v != mc_chk) mc_prev = 0x4e;
		else {
			for (int i = 0; i < 128; i++) psx_memcard[sector * 128 + i] = mc_buf[i];
			psx_memcard_writes++;
			mc_prev = 0x47;
		}
		mc_flag &= ~0x08;
		return r;
	}
	if (st == 6 + 128) return 0x5c;
	if (st == 7 + 128) return 0x5d;
	*ack = 0;
	return mc_prev;
}

static u32 transfer_cycles(void)
{
	static const u32 factor[4] = { 1, 1, 16, 64 };
	u32 b = baud ? baud : 0x88;
	return b * factor[mode & 3] * 8;
}

static void transfer(u8 v)
{
	u8 resp = 0xff;
	will_ack = 0;
#ifdef PSX_TRACE_IO
	if (seq == 1 && v == 0x42) sio_polls++;
#endif
	SIO_LOG("SIO tx %02x ctrl=%04x seq=%d (t=%u)\n", v, ctrl, seq, psx_cycles);
	if (!(ctrl & 2) || (ctrl & 0x2000)) {   // sin seleccionar, o puerto 2 (vacío)
		seq = 0;
	} else switch (seq) {
	case 0:
		if (v == 0x01) { will_ack = 1; seq = 1; }
		else if (v == 0x81 && psx_memcard) { will_ack = 1; seq = 10; mc_step = 0; }
		break;
	case 10: resp = memcard_byte(v, &will_ack); if (!will_ack) seq = 0; break;
	case 1: if (v == 0x42) { resp = 0x41; will_ack = 1; seq = 2; } else seq = 0; break;
	case 2: resp = 0x5a; will_ack = 1; seq = 3; break;
	case 3: resp = psx_pad_buttons & 0xff; will_ack = 1; seq = 4; break;
	case 4: resp = psx_pad_buttons >> 8; seq = 0; break;
	}
	STAT_ADD(sio_bytes, 1);
	if (seq == 2) STAT_ADD(sio_polls, 1);
	rx_next = resp;
	transferring = 1;
	xfer_end = psx_cycles + transfer_cycles();
	psx_schedule(xfer_end);
	ack_pending = 0;
	ack_line = 0;
}

int sio_next_event(u32 *when)
{
	int have = 0;
	if (transferring) { *when = xfer_end; have = 1; }
	if (ack_pending && (!have || TIME_BEFORE(ack_time, *when))) { *when = ack_time; have = 1; }
	if (ack_line && (!have || TIME_BEFORE(ack_end, *when))) { *when = ack_end; have = 1; }
	return have;
}

void sio_tick(void)
{
	if (transferring && !TIME_BEFORE(psx_cycles, xfer_end)) {
		transferring = 0;
		rx = rx_next;
		rx_full = 1;
		if (will_ack) {
			ack_pending = 1;
			ack_time = xfer_end + ACK_DELAY;
		}
	}
	if (ack_pending && !TIME_BEFORE(psx_cycles, ack_time)) {
		ack_pending = 0;
		ack_line = 1;
		ack_end = ack_time + ACK_LEN;
		STAT_ADD(sio_acks, 1);
		SIO_LOG("    ACK%s (t=%u)\n", (ctrl & 0x1000) ? " + IRQ7" : "", psx_cycles);
		if (ctrl & 0x1000) {
			stat_irq = 1;
			psx_irq(PSX_IRQ_SIO0);
		}
	}
	if (ack_line && !TIME_BEFORE(psx_cycles, ack_end)) ack_line = 0;
}

u32 sio_read(u32 off, int size)
{
	(void)size;
	switch (off) {
	case 0x0:
		SIO_LOG("  SIO rx %02x%s (t=%u pc=%08x)\n", rx, rx_full ? "" : " [vacío]", psx_cycles, cpu.cur_pc);
		if (rx_full) { rx_full = 0; return rx; }
		return 0xff;
	case 0x4: return 1 | (rx_full << 1) | ((!transferring) << 2) | (ack_line << 7) | (stat_irq << 9);
	case 0x8: return mode;
	case 0xa: return ctrl;
	case 0xe: return baud;
	}
	return 0;
}

void sio_write(u32 off, u32 v, int size)
{
	(void)size;
	switch (off) {
	case 0x0: transfer((u8)v); break;
	case 0x8: mode = v & 0xffff; break;
	case 0xa:
		SIO_LOG("    ctrl <- %04x (t=%u pc=%08x)\n", v, psx_cycles, cpu.cur_pc);
		ctrl = v & 0xffff;
		if (v & 0x10) stat_irq = 0;
		if (v & 0x40) sio_reset();
		if (!(v & 2)) { seq = 0; mc_step = 0; ack_line = 0; ack_pending = 0; }
		ctrl &= ~0x50;
		break;
	case 0xe: baud = v & 0xffff; break;
	}
}

void sio_state(PsxState *s)
{
	STATE(s, ctrl); STATE(s, mode); STATE(s, baud); STATE(s, stat_irq); STATE(s, ack_line);
	STATE(s, rx); STATE(s, rx_next);
	STATE(s, rx_full); STATE(s, seq); STATE(s, transferring); STATE(s, ack_pending); STATE(s, will_ack);
	STATE(s, xfer_end); STATE(s, ack_time); STATE(s, ack_end);
}
