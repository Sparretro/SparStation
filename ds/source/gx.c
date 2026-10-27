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
#include "psx.h"
#include "gx.h"

#ifdef PSX_SELFTEST
#define MAX_POLYS 1200   // en el modo DS la RAM va justa
#else
#define MAX_POLYS 2000   // el hardware admite 2048 por frame
#endif
#define LIST_WORDS (MAX_POLYS * 26 + 16)

static u32 list[LIST_WORDS];
static int ln, npolys;
static u32 frame_no;
int gx_display_dirty = 1;   // la zona mostrada de la VRAM cambió: hay que volver a copiarla al fondo
int gx_bg_hidden;           // el 3D tapa toda la pantalla: no hace falta copiar el fondo

// ---- paletas: banco E (64 KB) ----
#define PAL_HASH 512
typedef struct { s16 x, y; u16 n, off; u8 lum; } PalEnt;   // off en unidades de 16 bytes; n = 0: libre; lum = brillo medio 0-31
static PalEnt pal[PAL_HASH];
static u32 pal_top;   // bytes usados

static u16 *bank_lcd(int bank)   // dirección de un banco cuando está en modo LCD
{
	static u16 *const addr[4] = { (u16 *)0x06820000, (u16 *)0x06860000, (u16 *)0x06840000, (u16 *)0x06880000 };   // B, D, C, E
	return addr[bank];
}

static void pal_reset(void)
{
	memset(pal, 0, sizeof pal);
	pal_top = 16;   // los primeros 16 bytes: paleta blanca de la rampa (ver ramp_init)
}

// Base de paleta (para PLTT_BASE) con los n colores de la CLUT en (cx, cy). n = 32: los 16 colores
// llevados a brillo máximo (para A3I5, donde el brillo va en el alfa de cada texel) y 16 negros.
static u32 pal_get(int cx, int cy, int n, int *lum)
{
	u32 h = ((u32)cy * 64 + (u32)cx / 16 + (n == 256 ? 0x9e37 : n == 32 ? 0x4f1b : 0)) & (PAL_HASH - 1);
	PalEnt *e = &pal[h];
	u16 *dst;
	u32 sum = 0;
	if (e->n == n && e->x == cx && e->y == cy) { *lum = e->lum; return e->off; }
	if (pal_top + (u32)n * 2 > 0x10000) pal_reset();
	if (n == 256) pal_top = (pal_top + 511) & ~511u;
	e = &pal[h];
	e->x = (s16)cx; e->y = (s16)cy; e->n = (u16)n; e->off = (u16)(pal_top >> 4);
	vramSetBankE(VRAM_E_LCD);
	dst = bank_lcd(3) + pal_top / 2;
	for (int i = 0; i < n; i++) {
		u16 c = i < (n == 32 ? 16 : n) ? psx_vram[cy * 1024 + ((cx + i) & 1023)] : 0;
		int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31, m = r > g ? (r > b ? r : b) : (g > b ? g : b);
		if (n == 32 && m) { r = r * 31 / m; g = g * 31 / m; b = b * 31 / m; }
		dst[i] = (u16)(r | (g << 5) | (b << 10));
		if (c && (u32)m > sum) sum = (u32)m;   // brillo máximo de la CLUT
	}
	vramSetBankE(VRAM_E_TEX_PALETTE);
	pal_top += (u32)n * 2;
	e->lum = (u8)sum;
	*lum = e->lum;
	return e->off;
}

static void pal_dirty(int x, int y, int w, int h)
{
	for (int i = 0; i < PAL_HASH; i++) {
		PalEnt *e = &pal[i];
		if (e->n && e->y >= y && e->y < y + h && e->x < x + w && e->x + e->n > x) e->n = 0;
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
	vramSetBankB(VRAM_B_LCD);
	vramSetBankD(VRAM_D_LCD);
	vramSetBankC(VRAM_C_LCD);
	for (int row = row0; row < row1; row++) {
		u32 byte = (u32)row * (u32)rb;   // posición en la textura
		int unit = u + (int)(byte >> 15);
		u16 *dst = bank_lcd(unit >> 2) + ((unit & 3) << 14) + ((byte & 0x7fff) >> 1);
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
	vramSetBankB(VRAM_B_TEXTURE_SLOT0);
	vramSetBankD(VRAM_D_TEXTURE_SLOT1);
	vramSetBankC(VRAM_C_TEXTURE_SLOT2);
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

static void gx_poly(const GpuHwVtx *v, int n, const GpuHwPoly *p)
{
	static const u8 alpha_of[5] = { 31, 16, 16, 14, 8 };   // opaco, B/2+F/2, B+F, B-F, B+F/4 (aproximados)
	static const int order[4] = { 0, 1, 3, 2 };            // cuadrado de la PS1 -> perímetro de la DS
	int w = gpu_disp.width, h = gpu_disp.height;
	s32 kx = (8192 << 8) / w, ky = (8192 << 8) / h;
	u32 attr, teximg = 0, pltt = 0, *o;
	s32 z;
	int lum = 24, alpha, white = 0, black = 0;
	// B+F, B-F y B+F/4 no existen en la DS: B+F ~ «pantalla» (B*(1-a) + C*a con a = brillo de F), B-F ~
	// negro con a = brillo de F. Con textura de 4 bits, el brillo de cada texel va en su alfa (A3I5) y el
	// del color de los vértices en el alfa del polígono.
	int addlike = p->semi >= 2, a3 = 0, vb = 0, ramp = 0;   // a3 (A3I5 por página) desactivado: llenaba la caché
	if (npolys >= MAX_POLYS) return;
	// Con el bit de máscara (comprobar y poner) la PS1 solo pinta sobre lo que no lo tiene: en Silent Hill son
	// velos de niebla que tiñen solo al personaje (el fondo y la niebla ya tienen la marca). La DS no puede
	// limitarlos así y saldrían como un recuadro: mejor no dibujarlos.
	if (p->mask && p->semi) return;
	if (addlike) {
		for (int k = 0; k < n; k++) vb += (v[k].r > v[k].g ? (v[k].r > v[k].b ? v[k].r : v[k].b) : (v[k].g > v[k].b ? v[k].g : v[k].b));
		vb = p->raw && p->tex ? 128 : vb / n;
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
	z = 4000 - npolys * 3;
	for (int k = 0; k < n; k++) {
		const GpuHwVtx *q = &v[n == 4 ? order[k] : k];
		s32 x = (((s32)(q->x - p->ox) * kx) >> 8) - 4096, y = 4096 - (((s32)(q->y - p->oy) * ky) >> 8);
		u32 col;
		if (x < -32768) x = -32768; else if (x > 32767) x = 32767;
		if (y < -32768) y = -32768; else if (y > 32767) y = 32767;
		if (black) col = 0;
		else if (white && p->tex) col = 0x7fff;   // el brillo ya va en el alfa
		else if (white) { int m = q->r > q->g ? (q->r > q->b ? q->r : q->b) : (q->g > q->b ? q->g : q->b); m = m ? m : 1; col = rgb5(q->r * 255 / m, q->g * 255 / m, q->b * 255 / m); }
		else if (!p->tex) col = rgb5(q->r, q->g, q->b);
		else if (p->raw) col = 0x7fff;
		else col = (u32)mod5(q->r) | ((u32)mod5(q->g) << 5) | ((u32)mod5(q->b) << 10);
		*o++ = 0x20 | (0x22 << 8) | (0x23 << 16);   // COLOR, TEXCOORD, VTX_16
		*o++ = col;
		if (ramp) {
			int m = q->r > q->g ? (q->r > q->b ? q->r : q->b) : (q->g > q->b ? q->g : q->b);
			m >>= p->semi == 4 ? 5 : 3;
			if (m < 1) m = 1;   // con alfa 0 la DS no pinta y no marcaría el píxel (ver p->mask)
			if (m > 31) m = 31;
			*o++ = (8u << 16) | (u32)(m * 16 + 8);
		} else
			*o++ = ((u32)(q->v << 4) << 16) | ((u32)(q->u << 4) & 0xffff);
		*o++ = ((u32)y << 16) | ((u32)x & 0xffff);
		*o++ = (u32)z & 0xffff;
	}
	ln = (int)(o - list);
	npolys++;
}

static void gx_vram_written(int x, int y, int w, int h)
{
	tex_dirty(x, y, w, h);
	pal_dirty(x, y, w, h);
	if (x < gpu_disp.disp_x + gpu_disp.width && x + w > gpu_disp.disp_x) gx_display_dirty = 1;   // basta con mirar columnas
}

static u32 clear_next;   // color de fondo del 3D para el frame en curso (0 = transparente: se ve el fondo BG3)

static void gx_clear(u16 c)
{
	clear_next = (c & 0x7fffu) | (31u << 16) | (63u << 24);   // opaco: tapa el BG3, que ya no se borra
}

static void gx_flip(int drawn)
{
	if (!drawn) return;   // frame saltado: se queda la imagen anterior
	int opaque = clear_next != 0;
	GFX_CLEAR_COLOR = opaque ? clear_next : 63u << 24;
	clear_next = 0;
	for (int i = 0; i < ln; i++) GFX_FIFO = list[i];
	GFX_FLUSH = 1;   // orden manual de los semitransparentes; se cambia de buffer en la próxima vblank
	ln = npolys = 0;
	frame_no++;
	if (!opaque) gx_display_dirty = 1;   // el fondo pasa a mostrar el otro buffer (si el 3D no lo tapa entero)
	gx_bg_hidden = opaque;
}

const GpuHw gx_hw = { gx_poly, gx_vram_written, gx_flip, gx_clear };

void gx_init(void)
{
	glInit();
	videoSetMode(MODE_5_3D);
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
	tex_reset();
	pal_reset();
	ramp_init();
	ln = npolys = 0;
	GFX_FLUSH = 1;
}
