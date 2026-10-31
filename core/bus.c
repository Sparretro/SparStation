// Mapa de memoria de la PS1: RAM, scratchpad, BIOS y registros de E/S.
#include "psx.h"
#ifdef PSX_TRACE_IO
#include <stdio.h>
int trace_n;
#endif

u8 *psx_ram;
u8 *psx_bios;
PSX_FAST u8 psx_scratch[1024];
PSX_FAST u32 irq_stat, irq_mask;

static u32 memctrl[9], ram_size;

static const u32 region_mask[8] = {
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,   // KUSEG
	0x7fffffff,                                       // KSEG0
	0x1fffffff,                                       // KSEG1
	0xffffffff, 0xffffffff,                           // KSEG2
};

static inline u32 phys(u32 a) { return a & region_mask[a >> 29]; }

void irq_update(void)
{
	if (irq_stat & irq_mask) {
		cpu.cop0[13] |= 0x400;
#ifdef PSX_DYNAREC
		psx_schedule(psx_cycles);   // los bloques encadenados solo miran el tiempo: así vuelven a C a atenderla
#endif
	} else cpu.cop0[13] &= ~0x400;
}

void psx_irq(int n)
{
	irq_stat |= 1u << n;
	irq_update();
}

// ---- E/S ----

static u32 io_read(u32 p, int size)
{
	u32 off = p - 0x1f801000;
#ifndef PSX_TRACE_IO
	if (off == 0x814) return gpu_stat();   // los juegos la sondean en bucle: va primero
#endif
	if (off < 0x24) return memctrl[off >> 2];
	if (off >= 0x40 && off < 0x50) return sio_read(off - 0x40, size);
	if (off >= 0x50 && off < 0x60) return off == 0x54 ? 0x5 : 0;   // SIO1: siempre listo, sin nada
	if (off == 0x60) return ram_size;
	if (off == 0x70) { timers_sync(); return irq_stat; }   // un temporizador enmascarado no corta la ejecución: ponerlo al día
	if (off == 0x74) return irq_mask;
	if (off >= 0x80 && off < 0x100) return dma_read(off - 0x80);
	if (off >= 0x100 && off < 0x130) return timer_read(off - 0x100);
	if (off >= 0x800 && off < 0x804) {
		u32 v = 0;   // las lecturas anchas leen bytes seguidos del FIFO
		for (int i = 0; i < size; i++) v |= cdrom_read(off - 0x800) << (i * 8);
		return v;
	}
	if (off == 0x810) return gpu_read();
	if (off == 0x814) {
#ifdef PSX_TRACE_IO
		static u32 last; static int n;
		u32 s = gpu_stat();
		if ((s ^ last) & 0x80000000 && n++ < 40) printf("GPUSTAT %08x pc=%08x t=%u istat=%x\n", s, cpu.cur_pc, psx_cycles, irq_stat);
		last = s;
#endif
		return gpu_stat();
	}
	if (off == 0x820) return mdec_read_data();
	if (off == 0x824) return mdec_status();
	if (off >= 0xc00 && off < 0xe00) {
		u32 v = spu_read16(off - 0xc00);
		if (size == 4) v |= spu_read16(off - 0xc00 + 2) << 16;
		return v;
	}
	return 0;
}

static void io_write(u32 p, u32 v, int size)
{
	u32 off = p - 0x1f801000;
#ifdef PSX_TRACE_IO
	if (trace_n++ < 3000 && p != 0x1f801070 && p != 0x1f801074) printf("W%d %08x = %08x pc=%08x\n", size, p, v, cpu.cur_pc);
#endif
	if (off < 0x24) { memctrl[off >> 2] = v; return; }
	if (off >= 0x40 && off < 0x50) { sio_write(off - 0x40, v, size); return; }
	if (off >= 0x50 && off < 0x60) return;
	if (off == 0x60) { ram_size = v; return; }
	if (off == 0x70) { irq_stat &= v; irq_update(); return; }
	if (off == 0x74) { irq_mask = v & 0x7ff; irq_update(); return; }
	if (off >= 0x80 && off < 0x100) {
		if (size < 4) {
			// Escritura parcial: se mezcla con el valor actual. En DICR, los bits de «acuse» (24-30) de los
			// bytes no escritos no deben tocarse (escribir 1 ahí borraría las banderas).
			u32 r = (off - 0x80) & ~3, sh = (off & 3) * 8, m = (size == 1 ? 0xffu : 0xffffu) << sh;
			u32 nv = (dma_read(r) & ~m) | ((v << sh) & m);
			if (r == 0x74) nv &= ~(0x7f000000 & ~m);
			dma_write(r, nv);
		} else dma_write(off - 0x80, v);
		return;
	}
	if (off >= 0x100 && off < 0x130) { timer_write(off - 0x100, v); return; }
	if (off >= 0x800 && off < 0x804) { cdrom_write(off - 0x800, v & 0xff); return; }
	if (off == 0x810) { gpu_write_gp0(v); return; }
	if (off == 0x814) { gpu_write_gp1(v); return; }
	if (off == 0x820) { mdec_write_cmd(v); return; }
	if (off == 0x824) { mdec_write_ctrl(v); return; }
	if (off >= 0xc00 && off < 0xe00) {
		spu_write16(off - 0xc00, v & 0xffff);
		if (size == 4) spu_write16(off - 0xc00 + 2, v >> 16);
		return;
	}
}

// ---- accesos de la CPU ----
u32 bus_read32(u32 a)
{
	u32 p = phys(a);
	if (p < 0x800000) return *(u32 *)(psx_ram + (p & 0x1ffffc));
	if (p >= 0x1fc00000 && p < 0x1fc80000) return *(u32 *)(psx_bios + (p & 0x7fffc));
	if ((p & 0xfffffc00) == 0x1f800000) return *(u32 *)(psx_scratch + (p & 0x3fc));
	if (p >= 0x1f801000 && p < 0x1f803000) return io_read(p, 4);
	if (p >= 0x1f000000 && p < 0x1f800000) return 0xffffffff;
	return 0;
}

u32 bus_read16(u32 a)
{
	u32 p = phys(a);
	if (p < 0x800000) return *(u16 *)(psx_ram + (p & 0x1ffffe));
	if (p >= 0x1fc00000 && p < 0x1fc80000) return *(u16 *)(psx_bios + (p & 0x7fffe));
	if ((p & 0xfffffc00) == 0x1f800000) return *(u16 *)(psx_scratch + (p & 0x3fe));
	if (p >= 0x1f801000 && p < 0x1f803000) {
		if (p >= 0x1f801c00 && p < 0x1f801e00) return spu_read16(p - 0x1f801c00);
		if (p >= 0x1f801800 && p < 0x1f801804) return io_read(p, 2) & 0xffff;
		if (p >= 0x1f801040 && p < 0x1f801050) return sio_read(p - 0x1f801040, 2) & 0xffff;
		if (p >= 0x1f801100 && p < 0x1f801130) return timer_read(p - 0x1f801100) & 0xffff;
		return (io_read(p & ~3, 2) >> ((p & 2) * 8)) & 0xffff;
	}
	if (p >= 0x1f000000 && p < 0x1f800000) return 0xffff;
	return 0;
}

u32 bus_read8(u32 a)
{
	u32 p = phys(a);
	if (p < 0x800000) return psx_ram[p & 0x1fffff];
	if (p >= 0x1fc00000 && p < 0x1fc80000) return psx_bios[p & 0x7ffff];
	if ((p & 0xfffffc00) == 0x1f800000) return psx_scratch[p & 0x3ff];
	if (p >= 0x1f801000 && p < 0x1f803000) {
		if (p >= 0x1f801800 && p < 0x1f801804) return cdrom_read(p - 0x1f801800);
		if (p >= 0x1f801c00 && p < 0x1f801e00) return (spu_read16((p - 0x1f801c00) & ~1) >> ((p & 1) * 8)) & 0xff;
		if (p >= 0x1f801040 && p < 0x1f801050) return sio_read(p - 0x1f801040, 1) & 0xff;
		return (io_read(p & ~3, 1) >> ((p & 3) * 8)) & 0xff;
	}
	if (p >= 0x1f000000 && p < 0x1f800000) return 0xff;
	return 0;
}

void bus_write32(u32 a, u32 v)
{
	u32 p = phys(a);
	if (p < 0x800000) { *(u32 *)(psx_ram + (p & 0x1ffffc)) = v; DYN_CHECK_WRITE(p); return; }
	if ((p & 0xfffffc00) == 0x1f800000) { *(u32 *)(psx_scratch + (p & 0x3fc)) = v; return; }
	if (p >= 0x1f801000 && p < 0x1f803000) io_write(p, v, 4);
}

void bus_write16(u32 a, u32 v)
{
	u32 p = phys(a);
	v &= 0xffff;
	if (p < 0x800000) { *(u16 *)(psx_ram + (p & 0x1ffffe)) = (u16)v; DYN_CHECK_WRITE(p); return; }
	if ((p & 0xfffffc00) == 0x1f800000) { *(u16 *)(psx_scratch + (p & 0x3fe)) = (u16)v; return; }
	if (p >= 0x1f801000 && p < 0x1f803000) {
		if (p >= 0x1f801c00 && p < 0x1f801e00) { spu_write16(p - 0x1f801c00, v); return; }
		if (p >= 0x1f801040 && p < 0x1f801050) { sio_write(p - 0x1f801040, v, 2); return; }
		if (p >= 0x1f801100 && p < 0x1f801130) { timer_write(p - 0x1f801100, v); return; }
		if (p == 0x1f801070 || p == 0x1f801074) { io_write(p, v | (p == 0x1f801070 ? 0xffff0000 : 0), 2); return; }
		io_write(p, v, 2);
	}
}

void bus_write8(u32 a, u32 v)
{
	u32 p = phys(a);
	v &= 0xff;
	if (p < 0x800000) { psx_ram[p & 0x1fffff] = (u8)v; DYN_CHECK_WRITE(p); return; }
	if ((p & 0xfffffc00) == 0x1f800000) { psx_scratch[p & 0x3ff] = (u8)v; return; }
	if (p >= 0x1f801000 && p < 0x1f803000) {
		if (p >= 0x1f801800 && p < 0x1f801804) { cdrom_write(p - 0x1f801800, v); return; }
		if (p >= 0x1f801040 && p < 0x1f801050) { sio_write(p - 0x1f801040, v, 1); return; }
		if (p >= 0x1f802000) return;   // puerto de depuración / POST
		io_write(p, v, 1);
	}
}

void bus_reset(void)
{
	memset(memctrl, 0, sizeof memctrl);
	ram_size = 0xb88;
	irq_stat = irq_mask = 0;
}

void bus_state(PsxState *s)
{
	psx_state_io(s, psx_ram, 2 * 1024 * 1024);
	STATE(s, psx_scratch);
	STATE(s, irq_stat);
	STATE(s, irq_mask);
	STATE(s, memctrl);
	STATE(s, ram_size);
}
