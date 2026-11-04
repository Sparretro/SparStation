// Rutinas de los juegos hechas en nativo (HLE). Ahora solo una: DecDCTvlc de la librería de vídeo de Sony
// (libpress), que deshace el Huffman de cada imagen de los vídeos. En la PS1 la hace la CPU; aquí, emulada,
// era un buen trozo del tiempo de los vídeos (y con la CPU de la PS1 a menos del 100 %, imágenes perdidas).
//
// Se reconoce por su código (la misma rutina en muchos juegos; solo cambian dos direcciones de sus variables)
// y se hace instrucción a instrucción lo mismo que el MIPS: mismos registros al salir, misma salida en la RAM,
// mismo estado guardado para seguir en la siguiente llamada (DecDCTvlc(0, ...)).
#include "psx.h"
#if defined(HLE_CHECK) || defined(HLE_LOG)
#include <stdio.h>
#include <stdlib.h>
#endif

#ifdef PSX_DYNAREC
void dyn_ram_written(u32 off, u32 len);
#endif

// DecDCTvlc2 de libpress (vídeos v2 y v3), 207 palabras. Las de VLC_ADDR llevan direcciones propias de cada
// juego (lui/addiu de su tamaño máximo y de su estado guardado) y no se comparan.
#define VLC_WORDS 207
static const u32 vlc_code[VLC_WORDS] = {
	0x3c088017, 0x2508af9c, 0x20c60800, 0x3c010001, 0x00c13820, 0x1480000f, 0x8d090000, 0x3c088017,
	0x2508b30c, 0x8d040000, 0x8d050004, 0x8d020008, 0x8d03000c, 0x8d0c0010, 0x8d0d0014, 0x8d0f0018,
	0x8d18001c, 0x8d190020, 0x01294820, 0x0401005f, 0x00a97020, 0x00006820, 0x00007820, 0x0000c020,
	0x0000c820, 0x01294820, 0x00a97020, 0x94880000, 0x94890002, 0x948c0004, 0x948a0006, 0x94820008,
	0x9483000a, 0x214afffd, 0x05400002, 0x000c6280, 0x200d0001, 0x2084000c, 0x00021400, 0x00431025,
	0x00001825, 0xa4a80000, 0xa4a90002, 0x20a50002, 0x11a00035, 0x00024582, 0x390103ff, 0x10200085,
	0x20a50002, 0x21a1fffd, 0x04200002, 0x20c1fc00, 0x2021fc00, 0x00024602, 0x00084080, 0x01014020,
	0x95090000, 0x950a0002, 0x00004024, 0x1140000a, 0x01221004, 0x20010020, 0x002a0822, 0x00224006,
	0x04400004, 0x01421004, 0x200bffff, 0x002b5806, 0x010b4022, 0x006a1820, 0x00691820, 0x30610010,
	0x10200005, 0x3063000f, 0x94890000, 0x20840002, 0x00694804, 0x00491025, 0x21a1fffe, 0x1c200008,
	0x03284820, 0x10200004, 0x03084820, 0x01e84820, 0x04010004, 0x01e87820, 0x04010002, 0x0308c020,
	0x0328c820, 0x00094880, 0x312903ff, 0x01894825, 0x21ad0001, 0x21a1fff9, 0x14200011, 0xa4a90000,
	0x0401000f, 0x21adfffa, 0x390101ff, 0x10200051, 0x20a50002, 0x00021280, 0x2063000a, 0x30610010,
	0x10200005, 0x3063000f, 0x94890000, 0x20840002, 0x00694804, 0x00491025, 0x01884025, 0xa4a80000,
	0x00ae0823, 0x04210050, 0x20a50002, 0x000244c2, 0x000840c0, 0x01064020, 0x8d090000, 0x00000000,
	0x15200011, 0x312100ff, 0x00021200, 0x20630008, 0x30610010, 0x10200005, 0x3063000f, 0x94880000,
	0x20840002, 0x00684004, 0x00481025, 0x000245c2, 0x00084080, 0x01074020, 0x8d090000, 0x00005820,
	0x04010002, 0x312100ff, 0x8d0b0004, 0x00221004, 0x00611820, 0x30610010, 0x10200005, 0x3063000f,
	0x94880000, 0x20840002, 0x00684004, 0x00481025, 0x00094c02, 0x39217c1f, 0x10200015, 0x3921fe00,
	0x1020ff93, 0xa4a90000, 0x1160ffd8, 0x20a50002, 0x316affff, 0x39417c1f, 0x1020000d, 0x3941fe00,
	0x1020ff8b, 0xa4aa0000, 0x000b5402, 0x1140ffcf, 0x20a50002, 0x39417c1f, 0x10200005, 0x3941fe00,
	0x1020ff83, 0xa4aa0000, 0x0401ffc8, 0x20a50002, 0x00024402, 0xa4a80000, 0x20a50002, 0x94880000,
	0x20840002, 0x00021400, 0x00684004, 0x0401ffbf, 0x00481025, 0x3408fe00, 0x20020040, 0xa4a80000,
	0x20a50002, 0x1440fffd, 0x2042ffff, 0x40096000, 0x00000000, 0x3c010002, 0x01214825, 0x40896000,
	0x03e00008, 0x00001020, 0x3c088017, 0x2508b30c, 0xad040000, 0xad050004, 0xad020008, 0xad03000c,
	0xad0c0010, 0xad0d0014, 0xad0f0018, 0xad18001c, 0xad190020, 0x03e00008, 0x20020001,
};
static int vlc_addr_word(int i) { return i == 0 || i == 1 || i == 7 || i == 8 || i == 194 || i == 195; }

static inline u32 rd_word(u32 a)
{
	u32 p = a & 0x1fffffff;
	if (p < 0x800000) return *(const u32 *)(psx_ram + (p & 0x1ffffc));
	if (p >= 0x1fc00000 && p < 0x1fc80000) return *(const u32 *)(psx_bios + (p & 0x7fffc));
	return 0;
}

static u32 lui_addiu(u32 hi, u32 lo) { return (hi << 16) + (u32)(s32)(s16)(lo & 0xffff); }

// ¿Empieza aquí DecDCTvlc? Devuelve 1 y las direcciones de sus dos variables (se lee sin marcar nada como código)
static int vlc_match(u32 pc, u32 *maxsize, u32 *state)
{
	u32 w[VLC_WORDS];
	if ((rd_word(pc) & 0xffff0000) != 0x3c080000 || rd_word(pc + 8) != vlc_code[2]) return 0;   // filtro rápido
	for (int i = 0; i < VLC_WORDS; i++) {
		w[i] = rd_word(pc + 4 * (u32)i);
		if (vlc_addr_word(i) ? (w[i] & 0xffff0000) != (vlc_code[i] & 0xffff0000) : w[i] != vlc_code[i]) return 0;
	}
	*maxsize = lui_addiu(w[0] & 0xffff, w[1]);
	*state = lui_addiu(w[7] & 0xffff, w[8]);
	return *state == lui_addiu(w[194] & 0xffff, w[195]);
}

// ---- memoria: RAM directa (lo normal); si no, por el bus ----
static inline u32 lh(u32 a)
{
	u32 p = a & 0x1fffffff;
	if (p < 0x800000) return *(const u16 *)(psx_ram + (p & 0x1ffffe));
	return bus_read16(a) & 0xffff;
}
static inline u32 lw(u32 a)
{
	u32 p = a & 0x1fffffff;
	if (p < 0x800000) return *(const u32 *)(psx_ram + (p & 0x1ffffc));
	return bus_read32(a);
}
static inline void sh(u32 a, u32 v)
{
	u32 p = a & 0x1fffffff;
	if (p < 0x800000) *(u16 *)(psx_ram + (p & 0x1ffffe)) = (u16)v;
	else bus_write16(a, v & 0xffff);
}
static inline void sw(u32 a, u32 v)
{
	u32 p = a & 0x1fffffff;
	if (p < 0x800000) *(u32 *)(psx_ram + (p & 0x1ffffc)) = v;
	else bus_write32(a, v);
}

// El búfer en el registro bit a bit: v0 (32 bits) y v1 (bits gastados de la media palabra siguiente)
#define REFILL(tmp) do { at = v1 & 16; v1 &= 15; if (at) { tmp = lh(a0); a0 += 2; tmp <<= v1; v0 |= tmp; } } while (0)

static void vlc_run(u32 maxsize_addr, u32 state_addr)
{
	u32 *r = cpu.r;
	u32 at = r[1], v0 = r[2], v1 = r[3], a0 = r[4], a1 = r[5], a2 = r[6], a3;
	u32 t0, t1, t2 = r[10], t3 = r[11], t4 = r[12], t5 = r[13], t6, t7 = r[15], t8 = r[24], t9 = r[25];
	u32 out0, n = 0;

	t0 = maxsize_addr;
	a2 += 2048;
	at = 0x10000;
	a3 = a2 + at;
	t1 = lw(t0);
	if (a0 == 0) {   // seguir donde se quedó
		t0 = state_addr;
		a0 = lw(t0); a1 = lw(t0 + 4); v0 = lw(t0 + 8); v1 = lw(t0 + 12);
		t4 = lw(t0 + 16); t5 = lw(t0 + 20); t7 = lw(t0 + 24); t8 = lw(t0 + 28); t9 = lw(t0 + 32);
		t1 += t1;
		t6 = a1 + t1;
		out0 = a1;
		goto loop;
	}
	t5 = t7 = t8 = t9 = 0;
	t1 += t1;
	t6 = a1 + t1;
	t0 = lh(a0); t1 = lh(a0 + 2); t4 = lh(a0 + 4); t2 = lh(a0 + 6); v0 = lh(a0 + 8); v1 = lh(a0 + 10);
	t2 -= 3;
	t4 <<= 10;
	if ((s32)t2 >= 0) t5 = 1;   // v3: la continua va con su propio código
	a0 += 12;
	v0 = (v0 << 16) | v1;
	v1 = 0;
	out0 = a1;
	sh(a1, t0); sh(a1 + 2, t1);
	a1 += 2;

dc:   // coeficiente de continua de un bloque nuevo
	t0 = v0 >> 22;
	if (t5 == 0) goto dc_v2;
	a1 += 2;
	at = t0 ^ 1023;
	if (at == 0) goto eof;
	{
		s32 c = (s32)(t5 - 3);
		at = a2 - 1024;
		if (c >= 0) at -= 1024;   // luminancia: la otra tabla,
	}
	t0 = ((v0 >> 24) << 2) + at;
	t1 = lh(t0); t2 = lh(t0 + 2);
	t0 = 0;
	v0 <<= (t1 & 31);
	if (t2 != 0) {
		u32 old;
		at = 32 - t2;
		t0 = v0 >> (at & 31);
		old = v0;
		v0 <<= (t2 & 31);
		if ((s32)old >= 0) { t3 = 0xffffffffu >> (at & 31); t0 -= t3; }
		v1 += t2;
	}
	v1 += t1;
	REFILL(t1);
	at = t5 - 2;
	if ((s32)at > 0) { t1 = t9 + t0; t9 += t0; }        // Y
	else if (at == 0) { t1 = t8 + t0; t8 += t0; }       // Cb,
	else { t1 = t7 + t0; t7 += t0; }                    // Cr
	t1 = (t1 << 2) & 1023;
	t1 = t4 | t1;
	t5 += 1;
	at = t5 - 7;
	sh(a1, t1);
	n++;
	if (at == 0) t5 -= 6;
	goto next;

dc_v2:
	a1 += 2;
	at = t0 ^ 511;
	if (at == 0) goto eof;
	v0 <<= 10;
	v1 += 10;
	REFILL(t1);
	t0 = t4 | t0;
	sh(a1, t0);
	n++;

next:
	at = a1 - t6;
	a1 += 2;
	if ((s32)at >= 0) goto save;

loop:   // coeficientes de alterna: hasta 3 códigos por consulta a la tabla
	t0 = ((v0 >> 19) << 3) + a2;
	t1 = lw(t0);
	at = t1 & 255;
	if (t1 != 0) t3 = lw(t0 + 4);
	else {   // código largo: segunda tabla,
		v0 <<= 8;
		v1 += 8;
		REFILL(t0);
		t0 = ((v0 >> 23) << 2) + a3;
		t1 = lw(t0);
		t3 = 0;
		at = t1 & 255;
	}
	v0 <<= (at & 31);
	v1 += at;
	REFILL(t0);
	t1 >>= 16;
	if (t1 == 0x7c1f) { at = t1 ^ 0xfe00; goto esc; }
	at = t1 ^ 0xfe00;
	sh(a1, t1);
	n++;
	if (at == 0) goto dc;
	a1 += 2;
	if (t3 == 0) goto loop;
	t2 = t3 & 0xffff;
	if (t2 == 0x7c1f) { at = t2 ^ 0xfe00; goto esc; }
	at = t2 ^ 0xfe00;
	sh(a1, t2);
	n++;
	if (at == 0) goto dc;
	t2 = t3 >> 16;
	a1 += 2;
	if (t2 == 0) goto loop;
	if (t2 == 0x7c1f) { at = t2 ^ 0xfe00; goto esc; }
	at = t2 ^ 0xfe00;
	sh(a1, t2);
	n++;
	if (at == 0) goto dc;
	a1 += 2;
	goto loop;

esc:   // escape: el coeficiente va tal cual en los 16 bits siguientes
	t0 = v0 >> 16;
	sh(a1, t0);
	n++;
	a1 += 2;
	t0 = lh(a0);
	a0 += 2;
	v0 <<= 16;
	t0 <<= (v1 & 31);
	v0 |= t0;
	goto loop;

eof:   // fin de la imagen: 65 FE00h de relleno y el bit 17 del SR, como el original
	t0 = 0xfe00;
	v0 = 64;
	for (;;) {
		u32 c = v0;
		sh(a1, t0);
		a1 += 2;
		v0 -= 1;
		if (!c) break;
	}
	n += 65;
	t1 = cpu.cop0[12];
	at = 0x20000;
	t1 |= at;
	cpu.cop0[12] = t1;
	v0 = 0;
	goto done;

save:   // se llenó el tamaño máximo: guardar el estado y devolver 1
	t0 = state_addr;
	sw(t0, a0); sw(t0 + 4, a1); sw(t0 + 8, v0); sw(t0 + 12, v1); sw(t0 + 16, t4);
	sw(t0 + 20, t5); sw(t0 + 24, t7); sw(t0 + 28, t8); sw(t0 + 32, t9);
	v0 = 1;

done:
	r[1] = at; r[2] = v0; r[3] = v1; r[4] = a0; r[5] = a1; r[6] = a2; r[7] = a3;
	r[8] = t0; r[9] = t1; r[10] = t2; r[11] = t3; r[12] = t4; r[13] = t5; r[14] = t6; r[15] = t7;
	r[24] = t8; r[25] = t9;
	// tiempo de la PS1: bastante menos que el MIPS (unas 25 instrucciones por coeficiente), pero no gratis
	psx_cycles += 200 + n * 8;
#ifdef PSX_DYNAREC
	{   // por si la salida cae sobre código traducido (no debería: es un búfer de datos)
		u32 lo = out0 & 0x1fffffff, hi = a1 & 0x1fffffff;
		if (lo < 0x800000 && hi > lo && hi - lo < 0x200000) dyn_ram_written(lo & 0x1fffff, hi - lo);
	}
#endif
}

// ---- StGetNext (libpress): la siguiente imagen del anillo de sectores del CD ----
// Mientras no llega, el juego la pide una y otra vez (Klonoa hasta 2000 veces seguidas): en la DS eso era
// emular un bucle de espera. Aquí, si no está lista, el reloj salta al siguiente evento (como las esperas de
// VSync del dynarec): lo que el juego espera es al CD y nada cambia antes.
#define SGN_WORDS 49
static const u32 sgn_code[SGN_WORDS] = {
	0x00803821, 0x3c028011, 0x8c42e198, 0x3c038011, 0x8c63ee8c, 0x00021140, 0x00623021, 0x94c20000,
	0x24030001, 0x3042ffff, 0x1443000e, 0x00a04021, 0x3c028011, 0x8c42ee7c, 0x3c018011, 0xac20e198,
	0x10400002, 0x00000000, 0xa4c00000, 0x3c028011, 0x8c42e198, 0x3c038011, 0x8c63ee8c, 0x00021140,
	0x00623021, 0x94c20000, 0x24030002, 0x3042ffff, 0x14430012, 0x24020001, 0x24020004, 0xa4c20000,
	0x00001021, 0x3c038011, 0x8c6300b0, 0x3c048011, 0x8c84ee8c, 0x3c058011, 0x8ca5e198, 0x00031940,
	0x00832021, 0x00051980, 0x00651823, 0x00031940, 0x00832021, 0xace40000, 0xad060000, 0x03e00008,
	0x00000000,
};
// La misma en Tomb Raider (otro compilador: ori en vez de addiu, operandos en otro orden; mismas posiciones)
static const u32 sgn_code2[SGN_WORDS] = {
	0x00803821, 0x3c02801e, 0x8c42f7e8, 0x3c03801e, 0x8c630084, 0x00021140, 0x00433021, 0x94c20000,
	0x34030001, 0x3042ffff, 0x1443000e, 0x00a04021, 0x3c02801e, 0x8c420078, 0x3c01801e, 0xac20f7e8,
	0x10400002, 0x00000000, 0xa4c00000, 0x3c02801e, 0x8c42f7e8, 0x3c03801e, 0x8c630084, 0x00021140,
	0x00433021, 0x94c20000, 0x34030002, 0x3042ffff, 0x14430012, 0x34020001, 0x34020004, 0xa4c20000,
	0x00001021, 0x3c04801e, 0x8c8406c4, 0x3c03801e, 0x8c630084, 0x3c05801e, 0x8ca5f7e8, 0x00042140,
	0x00832021, 0x00051980, 0x00651823, 0x00031940, 0x00832021, 0xace40000, 0xad060000, 0x03e00008,
	0x00000000,
};
static const u32 *const sgn_variants[] = { sgn_code, sgn_code2 };

// Variables del juego: índice del anillo, base del anillo, la marca de «volver a empezar» y el desplazamiento
// de los datos; at_hi = lo que deja el lui de at
typedef struct { u32 idx, base, flag, off, at_hi; } SgnVars;

static int sgn_match(u32 pc, SgnVars *v)
{
	u32 w[SGN_WORDS];
	int found = 0;
	if (rd_word(pc) != sgn_code[0] || (rd_word(pc + 4) & 0xffff0000) != 0x3c020000) return 0;
	for (u32 k = 0; k < sizeof sgn_variants / sizeof sgn_variants[0] && !found; k++) {
		const u32 *c = sgn_variants[k];
		int i;
		for (i = 0; i < SGN_WORDS; i++) {
			int addr = i && ((c[i - 1] >> 26) == 0x0f || (c[i] >> 26) == 0x0f);   // lui y lo que lo usa
			w[i] = rd_word(pc + 4 * (u32)i);
			if (addr ? (w[i] & 0xffff0000) != (c[i] & 0xffff0000) : w[i] != c[i]) break;
		}
		found = i == SGN_WORDS;
	}
	if (!found) return 0;
	v->idx = lui_addiu(w[1] & 0xffff, w[2]);
	v->base = lui_addiu(w[3] & 0xffff, w[4]);
	v->flag = lui_addiu(w[12] & 0xffff, w[13]);
	v->off = lui_addiu(w[33] & 0xffff, w[34]);
	v->at_hi = w[14] << 16;
	return lui_addiu(w[14] & 0xffff, w[15]) == v->idx && lui_addiu(w[19] & 0xffff, w[20]) == v->idx &&
	       lui_addiu(w[37] & 0xffff, w[38]) == v->idx && lui_addiu(w[21] & 0xffff, w[22]) == v->base &&
	       lui_addiu(w[35] & 0xffff, w[36]) == v->base;
}

static void sgn_run(const SgnVars *s)
{
	u32 *r = cpu.r;
	u32 at = r[1], v0, v1, a0 = r[4], a1 = r[5], a2, a3, t0;
	a3 = a0;
	t0 = a1;
	v0 = lw(s->idx) << 5;
	a2 = lw(s->base) + v0;
	v0 = lh(a2);
	v1 = 1;
	if (v0 == v1) {   // el anillo dio la vuelta
		v0 = lw(s->flag);
		at = s->at_hi;
		sw(s->idx, 0);
		if (v0 != 0) sh(a2, 0);
		v0 = lw(s->idx) << 5;
		a2 = lw(s->base) + v0;
	}
	v0 = lh(a2);
	v1 = 2;
	if (v0 != v1) {   // aún no ha llegado: hasta el siguiente evento no cambia nada
		v0 = 1;
		psx_cycles += 60;
		if (TIME_BEFORE(psx_cycles, psx_run_until)) psx_cycles = psx_run_until;
	} else {
		sh(a2, 4);
		v1 = lw(s->off) << 5;
		a0 = lw(s->base);
		a1 = lw(s->idx);
		a0 += v1;
		v1 = ((a1 << 6) - a1) << 5;
		a0 += v1;
		sw(a3, a0);
		sw(t0, a2);
		v0 = 0;
		psx_cycles += 80;
	}
	r[1] = at; r[2] = v0; r[3] = v1; r[4] = a0; r[5] = a1; r[6] = a2; r[7] = a3; r[8] = t0;
}

// ---- interfaz ----
int psx_hle_off;   // 1 = sin HLE (comparar con la rutina emulada)

enum { HLE_NONE, HLE_VLC, HLE_SGN };
typedef struct { u32 pc, kind, a, b; SgnVars sv; } HleFn;
static HleFn fns[4];   // las últimas reconocidas (normalmente una de cada)
static u32 fn_next;

static int hle_find(u32 pc, HleFn *f)
{
	f->pc = pc;
	if (vlc_match(pc, &f->a, &f->b)) { f->kind = HLE_VLC; return VLC_WORDS; }
	if (sgn_match(pc, &f->sv)) { f->kind = HLE_SGN; return SGN_WORDS; }
	f->kind = HLE_NONE;
	return 0;
}

static void hle_run(const HleFn *f)
{
	if (f->kind == HLE_VLC) vlc_run(f->a, f->b);
	else sgn_run(&f->sv);
	cpu.pc = cpu.r[31];
}

// El dynarec pregunta al traducir un bloque que empieza en pc: nº de palabras de la rutina (0 = ninguna). fetch
// (el del dynarec) marca toda la rutina como código: si el juego la pisa, el bloque se retraduce.
int hle_match(u32 pc, u32 (*fetch)(u32))
{
	HleFn f;
	int n;
	if (psx_hle_off || !(n = hle_find(pc, &f))) return 0;
	for (int i = 0; i < n; i++) fetch(pc + 4 * (u32)i);
	fns[fn_next++ & 3] = f;
	return n;
}

// La llamada desde el código traducido: hace la rutina y vuelve a ra. Devuelve el PC siguiente.
u32 hle_call(u32 pc)
{
	HleFn f;
	for (int i = 0; i < 4; i++)
		if (fns[i].pc == pc && fns[i].kind) { hle_run(&fns[i]); return cpu.pc; }
	if (!hle_find(pc, &f)) return pc;   // no pasa: si el código cambia, el bloque se retraduce antes
	fns[fn_next++ & 3] = f;
	hle_run(&f);
	return cpu.pc;
}

// Para el intérprete (banco de pruebas del PC), antes de cada instrucción: 1 = hecha la rutina, cpu.pc = ra
int hle_interp(void)
{
	u32 pc = cpu.pc, w0;
	HleFn f;
	if (psx_hle_off) return 0;
	w0 = rd_word(pc);
	if ((w0 & 0xffff0000) != 0x3c080000 && w0 != sgn_code[0]) return 0;   // filtro rápido
	if (!hle_find(pc, &f)) return 0;
#ifdef HLE_LOG
	{ static u32 seen[8]; int k = 0; while (k < 8 && seen[k] && seen[k] != pc) k++; if (k < 8 && !seen[k]) { seen[k] = pc; printf("HLE%u en %08x\n", f.kind, pc); } }
#endif
	if (cpu.lr) { cpu.r[cpu.lr] = cpu.lv; cpu.lr = 0; }
#ifdef HLE_CHECK
	{   // prueba: la rutina nativa y luego la emulada desde el mismo punto; deben dejar lo mismo
		static u8 *snap, *res;
		static u32 calls[3], bad[3];
		Cpu c0 = cpu, c1;
		u32 ra = cpu.r[31], sr, nb = 0, nr = 0, cyc = psx_cycles, until = psx_run_until;
		if (!snap) { snap = malloc(0x200000); res = malloc(0x200000); }
		memcpy(snap, psx_ram, 0x200000);
		hle_run(&f);
		c1 = cpu;
		memcpy(res, psx_ram, 0x200000);
		memcpy(psx_ram, snap, 0x200000);
		cpu = c0;
		psx_cycles = cyc;
		psx_hle_off = 1;
		sr = cpu.cop0[12];
		cpu.cop0[12] &= ~1u;
		cpu.next_pc = cpu.pc + 4;
		cpu.branching = 0;
		while (cpu.pc != ra || cpu.branching) cpu_run(psx_cycles + 2);
		if (cpu.lr) { cpu.r[cpu.lr] = cpu.lv; cpu.lr = 0; }
		psx_hle_off = 0;
		psx_run_until = until;
		cpu.cop0[12] = (cpu.cop0[12] & ~1u) | (sr & 1);
		for (int i = 1; i < 32; i++)
			if (cpu.r[i] != c1.r[i] && !(f.kind == HLE_VLC && i == 9)) { if (nr++ < 4) printf("HLE%u r%d: %08x nativo %08x\n", f.kind, i, cpu.r[i], c1.r[i]); }
		if (cpu.cop0[12] != c1.cop0[12]) printf("HLE SR %08x nativo %08x\n", cpu.cop0[12], c1.cop0[12]);
		for (u32 i = 0; i < 0x200000; i++) if (psx_ram[i] != res[i]) { if (nb++ < 4) printf("HLE%u ram %06x: %02x nativo %02x\n", f.kind, i, psx_ram[i], res[i]); }
		calls[f.kind]++;
		if (nr || nb) bad[f.kind]++;
		if (nr || nb || calls[f.kind] % 500 == 0) printf("HLE%u comprobadas %u, distintas %u (v0=%u)\n", f.kind, calls[f.kind], bad[f.kind], cpu.r[2]);
		return 1;
	}
#endif
	hle_run(&f);
	return 1;
}
