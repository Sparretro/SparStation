// SIO0: mando digital en el puerto 1. Las tarjetas de memoria no responden (no hay ninguna puesta).
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
		break;
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
		if (!(v & 2)) { seq = 0; ack_line = 0; ack_pending = 0; }
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
