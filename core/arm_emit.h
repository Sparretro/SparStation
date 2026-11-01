// Emisor de instrucciones ARM (ARMv5TE, modo ARM) para el dynarec. Solo lo que el traductor necesita.
#pragma once
#include "psx.h"

enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC };
enum { C_EQ, C_NE, C_CS, C_CC, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL };
enum { OP_AND, OP_EOR, OP_SUB, OP_RSB, OP_ADD, OP_ADC, OP_SBC, OP_RSC, OP_TST, OP_TEQ, OP_CMP, OP_CMN, OP_ORR, OP_MOV, OP_BIC, OP_MVN };
enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR };

extern u32 *emit_ptr;

static inline void emit(u32 w) { *emit_ptr++ = w; }

// Codifica un inmediato de 32 bits como 8 bits rotados; devuelve -1 si no se puede
static inline int arm_imm(u32 v)
{
	int tz;
	if (v < 256) return (int)v;
	tz = __builtin_ctz(v) & ~1;   // los 8 bits empiezan en una posición par
	if ((v >> tz) < 256) return (((32 - tz) >> 1) << 8) | (int)(v >> tz);
	if ((v & 0xffffff00u) && (v & 0xff)) {   // bits en los dos extremos: rotaciones que dan la vuelta
		for (int r = 1; r < 4; r++) {
			u32 x = (v << (r * 2)) | (v >> (32 - r * 2));
			if (x < 256) return (r << 8) | (int)x;
		}
	}
	return -1;
}

// Procesamiento de datos con registro desplazado por inmediato
static inline void a_dp_reg(int cond, int op, int s, int rd, int rn, int rm, int sh, int amt)
{
	emit(((u32)cond << 28) | ((u32)op << 21) | ((u32)s << 20) | ((u32)rn << 16) | ((u32)rd << 12) | ((u32)(amt & 31) << 7) |
	     ((u32)sh << 5) | (u32)rm);
}

// Procesamiento de datos con registro desplazado por registro
static inline void a_dp_regsh(int cond, int op, int s, int rd, int rn, int rm, int sh, int rs)
{
	emit(((u32)cond << 28) | ((u32)op << 21) | ((u32)s << 20) | ((u32)rn << 16) | ((u32)rd << 12) | ((u32)rs << 8) |
	     ((u32)sh << 5) | 0x10 | (u32)rm);
}

// Procesamiento de datos con inmediato ya codificado (rot<<8 | imm8)
static inline void a_dp_imm(int cond, int op, int s, int rd, int rn, int enc)
{
	emit(((u32)cond << 28) | (1u << 25) | ((u32)op << 21) | ((u32)s << 20) | ((u32)rn << 16) | ((u32)rd << 12) | (u32)enc);
}

#define a_mov(rd, rm) a_dp_reg(C_AL, OP_MOV, 0, rd, 0, rm, SH_LSL, 0)

// LDR/STR con desplazamiento inmediato (12 bits, con signo)
static inline void a_mem_imm(int cond, int load, int byte, int rd, int rn, int off)
{
	int up = off >= 0;
	if (!up) off = -off;
	emit(((u32)cond << 28) | (1u << 26) | (1u << 24) | ((u32)up << 23) | ((u32)byte << 22) | ((u32)load << 20) |
	     ((u32)rn << 16) | ((u32)rd << 12) | (u32)(off & 0xfff));
}
#define a_ldr(rd, rn, off) a_mem_imm(C_AL, 1, 0, rd, rn, off)
#define a_str(rd, rn, off) a_mem_imm(C_AL, 0, 0, rd, rn, off)

// LDR/STR con registro de desplazamiento (sin shift)
static inline void a_mem_reg(int cond, int load, int byte, int rd, int rn, int rm)
{
	emit(((u32)cond << 28) | (3u << 25) | (1u << 24) | (1u << 23) | ((u32)byte << 22) | ((u32)load << 20) |
	     ((u32)rn << 16) | ((u32)rd << 12) | (u32)rm);
}

// LDR/STR/LDRB/STRB con registro de desplazamiento desplazado: [rn, rm, sh #amt]
static inline void a_mem_regsh(int cond, int load, int byte, int rd, int rn, int rm, int sh, int amt)
{
	emit(((u32)cond << 28) | (3u << 25) | (1u << 24) | (1u << 23) | ((u32)byte << 22) | ((u32)load << 20) |
	     ((u32)rn << 16) | ((u32)rd << 12) | ((u32)(amt & 31) << 7) | ((u32)sh << 5) | (u32)rm);
}

// LDRH/STRH/LDRSH/LDRSB con inmediato de 8 bits (con signo, -255..255); sh: 1 = H, 2 = SB, 3 = SH
static inline void a_memh_imm(int cond, int load, int sh, int rd, int rn, int off)
{
	int up = off >= 0;
	if (!up) off = -off;
	emit(((u32)cond << 28) | (1u << 24) | ((u32)up << 23) | (1u << 22) | ((u32)load << 20) | ((u32)rn << 16) |
	     ((u32)rd << 12) | ((u32)(off >> 4) << 8) | (1u << 7) | ((u32)sh << 5) | (1u << 4) | (u32)(off & 15));
}

// LDRH/STRH/LDRSH/LDRSB con registro de desplazamiento; sh: 1 = H, 2 = SB, 3 = SH
static inline void a_memh_reg(int cond, int load, int sh, int rd, int rn, int rm)
{
	emit(((u32)cond << 28) | (1u << 24) | (1u << 23) | ((u32)load << 20) | ((u32)rn << 16) | ((u32)rd << 12) |
	     (1u << 7) | ((u32)sh << 5) | (1u << 4) | (u32)rm);
}

// Saltos: B/BL a una dirección absoluta (±32 MB)
static inline void a_b(int cond, int link, const void *target)
{
	s32 off = (s32)((const u8 *)target - ((const u8 *)emit_ptr + 8)) >> 2;
	emit(((u32)cond << 28) | (5u << 25) | ((u32)link << 24) | ((u32)off & 0xffffff));
}

static inline void a_blx_reg(int rm) { emit(0xe12fff30 | (u32)rm); }
static inline void a_bx(int rm) { emit(0xe12fff10 | (u32)rm); }
static inline void a_bx_cond(int cond, int rm) { emit(((u32)cond << 28) | 0x012fff10u | (u32)rm); }

// MUL/MLA y multiplicaciones largas
static inline void a_mul(int rd, int rm, int rs) { emit(0xe0000090 | ((u32)rd << 16) | ((u32)rs << 8) | (u32)rm); }
static inline void a_smull(int lo, int hi, int rm, int rs) { emit(0xe0c00090 | ((u32)hi << 16) | ((u32)lo << 12) | ((u32)rs << 8) | (u32)rm); }
static inline void a_umull(int lo, int hi, int rm, int rs) { emit(0xe0800090 | ((u32)hi << 16) | ((u32)lo << 12) | ((u32)rs << 8) | (u32)rm); }

// PUSH/POP {lista}
static inline void a_push(u32 list) { emit(0xe92d0000 | list); }
static inline void a_pop(u32 list) { emit(0xe8bd0000 | list); }

// Carga una constante de 32 bits en un registro (MOV/MVN o MOV + ORR...)
static inline void a_movi(int rd, u32 v)
{
	int e = arm_imm(v);
	if (e >= 0) { a_dp_imm(C_AL, OP_MOV, 0, rd, 0, e); return; }
	e = arm_imm(~v);
	if (e >= 0) { a_dp_imm(C_AL, OP_MVN, 0, rd, 0, e); return; }
	{
		// trozos de 8 bits que empiezan en el bit par más bajo que quede: suele salir una instrucción
		// menos que cortando por bytes (0x8003be14 = 3 en vez de 4); si con MVN + BIC sale menos, así
		int n_or = 0, n_bic = 0;
		for (u32 r = v; r; n_or++) r &= ~(0xffu << (__builtin_ctz(r) & ~1));
		for (u32 r = ~v; r; n_bic++) r &= ~(0xffu << (__builtin_ctz(r) & ~1));
		if (n_bic < n_or) {
			u32 r = ~v;
			int first = 1;
			while (r) {
				u32 part = r & (0xffu << (__builtin_ctz(r) & ~1));
				r &= ~part;
				if (first) a_dp_imm(C_AL, OP_MVN, 0, rd, 0, arm_imm(part));
				else a_dp_imm(C_AL, OP_BIC, 0, rd, rd, arm_imm(part));
				first = 0;
			}
		} else {
			u32 r = v;
			int first = 1;
			while (r) {
				u32 part = r & (0xffu << (__builtin_ctz(r) & ~1));
				r &= ~part;
				if (first) a_dp_imm(C_AL, OP_MOV, 0, rd, 0, arm_imm(part));
				else a_dp_imm(C_AL, OP_ORR, 0, rd, rd, arm_imm(part));
				first = 0;
			}
		}
	}
}

// rd = rn + v (v cualquiera), usando tmp si hace falta
static inline void a_addi(int rd, int rn, u32 v, int tmp)
{
	int e = arm_imm(v);
	if (v == 0) { if (rd != rn) a_mov(rd, rn); return; }
	if (e >= 0) { a_dp_imm(C_AL, OP_ADD, 0, rd, rn, e); return; }
	e = arm_imm(-v);
	if (e >= 0) { a_dp_imm(C_AL, OP_SUB, 0, rd, rn, e); return; }
	a_movi(tmp, v);
	a_dp_reg(C_AL, OP_ADD, 0, rd, rn, tmp, SH_LSL, 0);
}
