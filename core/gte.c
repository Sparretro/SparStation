// GTE (Geometry Transformation Engine): el coprocesador 2 que hace la geometría 3D en punto fijo.
// Sigue la documentación psx-spx, incluida la división UNR exacta que usan los juegos para la perspectiva.
#include "psx.h"

// Los registros van en la DTCM (PSX_FAST): se tocan en cada operación
PSX_FAST static s16 V[3][3];
PSX_FAST static u8 RGBC[4] __attribute__((aligned(4)));
PSX_FAST static u16 OTZ;
PSX_FAST static s16 IR[4];
PSX_FAST static s16 SX[3], SY[3];
PSX_FAST static u16 SZ[4];
PSX_FAST static u8 CF[3][4] __attribute__((aligned(4)));   // FIFO de colores RGB0..RGB2
PSX_FAST static u32 RES1;
PSX_FAST static s32 MAC[4];
PSX_FAST static u32 LZCS, LZCR;
PSX_FAST static s16 RT[3][3], LM[3][3], LC[3][3];
PSX_FAST static s32 TR[3], BK[3], FC[3];
PSX_FAST static s32 OFX, OFY, DQB;
PSX_FAST static u16 H;
PSX_FAST static s16 DQA, ZSF3, ZSF4;
PSX_FAST static u32 FLAG;

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
static void rtp(int vi, int last) { if (sf) rtp_s(vi, last, 12); else rtp_s(vi, last, 0); }

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
	case 0x06: {                    // NCLIP
		s64 v = (s64)SX[0] * SY[1] + (s64)SX[1] * SY[2] + (s64)SX[2] * SY[0] -
		        (s64)SX[0] * SY[2] - (s64)SX[1] * SY[0] - (s64)SX[2] * SY[1];
		set_mac0(v);
		break;
	}
	case 0x0c: {   // OP
		s32 d1 = RT[0][0], d2 = RT[1][1], d3 = RT[2][2];
		set_mac(1, (s64)d2 * IR[3] - (s64)d3 * IR[2]);
		set_mac(2, (s64)d3 * IR[1] - (s64)d1 * IR[3]);
		set_mac(3, (s64)d1 * IR[2] - (s64)d2 * IR[1]);
		mac_to_ir();
		break;
	}
	case 0x10: dpc(RGBC); break;   // DPCS
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
	case 0x2a: dpc(CF[0]); dpc(CF[0]); dpc(CF[0]); break;   // DPCT
	case 0x2d:   // AVSZ3
		{ s64 v = (s64)ZSF3 * (SZ[1] + SZ[2] + SZ[3]); set_mac0(v); set_otz(v); }
		break;
	case 0x2e:   // AVSZ4
		{ s64 v = (s64)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]); set_mac0(v); set_otz(v); }
		break;
	case 0x30:   // RTPT
		if (sf) { rtp_s(0, 0, 12); rtp_s(1, 0, 12); rtp_s(2, 1, 12); }
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
	gte_exec(op);
}
