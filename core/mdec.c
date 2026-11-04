// MDEC: el decodificador de vídeo (FMV) de la PS1. Bloques 8x8 con RLE + IDCT y conversión YUV -> RGB,
// siguiendo el algoritmo de psx-spx. Decodifica cada comando entero al recibir sus datos.
#include "psx.h"
#include <stdlib.h>

static const u8 zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static u8 qt_y[64], qt_c[64];
static s16 scale[64];

static u32 cmd;                 // comando en curso
static u32 words_left;          // parámetros que faltan
static u16 *in_buf;             // datos del comando de decodificar (en medias palabras)
static u32 in_len, in_cap;
static u8 *out_buf;             // salida ya decodificada, pendiente de leer
static u32 out_len, out_pos, out_cap;
static u32 ctrl;
static int qt_pos, qt_color, sc_pos;

enum { IDLE, DECODE, QUANT, SCALE };
static u32 state;   // u32 y no el enum: en ARM los enum ocupan 1 byte y el estado guardado no casaría con el del PC

void mdec_reset(void)
{
	state = IDLE;
	words_left = 0;
	in_len = out_len = out_pos = 0;
	ctrl = 0;
	cmd = 0;
}

// Sitio para n bytes más; 0 si no hay memoria (el bloque se descarta en vez de escribir fuera)
static int out_reserve(u32 n)
{
	if (out_len + n > out_cap) {
		u32 cap = out_len + n < 320 * 256 * 3 ? 320 * 256 * 3 : out_len + n + 32 * 1024;   // un frame de vídeo de una vez (sin doblar: en el modo DS no cabría)
		u8 *b = realloc(out_buf, cap);
		if (!b) return 0;
		out_buf = b;
		out_cap = cap;
	}
	return 1;
}

static inline int s10(u16 v) { return ((s32)((u32)v << 22)) >> 22; }

// Lee un bloque comprimido; devuelve 0 si se acabaron los datos. *bound: lado del cuadrado de arriba a la
// izquierda donde están todos los coeficientes (1..8): con 4 o menos la IDCT se salta la mitad del trabajo.
static u8 zz_side[64];   // max(fila, columna) + 1 de cada posición del zigzag
static int rl_decode(s16 *blk, const u16 **src, const u16 *end, const u8 *qt, int *bound)
{
	const u16 *p = *src;
	int k, q, val, side = 1;
	u16 n;
	if (!zz_side[63])
		for (int i = 0; i < 64; i++) { int r = zigzag[i] >> 3, c = zigzag[i] & 7; zz_side[i] = (u8)((r > c ? r : c) + 1); }
	memset(blk, 0, 64 * sizeof(s16));
	while (p < end && *p == 0xfe00) p++;
	if (p >= end) return 0;
	n = *p++;
	q = (n >> 10) & 63;
	k = 0;
	val = s10(n) * qt[0];
	int ac = 0;
	if (q == 0) side = 8;   // sin escala los coeficientes no van en zigzag
	for (;;) {
		if (q == 0) val = s10(n) * 2;
		if (val < -0x400) val = -0x400;
		if (val > 0x3ff) val = 0x3ff;
		if (q > 0) { blk[zigzag[k]] = (s16)val; if (val && zz_side[k] > side) side = zz_side[k]; }
		else blk[k] = (s16)val;
		if (k) ac |= val;
		if (k >= 63 || p >= end) break;   // un bloque lleno termina sin código FE00h
		n = *p++;
		k += ((n >> 10) & 63) + 1;
		if (k > 63) break;
		val = (s10(n) * qt[k] * q + 4) / 8;
	}
	*src = p;
	*bound = side;
	return ac ? 1 : 2;   // 2: solo el coeficiente de continua (la IDCT es un relleno)
}

// ---- IDCT rápida (Loeffler, como jidctint de libjpeg) ----
// Con la tabla de escala estándar de Sony, la IDCT del MDEC es la IDCT 2D normal: esta hace lo mismo
// con ~12 multiplicaciones por fila en vez de 64, y se salta filas y columnas vacías.
#define CB 13
#define P1 2
#define DESC(x, n) (((x) + (1 << ((n) - 1))) >> (n))
enum {
	F0298 = 2446, F0390 = 3196, F0541 = 4433, F0765 = 6270, F0899 = 7373, F1175 = 9633,
	F1501 = 12299, F1847 = 15137, F1961 = 16069, F2053 = 16819, F2562 = 20995, F3072 = 25172,
};
static int scale_is_standard;

static inline __attribute__((always_inline)) void idct_1d(s32 *o, s32 i0, s32 i1, s32 i2, s32 i3, s32 i4, s32 i5, s32 i6, s32 i7, int shift_even)
{
	s32 z1 = (i2 + i6) * F0541;
	s32 t2 = z1 - i6 * F1847, t3 = z1 + i2 * F0765;
	s32 t0 = (i0 + i4) << shift_even, t1 = (i0 - i4) << shift_even;
	s32 t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
	s32 a0 = i7, a1 = i5, a2 = i3, a3 = i1;
	s32 y1 = a0 + a3, y2 = a1 + a2, y3 = a0 + a2, y4 = a1 + a3, z5 = (y3 + y4) * F1175;
	a0 *= F0298; a1 *= F2053; a2 *= F3072; a3 *= F1501;
	y1 *= -F0899; y2 *= -F2562; y3 = y3 * -F1961 + z5; y4 = y4 * -F0390 + z5;
	a0 += y1 + y3; a1 += y2 + y4; a2 += y2 + y3; a3 += y1 + y4;
	o[0] = t10 + a3; o[7] = t10 - a3;
	o[1] = t11 + a2; o[6] = t11 - a2;
	o[2] = t12 + a1; o[5] = t12 - a1;
	o[3] = t13 + a0; o[4] = t13 - a0;
}

static void idct_fast(s16 *blk)
{
	s32 ws[64], o[8];
	// columnas: blk[fila_frecuencia * 8 + columna]
	for (int c = 0; c < 8; c++) {
		const s16 *in = blk + c;
		if (!(in[8] | in[16] | in[24] | in[32] | in[40] | in[48] | in[56])) {
			s32 dc = in[0] << P1;
			for (int r = 0; r < 8; r++) ws[r * 8 + c] = dc;
			continue;
		}
		idct_1d(o, in[0], in[8], in[16], in[24], in[32], in[40], in[48], in[56], CB);
		for (int r = 0; r < 8; r++) ws[r * 8 + c] = DESC(o[r], CB - P1);
	}
	// filas
	for (int r = 0; r < 8; r++) {
		const s32 *in = ws + r * 8;
		s16 *out = blk + r * 8;
		if (!(in[1] | in[2] | in[3] | in[4] | in[5] | in[6] | in[7])) {
			s16 v = (s16)DESC(in[0], P1 + 3);
			for (int c = 0; c < 8; c++) out[c] = v;
			continue;
		}
		idct_1d(o, in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7], CB);
		for (int c = 0; c < 8; c++) out[c] = (s16)DESC(o[c], CB + P1 + 3);
	}
}

// Igual que idct_fast cuando todos los coeficientes caen en el cuadrado 4x4 de arriba a la izquierda (lo normal
// en los vídeos): las columnas 4-7 dan 0 y las entradas 4-7 de cada fila también, así que sale lo mismo bit a bit
static void idct_fast4(s16 *blk)
{
	s32 ws[32], o[8];
	for (int c = 0; c < 4; c++) {
		const s16 *in = blk + c;
		if (!(in[8] | in[16] | in[24])) {
			s32 dc = in[0] << P1;
			for (int r = 0; r < 8; r++) ws[r * 4 + c] = dc;
			continue;
		}
		idct_1d(o, in[0], in[8], in[16], in[24], 0, 0, 0, 0, CB);
		for (int r = 0; r < 8; r++) ws[r * 4 + c] = DESC(o[r], CB - P1);
	}
	for (int r = 0; r < 8; r++) {
		const s32 *in = ws + r * 4;
		s16 *out = blk + r * 8;
		if (!(in[1] | in[2] | in[3])) {
			s16 v = (s16)DESC(in[0], P1 + 3);
			u32 vv = (u16)v | (u32)(u16)v << 16, *w = (u32 *)out;
			w[0] = w[1] = w[2] = w[3] = vv;
			continue;
		}
		idct_1d(o, in[0], in[1], in[2], in[3], 0, 0, 0, 0, CB);
		for (int c = 0; c < 8; c++) out[c] = (s16)DESC(o[c], CB + P1 + 3);
	}
}

// Modo rápido: IDCT de 4 puntos con los coeficientes 0-3, que da el valor en el centro de cada pareja de píxeles
// (la de 8 puntos en n = 2m + 1/2 es exactamente la de 4 puntos, sin las frecuencias altas). Salida 4x4 en blk[0..15].
enum { Q13 = 10703, Q05 = 4433 };   // √2·cos(π/8) y √2·cos(3π/8) en 13 bits
static inline __attribute__((always_inline)) void idct4_1d(s32 *o, s32 x0, s32 x1, s32 x2, s32 x3)
{
	s32 e0 = (x0 + x2) << CB, e1 = (x0 - x2) << CB;
	s32 o0 = x1 * Q13 + x3 * Q05, o1 = x1 * Q05 - x3 * Q13;
	o[0] = e0 + o0; o[3] = e0 - o0;
	o[1] = e1 + o1; o[2] = e1 - o1;
}

static void idct_quarter(s16 *blk)
{
	s32 ws[16], o[4];
	for (int c = 0; c < 4; c++) {
		const s16 *in = blk + c;
		if (!(in[8] | in[16] | in[24])) {
			s32 dc = in[0] << P1;
			ws[c] = ws[4 + c] = ws[8 + c] = ws[12 + c] = dc;
			continue;
		}
		idct4_1d(o, in[0], in[8], in[16], in[24]);
		for (int r = 0; r < 4; r++) ws[r * 4 + c] = DESC(o[r], CB - P1);
	}
	for (int r = 0; r < 4; r++) {
		const s32 *in = ws + r * 4;
		s16 *out = blk + r * 4;
		if (!(in[1] | in[2] | in[3])) {
			s16 v = (s16)DESC(in[0], P1 + 3);
			out[0] = out[1] = out[2] = out[3] = v;
			continue;
		}
		idct4_1d(o, in[0], in[1], in[2], in[3]);
		for (int c = 0; c < 4; c++) out[c] = (s16)DESC(o[c], CB + P1 + 3);
	}
}

// ¿La tabla subida es la estándar de Sony? (32768 * C(u) * cos((2x+1)uπ/16), redondeada)
static void check_scale(void)
{
	static const s16 std_row1[8] = { 32138, 27245, 18204, 6392, -6393, -18205, -27246, -32139 };
	scale_is_standard = scale[0] == 23170 && scale[7] == 23170 && !memcmp(&scale[8], std_row1, sizeof std_row1) &&
	                    scale[16] == 30273 && scale[63] == -6393;
}

// IDCT en dos pasadas con la tabla de escala que sube el juego; cada pasada divide entre 0x2000
static void idct(s16 *blk)
{
	if (scale_is_standard) { idct_fast(blk); return; }
	{
	s32 a[64], b[64], *src = a, *dst = b;
	for (int i = 0; i < 64; i++) a[i] = blk[i];
	for (int pass = 0; pass < 2; pass++) {
		for (int x = 0; x < 8; x++)
			for (int y = 0; y < 8; y++) {
				s32 sum = 0;
				for (int z = 0; z < 8; z++) sum += src[y + z * 8] * (scale[x + z * 8] / 8);
				dst[x + y * 8] = (sum + 0xfff) >> 13;
			}
		{ s32 *t = src; src = dst; dst = t; }
	}
	for (int i = 0; i < 64; i++) blk[i] = (s16)src[i];
	}
}

// Bloque ya leído -> píxeles: relleno si solo hay continua, IDCT recortada si todo cae en el 4x4, o la entera
static void idct_blk(s16 *blk, int ok, int bound)
{
	if (scale_is_standard) {
		if (ok == 2) {
			s16 v = (s16)DESC(blk[0] << P1, P1 + 3);
			u32 vv = (u16)v | (u32)(u16)v << 16, *w = (u32 *)blk;
			for (int i = 0; i < 32; i++) w[i] = vv;
			return;
		}
		if (bound <= 4) { idct_fast4(blk); return; }
	}
	idct(blk);
}

// Modo rápido: 4x4 muestras por bloque en blk[0..15]
static void idct_blk_quarter(s16 *blk, int ok)
{
	if (scale_is_standard) {
		if (ok == 2) {
			s16 v = (s16)DESC(blk[0] << P1, P1 + 3);
			for (int i = 0; i < 16; i++) blk[i] = v;
		} else idct_quarter(blk);
		return;
	}
	idct(blk);   // tabla rara: la entera y la media de cada 2x2
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++) {
			const s16 *p = blk + r * 16 + c * 2;
			blk[r * 4 + c] = (s16)((p[0] + p[1] + p[8] + p[9] + 2) >> 2);
		}
}

static inline int clamp8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline __attribute__((always_inline)) u32 cl(int v, u32 x) { if ((u32)(v + 128) > 255) v = v < 0 ? -128 : 127; return ((u32)v & 0xff) ^ x; }

int mdec_fast;   // 1 = vídeos a un cuarto de resolución (cada muestra rellena 2x2 píxeles): mucho menos trabajo

// Un píxel (o un 2x2 en el modo rápido) en la salida del macrobloque
#define PUT24(o, Y) do { (o)[0] = (u8)cl((Y) + R, x); (o)[1] = (u8)cl((Y) + G, x); (o)[2] = (u8)cl((Y) + B, x); } while (0)
#define PIX15(Y) (u16)((cl((Y) + R, x) >> 3) | ((cl((Y) + G, x) >> 3) << 5) | ((cl((Y) + B, x) >> 3) << 10) | hb)

// Macrobloque de 16x16 en color, ya con sus 6 bloques en píxeles, directo a la salida (24 o 15 bits)
static void mb_out(s16 (*blk)[64], u8 *dst, int depth, u32 x, u32 hb)
{
	for (int cy = 0; cy < 8; cy++)
		for (int cx = 0; cx < 8; cx++) {
			int ci = cy * 8 + cx, Rc = blk[0][ci], Bc = blk[1][ci];
			int G = (-88 * Bc - 183 * Rc + 128) >> 8, R = (359 * Rc + 128) >> 8, B = (454 * Bc + 128) >> 8;
			int px = cx * 2, py = cy * 2;
			const s16 *y = blk[2 + (py >> 3) * 2 + (px >> 3)] + (py & 7) * 8 + (px & 7);
			if (depth == 2) {
				u8 *o = dst + (py * 16 + px) * 3;
				PUT24(o, y[0]); PUT24(o + 3, y[1]); PUT24(o + 48, y[8]); PUT24(o + 51, y[9]);
			} else {
				u16 *o = (u16 *)(dst + (py * 16 + px) * 2);
				o[0] = PIX15(y[0]); o[1] = PIX15(y[1]); o[16] = PIX15(y[8]); o[17] = PIX15(y[9]);
			}
		}
}

static void mb_out_quarter(s16 (*blk)[64], u8 *dst, int depth, u32 x, u32 hb)
{
	for (int qy = 0; qy < 8; qy++)
		for (int qx = 0; qx < 8; qx++) {
			int ci = (qy >> 1) * 4 + (qx >> 1), Rc = blk[0][ci], Bc = blk[1][ci];
			int G = (-88 * Bc - 183 * Rc + 128) >> 8, R = (359 * Rc + 128) >> 8, B = (454 * Bc + 128) >> 8;
			int Y = blk[2 + (qy >> 2) * 2 + (qx >> 2)][(qy & 3) * 4 + (qx & 3)];
			int px = qx * 2, py = qy * 2;
			if (depth == 2) {
				u8 *o = dst + (py * 16 + px) * 3;
				u8 r = (u8)cl(Y + R, x), g = (u8)cl(Y + G, x), b = (u8)cl(Y + B, x);
				o[0] = o[3] = o[48] = o[51] = r;
				o[1] = o[4] = o[49] = o[52] = g;
				o[2] = o[5] = o[50] = o[53] = b;
			} else {
				u16 *o = (u16 *)(dst + (py * 16 + px) * 2), c = PIX15(Y);
				o[0] = o[1] = o[16] = o[17] = c;
			}
		}
}

static void decode(void)
{
	int depth = (cmd >> 27) & 3, sign = (cmd >> 26) & 1, b15 = (cmd >> 25) & 1;
	const u16 *p = in_buf, *end = in_buf + in_len;
	s16 blk[6][64] __attribute__((aligned(4)));
	u32 x = sign ? 0 : 0x80;

	out_len = out_pos = 0;
	if (depth >= 2) {   // color: Cr, Cb, Y1..Y4
		u32 mb = depth == 2 ? 768 : 512;
		int fast = mdec_fast;
		for (;;) {
			int ok = 1, bound;
			for (int b = 0; b < 6 && ok; b++) {
				ok = rl_decode(blk[b], &p, end, b < 2 ? qt_c : qt_y, &bound);
				if (ok) { if (fast) idct_blk_quarter(blk[b], ok); else idct_blk(blk[b], ok, bound); }
			}
			if (!ok) break;
			STAT_ADD(mdec_blocks, 1);
			if (!out_reserve(mb)) break;
			if (fast) mb_out_quarter(blk, out_buf + out_len, depth, x, (u32)b15 << 15);
			else mb_out(blk, out_buf + out_len, depth, x, (u32)b15 << 15);
			out_len += mb;
		}
	} else {   // monocromo: solo Y
		int bound;
		while (rl_decode(blk[0], &p, end, qt_y, &bound)) {
			idct(blk[0]);
			u8 yv[64];
			for (int i = 0; i < 64; i++) yv[i] = (u8)(clamp8(blk[0][i]) ^ (sign ? 0 : 0x80));
			if (depth == 1) {
				if (!out_reserve(64)) break;
				memcpy(out_buf + out_len, yv, 64);
				out_len += 64;
			} else {
				if (!out_reserve(32)) break;
				for (int i = 0; i < 64; i += 2) out_buf[out_len++] = (u8)((yv[i] >> 4) | (yv[i + 1] & 0xf0));
			}
		}
	}
}

void mdec_write_cmd(u32 v)
{
	switch (state) {
	case IDLE:
		cmd = v;
#ifdef PSX_CDLOG
		{ extern void cdlog_any(u32 v); cdlog_any(0x30000000u | (v >> 4)); }
#endif
		switch (v >> 29) {
		case 1:
			state = DECODE;
			words_left = v & 0xffff;
			in_len = 0;
			if (in_cap < words_left * 2) {   // sin memoria (modo DS): se queda el búfer que hubiera y se descarta lo que no quepa
				u16 *b = realloc(in_buf, words_left * 2 * sizeof(u16));
				if (b) { in_buf = b; in_cap = words_left * 2; }
			}
			if (!words_left) { PROF_BEGIN(); decode(); PROF_END(PROF_MDEC); state = IDLE; }
			break;
		case 2:
			state = QUANT;
			qt_color = v & 1;
			words_left = qt_color ? 32 : 16;
			qt_pos = 0;
			break;
		case 3:
			state = SCALE;
			words_left = 32;
			sc_pos = 0;
			break;
		}
		break;
	case DECODE:
		if (in_len + 2 <= in_cap) {
			in_buf[in_len++] = (u16)v;
			in_buf[in_len++] = (u16)(v >> 16);
		}
		if (--words_left == 0) { PROF_BEGIN(); decode(); PROF_END(PROF_MDEC); state = IDLE; dma_mdec_kick(); }
		break;
	case QUANT:
		for (int i = 0; i < 4; i++, qt_pos++) {
			u8 b = (u8)(v >> (i * 8));
			if (qt_pos < 64) qt_y[qt_pos] = b; else qt_c[qt_pos - 64] = b;
		}
		if (--words_left == 0) state = IDLE;
		break;
	case SCALE:
		scale[sc_pos++] = (s16)v;
		scale[sc_pos++] = (s16)(v >> 16);
		if (--words_left == 0) { state = IDLE; check_scale(); }
		break;
	}
}

u32 mdec_read_data(void)
{
	u32 v = 0;
	for (int i = 0; i < 4; i++) {
		if (out_pos < out_len) v |= (u32)out_buf[out_pos++] << (i * 8);
	}
	return v;
}

// Hasta n palabras enteras de la salida de golpe (la DMA de salida de los vídeos); devuelve cuántas
u32 mdec_read_words(u32 *dst, u32 n)
{
	u32 k = (out_len - out_pos) / 4;
	if (out_pos >= out_len || (out_pos & 3)) return 0;
	if (k > n) k = n;
	memcpy(dst, out_buf + out_pos, k * 4);
	out_pos += k * 4;
	return k;
}

void mdec_write_ctrl(u32 v)
{
#ifdef PSX_CDLOG
	{ extern void cdlog_any(u32 v); cdlog_any(0x31000000u | (v >> 8)); }
#endif
	if (v & 0x80000000) mdec_reset();
	ctrl = v & 0x60000000;
}

u32 mdec_status(void)
{
	u32 s = 0;
	if (out_pos >= out_len) s |= 1u << 31;                         // salida vacía
	if (state != IDLE) s |= 1u << 29;                              // comando en marcha
	if ((ctrl & (1 << 30)) && state != IDLE) s |= 1u << 28;        // pide datos de entrada
	if ((ctrl & (1 << 29)) && out_pos < out_len) s |= 1u << 27;    // tiene datos de salida
	s |= ((cmd >> 25) & 0xf) << 23;                                // profundidad, signo y bit 15
	s |= (state != IDLE ? (words_left - 1) : 0xffff) & 0xffff;
	return s;
}

int mdec_out_available(void) { return out_pos < out_len; }

// Los búferes se guardan con su longitud; al cargar se hacen a medida. Si no hay memoria (modo DS) el
// contenido se salta y el búfer queda vacío (devuelve 0): un vídeo a medias se pierde, el resto del estado no.
static int buf_state(PsxState *s, void **buf, u32 *cap, u32 len)
{
	if (s->load && len > *cap) {
		void *nb = realloc(*buf, len);
		if (!nb) {
			static u8 skip[256];
			for (u32 o = 0; o < len; o += sizeof skip) psx_state_io(s, skip, len - o < sizeof skip ? len - o : sizeof skip);
			return 0;
		}
		*buf = nb;
		*cap = len;
	}
	if (len) psx_state_io(s, *buf, len);
	return 1;
}

void mdec_state(PsxState *s)
{
	u32 in_bytes;
	STATE(s, qt_y); STATE(s, qt_c); STATE(s, scale);
	STATE(s, cmd); STATE(s, words_left); STATE(s, ctrl);
	STATE(s, qt_pos); STATE(s, qt_color); STATE(s, sc_pos); STATE(s, state);
	STATE(s, in_len);
	STATE(s, out_len); STATE(s, out_pos);
	in_bytes = in_len * 2;
	{ u32 cap = in_cap * 2; void *b = in_buf; if (!buf_state(s, &b, &cap, in_bytes)) in_len = 0; in_buf = b; in_cap = cap / 2; }
	{ void *b = out_buf; if (!buf_state(s, &b, &out_cap, out_len)) out_len = out_pos = 0; out_buf = b; }
	if (s->load) check_scale();
}

// Varias palabras de golpe (la DMA de entrada): los datos de decodificar se copian tal cual (cada palabra son sus
// dos medias palabras en el mismo orden); lo demás, palabra a palabra
void mdec_write_words(const u32 *w, u32 n)
{
	while (n) {
		if (state == DECODE && words_left > 1) {
			u32 k = words_left - 1 < n ? words_left - 1 : n;   // la última, por mdec_write_cmd (decodifica)
			u32 room = (in_cap - in_len) / 2, c = k < room ? k : room;   // sin memoria: lo que no quepa se descarta
			memcpy(in_buf + in_len, w, c * 4);
			in_len += c * 2;
			words_left -= k;
			w += k;
			n -= k;
			continue;
		}
		mdec_write_cmd(*w++);
		n--;
	}
}
