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

// Lee un bloque comprimido; devuelve 0 si se acabaron los datos
static int rl_decode(s16 *blk, const u16 **src, const u16 *end, const u8 *qt)
{
	const u16 *p = *src;
	int k, q, val;
	u16 n;
	memset(blk, 0, 64 * sizeof(s16));
	while (p < end && *p == 0xfe00) p++;
	if (p >= end) return 0;
	n = *p++;
	q = (n >> 10) & 63;
	k = 0;
	val = s10(n) * qt[0];
	int ac = 0;
	for (;;) {
		if (q == 0) val = s10(n) * 2;
		if (val < -0x400) val = -0x400;
		if (val > 0x3ff) val = 0x3ff;
		if (q > 0) blk[zigzag[k]] = (s16)val;
		else blk[k] = (s16)val;
		if (k) ac |= val;
		if (k >= 63 || p >= end) break;   // un bloque lleno termina sin código FE00h
		n = *p++;
		k += ((n >> 10) & 63) + 1;
		if (k > 63) break;
		val = (s10(n) * qt[k] * q + 4) / 8;
	}
	*src = p;
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

static void idct_1d(s32 *o, s32 i0, s32 i1, s32 i2, s32 i3, s32 i4, s32 i5, s32 i6, s32 i7, int shift_even)
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

// IDCT con atajo: un bloque con solo la continua es un relleno (la misma cuenta que idct_fast)
static void idct_dc(s16 *blk, int dc_only)
{
	if (dc_only && scale_is_standard) {
		s16 v = (s16)DESC(blk[0] << P1, P1 + 3);
		u32 vv = (u16)v | (u32)(u16)v << 16, *w = (u32 *)blk;
		for (int i = 0; i < 32; i++) w[i] = vv;
		return;
	}
	idct(blk);
}

static inline int clamp8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

// Un macrobloque de 16x16 en color: escribe 24 o 15 bits por píxel
static void yuv_block(const s16 *cr, const s16 *cb, const s16 *y, int xx, int yy, u8 *rgb)
{
	// cada muestra de color sirve a 2x2 píxeles: sus términos se calculan una vez (mismas fórmulas que antes)
	for (int cy = 0; cy < 4; cy++)
		for (int cx = 0; cx < 4; cx++) {
			int ci = cx + (xx >> 1) + (cy + (yy >> 1)) * 8, Rc = cr[ci], Bc = cb[ci];
			int G = (-88 * Bc - 183 * Rc + 128) >> 8, R = (359 * Rc + 128) >> 8, B = (454 * Bc + 128) >> 8;
			for (int d = 0; d < 4; d++) {
				int px = cx * 2 + (d & 1), py = cy * 2 + (d >> 1), Y = y[px + py * 8];
				u8 *o = rgb + ((px + xx) + (py + yy) * 16) * 3;
				o[0] = (u8)clamp8(Y + R);
				o[1] = (u8)clamp8(Y + G);
				o[2] = (u8)clamp8(Y + B);
			}
		}
}

static void decode(void)
{
	int depth = (cmd >> 27) & 3, sign = (cmd >> 26) & 1, b15 = (cmd >> 25) & 1;
	const u16 *p = in_buf, *end = in_buf + in_len;
	s16 blk[6][64];
	u8 rgb[16 * 16 * 3];

	out_len = out_pos = 0;
	if (depth >= 2) {   // color: Cr, Cb, Y1..Y4
		for (;;) {
			int ok = 1;
			for (int b = 0; b < 6 && ok; b++) {
				ok = rl_decode(blk[b], &p, end, b < 2 ? qt_c : qt_y);
				if (ok) idct_dc(blk[b], ok == 2);
			}
			if (!ok) break;
			STAT_ADD(mdec_blocks, 1);
			yuv_block(blk[0], blk[1], blk[2], 0, 0, rgb);
			yuv_block(blk[0], blk[1], blk[3], 8, 0, rgb);
			yuv_block(blk[0], blk[1], blk[4], 0, 8, rgb);
			yuv_block(blk[0], blk[1], blk[5], 8, 8, rgb);
			if (!sign) for (int i = 0; i < 768; i++) rgb[i] ^= 0x80;
			if (depth == 2) {
				if (!out_reserve(768)) break;
				memcpy(out_buf + out_len, rgb, 768);
				out_len += 768;
			} else {
				if (!out_reserve(512)) break;
				for (int i = 0; i < 256; i++) {
					u16 c = (u16)((rgb[i * 3] >> 3) | ((rgb[i * 3 + 1] >> 3) << 5) | ((rgb[i * 3 + 2] >> 3) << 10) | (b15 << 15));
					out_buf[out_len++] = (u8)c;
					out_buf[out_len++] = (u8)(c >> 8);
				}
			}
		}
	} else {   // monocromo: solo Y
		while (rl_decode(blk[0], &p, end, qt_y)) {
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
