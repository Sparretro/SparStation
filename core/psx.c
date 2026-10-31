// Sistema: reserva de memoria, reinicio, bucle de un frame y carga directa de ejecutables.
#include "psx.h"
#include <stdlib.h>

PSX_FAST u32 psx_cycles;
PSX_FAST u32 psx_run_until;
PSX_FAST u16 psx_cpu_scale = 256;
void (*psx_line_cb)(void);
u16 *psx_vram;
#ifdef PSX_PROFILE
PSX_FAST PsxStats psx_stat;
#endif

void bus_reset(void);

static const u8 *pending_exe;
static u32 pending_len;

int psx_init(void)
{
	psx_ram = malloc(2 * 1024 * 1024);
	if (!psx_vram) psx_vram = malloc(1024 * 512 * 2);   // el banco de pruebas puede ponerla antes en el Expansion Pak
	if (!psx_ram || !psx_vram) return 0;
	return 1;
}

void psx_reset(void)
{
	memset(psx_ram, 0, 2 * 1024 * 1024);
	memset(psx_scratch, 0, sizeof psx_scratch);
	memset(psx_vram, 0, 1024 * 512 * 2);
	bus_reset();
	cpu_reset();
	gte_reset();
	gpu_reset();
	timers_reset();
	cdrom_reset();
	sio_reset();
	spu_reset();
	mdec_reset();
	psx_cycles = 0;
	psx_hook_pc = pending_exe ? 0x80030000 : 0;
}

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static int (*pending_read)(void *user, u8 *dst, u32 len);
static void *pending_user;

int psx_sideload_exe(const u8 *exe, u32 len)
{
	if (len < 0x800 || memcmp(exe, "PS-X EXE", 8) != 0) return 0;
	pending_exe = exe;
	pending_len = len;
	pending_read = NULL;
	psx_hook_pc = 0x80030000;
	return 1;
}

// Igual, pero solo con la cabecera en memoria: el cuerpo se lee con read() directamente en la RAM
int psx_sideload_exe_reader(const u8 *header, int (*read)(void *user, u8 *dst, u32 len), void *user)
{
	if (!psx_sideload_exe(header, 0x800)) return 0;
	pending_read = read;
	pending_user = user;
	return 1;
}

// La BIOS salta al shell en 0x80030000 cuando ya ha montado el kernel: justo ahí cambiamos el
// shell por el ejecutable, igual que hacen los emuladores de PC con el «fast boot».
void psx_hook(void)
{
	const u8 *e = pending_exe;
	u32 pc0, gp0, dst, size, sp_base, sp_size;
	psx_hook_pc = 0;
	if (!e) return;
	pc0 = rd32(e + 0x10);
	gp0 = rd32(e + 0x14);
	dst = rd32(e + 0x18);
	size = rd32(e + 0x1c);
	sp_base = rd32(e + 0x30);
	sp_size = rd32(e + 0x34);
	if (pending_read) {
		u32 off = dst & 0x1fffff;
		if (size > 0x200000 - off) size = 0x200000 - off;
		pending_read(pending_user, psx_ram + off, size);
#ifdef PSX_DYNAREC
		dyn_ram_written(off, size);
#endif
	} else {
		if (size > pending_len - 0x800) size = pending_len - 0x800;
		for (u32 i = 0; i < size; i++) { psx_ram[(dst + i) & 0x1fffff] = e[0x800 + i]; DYN_CHECK_WRITE(dst + i); }
	}
	cpu.pc = pc0;
	cpu.next_pc = pc0 + 4;
	cpu.r[28] = gp0;
	if (sp_base) {
		cpu.r[29] = sp_base + sp_size;
		cpu.r[30] = sp_base + sp_size;
	}
	pending_exe = NULL;
}

static void run_until(u32 end)
{
	while (TIME_BEFORE(psx_cycles, end)) {
		u32 t = end, w, start = psx_cycles;
		if (cdrom_next_event(&w) && TIME_BEFORE(w, t)) t = w;
		if (sio_next_event(&w) && TIME_BEFORE(w, t)) t = w;
		if (!TIME_BEFORE(psx_cycles, t)) t = psx_cycles + 1;
#ifdef PSX_DYNAREC
		dyn_run(t);   // el tiempo de CPU se mide una vez por frame (psx_run_frame): por línea costaba ~2%
#else
		cpu_run(t);
#endif
		timers_advance(psx_cycles - start);
		cdrom_tick();
		sio_tick();
	}
}

void psx_run_frame(void)
{
	u32 line_start = psx_cycles;
#ifdef PSX_PROFILE
	u32 disc0 = psx_prof[PROF_DISC];
	PROF_BEGIN();
#endif
	gpu_disp.frame_done = 0;
	for (int line = 0; line < PSX_LINES; line++) {
		gpu_set_line(line);
		run_until(line_start + PSX_LINE_CYCLES);
		line_start += PSX_LINE_CYCLES;
		spu_advance(PSX_LINE_CYCLES);
		timers_hblank();
		if (psx_line_cb) psx_line_cb();
		if (line == PSX_VBLANK_LINE - 1) {
			psx_irq(PSX_IRQ_VBLANK);
			gpu_vblank();
		}
	}
#ifdef PSX_PROFILE
	PROF_END(PROF_CPU);
	psx_prof[PROF_CPU] -= psx_prof[PROF_DISC] - disc0;   // el disco va fuera de la CPU
#endif
}


// ---- estados guardados ----
#define STATE_MAGIC 0x54535053u   // "SPST"
#define STATE_VERSION 1

void psx_state_io(PsxState *s, void *p, u32 len)
{
	if (s->ok && !s->io(s->user, p, len)) s->ok = 0;
}

static int state_all(PsxState *s)
{
	u32 magic = STATE_MAGIC, ver = STATE_VERSION;
	STATE(s, magic);
	STATE(s, ver);
	if (!s->ok || magic != STATE_MAGIC || ver != STATE_VERSION) return 0;
	STATE(s, psx_cycles);
	bus_state(s);
	cpu_state(s);
	dma_state(s);
	gpu_state(s);
	gte_state(s);
	mdec_state(s);
	cdrom_state(s);
	sio_state(s);
	timers_state(s);
	spu_state(s);
	return s->ok;
}

int psx_state_save(int (*write)(void *user, void *p, u32 len), void *user)
{
	PsxState s = { write, user, 0, 1 };
	return state_all(&s);
}

int psx_state_load(int (*read)(void *user, void *p, u32 len), void *user)
{
	PsxState s = { read, user, 1, 1 };
	int ok = state_all(&s);
	psx_hook_pc = 0;
	pending_exe = NULL;
#ifdef PSX_DYNAREC
	dyn_reset();
#endif
	return ok;
}
