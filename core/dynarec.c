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

#ifndef MAX_BLOCK
#define MAX_BLOCK 64
#endif
#ifndef DYN_DS_SLOTS
#define DYN_DS_SLOTS 0x2000   // huecos de la tabla de bloques en el modo DS (potencia de 2; el banco de pruebas usa más)
#endif
#ifndef DYN_DS_RESERVE_KB
#define DYN_DS_RESERVE_KB 704   // modo DS: memoria que se deja fuera de la caché de código (los vídeos necesitan ~500 KB)
#endif

typedef u32 *block_fn;   // primera instrucción del bloque; la palabra anterior es el PC traducido

u32 *emit_ptr;
u8 dyn_pages[DYN_NPAGES];
// Y palabra a palabra: un bit por instrucción traducida. Crash Bandicoot tiene un contador (80034520) entre dos funciones
// del mismo KB y lo sube en cada imagen: con solo el rango lo/hi, cada escritura borraba y retraducía todo ese KB.
#define CODE_BITS_BYTES (0x200000 / 4 / 8)
static u32 *code_bits;   // en el montón (NULL sin memoria: todo cuenta como código, como antes)
static inline int is_code(u32 a) { return !code_bits || (code_bits[a >> 7] >> ((a >> 2) & 31) & 1); }
#ifdef PSX_SMCLOG
struct { char magic[8]; u32 n, e[4096]; } dyn_smc_log = { "SPSMCLOG" };
struct { char magic[8]; u32 n, e[8192]; } dyn_cmp_log = { "SPCMPLOG" };
extern u32 psx_frame_no;
#endif
static u32 *code_buf, *code_end, *code_start, code_size;
static u32 *cold_base, *cold_ptr, *hot_save;   // zona fría: [cold_base, code_end)
// Zona caliente (stubs y bloques): [code_buf, hot_end). Normalmente los 3/4 primeros del mismo búfer que la fría;
// con dyn_hot_area, en otra memoria (la NWRAM de la DSi) y todo el búfer de antes queda de zona fría.
static u32 *hot_end;
static int hot_apart;
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
// Ciclos de n instrucciones con la CPU a psx_cpu_scale: el código traducido los lee de aquí (r11 + off_ctab),
// así la velocidad de la CPU se cambia en marcha (modo automático) sin volver a traducir nada
#define CYC_TAB 72
static PSX_FAST u32 cyc_tab[CYC_TAB];
static int off_ctab = -1;

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
void dyn_grp_fail(u32 pc);
static const void *const tramp_fn[] = {
	(const void *)cpu_dyn_interp, (const void *)cpu_dyn_exception, (const void *)gte_command, (const void *)gte_read,
	(const void *)gte_write, (const void *)h_div, (const void *)h_divu, (const void *)dyn_smc,
	(const void *)h_r32, (const void *)h_r16s, (const void *)h_r16u, (const void *)h_r8s, (const void *)h_r8u,
	(const void *)h_w32, (const void *)h_w16, (const void *)h_w8, (const void *)gpu_stat, (const void *)gpu_write_gp0,
	(const void *)dyn_grp_fail,
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
	if (!hot_apart) hot_end = cold_base = code_start + (code_end - code_start) * 3 / 4;
	cold_ptr = cold_base;
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
	if (!code_bits) code_bits = calloc(1, CODE_BITS_BYTES);
	links = malloc(link_max * sizeof(Link));
	if (big) {
		code_size = 3 * 1024 * 1024;
		code_buf = malloc(code_size);
		while (!code_buf && code_size > 1024 * 1024) code_buf = malloc(code_size -= 512 * 1024);
#if defined(SPAR_PROF) || defined(PSX_BLKTAB)
		blk_max = 65536;   // solo para el perfilador (qué bloque MIPS es cada dirección ARM): 512 KB
		blk_arm = malloc(blk_max * 4);
		blk_mips = malloc(blk_max * 4);
		if (!blk_arm || !blk_mips) blk_max = 0;
#endif
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
	if (!near_cpu(cyc_tab, CYC_TAB * 4, &off_ctab)) off_ctab = -1;
	dyn_set_cpu_scale(psx_cpu_scale);
	memset(link_head, 0xff, sizeof link_head);
	emit_stubs();
	return 1;
}

u32 *dyn_code_base(void) { return code_buf; }

static void flush_all(void);

// La zona caliente pasa a [p, p + size) (en la DSi, la NWRAM: ver main.c) y el búfer de antes entero queda de zona
// fría (salidas por fin de tiempo, excepciones...: se ejecutan poco). Antes de que se traduzca nada del juego.
// Los saltos B/BL llegan a ±32 MB: de la NWRAM (037xxxxxh) a la RAM principal y a la ITCM hay menos.
void dyn_hot_area(void *p, u32 size)
{
	cold_base = code_buf;
	if ((u32 *)p > code_buf && (u32 *)p < code_end) code_end = p;   // dentro del búfer de antes (pruebas): la fría acaba ahí
	code_buf = p;
	hot_end = code_buf + size / 4;
	hot_apart = 1;
	emit_stubs();
	flush_all();
}

void dyn_set_cpu_scale(u32 scale)
{
	if (scale == psx_cpu_scale && cyc_tab[1]) return;
	psx_cpu_scale = (u16)scale;
	for (int n = 0; n < CYC_TAB; n++) {
		u32 c = ((u32)n * 2 * scale) >> 8;
		cyc_tab[n] = c ? c : 1;
	}
	if (off_ctab < 0 && code_buf) flush_all();   // sin tabla: los ciclos van dentro del código
}


void dyn_cache_use(u32 *used, u32 *size)
{
	if (hot_apart) {   // la fría (el búfer grande de antes) no se llena: manda la caliente
		*used = (u32)(emit_ptr - code_buf) * 4;
		*size = (u32)(hot_end - code_buf) * 4;
		return;
	}
	*used = (u32)(emit_ptr - code_buf + cold_ptr - cold_base) * 4;
	*size = code_size;
}

static void flush_all(void)
{
#ifdef PSX_SMCLOG
	if (dyn_cmp_log.n < 8192) dyn_cmp_log.e[dyn_cmp_log.n++] = 0xfa000000 | (u32)(emit_ptr - code_buf);
#endif
	STAT_ADD(dyn_flushes, 1);
	memset(slots, 0, (slot_mask + 1) * sizeof(block_fn));
	memset(dyn_pages, 0, sizeof dyn_pages);
	if (code_bits) memset(code_bits, 0, CODE_BITS_BYTES);
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

// Parte de cada página que ocupa el código traducido [lo, hi): los juegos guardan datos junto al código
// (Silent Hill monta paquetes de la GPU en el mismo KB que su bucle principal) y escribirlos no lo borra.
static u16 code_lo[DYN_NPAGES], code_hi[DYN_NPAGES];

static void smc_page(u32 page, u32 off)
{
	dyn_pages[page] = 0;
	if (code_bits)
		for (u32 w = (page << DYN_PAGE_BITS) >> 7; w < ((page + 1) << DYN_PAGE_BITS) >> 7; w++) code_bits[w] = 0;
	STAT_ADD(dyn_smc, 1);
#ifdef PSX_SMCLOG
	if (dyn_smc_log.n < 4096) dyn_smc_log.e[dyn_smc_log.n++] = (off & 0x1fffff) | (psx_frame_no << 21);
#endif
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

void dyn_smc(u32 off)
{
	u32 page = (off >> DYN_PAGE_BITS) & (DYN_NPAGES - 1), o = off & ((1u << DYN_PAGE_BITS) - 1);
#ifdef DYN_SMC_PAGE
	if (!dyn_pages[page]) return;
#else
#ifdef PSX_SMCLOG
	if (dyn_pages[page] && (o + 4 <= code_lo[page] || o >= code_hi[page])) {   // descartada: ¿había un bloque que la cubra?
		u32 first = page << DYN_PAGE_BITS;
		for (u32 a = first >= 256 ? first - 256 : 0; a < first + (1u << DYN_PAGE_BITS); a += 4) {
			block_fn *s = &slots[(a >> 2) & slot_mask];
			if (*s && lin((*s)[-1]) == a && a <= (off & 0x1fffff) && (off & 0x1fffff) < a + 256 && dyn_cmp_log.n < 8189) {
				dyn_cmp_log.e[dyn_cmp_log.n++] = 0xbad00000 | (psx_frame_no & 0xfffff);
				dyn_cmp_log.e[dyn_cmp_log.n++] = off & 0x1fffff;
				dyn_cmp_log.e[dyn_cmp_log.n++] = a | (u32)code_lo[page] << 21;
				break;
			}
		}
	}
#endif
	if (!dyn_pages[page] || o + 4 <= code_lo[page] || o >= code_hi[page]) return;
#endif
	if (!is_code(off & 0x1ffffc)) return;   // dato entre funciones: el código no cambió
	smc_page(page, off);
}

void dyn_ram_written(u32 off, u32 len)
{
	u32 end = off + len;
	if (end > 0x200000) end = 0x200000;
	for (u32 a = off; a < end; a = (a | ((1u << DYN_PAGE_BITS) - 1)) + 1) {
		u32 page = a >> DYN_PAGE_BITS, base = page << DYN_PAGE_BITS;
		u32 lo = a - base, hi = (end - base < (1u << DYN_PAGE_BITS) ? end - base : 1u << DYN_PAGE_BITS);
		if (dyn_pages[page] && lo < code_hi[page] && hi > code_lo[page]) {
			u32 b;
			for (b = (base + lo) & ~3u; b < base + hi && !is_code(b); b += 4) { }
			if (b < base + hi) smc_page(page, a);
		}
	}
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
	else {
#ifdef ARM9
		// divisor por hardware de la DS (32/32 con signo): más rápido que la división por software
		REG_DIVCNT = DIV_32_32;
		REG_DIV_NUMER_L = n;
		REG_DIV_DENOM_L = d;
		while (REG_DIVCNT & DIV_BUSY) { }
		cpu.lo = (u32)REG_DIV_RESULT_L;
		cpu.hi = (u32)REG_DIVREM_RESULT_L;
#else
		cpu.lo = (u32)(n / d);
		cpu.hi = (u32)(n % d);
#endif
	}
}

static void h_divu(u32 n, u32 d)
{
	if (d == 0) { cpu.hi = n; cpu.lo = 0xffffffff; }
#ifdef ARM9
	else if ((s32)n >= 0 && (s32)d > 0) {   // sin el bit alto: vale la división con signo del hardware
		REG_DIVCNT = DIV_32_32;
		REG_DIV_NUMER_L = (s32)n;
		REG_DIV_DENOM_L = (s32)d;
		while (REG_DIVCNT & DIV_BUSY) { }
		cpu.lo = (u32)REG_DIV_RESULT_L;
		cpu.hi = (u32)REG_DIVREM_RESULT_L;
	}
#endif
	else { cpu.lo = n / d; cpu.hi = n % d; }
}

// ---- registros MIPS durante la traducción ----
// host: registro ARM asignado (o -1); valid: el registro ARM tiene el valor; stale: la memoria (cpu.r)
// no lo tiene; isconst: el valor se conoce al traducir (k)
static struct { s8 host; u8 valid, stale, isconst; u32 k; } rg[32];
static const int host_pool[5] = { R4, R5, R6, R7, R8 };
#ifndef DYN_POOL
#define DYN_POOL 5
#endif
static u32 stale_mask;   // registros con stale = 1

// Reparto de los registros ARM instrucción a instrucción (con -DDYN_STATIC_RA, el de antes: los 5 más usados del bloque
// para todo el bloque). Antes de traducir cada instrucción, los registros MIPS que usa toman un registro ARM si se van a
// volver a usar; si no queda sitio, sale el que más tarde se vuelve a usar (guardándolo si había cambiado). Así los
// temporales que TR usa en tramos cortos (gp, ra, at, t3...) no van y vienen de la memoria en cada instrucción.
#define RA_MAX (MAX_BLOCK + 4)
#define RA_NEVER 10000
static u32 ra_use[RA_MAX];   // registros que usa cada instrucción del bloque (por posición desde el PC inicial)
static int ra_n, ra_frozen;  // ra_frozen: dentro de un si-entonces el reparto no cambia (los dos caminos lo ven igual)
static s8 host_of[16];       // registro MIPS en cada registro ARM del reparto (-1: libre)
static u8 ra_pool[5], ra_npool;

static u8 ra_nu[RA_MAX + 1][32];   // próxima posición (>= p) en la que se usa cada registro (255: ninguna); ver ra_index
static int ra_next(int mr, int pos)
{
	u32 q = pos < ra_n ? ra_nu[pos][mr] : 255;
	return q == 255 ? RA_NEVER : (int)q;
}

// La tabla de próximos usos del bloque, de atrás adelante (una vez por bloque: ra_need la mira en cada instrucción)
static void ra_index(void)
{
	memset(ra_nu[ra_n], 255, 32);
	for (int p = ra_n - 1; p >= 0; p--) {
		memcpy(ra_nu[p], ra_nu[p + 1], 32);
		for (u32 m = ra_use[p]; m; m &= m - 1) ra_nu[p][__builtin_ctz(m)] = (u8)p;
	}
}

static void ra_evict(int h)
{
	int x = host_of[h];
	if (rg[x].stale) {
		if (rg[x].valid) a_str(h, RCPU, OFF_R(x));
		else {
			a_movi(R0, rg[x].k);
			a_str(R0, RCPU, OFF_R(x));
		}
		rg[x].stale = 0;
		stale_mask &= ~(1u << x);
	}
	rg[x].host = -1;
	rg[x].valid = 0;
	host_of[h] = -1;
}

// Antes de lo que use los registros need en la posición pos (no emite nada salvo guardar a los que salen; usa r0)
static void ra_need(u32 need, int pos)
{
#ifdef DYN_STATIC_RA
	return;
#endif
	if (ra_frozen) return;
	need &= ~1u;
	for (u32 m = need; m; m &= m - 1) {
		int i = __builtin_ctz(m), best = -1, bdist = -1, mine;
		if (rg[i].host >= 0) continue;
		mine = ra_next(i, pos + 1);
		for (int k = 0; k < ra_npool; k++) {
			int h = ra_pool[k], x = host_of[h], d;
			if (x < 0) { best = h; bdist = RA_NEVER + 1; break; }   // libre
			if (need & (1u << x)) continue;                          // lo usa esta misma instrucción
			d = ra_next(x, pos);
			if (d > bdist || (d == bdist && !rg[x].stale)) { best = h; bdist = d; }
		}
		if (best < 0 || bdist <= mine) continue;   // los que hay se usan antes que este: que vaya por un temporal
		if (host_of[best] >= 0) ra_evict(best);
		host_of[best] = (s8)i;
		rg[i].host = (s8)best;
		rg[i].valid = 0;
	}
}

static u32 blk_pc;   // PC con el que empieza el bloque que se traduce (las posiciones del reparto son desde aquí)
static int cur_pos, in_dslot;   // posición de la instrucción que se traduce; si es un delay slot
static void op_regs(u32 op, u32 *r, u32 *w);
static void ra_op(u32 op, u32 at)
{
	u32 r, w;
	cur_pos = (int)((at - blk_pc) >> 2);
	op_regs(op, &r, &w);
	ra_need(r | w, cur_pos);
}

// Grupos: accesos seguidos a la memoria por la misma base sin que esta cambie entre medias (TR monta cada primitiva
// con 13 escrituras por a3). Al llegar al primero se comprueba una vez que la base es de la RAM, que todo el grupo cae
// dentro de sus 2 MB y, si escribe, que en esas páginas no hay código traducido; la dirección real queda en un
// registro ARM apartado (grp[g].reg) y cada acceso del grupo es una sola instrucción. Si la comprobación falla, se sale
// del bloque en esa instrucción y el bloque se traduce otra vez sin grupos (como el atajo de la pila).
#define GRP_MAX 8
#ifndef GRP_MIN
#define GRP_MIN 4   // ahorro mínimo (instrucciones) para que un grupo valga el registro apartado
#endif
#ifndef GRP_REGS
#define GRP_REGS 2   // registros ARM para grupos (dos grupos que se solapan, cada uno el suyo)
#endif
static struct { s8 base, reg; u8 nld, nst, live, dead; s16 first, last; s32 lo, hi, slo, shi; } grp[GRP_MAX];
static int grp_n, grp_rmask;   // registros ARM apartados para los grupos del bloque
static s8 grp_at[RA_MAX];   // grupo de cada posición (-1: ninguno)
static s16 grp_adj[RA_MAX];  // lo que se ha movido la base (addiu base, base, n dentro del grupo) al llegar a ese acceso
static u32 ra_wr[RA_MAX];   // registros que escribe cada instrucción del bloque
static u32 grp_unsafe[256];
static int grp_unsafe_n;

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

// Lo mismo sin darlo por guardado: para un camino que sale del bloque mientras el otro sigue con los registros
static void flush_keep(void)
{
	for (u32 m = stale_mask; m; m &= m - 1) {
		int i = __builtin_ctz(m);
		if (rg[i].valid) a_str(rg[i].host, RCPU, OFF_R(i));
		else {
			a_movi(R0, rg[i].k);
			a_str(R0, RCPU, OFF_R(i));
		}
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
	u32 c;
	if (off_ctab >= 0 && ninstr < CYC_TAB) {
		a_ldr(R1, RCPU, off_ctab + ninstr * 4);   // r1 está libre en todas las salidas
		a_dp_reg(C_AL, OP_ADD, 1, RCYC, RCYC, R1, SH_LSL, 0);
		return;
	}
	c = ((u32)ninstr * 2 * psx_cpu_scale) >> 8;
	if (!c) c = 1;
	if (c > 255) { a_dp_imm(C_AL, OP_ADD, 0, RCYC, RCYC, arm_imm(c & ~255u)); c &= 255; }   // CPU lenta: no cabe en un inmediato
	if (c) a_dp_imm(C_AL, OP_ADD, 1, RCYC, RCYC, arm_imm(c));
	else a_dp_imm(C_AL, OP_CMP, 1, 0, RCYC, 0);   // los flags (MI si queda tiempo) sin sumar nada
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
// Las mismas con la dirección en un registro asignado (r4-r8) en vez de r0: «mov r0, rb» y a la de siempre. Así un
// acceso sin desplazamiento por un registro ya cargado no copia antes la dirección a r0.
static u32 *ldb_handler[5][16][9], *stb_handler[5][16][9];
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
			if (sz == M_HS || sz == M_HU || sz == M_BS) {
				// en línea solo van los primeros 2 MB de cada segmento: los otros espejos de la RAM (TR tiene la pila en
				// 807Fxxxx) se leen aquí, sin ir a C (después de la scratchpad, que es lo más frecuente por aquí)
				static const int hsh[5] = { 0, 3, 1, 2, 0 };
				a_dp_imm(C_AL, OP_TST, 1, 0, R0, arm_imm(0x1f800000));
				a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, R0, SH_LSL, 11);
				a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, R1, SH_LSR, 11);
				a_memh_reg(C_EQ, 1, hsh[sz], r, RRAM, R1);
				a_bx_cond(C_EQ, LR);
			}
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
	for (int sz = 0; sz < 5; sz++)
		for (int i = 0; i < 6; i++)
			for (int b = R4; b <= R8; b++) {
				int r = mem_regs[i];
				ldb_handler[sz][r][b] = emit_ptr;
				a_mov(R0, b);
				a_b(C_AL, 0, ld_handler[sz][r]);
				if (sz == M_HS || sz == M_BS) continue;
				stb_handler[sz][r][b] = emit_ptr;
				a_mov(R0, b);
				a_b(C_AL, 0, st_handler[sz][r]);
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
#ifdef DYN_NOSCR
	return 0;
#endif
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

// ---- grupos (ver grp_find) ----
static int cur_ninstr;   // instrucciones del bloque hasta la que se traduce (para salir a mitad)
void dyn_grp_fail(u32 pc);

// Al llegar al primer acceso del grupo g: comprobar la base y dejar su dirección real en grp[g].reg
static void grp_materialize(int g)
{
	u32 *fail[4], k;
	int nf = 0, x = get(grp[g].base, R0);
	a_dp_imm(C_AL, OP_TST, 1, 0, x, arm_imm(0x1f800000));   // de la RAM (en cualquier espejo)
	fail[nf++] = fwd(C_NE);
	a_dp_reg(C_AL, OP_MOV, 0, R1, 0, x, SH_LSL, 11);      // r1 = (base & 1FFFFF) << 11
	if (grp[g].lo < 0) {   // base + lo no baja de 0
		k = (u32)(-grp[g].lo) << 11;
		if (arm_imm(k) >= 0) a_dp_imm(C_AL, OP_CMP, 1, 0, R1, arm_imm(k));
		else { a_movi(R2, k); a_dp_reg(C_AL, OP_CMP, 1, 0, R1, R2, SH_LSL, 0); }
		fail[nf++] = fwd(C_CC);
	}
	if (grp[g].hi > 0) {   // base + hi no pasa de los 2 MB: r1 + (hi << 11) cabe en 32 bits (o da justo 2^32)
		k = (u32)grp[g].hi << 11;
		if (arm_imm(k) >= 0) a_dp_imm(C_AL, OP_CMN, 1, 0, R1, arm_imm(k));
		else { a_movi(R2, k); a_dp_reg(C_AL, OP_CMN, 1, 0, R1, R2, SH_LSL, 0); }
		fail[nf++] = fwd(C_HI);
	}
	a_dp_reg(C_AL, OP_ADD, 0, grp[g].reg, RRAM, R1, SH_LSR, 11);
	if (grp[g].nst) {   // en las páginas que escribe el grupo (dos como mucho) no hay código traducido
		a_addi(R2, R1, (u32)grp[g].slo << 11, R0);
		a_mem_regsh(C_AL, 1, 1, R3, RPAGES, R2, SH_LSR, 11 + DYN_PAGE_BITS);
		a_addi(R2, R1, (u32)(grp[g].shi - 1) << 11, R0);
		a_mem_regsh(C_AL, 1, 1, R2, RPAGES, R2, SH_LSR, 11 + DYN_PAGE_BITS);
		a_dp_reg(C_AL, OP_ORR, 1, R3, R3, R2, SH_LSL, 0);
		fail[nf++] = fwd(C_NE);
	}
	begin_cold();   // no vale: salir antes de esta instrucción y que el bloque se traduzca sin grupos
	for (int i = 0; i < nf; i++) land(fail[i]);
	flush_keep();
	set_pc_const(blk_pc + 4 * (u32)cur_pos);
	emit_cycles(cur_ninstr - 1);
	a_movi(R0, blk_pc);
	call((const void *)dyn_grp_fail);
	a_b(C_AL, 0, stub_exit);
	end_cold();
}

// Acceso del grupo de esta posición: una instrucción sobre su registro. 0 si no va por grupo.
static int scr_derived(int rs, u32 simm, int bytes);
// Antes de leer nada para el acceso de esta posición (la comprobación usa r0-r3: el valor que se va a escribir aún
// no está en r2): si es el primero de su grupo, comprobar la base. Solo en el primero, y no dentro de un si-entonces
// ni en un delay slot (un camino no pasaría por ella), ni si el acceso irá por otro atajo.
static void grp_pre(int rs, u32 simm)
{
	int g = cur_pos >= 0 && cur_pos < RA_MAX ? grp_at[cur_pos] : -1;
	if (g < 0 || grp[g].base != rs || grp[g].dead || grp[g].live) return;
	if (cur_pos != grp[g].first || ra_frozen || in_dslot || is_const(rs) || (rs == scr_reg && scr_ok) ||
	    scr_derived(rs, simm, 1) || scratch_hint(rs, simm)) { grp[g].dead = 1; return; }
	grp_materialize(g);
	grp[g].live = 1;
}

// Acceso del grupo de esta posición: una instrucción sobre su registro. 0 si no va por grupo.
static int grp_use(int load, int size, int rs, u32 simm, int r)
{
	int g = cur_pos >= 0 && cur_pos < RA_MAX ? grp_at[cur_pos] : -1;
	if (g < 0 || grp[g].base != rs || !grp[g].live) return 0;
	mem_off(C_AL, load, size, r, grp[g].reg, (s32)simm + grp_adj[cur_pos]);
	return 1;
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
		else if (size == M_W && a < 0xc0000000 && (a & 0x1fffffff) == 0x1f801814) {   // GPUSTAT: directo, sin el bus
			call(gpu_stat);
			if (d != R0) a_mov(d, R0);
		} else {
			a_movi(R0, a);
			call(rd_fn[size]);
			if (d != R0) a_mov(d, R0);
		}
		return;
	}
	if (grp_use(1, size, rs, simm, d)) return;
	if (scratch_hint(rs, simm)) { scratch_first(1, size, rs, simm, d); return; }
	int x = get(rs, R0), a = R0;
	u32 *const *h = ld_handler[size];
	if (simm == 0 && x >= R4 && x <= R8) { a = x; h = NULL; }   // la dirección ya está en un registro asignado
	else a_addi(R0, x, simm, R1);
	if (size == M_W || size == M_BU) {
		a_dp_imm(C_AL, OP_TST, 1, 0, a, arm_imm(0x1f800000));   // fuera de la RAM (bits 23-28): NE
		a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, a, SH_LSL, 11);
		a_mem_regsh(C_EQ, 1, size == M_BU, d, RRAM, R1, SH_LSR, 11);
		a_b(C_NE, 1, h ? h[d] : ldb_handler[size][d][a]);
	} else {   // LDRH/LDRSH/LDRSB no admiten registro desplazado: los primeros 2 MB de cada segmento en línea
		a_dp_imm(C_AL, OP_BIC, 0, R1, a, arm_imm(0xe0000000));
		a_dp_imm(C_AL, OP_CMP, 1, 0, R1, arm_imm(0x200000));
		a_memh_reg(C_CC, 1, hsh[size], d, RRAM, R1);
		a_b(C_CS, 1, h ? h[d] : ldb_handler[size][d][a]);
	}
}

static void emit_load(int size, int rs, int rt, u32 simm)
{
	grp_pre(rs, simm);
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
		else if (size == M_W && a < 0xc0000000 && (a & 0x1fffffff) == 0x1f801810) {   // GP0 de la GPU: directo, sin el bus
			if (v != R0) a_mov(R0, v);
			call(gpu_write_gp0);
		} else {
			a_movi(R0, a);
			if (v != R1) a_mov(R1, v);
			call(wr_fn[size]);
		}
		return;
	}
	if (grp_use(0, size, rs, simm, v)) return;
	if (scratch_hint(rs, simm)) { scratch_first(0, size, rs, simm, v); return; }
	int x = get(rs, R0), a = R0;
	if (simm == 0 && x >= R4 && x <= R8 && x != v) a = x;   // la dirección ya está en un registro asignado
	else a_addi(R0, x, simm, R1);
	a_dp_imm(C_AL, OP_TST, 1, 0, a, arm_imm(0x1f800000));
	a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, a, SH_LSL, 11);
	if (size == M_W || size == M_BU) a_mem_regsh(C_EQ, 0, size == M_BU, v, RRAM, R1, SH_LSR, 11);
	else {
		a_dp_reg(C_EQ, OP_MOV, 0, R3, 0, R1, SH_LSR, 11);
		a_memh_reg(C_EQ, 0, 1, v, RRAM, R3);
	}
	a_mem_regsh(C_EQ, 1, 1, R3, RPAGES, R1, SH_LSR, 11 + DYN_PAGE_BITS);   // ¿había código traducido en esa página?
	a_dp_imm(C_EQ, OP_CMP, 1, 0, R3, 0);
	a_b(C_NE, 1, a == R0 ? st_handler[size][v] : stb_handler[size][v][a]);   // E/S o scratchpad (NE del TST) o página con código
}

static void emit_store(int size, int rs, int rt, u32 simm)
{
	grp_pre(rs, simm);
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

// Al traducir, toda instrucción leída cuenta como código del bloque (también las que se miran por delante o en el
// destino de un salto para decidir cómo traducir): si cambian, el bloque se tiene que volver a traducir
static int fetch_mark;
static void mark_pages(u32 start, u32 end);

static u32 fetch_op(u32 pc)
{
	u32 p = pc & 0x1fffffff;
	if (fetch_mark && p < 0x800000) {   // lo normal: ya está dentro de lo marcado
		u32 a = p & 0x1fffff, pg = a >> DYN_PAGE_BITS, o = a & ((1u << DYN_PAGE_BITS) - 1);
		if (!dyn_pages[pg] || o < code_lo[pg] || o + 4 > code_hi[pg]) mark_pages(p, p);
	}
	if (p < 0x800000) return *(u32 *)(psx_ram + (p & 0x1ffffc));
	return *(u32 *)(psx_bios + (p & 0x7fffc));
}

// ¿Carga o escritura (de las que van en línea) con base sp?
static int sp_mem_op(u32 op)
{
	u32 o = op >> 26;
	return ((op >> 21) & 31) == 29 && (o == 0x20 || o == 0x21 || o == 0x23 || o == 0x24 || o == 0x25 || o == 0x28 || o == 0x29 || o == 0x2b);
}

// Bytes [hsp_lo, hsp_hi) respecto a sp a la entrada que toca el bloque por el atajo de la pila: con la pila en la
// scratchpad, la entrada comprueba que todo cae dentro de sus 1024 bytes. Los «addiu sp, sp, n» mueven sp y el
// atajo con él; si sp cambia de otra forma, lo que sigue ya no va por el atajo y no cuenta.
static s32 hsp_lo, hsp_hi;
static void sp_range(u32 op, s32 *delta, int *track)
{
	u32 o = op >> 26, r, w;
	if (!*track) return;
	if (sp_mem_op(op)) {
		s32 off = *delta + (s32)(s16)(op & 0xffff);
		int size = (o == 0x23 || o == 0x2b) ? 4 : (o == 0x21 || o == 0x25 || o == 0x29) ? 2 : 1;
		if (off < hsp_lo) hsp_lo = off;
		if (off + size > hsp_hi) hsp_hi = off + size;
	}
	op_regs(op, &r, &w);
	if (w & (1u << 29)) {
		if (o == 0x09 && ((op >> 21) & 31) == 29 && ((op >> 16) & 31) == 29) *delta += (s32)(s16)(op & 0xffff);
		else *track = 0;
	}
}

// Elige qué registros MIPS van en registros ARM: los más usados del bloque (al menos dos veces)
static int psyq_vsync(u32 pc);
static int vsync_poll_call(u32 pc);

// Superbloques: un salto condicional sin enlace que no sea de un bucle de espera no corta el bloque; se sigue
// traduciendo por el camino de no saltar (el de saltar sale enlazado como siempre)
// Límite del superbloque y no seguir por código que ya tiene su propio bloque: cada destino de salto empieza
// otro superbloque, y sin esto el mismo camino se traduciría muchas veces (la caché se llenaba)
#ifndef SB_MAX
#define SB_MAX 32
#endif
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
#ifdef DYN_NOSB
	cond = 0;
#endif
	if (!cond) return 0;
	return target != pc && !psyq_vsync(target) && !vsync_poll_call(target);
}

// Si-entonces corto: un salto condicional hacia delante que solo se salta 1-4 instrucciones sencillas («si a < b, a = b»;
// el motor de dibujo de Tomb Raider tiene docenas) no corta el bloque: se salta con un salto ARM dentro del mismo
// bloque. Si cortara, cada vez que se toma habría que volcar registros, contar ciclos y seguir en otro bloque que
// repite lo de después. Lo de dentro solo puede ser ALU, cargas y escrituras (nada que vuelque registros ni los
// olvide), sin tocar sp ni la base de la scratchpad (strict = 0: aún no se sabe cuál es; alloc_regs acepta de más).
#ifndef HK_MAX
#define HK_MAX 4
#endif
static int hk_safe(u32 op, int strict)
{
	u32 o = op >> 26, f = op & 0x3f, r, w;
	if (o == 0) {
		if (!((f <= 0x07 && f != 0x01 && f != 0x05) || (f >= 0x20 && f <= 0x27) || f == 0x2a || f == 0x2b)) return 0;
	} else if (!((o >= 0x08 && o <= 0x0f) || o == 0x20 || o == 0x21 || o == 0x23 || o == 0x24 || o == 0x25 ||
	             o == 0x28 || o == 0x29 || o == 0x2b)) return 0;
	op_regs(op, &r, &w);
	return !(w & (1u << 29)) && !(strict && scr_reg && (w & (1u << scr_reg)));
}

// Instrucciones que se salta el salto en cur (0 si no es un si-entonces); count = instrucciones del bloque con él
static int hammock_len(u32 op, u32 cur, int count, int strict)
{
	u32 o = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31;
	u32 target = cur + 4 + ((u32)(s32)(s16)(op & 0xffff) << 2);
	s32 k = (s32)(target - cur - 8) >> 2;
	int cond = (o == 0x01 && rs && (rt & 0x1e) != 0x10) || ((o == 0x04 || o == 0x05) && rs != rt) || ((o == 0x06 || o == 0x07) && rs);
#ifdef DYN_NOHK
	return 0;
#endif
	if (!cond || k < 1 || k > HK_MAX || count + 2 + k > MAX_BLOCK) return 0;
	for (s32 i = 0; i < k; i++) if (!hk_safe(fetch_op(cur + 8 + 4 * (u32)i), strict)) return 0;
	if (psyq_vsync(target) || vsync_poll_call(target)) return 0;
	return (int)k;
}

// Una constante que solo se conocía al traducir, a su sitio (registro ARM o memoria): los dos caminos del si-entonces
// tienen que llegar al final igual
static void hk_unconst(int i)
{
	if (!rg[i].isconst) return;
	if (rg[i].host >= 0) {
		if (!rg[i].valid) { a_movi(rg[i].host, rg[i].k); rg[i].valid = 1; }
	} else {
		a_movi(R0, rg[i].k);
		a_str(R0, RCPU, OFF_R(i));
		rg[i].stale = 0;
		stale_mask &= ~(1u << i);
	}
	rg[i].isconst = 0;
}

// Antes de saltar: cargados los registros ARM que se usan dentro y fuera de las constantes los que se cambian dentro.
// Devuelve los registros MIPS que cambian dentro.
static u32 hk_prepare(u32 from, int k)
{
	u32 rm = 0, wm = 0, r, w;
	for (int i = 0; i < k; i++) { op_regs(fetch_op(from + 4 * (u32)i), &r, &w); rm |= r; wm |= w; }
	ra_need(rm | wm, (int)((from - blk_pc) >> 2));   // registros ARM para lo de dentro antes de saltar, y quietos hasta el final
	ra_frozen = 1;
	for (u32 m = (rm | wm) & ~1u; m; m &= m - 1) {
		int i = __builtin_ctz(m);
		if (rg[i].host >= 0 && !rg[i].valid) get(i, R0);
	}
	for (u32 m = wm & ~1u; m; m &= m - 1) hk_unconst(__builtin_ctz(m));
	return wm & ~1u;
}

// Grupos del bloque (ver grp_materialize): por cada registro base, los accesos seguidos hasta que algo lo escribe (un
// «addiu base, base, n» no lo corta: se apunta lo movido). Los que más ahorran, cada uno en un registro ARM apartado.
static int grp_size(u32 o) { return o == 0x20 || o == 0x24 || o == 0x28 ? 1 : o == 0x21 || o == 0x25 || o == 0x29 ? 2 : 4; }
static void grp_find(u32 pc)
{
	s8 open[32];
	int val[GRP_MAX], take[GRP_MAX] = { 0 }, any = 0;
	memset(open, -1, sizeof open);
	memset(grp_at, -1, sizeof grp_at);
	grp_n = 0;
	grp_rmask = 0;
#ifdef DYN_NOGRP
	return;
#endif
	if (pc_in(grp_unsafe, pc) || grp_unsafe_n >= 200) return;
	u32 pv_save[32], ok_save = pv_ok;   // valores probables al ir avanzando (se dejan como estaban al acabar)
	memcpy(pv_save, pv, sizeof pv);
	s32 delta[32];   // cuánto se ha movido cada base desde que empezó su grupo
	u8 in_hk[RA_MAX];   // dentro de un si-entonces (un addiu ahí no se ejecuta en los dos caminos)
	memset(in_hk, 0, sizeof in_hk);
	for (int p = 0; p < ra_n; p++) {
		u32 op = fetch_op(pc + 4 * (u32)p), o = op >> 26, b = (op >> 21) & 31;
		if (is_branch(op))
			for (int i = 0, k = hammock_len(op, pc + 4 * (u32)p, p + 1, 1); i < k; i++) if (p + 2 + i < RA_MAX) in_hk[p + 2 + i] = 1;
		// cargas y escrituras normales (también LWC2/SWC2) con base en un registro que no sea sp (el atajo de la pila)
		// ni la base de la scratchpad
		if (((o >= 0x20 && o <= 0x25 && o != 0x22) || o == 0x28 || o == 0x29 || o == 0x2b || o == 0x32 || o == 0x3a) &&
		    b && b != 29 && (int)b != scr_reg &&
		    (!(pv_ok & (1u << b)) || !((pv[b] + (u32)(s32)(s16)op) & 0x1f800000))) {   // si se sabe que no es RAM, no
			int g = open[b];
			s32 off, sz = grp_size(o);
			if (g < 0 && grp_n < GRP_MAX) {
				delta[b] = 0;
				g = open[b] = (s8)grp_n++;
				memset(&grp[g], 0, sizeof grp[g]);
				grp[g].base = (s8)b;
				grp[g].first = (s16)p;
				grp[g].lo = (s16)op;
				grp[g].hi = (s16)op + sz;
				grp[g].slo = 0x7fffffff;
				grp[g].shi = -0x7fffffff;
			}
			off = (s16)op + (g >= 0 ? delta[b] : 0);   // respecto a la base al empezar el grupo
			if (g >= 0 && off >= -0x8000 && off < 0x8000) {
				grp_at[p] = (s8)g;
				grp_adj[p] = (s16)(g >= 0 ? delta[b] : 0);
				grp[g].last = (s16)p;
				if (off < grp[g].lo) grp[g].lo = off;
				if (off + sz > grp[g].hi) grp[g].hi = off + sz;
				if (o >= 0x28 && o != 0x32) {
					grp[g].nst++;
					if (off < grp[g].slo) grp[g].slo = off;
					if (off + sz > grp[g].shi) grp[g].shi = off + sz;
				} else grp[g].nld++;
			}
		}
		for (u32 m = ra_wr[p]; m; m &= m - 1) {   // la base cambia: el grupo se acaba, salvo «addiu base, base, n»
			int r = __builtin_ctz(m);
			if (o == 0x09 && (int)b == r && (int)((op >> 16) & 31) == r && open[r] >= 0 && !in_hk[p] && p < RA_MAX) {
				delta[r] += (s16)op;
				continue;
			}
			open[r] = -1;
		}
		pv_after(op);
	}
	memcpy(pv, pv_save, sizeof pv);
	pv_ok = ok_save;
	// ahorro: una carga normal son ~5 instrucciones y una escritura ~7; en el grupo, 1; la comprobación, ~7 (+6 si escribe)
	for (int g = 0; g < grp_n; g++) {
		val[g] = grp[g].nld * 4 + grp[g].nst * 6 - 7 - (grp[g].nst ? 6 : 0);
		if (grp[g].nst && grp[g].shi - grp[g].slo > (1 << DYN_PAGE_BITS)) val[g] = 0;   // más de dos páginas
	}
	// de más a menos ahorro, cada grupo al primer registro libre en su tramo (el segundo, solo si ahorra bastante: se
	// le quita un registro al reparto)
	(void)any;
	for (;;) {
		int best = -1, slot = -1;
		for (int g = 0; g < grp_n; g++)
			if (!take[g] && val[g] > GRP_MIN && (best < 0 || val[g] > val[best])) best = g;
		if (best < 0) break;
		for (int s = 0; s < GRP_REGS && slot < 0; s++) {
			int clash = s > 0 && val[best] <= 12;
			for (int h = 0; h < grp_n; h++)
				if (take[h] == s + 1 && grp[h].first <= grp[best].last && grp[best].first <= grp[h].last) clash = 1;
			if (!clash) slot = s;
		}
		take[best] = slot >= 0 ? slot + 1 : -1;
		if (slot >= 0) {
			static const int regs[2][2] = { { R8, R7 }, { R7, R6 } };   // sin / con el atajo de la pila (r8)
			grp[best].reg = (s8)regs[use_hsp ? 1 : 0][slot];
			grp_rmask |= 1 << grp[best].reg;
		}
	}
	for (int p = 0; p < ra_n && p < RA_MAX; p++) if (grp_at[p] >= 0 && take[grp_at[p]] <= 0) grp_at[p] = -1;
}

static void alloc_regs(u32 pc)
{
	u8 uses[32] = { 0 };
	u32 seen = 0;
	int n = 0;
	int sp_ops = 0;
	u8 scr_uses[32] = { 0 };
	u32 wr_before = 0;
	s32 sp_delta = 0;
	int sp_track = 1;
	hsp_lo = 0x7fffffff;
	hsp_hi = -0x7fffffff;
	memset(ra_use, 0, sizeof ra_use);
	memset(ra_wr, 0, sizeof ra_wr);
	ra_n = 0;
	for (u32 cur = pc;; cur += 4) {
		u32 op = fetch_op(cur), r, w, m, pos = (cur - pc) >> 2;
		int last = 0;
		if (++n > MAX_BLOCK || ends_block(op)) last = 1;
		int cont = 0;
		if (is_branch(op)) {   // con su delay slot
			u32 r2, w2, dop = fetch_op(cur + 4);
			op_regs(dop, &r2, &w2);
			if (pos + 1 < RA_MAX) { ra_use[pos + 1] = r2 | w2; ra_wr[pos + 1] = w2; ra_n = (int)pos + 2; }
			sp_ops += sp_mem_op(dop);
			sp_range(dop, &sp_delta, &sp_track);
			for (m = r2 | w2; m; m &= m - 1) uses[__builtin_ctz(m)] += 1;
			seen |= r2 | w2;
			// superbloque: tras un salto condicional se sigue por el camino de no saltar (igual que compile); en un
			// si-entonces también (lo de dentro y lo de después van en el mismo bloque)
			if (!is_branch(dop) && !ends_block(dop) &&
			    (hammock_len(op, cur, n, 0) || (sb_cont(op, cur, pc) && n + 2 < SB_MAX && !has_block(cur + 8)))) { cont = 1; n++; wr_before |= w2; }
			else last = 1;
		}
		sp_ops += sp_mem_op(op);
		sp_range(op, &sp_delta, &sp_track);
		op_regs(op, &r, &w);
		if (pos < RA_MAX) { ra_use[pos] = r | w; ra_wr[pos] = w; if ((int)pos + 1 > ra_n) ra_n = (int)pos + 1; }
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
#ifdef DYN_NOHSP
	use_hsp = hsp_ok = 0;
#endif
	scr_reg = scr_ok = 0;
	if (!pc_in(scr_unsafe, pc) && scr_unsafe_n < 200)
		for (int i = 1; i < 32; i++)
			if (scr_uses[i] >= 2 && (!scr_reg || scr_uses[i] > scr_uses[scr_reg])) scr_reg = i;
#ifdef DYN_NOSCR
	scr_reg = 0;
#endif
	if (scr_reg) { scr_ok = 1; scr_val = pv[scr_reg]; }
	ra_index();
	grp_find(pc);
	ra_npool = 0;
	ra_frozen = 0;
	for (int h = 0; h < 16; h++) host_of[h] = -1;
	for (int h = 0; h < DYN_POOL; h++) if (!(use_hsp && host_pool[h] == RHSP) && !(grp_rmask & (1 << host_pool[h]))) ra_pool[ra_npool++] = (u8)host_pool[h];
#ifndef DYN_STATIC_RA
	(void)uses;
	return;   // reparto instrucción a instrucción (ra_need)
#endif
	for (int h = 0; h < DYN_POOL && seen; h++) {
		if ((use_hsp && host_pool[h] == RHSP) || (grp_rmask & (1 << host_pool[h]))) continue;
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
	cur_ninstr = ninstr;

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
			// en ARM, LSR #0 y ASR #0 significan «por 32»: desplazar 0 es copiar
			a_dp_reg(C_AL, OP_MOV, 0, d, 0, a, sa ? shs[op & 3] : SH_LSL, sa);
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
#ifdef ARM9
			{   // en línea con el divisor por hardware; los casos raros (entre 0, desbordamiento, bit alto en DIVU) en C
				u32 *slow1, *slow2, *loop, *back;
				a_dp_imm(C_AL, OP_CMP, 1, 0, R1, 0);
				slow1 = fwd(C_EQ);
				if ((op & 0x3f) == 0x1a) {
					a_dp_imm(C_AL, OP_CMN, 1, 0, R1, 1);   // d == -1: el desbordamiento lo resuelve h_div
					slow2 = fwd(C_EQ);
				} else {
					a_dp_reg(C_AL, OP_ORR, 1, R2, R0, R1, SH_LSL, 0);   // algún bit alto: la división del hardware es con signo
					slow2 = fwd(C_MI);
				}
				a_movi(R2, 0x04000280);   // REG_DIVCNT
				a_dp_imm(C_AL, OP_MOV, 0, R3, 0, 0);   // 32/32
				a_str(R3, R2, 0);
				a_str(R0, R2, 0x10);
				a_str(R1, R2, 0x18);
				loop = emit_ptr;
				a_ldr(R3, R2, 0);
				a_dp_imm(C_AL, OP_TST, 1, 0, R3, arm_imm(0x8000));
				a_b(C_NE, 0, loop);
				a_ldr(R0, R2, 0x20);
				a_ldr(R1, R2, 0x28);
				a_str(R0, RCPU, OFF_LO);
				a_str(R1, RCPU, OFF_HI);
				back = emit_ptr;
				begin_cold();
				land(slow1);
				land(slow2);
				call((op & 0x3f) == 0x1a ? (const void *)h_div : (const void *)h_divu);
				a_b(C_AL, 0, back);
				end_cold();
				return 1;
			}
#endif
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
			a_movi(R0, op & 0xfffff);   // la GTE solo mira los bits 0-19: con los altos fuera, NCLIP es un MOV y RTPT dos (antes tres)
#ifdef DYN_NOGTED
			call(gte_command);
			return 1;
#endif
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
	case 0x2a: case 0x2e: {   // SWL, SWR: leer la palabra alineada, mezclar y escribirla (como un SW normal)
		int x, v;
		x = get(rs, R0);
		a_addi(R0, x, simm, R1);
		a_dp_imm(C_AL, OP_BIC, 0, R0, R0, 3);
		a_dp_imm(C_AL, OP_TST, 1, 0, R0, arm_imm(0x1f800000));
		a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, R0, SH_LSL, 11);
		a_mem_regsh(C_EQ, 1, 0, R2, RRAM, R1, SH_LSR, 11);
		a_b(C_NE, 1, ld_handler[M_W][R2]);
		x = get(rs, R3);
		a_addi(R3, x, simm, R1);
		a_dp_imm(C_AL, OP_AND, 0, R3, R3, 3);
		a_dp_reg(C_AL, OP_MOV, 0, R3, 0, R3, SH_LSL, 3);   // s = (dirección & 3) * 8
		v = get(rt, R1);
		if ((op >> 26) == 0x2a) {   // SWL: (w & (FFFFFF00 << s)) | (rt >> (24 - s))
			a_dp_imm(C_AL, OP_MVN, 0, R0, 0, 0xff);
			a_dp_regsh(C_AL, OP_MOV, 0, R0, 0, R0, SH_LSL, R3);
			a_dp_reg(C_AL, OP_AND, 0, R2, R2, R0, SH_LSL, 0);
			a_dp_imm(C_AL, OP_RSB, 0, R0, R3, 24);
			a_dp_regsh(C_AL, OP_MOV, 0, R0, 0, v, SH_LSR, R0);
		} else {                    // SWR: (w & ~(FFFFFFFF << s)) | (rt << s)
			a_dp_imm(C_AL, OP_MVN, 0, R0, 0, 0);
			a_dp_regsh(C_AL, OP_MOV, 0, R0, 0, R0, SH_LSL, R3);
			a_dp_reg(C_AL, OP_BIC, 0, R2, R2, R0, SH_LSL, 0);
			a_dp_regsh(C_AL, OP_MOV, 0, R0, 0, v, SH_LSL, R3);
		}
		a_dp_reg(C_AL, OP_ORR, 0, R2, R2, R0, SH_LSL, 0);
		x = get(rs, R0);
		a_addi(R0, x, simm, R1);
		a_dp_imm(C_AL, OP_BIC, 0, R0, R0, 3);
		a_dp_imm(C_AL, OP_TST, 1, 0, R0, arm_imm(0x1f800000));
		a_dp_reg(C_EQ, OP_MOV, 0, R1, 0, R0, SH_LSL, 11);
		a_mem_regsh(C_EQ, 0, 0, R2, RRAM, R1, SH_LSR, 11);
		a_mem_regsh(C_EQ, 1, 1, R3, RPAGES, R1, SH_LSR, 11 + DYN_PAGE_BITS);   // ¿había código traducido en esa página?
		a_dp_imm(C_EQ, OP_CMP, 1, 0, R3, 0);
		a_b(C_NE, 1, st_handler[M_W][R2]);
		return 1;
	}
	case 0x28: emit_store(M_BU, rs, rt, simm); return 1;  // SB
	case 0x29: emit_store(M_HU, rs, rt, simm); return 1;  // SH
	case 0x2b: emit_store(M_W, rs, rt, simm); return 1;   // SW
	case 0x32:   // LWC2
		grp_pre((int)rs, simm);
		load_to(M_W, rs, simm, R2);
		gte_from((int)rt, R2);
		return 1;
	case 0x3a:   // SWC2
		grp_pre((int)rs, simm);
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

// Saltar la espera hasta el siguiente evento. vs_off >= 0: bucle de VSync() de PsyQ, cuyo plazo (un contador en
// vs_off(sp) que baja en cada vuelta) se descuenta con las vueltas que habría dado en el tiempo saltado
static void emit_idle(u32 target, int vs_off)
{
#ifdef PSX_PROFILE
	a_movi(R2, (u32)(uintptr_t)&psx_stat.dyn_idle);
	a_ldr(R3, R2, 0);
	a_dp_imm(C_AL, OP_ADD, 0, R3, R3, 1);
	a_str(R3, R2, 0);
#endif
	// Esperas de hardware (registros que cambian con el tiempo) o de interrupciones: se salta al siguiente evento.
	// El VSync() de PsyQ además lleva un plazo; Tomb Raider lo llama sin la vblank activada y sale por ahí.
	{
#ifdef DYN_NOVSADJ
		vs_off = -1;
#endif
		if (vs_off >= 0) {
			u32 *not_ram;
			a_ldr(R0, RCPU, OFF_R(29));
			a_addi(R0, R0, (u32)(s32)(s16)vs_off, R1);
			a_dp_imm(C_AL, OP_TST, 1, 0, R0, arm_imm(0x1f800000));
			not_ram = fwd(C_NE);
			a_dp_reg(C_AL, OP_MOV, 0, R0, 0, R0, SH_LSL, 11);
			a_mem_regsh(C_AL, 1, 0, R1, RRAM, R0, SH_LSR, 11);
			a_dp_imm(C_AL, OP_RSB, 0, R2, RCYC, 0);   // ciclos saltados
			a_movi(R3, 65536 / 26);                     // una vuelta son unas 13 instrucciones
			a_mul(R2, R3, R2);
			a_dp_reg(C_AL, OP_MOV, 0, R2, 0, R2, SH_LSR, 16);
			a_dp_reg(C_AL, OP_SUB, 1, R1, R1, R2, SH_LSL, 0);
			a_dp_imm(C_MI, OP_MOV, 0, R1, 0, 0);       // sin pasarse: la siguiente vuelta lo deja en -1
			a_mem_regsh(C_AL, 0, 0, R1, RRAM, R0, SH_LSR, 11);
			land(not_ram);
		}
		a_dp_imm(C_AL, OP_MOV, 0, RCYC, 0, 0);   // psx_cycles = psx_run_until
	}
	exit_pc(target);
}

#ifndef DYN_HIPAD
#define DYN_HIPAD 0
#endif
static void mark_pages(u32 start, u32 end)
{
	u32 p0 = start & 0x1fffffff, p1 = end & 0x1fffffff;
	if (p0 >= 0x800000) return;
	u32 a0 = p0 & 0x1fffff, a1 = (p1 & 0x1fffff) + 4 + DYN_HIPAD, pg0 = a0 >> DYN_PAGE_BITS, pg1 = (a1 - 1) >> DYN_PAGE_BITS;
	for (u32 p = pg0; p <= pg1 && p < DYN_NPAGES; p++) {   // con el delay slot del final
		u32 base = p << DYN_PAGE_BITS, lo = p == pg0 ? a0 - base : 0, hi = p == pg1 ? a1 - base : 1u << DYN_PAGE_BITS;
		if (!dyn_pages[p]) { code_lo[p] = (u16)lo; code_hi[p] = (u16)hi; dyn_pages[p] = 1; continue; }
		if (lo < code_lo[p]) code_lo[p] = (u16)lo;
		if (hi > code_hi[p]) code_hi[p] = (u16)hi;
	}
	if (code_bits)
		for (u32 a = a0 & ~3u; a < a1 && a < 0x200000; a += 4) code_bits[a >> 7] |= 1u << ((a >> 2) & 31);
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
static block_fn compile_block(u32 pc, block_fn *s);
static block_fn compile(u32 pc, block_fn *s)
{
	block_fn f;
	fetch_mark = 1;
	f = compile_block(pc, s);
	fetch_mark = 0;
	return f;
}

static block_fn compile_block(u32 pc, block_fn *s)
{
	u32 *start, *entry, *cold_start, cur = pc;
	int n = 0, tot = 0;   // n: instrucciones del tramo actual (para los ciclos); tot: de todo el superbloque

	if (emit_ptr + MAX_BLOCK * 48 + 512 > hot_end || cold_ptr + MAX_BLOCK * 16 + 256 > code_end) flush_all();
	start = emit_ptr;
	cold_start = cold_ptr;
#ifdef PSX_SMCLOG
	{ static u32 lf; if (lf != psx_frame_no && dyn_cmp_log.n < 8192) { lf = psx_frame_no; dyn_cmp_log.e[dyn_cmp_log.n++] = 0xff000000 | lf; } }
#ifdef PSX_CMPPC
	if (psx_frame_no >= 200 && dyn_cmp_log.n < 8192) dyn_cmp_log.e[dyn_cmp_log.n++] = pc;
#endif
#endif
	pv_start();
	rng_start();
	blk_pc = pc;
	alloc_regs(pc);
	emit(pc);   // cabecera: el PC virtual con el que se tradujo
	entry = emit_ptr;
	if (use_hsp) {
		u32 spv = cpu.r[29] & 0x1fffffff;   // dónde está la pila ahora: así se traduce (si cambia, la entrada falla)
		int sp = get(29, R0);
		if (spv >= 0x1f800000 && spv < 0x1f800400 && hsp_lo <= hsp_hi && hsp_hi - hsp_lo <= 0x400) {
			// pila en la scratchpad (Pepsiman y muchos otros): r8 = su sitio en la copia de la DTCM, si todo lo que
			// toca el bloque [sp + hsp_lo, sp + hsp_hi) cabe en sus 1024 bytes
			a_dp_imm(C_AL, OP_BIC, 0, R1, sp, arm_imm(0xe0000000));
			a_dp_imm(C_AL, OP_SUB, 0, R1, R1, arm_imm(0x1f800000));
			a_addi(R2, R1, (u32)hsp_lo, R3);
			a_movi(R3, 0x400u - (u32)(hsp_hi - hsp_lo));
			a_dp_reg(C_AL, OP_CMP, 1, 0, R2, R3, SH_LSL, 0);
			sp_fail[0] = fwd(C_HI);
			sp_fail[1] = NULL;
			a_dp_reg(C_AL, OP_ADD, 0, RHSP, RCPU, R1, SH_LSL, 0);
			a_addi(RHSP, RHSP, (u32)off_scr, R3);
		} else {   // pila en la RAM (y no en los primeros 32 KB) -> r8 = su dirección en la DS
			a_dp_imm(C_AL, OP_TST, 1, 0, sp, arm_imm(0x1f800000));
			sp_fail[0] = fwd(C_NE);
			a_dp_reg(C_AL, OP_MOV, 0, R0, 0, sp, SH_LSL, 11);
			a_dp_imm(C_AL, OP_CMP, 1, 0, R0, arm_imm(0x04000000));
			sp_fail[1] = fwd(C_CC);
			a_dp_reg(C_AL, OP_ADD, 0, RHSP, RRAM, R0, SH_LSR, 11);
		}
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
			int late, a, b, hk;
			if (is_branch(dop) || ends_block(dop)) {
				if (tot == 1) { emit_ptr = start; cold_ptr = cold_start; return NULL; }   // que lo haga el intérprete
				flush_regs();
				set_pc_const(cur);
				exit_block(n - 1);
				break;
			}
			ra_op(op, cur);
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
			hk = cond_kind == 1 && !link_reg ? hammock_len(op, cur, tot, 1) : 0;
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
			if (dop) { ra_op(dop, cur + 4); in_dslot = 1; emit_op(dop, cur + 4, n); in_dslot = 0; }
			if (hk) {   // si-entonces corto (ver hammock_len): saltar lo de dentro sin salir del bloque
				u32 from = cur + 8, wm = hk_prepare(from, hk), *skip;
				int c;
				if (late) { branch_operands(op, &a, &b, R2, R3); c = branch_cond(op, a, b); }
				else { a_ldr(R2, RCPU, off_tmp); a_dp_imm(C_AL, OP_CMP, 1, 0, R2, 0); c = C_NE; }
				skip = fwd(c);
				pv_after(op);
				rng_after(op);
				pv_after(dop);
				rng_after(dop);
				for (int i = 0; i < hk; i++) {
					u32 sop = fetch_op(from + 4 * (u32)i);
					n++;
					tot++;
					cur_pos = (int)((from + 4 * (u32)i - blk_pc) >> 2);
					if (sop) emit_op(sop, from + 4 * (u32)i, n);
					pv_after(sop);
					rng_after(sop);
				}
				for (u32 m = wm; m; m &= m - 1) hk_unconst(__builtin_ctz(m));   // lo que se volvió constante dentro
				ra_frozen = 0;
				land(skip);
				// tras juntarse los caminos, lo cambiado dentro ya no tiene valor ni rango conocido
				pv_ok &= ~wm;
				for (u32 m = wm; m; m &= m - 1) rng[__builtin_ctz(m)].kind = RK_NONE;
				cur = from + 4 * (u32)hk;
				continue;
			}
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
				emit_idle(target, -1);
				begin_cold();
				land(out);
				exit_pc(target);
				end_cold();
			} else {
				u32 *out, *not_taken, *taken_exit;
				int idle = (target == pc || psyq_vsync(target) || vsync_poll_call(target)) && pure_loop(pc, cur), c;
#ifdef DYN_NOIDLE
				idle = 0;
#endif
				// operandos listos antes de mirar el tiempo: los dos caminos los usan
				if (late) branch_operands(op, &a, &b, R2, R3);
				else { a_ldr(R2, RCPU, off_tmp); a = R2; b = -1; }
				goon = !idle && sb_cont(op, cur, pc) && tot + 2 < SB_MAX && !has_block(cur + 8);
#ifndef DYN_SB_CYC_BOTH
				if (goon) {
					// superbloque: el camino que sigue no cuenta ciclos ni mira el tiempo aquí (lo hará la próxima salida
					// con todas las instrucciones juntas); solo el que salta, que sale del bloque
					if (late) c = branch_cond(op, a, b);
					else { a_dp_imm(C_AL, OP_CMP, 1, 0, R2, 0); c = C_NE; }
					not_taken = fwd(c ^ 1);
					emit_cycles(n);
					out = fwd(C_PL);
					link_site(target);
					land(not_taken);
					begin_cold();
					land(out);
					exit_pc(target);
					end_cold();
					pv_after(op);
					rng_after(op);
					pv_after(dop);
					rng_after(dop);
					cur += 8;
					continue;
				}
#endif
				emit_cycles(n);
				out = fwd(C_PL);
				if (late) c = branch_cond(op, a, b);
				else { a_dp_imm(C_AL, OP_CMP, 1, 0, R2, 0); c = C_NE; }
				not_taken = fwd(c ^ 1);
				if (idle) {
#ifdef PSX_SMCLOG
					if (dyn_cmp_log.n < 8190) { dyn_cmp_log.e[dyn_cmp_log.n++] = 0x1d1e0000 | (psyq_vsync(target) ? 1 : vsync_poll_call(target) ? 2 : 3); dyn_cmp_log.e[dyn_cmp_log.n++] = pc; }
#endif
				}
				if (idle) emit_idle(target, psyq_vsync(target) ? (int)(fetch_op(target) & 0xffff) : -1);
				else link_site(target);
				land(not_taken);
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
		if (op) ra_op(op, cur);
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
		if (sp_fail[1]) land(sp_fail[1]);
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
	mark_pages(pc, cur);   // cur ya es el delay slot del final (antes «cur + 4» marcaba una palabra de más: el contador de
	                        // Crash que va justo detrás borraba el bloque en cada imagen). fetch_op ya marcó lo leído por delante
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
#ifdef PSX_SMCLOG
	if (psx_frame_no >= 100 && dyn_cmp_log.n < 8192) dyn_cmp_log.e[dyn_cmp_log.n++] = pc | 1;
#endif
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
#ifdef PSX_SMCLOG
	if (psx_frame_no >= 100 && dyn_cmp_log.n < 8192) dyn_cmp_log.e[dyn_cmp_log.n++] = pc | 2;
#endif
	if (scr_unsafe_n < 256) {
		for (i = (pc >> 2) & 255; scr_unsafe[i] && scr_unsafe[i] != pc; i = (i + 1) & 255) { }
		if (!scr_unsafe[i]) { scr_unsafe[i] = pc; scr_unsafe_n++; }
	}
	if (s && *s && (*s)[-1] == pc) *s = NULL;
	if (l < 0x200000) unlink_page(l >> DYN_PAGE_BITS, l, l + 4);
}

// Lo llama la salida de un grupo cuya comprobación falló (ver grp_materialize): el bloque de pc, sin grupos
void dyn_grp_fail(u32 pc)
{
	block_fn *s = slot(pc);
	u32 l = lin(pc), i;
	if (grp_unsafe_n < 256) {
		for (i = (pc >> 2) & 255; grp_unsafe[i] && grp_unsafe[i] != pc; i = (i + 1) & 255) { }
		if (!grp_unsafe[i]) { grp_unsafe[i] = pc; grp_unsafe_n++; }
	}
	if (s && *s && (*s)[-1] == pc) *s = NULL;
	if (l < 0x200000) unlink_page(l >> DYN_PAGE_BITS, l, l + 4);
}

// PCs que compile() deja al intérprete (un salto con otro salto o un RFE en su delay slot, como el final de
// las interrupciones de la BIOS): sin esto se intentaba traducirlos cada vez, cientos por segundo
static u32 interp_only[64];
static int is_interp_only(u32 pc) { return pc && interp_only[(pc >> 2) & 63] == pc; }

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
		if (is_interp_only(target)) return NULL;
		{ PROF_BEGIN(); f = compile(target, s); PROF_END(PROF_COMPILE); }
		STAT_ADD(dyn_compiled, 1);
		if (!f) { interp_only[(target >> 2) & 63] = target; return NULL; }
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
#ifdef DYN_STRESS
	{ extern u32 psx_frame_no; static u32 lf; if (psx_frame_no - lf >= DYN_STRESS) { lf = psx_frame_no; flush_all(); } }   // pruebas: medir el traductor (todo se retraduce cada DYN_STRESS imágenes)
#endif
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
#ifdef DYN_INTERP
		cpu_interp_step();   // pruebas: todo con el intérprete (¿el fallo es del dynarec?)
		continue;
#endif
		// con la caché aislada (la BIOS la vacía así) las escrituras no deben llegar a la RAM
		if (cpu.cop0[12] & (1 << 16)) { cpu_interp_step(); continue; }
		s = slot(cpu.pc);
		dyn_where = 1;
		if (!s) { dyn_where = 3; cpu_interp_step(); continue; }
		f = *s;
		if (!f || f[-1] != cpu.pc) {
			if (is_interp_only(cpu.pc)) { dyn_where = 3; cpu_interp_step(); continue; }
			dyn_where = 2;
			{ PROF_BEGIN(); f = compile(cpu.pc, s); PROF_END(PROF_COMPILE); }
			STAT_ADD(dyn_compiled, 1);
			if (!f) { interp_only[(cpu.pc >> 2) & 63] = cpu.pc; dyn_where = 3; cpu_interp_step(); continue; }
		}
		dyn_where = 4;
		dyn_enter(&cpu, psx_ram, dyn_pages, f);   // corre bloques encadenados hasta psx_run_until
	}
	dyn_where = 0;
}

// Para el perfilador: inicio de la caché, trampolines, rutinas de memoria, bloques, zona fría y final
// stubs [0, 1), trampolines [1, 2), rutinas de memoria [2, 3), bloques [3, 4), zona fría [5, 6); r[7] = 0
void dyn_regions(u32 r[8])
{
	r[0] = (u32)(uintptr_t)code_buf;
	r[1] = (u32)(uintptr_t)tramp_at[0];
	r[2] = (u32)(uintptr_t)smc_handler;
	r[3] = (u32)(uintptr_t)code_start;
	r[4] = (u32)(uintptr_t)hot_end;
	r[5] = (u32)(uintptr_t)cold_base;
	r[6] = (u32)(uintptr_t)code_end;
	r[7] = 0;
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
