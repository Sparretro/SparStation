// GTE (Geometry Transformation Engine): el coprocesador 2 que hace la geometría 3D en punto fijo.
// Sigue la documentación psx-spx, incluida la división UNR exacta que usan los juegos para la perspectiva.
#include "psx.h"

// Los registros van en la DTCM, dentro de psx_core justo delante de cpu (psx.h): el dynarec los alcanza desde
// r11 con desplazamientos cortos. Aquí se usan por su nombre.
#define V (psx_core.gte.V)
#define RGBC (psx_core.gte.RGBC)
#define OTZ (psx_core.gte.OTZ)
#define IR (psx_core.gte.IR)
#define SX (psx_core.gte.SX)
#define SY (psx_core.gte.SY)
#define SZ (psx_core.gte.SZ)
#define CF (psx_core.gte.CF)
#define RES1 (psx_core.gte.RES1)
#define MAC (psx_core.gte.MAC)
#define LZCS (psx_core.gte.LZCS)
#define LZCR (psx_core.gte.LZCR)
#define RT (psx_core.gte.RT)
#define LM (psx_core.gte.LM)
#define LC (psx_core.gte.LC)
#define TR (psx_core.gte.TR)
#define BK (psx_core.gte.BK)
#define FC (psx_core.gte.FC)
#define OFX (psx_core.gte.OFX)
#define OFY (psx_core.gte.OFY)
#define DQB (psx_core.gte.DQB)
#define H (psx_core.gte.H)
#define DQA (psx_core.gte.DQA)
#define ZSF3 (psx_core.gte.ZSF3)
#define ZSF4 (psx_core.gte.ZSF4)
#define FLAG (psx_core.gte.FLAG)

PSX_FAST static int sf, lm;
PSX_FAST static u8 unr[257];
PSX_FAST static int unr_ready;

static inline s32 clampi(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- registros ----
static u32 pack16(s16 a, s16 b) { return (u16)a | ((u32)(u16)b << 16); }

static u32 irgb(void)
{
	return clampi(IR[1] >> 7, 0, 31) | (clampi(IR[2] >> 7, 0, 31) << 5) | (clampi(IR[3] >> 7, 0, 31) << 10);
}

u32 gte_read(int r)
{
	switch (r) {
	case 0: case 2: case 4: return pack16(V[r / 2][0], V[r / 2][1]);
	case 1: case 3: case 5: return (u32)(s32)V[r / 2][2];
	case 6: return RGBC[0] | (RGBC[1] << 8) | (RGBC[2] << 16) | ((u32)RGBC[3] << 24);
	case 7: return OTZ;
	case 8: case 9: case 10: case 11: return (u32)(s32)IR[r - 8];
	case 12: case 13: case 14: return pack16(SX[r - 12], SY[r - 12]);
	case 15: return pack16(SX[2], SY[2]);
	case 16: case 17: case 18: case 19: return SZ[r - 16];
	case 20: case 21: case 22: { u8 *c = CF[r - 20]; return c[0] | (c[1] << 8) | (c[2] << 16) | ((u32)c[3] << 24); }
	case 23: return RES1;
	case 24: case 25: case 26: case 27: return (u32)MAC[r - 24];
	case 28: case 29: return irgb();
	case 30: return LZCS;
	case 31: return LZCR;
	// control
	case 32: case 40: case 48: { s16 (*m)[3] = r == 32 ? RT : r == 40 ? LM : LC; return pack16(m[0][0], m[0][1]); }
	case 33: case 41: case 49: { s16 (*m)[3] = r == 33 ? RT : r == 41 ? LM : LC; return pack16(m[0][2], m[1][0]); }
	case 34: case 42: case 50: { s16 (*m)[3] = r == 34 ? RT : r == 42 ? LM : LC; return pack16(m[1][1], m[1][2]); }
	case 35: case 43: case 51: { s16 (*m)[3] = r == 35 ? RT : r == 43 ? LM : LC; return pack16(m[2][0], m[2][1]); }
	case 36: case 44: case 52: { s16 (*m)[3] = r == 36 ? RT : r == 44 ? LM : LC; return (u32)(s32)m[2][2]; }
	case 37: case 38: case 39: return (u32)TR[r - 37];
	case 45: case 46: case 47: return (u32)BK[r - 45];
	case 53: case 54: case 55: return (u32)FC[r - 53];
	case 56: return (u32)OFX;
	case 57: return (u32)OFY;
	case 58: return (u32)(s32)(s16)H;   // fallo del hardware: H se lee con signo
	case 59: return (u32)(s32)DQA;
	case 60: return (u32)DQB;
	case 61: return (u32)(s32)ZSF3;
	case 62: return (u32)(s32)ZSF4;
	case 63: {
		u32 f = FLAG & 0x7ffff000;
		if (f & 0x7f87e000) f |= 1u << 31;
		return f;
	}
	}
	return 0;
}

void gte_write(int r, u32 v)
{
	switch (r) {
	case 0: case 2: case 4: V[r / 2][0] = (s16)v; V[r / 2][1] = (s16)(v >> 16); break;
	case 1: case 3: case 5: V[r / 2][2] = (s16)v; break;
	case 6: RGBC[0] = (u8)v; RGBC[1] = (u8)(v >> 8); RGBC[2] = (u8)(v >> 16); RGBC[3] = (u8)(v >> 24); break;
	case 7: OTZ = (u16)v; break;
	case 8: case 9: case 10: case 11: IR[r - 8] = (s16)v; break;
	case 12: case 13: case 14: SX[r - 12] = (s16)v; SY[r - 12] = (s16)(v >> 16); break;
	case 15:
		SX[0] = SX[1]; SY[0] = SY[1];
		SX[1] = SX[2]; SY[1] = SY[2];
		SX[2] = (s16)v; SY[2] = (s16)(v >> 16);
		break;
	case 16: case 17: case 18: case 19: SZ[r - 16] = (u16)v; break;
	case 20: case 21: case 22: { u8 *c = CF[r - 20]; c[0] = (u8)v; c[1] = (u8)(v >> 8); c[2] = (u8)(v >> 16); c[3] = (u8)(v >> 24); break; }
	case 23: RES1 = v; break;
	case 24: case 25: case 26: case 27: MAC[r - 24] = (s32)v; break;
	case 28:
		IR[1] = (s16)((v & 0x1f) << 7);
		IR[2] = (s16)(((v >> 5) & 0x1f) << 7);
		IR[3] = (s16)(((v >> 10) & 0x1f) << 7);
		break;
	case 29: break;
	case 30: {
		u32 x = (v & 0x80000000) ? ~v : v;
		int n = 0;
		LZCS = v;
		while (n < 32 && !(x & 0x80000000)) { x <<= 1; n++; }
		LZCR = n;
		break;
	}
	case 31: break;
	case 32: case 40: case 48: { s16 (*m)[3] = r == 32 ? RT : r == 40 ? LM : LC; m[0][0] = (s16)v; m[0][1] = (s16)(v >> 16); break; }
	case 33: case 41: case 49: { s16 (*m)[3] = r == 33 ? RT : r == 41 ? LM : LC; m[0][2] = (s16)v; m[1][0] = (s16)(v >> 16); break; }
	case 34: case 42: case 50: { s16 (*m)[3] = r == 34 ? RT : r == 42 ? LM : LC; m[1][1] = (s16)v; m[1][2] = (s16)(v >> 16); break; }
	case 35: case 43: case 51: { s16 (*m)[3] = r == 35 ? RT : r == 43 ? LM : LC; m[2][0] = (s16)v; m[2][1] = (s16)(v >> 16); break; }
	case 36: case 44: case 52: { s16 (*m)[3] = r == 36 ? RT : r == 44 ? LM : LC; m[2][2] = (s16)v; break; }
	case 37: case 38: case 39: TR[r - 37] = (s32)v; break;
	case 45: case 46: case 47: BK[r - 45] = (s32)v; break;
	case 53: case 54: case 55: FC[r - 53] = (s32)v; break;
	case 56: OFX = (s32)v; break;
	case 57: OFY = (s32)v; break;
	case 58: H = (u16)v; break;
	case 59: DQA = (s16)v; break;
	case 60: DQB = (s32)v; break;
	case 61: ZSF3 = (s16)v; break;
	case 62: ZSF4 = (s16)v; break;
	case 63: FLAG = v & 0x7ffff000; break;
	}
}

static int part2(GteRegPart *p, void *lo, void *hi)
{
	p[0].addr = lo; p[0].bits = 16; p[0].sign = 0;
	p[1].addr = hi; p[1].bits = 16; p[1].sign = 0;
	return 2;
}

static int part1(GteRegPart *p, void *a, int bits, int sign)
{
	p[0].addr = a; p[0].bits = (u8)bits; p[0].sign = (u8)sign;
	return 1;
}

int gte_reg_parts(int r, int write, GteRegPart p[2])
{
	if (r >= 32 && r < 56 && (r & 7) < 5) {   // matrices RT, LM y LC: pares de s16 seguidos
		s16 *m = r < 40 ? &RT[0][0] : r < 48 ? &LM[0][0] : &LC[0][0];
		int k = r & 7;
		return k < 4 ? part2(p, m + 2 * k, m + 2 * k + 1) : part1(p, m + 8, 16, 1);
	}
	switch (r) {
	case 0: case 2: case 4: return part2(p, &V[r / 2][0], &V[r / 2][1]);
	case 1: case 3: case 5: return part1(p, &V[r / 2][2], 16, 1);
	case 6: return part1(p, RGBC, 32, 0);
	case 7: return part1(p, &OTZ, 16, 0);
	case 8: case 9: case 10: case 11: return part1(p, &IR[r - 8], 16, 1);
	case 12: case 13: case 14: return part2(p, &SX[r - 12], &SY[r - 12]);
	case 15: return write ? 0 : part2(p, &SX[2], &SY[2]);   // escribir SXYP mueve la FIFO
	case 16: case 17: case 18: case 19: return part1(p, &SZ[r - 16], 16, 0);
	case 20: case 21: case 22: return part1(p, CF[r - 20], 32, 0);
	case 23: return part1(p, &RES1, 32, 0);
	case 24: case 25: case 26: case 27: return part1(p, &MAC[r - 24], 32, 0);
	case 30: return write ? 0 : part1(p, &LZCS, 32, 0);
	case 31: return write ? 0 : part1(p, &LZCR, 32, 0);
	case 37: case 38: case 39: return part1(p, &TR[r - 37], 32, 0);
	case 45: case 46: case 47: return part1(p, &BK[r - 45], 32, 0);
	case 53: case 54: case 55: return part1(p, &FC[r - 53], 32, 0);
	case 56: return part1(p, &OFX, 32, 0);
	case 57: return part1(p, &OFY, 32, 0);
	case 58: return part1(p, &H, 16, 1);   // se lee con signo (ver gte_read)
	case 59: return part1(p, &DQA, 16, 1);
	case 60: return part1(p, &DQB, 32, 0);
	case 61: return part1(p, &ZSF3, 16, 1);
	case 62: return part1(p, &ZSF4, 16, 1);
	}
	return 0;   // IRGB/ORGB, LZCS al escribir, FLAG: con gte_read/gte_write
}

// ---- aritmética con banderas ----
#define GTE_INL static inline __attribute__((always_inline))   // las pequeñas, integradas: se llaman muchísimo
GTE_INL s64 chk44(int i, s64 v)
{
	if (v > 0x7ffffffffffLL) FLAG |= 1u << (31 - i);
	else if (v < -0x80000000000LL) FLAG |= 1u << (28 - i);
	return (v << 20) >> 20;
}

GTE_INL void set_mac(int i, s64 v) { MAC[i] = (s32)(chk44(i, v) >> sf); }
GTE_INL void set_mac_s(int i, s64 v, int s) { MAC[i] = (s32)(chk44(i, v) >> s); }

GTE_INL void set_ir(int i, s32 v, int lmv)
{
	s32 lo = lmv ? 0 : -0x8000;
	if (v < lo) { v = lo; FLAG |= 1u << (25 - i); }
	else if (v > 0x7fff) { v = 0x7fff; FLAG |= 1u << (25 - i); }
	IR[i] = (s16)v;
}

GTE_INL void mac_to_ir(void) { set_ir(1, MAC[1], lm); set_ir(2, MAC[2], lm); set_ir(3, MAC[3], lm); }

GTE_INL void set_mac0(s64 v)
{
	if (v > 0x7fffffffLL) FLAG |= 1u << 16;
	else if (v < -0x80000000LL) FLAG |= 1u << 15;
	MAC[0] = (s32)v;
}

static void set_otz(s64 full)
{
	s64 z = full >> 12;   // se satura con el valor completo, no con MAC0 recortado
	if (z < 0) { z = 0; FLAG |= 1u << 18; }
	else if (z > 0xffff) { z = 0xffff; FLAG |= 1u << 18; }
	OTZ = (u16)z;
}

GTE_INL u8 col_sat(int i, s32 v)
{
	if (v < 0) { FLAG |= 1u << (22 - i); return 0; }
	if (v > 255) { FLAG |= 1u << (22 - i); return 255; }
	return (u8)v;
}

static void push_color(void)
{
	memcpy(CF[0], CF[1], 4);
	memcpy(CF[1], CF[2], 4);
	CF[2][0] = col_sat(1, MAC[1] >> 4);
	CF[2][1] = col_sat(2, MAC[2] >> 4);
	CF[2][2] = col_sat(3, MAC[3] >> 4);
	CF[2][3] = RGBC[3];
}

GTE_INL void push_sz(s32 z)
{
	SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3];
	if (z < 0) { z = 0; FLAG |= 1u << 18; }
	else if (z > 0xffff) { z = 0xffff; FLAG |= 1u << 18; }
	SZ[3] = (u16)z;
}

GTE_INL void push_sxy(s32 x, s32 y)
{
	SX[0] = SX[1]; SY[0] = SY[1];
	SX[1] = SX[2]; SY[1] = SY[2];
	if (x < -0x400) { x = -0x400; FLAG |= 1u << 14; } else if (x > 0x3ff) { x = 0x3ff; FLAG |= 1u << 14; }
	if (y < -0x400) { y = -0x400; FLAG |= 1u << 13; } else if (y > 0x3ff) { y = 0x3ff; FLAG |= 1u << 13; }
	SX[2] = (s16)x;
	SY[2] = (s16)y;
}

static u32 divide(void)
{
	u32 n, d, u;
	int z = 0;
	u16 sz = SZ[3];
	if (H >= sz * 2u) { FLAG |= 1u << 17; return 0x1ffff; }
	z = sz ? __builtin_clz(sz) - 16 : 16;
	n = (u32)H << z;
	d = (u32)sz << z;
	u = unr[(d - 0x7fc0) >> 7] + 0x101;
	d = (0x2000080 - (d * u)) >> 8;
	d = (0x0000080 + (d * u)) >> 8;
	{
		u64 q = ((u64)n * d + 0x8000) >> 16;
		return q > 0x1ffff ? 0x1ffff : (u32)q;
	}
}

// ---- operaciones ----
// M*V + add<<12. Con |add| < 2^30 y factores de 16 bits la suma no pasa de 2^43: no puede desbordar
// los 44 bits y no hace falta mirar las banderas en cada paso (el caso normal, mucho más rápido).
GTE_INL s64 mat_row(const s16 *m, const s32 *add, int i, s32 vx, s32 vy, s32 vz)
{
	s64 t = add ? (s64)add[i] << 12 : 0;
	if (add && (add[i] >= 0x40000000 || add[i] < -0x40000000)) {
		t = chk44(i + 1, t + (s64)m[0] * vx);
		t = chk44(i + 1, t + (s64)m[1] * vy);
		return chk44(i + 1, t + (s64)m[2] * vz);
	}
	return t + (s64)(m[0] * vx) + (s64)(m[1] * vy) + (s64)(m[2] * vz);
}

static void mat_vec(s16 m[3][3], const s32 *add, s32 vx, s32 vy, s32 vz)
{
	for (int i = 0; i < 3; i++) MAC[i + 1] = (s32)(mat_row(m[i], add, i, vx, vy, vz) >> sf);
}

GTE_INL void rtp_s(int vi, int last, int s)
{
	s64 z;
	u32 n;
	s32 vx = V[vi][0], vy = V[vi][1], vz = V[vi][2];
	MAC[1] = (s32)(mat_row(RT[0], TR, 0, vx, vy, vz) >> s);
	MAC[2] = (s32)(mat_row(RT[1], TR, 1, vx, vy, vz) >> s);
	z = mat_row(RT[2], TR, 2, vx, vy, vz);
	MAC[3] = (s32)(z >> s);
	set_ir(1, MAC[1], lm);
	set_ir(2, MAC[2], lm);
	// IR3: la bandera mira MAC3>>12 aunque el valor se recorta con el MAC3 normal
	{
		s32 z12 = (s32)(z >> 12);
		if (z12 < -0x8000 || z12 > 0x7fff) FLAG |= 1u << 22;
		IR[3] = (s16)clampi(MAC[3], lm ? 0 : -0x8000, 0x7fff);
		push_sz(z12);
	}
	n = divide();
	{
		s64 x = (s64)n * IR[1] + OFX, y = (s64)n * IR[2] + OFY;
		set_mac0(x);
		set_mac0(y);
		push_sxy((s32)(x >> 16), (s32)(y >> 16));
	}
	if (last) {
		s64 d = (s64)n * DQA + DQB;
		set_mac0(d);
		{
			s32 ir0 = (s32)(d >> 12);
			if (ir0 < 0) { ir0 = 0; FLAG |= 1u << 12; } else if (ir0 > 0x1000) { ir0 = 0x1000; FLAG |= 1u << 12; }
			IR[0] = (s16)ir0;
		}
	}
}

// con sf fijo (12 o 0) los desplazamientos de 64 bits salen mucho más cortos
static __attribute__((noinline)) void rtp12(int vi, int last) { rtp_s(vi, last, 12); }   // una copia por desplazamiento (la ITCM va justa)
static __attribute__((noinline)) void rtp0(int vi, int last) { rtp_s(vi, last, 0); }
static void rtp12f(int vi, int last);
static void rtp(int vi, int last) { if (sf) rtp12f(vi, last); else rtp0(vi, last); }

// RTP con sf = 12, la versión rápida: las banderas se juntan en un registro y van a FLAG una sola vez (cada
// «FLAG |=» era leer y escribir memoria), y la división va en línea. Si TR es enorme (|TR| >= 2^30, puede
// desbordar 44 bits) se usa la general. Mismo resultado y banderas que rtp_s (out/t/rtpfast.c lo comprueba).
static __attribute__((noinline)) void rtp12f(int vi, int last)
{
	u32 fl = 0, n;
	s32 vx = V[vi][0], vy = V[vi][1], vz = V[vi][2], mac1, mac2, z12, ir1, ir2, ir3, lo = lm ? 0 : -0x8000;
	s64 m3;
	if (((u32)(TR[0] + 0x40000000) | (u32)(TR[1] + 0x40000000) | (u32)(TR[2] + 0x40000000)) >= 0x80000000u) { rtp12(vi, last); return; }
	mac1 = (s32)((((s64)TR[0] << 12) + (s64)(RT[0][0] * vx) + (s64)(RT[0][1] * vy) + (s64)(RT[0][2] * vz)) >> 12);
	mac2 = (s32)((((s64)TR[1] << 12) + (s64)(RT[1][0] * vx) + (s64)(RT[1][1] * vy) + (s64)(RT[1][2] * vz)) >> 12);
	m3 = ((s64)TR[2] << 12) + (s64)(RT[2][0] * vx) + (s64)(RT[2][1] * vy) + (s64)(RT[2][2] * vz);
	z12 = (s32)(m3 >> 12);
	MAC[1] = mac1;
	MAC[2] = mac2;
	MAC[3] = z12;
	ir1 = mac1 < lo ? lo : mac1 > 0x7fff ? 0x7fff : mac1;
	ir2 = mac2 < lo ? lo : mac2 > 0x7fff ? 0x7fff : mac2;
	ir3 = z12 < lo ? lo : z12 > 0x7fff ? 0x7fff : z12;
	if (ir1 != mac1) fl |= 1u << 24;
	if (ir2 != mac2) fl |= 1u << 23;
	if ((u32)(z12 + 0x8000) > 0xffff) fl |= 1u << 22;   // IR3: la bandera mira MAC3 sin lm
	IR[1] = (s16)ir1;
	IR[2] = (s16)ir2;
	IR[3] = (s16)ir3;
	SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3];
	if ((u32)z12 > 0xffff) { SZ[3] = z12 < 0 ? 0 : 0xffff; fl |= 1u << 18; } else SZ[3] = (u16)z12;
	{   // división UNR (como divide())
		u32 sz = SZ[3];
		if (H >= sz * 2) { fl |= 1u << 17; n = 0x1ffff; }
		else {
			int sh = __builtin_clz(sz) - 16;   // sz > 0 aquí: H < 2*sz
			u32 d = sz << sh, u = unr[(d - 0x7fc0) >> 7] + 0x101;
			u64 q;
			d = (0x2000080 - (d * u)) >> 8;
			d = (0x0000080 + (d * u)) >> 8;
			q = ((u64)((u32)H << sh) * d + 0x8000) >> 16;
			n = q > 0x1ffff ? 0x1ffff : (u32)q;
		}
	}
	{
		s64 x = (s64)n * ir1 + OFX, y = (s64)n * ir2 + OFY;
		s32 sx = (s32)(x >> 16), sy = (s32)(y >> 16);
		if (x > 0x7fffffffLL || y > 0x7fffffffLL) fl |= 1u << 16;
		if (x < -0x80000000LL || y < -0x80000000LL) fl |= 1u << 15;
		MAC[0] = (s32)y;
		SX[0] = SX[1]; SY[0] = SY[1];
		SX[1] = SX[2]; SY[1] = SY[2];
		if ((u32)(sx + 0x400) > 0x7ff) { sx = sx < 0 ? -0x400 : 0x3ff; fl |= 1u << 14; }
		if ((u32)(sy + 0x400) > 0x7ff) { sy = sy < 0 ? -0x400 : 0x3ff; fl |= 1u << 13; }
		SX[2] = (s16)sx;
		SY[2] = (s16)sy;
	}
	if (last) {
		s64 d = (s64)n * DQA + DQB;
		s32 ir0 = (s32)(d >> 12);
		if (d > 0x7fffffffLL) fl |= 1u << 16;
		else if (d < -0x80000000LL) fl |= 1u << 15;
		MAC[0] = (s32)d;
		if (ir0 < 0) { ir0 = 0; fl |= 1u << 12; } else if (ir0 > 0x1000) { ir0 = 0x1000; fl |= 1u << 12; }
		IR[0] = (s16)ir0;
	}
	FLAG |= fl;
}

#if defined(__arm__) && defined(ARM9)
// RTPT en ensamblador para el ARM946E-S: la parte cara (matriz por vértice, recortes, división UNR y proyección)
// de los tres vértices. El compilador sacaba la matriz a la pila y rehacía en 64 bits cada fila; aquí cada fila
// son tres SMULL/SMLAL y una suma (TR << 12 no tiene bits bajos: TR + (suma >> 12) es exacto). Da lo mismo, bit a
// bit, que rtp12f: el fuzz de instrucciones compara el código traducido de la DS con el intérprete del PC (en C).
typedef struct { s32 mac1, mac2, z12, ir1, ir2, ir3; u32 n, fl; s16 sx[3], sy[3]; u16 sz[3]; } RtpOut;
_Static_assert(offsetof(RtpOut, ir1) == 12 && offsetof(RtpOut, n) == 24 && offsetof(RtpOut, fl) == 28 &&
               offsetof(RtpOut, sx) == 32 && offsetof(RtpOut, sy) == 38 && offsetof(RtpOut, sz) == 44, "RtpOut");
void gte_rtpt_asm(const GteRegs *g, RtpOut *o, s32 lo, const u8 *unr_tab);
void gte_rtps_asm(const GteRegs *g, RtpOut *o, s32 lo, const u8 *unr_tab);
__asm__(
"	.text\n"
"	.arm\n"
"	.align 2\n"
// una fila de la matriz: mac = TR + (RT . V) >> 12 en r7; ir recortado a [lo, 7FFFh] en r8; ambos a o
".macro RTPROW rt, tr, omac, oir, flagbit\n"
"	ldrsh r8, [r0, #\\rt]\n"
"	ldrsh r9, [r0, #\\rt+2]\n"
"	ldrsh r10, [r0, #\\rt+4]\n"
"	smull r12, lr, r8, r4\n"
"	smlal r12, lr, r9, r5\n"
"	smlal r12, lr, r10, r6\n"
"	ldr r7, [r0, #\\tr]\n"
"	add r7, r7, r12, lsr #12\n"
"	add r7, r7, lr, lsl #20\n"
"	cmp r7, r2\n"
"	movlt r8, r2\n"
"	movge r8, r7\n"
"	cmp r8, #0x8000\n"
"	movge r8, #0x7f00\n"
"	orrge r8, r8, #0xff\n"
"	str r7, [r1, #\\omac]\n"
"	str r8, [r1, #\\oir]\n"
"	.if \\flagbit\n"
"	teq r8, r7\n"
"	orrne r11, r11, #(1 << \\flagbit)\n"
"	.endif\n"
".endm\n"
// proyección de un eje: n (r7) * ir + OF, desbordamientos (bits 16/15), y recorte a [-400h, 3FFh]
".macro PROJ irr, ofr, satbit, osx\n"
"	smull r9, r10, r7, \\irr\n"
"	adds r9, r9, \\ofr\n"
"	adc r10, r10, \\ofr, asr #31\n"
"	cmp r10, r9, asr #31\n"
"	beq 1f\n"
"	cmp r10, #0\n"
"	orrge r11, r11, #(1 << 16)\n"
"	orrlt r11, r11, #(1 << 15)\n"
"1:	mov r9, r9, lsr #16\n"
"	orr r9, r9, r10, lsl #16\n"
"	add r10, r9, #0x400\n"
"	cmp r10, #0x800\n"
"	blo 2f\n"
"	orr r11, r11, #(1 << \\satbit)\n"
"	cmp r9, #0\n"                      // -400h o 3FFh (ninguno cabe como inmediato)
"	mov r9, #0x400\n"
"	rsblt r9, r9, #0\n"
"	subge r9, r9, #1\n"
"2:	strh r9, [r1, #\\osx]\n"
".endm\n"
// un vértice entero
".macro RTPV vi\n"
"	ldrsh r4, [r0, #96+\\vi*6]\n"
"	ldrsh r5, [r0, #98+\\vi*6]\n"
"	ldrsh r6, [r0, #100+\\vi*6]\n"
"	RTPROW 134, 48, 0, 12, 24\n"
"	RTPROW 140, 52, 4, 16, 23\n"
"	RTPROW 146, 56, 8, 20, 0\n"
"	add r9, r7, #0x8000\n"           // bandera de IR3: mira MAC3 sin lm
"	cmp r9, #0x10000\n"
"	orrhs r11, r11, #(1 << 22)\n"
"	movs r9, r7, lsr #16\n"            // SZ: MAC3 recortado a [0, FFFFh]
"	moveq r9, r7\n"
"	mvnne r9, r7, asr #31\n"
"	movne r9, r9, lsr #16\n"
"	orrne r11, r11, #(1 << 18)\n"
"	strh r9, [r1, #44+\\vi*2]\n"
"	ldrh r10, [r0, #204]\n"            // división UNR: n = H / SZ (como divide())
"	cmp r10, r9, lsl #1\n"
"	bhs 4f\n"
"	clz r12, r9\n"
"	sub r12, r12, #16\n"
"	mov lr, r9, lsl r12\n"
"	sub r8, lr, #0x7f00\n"
"	sub r8, r8, #0xc0\n"
"	ldrb r8, [r3, r8, lsr #7]\n"
"	add r8, r8, #0x100\n"
"	add r8, r8, #1\n"
"	mul r7, lr, r8\n"
"	rsb r7, r7, #0x2000000\n"
"	add r7, r7, #0x80\n"
"	mov r7, r7, lsr #8\n"
"	mul r9, r7, r8\n"
"	add r9, r9, #0x80\n"
"	mov r9, r9, lsr #8\n"
"	mov r10, r10, lsl r12\n"
"	umull r7, r8, r10, r9\n"
"	adds r7, r7, #0x8000\n"
"	adc r8, r8, #0\n"
"	movs r10, r8, lsr #16\n"
"	mov r7, r7, lsr #16\n"
"	orr r7, r7, r8, lsl #16\n"
"	bne 5f\n"
"	cmp r7, #0x20000\n"
"	blo 6f\n"
"5:	mov r7, #0x20000\n"
"	sub r7, r7, #1\n"
"	b 6f\n"
"4:	orr r11, r11, #(1 << 17)\n"
"	mov r7, #0x20000\n"
"	sub r7, r7, #1\n"
"6:	str r7, [r1, #24]\n"
"	ldr r8, [r1, #12]\n"
"	ldr r12, [r0, #84]\n"
"	PROJ r8, r12, 14, 32+\\vi*2\n"
"	ldr r8, [r1, #16]\n"
"	ldr r12, [r0, #88]\n"
"	PROJ r8, r12, 13, 38+\\vi*2\n"
".endm\n"
"	.global gte_rtpt_asm\n"
"	.type gte_rtpt_asm, %function\n"
"gte_rtpt_asm:\n"
"	push {r4-r11, lr}\n"
"	mov r11, #0\n"
"	RTPV 0\n"
"	RTPV 1\n"
"	RTPV 2\n"
"	str r11, [r1, #28]\n"
"	pop {r4-r11, pc}\n"
"	.size gte_rtpt_asm, . - gte_rtpt_asm\n"
"	.global gte_rtps_asm\n"             // RTPS: solo el vértice 0
"	.type gte_rtps_asm, %function\n"
"gte_rtps_asm:\n"
"	push {r4-r11, lr}\n"
"	mov r11, #0\n"
"	RTPV 0\n"
"	str r11, [r1, #28]\n"
"	pop {r4-r11, pc}\n"
"	.size gte_rtps_asm, . - gte_rtps_asm\n"
"	.purgem RTPROW\n"
"	.purgem PROJ\n"
"	.purgem RTPV\n"
);

static __attribute__((noinline)) void rtpt12f(void)
{
	RtpOut o;
	if (((u32)(TR[0] + 0x40000000) | (u32)(TR[1] + 0x40000000) | (u32)(TR[2] + 0x40000000)) >= 0x80000000u) {
		rtp12f(0, 0); rtp12f(1, 0); rtp12f(2, 1);
		return;
	}
	gte_rtpt_asm(&psx_core.gte, &o, lm ? 0 : -0x8000, unr);
	SZ[0] = SZ[3]; SZ[1] = o.sz[0]; SZ[2] = o.sz[1]; SZ[3] = o.sz[2];
	SX[0] = o.sx[0]; SX[1] = o.sx[1]; SX[2] = o.sx[2];
	SY[0] = o.sy[0]; SY[1] = o.sy[1]; SY[2] = o.sy[2];
	MAC[1] = o.mac1; MAC[2] = o.mac2; MAC[3] = o.z12;
	IR[1] = (s16)o.ir1; IR[2] = (s16)o.ir2; IR[3] = (s16)o.ir3;
	{
		u32 fl = o.fl;
		s64 d = (s64)o.n * DQA + DQB;
		s32 ir0 = (s32)(d >> 12);
		if (d > 0x7fffffffLL) fl |= 1u << 16;
		else if (d < -0x80000000LL) fl |= 1u << 15;
		MAC[0] = (s32)d;
		if (ir0 < 0) { ir0 = 0; fl |= 1u << 12; } else if (ir0 > 0x1000) { ir0 = 0x1000; fl |= 1u << 12; }
		IR[0] = (s16)ir0;
		FLAG |= fl;
	}
}

// RTPS: lo mismo que rtp12f(0, 1) con el vértice en ensamblador
static __attribute__((noinline)) void rtps12f(void)
{
	RtpOut o;
	if (((u32)(TR[0] + 0x40000000) | (u32)(TR[1] + 0x40000000) | (u32)(TR[2] + 0x40000000)) >= 0x80000000u) {
		rtp12f(0, 1);
		return;
	}
	gte_rtps_asm(&psx_core.gte, &o, lm ? 0 : -0x8000, unr);
	SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3]; SZ[3] = o.sz[0];
	SX[0] = SX[1]; SX[1] = SX[2]; SX[2] = o.sx[0];
	SY[0] = SY[1]; SY[1] = SY[2]; SY[2] = o.sy[0];
	MAC[1] = o.mac1; MAC[2] = o.mac2; MAC[3] = o.z12;
	IR[1] = (s16)o.ir1; IR[2] = (s16)o.ir2; IR[3] = (s16)o.ir3;
	{
		u32 fl = o.fl;
		s64 d = (s64)o.n * DQA + DQB;
		s32 ir0 = (s32)(d >> 12);
		if (d > 0x7fffffffLL) fl |= 1u << 16;
		else if (d < -0x80000000LL) fl |= 1u << 15;
		MAC[0] = (s32)d;
		if (ir0 < 0) { ir0 = 0; fl |= 1u << 12; } else if (ir0 > 0x1000) { ir0 = 0x1000; fl |= 1u << 12; }
		IR[0] = (s16)ir0;
		FLAG |= fl;
	}
}
#else
static void rtps12f(void) { rtp12f(0, 1); }

// RTPT entera (lo mismo que rtp12f de los vértices 0, 1 y 2): la matriz y la traslación se leen una vez, y MAC/IR
// solo se escriben al final (los de los dos primeros vértices los pisa el tercero); las banderas, de los tres
static __attribute__((noinline)) void rtpt12f(void)
{
	u32 fl = 0, n = 0;
	s32 lo = lm ? 0 : -0x8000, mac1 = 0, mac2 = 0, z12 = 0, ir1 = 0, ir2 = 0, ir3 = 0;
	const s32 t0 = TR[0], t1 = TR[1], t2 = TR[2];
	const u32 h = H;
	if (((u32)(t0 + 0x40000000) | (u32)(t1 + 0x40000000) | (u32)(t2 + 0x40000000)) >= 0x80000000u) {
		rtp12f(0, 0); rtp12f(1, 0); rtp12f(2, 1);
		return;
	}
	for (int vi = 0; vi < 3; vi++) {
		s32 vx = V[vi][0], vy = V[vi][1], vz = V[vi][2], sx, sy;
		mac1 = (s32)((((s64)t0 << 12) + (s64)(RT[0][0] * vx) + (s64)(RT[0][1] * vy) + (s64)(RT[0][2] * vz)) >> 12);
		mac2 = (s32)((((s64)t1 << 12) + (s64)(RT[1][0] * vx) + (s64)(RT[1][1] * vy) + (s64)(RT[1][2] * vz)) >> 12);
		z12 = (s32)((((s64)t2 << 12) + (s64)(RT[2][0] * vx) + (s64)(RT[2][1] * vy) + (s64)(RT[2][2] * vz)) >> 12);
		ir1 = mac1 < lo ? lo : mac1 > 0x7fff ? 0x7fff : mac1;
		ir2 = mac2 < lo ? lo : mac2 > 0x7fff ? 0x7fff : mac2;
		ir3 = z12 < lo ? lo : z12 > 0x7fff ? 0x7fff : z12;
		if (ir1 != mac1) fl |= 1u << 24;
		if (ir2 != mac2) fl |= 1u << 23;
		if ((u32)(z12 + 0x8000) > 0xffff) fl |= 1u << 22;
		SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3];
		if ((u32)z12 > 0xffff) { SZ[3] = z12 < 0 ? 0 : 0xffff; fl |= 1u << 18; } else SZ[3] = (u16)z12;
		{
			u32 sz = SZ[3];
			if (h >= sz * 2) { fl |= 1u << 17; n = 0x1ffff; }
			else {
				int sh = __builtin_clz(sz) - 16;
				u32 d = sz << sh, u = unr[(d - 0x7fc0) >> 7] + 0x101;
				u64 q;
				d = (0x2000080 - (d * u)) >> 8;
				d = (0x0000080 + (d * u)) >> 8;
				q = ((u64)(h << sh) * d + 0x8000) >> 16;
				n = q > 0x1ffff ? 0x1ffff : (u32)q;
			}
		}
		{
			s64 x = (s64)n * ir1 + OFX, y = (s64)n * ir2 + OFY;
			sx = (s32)(x >> 16); sy = (s32)(y >> 16);
			if (x > 0x7fffffffLL || y > 0x7fffffffLL) fl |= 1u << 16;
			if (x < -0x80000000LL || y < -0x80000000LL) fl |= 1u << 15;
			MAC[0] = (s32)y;
			if ((u32)(sx + 0x400) > 0x7ff) { sx = sx < 0 ? -0x400 : 0x3ff; fl |= 1u << 14; }
			if ((u32)(sy + 0x400) > 0x7ff) { sy = sy < 0 ? -0x400 : 0x3ff; fl |= 1u << 13; }
			SX[0] = SX[1]; SY[0] = SY[1];
			SX[1] = SX[2]; SY[1] = SY[2];
			SX[2] = (s16)sx;
			SY[2] = (s16)sy;
		}
	}
	MAC[1] = mac1; MAC[2] = mac2; MAC[3] = z12;
	IR[1] = (s16)ir1; IR[2] = (s16)ir2; IR[3] = (s16)ir3;
	{
		s64 d = (s64)n * DQA + DQB;
		s32 ir0 = (s32)(d >> 12);
		if (d > 0x7fffffffLL) fl |= 1u << 16;
		else if (d < -0x80000000LL) fl |= 1u << 15;
		MAC[0] = (s32)d;
		if (ir0 < 0) { ir0 = 0; fl |= 1u << 12; } else if (ir0 > 0x1000) { ir0 = 0x1000; fl |= 1u << 12; }
		IR[0] = (s16)ir0;
	}
	FLAG |= fl;
}
#endif

// interpolación hacia el color de lejanía (FC) según IR0
GTE_INL void interpolate_s(s32 m1, s32 m2, s32 m3, int s)
{
	set_mac_s(1, ((s64)FC[0] << 12) - m1, s);
	set_mac_s(2, ((s64)FC[1] << 12) - m2, s);
	set_mac_s(3, ((s64)FC[2] << 12) - m3, s);
	set_ir(1, MAC[1], 0);
	set_ir(2, MAC[2], 0);
	set_ir(3, MAC[3], 0);
	set_mac_s(1, (s64)IR[1] * IR[0] + m1, s);
	set_mac_s(2, (s64)IR[2] * IR[0] + m2, s);
	set_mac_s(3, (s64)IR[3] * IR[0] + m3, s);
	mac_to_ir();
}

static void interpolate(s32 m1, s32 m2, s32 m3) { if (sf) interpolate_s(m1, m2, m3, 12); else interpolate_s(m1, m2, m3, 0); }

static void light(int vi)   // L*V y luego BK + LC*IR
{
	mat_vec(LM, NULL, V[vi][0], V[vi][1], V[vi][2]);
	mac_to_ir();
	mat_vec(LC, BK, IR[1], IR[2], IR[3]);
	mac_to_ir();
}

static void color_mul(void)   // MAC = RGB*IR << 4
{
	set_mac(1, ((s64)RGBC[0] * IR[1]) << 4);
	set_mac(2, ((s64)RGBC[1] * IR[2]) << 4);
	set_mac(3, ((s64)RGBC[2] * IR[3]) << 4);
}

static void ncd(int vi)
{
	light(vi);
	{
		s64 m1 = ((s64)RGBC[0] * IR[1]) << 4, m2 = ((s64)RGBC[1] * IR[2]) << 4, m3 = ((s64)RGBC[2] * IR[3]) << 4;
		interpolate((s32)m1, (s32)m2, (s32)m3);
	}
	push_color();
}

static void ncc(int vi)
{
	light(vi);
	color_mul();
	mac_to_ir();
	push_color();
}

static void nc(int vi)
{
	light(vi);
	push_color();
}

static void dpc(const u8 *c)
{
	interpolate((s32)c[0] << 16, (s32)c[1] << 16, (s32)c[2] << 16);
	push_color();
}

// DPCS/DPCT con sf = 12 y |FC| < 2^18 (lo normal): todo cabe en 32 bits y los MAC no pueden desbordar.
// Mismo resultado y banderas que dpc(); es la operación que más usa Silent Hill (niebla).
GTE_INL int dpc_fast_ok(void)
{
	return sf && ((u32)(FC[0] + 0x40000) | (u32)(FC[1] + 0x40000) | (u32)(FC[2] + 0x40000)) < 0x80000;
}

static void dpc_fast(const u8 *c)
{
	s32 ir0 = IR[0];
	for (int i = 0; i < 3; i++) {
		s32 m = (s32)c[i] << 16, t = ((FC[i] << 12) - m) >> 12, ir;
		if (t < -0x8000) { t = -0x8000; FLAG |= 1u << (24 - i); }
		else if (t > 0x7fff) { t = 0x7fff; FLAG |= 1u << (24 - i); }
		t = (t * ir0 + m) >> 12;
		MAC[i + 1] = t;
		ir = t;
		if (ir < (lm ? 0 : -0x8000)) { ir = lm ? 0 : -0x8000; FLAG |= 1u << (24 - i); }
		else if (ir > 0x7fff) { ir = 0x7fff; FLAG |= 1u << (24 - i); }
		IR[i + 1] = (s16)ir;
	}
	push_color();
}


// Lo mismo que dpc_fast() + push_color() en una pasada, con las banderas en un registro (FLAG se toca una vez)
#define DPC_CH(i) do { \
	s32 m = (s32)c##i << 16, t = ((FC[i] << 12) - m) >> 12, ir, col; \
	if ((u32)(t + 0x8000) > 0xffff) { t = t < 0 ? -0x8000 : 0x7fff; fl |= 1u << (24 - i); } \
	t = (t * ir0 + m) >> 12; \
	MAC[i + 1] = t; \
	ir = t < lo ? lo : t > 0x7fff ? 0x7fff : t; \
	if (ir != t) fl |= 1u << (24 - i); \
	IR[i + 1] = (s16)ir; \
	col = t >> 4; \
	if ((u32)col > 255) { col = col < 0 ? 0 : 255; fl |= 1u << (21 - i); } \
	out##i = (u8)col; \
} while (0)

static void dpc12f(const u8 *c)
{
	u32 fl = 0;
	s32 ir0 = IR[0], lo = lm ? 0 : -0x8000, c0 = c[0], c1 = c[1], c2 = c[2];
	u8 out0, out1, out2;
	DPC_CH(0);
	DPC_CH(1);
	DPC_CH(2);
	memcpy(CF[0], CF[1], 4);   // CF[0] <- CF[1], CF[1] <- CF[2] (de 4 en 4: se integra; memmove era una llamada)
	memcpy(CF[1], CF[2], 4);
	CF[2][0] = out0;
	CF[2][1] = out1;
	CF[2][2] = out2;
	CF[2][3] = RGBC[3];
	FLAG |= fl;
}

GTE_INL void nclip(void)
{
	// Con SX y SY de 11 bits (lo normal: RTP los recorta) la suma cabe en 32 bits y no hay desbordamiento
	if ((((u32)(SX[0] + 0x400) | (u32)(SX[1] + 0x400) | (u32)(SX[2] + 0x400) |
	      (u32)(SY[0] + 0x400) | (u32)(SY[1] + 0x400) | (u32)(SY[2] + 0x400)) & ~0x7ffu) == 0) {
		MAC[0] = SX[0] * (SY[1] - SY[2]) + SX[1] * (SY[2] - SY[0]) + SX[2] * (SY[0] - SY[1]);
		return;
	}
	set_mac0((s64)SX[0] * SY[1] + (s64)SX[1] * SY[2] + (s64)SX[2] * SY[0] -
	         (s64)SX[0] * SY[2] - (s64)SX[1] * SY[0] - (s64)SX[2] * SY[1]);
}

static void gte_exec(u32 op)
{
	if (!unr_ready) {
		for (int i = 0; i < 257; i++) {
			int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
			unr[i] = (u8)(v > 0 ? v : 0);
		}
		unr_ready = 1;
	}
	sf = (op >> 19) & 1 ? 12 : 0;
	lm = (op >> 10) & 1;
	FLAG = 0;

	switch (op & 0x3f) {
	case 0x01: rtp(0, 1); break;   // RTPS
	case 0x06: nclip(); break;      // NCLIP
	case 0x0c: {   // OP
		s32 d1 = RT[0][0], d2 = RT[1][1], d3 = RT[2][2];
		set_mac(1, (s64)d2 * IR[3] - (s64)d3 * IR[2]);
		set_mac(2, (s64)d3 * IR[1] - (s64)d1 * IR[3]);
		set_mac(3, (s64)d1 * IR[2] - (s64)d2 * IR[1]);
		mac_to_ir();
		break;
	}
	case 0x10: if (dpc_fast_ok()) dpc_fast(RGBC); else dpc(RGBC); break;   // DPCS
	case 0x11:                     // INTPL
		interpolate((s32)IR[1] << 12, (s32)IR[2] << 12, (s32)IR[3] << 12);
		push_color();
		break;
	case 0x12: {   // MVMVA
		int mx = (op >> 17) & 3, vv = (op >> 15) & 3, cv = (op >> 13) & 3;
		s16 (*m)[3] = mx == 0 ? RT : mx == 1 ? LM : LC;
		static s16 garbage[3][3];
		const s32 *add = cv == 0 ? TR : cv == 1 ? BK : cv == 2 ? FC : NULL;
		s32 vx, vy, vz;
		if (mx == 3) {
			garbage[0][0] = (s16)(-(RGBC[0] << 4)); garbage[0][1] = (s16)(RGBC[0] << 4); garbage[0][2] = IR[0];
			garbage[1][0] = garbage[1][1] = garbage[1][2] = RT[0][2];
			garbage[2][0] = garbage[2][1] = garbage[2][2] = RT[1][1];
			m = garbage;
		}
		if (vv == 3) { vx = IR[1]; vy = IR[2]; vz = IR[3]; }
		else { vx = V[vv][0]; vy = V[vv][1]; vz = V[vv][2]; }
		if (cv == 2) {
			// Fallo del hardware con FC: FC*1000h + M?1*VX solo deja banderas y se descarta;
			// el resultado es M?2*VY + M?3*VZ.
			for (int i = 0; i < 3; i++) {
				s64 t = chk44(i + 1, ((s64)FC[i] << 12) + (s64)m[i][0] * vx);
				s32 s = (s32)(t >> sf);
				if (s < -0x8000 || s > 0x7fff) FLAG |= 1u << (24 - i);
				t = chk44(i + 1, (s64)m[i][1] * vy);
				t = chk44(i + 1, t + (s64)m[i][2] * vz);
				MAC[i + 1] = (s32)(t >> sf);
			}
		} else mat_vec(m, add, vx, vy, vz);
		mac_to_ir();
		break;
	}
	case 0x13: ncd(0); break;   // NCDS
	case 0x14:                  // CDP
		mat_vec(LC, BK, IR[1], IR[2], IR[3]);
		mac_to_ir();
		{
			s64 m1 = ((s64)RGBC[0] * IR[1]) << 4, m2 = ((s64)RGBC[1] * IR[2]) << 4, m3 = ((s64)RGBC[2] * IR[3]) << 4;
			interpolate((s32)m1, (s32)m2, (s32)m3);
		}
		push_color();
		break;
	case 0x16: ncd(0); ncd(1); ncd(2); break;   // NCDT
	case 0x1b: ncc(0); break;                   // NCCS
	case 0x1c:                                  // CC
		mat_vec(LC, BK, IR[1], IR[2], IR[3]);
		mac_to_ir();
		color_mul();
		mac_to_ir();
		push_color();
		break;
	case 0x1e: nc(0); break;                    // NCS
	case 0x20: nc(0); nc(1); nc(2); break;      // NCT
	case 0x28:                                  // SQR
		set_mac(1, (s64)IR[1] * IR[1]);
		set_mac(2, (s64)IR[2] * IR[2]);
		set_mac(3, (s64)IR[3] * IR[3]);
		mac_to_ir();
		break;
	case 0x29:                                  // DCPL
		{
			s64 m1 = ((s64)RGBC[0] * IR[1]) << 4, m2 = ((s64)RGBC[1] * IR[2]) << 4, m3 = ((s64)RGBC[2] * IR[3]) << 4;
			interpolate((s32)m1, (s32)m2, (s32)m3);
		}
		push_color();
		break;
	case 0x2a:   // DPCT
		if (dpc_fast_ok()) { dpc_fast(CF[0]); dpc_fast(CF[0]); dpc_fast(CF[0]); }
		else { dpc(CF[0]); dpc(CF[0]); dpc(CF[0]); }
		break;
	case 0x2d:   // AVSZ3
		{ s64 v = (s64)ZSF3 * (SZ[1] + SZ[2] + SZ[3]); set_mac0(v); set_otz(v); }
		break;
	case 0x2e:   // AVSZ4
		{ s64 v = (s64)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]); set_mac0(v); set_otz(v); }
		break;
	case 0x30:   // RTPT
		if (sf) rtpt12f();
		else { rtp(0, 0); rtp(1, 0); rtp(2, 1); }
		break;
	case 0x3d:                                            // GPF
		set_mac(1, (s64)IR[0] * IR[1]);
		set_mac(2, (s64)IR[0] * IR[2]);
		set_mac(3, (s64)IR[0] * IR[3]);
		mac_to_ir();
		push_color();
		break;
	case 0x3e:   // GPL
		set_mac(1, ((s64)MAC[1] << sf) + (s64)IR[0] * IR[1]);
		set_mac(2, ((s64)MAC[2] << sf) + (s64)IR[0] * IR[2]);
		set_mac(3, ((s64)MAC[3] << sf) + (s64)IR[0] * IR[3]);
		mac_to_ir();
		push_color();
		break;
	case 0x3f: ncc(0); ncc(1); ncc(2); break;   // NCCT
	}
}

void gte_state(PsxState *s)
{
	STATE(s, V); STATE(s, RGBC); STATE(s, OTZ); STATE(s, IR); STATE(s, SX); STATE(s, SY); STATE(s, SZ);
	STATE(s, CF); STATE(s, RES1); STATE(s, MAC); STATE(s, LZCS); STATE(s, LZCR);
	STATE(s, RT); STATE(s, LM); STATE(s, LC); STATE(s, TR); STATE(s, BK); STATE(s, FC);
	STATE(s, OFX); STATE(s, OFY); STATE(s, DQB); STATE(s, H); STATE(s, DQA); STATE(s, ZSF3); STATE(s, ZSF4);
	STATE(s, FLAG);
}

void gte_command(u32 op)
{
	// sin PROF_BEGIN/END: se llama miles de veces por frame y medir costaba casi tanto como calcular
	STAT_ADD(gte_ops, 1);
#ifdef PSX_GTESTAT
	{ extern u32 gte_hist[64]; gte_hist[op & 0x3f]++; }
#endif
	gte_exec(op);
}

// ---- entradas directas para el dynarec ----
// La operación se sabe al traducir: cada una tiene su función (sin decodificar, sin contar, con sf ya fijo),
// y el código traducido la llama sin pasar por el reloj (la GTE no lo mira). Lo raro va por gte_command.
void gte_reset(void)
{
	for (int i = 0; i < 257; i++) {
		int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
		unr[i] = (u8)(v > 0 ? v : 0);
	}
	unr_ready = 1;
}

#define GTE_ENTRY(name, s, body) static void name(u32 op) { sf = s; lm = (op >> 10) & 1; FLAG = 0; body }
GTE_ENTRY(e_rtps, 12, rtps12f();)
GTE_ENTRY(e_rtpt, 12, rtpt12f();)
GTE_ENTRY(e_nclip, 0, (void)op; nclip();)
GTE_ENTRY(e_dpcs, 12, if (dpc_fast_ok()) dpc12f(RGBC); else dpc(RGBC);)
GTE_ENTRY(e_dpct, 12, if (dpc_fast_ok()) { dpc12f(CF[0]); dpc12f(CF[0]); dpc12f(CF[0]); } else { dpc(CF[0]); dpc(CF[0]); dpc(CF[0]); })
GTE_ENTRY(e_avsz3, 0, { s64 v = (s64)ZSF3 * (SZ[1] + SZ[2] + SZ[3]); set_mac0(v); set_otz(v); })
GTE_ENTRY(e_avsz4, 0, { s64 v = (s64)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]); set_mac0(v); set_otz(v); })
GTE_ENTRY(e_ncds, 12, ncd(0);)
GTE_ENTRY(e_ncdt, 12, ncd(0); ncd(1); ncd(2);)
GTE_ENTRY(e_nccs, 12, ncc(0);)
GTE_ENTRY(e_ncct, 12, ncc(0); ncc(1); ncc(2);)
GTE_ENTRY(e_ncs, 12, nc(0);)
GTE_ENTRY(e_nct, 12, nc(0); nc(1); nc(2);)

// MVMVA directo en lo normal (matriz RT/LM/LC; traslación TR, BK o ninguna): lo mismo que gte_exec sin su
// despacho. La matriz «3» (basura) y la traslación FC (con el fallo del hardware) siguen por gte_exec.
static void e_mvmva(u32 op)
{
	int mx = (op >> 17) & 3, vv = (op >> 15) & 3, cv = (op >> 13) & 3;
	s32 vx, vy, vz;
	if (mx == 3 || cv == 2) { gte_exec(op); return; }
	sf = (op >> 19) & 1 ? 12 : 0;
	lm = (op >> 10) & 1;
	FLAG = 0;
	if (vv == 3) { vx = IR[1]; vy = IR[2]; vz = IR[3]; }
	else { vx = V[vv][0]; vy = V[vv][1]; vz = V[vv][2]; }
	mat_vec(mx == 0 ? RT : mx == 1 ? LM : LC, cv == 0 ? TR : cv == 1 ? BK : NULL, vx, vy, vz);
	mac_to_ir();
}

void (*gte_entry(u32 op))(u32)
{
	int s12 = (op >> 19) & 1;
	switch (op & 0x3f) {
	case 0x06: return e_nclip;
	case 0x2d: return e_avsz3;
	case 0x2e: return e_avsz4;
	case 0x12: return e_mvmva;
	}
	if (!s12) return gte_exec;   // el resto también directo: sin trampolín ni contar (MVMVA en Silent Hill)
	switch (op & 0x3f) {
	case 0x01: return e_rtps;
	case 0x30: return e_rtpt;
	case 0x10: return e_dpcs;
	case 0x2a: return e_dpct;
	case 0x13: return e_ncds;
	case 0x16: return e_ncdt;
	case 0x1b: return e_nccs;
	case 0x3f: return e_ncct;
	case 0x1e: return e_ncs;
	case 0x20: return e_nct;
	}
	return gte_exec;
}
