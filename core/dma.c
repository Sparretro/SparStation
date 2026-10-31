// DMA: las transferencias se hacen de golpe al arrancarlas; la CPU no se entera de que hubo pausa.
#include "psx.h"
#ifdef PSX_DYNAREC
#define dyn_ram_written_if(o, l) dyn_ram_written(o, l)
#else
#define dyn_ram_written_if(o, l) ((void)0)
#endif
#ifdef PSX_TRACE_IO
#include <stdio.h>
#endif

enum { CH_MDEC_IN, CH_MDEC_OUT, CH_GPU, CH_CDROM, CH_SPU, CH_PIO, CH_OTC };

static u32 madr[7], bcr[7], chcr[7];
static u32 dpcr = 0x07654321, dicr;

static void dicr_update_master(void)
{
	int old = (dicr >> 31) & 1;
	int on = ((dicr >> 15) & 1) || (((dicr >> 23) & 1) && ((dicr >> 24) & (dicr >> 16) & 0x7f));
	if (on) dicr |= 1u << 31; else dicr &= ~(1u << 31);
	if (on && !old) psx_irq(PSX_IRQ_DMA);
}

static void finish(int ch)
{
	chcr[ch] &= ~((1 << 24) | (1 << 28));
	if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
	dicr_update_master();
}

static inline u32 *ram32(u32 a) { return (u32 *)(psx_ram + (a & 0x1ffffc)); }

// La salida del MDEC no puede ir de golpe: el SDK arranca este canal antes de mandar los datos a
// decodificar, y en la consola real el canal espera. Aquí se queda a medias y mdec lo reanuda.
static u32 mdec_out_left, mdec_out_addr;

static void mdec_out_pump(void)
{
	while (mdec_out_left && mdec_out_available()) {
		*ram32(mdec_out_addr) = mdec_read_data();
		DYN_CHECK_WRITE(mdec_out_addr);
		mdec_out_addr += 4;
		mdec_out_left--;
	}
	if (!mdec_out_left) {
		madr[CH_MDEC_OUT] = mdec_out_addr & 0xffffff;
		finish(CH_MDEC_OUT);
	}
}

void dma_mdec_kick(void)
{
	if (mdec_out_left && (chcr[CH_MDEC_OUT] & (1 << 24))) mdec_out_pump();
}

static void run(int ch)
{
	u32 c = chcr[ch], addr = madr[ch] & 0xfffffc;
	int from_ram = c & 1, step = (c & 2) ? -4 : 4, sync = (c >> 9) & 3;

	if (ch == CH_MDEC_OUT) {
		if (sync == 0) { mdec_out_left = bcr[ch] & 0xffff; if (!mdec_out_left) mdec_out_left = 0x10000; }
		else mdec_out_left = (bcr[ch] & 0xffff) * (bcr[ch] >> 16);
		mdec_out_addr = addr;
		mdec_out_pump();
		return;
	}

	if (ch == CH_OTC) {
		u32 n = bcr[ch] & 0xffff, lo;
		if (!n) n = 0x10000;
		lo = (addr & 0x1ffffc) - (n - 1) * 4;
		if ((addr & 0x1ffffc) >= (n - 1) * 4) {   // sin dar la vuelta a la RAM: de un tirón y el código traducido una vez
			u32 *p = ram32(addr), a = addr;
			for (u32 i = n - 1; i; i--) { *p-- = (a - 4) & 0xffffff; a -= 4; }
			*p = 0xffffff;
			dyn_ram_written_if(lo, n * 4);
			finish(ch);
			return;
		}
		while (n--) {
			*ram32(addr) = n ? ((addr - 4) & 0xffffff) : 0xffffff;
			DYN_CHECK_WRITE(addr);
			addr -= 4;
		}
		finish(ch);
		return;
	}

	if (sync == 2) {   // lista enlazada (GPU)
		int guard = 0x80000;
		PROF_BEGIN();
		for (;;) {
			u32 hdr = *ram32(addr), n = hdr >> 24;
			if (n && (addr & 0x1ffffc) + n * 4 < 0x200000) gpu_write_gp0_n(ram32(addr + 4), n);   // el paquete entero
			else for (u32 i = 1; i <= n; i++) gpu_write_gp0(*ram32(addr + i * 4));
			if ((hdr & 0x800000) || --guard == 0) break;
			addr = hdr & 0xfffffc;
		}
		PROF_END(PROF_GPU);
		madr[ch] = 0xffffff;
		finish(ch);
		return;
	}

	{
		u32 words;
		if (sync == 0) { words = bcr[ch] & 0xffff; if (!words) words = 0x10000; }
		else words = (bcr[ch] & 0xffff) * (bcr[ch] >> 16);
#ifdef PSX_TRACE_IO
		if (ch == CH_CDROM) printf("DMA3 -> %06x  %u palabras (bcr=%08x chcr=%08x)\n", addr, words, bcr[ch], c);
#endif
		while (words--) {
			if (from_ram) {
				u32 v = *ram32(addr);
				if (ch == CH_GPU) gpu_write_gp0(v);
				else if (ch == CH_SPU) spu_dma_write(v);
				else if (ch == CH_MDEC_IN) mdec_write_cmd(v);
			} else {
				u32 v = 0;
				if (ch == CH_GPU) v = gpu_read();
				else if (ch == CH_CDROM) v = cdrom_dma_word();
				else if (ch == CH_SPU) v = spu_dma_read();
				else if (ch == CH_MDEC_OUT) v = mdec_read_data();
				*ram32(addr) = v;
				DYN_CHECK_WRITE(addr);
			}
			addr += step;
		}
		if (sync == 1) { madr[ch] = addr & 0xffffff; bcr[ch] &= 0xffff; }
	}
	finish(ch);
}

u32 dma_read(u32 off)
{
	int ch = off >> 4;
	if (ch == 7) return (off & 0xc) == 0 ? dpcr : (off & 0xc) == 4 ? dicr : 0;
	switch (off & 0xc) {
	case 0: return madr[ch];
	case 4: return bcr[ch];
	case 8: return chcr[ch];
	}
	return 0;
}

void dma_write(u32 off, u32 v)
{
	int ch = off >> 4;
	if (ch == 7) {
		if ((off & 0xc) == 0) dpcr = v;
		else if ((off & 0xc) == 4) {
#ifdef PSX_TRACE_IO
			{ static int n; if (psx_cycles > 700000000u && n++ < 40) printf("DICR <- %08x (antes %08x) pc=%08x\n", v, dicr, cpu.cur_pc); }
#endif
			dicr = (dicr & 0x7f000000 & ~(v & 0x7f000000)) | (v & 0x00ff803f);
			dicr_update_master();
		}
		return;
	}
	switch (off & 0xc) {
	case 0: madr[ch] = v & 0xffffff; break;
	case 4: bcr[ch] = v; break;
	case 8:
		if (ch == CH_OTC) v = (v & 0x51000000) | 2;
		chcr[ch] = v;
		if ((v & (1 << 24)) && (dpcr & (8u << (ch * 4)))) {
			int sync = (v >> 9) & 3;
			if (sync != 0 || (v & (1 << 28))) run(ch);
		}
		break;
	}
}

void dma_state(PsxState *s)
{
	STATE(s, madr);
	STATE(s, bcr);
	STATE(s, chcr);
	STATE(s, dpcr);
	STATE(s, dicr);
	STATE(s, mdec_out_left);
	STATE(s, mdec_out_addr);
}
