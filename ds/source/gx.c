// GPU de la PS1 con el motor 3D de la DS: los polígonos y sprites que el juego dibuja en la pantalla se
// mandan al hardware (casi gratis para la CPU) en vez de rellenarlos por software.
//
// - Orden: la PS1 pinta en orden (algoritmo del pintor); aquí cada polígono lleva una profundidad que
//   baja con el orden de llegada, así que lo último queda delante. Los semitransparentes se dibujan en
//   orden de llegada (orden manual) y no escriben profundidad.
// - Texturas: cada página de la VRAM de la PS1 (256x256 a 4/8/15 bits) se copia a la memoria de
//   texturas (bancos B y D, 8 huecos de 32 KB). Los formatos de 4 y 8 bits son iguales en las dos
//   máquinas; las paletas (CLUT) van al banco E. Se invalidan cuando el juego escribe en esa zona.
// - Lo que el juego sube a la VRAM (vídeos, fondos) se sigue viendo en el fondo de mapa de bits (BG3,
//   detrás); el 3D va encima con el fondo transparente.
// - Aproximaciones: la mezcla aditiva/sustractiva de la PS1 sale como transparencia normal; la
//   semitransparencia por texel, como de todo el polígono; los colores por encima de 1.0 se recortan.
//
// Los comandos de cada frame se guardan en una lista y se mandan juntos cuando el juego cambia de
// buffer: así no hay que esperar a que el hardware acabe el frame anterior.
#include <nds.h>
#include <string.h>
#include <stdlib.h>
#include "psx.h"
#include "gx.h"

#ifdef PSX_SELFTEST
#define MAX_POLYS 1200   // en el modo DS la RAM va justa
#else
#define MAX_POLYS 2000   // el hardware admite 2048 por frame
#endif
#define LIST_WORDS (MAX_POLYS * 26 + 16)

// Dos listas: mientras la DMA lleva una al FIFO del 3D (en segundo plano, modo GXFIFO), los polígonos del frame
// siguiente se escriben en la otra
#define GX_DMA 1
static u32 list_buf[2][LIST_WORDS + 2] __attribute__((aligned(32)));
static u32 *list = list_buf[0];
static int list_cur;
static int ln, npolys;
static u32 frame_no;
// Fondo BG3 (la VRAM de la PS1 copiada a la pantalla): con el 3D solo hace falta copiarlo si el juego escribió
// en la zona mostrada (vídeos, imágenes subidas). Por franjas de 16 filas: cambiado desde la última copia, y
// con algo escrito alguna vez (si no, da igual qué buffer se ve: lo dibuja el 3D).
static u8 band_dirty[32], band_content[32];
static int shown_y = -1;

int gx_need_copy(void)
{
	int y0 = gpu_disp.disp_y >> 4, y1 = (gpu_disp.disp_y + gpu_disp.height - 1) >> 4, dirty = 0, content = 0;
	for (int b = y0; b <= y1; b++) { dirty |= band_dirty[b & 31]; content |= band_content[b & 31]; }
	if (!dirty && !(content && shown_y != gpu_disp.disp_y) && shown_y >= 0) return 0;
	for (int b = y0; b <= y1; b++) band_dirty[b & 31] = 0;
	shown_y = gpu_disp.disp_y;
	return 1;
}
int gx_bg_hidden;           // el 3D tapa toda la pantalla: no hace falta copiar el fondo
// Depuración: en qué paso está gx (tools/dsbench lo enseña al final si encuentra la marca)
GxDbg gx_dbg = { "SPDBGSTG", 0, 0, 0, 0 };
#define STAGE(s) (gx_dbg.stage = (s), gx_dbg.n++)

// ---- paletas: banco E (64 KB) ----
#define PAL_HASH 2048
typedef struct { s16 x, y; u16 n, slot; u8 lum; } PalEnt;   // n = 0: libre; slot: hueco de su tamaño (ver pal_get); lum = brillo máximo 0-31
static PalEnt pal[PAL_HASH];

static u16 *bank_lcd(int bank)   // dirección de un banco cuando está en modo LCD
{
	static u16 *const addr[4] = { (u16 *)0x06820000, (u16 *)0x06860000, (u16 *)0x06840000, (u16 *)0x06880000 };   // B, D, C, E
	return addr[bank];
}

// ---- copias en la RAM principal ----
// En la DSi real el motor 3D lee las texturas y paletas mientras dibuja la pantalla, línea a línea: si se
// pasa un banco a modo LCD para escribir en él a mitad de frame, lee basura (píxeles de otros colores,
// parpadeos). melonDS dibuja el 3D de golpe y no lo enseña. Así que se escribe en estas copias y lo cambiado
// se lleva a la VRAM por DMA en la vblank en que el 3D cambia de buffer (gx_vblank), antes de que empiece a
// dibujar el frame nuevo. El frame que aún se ve sigue con sus texturas intactas hasta entonces.
#define GX_NR 13            // 12 huecos de textura + las paletas
#define GX_PAL 12
static u16 *sh_tex, *sh_pal;   // NULL: sin memoria, se escribe directo en la VRAM (como antes)
static u32 pend_lo[GX_NR], pend_hi[GX_NR];          // bytes cambiados desde el último cambio de buffer
static u32 com_lo[GX_NR], com_hi[GX_NR];            // los que esperan a la próxima vblank
static volatile int commit_on;

// Se va a escribir [lo, hi) del hueco k (o de las paletas): si eso aún tiene que llegar a la VRAM para el
// frame que se acaba de mandar, esperar a que llegue; luego se apunta como cambiado.
static void sh_mark(int k, u32 lo, u32 hi)
{
	if (commit_on && com_lo[k] < hi && lo < com_hi[k]) while (commit_on) { }
	if (pend_lo[k] >= pend_hi[k]) { pend_lo[k] = lo; pend_hi[k] = hi; }
	else { if (lo < pend_lo[k]) pend_lo[k] = lo; if (hi > pend_hi[k]) pend_hi[k] = hi; }
}

// Bytes [b0, b1) de una textura que empieza en el hueco u (puede ocupar varios)
static void sh_mark_tex(int u, u32 b0, u32 b1)
{
	for (u32 k = b0 >> 15; k <= (b1 - 1) >> 15; k++) {
		u32 base = k << 15, lo = b0 > base ? b0 - base : 0, hi = b1 < base + 0x8000 ? b1 - base : 0x8000;
		sh_mark(u + (int)k, lo, hi);
	}
}

static void bank_mode(int bank, int lcd)   // 0 = B, 1 = D, 2 = C, 3 = E
{
	switch (bank) {
	case 0: vramSetBankB(lcd ? VRAM_B_LCD : VRAM_B_TEXTURE_SLOT0); break;
	case 1: vramSetBankD(lcd ? VRAM_D_LCD : VRAM_D_TEXTURE_SLOT1); break;
	case 2: vramSetBankC(lcd ? VRAM_C_LCD : VRAM_C_TEXTURE_SLOT2); break;
	default: vramSetBankE(lcd ? VRAM_E_LCD : VRAM_E_TEX_PALETTE); break;
	}
}

// En la vblank (main.c): si el 3D ya cambió de buffer, lleva lo cambiado a la VRAM
volatile u32 gx_underflows, gx_late, gx_rdlines_min = 99;

void gx_vblank(void)
{
	// diagnóstico para el registro (hud.c): líneas que el 3D no llegó a dibujar a tiempo (demasiados polígonos
	// en una línea; en la DSi real salen con colores raros, melonDS no lo imita) y su margen mínimo
	vu16 *cnt = (vu16 *)0x04000060;
	u32 rl = *(vu8 *)0x04000320;
	if (*cnt & (1u << 12)) { gx_underflows++; *cnt |= 1u << 12; }
	if (rl < gx_rdlines_min) gx_rdlines_min = rl;
	if (!commit_on) return;
	if (GFX_STATUS & (1u << 27)) { gx_late++; return; }   // el motor aún no ha cambiado de buffer: en la siguiente
	for (int k = 0; k < GX_NR; k++) {
		u32 lo = com_lo[k] & ~3u, hi = (com_hi[k] + 3) & ~3u;
		int bank = k == GX_PAL ? 3 : k >> 2;
		const u8 *src;
		u8 *dst;
		if (lo >= hi) continue;
		src = k == GX_PAL ? (const u8 *)sh_pal + lo : (const u8 *)sh_tex + ((u32)k << 15) + lo;
		dst = k == GX_PAL ? (u8 *)bank_lcd(3) + lo : (u8 *)bank_lcd(bank) + ((u32)(k & 3) << 15) + lo;
		bank_mode(bank, 1);
		dmaCopyWords(2, src, dst, hi - lo);
		bank_mode(bank, 0);
		com_lo[k] = com_hi[k] = 0;
	}
	commit_on = 0;
}

// Paletas en huecos fijos con reemplazo del menos usado: 1024 de 16 colores (32 bytes, primeros 32 KB; el 0
// es la paleta blanca de la rampa) y 64 de 256 colores (512 bytes, los otros 32 KB). Nunca se reutiliza un hueco
// que ya usa un polígono de este frame: las paletas suben a la VRAM al cambiar de buffer y todos los polígonos
// del frame verían la última (antes la memoria se vaciaba entera a mitad de frame y salían colores cambiados).
#define P16_N 1024
#define P256_N 64
static u32 p16_used[P16_N], p256_used[P256_N];      // frame_no + 1 del último uso (0 = nunca)
static s16 p16_owner[P16_N], p256_owner[P256_N];    // entrada de pal[] que lo ocupa (-1 = libre)
static int p16_hand = 1, p256_hand;

static void pal_reset(void)
{
	memset(pal, 0, sizeof pal);
	memset(p16_used, 0, sizeof p16_used);
	memset(p256_used, 0, sizeof p256_used);
	for (int i = 0; i < P16_N; i++) p16_owner[i] = -1;
	for (int i = 0; i < P256_N; i++) p256_owner[i] = -1;
	p16_hand = 1;
	p256_hand = 0;
}

static void pal_mark_used(const PalEnt *e)
{
	if (e->n == 16) p16_used[e->slot] = frame_no + 1;
	else p256_used[e->slot] = frame_no + 1;
}

static u32 pal_off(const PalEnt *e) { return e->n == 16 ? (u32)e->slot * 32 : 0x8000 + (u32)e->slot * 512; }

// Base de paleta (para PLTT_BASE) con los n colores de la CLUT en (cx, cy). n = 32: los 16 colores
// llevados a brillo máximo (para A3I5, donde el brillo va en el alfa de cada texel) y 16 negros.
static u32 pal_get(int cx, int cy, int n, int *lum)
{
	STAGE(30);
	u32 h = ((u32)cy * 64 + (u32)cx / 16 + (n == 256 ? 0x9e37 : n == 32 ? 0x4f1b : 0)) & (PAL_HASH - 1), off;
	PalEnt *e = &pal[h];
	u16 *dst;
	u32 sum = 0, now = frame_no + 1;
	int small = n == 16, slot = 0;
	if (e->n == n && e->x == cx && e->y == cy) { pal_mark_used(e); *lum = e->lum; return pal_off(e) >> 4; }
	// hueco: el siguiente que no se haya usado en este frame (vuelta circular = aproximadamente el más viejo)
	if (small) {
		for (int t = 0; t < P16_N; t++) {
			slot = p16_hand;
			p16_hand = p16_hand + 1 < P16_N ? p16_hand + 1 : 1;
			if (p16_used[slot] != now) break;
		}
		if (p16_owner[slot] >= 0) pal[p16_owner[slot]].n = 0;
	} else {
		for (int t = 0; t < P256_N; t++) {
			slot = p256_hand;
			p256_hand = (p256_hand + 1) % P256_N;
			if (p256_used[slot] != now) break;
		}
		if (p256_owner[slot] >= 0) pal[p256_owner[slot]].n = 0;
	}
	if (e->n) {   // la entrada tenía otra CLUT: su hueco queda libre
		if (e->n == 16) p16_owner[e->slot] = -1; else p256_owner[e->slot] = -1;
	}
	e->x = (s16)cx; e->y = (s16)cy; e->n = (u16)(small ? 16 : n); e->slot = (u16)slot;
	if (small) { p16_owner[slot] = (s16)h; p16_used[slot] = now; }
	else { p256_owner[slot] = (s16)h; p256_used[slot] = now; }
	e->n = (u16)n;
	off = small ? (u32)slot * 32 : 0x8000 + (u32)slot * 512;
	if (sh_pal) { sh_mark(GX_PAL, off, off + (u32)n * 2); dst = sh_pal + off / 2; }
	else { vramSetBankE(VRAM_E_LCD); dst = bank_lcd(3) + off / 2; }
	for (int i = 0; i < n; i++) {
		u16 c = i < (n == 32 ? 16 : n) ? psx_vram[cy * 1024 + ((cx + i) & 1023)] : 0;
		int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31, m = r > g ? (r > b ? r : b) : (g > b ? g : b);
		if (n == 32 && m) { r = r * 31 / m; g = g * 31 / m; b = b * 31 / m; }
		dst[i] = (u16)(r | (g << 5) | (b << 10));
		if (c && (u32)m > sum) sum = (u32)m;   // brillo máximo de la CLUT
	}
	if (!sh_pal) vramSetBankE(VRAM_E_TEX_PALETTE);
	e->lum = (u8)sum;
	*lum = e->lum;
	return off >> 4;
}

static void pal_dirty(int x, int y, int w, int h)
{
	for (int i = 0; i < PAL_HASH; i++) {
		PalEnt *e = &pal[i];
		if (e->n && e->y >= y && e->y < y + h && e->x < x + w && e->x + e->n > x) {
			if (e->n == 16) p16_owner[e->slot] = -1; else p256_owner[e->slot] = -1;
			e->n = 0;
		}
	}
}

// ---- texturas: 12 huecos de 32 KB (banco B = huecos 0-3, D = 4-7, C = 8-11) ----
// depth 1/2/3 = página de 4/8/15 bits tal cual; 4 = página de 4 bits como A3I5 (8 bits por texel con alfa
// por texel sacado del brillo de su color en la CLUT cx, cy: para las capas aditivas)
#define UNITS 12
typedef struct { s16 x, y, cx, cy; u8 depth, units, used; s16 d0, d1; u32 stamp; } TexEnt;   // used = 0: libre; filas [d0, d1) por volver a copiar
static TexEnt tex[UNITS];   // la entrada i ocupa los huecos i .. i+units-1 (alineada a su tamaño)

static void tex_reset(void) { memset(tex, 0, sizeof tex); }

static int src_w(int depth) { return depth == 1 || depth == 4 ? 64 : depth == 2 ? 128 : 256; }   // medias palabras por fila
static int dst_row(int depth) { return depth == 1 ? 128 : depth == 3 ? 512 : 256; }              // bytes por fila
static int units_of(int depth) { return depth == 1 ? 1 : depth == 3 ? 4 : 2; }

static void tex_dirty(int x, int y, int w, int h)
{
	for (int i = 0; i < UNITS; i++) {
		TexEnt *t = &tex[i];
		if (!t->used) continue;
		if (t->x < x + w && t->x + src_w(t->depth) > x && t->y < y + h && t->y + 256 > y) {   // solo esas filas
			int r0 = y - t->y, r1 = y + h - t->y;
			if (r0 < 0) r0 = 0;
			if (r1 > 256) r1 = 256;
			if (t->d0 == t->d1) { t->d0 = (s16)r0; t->d1 = (s16)r1; }
			else { if (r0 < t->d0) t->d0 = (s16)r0; if (r1 > t->d1) t->d1 = (s16)r1; }
		}
	}
}

// Copia la página a los huecos desde u (en modo LCD los bancos B y D no son contiguos: por huecos)
static void tex_upload(const TexEnt *t, int u, int row0, int row1)
{
	STAGE(20);
	int tw = src_w(t->depth), rb = dst_row(t->depth);
	u16 lut[256];   // A3I5: un byte de origen (2 texels) -> 2 bytes
	if (t->depth == 4) {
		// alfa relativo al color más brillante de la CLUT plantilla (la primera con la que se usa la página):
		// las demás CLUT de esa página suelen ser la misma con otro brillo, que va en el alfa del polígono
		u8 b[16], m[16], mmax = 1;
		for (int i = 0; i < 16; i++) {
			u16 c = psx_vram[t->cy * 1024 + ((t->cx + i) & 1023)];
			int r = c & 31, g = (c >> 5) & 31, bl = (c >> 10) & 31;
			m[i] = (u8)(r > g ? (r > bl ? r : bl) : (g > bl ? g : bl));
			if (m[i] > mmax) mmax = m[i];
		}
		for (int i = 0; i < 16; i++) b[i] = (u8)(i | (((m[i] * 7 + mmax / 2) / mmax) << 5));
		for (int k = 0; k < 256; k++) lut[k] = (u16)(b[k & 15] | (b[k >> 4] << 8));
	}
	if (sh_tex) sh_mark_tex(u, (u32)row0 * (u32)rb, (u32)row1 * (u32)rb);
	else { vramSetBankB(VRAM_B_LCD); vramSetBankD(VRAM_D_LCD); vramSetBankC(VRAM_C_LCD); }
	for (int row = row0; row < row1; row++) {
		u32 byte = (u32)row * (u32)rb;   // posición en la textura
		int unit = u + (int)(byte >> 15);
		u16 *dst = sh_tex ? sh_tex + ((u32)unit << 14) + ((byte & 0x7fff) >> 1)
		                  : bank_lcd(unit >> 2) + ((unit & 3) << 14) + ((byte & 0x7fff) >> 1);
		const u16 *src = &psx_vram[((t->y + row) & 511) * 1024];
		if (t->depth == 3) {
			for (int k = 0; k < tw; k++) {
				u16 c = src[(t->x + k) & 1023];
				dst[k] = c ? (c | 0x8000) : 0;
			}
		} else if (t->depth == 4) {
			for (int k = 0; k < tw; k++) {
				u16 s = src[(t->x + k) & 1023];
				dst[2 * k] = lut[s & 0xff];
				dst[2 * k + 1] = lut[s >> 8];
			}
		} else if (t->x + tw <= 1024) {
			const u32 *s32 = (const u32 *)(src + t->x);
			u32 *d32 = (u32 *)dst;
			for (int k = 0; k < tw / 2; k++) d32[k] = s32[k];
		} else {
			for (int k = 0; k < tw; k++) dst[k] = src[(t->x + k) & 1023];
		}
	}
	if (!sh_tex) { vramSetBankB(VRAM_B_TEXTURE_SLOT0); vramSetBankD(VRAM_D_TEXTURE_SLOT1); vramSetBankC(VRAM_C_TEXTURE_SLOT2); }
}

// Hueco donde está (o se carga) la página; devuelve el primero
static int tex_get(int x, int y, int depth, int cx, int cy)
{
	int units = units_of(depth), best = 0;
	u32 best_age = 0;
	for (int i = 0; i < UNITS; i++) {
		TexEnt *t = &tex[i];
		if (t->used && t->x == x && t->y == y && t->depth == depth) {
			t->stamp = frame_no;
			if (t->d0 != t->d1) { tex_upload(t, i, t->d0, t->d1); t->d0 = t->d1 = 0; STAT_ADD(gpu_lines, 1); }
			return i;
		}
	}
	// el bloque alineado cuyo uso más reciente sea el más viejo (libre = viejísimo)
	for (int i = 0; i + units <= UNITS - 1; i += units) {   // el hueco 11 es de la rampa
		u32 newest = 0, age;
		for (int k = 0; k < UNITS; k++) {   // entradas que tocan [i, i+units)
			TexEnt *t = &tex[k];
			if (t->used && k < i + units && k + t->units > i && t->stamp + 1 > newest) newest = t->stamp + 1;
		}
		age = frame_no + 1 - newest;
		if (age > best_age) { best_age = age; best = i; }
	}
	for (int k = 0; k < UNITS; k++)
		if (tex[k].used && k < best + units && k + tex[k].units > best) tex[k].used = 0;
	tex[best].x = (s16)x; tex[best].y = (s16)y; tex[best].cx = (s16)cx; tex[best].cy = (s16)cy;
	tex[best].depth = (u8)depth; tex[best].units = (u8)units; tex[best].used = 1; tex[best].stamp = frame_no;
	tex[best].d0 = tex[best].d1 = 0;
	tex_upload(&tex[best], best, 0, 256);
	STAT_ADD(gpu_lines, 1000);   // diagnóstico temporal: subidas de texturas
	return best;
}

// Rampa de transparencia: textura A5I3 de 32x8 en el hueco 11 con alfa = columna (0-31) y color blanco.
// Da alfa por píxel interpolando la coordenada s entre vértices (la DS no tiene alfa por vértice).
#define RAMP_TEXIMG ((11u << 12) | (2u << 20) | (0u << 23) | (6u << 26))

static void ramp_init(void)
{
	u16 *t = bank_lcd(2) + 3 * 0x4000, *pl = bank_lcd(3);   // hueco 11 = último cuarto del banco C
	vramSetBankC(VRAM_C_LCD);
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 32; x += 2) t[y * 16 + x / 2] = (u16)((x << 3) | ((x + 1) << 11));   // A5I3: alfa << 3, índice 0
	vramSetBankC(VRAM_C_TEXTURE_SLOT2);
	vramSetBankE(VRAM_E_LCD);
	for (int i = 0; i < 8; i++) pl[i] = 0x7fff;
	vramSetBankE(VRAM_E_TEX_PALETTE);
}

// ---- polígonos ----
static inline u32 rgb5(int r, int g, int b) { return (u32)(r >> 3) | ((u32)(g >> 3) << 5) | ((u32)(b >> 3) << 10); }
static inline int mod5(int c) { c >>= 2; return c > 31 ? 31 : c; }   // color de modulación: 128 = 1.0
static u32 inv255[256];   // inv255[m] = 255 * 65536 / m redondeado hacia arriba: (r * inv255[m]) >> 16 == r * 255 / m
static u8 mod5t[256];     // color de modulación (128 = 1.0) a 5 bits, saturado

static void gx_poly(const GpuHwVtx *v, int n, const GpuHwPoly *p)
{
	STAGE(40);
	static const u8 alpha_of[5] = { 31, 16, 16, 14, 8 };   // opaco, B/2+F/2, B+F, B-F, B+F/4 (aproximados)
	static const int order[4] = { 0, 1, 3, 2 };            // cuadrado de la PS1 -> perímetro de la DS
	int w = gpu_disp.width, h = gpu_disp.height;
	static int last_w, last_h;
	static s32 kx, ky;   // escala a coordenadas de la DS (4.12) por píxel, en 24.8: se recalcula si cambia la resolución
	u32 attr, teximg = 0, pltt = 0, *o;
	u32 z;
	int lum = 24, alpha, white = 0, black = 0;
	// B+F, B-F y B+F/4 no existen en la DS: B+F ~ «pantalla» (B*(1-a) + C*a con a = brillo de F), B-F ~
	// negro con a = brillo de F. Con textura de 4 bits, el brillo de cada texel va en su alfa (A3I5) y el
	// del color de los vértices en el alfa del polígono.
	int addlike = p->semi >= 2, a3 = 0, vb = 0, ramp = 0;   // a3 (A3I5 por página) desactivado: llenaba la caché
	if (npolys >= MAX_POLYS) return;
	if (w != last_w || h != last_h) { kx = (8192 << 8) / w; ky = (8192 << 8) / h; last_w = w; last_h = h; }
	// Con el bit de máscara (comprobar y poner) la PS1 solo pinta sobre lo que no lo tiene: en Silent Hill son
	// velos de niebla que tiñen solo al personaje (el fondo y la niebla ya tienen la marca). La DS no puede
	// limitarlos así y saldrían como un recuadro: mejor no dibujarlos.
	if (p->mask && p->semi) return;
	if (addlike) {
		for (int k = 0; k < n; k++) vb += (v[k].r > v[k].g ? (v[k].r > v[k].b ? v[k].r : v[k].b) : (v[k].g > v[k].b ? v[k].g : v[k].b));
		vb = p->raw && p->tex ? 128 : n == 4 ? vb >> 2 : (vb * 21846) >> 16;   // vb / n sin dividir (n = 3 o 4)
	}
	if (p->tex) {
		int u = tex_get(p->tpx, p->tpy, a3 ? 4 : p->tex, p->cx, p->cy), fmt = a3 ? 1 : p->tex == 1 ? 3 : p->tex == 2 ? 4 : 7;
		int c0 = 0;
		if (p->tex < 3) {
			pltt = pal_get(p->cx, p->cy, a3 ? 32 : p->tex == 1 ? 16 : 256, &lum);
			c0 = psx_vram[p->cy * 1024 + p->cx] == 0;   // índice 0 transparente si su color es 0
		}
		teximg = ((u32)u << 12) | (1u << 16) | (1u << 17) | (5u << 20) | (5u << 23) | ((u32)fmt << 26) | ((u32)c0 << 29);
	}
	alpha = alpha_of[p->semi];
	if (addlike) {
		int f = !p->tex ? vb >> 3 : lum * vb / 128;   // 0-31; con A3I5 el reparto entre texels va en la textura
		if (p->semi == 4) f >>= 2;
		alpha = f < 1 ? 1 : f > 30 ? 30 : f;
		if (!p->tex) {   // sin textura: alfa por píxel con la rampa (coordenada s = brillo de cada vértice)
			ramp = 1;
			alpha = 31;
			teximg = RAMP_TEXIMG;
		}
		if (p->semi == 3) black = 1; else white = 1;
	}
	attr = (3u << 6) | (1u << 12) | (1u << 13) | ((u32)alpha << 16);
	// ID: el hardware no pinta un semitransparente sobre otro con el mismo ID. Distintos para que se mezclen;
	// el mismo (62) para los que usan el bit de máscara de la PS1, que solo dejan pintar cada píxel una vez
	if (p->semi) attr |= (u32)(p->mask ? 62 : 1 + (npolys % 61)) << 24;
	o = &list[ln];
	*o++ = 0x29 | (0x2a << 8) | (0x2b << 16) | (0x40 << 24);   // POLY_FORMAT, TEXIMAGE_PARAM, PLTT_BASE, BEGIN_VTXS
	*o++ = attr;
	*o++ = teximg;
	*o++ = pltt;
	*o++ = n == 4 ? 1 : 0;
	z = (u32)(4000 - npolys * 3) & 0xffff;
	{
		// color de cada vértice: constante, normalizado (aditivos), RGB tal cual o modulación de textura
		enum { CM_CONST, CM_NORM, CM_RGB, CM_MOD } cm;
		u32 kcol = 0;
		int flat = !p->gouraud && !ramp;   // un solo COLOR para todo el polígono (el hardware lo mantiene)
		if (black) cm = CM_CONST;
		else if (white && p->tex) { cm = CM_CONST; kcol = 0x7fff; }   // el brillo ya va en el alfa
		else if (white) cm = CM_NORM;
		else if (!p->tex) cm = CM_RGB;
		else if (p->raw) { cm = CM_CONST; kcol = 0x7fff; }
		else cm = CM_MOD;
		for (int k = 0; k < n; k++) {
			const GpuHwVtx *q = &v[n == 4 ? order[k] : k];
			s32 x = (((s32)(q->x - p->ox) * kx) >> 8) - 4096, y = 4096 - (((s32)(q->y - p->oy) * ky) >> 8);
			if ((u32)(x + 32768) > 0xffff) x = x < 0 ? -32768 : 32767;
			if ((u32)(y + 32768) > 0xffff) y = y < 0 ? -32768 : 32767;
			if (k == 0 || !flat) {
				u32 col;
				switch (cm) {
				case CM_CONST: col = kcol; break;
				case CM_NORM: {   // color llevado a brillo máximo (el brillo va en el alfa): r * 255 / m con la tabla de inversos
					int m = q->r > q->g ? (q->r > q->b ? q->r : q->b) : (q->g > q->b ? q->g : q->b);
					u32 iv = inv255[m];
					col = rgb5((int)((q->r * iv) >> 16), (int)((q->g * iv) >> 16), (int)((q->b * iv) >> 16));
					break;
				}
				case CM_RGB: col = rgb5(q->r, q->g, q->b); break;
				default: col = mod5t[q->r] | ((u32)mod5t[q->g] << 5) | ((u32)mod5t[q->b] << 10); break;
				}
				*o++ = 0x20 | (0x22 << 8) | (0x23 << 16);   // COLOR, TEXCOORD, VTX_16
				*o++ = col;
			} else *o++ = 0x22 | (0x23 << 8);           // TEXCOORD, VTX_16 (el color sigue)
			if (ramp) {
				int m = q->r > q->g ? (q->r > q->b ? q->r : q->b) : (q->g > q->b ? q->g : q->b);
				m >>= p->semi == 4 ? 5 : 3;
				if (m < 1) m = 1;   // con alfa 0 la DS no pinta y no marcaría el píxel (ver p->mask)
				if (m > 31) m = 31;
				*o++ = (8u << 16) | (u32)(m * 16 + 8);
			} else
				*o++ = ((u32)(q->v << 4) << 16) | ((u32)(q->u << 4) & 0xffff);
			*o++ = ((u32)y << 16) | ((u32)x & 0xffff);
			*o++ = z;
		}
	}
	ln = (int)(o - list);
	npolys++;
}

static void gx_vram_written(int x, int y, int w, int h)
{
	tex_dirty(x, y, w, h);
	pal_dirty(x, y, w, h);
	if (x < gpu_disp.disp_x + gpu_disp.width && x + w > gpu_disp.disp_x)
		for (int b = y >> 4; b <= (y + h - 1) >> 4; b++) band_dirty[b & 31] = band_content[b & 31] = 1;
}

static u32 clear_next;   // color de fondo del 3D para el frame en curso (0 = transparente: se ve el fondo BG3)

static void gx_clear(u16 c)
{
	clear_next = c == 0xffff ? 0 : (c & 0x7fffu) | (31u << 16) | (63u << 24);   // opaco: tapa el BG3, que ya no se borra; 0xffff = transparente
}

static void gx_flip(int drawn)
{
	STAGE(10);
	if (!drawn) return;   // frame saltado: se queda la imagen anterior
	int opaque = clear_next != 0;
	while (DMA_CR(GX_DMA) & DMA_BUSY) { }   // la lista anterior aún va camino del FIFO
	GFX_CLEAR_COLOR = opaque ? clear_next : 63u << 24;
	clear_next = 0;
	STAGE(11);
	while (commit_on) { }   // lo del frame anterior aún no ha llegado a la VRAM (dos frames en una vblank)
	list[ln++] = 0x50;   // SWAP_BUFFERS (orden manual de los semitransparentes): el buffer cambia en la próxima vblank
	list[ln++] = 1;
	DC_FlushRange(list, (u32)ln * 4);   // la DMA lee la RAM, no la caché
	DMA_SRC(GX_DMA) = (u32)list;
	DMA_DEST(GX_DMA) = 0x04000400;
	DMA_CR(GX_DMA) = DMA_FIFO | (u32)ln;
	list_cur ^= 1;
	list = list_buf[list_cur];
	STAGE(12);
	if (sh_tex) {   // lo cambiado para este frame va a la VRAM en esa vblank (gx_vblank)
		int any = 0;
		for (int k = 0; k < GX_NR; k++) {
			com_lo[k] = pend_lo[k]; com_hi[k] = pend_hi[k];
			pend_lo[k] = pend_hi[k] = 0;
			if (com_lo[k] < com_hi[k]) {
				any = 1;
				DC_FlushRange(k == GX_PAL ? (u8 *)sh_pal + com_lo[k] : (u8 *)sh_tex + ((u32)k << 15) + com_lo[k], com_hi[k] - com_lo[k]);
			}
		}
		commit_on = any;
	}
	ln = npolys = 0;
	frame_no++;
	STAGE(13);
	gx_bg_hidden = opaque;
}

const GpuHw gx_hw = { gx_poly, gx_vram_written, gx_flip, gx_clear };

void gx_init(void)
{
	glInit();
	videoSetMode(MODE_5_3D | DISPLAY_BG3_ACTIVE);   // el BG3 (VRAM de la PS1: vídeos, fondos) sigue detrás del 3D
	REG_BG0CNT = BG_PRIORITY(0);
	REG_BG3CNT = (REG_BG3CNT & ~3) | BG_PRIORITY(1);
	vramSetBankB(VRAM_B_TEXTURE_SLOT0);
	vramSetBankD(VRAM_D_TEXTURE_SLOT1);
	vramSetBankC(VRAM_C_TEXTURE_SLOT2);
	vramSetBankE(VRAM_E_TEX_PALETTE);
	glEnable(GL_TEXTURE_2D | GL_BLEND);
	GFX_CLEAR_COLOR = 63u << 24;   // negro con alfa 0 (se ve el fondo) e ID 63
	GFX_CLEAR_DEPTH = 0x7fff;
	glViewport(0, 0, 255, 191);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG0 | BLEND_DST_BG3 | BLEND_DST_BACKDROP;
	if (!sh_tex) {
#ifdef BENCH_VRAM_PAK
		sh_tex = (u16 *)0x09200000;   // banco de pruebas en modo DS: en el Expansion Pak, sin quitar RAM a la caché de código
		sh_pal = (u16 *)(0x09200000 + GX_PAL * 0x8000);
#else
		sh_tex = malloc(GX_PAL * 0x8000);
		sh_pal = malloc(0x10000);
#endif
		if (!sh_tex || !sh_pal) { free(sh_tex); free(sh_pal); sh_tex = sh_pal = NULL; }
	}
	for (int m = 1; m < 256; m++) inv255[m] = ((255u << 16) + (u32)m - 1) / (u32)m;
	for (int c = 0; c < 256; c++) mod5t[c] = (u8)mod5(c);
	tex_reset();
	pal_reset();
	ramp_init();
	ln = npolys = 0;
	GFX_FLUSH = 1;
}
