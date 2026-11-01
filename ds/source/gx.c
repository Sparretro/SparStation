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

#ifdef GX_HASH
// Prueba: huella de lo que se manda al 3D (cabecera y vértices de cada polígono), la misma por gx_poly que por
// gx_poly_raw aunque la lista quede distinta. main.c la deja en gx_dbg.dyn en el frame GX_HASH de la PS1;
// con -DGX_NO_RAW todo va por el camino viejo: si las dos huellas coinciden, el 3D recibe lo mismo.
u32 gx_hash = 2166136261u;
#define GXH(v) (gx_hash = (gx_hash ^ (u32)(v)) * 16777619u)
#else
#define GXH(v) ((void)0)
#endif

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
static int ln, npolys, nverts, semi_id;   // el hardware admite 2048 polígonos y 6144 vértices por frame
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
// Copia aparte de lo que espera a la vblank: así el frame siguiente puede escribir en la sombra sin esperar a que
// llegue (antes sh_mark se quedaba parado hasta la vblank si tocaba esa zona: un 14 % del tiempo en Pepsiman)
#define STG_BYTES 0x18000
static u8 *stg;
static u32 com_stg[GX_NR];   // posición en stg de lo que espera de cada hueco
static u32 stg_used;
static volatile u32 staged; // bit k: lo que espera del hueco k está en stg (si no cabe, se espera como antes)

// Se va a escribir [lo, hi) del hueco k (o de las paletas): si eso aún tiene que llegar a la VRAM para el
// frame que se acaba de mandar, esperar a que llegue; luego se apunta como cambiado.
#ifdef GX_TEXLOG
struct { char magic[8]; u32 n[16 + 64]; } gx_waits = { "SPGXWAIT" };   // n[16 + combinación]: polígonos de gx_poly_raw por tipo
#endif
static void sh_mark(int k, u32 lo, u32 hi)
{
#ifdef GX_TEXLOG
	if (commit_on && !(staged >> k & 1) && com_lo[k] < hi && lo < com_hi[k]) gx_waits.n[k]++;
#endif
	if (commit_on && !(staged >> k & 1) && com_lo[k] < hi && lo < com_hi[k]) {
		// se va a pisar algo que aún tiene que subir: se copia antes ese hueco a stg (solo cuando pasa; si no
		// cabe, se espera a la vblank). Si la vblank llega durante la copia, sube la sombra, que aún no ha cambiado.
		u32 clo = com_lo[k] & ~3u, chi = (com_hi[k] + 3) & ~3u;
		if (stg && stg_used + (chi - clo) <= STG_BYTES) {
			const u8 *src = k == GX_PAL ? (u8 *)sh_pal + clo : (u8 *)sh_tex + ((u32)k << 15) + clo;
			memcpy(stg + stg_used, src, chi - clo);
			DC_FlushRange(stg + stg_used, chi - clo);
			com_stg[k] = stg_used;
			stg_used += chi - clo;
			staged |= 1u << k;
		} else while (commit_on) { }
	}
	if (pend_lo[k] >= pend_hi[k]) { pend_lo[k] = lo; pend_hi[k] = hi; }
	else { if (lo < pend_lo[k]) pend_lo[k] = lo; if (hi > pend_hi[k]) pend_hi[k] = hi; }
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
		src = staged >> k & 1 ? stg + com_stg[k] : k == GX_PAL ? (const u8 *)sh_pal + lo : (const u8 *)sh_tex + ((u32)k << 15) + lo;
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

// Textura del último polígono de gx_poly_raw: si el siguiente usa la misma página, paleta y filas ya cargadas (lo
// normal en Tomb Raider), no se busca en las cachés. Se anula si cambia la VRAM, si una banda o una paleta nueva
// pudo echar la suya, y en cada imagen nueva (la primera vez pasa por tex_get/pal_get, que apuntan el uso).
static struct { s16 tpx, tpy, cx, cy, v0, v1; u8 td, ok; u32 teximg, pltt, frame; int lum, vbase; } tc;
#ifdef GX_NO_TC
#define TC_ON 0   // medida: sin esta caché
#else
#define TC_ON 1
#endif

static void pal_reset(void)
{
	tc.ok = 0;
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
PSX_ITCM static u32 pal_get(int cx, int cy, int n, int *lum)
{
	STAGE(30);
	u32 h = ((u32)cy * 64 + (u32)cx / 16 + (n == 256 ? 0x9e37 : n == 32 ? 0x4f1b : 0)) & (PAL_HASH - 1), off;
	PalEnt *e = &pal[h];
	u16 *dst;
	u32 sum = 0, now = frame_no + 1;
	int small = n == 16, slot = 0;
	if (e->n == n && e->x == cx && e->y == cy) { pal_mark_used(e); *lum = e->lum; return pal_off(e) >> 4; }
	tc.ok = 0;   // una paleta nueva puede quedarse el hueco de la del último polígono
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
	tc.ok = 0;
	for (int i = 0; i < PAL_HASH; i++) {
		PalEnt *e = &pal[i];
		if (e->n && e->y >= y && e->y < y + h && e->x < x + w && e->x + e->n > x) {
			if (e->n == 16) p16_owner[e->slot] = -1; else p256_owner[e->slot] = -1;
			e->n = 0;
		}
	}
}

// ---- texturas: 384 huecos de 1 KB (banco B = huecos 0-127, D = 128-255, C = 256-383; el 352 es de la rampa) ----
// Cada entrada es una banda de 16 a 256 filas (potencia de 2) de una página de la PS1, desde la fila del polígono
// (la DS admite cualquier dirección): casi todos usan un trozo de su página, y así caben muchas más (Pepsiman usa
// hasta unas 20 páginas por frame y con páginas enteras de 32 KB se echaban unas a otras sin parar).
// depth 1/2/3 = página de 4/8/15 bits tal cual; 4 = página de 4 bits como A3I5 (8 bits por texel con alfa
// por texel sacado del brillo de su color en la CLUT cx, cy: para las capas aditivas)
#define NSUB 384
#define SUB_BYTES 1024
#define RAMP_SUB (0x58000 / SUB_BYTES)   // la rampa está en el byte 0x58000
typedef struct { s16 x, y, cx, cy; u8 depth, units, used, start; u16 h; u8 hl; s16 next; u32 stamp, rows[8]; } TexEnt;   // used = 0: libre; rows: filas ya copiadas (bit = fila de la página); next: siguiente de su página
static TexEnt tex[NSUB];     // la entrada i ocupa los huecos i .. i+units-1 (alineada a su tamaño)
static s16 sub_owner[NSUB];   // entrada que ocupa cada hueco (-1: ninguna)
static s16 page_head[128];    // primera entrada de cada página (32 páginas x 4 profundidades): buscar sin recorrer las 192

static int page_key(int x, int y, int depth) { return ((y >> 8) & 1) << 4 | ((x >> 6) & 15) | ((depth - 1) & 3) << 5; }

static void tex_reset(void)
{
	tc.ok = 0;
	memset(tex, 0, sizeof tex);
	memset(sub_owner, -1, sizeof sub_owner);
	memset(page_head, -1, sizeof page_head);
}

static void tex_unlink(int k)   // quita la entrada k de la lista de su página
{
	s16 *pp = &page_head[page_key(tex[k].x, tex[k].y, tex[k].depth)];
	while (*pp >= 0 && *pp != k) pp = &tex[*pp].next;
	if (*pp == k) *pp = tex[k].next;
}

static int src_w(int depth) { return depth == 1 || depth == 4 ? 64 : depth == 2 ? 128 : 256; }   // medias palabras por fila
static int dst_row(int depth) { return depth == 1 ? 128 : depth == 3 ? 512 : 256; }              // bytes por fila

static void tex_dirty(int x, int y, int w, int h)
{
	tc.ok = 0;
	for (int i = 0; i < NSUB; i++) {
		TexEnt *t = &tex[i];
		if (!t->used) continue;
		if (t->x < x + w && t->x + src_w(t->depth) > x && t->y < y + h && t->y + 256 > y) {   // solo esas filas
			int r0 = y - t->y, r1 = y + h - t->y;
			if (r0 < 0) r0 = 0;
			if (r1 > 256) r1 = 256;
			for (int r = r0; r < r1; r++) t->rows[r >> 5] &= ~(1u << (r & 31));   // hay que volver a copiarlas
		}
	}
}

// Bytes [b0, b1) de la memoria de texturas: se apuntan como cambiados en los trozos de 32 KB que tocan
static void sh_mark_bytes(u32 b0, u32 b1)
{
	for (u32 k = b0 >> 15; k <= (b1 - 1) >> 15; k++) {
		u32 base = k << 15, lo = b0 > base ? b0 - base : 0, hi = b1 < base + 0x8000 ? b1 - base : 0x8000;
		sh_mark((int)k, lo, hi);
	}
}

// Copia las filas [row0, row1) de la página (dentro de la banda de la entrada) a su sitio desde el hueco u
static void tex_upload(const TexEnt *t, int u, int row0, int row1)
{
	STAGE(20);
	int tw = src_w(t->depth), rb = dst_row(t->depth);
	u32 base = (u32)u * SUB_BYTES;
	u16 lut[256];   // A3I5: un byte de origen (2 texels) -> 2 bytes
	STAT_ADD(gpu_pixels, (u32)(row1 - row0) * (u32)rb);   // diagnóstico: bytes de textura copiados
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
	if (sh_tex) sh_mark_bytes(base + (u32)(row0 - t->start) * (u32)rb, base + (u32)(row1 - t->start) * (u32)rb);
	else { vramSetBankB(VRAM_B_LCD); vramSetBankD(VRAM_D_LCD); vramSetBankC(VRAM_C_LCD); }
	for (int row = row0; row < row1; row++) {
		u32 byte = base + (u32)(row - t->start) * (u32)rb;   // posición en la memoria de texturas
		u16 *dst = sh_tex ? sh_tex + (byte >> 1) : bank_lcd((int)(byte >> 17)) + ((byte & 0x1ffff) >> 1);
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

// Filas [v0, v1] de la página que aún no se han copiado: se copian ahora (por tramos seguidos)
PSX_ITCM static void tex_ensure(TexEnt *t, int u, int v0, int v1)
{
	int w0 = v0 >> 5, w1 = v1 >> 5, missing = 0;
	for (int w = w0; w <= w1; w++) {   // lo normal: ya están todas (por palabras de 32 filas)
		u32 m = ~0u;
		if (w == w0) m &= ~0u << (v0 & 31);
		if (w == w1) m &= ~0u >> (31 - (v1 & 31));
		if ((t->rows[w] & m) != m) { missing = 1; break; }
	}
	if (!missing) return;
	for (int r = v0; r <= v1;) {
		int e;
		if (!(r & 31) && t->rows[r >> 5] == ~0u) { r += 32; continue; }
		if (t->rows[r >> 5] & (1u << (r & 31))) { r++; continue; }
		for (e = r; e <= v1 && !(t->rows[e >> 5] & (1u << (e & 31))); e++) t->rows[e >> 5] |= 1u << (e & 31);
		tex_upload(t, u, r, e);
		r = e;
	}
}

static int tex_has(const TexEnt *t, int x, int y, int depth, int v0, int v1)
{
	return t->used && t->x == x && t->y == y && t->depth == depth && v0 >= t->start && v1 < t->start + t->h;
}

// Hueco donde está (o se carga) la banda con las filas [v0, v1] de la página; devuelve el primero. Solo se
// copian las filas que usa el polígono.
#ifdef GX_TEXLOG
struct { char magic[8]; u32 n, e[4096]; } gx_texlog = { "SPTEXLOG" };
#endif
static int tex_last;   // los polígonos seguidos suelen usar la misma página

// Banda nueva: fuera de la ITCM (unas 30 por segundo; buscar sitio ocupa mucho código)
static __attribute__((noinline)) int tex_new(int x, int y, int depth, int cx, int cy, int v0, int v1)
{
	int h, start, units, best = -1;
	u32 best_age = 0;
	// la banda justa (en huecos de 1 KB, desde 8 filas) que empieza a la altura del polígono y llega a sus filas, en
	// cualquier hueco: el 3D la ve con la altura potencia de 2 de encima (con repetición), pero los polígonos que la
	// usan nunca pasan de sus filas (tex_has). Antes la altura era potencia de 2 y la banda iba alineada a su tamaño:
	// una de 191 filas ocupaba 256 y en Pepsiman la memoria se llenaba y dos bandas se echaban una a otra en cada frame.
	tc.ok = 0;   // puede echar la banda del último polígono
	{
		int rb = dst_row(depth), per = SUB_BYTES / rb;   // filas por hueco (2, 4 u 8)
		start = v0 & ~3;
		h = v1 - start + 1;
		if (h < 8) h = 8;
		h = (h + per - 1) / per * per;
		if (start + h > 256) start = 256 - h;
		units = h * rb / SUB_BYTES;
	}
	// los huecos seguidos cuyo uso más reciente sea el más viejo (libre = viejísimo); los usados en este frame
	// solo si no queda otro (el 3D dibuja el frame entero al final: pisarlos estropearía sus polígonos). A igual
	// edad, el sitio que echa menos huecos ocupados.
	{
		u32 best_ev = 0;
		for (int i = 0; i + units <= NSUB; i++) {
			u32 newest = 0, age, ev = 0;
			int prev = -1;
			if (i <= RAMP_SUB && RAMP_SUB < i + units) continue;
			for (int s = i; s < i + units; s++) {
				int k = sub_owner[s];
				if (k < 0 || !tex[k].used || k == prev) continue;
				prev = k;
				ev += tex[k].units;
				if (tex[k].stamp + 1 > newest) newest = tex[k].stamp + 1;
			}
			age = frame_no + 1 - newest;   // 0: usado en este frame
			if (best < 0 || age > best_age || (age == best_age && ev < best_ev)) { best_age = age; best = i; best_ev = ev; }
			if (!ev) break;   // huecos libres: no hay nada mejor
		}
	}
#ifdef GX_TEXLOG
	if (gx_texlog.n < 4094) { gx_texlog.e[gx_texlog.n++] = (u32)x | (u32)y << 10 | (u32)depth << 19 | (u32)(best & 3) << 22 | (frame_no & 0xff) << 24; gx_texlog.e[gx_texlog.n++] = (u32)v0 | (u32)v1 << 8 | (u32)best << 16 | (u32)(h - 1) << 24; }
#endif
	for (int s = best; s < best + units; s++) {   // fuera lo que ocupaba esos huecos
		int k = sub_owner[s];
		if (k >= 0 && tex[k].used) {
			for (int q = k; q < k + tex[k].units; q++) sub_owner[q] = -1;
			tex_unlink(k);
			tex[k].used = 0;
		}
	}
	for (int s = best; s < best + units; s++) sub_owner[s] = (s16)best;
	{
		s16 *head = &page_head[page_key(x, y, depth)];
		tex[best].next = *head;
		*head = (s16)best;
	}
	tex[best].x = (s16)x; tex[best].y = (s16)y; tex[best].cx = (s16)cx; tex[best].cy = (s16)cy;
	tex[best].depth = (u8)depth; tex[best].units = (u8)units; tex[best].used = 1; tex[best].stamp = frame_no;
	tex[best].start = (u8)start; tex[best].h = (u16)h;
	for (tex[best].hl = 3; (1 << tex[best].hl) < h; tex[best].hl++) { }   // altura para el 3D: potencia de 2
	memset(tex[best].rows, 0, sizeof tex[best].rows);
	tex_ensure(&tex[best], best, v0, v1);
	STAT_ADD(tex_bands, 1);
	tex_last = best;
	return best;
}

PSX_ITCM static int tex_get(int x, int y, int depth, int cx, int cy, int v0, int v1)
{
	int last = tex_last;
	if (tex_has(&tex[last], x, y, depth, v0, v1)) {
		tex[last].stamp = frame_no;
		tex_ensure(&tex[last], last, v0, v1);
		return last;
	}
	for (int i = page_head[page_key(x, y, depth)]; i >= 0; i = tex[i].next)
		if (tex_has(&tex[i], x, y, depth, v0, v1)) {
			tex[i].stamp = frame_no;
			tex_ensure(&tex[i], i, v0, v1);
			tex_last = i;
			return i;
		}
	return tex_new(x, y, depth, cx, cy, v0, v1);
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
static s32 kx, ky;        // escala a coordenadas de la DS (4.12) por píxel, en 24.8: se recalcula si cambia la resolución

static inline void scale_update(void)
{
	static int last_w, last_h;
	int w = gpu_disp.width, h = gpu_disp.height;
	if (w != last_w || h != last_h) { kx = (8192 << 8) / w; ky = (8192 << 8) / h; last_w = w; last_h = h; }
}

// Cabecera (POLY_FORMAT, TEXIMAGE_PARAM, PLTT_BASE y BEGIN_VTXS) del bloque de vértices abierto en la lista: si el
// polígono siguiente lleva la misma, sus vértices siguen en ese bloque (el hardware hace un polígono de cada 3 o 4).
// blk_attr = ~0u: hay que empezar uno.
static u32 blk_attr = ~0u, blk_teximg, blk_pltt, blk_quad;

static void gx_poly(const GpuHwVtx *v, int n, const GpuHwPoly *p)
{
	STAGE(40);
	static const u8 alpha_of[5] = { 31, 16, 16, 14, 8 };   // opaco, B/2+F/2, B+F, B-F, B+F/4 (aproximados)
	static const int order[4] = { 0, 1, 3, 2 };            // cuadrado de la PS1 -> perímetro de la DS
	u32 attr, teximg = 0, pltt = 0, *o;
	u32 z;
	int lum = 24, alpha, white = 0, black = 0, vbase = 0;   // vbase: primera fila de la banda de textura
	// B+F, B-F y B+F/4 no existen en la DS: B+F ~ «pantalla» (B*(1-a) + C*a con a = brillo de F), B-F ~
	// negro con a = brillo de F. Con textura de 4 bits, el brillo de cada texel va en su alfa (A3I5) y el
	// del color de los vértices en el alfa del polígono.
	int addlike = p->semi >= 2, a3 = 0, vb = 0, ramp = 0;   // a3 (A3I5 por página) desactivado: llenaba la caché
	if (npolys >= MAX_POLYS || nverts + n > 6144) return;
	scale_update();
	// Con el bit de máscara (comprobar y poner) la PS1 solo pinta sobre lo que no lo tiene: en Silent Hill son
	// velos de niebla que tiñen solo al personaje (el fondo y la niebla ya tienen la marca). La DS no puede
	// limitarlos así y saldrían como un recuadro: mejor no dibujarlos.
	if (p->mask && p->semi) return;
	if (addlike) {
		for (int k = 0; k < n; k++) vb += (v[k].r > v[k].g ? (v[k].r > v[k].b ? v[k].r : v[k].b) : (v[k].g > v[k].b ? v[k].g : v[k].b));
		vb = p->raw && p->tex ? 128 : n == 4 ? vb >> 2 : (vb * 21846) >> 16;   // vb / n sin dividir (n = 3 o 4)
	}
	if (p->tex) {
		int vmin = v[0].v, vmax = v[0].v, u, fmt;
		for (int k = 1; k < n; k++) { if (v[k].v < vmin) vmin = v[k].v; if (v[k].v > vmax) vmax = v[k].v; }
		if (vmin < 0 || vmax > 255) { vmin = 0; vmax = 255; }   // se sale de la página (repite): todas
		u = tex_get(p->tpx, p->tpy, a3 ? 4 : p->tex, p->cx, p->cy, vmin, vmax);
		fmt = a3 ? 1 : p->tex == 1 ? 3 : p->tex == 2 ? 4 : 7;
		int c0 = 0;
		if (p->tex < 3) {
			pltt = pal_get(p->cx, p->cy, a3 ? 32 : p->tex == 1 ? 16 : 256, &lum);
			c0 = psx_vram[p->cy * 1024 + p->cx] == 0;   // índice 0 transparente si su color es 0
		}
		vbase = tex[u].start;
		teximg = ((u32)u * (SUB_BYTES / 8)) | (1u << 16) | (1u << 17) | (5u << 20) | ((u32)(tex[u].hl - 3) << 23) | ((u32)fmt << 26) | ((u32)c0 << 29);
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
	if (p->semi) attr |= (u32)(p->mask ? 62 : (semi_id = semi_id >= 61 ? 1 : semi_id + 1)) << 24;
	o = &list[ln];
	*o++ = 0x29 | (0x2a << 8) | (0x2b << 16) | (0x40 << 24);   // POLY_FORMAT, TEXIMAGE_PARAM, PLTT_BASE, BEGIN_VTXS
	*o++ = attr;
	*o++ = teximg;
	*o++ = pltt;
	*o++ = n == 4 ? 1 : 0;
	blk_attr = ~0u;   // estos vértices van con VTX_16: el siguiente de gx_poly_raw empieza bloque propio
	z = (u32)(4000 - npolys * 3) & 0xffff;
	GXH(attr); GXH(teximg); GXH(pltt); GXH(n == 4); GXH(z);
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
				GXH(col);
			} else *o++ = 0x22 | (0x23 << 8);           // TEXCOORD, VTX_16 (el color sigue)
			if (ramp) {
				int m = q->r > q->g ? (q->r > q->b ? q->r : q->b) : (q->g > q->b ? q->g : q->b);
				m >>= p->semi == 4 ? 5 : 3;
				if (m < 1) m = 1;   // con alfa 0 la DS no pinta y no marcaría el píxel (ver p->mask)
				if (m > 31) m = 31;
				*o++ = (8u << 16) | (u32)(m * 16 + 8);
			} else
				*o++ = ((u32)((q->v - vbase) << 4) << 16) | ((u32)(q->u << 4) & 0xffff);
			GXH(o[-1]);
			*o++ = ((u32)y << 16) | ((u32)x & 0xffff);
			GXH(o[-1]);
			*o++ = z;
		}
	}
	ln = (int)(o - list);
	npolys++;
	nverts += n;
}

static inline int sx11(u32 v) { return ((s32)(v << 21)) >> 21; }

// Lo mismo que gx_poly, leyendo los vértices del paquete de la PS1 (gpu.c, hw_poly_packet) sin pasar por GpuHwVtx.
// Con la misma cabecera que el anterior sigue en su bloque; los vértices después del primero van con VTX_XY (la z
// es la del polígono entero). Mismos vértices, colores y profundidades que gx_poly: la imagen es la misma.
// En la ITCM: en la DSi real (perfil de Tomb Raider, v25) era el 7% del tiempo, repartido por toda la función
#ifdef GX_TEXLOG
static void gx_poly_raw(const u32 *w, const GpuHwRaw *r)   // con los contadores no cabe en la ITCM
#else
PSX_ITCM static void gx_poly_raw(const u32 *w, const GpuHwRaw *r)
#endif
{
	static const u8 alpha_of[5] = { 31, 16, 16, 14, 8 };
	static const u8 order[4] = { 0, 1, 3, 2 };
	const u32 *vp[4];   // xy de cada vértice; delante su color (gouraud, menos el 0) y detrás su uv (con textura)
	u32 cmd = r->cmd, attr, teximg = 0, pltt = 0, z, *o;
	int n = cmd & 8 ? 4 : 3, gour = (cmd & 0x10) != 0, textured = (cmd & 4) != 0, td = r->tex, semi = r->semi;
	int raw = textured && (cmd & 1), lum = 24, alpha, white = 0, black = 0, vbase = 0, addlike = semi >= 2, vb = 0, ramp = 0;
	STAGE(41);
	if (npolys >= MAX_POLYS || nverts + n > 6144) return;
	if (r->mask && semi) return;   // ver gx_poly
	scale_update();
	{
		const u32 *q = w + 1;
		for (int j = 0; j < n; j++) { q += j && gour; vp[j] = q; q += textured ? 2 : 1; }
	}
	if (addlike) {
		for (int j = 0; j < n; j++) {
			u32 c = j && gour ? vp[j][-1] : w[0];
			int cr = c & 0xff, cg = (c >> 8) & 0xff, cb = (c >> 16) & 0xff;
			vb += cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
		}
		vb = raw ? 128 : n == 4 ? vb >> 2 : (vb * 21846) >> 16;
	}
	if (td) {
		int vmin = (vp[0][1] >> 8) & 0xff, vmax = vmin, u, fmt, c0 = 0;
		for (int j = 1; j < n; j++) { int v = (vp[j][1] >> 8) & 0xff; if (v < vmin) vmin = v; if (v > vmax) vmax = v; }
		if (TC_ON && tc.ok && tc.frame == frame_no && tc.tpx == r->tpx && tc.tpy == r->tpy && tc.td == td && tc.cx == r->cx && tc.cy == r->cy &&
		    vmin >= tc.v0 && vmax <= tc.v1) {   // la misma textura que el anterior y filas ya cargadas
			teximg = tc.teximg;
			pltt = tc.pltt;
			lum = tc.lum;
			vbase = tc.vbase;
		} else {
			u = tex_get(r->tpx, r->tpy, td, r->cx, r->cy, vmin, vmax);
			fmt = td == 1 ? 3 : td == 2 ? 4 : 7;
			if (td < 3) {
				pltt = pal_get(r->cx, r->cy, td == 1 ? 16 : 256, &lum);
				c0 = psx_vram[r->cy * 1024 + r->cx] == 0;
			}
			vbase = tex[u].start;
			teximg = ((u32)u * (SUB_BYTES / 8)) | (1u << 16) | (1u << 17) | (5u << 20) | ((u32)(tex[u].hl - 3) << 23) | ((u32)fmt << 26) | ((u32)c0 << 29);
			tc.tpx = r->tpx; tc.tpy = r->tpy; tc.cx = r->cx; tc.cy = r->cy; tc.td = (u8)td;
			tc.v0 = (s16)vmin; tc.v1 = (s16)vmax;
			tc.teximg = teximg; tc.pltt = pltt; tc.lum = lum; tc.vbase = vbase;
			tc.frame = frame_no;
			tc.ok = 1;
		}
	}
	alpha = alpha_of[semi];
	if (addlike) {
		int f = !td ? vb >> 3 : lum * vb / 128;
		if (semi == 4) f >>= 2;
		alpha = f < 1 ? 1 : f > 30 ? 30 : f;
		if (!td) { ramp = 1; alpha = 31; teximg = RAMP_TEXIMG; }
		if (semi == 3) black = 1; else white = 1;
	}
	attr = (3u << 6) | (1u << 12) | (1u << 13) | ((u32)alpha << 16);
	if (semi) attr |= (u32)(r->mask ? 62 : (semi_id = semi_id >= 61 ? 1 : semi_id + 1)) << 24;
	o = &list[ln];
	if (attr != blk_attr || teximg != blk_teximg || pltt != blk_pltt || (u32)(n == 4) != blk_quad) {
		*o++ = 0x29 | (0x2a << 8) | (0x2b << 16) | (0x40 << 24);   // POLY_FORMAT, TEXIMAGE_PARAM, PLTT_BASE, BEGIN_VTXS
		*o++ = attr;
		*o++ = teximg;
		*o++ = pltt;
		*o++ = n == 4 ? 1 : 0;
		blk_attr = attr; blk_teximg = teximg; blk_pltt = pltt; blk_quad = n == 4;
	}
	z = (u32)(4000 - npolys * 3) & 0xffff;
	GXH(attr); GXH(teximg); GXH(pltt); GXH(n == 4); GXH(z);
	{
		enum { CM_CONST, CM_NORM, CM_RGB, CM_MOD } cm;
		u32 kcol = 0;
		int flat = !gour && !ramp;
		if (black) cm = CM_CONST;
		else if (white && td) { cm = CM_CONST; kcol = 0x7fff; }
		else if (white) cm = CM_NORM;
		else if (!td) cm = CM_RGB;
		else if (raw) { cm = CM_CONST; kcol = 0x7fff; }
		else cm = CM_MOD;
#ifdef GX_TEXLOG
		gx_waits.n[16 + (cm << 4 | flat << 3 | textured << 2 | ramp << 1 | (n == 4))]++;
#endif
		for (int k = 0; k < n; k++) {
			int j = n == 4 ? order[k] : k;
			u32 xy = vp[j][0], c = j && gour ? vp[j][-1] : w[0], vtx = k ? 0x25 : 0x23;   // VTX_16 el primero, VTX_XY los demás
			int qr = c & 0xff, qg = (c >> 8) & 0xff, qb = (c >> 16) & 0xff;
			s32 x = (((sx11(xy) + r->dx) * kx) >> 8) - 4096, y = 4096 - (((sx11(xy >> 16) + r->dy) * ky) >> 8);
			if ((u32)(x + 32768) > 0xffff) x = x < 0 ? -32768 : 32767;
			if ((u32)(y + 32768) > 0xffff) y = y < 0 ? -32768 : 32767;
			if (k == 0 || !flat) {
				u32 col;
				switch (cm) {
				case CM_CONST: col = kcol; break;
				case CM_NORM: {
					int m = qr > qg ? (qr > qb ? qr : qb) : (qg > qb ? qg : qb);
					u32 iv = inv255[m];
					col = rgb5((int)((qr * iv) >> 16), (int)((qg * iv) >> 16), (int)((qb * iv) >> 16));
					break;
				}
				case CM_RGB: col = rgb5(qr, qg, qb); break;
				default: col = mod5t[qr] | ((u32)mod5t[qg] << 5) | ((u32)mod5t[qb] << 10); break;
				}
				*o++ = 0x20 | (0x22 << 8) | (vtx << 16);   // COLOR, TEXCOORD, VTX
				*o++ = col;
				GXH(col);
			} else *o++ = 0x22 | (vtx << 8);           // TEXCOORD, VTX (el color sigue)
			if (ramp) {
				int m = qr > qg ? (qr > qb ? qr : qb) : (qg > qb ? qg : qb);
				m >>= semi == 4 ? 5 : 3;
				if (m < 1) m = 1;
				if (m > 31) m = 31;
				*o++ = (8u << 16) | (u32)(m * 16 + 8);
			} else if (textured) {
				u32 uv = vp[j][1];
				*o++ = ((u32)((int)((uv >> 8) & 0xff) - vbase) << 4 << 16) | (((uv & 0xff) << 4) & 0xffff);
			} else *o++ = 0;
			GXH(o[-1]);
			*o++ = ((u32)y << 16) | ((u32)x & 0xffff);
			GXH(o[-1]);
			if (!k) *o++ = z;
		}
	}
	ln = (int)(o - list);
	npolys++;
	nverts += n;
}

static void gx_vram_written(int x, int y, int w, int h)
{
	tex_dirty(x, y, w, h);
	pal_dirty(x, y, w, h);
	if (x < gpu_disp.disp_x + (gpu_disp.color24 ? gpu_disp.width * 3 / 2 : gpu_disp.width) && x + w > gpu_disp.disp_x)   // en 24 bits, 1,5 medias palabras por píxel
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
		}
		staged = 0;
		stg_used = 0;
		for (int k = 0; k < GX_NR; k++) {   // stg solo se llena si el frame siguiente pisa algo antes de la vblank (sh_mark)
			if (com_lo[k] < com_hi[k]) {
				u32 lo = com_lo[k] & ~3u, hi = (com_hi[k] + 3) & ~3u;
				any = 1;
				DC_FlushRange(k == GX_PAL ? (u8 *)sh_pal + lo : (u8 *)sh_tex + ((u32)k << 15) + lo, hi - lo);
			}
		}
		commit_on = any;
	}
	ln = npolys = nverts = 0;
	blk_attr = ~0u;   // lista nueva: sin bloque abierto
#ifdef GX_TEXLOG
	{   // marca de fin de frame: huecos ocupados por bandas usadas en este frame y ocupados en total
		u32 now = 0, occ = 0;
		for (int i = 0; i < NSUB; i++) if (tex[i].used) { occ += tex[i].units; if (tex[i].stamp == frame_no) now += tex[i].units; }
		if (gx_texlog.n < 4094) { gx_texlog.e[gx_texlog.n++] = 0xffffffffu; gx_texlog.e[gx_texlog.n++] = now | occ << 16; }
	}
#endif
	frame_no++;
	STAGE(13);
	gx_bg_hidden = opaque;
}

#ifdef GX_NO_RAW
const GpuHw gx_hw = { gx_poly, gx_vram_written, gx_flip, gx_clear, NULL };   // prueba: todo por gx_poly
#else
const GpuHw gx_hw = { gx_poly, gx_vram_written, gx_flip, gx_clear, gx_poly_raw };
#endif

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
		stg = (u8 *)(0x09200000 + GX_PAL * 0x8000 + 0x10000);
#else
		sh_tex = malloc(GX_PAL * 0x8000);
		sh_pal = malloc(0x10000);
		if (sh_tex && sh_pal) stg = malloc(STG_BYTES);   // sin ella, como antes
#endif
		if (!sh_tex || !sh_pal) { free(sh_tex); free(sh_pal); sh_tex = sh_pal = NULL; }
	}
	for (int m = 1; m < 256; m++) inv255[m] = ((255u << 16) + (u32)m - 1) / (u32)m;
	for (int c = 0; c < 256; c++) mod5t[c] = (u8)mod5(c);
	tex_reset();
	pal_reset();
	ramp_init();
	ln = npolys = nverts = 0;
	blk_attr = ~0u;
	GFX_FLUSH = 1;
}
