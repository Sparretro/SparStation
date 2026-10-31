// Dynarec: traduce bloques de código MIPS de la PS1 a código ARM de la DS la primera vez que se
// ejecutan.
//
// Registros: dentro de cada bloque, los registros MIPS más usados viven en registros ARM (r4-r8):
// se cargan de la estructura cpu la primera vez que se leen y se guardan al salir del bloque o
// antes de lo que necesite verlos en memoria (el intérprete, las excepciones). Los valores que se
// conocen al traducir (lui, addiu sobre constantes, la dirección de vuelta de jal...) ni siquiera se
// calculan: se escriben directamente donde hagan falta. Lo raro (COP0, LWL/SWR) lo hace el intérprete;
// la GTE se llama directamente.
//
// Los bloques se encadenan: al final cada uno suma sus ciclos a psx_cycles y, si aún no se ha
// llegado a psx_run_until, salta directamente al siguiente (el salto se parchea la primera vez) o,
// en los saltos a registro, lo busca en la tabla con un despachador en ARM. Solo se vuelve a C al
// acabarse el tiempo (los dispositivos y las interrupciones lo adelantan), en excepciones y en lo raro.
//
// Registros ARM fijos: r11 = &cpu (y psx_cycles, psx_run_until y la scratchpad cerca, en la DTCM),
// r10 = RAM de la PS1, r9 = dyn_pages, r12 = psx_cycles - psx_run_until (negativo mientras quede
// tiempo: el final de un bloque es un solo ADDS). r0-r3 = temporales. Las llamadas a C pasan por
// trampolines que ponen psx_cycles en memoria antes y recalculan r12 después.
//
// Densidad: en la DSi el código traducido se ejecuta desde la RAM principal y la caché de instrucciones
// es de 8 KB, así que cada palabra cuenta. Lo que casi nunca se ejecuta (salidas por fin de tiempo,
// excepciones) va a una zona fría aparte, al final de la caché.
#ifdef PSX_DYNAREC
#include "psx.h"
#include "arm_emit.h"
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ARM9
#include <nds.h>
#include <unistd.h>
#endif

#define MAX_BLOCK 64
#ifndef DYN_DS_SLOTS
#define DYN_DS_SLOTS 0x2000   // huecos de la tabla de bloques en el modo DS (potencia de 2; el banco de pruebas usa más)
#endif
#ifndef DYN_DS_RESERVE_KB
#define DYN_DS_RESERVE_KB 704   // modo DS: memoria que se deja fuera de la caché de código (los vídeos necesitan ~500 KB)
#endif

typedef u32 *block_fn;   // primera instrucción del bloque; la palabra anterior es el PC traducido

u32 *emit_ptr;
u8 dyn_pages[DYN_NPAGES];
static u32 *code_buf, *code_end, *code_start, code_size;
static u32 *cold_base, *cold_ptr, *hot_save;   // zona fría: [cold_base, code_end)
// Bloques en orden de dirección ARM (para el perfilador): inicio y PC MIPS; se vacía con la caché
static u32 *blk_arm, *blk_mips, blk_n, blk_max;
// Tabla hash de bloques: la entrada apunta al código y la palabra anterior guarda el PC traducido
static block_fn *slots;
static u32 slot_mask;

// Código fijo al principio de la caché: entrar desde C, salir a C, enlazar y despachar
static void (*dyn_enter)(Cpu *c, u8 *ram, u8 *pages, block_fn body);
static u32 *stub_exit, *stub_link, *stub_dispatch, *stub_exit_pc, *stub_sp_fail, *stub_scr_fail;
static u32 gen;              // sube en cada vaciado: un enlace pendiente de una caché vieja no se parchea
u8 dyn_chain;                // 0 = volver a C en cada bloque (psx_hook_pc); 1 = salvo vectores A0/B0/C0 (psx_tty)
PSX_FAST static u32 dyn_tmp; // condición o destino de salto guardado cuando el delay slot pisa sus registros

// Enlaces parcheados, por página de 4 KB del destino, para deshacerlos si el juego reescribe ese código
typedef struct { u32 *site; u32 lin; s32 next; } Link;
static Link *links;
static s32 link_head[DYN_NPAGES], link_n, link_max;

#define RCPU R11
#define RRAM R10
#define RPAGES R9
#define RCYC R12
#define OFF_R(n) ((int)offsetof(Cpu, r) + (n) * 4)
#define OFF_HI ((int)offsetof(Cpu, hi))
#define OFF_LO ((int)offsetof(Cpu, lo))
#define OFF_PC ((int)offsetof(Cpu, pc))
static int off_cyc, off_until, off_tmp, off_scr;   // respecto a &cpu (todo en la DTCM)

static void sync_code(void *p, u32 len)
{
#ifdef ARM9
	DC_FlushRange(p, len);
	IC_InvalidateRange(p, len);
#else
	(void)p; (void)len;
#endif
}

static u32 b_enc(int cond, int link, const u32 *at, const void *target)
{
	s32 off = (s32)((const u8 *)target - ((const u8 *)at + 8)) >> 2;
	return ((u32)cond << 28) | (5u << 25) | ((u32)link << 24) | ((u32)off & 0xffffff);
}

static u32 *fwd(int cond)
{
	u32 *p = emit_ptr;
	emit((u32)cond << 28);
	return p;
}

static void land(u32 *p)
{
	s32 off = (s32)((u8 *)emit_ptr - ((u8 *)p + 8)) >> 2;
	*p = (*p & 0xf0000000) | (5u << 25) | ((u32)off & 0xffffff);
}

// Emitir en la zona fría (begin_cold ... end_cold); fwd/land funcionan igual entre las dos zonas
static void begin_cold(void) { hot_save = emit_ptr; emit_ptr = cold_ptr; }
static void end_cold(void) { cold_ptr = emit_ptr; emit_ptr = hot_save; }

static void emit_handlers(void);

// r12 <-> psx_cycles alrededor de las llamadas a C (usan r2 y r3; r0 y r1 son los argumentos o el resultado)
static void cycles_out(void)
{
	a_ldr(R3, RCPU, off_until);
	a_dp_reg(C_AL, OP_ADD, 0, R2, RCYC, R3, SH_LSL, 0);
	a_str(R2, RCPU, off_cyc);
}

static void cycles_in(void)
{
	a_ldr(R2, RCPU, off_cyc);
	a_ldr(R3, RCPU, off_until);
	a_dp_reg(C_AL, OP_SUB, 0, RCYC, R2, R3, SH_LSL, 0);
}

// Llamada a C desde una rutina compartida a la que se llegó con BL (lr guardado alrededor)
static void call_c_inline(const void *fn)
{
	a_push((1u << R12) | (1u << LR));   // r12 solo para que la pila siga alineada a 8
	cycles_out();
	a_b(C_AL, 1, fn);
	cycles_in();
	a_pop((1u << R3) | (1u << LR));
}

// Trampolines: el código traducido llama a C con un solo BL
static u32 h_r32(u32 a), h_r16s(u32 a), h_r16u(u32 a), h_r8s(u32 a), h_r8u(u32 a);
static void h_w32(u32 a, u32 v), h_w16(u32 a, u32 v), h_w8(u32 a, u32 v);
static void h_div(s32 n, s32 d), h_divu(u32 n, u32 d);
static const void *const tramp_fn[] = {
	(const void *)cpu_dyn_interp, (const void *)cpu_dyn_exception, (const void *)gte_command, (const void *)gte_read,
	(const void *)gte_write, (const void *)h_div, (const void *)h_divu, (const void *)dyn_smc,
	(const void *)h_r32, (const void *)h_r16s, (const void *)h_r16u, (const void *)h_r8s, (const void *)h_r8u,
	(const void *)h_w32, (const void *)h_w16, (const void *)h_w8,
};
#define NTRAMP (int)(sizeof tramp_fn / sizeof tramp_fn[0])
static u32 *tramp_at[NTRAMP];

static const void *tramp(const void *fn)
{
	for (int i = 0; i < NTRAMP; i++)
		if (tramp_fn[i] == fn) return tramp_at[i];
	return NULL;   // no pasa: todas las funciones llamadas desde el código están en la lista
}

static void emit_stubs(void)
{
	u32 *p;
	emit_ptr = code_buf;
	// entrar: dyn_enter(cpu, ram, pages, cuerpo)
	dyn_enter = (void (*)(Cpu *, u8 *, u8 *, block_fn))emit_ptr;
	a_push(0x4ff8);   // r3-r11, lr (10 registros: la pila queda alineada a 8)
	a_mov(RCPU, R0);
	a_mov(RRAM, R1);
	a_mov(RPAGES, R2);
	a_ldr(R0, RCPU, off_cyc);   // r3 es el cuerpo: aquí no vale cycles_in
	a_ldr(R1, RCPU, off_until);
	a_dp_reg(C_AL, OP_SUB, 0, RCYC, R0, R1, SH_LSL, 0);
	a_bx(R3);
	// salir a C (cpu.pc ya está guardado)
	stub_exit = emit_ptr;
	a_ldr(R1, RCPU, off_until);
	a_dp_reg(C_AL, OP_ADD, 0, R0, RCYC, R1, SH_LSL, 0);
	a_str(R0, RCPU, off_cyc);
	a_pop(0x8ff8);
	for (int i = 0; i < NTRAMP; i++) {
		tramp_at[i] = emit_ptr;
		a_push((1u << R12) | (1u << LR));
		cycles_out();
		a_b(C_AL, 1, tramp_fn[i]);
		cycles_in();
		a_pop((1u << R2) | (1u << PC));
	}
	// salir a C con cpu.pc = la palabra que sigue al BL que llega aquí
	stub_exit_pc = emit_ptr;
	a_ldr(R0, LR, 0);
	a_str(R0, RCPU, OFF_PC);
	a_b(C_AL, 0, stub_exit);
	// sp no apunta a la RAM al entrar en un bloque con el atajo de la pila: apuntarlo y salir (se retraduce)
	stub_sp_fail = emit_ptr;
	a_ldr(R0, LR, 0);
	a_str(R0, RCPU, OFF_PC);
	a_push((1u << R3) | (1u << R12));
	{ extern void dyn_sp_fail(u32 pc); a_b(C_AL, 1, (const void *)dyn_sp_fail); }
	a_pop((1u << R3) | (1u << R12));
	a_b(C_AL, 0, stub_exit);
	// igual con la base de la scratchpad (ver scr_reg)
	stub_scr_fail = emit_ptr;
	a_ldr(R0, LR, 0);
	a_str(R0, RCPU, OFF_PC);
	a_push((1u << R3) | (1u << R12));
	{ extern void dyn_scr_fail(u32 pc); a_b(C_AL, 1, (const void *)dyn_scr_fail); }
	a_pop((1u << R3) | (1u << R12));
	a_b(C_AL, 0, stub_exit);
	// enlazar: se llega con BL desde el final de un bloque; la palabra siguiente al BL es el PC destino
	stub_link = emit_ptr;
	a_ldr(R0, LR, 0);
	a_dp_imm(C_AL, OP_SUB, 0, R1, LR, 4);
	a_push((1u << R3) | (1u << R12));   // traducir no toca el tiempo: r12 sigue valiendo
	{ extern block_fn dyn_link(u32 target, u32 *site); a_b(C_AL, 1, (const void *)dyn_link); }
	a_pop((1u << R3) | (1u << R12));
	a_dp_imm(C_AL, OP_CMP, 1, 0, R0, 0);
	a_b(C_EQ, 0, stub_exit);
	a_bx(R0);
	// despachar: r0 = PC destino (ya guardado en cpu.pc); RAM y ROM de la BIOS. Los vectores A0/B0/C0
	// solo con dyn_chain == 2 (con 1, C tiene que ver las llamadas para sacar el texto de la BIOS)
	stub_dispatch = emit_ptr;
	a_movi(R1, (u32)(uintptr_t)&dyn_chain);
	a_mem_imm(C_AL, 1, 1, R3, R1, 0);
	a_dp_imm(C_AL, OP_CMP, 1, 0, R3, 0);
	a_b(C_EQ, 0, stub_exit);
	a_dp_imm(C_AL, OP_BIC, 0, R1, R0, arm_imm(0xe0000000));
	a_dp_imm(C_AL, OP_CMP, 1, 0, R1, arm_imm(0x800000));
	{
		u32 *ram = fwd(C_CC), *lookup;
		a_addi(R2, R1, (u32)-0x1fc00000, R2);   // BIOS: 1FC00000..1FC7FFFF
		a_dp_imm(C_AL, OP_CMP, 1, 0, R2, arm_imm(0x80000));
		a_b(C_CS, 0, stub_exit);
		lookup = fwd(C_AL);
		land(ram);
		a_dp_imm(C_AL, OP_CMP, 1, 0, R1, arm_imm(0x100));
		a_dp_imm(C_CC, OP_CMP, 1, 0, R3, 2);
		a_b(C_CC, 0, stub_exit);   // vector bajo y dyn_chain < 2
		land(lookup);
	}
	// el hueco de la tabla es (dirección lineal >> 2) & slot_mask, que para RAM y BIOS sale de los mismos bits
	a_movi(R2, (slot_mask << 2) & 0x1ffffc);   // RAM: los 2 MB (y sus espejos); BIOS: su desplazamiento
	a_dp_reg(C_AL, OP_AND, 0, R1, R1, R2, SH_LSL, 0);
	a_movi(R2, (u32)(uintptr_t)slots);
	a_mem_reg(C_AL, 1, 0, R2, R2, R1);
	a_dp_imm(C_AL, OP_CMP, 1, 0, R2, 0);
	a_b(C_EQ, 0, stub_exit);
	a_ldr(R3, R2, -4);
	a_dp_reg(C_AL, OP_CMP, 1, 0, R3, R0, SH_LSL, 0);
	a_b(C_NE, 0, stub_exit);
	a_bx(R2);
	emit_handlers();
	code_start = emit_ptr;
	cold_base = cold_ptr = code_start + (code_end - code_start) * 3 / 4;
	p = code_buf;
	sync_code(p, (u32)((u8 *)emit_ptr - (u8 *)p));
}

static int near_cpu(const void *p, int extra, int *off)
{
	*off = (int)((const u8 *)p - (const u8 *)&cpu);
	return *off >= -4095 && *off + extra <= 4095;
}

// big: DSi (16 MB) con espacio de sobra; si no, tamaños para una DS de 4 MB
int dyn_init(int big)
{
	u32 nslots = big ? 0x80000 : DYN_DS_SLOTS;   // DSi: un hueco por instrucción de los 2 MB (si dos bloques chocan, se retraducen sin parar)
	link_max = big ? 65536 : 4096;
#ifdef DYN_SLOTS_AT
	slots = (block_fn *)DYN_SLOTS_AT;   // banco de pruebas: tabla grande en el Expansion Pak
	memset(slots, 0, nslots * sizeof(block_fn));
#else
	slots = calloc(nslots, sizeof(block_fn));
	while (!slots && nslots > 0x2000) slots = calloc(nslots >>= 1, sizeof(block_fn));   // sin sitio: más pequeña
#endif
	links = malloc(link_max * sizeof(Link));
	if (big) {
		code_size = 3 * 1024 * 1024;
		code_buf = malloc(code_size);
		while (!code_buf && code_size > 1024 * 1024) code_buf = malloc(code_size -= 512 * 1024);
		blk_max = 65536;
		blk_arm = malloc(blk_max * 4);
		blk_mips = malloc(blk_max * 4);
		if (!blk_arm || !blk_mips) blk_max = 0;
	} else {   // en modo DS la memoria va muy justa: lo que quede del montón, dejando 704 KB para lo demás
		code_size = 64 * 1024;
#ifdef ARM9
		{
			extern char *fake_heap_end;
			s32 room = (s32)(fake_heap_end - (char *)sbrk(0)) - DYN_DS_RESERVE_KB * 1024;   // lo demás: búfers del MDEC (hasta ~500 KB), el 3D, CD...
			if (room > (s32)code_size) code_size = (u32)room & ~0x7fffu;
		}
#endif
		code_buf = malloc(code_size);
	}
#ifdef PSX_BLKTAB
	if (!big) {   // perfilador en el modo DS: tabla de bloques pequeña
		blk_max = 8192;
		blk_arm = malloc(blk_max * 4);
		blk_mips = malloc(blk_max * 4);
		if (!blk_arm || !blk_mips) blk_max = 0;
	}
#endif
	if (!code_buf || !slots || !links) return 0;
	slot_mask = nslots - 1;
	code_end = code_buf + code_size / 4;
	if (!near_cpu(&psx_cycles, 0, &off_cyc) || !near_cpu(&psx_run_until, 0, &off_until) ||
	    !near_cpu(&dyn_tmp, 0, &off_tmp) || !near_cpu(psx_scratch, 1023, &off_scr))
		return 0;
	memset(link_head, 0xff, sizeof link_head);
	emit_stubs();
	return 1;
}

u32 *dyn_code_base(void) { return code_buf; }


void dyn_cache_use(u32 *used, u32 *size)
{
	*used = (u32)(emit_ptr - code_buf + cold_ptr - cold_base) * 4;
	*size = code_size;
}

static void flush_all(void)
{
	STAT_ADD(dyn_flushes, 1);
	memset(slots, 0, (slot_mask + 1) * sizeof(block_fn));
	memset(dyn_pages, 0, sizeof dyn_pages);
	memset(link_head, 0xff, sizeof link_head);
	link_n = 0;
	gen++;
	emit_ptr = code_start;
	cold_ptr = cold_base;
	blk_n = 0;
}

void dyn_reset(void) { flush_all(); }

// Dirección física «lineal»: RAM en 0..2 MB, BIOS a partir de 2 MB; ~0 si no es ninguna
static u32 lin(u32 pc)
{
	u32 p = pc & 0x1fffffff;
	if (p < 0x800000) return p & 0x1ffffc;
	if (p >= 0x1fc00000 && p < 0x1fc80000) return 0x200000 + (p & 0x7fffc);
	return ~0u;
}

static block_fn *slot(u32 pc)
{
	u32 l = lin(pc);
	if (l == ~0u) return NULL;
	return &slots[(l >> 2) & slot_mask];
}

// Devuelve al enlazador los saltos parcheados hacia bloques de la página que empiecen en [first, last)
static void unlink_page(u32 page, u32 first, u32 last)
{
	s32 *pp = &link_head[page];
	while (*pp >= 0) {
		Link *l = &links[*pp];
		if (l->lin >= first && l->lin < last) {
			*l->site = b_enc(C_AL, 1, l->site, stub_link);
			sync_code(l->site, 4);
			*pp = l->next;
		} else pp = &l->next;
	}
}

void dyn_smc(u32 off)
{
	u32 page = (off >> DYN_PAGE_BITS) & (DYN_NPAGES - 1);
	if (!dyn_pages[page]) return;
	dyn_pages[page] = 0;
	STAT_ADD(dyn_smc, 1);
	// la página entera y el final de la anterior (bloques que la cruzan)
	{
		u32 first = page << DYN_PAGE_BITS, last = first + (1u << DYN_PAGE_BITS);
		if (page) first -= MAX_BLOCK * 4;
		for (u32 a = first; a < last; a += 4) {
			block_fn *s = &slots[(a >> 2) & slot_mask];
			if (*s && lin((*s)[-1]) == a) *s = NULL;
		}
		unlink_page(page, page << DYN_PAGE_BITS, last);
		if (page) unlink_page(page - 1, first, page << DYN_PAGE_BITS);
	}
}

void dyn_ram_written(u32 off, u32 len)
{
	for (u32 a = off & ~((1u << DYN_PAGE_BITS) - 1); a < off + len; a += 1u << DYN_PAGE_BITS) DYN_CHECK_WRITE(a);
}

// ---- servicios llamados desde el código traducido ----
static u32 h_r32(u32 a) { STAT_ADD(io_calls, 1); return bus_read32(a); }
static u32 h_r16u(u32 a) { STAT_ADD(io_calls, 1); return bus_read16(a); }
static u32 h_r16s(u32 a) { STAT_ADD(io_calls, 1); return (u32)(s32)(s16)bus_read16(a); }
static u32 h_r8u(u32 a) { STAT_ADD(io_calls, 1); return bus_read8(a); }
static u32 h_r8s(u32 a) { STAT_ADD(io_calls, 1); return (u32)(s32)(s8)bus_read8(a); }
static void h_w32(u32 a, u32 v) { STAT_ADD(io_calls, 1); bus_write32(a, v); }
static void h_w16(u32 a, u32 v) { STAT_ADD(io_calls, 1); bus_write16(a, v); }
static void h_w8(u32 a, u32 v) { STAT_ADD(io_calls, 1); bus_write8(a, v); }

static void h_div(s32 n, s32 d)
{
	if (d == 0) { cpu.hi = (u32)n; cpu.lo = n >= 0 ? 0xffffffff : 1; }
	else if ((u32)n == 0x80000000 && d == -1) { cpu.hi = 0; cpu.lo = 0x80000000; }
	else { cpu.lo = (u32)(n / d); cpu.hi = (u32)(n % d); }
}

static void h_divu(u32 n, u32 d)
{
	if (d == 0) { cpu.hi = n; cpu.lo = 0xffffffff; }
	else { cpu.lo = n / d; cpu.hi = n % d; }
}

// ---- registros MIPS durante la traducción ----
// host: registro ARM asignado (o -1); valid: el registro ARM tiene el valor; stale: la memoria (cpu.r)
// no lo tiene; isconst: el valor se conoce al traducir (k)
static struct { s8 host; u8 valid, stale, isconst; u32 k; } rg[32];
static const int host_pool[5] = { R4, R5, R6, R7, R8 };

// Pila: si un bloque accede a memoria con sp, se comprueba una vez al entrar que sp apunta a la RAM y
// r8 guarda su dirección real en la DS; así «lw s0, 20(sp)» es una sola instrucción ARM. Si no apunta
// a la RAM (pila en la scratchpad, por ejemplo), ese bloque se vuelve a traducir sin el atajo.
// Las escrituras por sp no miran si había código en esa página (nadie ejecuta código de la pila).
#define RHSP R8
static int use_hsp, hsp_ok;            // el bloque usa el atajo; r8 vale ahora (sp no ha cambiado de otra forma)
static u32 *sp_fail[2];               // saltos de la comprobación de entrada que fallan
static u32 sp_unsafe[256];             // PC de bloques donde falló (tabla abierta)
static int sp_unsafe_n;

// Scratchpad: si al traducir un registro vale una dirección fija de la scratchpad (SH: s0 = 1F800000) y el
// bloque lo usa de base al menos dos veces, se comprueba una vez al entrar que sigue valiendo eso; entonces
// «lbu a0, 912(s0)» es un solo LDRB sobre la DTCM. Si al entrar no vale eso, se retraduce sin el atajo.
static int scr_reg, scr_ok;            // registro MIPS (0 = ninguno) y si aún vale scr_val en este punto
static u32 scr_val, *scr_fail;
static u32 scr_unsafe[256];
static int scr_unsafe_n;

static int pc_in(const u32 *tab, u32 pc)
{
	for (u32 i = (pc >> 2) & 255, k = 0; k < 256 && tab[i]; i = (i + 1) & 255, k++)
		if (tab[i] == pc) return 1;
	return 0;
}

static int sp_is_unsafe(u32 pc)
{
	for (u32 i = (pc >> 2) & 255, k = 0; k < 256 && sp_unsafe[i]; i = (i + 1) & 255, k++)
		if (sp_unsafe[i] == pc) return 1;
	return sp_unsafe_n >= 200;   // tabla casi llena: este juego no usa la pila en la RAM
}

// Llamada a C por su trampolín (r4-r8 los conserva C; r0-r3 se pierden)
static void call(const void *fn) { a_b(C_AL, 1, tramp(fn)); }

// Valor del registro MIPS mr en un registro ARM (el asignado o tmp)
static int get(int mr, int tmp)
{
	if (mr == 0) { a_movi(tmp, 0); return tmp; }
	if (rg[mr].host >= 0) {
		int h = rg[mr].host;
		if (!rg[mr].valid) {
			if (rg[mr].isconst) a_movi(h, rg[mr].k);
			else a_ldr(h, RCPU, OFF_R(mr));
			rg[mr].valid = 1;
		}
		return h;
	}
	if (rg[mr].isconst) { a_movi(tmp, rg[mr].k); return tmp; }
	a_ldr(tmp, RCPU, OFF_R(mr));
	return tmp;
}

static int is_const(int mr) { return mr == 0 || rg[mr].isconst; }
static u32 const_of(int mr) { return mr ? rg[mr].k : 0; }

// Registro ARM donde calcular el nuevo valor de mr; después, put(mr, ese registro)
static int dst(int mr, int tmp) { return mr && rg[mr].host >= 0 ? rg[mr].host : tmp; }

static u32 stale_mask;   // registros con stale = 1

static void put(int mr, int r)
{
	if (!mr) return;
	if (mr == 29) hsp_ok = 0;
	if (mr == scr_reg) scr_ok = 0;
	rg[mr].isconst = 0;
	if (rg[mr].host >= 0) {
		rg[mr].valid = 1;
		rg[mr].stale = 1;
		stale_mask |= 1u << mr;
	} else {
		a_str(r, RCPU, OFF_R(mr));
		rg[mr].stale = 0;
		stale_mask &= ~(1u << mr);
	}
}

static void put_const(int mr, u32 k)
{
	if (!mr) return;
	if (mr == 29) hsp_ok = 0;
	if (mr == scr_reg) scr_ok = 0;
	rg[mr].isconst = 1;
	rg[mr].k = k;
	rg[mr].stale = 1;
	rg[mr].valid = 0;
	stale_mask |= 1u << mr;
}

// Pone en cpu.r todo lo que solo estaba en registros o era constante (usa r0)
static void flush_regs(void)
{
	while (stale_mask) {
		int i = __builtin_ctz(stale_mask);
		stale_mask &= stale_mask - 1;
		if (rg[i].valid) a_str(rg[i].host, RCPU, OFF_R(i));
		else {
			a_movi(R0, rg[i].k);
			a_str(R0, RCPU, OFF_R(i));
		}
		rg[i].stale = 0;
	}
}

// Tras una llamada que puede cambiar cpu.r: olvidar lo que ya no vale
static void forget(u32 written_mask)
{
	if (written_mask & (1u << 29)) hsp_ok = 0;
	if (written_mask & (1u << scr_reg) & ~1u) scr_ok = 0;
	written_mask &= ~1u;
	stale_mask &= ~written_mask;
	while (written_mask) {
		int i = __builtin_ctz(written_mask);
		written_mask &= written_mask - 1;
		rg[i].valid = rg[i].isconst = rg[i].stale = 0;
	}
}

// ---- utilidades de emisión ----
static void set_pc_const(u32 v)
{
	a_movi(R0, v);
	a_str(R0, RCPU, OFF_PC);
}

// Suma los ciclos de ninstr instrucciones al contador (r12); flags MI si aún queda tiempo
static void emit_cycles(int ninstr)
{
	u32 c = ((u32)ninstr * 2 * psx_cpu_scale) >> 8;   // <= 130: cabe como inmediato
	a_dp_imm(C_AL, OP_ADD, 1, RCYC, RCYC, arm_imm(c ? c : 1));
}

// Vuelve a C (cpu.pc ya puesto y los registros ya en memoria)
static void exit_block(int ninstr)
{
	emit_cycles(ninstr);
	a_b(C_AL, 0, stub_exit);
}

// Salto encadenable a un PC fijo: BL al enlazador y el PC detrás (se cambia por B al bloque)
static void link_site(u32 target)
{
	a_b(C_AL, 1, stub_link);
	emit(target);
}

// Vuelta a C con cpu.pc = target (BL a stub_exit_pc y el PC detrás)
static void exit_pc(u32 target)
{
	a_b(C_AL, 1, stub_exit_pc);
	emit(target);
}

// Fin de bloque hacia un PC conocido; si se acabó el tiempo, la salida está en la zona fría
static void exit_to(u32 target, int ninstr)
{
	u32 *out;
	flush_regs();
	emit_cycles(ninstr);
	out = fwd(C_PL);
	link_site(target);
	begin_cold();
	land(out);
	exit_pc(target);
	end_cold();
}

// ---- accesos a memoria ----
// El caso normal (RAM) va en línea con ejecución condicional; lo demás (scratchpad, E/S) lo hacen
// rutinas compartidas al principio de la caché, una por tamaño y registro, a las que se llega con BLNE.
enum { M_W, M_HS, M_HU, M_BS, M_BU };   // tamaño y signo
static u32 *ld_handler[5][16], *st_handler[5][16], *smc_handler;
static const int mem_regs[6] = { R2, R4, R5, R6, R7, R8 };   // destinos o valores posibles

// Carga o guarda rd en [rn + off] con la instrucción del tamaño pedido (usa r3 si off no cabe)
static void mem_off(int cond, int load, int size, int rd, int rn, int off)
{
	static const int hsh[5] = { 0, 3, 1, 2, 0 };
	int word = size == M_W || size == M_BU || (!load && size == M_BS), lim = word ? 4095 : 255;
	if ((off < -lim || off > lim) && off > 0 && arm_imm((u32)off & ~(u32)lim) >= 0) {   // base + parte alta, y el resto
		a_dp_imm(cond, OP_ADD, 0, R3, rn, arm_imm((u32)off & ~(u32)lim));
		rn = R3;
		off &= lim;
	}
	if (word) {
		if (off >= -4095 && off <= 4095) a_mem_imm(cond, load, size != M_W, rd, rn, off);
		else { a_movi(R3, (u32)off); a_mem_reg(cond, load, size != M_W, rd, rn, R3); }
	} else {
		int sh = load ? hsh[size] : 1;
		if (off >= -255 && off <= 255) a_memh_imm(cond, load, sh, rd, rn, off);
		else { a_movi(R3, (u32)off); a_memh_reg(cond, load, sh, rd, rn, R3); }
	}
}

static const void *const rd_fn[5] = { h_r32, h_r16s, h_r16u, h_r8s, h_r8u };
static const void *const wr_fn[5] = { h_w32, h_w16, h_w16, h_w8, h_w8 };

// r0 = dirección fuera de la RAM. Si es la scratchpad (1F800000-1F8003FF en cualquier segmento, está en
// la DTCM) la atiende y vuelve; si no, sigue en lo que se emita después (la llamada a C)
static void handler_scratch(int load, int size, int r)
{
	u32 *not_scratch;
	a_dp_imm(C_AL, OP_BIC, 0, R1, R0, arm_imm(0xe0000000));
	a_dp_imm(C_AL, OP_SUB, 0, R1, R1, arm_imm(0x1f800000));
	a_dp_imm(C_AL, OP_CMP, 1, 0, R1, arm_imm(0x400));
	not_scratch = fwd(C_CS);
	a_dp_reg(C_AL, OP_ADD, 0, R1, R1, RCPU, SH_LSL, 0);
	mem_off(C_AL, load, size, r, R1, off_scr);
	a_bx(LR);
	land(not_scratch);
}

static void emit_handlers(void)
{
	// escritura en una página con código traducido: r1 = desplazamiento en RAM << 11
	smc_handler = emit_ptr;
	a_dp_reg(C_AL, OP_MOV, 0, R0, 0, R1, SH_LSR, 11);
	call_c_inline((const void *)dyn_smc);
	a_bx(LR);
	for (int sz = 0; sz < 5; sz++)
		for (int i = 0; i < 6; i++) {
			int r = mem_regs[i];
			// carga: el valor en r
			ld_handler[sz][r] = emit_ptr;
			handler_scratch(1, sz, r);
			call_c_inline(rd_fn[sz]);
			if (r != R0) a_mov(r, R0);
			a_bx(LR);
			// escritura del valor de r: se llega si la dirección no es de la RAM (NE) o si lo es y en su
			// página había código traducido (EQ, r1 = desplazamiento << 11)
			if (sz == M_HS || sz == M_BS) continue;
			st_handler[sz][r] = emit_ptr;
			a_dp_imm(C_AL, OP_TST, 1, 0, R0, arm_imm(0x1f800000));
			a_b(C_EQ, 0, smc_handler);
			handler_scratch(0, sz, r);
			a_mov(R1, r);
			call_c_inline(wr_fn[sz]);
			a_bx(LR);
		}
}

static int ram_const(u32 a, u32 *off)   // dirección fija en RAM: desplazamiento
{
	if (a >= 0xc0000000 || (a & 0x1f800000)) return 0;
	*off = a & 0x1fffff;
	return 1;
}

static int scratch_const(u32 a, u32 *off)
{
	if (a >= 0xc0000000 || ((a & 0x1fffffff) & ~0x3ffu) != 0x1f800000) return 0;
	*off = a & 0x3ff;
	return 1;
}

// Valor probable de cada registro MIPS en el punto que se traduce (pv_ok: los que se saben). El bloque se
// traduce justo antes de ejecutarse: al empezar valen lo que hay en cpu.r, y se siguen las sumas sencillas
// (addiu v0, s1, 896 con s1 = 1F800000, como hace SH antes de un lwc2)
static u32 pv[32], pv_ok;
static void op_regs(u32 op, u32 *r, u32 *w);

static void pv_start(void)
{
	memcpy(pv, cpu.r, sizeof pv);
	pv[0] = 0;
	pv_ok = ~0u;
}

static void pv_after(u32 op)
{
	u32 o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, r, w;
	u32 ok_s = (pv_ok >> rs) & 1, ok_t = (pv_ok >> rt) & 1, v = 0;
	int known = 0, dst = -1;
	if (o == 0x08 || o == 0x09) { dst = (int)rt; known = ok_s; v = pv[rs] + (u32)(s32)(s16)op; }   // ADDI(U)
	else if (o == 0x0d) { dst = (int)rt; known = ok_s; v = pv[rs] | (op & 0xffff); }               // ORI
	else if (o == 0x0f) { dst = (int)rt; known = 1; v = op << 16; }                                 // LUI
	else if (o == 0 && ((op & 0x3f) == 0x21 || (op & 0x3f) == 0x25)) {                             // ADDU, OR
		dst = (int)rd; known = ok_s && ok_t;
		v = (op & 0x3f) == 0x21 ? pv[rs] + pv[rt] : pv[rs] | pv[rt];
	}
	op_regs(op, &r, &w);
	pv_ok &= ~w;
	if (dst > 0 && known) { pv[dst] = v; pv_ok |= 1u << dst; }
	pv_ok |= 1;
}


// Rangos al traducir: valores pequeños sin signo (lbu, andi, sll de ellos) y punteros a la scratchpad
// sacados de la base comprobada (scr_reg) más un valor pequeño. «addu a0, a0, s0» con a0 = byte << 2 queda
// dentro de la scratchpad seguro, y «lw a0, 0(a0)» se lee de la DTCM sin mirar nada.
enum { RK_NONE, RK_SMALL, RK_SCR };   // RK_SMALL: [lo, hi]; RK_SCR: dirección 1F800000 + [lo, hi]
static struct { u8 kind; u32 lo, hi; } rng[32];

static void rng_start(void) { memset(rng, 0, sizeof rng); }

static void rng_after(u32 op)
{
	u32 o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, f = op & 0x3f, r, w;
	int dst = -1, kind = RK_NONE;
	u32 lo = 0, hi = 0;
	op_regs(op, &r, &w);
	if (o == 0x24) { dst = (int)rt; kind = RK_SMALL; hi = 255; }                          // LBU
	else if (o == 0x25) { dst = (int)rt; kind = RK_SMALL; hi = 0xffff; }                   // LHU
	else if (o == 0x0c) { dst = (int)rt; kind = RK_SMALL; hi = op & 0xffff; }              // ANDI
	else if (o == 0 && f == 0x00 && rng[rt].kind == RK_SMALL && rng[rt].hi < 0x10000 && ((op >> 6) & 31) <= 15) {   // SLL
		u32 sa = (op >> 6) & 31;
		dst = (int)rd; kind = RK_SMALL; lo = rng[rt].lo << sa; hi = rng[rt].hi << sa;
	} else if (o == 0 && f == 0x02 && rng[rt].kind == RK_SMALL) {                             // SRL
		u32 sa = (op >> 6) & 31;
		dst = (int)rd; kind = RK_SMALL; lo = rng[rt].lo >> sa; hi = rng[rt].hi >> sa;
	} else if (o == 0 && f == 0x21) {                                                        // ADDU
		const int pair[2][2] = { { (int)rs, (int)rt }, { (int)rt, (int)rs } };   // probar los dos órdenes
		u32 base = scr_val - 0x1f800000;
		dst = (int)rd;
		for (int k = 0; k < 2; k++) {
			int a = pair[k][0], b = pair[k][1];
			if (rng[b].kind != RK_SMALL || rng[b].hi >= 0x400) continue;
			if (a == scr_reg && scr_ok) { kind = RK_SCR; lo = base + rng[b].lo; hi = base + rng[b].hi; break; }
			if (rng[a].kind == RK_SCR) { kind = RK_SCR; lo = rng[a].lo + rng[b].lo; hi = rng[a].hi + rng[b].hi; break; }
			if (rng[a].kind == RK_SMALL && rng[a].hi < 0x10000) { kind = RK_SMALL; lo = rng[a].lo + rng[b].lo; hi = rng[a].hi + rng[b].hi; break; }
		}
	} else if (o == 0x09 && (rng[rs].kind == RK_SCR || (rs == (u32)scr_reg && scr_ok))) {    // ADDIU sobre la scratchpad
		s32 im = (s16)op;
		u32 l0 = rng[rs].kind == RK_SCR ? rng[rs].lo : scr_val - 0x1f800000, h0 = rng[rs].kind == RK_SCR ? rng[rs].hi : l0;
		dst = (int)rt;
		if ((s32)l0 + im >= 0) { kind = RK_SCR; lo = l0 + (u32)im; hi = h0 + (u32)im; }
	}
	for (u32 m = w; m; m &= m - 1) rng[__builtin_ctz(m)].kind = RK_NONE;
	if (dst > 0 && kind != RK_NONE && hi < 0x10000 && lo <= hi) { rng[dst].kind = (u8)kind; rng[dst].lo = lo; rng[dst].hi = hi; }
	rng[0].kind = RK_NONE;
}

// ¿rs + simm cae seguro dentro de la scratchpad (bytes [0, 0x400))?
static int scr_derived(int rs, u32 simm, int bytes)
{
	s32 lo, hi;
	if (!rs || rng[rs].kind != RK_SCR) return 0;
	lo = (s32)rng[rs].lo + (s32)simm;
	hi = (s32)rng[rs].hi + (s32)simm + bytes;
	return lo >= 0 && hi <= 0x400;
}

// Acceso por un puntero derivado: dirección en la DS = r11 + off_scr + (rs - 1F800000) + simm
static void scr_derived_mem(int load, int size, int rs, u32 simm, int r)
{
	int x = get(rs, R0);
	a_dp_imm(C_AL, OP_SUB, 0, R1, x, arm_imm(0x1f800000));
	a_dp_reg(C_AL, OP_ADD, 0, R1, R1, RCPU, SH_LSL, 0);
	mem_off(C_AL, load, size, r, R1, off_scr + (s32)simm);
}

// ¿rs + simm apuntará a la scratchpad? (SH tiene s0 = 1F800000 casi siempre)
static int scratch_hint(int rs, u32 simm)
{
	u32 a;
	if (!(pv_ok & (1u << rs))) return 0;
	a = (pv[rs] + simm) & 0x1fffffff;
	return (a & ~0x3ffu) == 0x1f800000;
}

// Acceso con la scratchpad primero: r1 = dirección - 1F800000 (sin segmento), en línea si es < 400h;
// si no, la ruta general en C desde la zona fría (r0 = dirección)
static void scratch_first(int load, int size, int rs, u32 simm, int r)
{
	u32 *slow, *back;
	a_addi(R0, get(rs, R0), simm, R1);
	a_dp_imm(C_AL, OP_BIC, 0, R1, R0, arm_imm(0xe0000000));
	a_dp_imm(C_AL, OP_SUB, 0, R1, R1, arm_imm(0x1f800000));
	a_dp_imm(C_AL, OP_CMP, 1, 0, R1, arm_imm(0x400));
	a_dp_reg(C_CC, OP_ADD, 0, R1, R1, RCPU, SH_LSL, 0);
	mem_off(C_CC, load, size, r, R1, off_scr);
	slow = fwd(C_CS);
	back = emit_ptr;
	begin_cold();
	land(slow);
	if (!load && r != R1) a_mov(R1, r);
	call(load ? rd_fn[size] : wr_fn[size]);
	if (load && r != R0) a_mov(r, R0);
	a_b(C_AL, 0, back);
	end_cold();
}

// Carga en el registro ARM d (r2 o uno asignado) desde rs + simm
static void load_to(int size, int rs, u32 simm, int d)
{
	static const int hsh[5] = { 0, 3, 1, 2, 0 };
	if (rs == 29 && hsp_ok) { mem_off(C_AL, 1, size, d, RHSP, (s32)simm); return; }   // pila (ver RHSP)
	if (rs == scr_reg && scr_ok && scr_val - 0x1f800000 + simm < 0x400) { mem_off(C_AL, 1, size, d, RCPU, off_scr + (int)(scr_val - 0x1f800000 + simm)); return; }
	if (scr_derived(rs, simm, size == M_W ? 4 : size <= M_HU ? 2 : 1)) { scr_derived_mem(1, size, rs, simm, d); return; }
	if (is_const(rs)) {
		u32 a = const_of(rs) + simm, off;
		if (ram_const(a, &off)) mem_off(C_AL, 1, size, d, RRAM, (int)off);
		else if (scratch_const(a, &off)) mem_off(C_AL, 1, size, d, RCPU, off_scr + (int)off);
		else {
			a_movi(R0, a);
			call(rd_fn[size]);
			if (d != R0) a_mov(d, R0);
		}
		return;
	}
	if (scratch_hint(rs, simm)) { scratch_first(1, size, rs, simm, d); return; }
	a_addi(R0, get(rs, R0), simm, R1);
	a_dp_imm(C_AL, OP_TST, 1, 0, R0, arm_imm(0x1f800000));   // fuera de la RAM (bits 23-28): NE
	a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, R0, SH_LSL, 11);
	if (size == M_W || size == M_BU) a_mem_regsh(C_EQ, 1, size == M_BU, d, RRAM, R1, SH_LSR, 11);
	else {
		a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, R1, SH_LSR, 11);
		a_memh_reg(C_EQ, 1, hsh[size], d, RRAM, R1);
	}
	a_b(C_NE, 1, ld_handler[size][d]);
}

static void emit_load(int size, int rs, int rt, u32 simm)
{
	int d = dst(rt, R2);
	load_to(size, rs, simm, d);
	put(rt, d);
}

// Escribe el registro ARM v (r2 o uno asignado) en rs + simm. size: M_W, M_HU, M_BU
static void store_from(int size, int rs, u32 simm, int v)
{
	if (rs == 29 && hsp_ok) { mem_off(C_AL, 0, size, v, RHSP, (s32)simm); return; }
	if (rs == scr_reg && scr_ok && scr_val - 0x1f800000 + simm < 0x400) { mem_off(C_AL, 0, size, v, RCPU, off_scr + (int)(scr_val - 0x1f800000 + simm)); return; }
	if (scr_derived(rs, simm, size == M_W ? 4 : size <= M_HU ? 2 : 1)) { scr_derived_mem(0, size, rs, simm, v); return; }
	if (is_const(rs)) {
		u32 a = const_of(rs) + simm, off;
		if (ram_const(a, &off)) {
			u32 *smc, *back;
			mem_off(C_AL, 0, size, v, RRAM, (int)off);
			a_mem_imm(C_AL, 1, 1, R3, RPAGES, (int)(off >> DYN_PAGE_BITS));
			a_dp_imm(C_AL, OP_CMP, 1, 0, R3, 0);
			smc = fwd(C_NE);
			back = emit_ptr;
			begin_cold();
			land(smc);
			a_movi(R0, off);
			call(dyn_smc);
			a_b(C_AL, 0, back);
			end_cold();
		} else if (scratch_const(a, &off)) mem_off(C_AL, 0, size, v, RCPU, off_scr + (int)off);
		else {
			a_movi(R0, a);
			if (v != R1) a_mov(R1, v);
			call(wr_fn[size]);
		}
		return;
	}
	if (scratch_hint(rs, simm)) { scratch_first(0, size, rs, simm, v); return; }
	a_addi(R0, get(rs, R0), simm, R1);
	a_dp_imm(C_AL, OP_TST, 1, 0, R0, arm_imm(0x1f800000));
	a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, R0, SH_LSL, 11);
	if (size == M_W || size == M_BU) a_mem_regsh(C_EQ, 0, size == M_BU, v, RRAM, R1, SH_LSR, 11);
	else {
		a_dp_reg(C_EQ, OP_MOV, 0, R3, 0, R1, SH_LSR, 11);
		a_memh_reg(C_EQ, 0, 1, v, RRAM, R3);
	}
	a_mem_regsh(C_EQ, 1, 1, R3, RPAGES, R1, SH_LSR, 11 + DYN_PAGE_BITS);   // ¿había código traducido en esa página?
	a_dp_imm(C_EQ, OP_CMP, 1, 0, R3, 0);
	a_b(C_NE, 1, st_handler[size][v]);   // E/S o scratchpad (NE del TST) o página con código
}

static void emit_store(int size, int rs, int rt, u32 simm)
{
	int v = get(rt, R2);
	store_from(size, rs, simm, v);
}
// Instrucción delegada al intérprete; si provoca excepción, se sale del bloque
static void emit_interp(u32 op, u32 pc, int ninstr)
{
	u32 *ok;
	flush_regs();
	a_movi(R0, op);
	a_movi(R1, pc);
	call(cpu_dyn_interp);
	a_dp_imm(C_AL, OP_CMP, 1, 0, R0, 0);
	ok = fwd(C_NE);
	begin_cold();
	land(ok);
	exit_block(ninstr);
	end_cold();
	forget((1u << ((op >> 16) & 31)) | (1u << ((op >> 11) & 31)));
}

static int is_branch(u32 op)
{
	u32 o = op >> 26, f = op & 0x3f;
	if (o == 0) return f == 0x08 || f == 0x09;
	return o >= 1 && o <= 7;
}

// Instrucciones que terminan el bloque después de ejecutarse (cambian SR, excepciones...)
static int ends_block(u32 op)
{
	u32 o = op >> 26, f = op & 0x3f;
	if (o == 0) return f == 0x0c || f == 0x0d;      // SYSCALL, BREAK
	if (o == 0x10) return ((op >> 21) & 31) == 4 || ((op >> 21) & 31) == 0x10;   // MTC0, RFE
	return 0;
}

// Registros MIPS que lee y escribe una instrucción (para elegir cuáles van en registros ARM)
static void op_regs_calc(u32 op, u32 *r, u32 *w)
{
	u32 o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, f = op & 0x3f;
	*r = *w = 0;
	switch (o) {
	case 0x00:
		if (f <= 0x03) { *r = 1u << rt; *w = 1u << rd; }
		else if (f <= 0x07) { *r = (1u << rs) | (1u << rt); *w = 1u << rd; }
		else if (f == 0x08) *r = 1u << rs;
		else if (f == 0x09) { *r = 1u << rs; *w = 1u << rd; }
		else if (f == 0x10 || f == 0x12) *w = 1u << rd;
		else if (f == 0x11 || f == 0x13) *r = 1u << rs;
		else if (f >= 0x18 && f <= 0x1b) *r = (1u << rs) | (1u << rt);
		else if (f >= 0x20) { *r = (1u << rs) | (1u << rt); *w = 1u << rd; }
		break;
	case 0x01: *r = 1u << rs; if ((rt & 0x1e) == 0x10) *w = 1u << 31; break;
	case 0x03: *w = 1u << 31; break;
	case 0x04: case 0x05: *r = (1u << rs) | (1u << rt); break;
	case 0x06: case 0x07: *r = 1u << rs; break;
	case 0x0f: *w = 1u << rt; break;
	case 0x10: case 0x12:
		if (rs == 0 || rs == 2) *w = 1u << rt;
		else if (rs == 4 || rs == 6) *r = 1u << rt;
		break;
	case 0x22: case 0x26: *r = (1u << rs) | (1u << rt); *w = 1u << rt; break;   // LWL, LWR
	case 0x32: case 0x3a: *r = 1u << rs; break;                                  // LWC2, SWC2
	default:
		if (o >= 0x08 && o <= 0x0e) { *r = 1u << rs; *w = 1u << rt; }
		else if (o >= 0x20 && o <= 0x25) { *r = 1u << rs; *w = 1u << rt; }
		else if (o >= 0x28 && o <= 0x2e) *r = (1u << rs) | (1u << rt);
		break;
	}
	*r &= ~1u;
	*w &= ~1u;
}

// Se pide varias veces seguidas para la misma instrucción (asignación, valores, rangos): recordar la última
static void op_regs(u32 op, u32 *r, u32 *w)
{
	static u32 last_op = 0xffffffff, last_r, last_w;
	if (op != last_op) { op_regs_calc(op, &last_r, &last_w); last_op = op; }
	*r = last_r;
	*w = last_w;
}

static u32 fetch_op(u32 pc)
{
	u32 p = pc & 0x1fffffff;
	if (p < 0x800000) return *(u32 *)(psx_ram + (p & 0x1ffffc));
	return *(u32 *)(psx_bios + (p & 0x7fffc));
}

// ¿Carga o escritura (de las que van en línea) con base sp?
static int sp_mem_op(u32 op)
{
	u32 o = op >> 26;
	return ((op >> 21) & 31) == 29 && (o == 0x20 || o == 0x21 || o == 0x23 || o == 0x24 || o == 0x25 || o == 0x28 || o == 0x29 || o == 0x2b);
}

// Elige qué registros MIPS van en registros ARM: los más usados del bloque (al menos dos veces)
static int psyq_vsync(u32 pc);
static int vsync_poll_call(u32 pc);

// Superbloques: un salto condicional sin enlace que no sea de un bucle de espera no corta el bloque; se sigue
// traduciendo por el camino de no saltar (el de saltar sale enlazado como siempre)
// Límite del superbloque y no seguir por código que ya tiene su propio bloque: cada destino de salto empieza
// otro superbloque, y sin esto el mismo camino se traduciría muchas veces (la caché se llenaba)
#define SB_MAX 32
static block_fn *slot(u32 pc);
static int has_block(u32 pc)
{
	block_fn *s = slot(pc);
	return s && *s && (*s)[-1] == pc;
}

static int sb_cont(u32 op, u32 cur, u32 pc)
{
	u32 o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31;
	u32 target = cur + 4 + ((u32)(s32)(s16)(op & 0xffff) << 2);
	int cond = (o == 0x01 && rs && (rt & 0x1e) != 0x10) || ((o == 0x04 || o == 0x05) && rs != rt) || ((o == 0x06 || o == 0x07) && rs);
	if (!cond) return 0;
	return target != pc && !psyq_vsync(target) && !vsync_poll_call(target);
}

static void alloc_regs(u32 pc)
{
	u8 uses[32] = { 0 };
	u32 seen = 0;
	int n = 0;
	int sp_ops = 0;
	u8 scr_uses[32] = { 0 };
	u32 wr_before = 0;
	for (u32 cur = pc;; cur += 4) {
		u32 op = fetch_op(cur), r, w, m;
		int last = 0;
		if (++n > MAX_BLOCK || ends_block(op)) last = 1;
		int cont = 0;
		if (is_branch(op)) {   // con su delay slot
			u32 r2, w2, dop = fetch_op(cur + 4);
			op_regs(dop, &r2, &w2);
			sp_ops += sp_mem_op(dop);
			for (m = r2 | w2; m; m &= m - 1) uses[__builtin_ctz(m)] += 1;
			seen |= r2 | w2;
			// superbloque: tras un salto condicional se sigue por el camino de no saltar (igual que compile)
			if (sb_cont(op, cur, pc) && n + 2 < SB_MAX && !has_block(cur + 8) && !is_branch(dop) && !ends_block(dop)) { cont = 1; n++; wr_before |= w2; }
			else last = 1;
		}
		sp_ops += sp_mem_op(op);
		op_regs(op, &r, &w);
		{   // base de la scratchpad conocida al entrar y aún sin cambiar
			u32 o = op >> 26, rs = (op >> 21) & 31;
			if (((o >= 0x20 && o <= 0x2e) || o == 0x32 || o == 0x3a) && rs && !(wr_before & (1u << rs)) &&
			    (pv[rs] & 0xfffffc00) == 0x1f800000) scr_uses[rs]++;
			wr_before |= w;
		}
		for (m = r; m; m &= m - 1) uses[__builtin_ctz(m)]++;
		for (m = w; m; m &= m - 1) uses[__builtin_ctz(m)]++;
		seen |= r | w;
		if (last) break;
		if (cont) cur += 4;   // el delay slot ya está contado
	}
	memset(rg, 0, sizeof rg);
	for (int i = 0; i < 32; i++) rg[i].host = -1;
	stale_mask = 0;
	use_hsp = hsp_ok = sp_ops >= 2 && lin(pc) < 0x200000 && !sp_is_unsafe(pc);
	scr_reg = scr_ok = 0;
	if (!pc_in(scr_unsafe, pc) && scr_unsafe_n < 200)
		for (int i = 1; i < 32; i++)
			if (scr_uses[i] >= 2 && (!scr_reg || scr_uses[i] > scr_uses[scr_reg])) scr_reg = i;
	if (scr_reg) { scr_ok = 1; scr_val = pv[scr_reg]; }
	for (int h = 0; h < 5 && seen; h++) {
		if (use_hsp && host_pool[h] == RHSP) continue;
		int best = 0;
		for (u32 m = seen; m; m &= m - 1) {
			int i = __builtin_ctz(m);
			if (uses[i] > uses[best]) best = i;
		}
		if (!best || uses[best] < 2) break;
		seen &= ~(1u << best);
		rg[best].host = (s8)host_pool[h];
	}
}

// ---- ALU ----
static u32 alu_fold(int f, u32 a, u32 b)
{
	switch (f) {
	case 0x04: return b << (a & 31);
	case 0x06: return b >> (a & 31);
	case 0x07: return (u32)((s32)b >> (a & 31));
	case 0x20: case 0x21: return a + b;
	case 0x22: case 0x23: return a - b;
	case 0x24: return a & b;
	case 0x25: return a | b;
	case 0x26: return a ^ b;
	case 0x27: return ~(a | b);
	case 0x2a: return (s32)a < (s32)b;
	case 0x2b: return a < b;
	}
	return 0;
}

// ---- registros de la GTE ----
// Los simples (gte_reg_parts) se copian en línea: están en la DTCM, normalmente a tiro de r11
static void gte_part_mem(int load, const GteRegPart *p, int rd)
{
	int off = (int)((u8 *)p->addr - (u8 *)&cpu), lim = p->bits == 32 ? 4095 : 255, base = RCPU;
	if (off < -lim || off > lim) {
		a_movi(R1, (u32)(uintptr_t)p->addr);
		base = R1;
		off = 0;
	}
	if (p->bits == 32) a_mem_imm(C_AL, load, 0, rd, base, off);
	else a_memh_imm(C_AL, load, load && p->sign ? 3 : 1, rd, base, off);
}

// Registro reg de la GTE (0-63) en el registro ARM d (ni r1 ni r3)
static void gte_to(int reg, int d)
{
	GteRegPart p[2];
	int n = gte_reg_parts(reg, 0, p);
	if (!n) {
		a_movi(R0, (u32)reg);
		call(gte_read);
		if (d != R0) a_mov(d, R0);
		return;
	}
	gte_part_mem(1, &p[0], d);
	if (n == 2) {
		gte_part_mem(1, &p[1], R3);
		a_dp_reg(C_AL, OP_ORR, 0, d, d, R3, SH_LSL, 16);
	}
}

// Registro ARM v (ni r0, ni r1 ni r3) al registro reg de la GTE
static void gte_from(int reg, int v)
{
	GteRegPart p[2];
	int n = gte_reg_parts(reg, 1, p);
	if (!n) {
		a_mov(R1, v);
		a_movi(R0, (u32)reg);
		call(gte_write);
		return;
	}
	gte_part_mem(0, &p[0], v);
	if (n == 2) {
		a_dp_reg(C_AL, OP_MOV, 0, R3, 0, v, SH_LSR, 16);
		gte_part_mem(0, &p[1], R3);
	}
}

// Traduce una instrucción que no es salto. Devuelve 0 si termina el bloque.
static int emit_op(u32 op, u32 pc, int ninstr)
{
	u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
	u32 imm = op & 0xffff, simm = (u32)(s32)(s16)imm;

	switch (op >> 26) {
	case 0x00:
		switch (op & 0x3f) {
		case 0x00: case 0x02: case 0x03: {   // SLL, SRL, SRA
			static const int shs[4] = { SH_LSL, 0, SH_LSR, SH_ASR };
			int a, d;
			if (!rd) return 1;
			if (is_const(rt)) {
				u32 v = const_of(rt);
				put_const(rd, (op & 3) == 0 ? v << sa : (op & 3) == 2 ? v >> sa : (u32)((s32)v >> sa));
				return 1;
			}
			a = get(rt, R0);
			d = dst(rd, R0);
			a_dp_reg(C_AL, OP_MOV, 0, d, 0, a, shs[op & 3], sa);
			put(rd, d);
			return 1;
		}
		case 0x04: case 0x06: case 0x07: {   // SLLV, SRLV, SRAV
			static const int shs[4] = { SH_LSL, 0, SH_LSR, SH_ASR };
			int a, s, d;
			if (!rd) return 1;
			if (is_const(rs) && is_const(rt)) { put_const(rd, alu_fold(op & 0x3f, const_of(rs), const_of(rt))); return 1; }
			a = get(rt, R0);
			s = get(rs, R1);
			a_dp_imm(C_AL, OP_AND, 0, R1, s, 31);
			d = dst(rd, R0);
			a_dp_regsh(C_AL, OP_MOV, 0, d, 0, a, shs[op & 3], R1);
			put(rd, d);
			return 1;
		}
		case 0x0c: case 0x0d:   // SYSCALL, BREAK
			flush_regs();
			a_movi(R0, (op & 0x3f) == 0x0c ? 8 : 9);
			a_movi(R1, pc);
			call(cpu_dyn_exception);
			exit_block(ninstr);
			return 0;
		case 0x10: case 0x12: {   // MFHI, MFLO
			int d;
			if (!rd) return 1;
			d = dst(rd, R0);
			a_ldr(d, RCPU, (op & 0x3f) == 0x10 ? OFF_HI : OFF_LO);
			put(rd, d);
			return 1;
		}
		case 0x11: case 0x13:   // MTHI, MTLO
			a_str(get(rs, R0), RCPU, (op & 0x3f) == 0x11 ? OFF_HI : OFF_LO);
			return 1;
		case 0x18: case 0x19: {   // MULT, MULTU
			int a = get(rs, R0), b = get(rt, R1);
			if ((op & 0x3f) == 0x18) a_smull(R2, R3, a, b); else a_umull(R2, R3, a, b);
			a_str(R2, RCPU, OFF_LO);
			a_str(R3, RCPU, OFF_HI);
			return 1;
		}
		case 0x1a: case 0x1b: {   // DIV, DIVU
			int a = get(rs, R0), b = get(rt, R1);   // a es r0 o asignado; b, r1 o asignado
			if (a != R0) a_mov(R0, a);
			if (b != R1) a_mov(R1, b);
			call((op & 0x3f) == 0x1a ? (const void *)h_div : (const void *)h_divu);
			return 1;
		}
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {
			// ADD, ADDU, SUB, SUBU, AND, OR, XOR, NOR (ADD/SUB sin excepción de desbordamiento)
			int f = op & 0x3f, a, b, d;
			int aop = (f <= 0x21) ? OP_ADD : (f <= 0x23) ? OP_SUB : f == 0x24 ? OP_AND : f == 0x26 ? OP_EOR : OP_ORR;
			if (!rd) return 1;
			if (is_const(rs) && is_const(rt)) { put_const(rd, alu_fold(f, const_of(rs), const_of(rt))); return 1; }
			if (rt == 0 && f == 0x27) {   // NOR con cero = negación
				a = get(rs, R0);
				d = dst(rd, R0);
				a_dp_reg(C_AL, OP_MVN, 0, d, 0, a, SH_LSL, 0);
				put(rd, d);
				return 1;
			}
			if ((rt == 0 && f != 0x24) || (rs == 0 && f != 0x24 && f != 0x27 && f != 0x22 && f != 0x23)) {
				// copia: se carga directamente en el registro de destino, o se guarda directamente el de origen
				int src = rt == 0 ? (int)rs : (int)rt;
				d = dst(rd, -1);
				if (d < 0) put(rd, get(src, R0));
				else {
					a = get(src, d);
					if (a != d) a_mov(d, a);
					put(rd, d);
				}
				return 1;
			}
			if (is_const(rt) && f != 0x27 && arm_imm(const_of(rt)) >= 0) {
				a = get(rs, R0);
				d = dst(rd, R0);
				a_dp_imm(C_AL, aop, 0, d, a, arm_imm(const_of(rt)));
				put(rd, d);
				return 1;
			}
			a = get(rs, R0);
			b = get(rt, R1);
			d = dst(rd, R0);
			a_dp_reg(C_AL, aop, 0, d, a, b, SH_LSL, 0);
			if (f == 0x27) a_dp_reg(C_AL, OP_MVN, 0, d, 0, d, SH_LSL, 0);
			put(rd, d);
			return 1;
		}
		case 0x2a: case 0x2b: {   // SLT, SLTU
			int a, b, d, lt = (op & 0x3f) == 0x2a ? C_LT : C_CC;
			if (!rd) return 1;
			if (is_const(rs) && is_const(rt)) { put_const(rd, alu_fold(op & 0x3f, const_of(rs), const_of(rt))); return 1; }
			a = get(rs, R0);
			b = get(rt, R1);
			d = dst(rd, R0);
			a_dp_reg(C_AL, OP_CMP, 1, 0, a, b, SH_LSL, 0);
			a_dp_imm(lt, OP_MOV, 0, d, 0, 1);
			a_dp_imm(lt ^ 1, OP_MOV, 0, d, 0, 0);
			put(rd, d);
			return 1;
		}
		}
		break;
	case 0x08: case 0x09: {   // ADDI, ADDIU
		int a, d;
		if (!rt) return 1;
		if (is_const(rs)) { put_const(rt, const_of(rs) + simm); return 1; }
		d = dst(rt, R0);
		a = get(rs, d);   // si rs no tiene registro, se carga ya en el destino
		if (simm == 0 && d == R0 && a != R0) { put(rt, a); return 1; }   // copia a un registro sin asignar: guardar el origen
		a_addi(d, a, simm, R1);
		if (rt == 29 && rs == 29 && hsp_ok) {   // addiu sp, sp, n: la dirección real de la pila se mueve igual
			put(rt, d);
			a_addi(RHSP, RHSP, simm, R1);
			hsp_ok = 1;
			return 1;
		}
		put(rt, d);
		return 1;
	}
	case 0x0a: case 0x0b: {   // SLTI, SLTIU
		int a, d, lt = (op >> 26) == 0x0a ? C_LT : C_CC;
		if (!rt) return 1;
		if (is_const(rs)) { put_const(rt, alu_fold((op >> 26) == 0x0a ? 0x2a : 0x2b, const_of(rs), simm)); return 1; }
		a = get(rs, R0);
		d = dst(rt, R0);
		if (arm_imm(simm) >= 0) a_dp_imm(C_AL, OP_CMP, 1, 0, a, arm_imm(simm));
		else if (arm_imm(-simm) >= 0) a_dp_imm(C_AL, OP_CMN, 1, 0, a, arm_imm(-simm));
		else { a_movi(R1, simm); a_dp_reg(C_AL, OP_CMP, 1, 0, a, R1, SH_LSL, 0); }
		a_dp_imm(lt, OP_MOV, 0, d, 0, 1);
		a_dp_imm(lt ^ 1, OP_MOV, 0, d, 0, 0);
		put(rt, d);
		return 1;
	}
	case 0x0c: case 0x0d: case 0x0e: {   // ANDI, ORI, XORI
		int o = op >> 26, aop = o == 0x0c ? OP_AND : o == 0x0d ? OP_ORR : OP_EOR, e = arm_imm(imm), a, d;
		if (!rt) return 1;
		if (is_const(rs)) {
			u32 v = const_of(rs);
			put_const(rt, o == 0x0c ? v & imm : o == 0x0d ? v | imm : v ^ imm);
			return 1;
		}
		a = get(rs, R0);
		d = dst(rt, R0);
		if (e >= 0) a_dp_imm(C_AL, aop, 0, d, a, e);
		else if (aop != OP_AND) {   // OR/XOR con 16 bits: byte alto y byte bajo por separado
			a_dp_imm(C_AL, aop, 0, d, a, arm_imm(imm & 0xff00));
			a_dp_imm(C_AL, aop, 0, d, d, arm_imm(imm & 0xff));
		} else if (imm == 0xffff) {   // quedarse con los 16 bits bajos
			a_dp_reg(C_AL, OP_MOV, 0, d, 0, a, SH_LSL, 16);
			a_dp_reg(C_AL, OP_MOV, 0, d, 0, d, SH_LSR, 16);
		} else {
			a_movi(R1, imm);
			a_dp_reg(C_AL, OP_AND, 0, d, a, R1, SH_LSL, 0);
		}
		put(rt, d);
		return 1;
	}
	case 0x0f:   // LUI
		put_const(rt, imm << 16);
		return 1;
	case 0x12:   // COP2 (GTE): directo, sin pasar por el intérprete
		if (op & (1 << 25)) {   // comando: su función directa, sin trampolín (la GTE no mira el reloj; r12 se guarda)
			void (*fn)(u32) = gte_entry(op);
			s32 d = (s32)((const u8 *)fn - ((const u8 *)emit_ptr + 12));
			a_movi(R0, op);
			if (d < -0x2000000 || d >= 0x2000000) { call(gte_command); return 1; }
			a_push((1u << R12) | (1u << LR));
			a_b(C_AL, 1, (const void *)fn);
			a_pop((1u << R12) | (1u << LR));
			return 1;
		}
		switch (rs) {
		case 0x00: case 0x02: {   // MFC2, CFC2
			int d = dst(rt, R0);
			gte_to((int)rd + (rs == 2 ? 32 : 0), d);
			put(rt, d);
			return 1;
		}
		case 0x04: case 0x06:   // MTC2, CTC2
			gte_from((int)rd + (rs == 6 ? 32 : 0), get(rt, R2));
			return 1;
		}
		break;
	case 0x20: emit_load(M_BS, rs, rt, simm); return 1;   // LB
	case 0x21: emit_load(M_HS, rs, rt, simm); return 1;   // LH
	case 0x23: emit_load(M_W, rs, rt, simm); return 1;    // LW
	case 0x24: emit_load(M_BU, rs, rt, simm); return 1;   // LBU
	case 0x25: emit_load(M_HU, rs, rt, simm); return 1;   // LHU
	case 0x22: case 0x26: {   // LWL, LWR: la palabra alineada (RAM en línea, lo demás por su rutina) y la mezcla
		int x, cur, d;
		if (!rt) return 1;
		x = get(rs, R0);
		a_addi(R0, x, simm, R1);
		a_dp_imm(C_AL, OP_BIC, 0, R0, R0, 3);
		a_dp_imm(C_AL, OP_TST, 1, 0, R0, arm_imm(0x1f800000));
		a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, R0, SH_LSL, 11);
		a_mem_regsh(C_EQ, 1, 0, R2, RRAM, R1, SH_LSR, 11);
		a_b(C_NE, 1, ld_handler[M_W][R2]);
		x = get(rs, R3);   // la rutina lenta pierde r0-r3: el desplazamiento se calcula después
		a_addi(R3, x, simm, R1);
		a_dp_imm(C_AL, OP_AND, 0, R3, R3, 3);
		a_dp_reg(C_AL, OP_MOV, 0, R3, 0, R3, SH_LSL, 3);   // s = (dirección & 3) * 8
		cur = get(rt, R1);
		d = dst(rt, R1);
		if ((op >> 26) == 0x22) {   // LWL: (rt & (00FFFFFF >> s)) | (w << (24 - s))
			a_dp_imm(C_AL, OP_RSB, 0, R0, R3, 24);
			a_dp_regsh(C_AL, OP_MOV, 0, R2, 0, R2, SH_LSL, R0);
			a_dp_imm(C_AL, OP_MVN, 0, R0, 0, arm_imm(0xff000000));
			a_dp_regsh(C_AL, OP_MOV, 0, R0, 0, R0, SH_LSR, R3);
			a_dp_reg(C_AL, OP_AND, 0, d, cur, R0, SH_LSL, 0);
		} else {           // LWR: (rt & ~(FFFFFFFF >> s)) | (w >> s)
			a_dp_regsh(C_AL, OP_MOV, 0, R2, 0, R2, SH_LSR, R3);
			a_dp_imm(C_AL, OP_MVN, 0, R0, 0, 0);
			a_dp_regsh(C_AL, OP_MOV, 0, R0, 0, R0, SH_LSR, R3);
			a_dp_reg(C_AL, OP_BIC, 0, d, cur, R0, SH_LSL, 0);
		}
		a_dp_reg(C_AL, OP_ORR, 0, d, d, R2, SH_LSL, 0);
		put(rt, d);
		return 1;
	}
	case 0x28: emit_store(M_BU, rs, rt, simm); return 1;  // SB
	case 0x29: emit_store(M_HU, rs, rt, simm); return 1;  // SH
	case 0x2b: emit_store(M_W, rs, rt, simm); return 1;   // SW
	case 0x32:   // LWC2
		load_to(M_W, rs, simm, R2);
		gte_from((int)rt, R2);
		return 1;
	case 0x3a:   // SWC2
		gte_to((int)rt, R2);
		store_from(M_W, rs, simm, R2);
		return 1;
	}

	// todo lo demás, con el intérprete
	emit_interp(op, pc, ninstr);
	if (ends_block(op)) {   // MTC0/RFE pueden activar interrupciones: que C lo mire
		set_pc_const(pc + 4);
		exit_block(ninstr);
		return 0;
	}
	return 1;
}

// ---- bucles de espera ----
// Un bucle que solo lee memoria y vuelve a empezar hace lo mismo en cada vuelta hasta que una
// interrupción o un dispositivo cambie algo: en vez de dar vueltas se adelanta el reloj al siguiente
// evento (psx_run_until). También el VSync() de las librerías PsyQ, que además cuenta un tiempo límite.

// Instrucción sin efectos salvo escribir un registro (y leer memoria): registros que lee y escribe
static int pure_op(u32 op, u32 *rd_mask, u32 *wr_mask)
{
	u32 o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
	*rd_mask = *wr_mask = 0;
	if (op == 0) return 1;
	switch (o) {
	case 0x00:
		switch (op & 0x3f) {
		case 0x00: case 0x02: case 0x03: *rd_mask = 1u << rt; *wr_mask = 1u << rd; return 1;
		case 0x04: case 0x06: case 0x07: case 0x20: case 0x21: case 0x22: case 0x23:
		case 0x24: case 0x25: case 0x26: case 0x27: case 0x2a: case 0x2b:
			*rd_mask = (1u << rs) | (1u << rt);
			*wr_mask = 1u << rd;
			return 1;
		}
		return 0;
	case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e:   // ALU con inmediato
	case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:                          // cargas
		*rd_mask = 1u << rs;
		*wr_mask = 1u << rt;
		return 1;
	case 0x0f: *wr_mask = 1u << rt; return 1;   // LUI
	}
	return 0;
}

// ¿El código de start al salto en branch_pc (con su delay slot) solo espera a que cambie la memoria?
// Ningún registro puede pasar de una vuelta a la siguiente (eso sería un contador que avanza).
static int pure_loop(u32 start, u32 branch_pc)
{
	u32 written = 0, defined = 0, op = fetch_op(branch_pc), o = op >> 26, r, w;
	if (o == 0x01 && (((op >> 16) & 31) & 0x1e) == 0x10) return 0;   // BLTZAL/BGEZAL escriben ra
	if (branch_pc - start > 32 * 4) return 0;
	for (u32 pc = start; pc <= branch_pc + 4; pc += 4) {
		if (pc == branch_pc) continue;
		if (!pure_op(fetch_op(pc), &r, &w)) return 0;
		written |= w;
	}
	for (u32 pc = start; pc <= branch_pc + 4; pc += 4) {
		if (pc == branch_pc) {
			r = (1u << ((op >> 21) & 31)) | ((o == 0x04 || o == 0x05) ? 1u << ((op >> 16) & 31) : 0);
			w = 0;
		} else pure_op(fetch_op(pc), &r, &w);
		if (r & written & ~defined & ~1u) return 0;
		defined |= w;
	}
	return 1;
}

// Primeras instrucciones del bucle de VSync() de PsyQ: lw x,off(sp); nop; addiu x,x,-1; sw x,off(sp);
// lw x,off(sp); nop; bne ...  (luego mira el contador de vblanks y vuelve aquí)
static int psyq_vsync(u32 pc)
{
	u32 w0 = fetch_op(pc), x = (w0 >> 16) & 31, off = w0 & 0xffff;
	if ((w0 >> 26) != 0x23 || ((w0 >> 21) & 31) != 29) return 0;
	return fetch_op(pc + 4) == 0 && fetch_op(pc + 8) == (0x2400ffffu | x << 21 | x << 16) &&
	       fetch_op(pc + 12) == (0xafa00000u | x << 16 | off) && fetch_op(pc + 16) == w0 && fetch_op(pc + 20) == 0 &&
	       (fetch_op(pc + 24) >> 26) == 0x05;
}

// «jal X» con «li a0,-1» en el delay slot, y X con un «bgez a0» al principio: VSync(-1) de PsyQ, que solo
// devuelve el contador de vblanks. Un bucle que lo llama y compara el resultado espera a la vblank.
static int vsync_poll_call(u32 pc)
{
	u32 j = fetch_op(pc), x;
	if ((j >> 26) != 3 || fetch_op(pc + 4) != 0x2404ffff) return 0;
	x = (pc & 0xf0000000) | ((j & 0x3ffffff) << 2);
	for (int i = 0; i < 24; i++)
		if ((fetch_op(x + i * 4) & 0xffff0000) == 0x04810000) return 1;
	return 0;
}

// Saltar la espera hasta el siguiente evento
static void emit_idle(u32 target)
{
#ifdef PSX_PROFILE
	a_movi(R2, (u32)(uintptr_t)&psx_stat.dyn_idle);
	a_ldr(R3, R2, 0);
	a_dp_imm(C_AL, OP_ADD, 0, R3, R3, 1);
	a_str(R3, R2, 0);
#endif
	a_dp_imm(C_AL, OP_MOV, 0, RCYC, 0, 0);   // psx_cycles = psx_run_until
	exit_pc(target);
}

static void mark_pages(u32 start, u32 end)
{
	u32 p0 = start & 0x1fffffff, p1 = end & 0x1fffffff;
	if (p0 >= 0x800000) return;
	for (u32 p = (p0 & 0x1fffff) >> DYN_PAGE_BITS; p <= ((p1 & 0x1fffff) >> DYN_PAGE_BITS) && p < DYN_NPAGES; p++) dyn_pages[p] = 1;
}

// Compara los operandos de un salto condicional (b < 0: con cero) y devuelve la condición ARM de «se salta»
static int branch_cond(u32 op, int a, int b)
{
	u32 o = op >> 26, rt = (op >> 16) & 31;
	if (b >= 0) a_dp_reg(C_AL, OP_CMP, 1, 0, a, b, SH_LSL, 0);
	else a_dp_imm(C_AL, OP_CMP, 1, 0, a, 0);
	switch (o) {
	case 0x01: return (rt & 1) ? C_GE : C_LT;   // BGEZ(AL) / BLTZ(AL)
	case 0x04: return C_EQ;
	case 0x05: return C_NE;
	case 0x06: return C_LE;
	default: return C_GT;
	}
}

// Operandos del salto en registros ARM (tmp1, tmp2 si no están asignados)
static void branch_operands(u32 op, int *a, int *b, int tmp1, int tmp2)
{
	u32 o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31;
	*a = get(rs, tmp1);
	*b = (o == 0x04 || o == 0x05) && rt ? get(rt, tmp2) : -1;
}

// Traduce el bloque que empieza en pc; NULL si no se puede (se interpretará)
static block_fn compile(u32 pc, block_fn *s)
{
	u32 *start, *entry, *cold_start, cur = pc;
	int n = 0, tot = 0;   // n: instrucciones del tramo actual (para los ciclos); tot: de todo el superbloque

	if (emit_ptr + MAX_BLOCK * 48 + 512 > cold_base || cold_ptr + MAX_BLOCK * 16 + 256 > code_end) flush_all();
	start = emit_ptr;
	cold_start = cold_ptr;
	pv_start();
	rng_start();
	alloc_regs(pc);
	emit(pc);   // cabecera: el PC virtual con el que se tradujo
	entry = emit_ptr;
	if (use_hsp) {   // atajo de la pila: sp en la RAM (y no en los primeros 32 KB) -> r8 = su dirección en la DS
		int sp = get(29, R0);
		u32 *fail1, *fail2;
		a_dp_imm(C_AL, OP_TST, 1, 0, sp, arm_imm(0x1f800000));
		fail1 = fwd(C_NE);
		a_dp_reg(C_AL, OP_MOV, 0, R0, 0, sp, SH_LSL, 11);
		a_dp_imm(C_AL, OP_CMP, 1, 0, R0, arm_imm(0x04000000));
		fail2 = fwd(C_CC);
		a_dp_reg(C_AL, OP_ADD, 0, RHSP, RRAM, R0, SH_LSR, 11);
		sp_fail[0] = fail1;
		sp_fail[1] = fail2;
	}
	if (scr_reg) {   // base de la scratchpad: ¿sigue valiendo lo mismo?
		int x = get(scr_reg, R0), k = arm_imm(scr_val);
		if (k >= 0) a_dp_imm(C_AL, OP_CMP, 1, 0, x, k);
		else { a_movi(R1, scr_val); a_dp_reg(C_AL, OP_CMP, 1, 0, x, R1, SH_LSL, 0); }
		scr_fail = fwd(C_NE);
	}

	for (;;) {
		u32 op = fetch_op(cur);
		n++;
		tot++;
		if (is_branch(op)) {
			u32 dop = fetch_op(cur + 4), dr, dw, operands, clobbered;
			u32 o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
			u32 simm = (u32)(s32)(s16)(op & 0xffff), target = cur + 4 + (simm << 2);
			int dynamic = 0, cond_kind = 0, link_reg = 0, goon = 0;   // cond_kind: 0 = siempre, 1 = condicional, 2 = nunca
			int late, a, b;
			if (is_branch(dop) || ends_block(dop)) {
				if (tot == 1) { emit_ptr = start; cold_ptr = cold_start; return NULL; }   // que lo haga el intérprete
				flush_regs();
				set_pc_const(cur);
				exit_block(n - 1);
				break;
			}
			op_regs(dop, &dr, &dw);
			switch (o) {
			case 0x00:   // JR, JALR
				dynamic = 1;
				if ((op & 0x3f) == 0x09) link_reg = (int)rd;
				break;
			case 0x01:   // BLTZ, BGEZ, BLTZAL, BGEZAL
				if ((rt & 0x1e) == 0x10) link_reg = 31;
				cond_kind = rs ? 1 : (rt & 1) ? 0 : 2;
				break;
			case 0x02: case 0x03:   // J, JAL
				target = (cur & 0xf0000000) | ((op & 0x3ffffff) << 2);
				if (o == 0x03) link_reg = 31;
				break;
			case 0x04: case 0x05:   // BEQ, BNE
				cond_kind = rs != rt ? 1 : o == 0x04 ? 0 : 2;
				break;
			case 0x06: case 0x07:   // BLEZ, BGTZ
				cond_kind = rs ? 1 : o == 0x06 ? 0 : 2;
				break;
			}
			// los operandos se leen antes del delay slot: si este (o el enlace) los pisa, se guardan antes
			operands = (1u << rs) | (o == 0x04 || o == 0x05 ? 1u << rt : 0);
			clobbered = dw | (link_reg ? 1u << link_reg : 0);
			late = !(operands & clobbered & ~1u);
			if (!late) {
				if (dynamic) a_str(get(rs, R0), RCPU, off_tmp);
				else if (cond_kind == 1) {
					int c;
					branch_operands(op, &a, &b, R0, R1);
					c = branch_cond(op, a, b);
					a_dp_imm(c, OP_MOV, 0, R0, 0, 1);
					a_dp_imm(c ^ 1, OP_MOV, 0, R0, 0, 0);
					a_str(R0, RCPU, off_tmp);
				}
			}
			if (link_reg) put_const(link_reg, cur + 8);
			n++;
			tot++;
			if (dop) emit_op(dop, cur + 4, n);
			flush_regs();
			if (dynamic) {   // JR/JALR: buscar el bloque con el despachador
				int t = R2;
				if (late) t = get(rs, R2);
				else a_ldr(R2, RCPU, off_tmp);
				emit_cycles(n);
				a_str(t, RCPU, OFF_PC);
				a_mov(R0, t);
				a_b(C_MI, 0, stub_dispatch);
				a_b(C_AL, 0, stub_exit);
			} else if (cond_kind == 2) exit_to(cur + 8, n);
			else if (cond_kind == 0 && !(!link_reg && target == pc && pure_loop(pc, cur))) exit_to(target, n);
			else if (cond_kind == 0) {   // «j .»: esperar a una interrupción
				u32 *out;
				emit_cycles(n);
				out = fwd(C_PL);
				emit_idle(target);
				begin_cold();
				land(out);
				exit_pc(target);
				end_cold();
			} else {
				u32 *out, *not_taken, *taken_exit;
				int idle = (target == pc || psyq_vsync(target) || vsync_poll_call(target)) && pure_loop(pc, cur), c;
				// operandos listos antes de mirar el tiempo: los dos caminos los usan
				if (late) branch_operands(op, &a, &b, R2, R3);
				else { a_ldr(R2, RCPU, off_tmp); a = R2; b = -1; }
				emit_cycles(n);
				out = fwd(C_PL);
				if (late) c = branch_cond(op, a, b);
				else { a_dp_imm(C_AL, OP_CMP, 1, 0, R2, 0); c = C_NE; }
				not_taken = fwd(c ^ 1);
				if (idle) emit_idle(target);
				else link_site(target);
				land(not_taken);
				goon = !idle && sb_cont(op, cur, pc) && tot + 2 < SB_MAX && !has_block(cur + 8);
				if (!goon) link_site(cur + 8);   // si no, el bloque sigue aquí mismo (superbloque)
				begin_cold();   // sin tiempo: volver a C por el camino que toque
				land(out);
				if (late) c = branch_cond(op, a, b);
				else { a_dp_imm(C_AL, OP_CMP, 1, 0, R2, 0); c = C_NE; }
				taken_exit = fwd(c);
				exit_pc(cur + 8);
				land(taken_exit);
				exit_pc(target);
				end_cold();
			}
			if (goon) {   // seguir tras el delay slot; los ciclos de lo anterior ya están sumados
				pv_after(op);
				rng_after(op);
				pv_after(dop);
				rng_after(dop);
				cur += 8;
				n = 0;
				continue;
			}
			cur += 4;
			break;
		}
		if (op && !emit_op(op, cur, n)) break;
		pv_after(op);
		rng_after(op);
		if (tot >= MAX_BLOCK) {
			exit_to(cur + 4, n);
			break;
		}
		cur += 4;
	}

	if (use_hsp) {   // salida de los fallos de la comprobación de entrada
		begin_cold();
		land(sp_fail[0]);
		land(sp_fail[1]);
		a_b(C_AL, 1, stub_sp_fail);
		emit(pc);
		end_cold();
	}
	if (scr_reg) {
		begin_cold();
		land(scr_fail);
		a_b(C_AL, 1, stub_scr_fail);
		emit(pc);
		end_cold();
	}
	mark_pages(pc, cur);
	STAT_ADD(dyn_mips, (cur - pc) / 4 + 1);
	STAT_ADD(dyn_arm, (u32)(emit_ptr - start));   // solo la zona caliente
	sync_code(start, (u32)((u8 *)emit_ptr - (u8 *)start));
	if (cold_ptr != cold_start) sync_code(cold_start, (u32)((u8 *)cold_ptr - (u8 *)cold_start));
	if (blk_n < blk_max) { blk_arm[blk_n] = (u32)(uintptr_t)start; blk_mips[blk_n++] = pc; }
	*s = entry;
	return entry;
}

// Lo llama stub_sp_fail: el bloque de pc supuso que sp apuntaba a la RAM y no era así
void dyn_sp_fail(u32 pc)
{
	block_fn *s = slot(pc);
	u32 l = lin(pc), i;
	if (sp_unsafe_n < 256) {
		for (i = (pc >> 2) & 255; sp_unsafe[i] && sp_unsafe[i] != pc; i = (i + 1) & 255) { }
		if (!sp_unsafe[i]) { sp_unsafe[i] = pc; sp_unsafe_n++; }
	}
	if (s && *s && (*s)[-1] == pc) *s = NULL;
	if (l < 0x200000) unlink_page(l >> DYN_PAGE_BITS, l, l + 4);   // los saltos ya parcheados hacia él, también
}

// Lo llama stub_scr_fail: la base de la scratchpad del bloque de pc no valía lo que se supuso
void dyn_scr_fail(u32 pc)
{
	block_fn *s = slot(pc);
	u32 l = lin(pc), i;
	if (scr_unsafe_n < 256) {
		for (i = (pc >> 2) & 255; scr_unsafe[i] && scr_unsafe[i] != pc; i = (i + 1) & 255) { }
		if (!scr_unsafe[i]) { scr_unsafe[i] = pc; scr_unsafe_n++; }
	}
	if (s && *s && (*s)[-1] == pc) *s = NULL;
	if (l < 0x200000) unlink_page(l >> DYN_PAGE_BITS, l, l + 4);
}

// Lo llama el enlazador (stub_link) desde el final de un bloque: busca o traduce el destino y, si
// se puede, cambia el BL de ese final por un salto directo. NULL = volver a C con cpu.pc = target.
block_fn dyn_link(u32 target, u32 *site)
{
	block_fn *s, f;
	u32 g0 = gen, l;
	cpu.pc = target;
	l = lin(target);
	if (!dyn_chain || l == ~0u || (l < 0x100 && dyn_chain < 2)) return NULL;   // ver stub_dispatch
	s = &slots[(l >> 2) & slot_mask];
	f = *s;
	if (!f || f[-1] != target) {
		{ PROF_BEGIN(); f = compile(target, s); PROF_END(PROF_COMPILE); }
		STAT_ADD(dyn_compiled, 1);
		if (!f) return NULL;
	}
	if (gen != g0) return f;   // se vació la caché al traducir: el sitio ya no existe
	if (l < 0x200000) {        // destino en RAM: apuntar el enlace por si se reescribe ese código
		if (link_n >= link_max) return f;
		links[link_n].site = site;
		links[link_n].lin = l;
		links[link_n].next = link_head[l >> DYN_PAGE_BITS];
		link_head[l >> DYN_PAGE_BITS] = link_n++;
	}
	*site = b_enc(C_AL, 0, site, f);
	sync_code(site, 4);
	return f;
}

u32 dyn_where;   // depuración: 1 = despacho, 2 = traducir, 3 = paso interpretado, 4 = bloque, 0 = fuera

void dyn_run(u32 until)
{
	psx_run_until = until;
	dyn_chain = psx_hook_pc ? 0 : psx_tty ? 1 : 2;
#ifdef DYN_NOCHAIN
	dyn_chain = DYN_NOCHAIN;   // pruebas: 0 = volver a C tras cada bloque, 1 = sin vectores de la BIOS
#endif
	while (TIME_BEFORE(psx_cycles, psx_run_until)) {
		block_fn *s, f;
		if (psx_hook_pc && cpu.pc == psx_hook_pc) psx_hook();
#ifdef PSX_TTY
		if (psx_tty) {   // putchar de la BIOS (A0h:3Ch / B0h:3Dh)
			u32 p = cpu.pc & 0x1fffffff;
			if ((p == 0xa0 && cpu.r[9] == 0x3c) || (p == 0xb0 && cpu.r[9] == 0x3d)) psx_tty(cpu.r[4] & 0xff);
		}
#endif
		if (cpu_dyn_irq()) { psx_cycles += 2; continue; }
		// con la caché aislada (la BIOS la vacía así) las escrituras no deben llegar a la RAM
		if (cpu.cop0[12] & (1 << 16)) { cpu_interp_step(); continue; }
		s = slot(cpu.pc);
		dyn_where = 1;
		if (!s) { dyn_where = 3; cpu_interp_step(); continue; }
		f = *s;
		if (!f || f[-1] != cpu.pc) {
			dyn_where = 2;
			{ PROF_BEGIN(); f = compile(cpu.pc, s); PROF_END(PROF_COMPILE); }
			STAT_ADD(dyn_compiled, 1);
			if (!f) { dyn_where = 3; cpu_interp_step(); continue; }
		}
		dyn_where = 4;
		dyn_enter(&cpu, psx_ram, dyn_pages, f);   // corre bloques encadenados hasta psx_run_until
	}
	dyn_where = 0;
}

// Para el perfilador: inicio de la caché, trampolines, rutinas de memoria, bloques, zona fría y final
void dyn_regions(u32 r[6])
{
	r[0] = (u32)(uintptr_t)code_buf;
	r[1] = (u32)(uintptr_t)tramp_at[0];
	r[2] = (u32)(uintptr_t)smc_handler;
	r[3] = (u32)(uintptr_t)code_start;
	r[4] = (u32)(uintptr_t)cold_base;
	r[5] = (u32)(uintptr_t)code_end;
}

// PC MIPS del bloque que contiene la dirección ARM a (0 si no es de un bloque de la zona caliente)
u32 dyn_block_of(u32 a)
{
	u32 lo = 0, hi = blk_n;
	if (!blk_n || a < blk_arm[0] || a >= (u32)(uintptr_t)emit_ptr) return 0;
	while (hi - lo > 1) {
		u32 mid = (lo + hi) / 2;
		if (blk_arm[mid] <= a) lo = mid; else hi = mid;
	}
	return blk_mips[lo];
}
#endif
