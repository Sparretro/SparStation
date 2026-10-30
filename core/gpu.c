// GPU de la PS1 por software: dibuja en la VRAM de 1024x512 a 15 bits igual que la máquina real
// (texturas afines, gouraud, semitransparencias y bit de máscara).
#include "psx.h"

GpuDisplay gpu_disp;
const GpuHw *gpu_hw;
u8 gpu_frameskip, gpu_rows_on, gpu_row_draw[512];
static int skip_frame, flip_n, vbl_since_flip;   // salto de frames: se cuenta por cambios del buffer mostrado
static int prev_disp_y, prev_disp_x;   // origen del otro buffer (el juego alterna entre dos)
static int need_rows, rows_mode, rows_lines;   // hay que recalcular gpu_row_draw (cambió el buffer o la resolución)
static void rows_update(void);

static struct {
	// E1: página de textura y modo
	int tp_x, tp_y, semi, depth, dither, draw_display, tex_disable, flip_x, flip_y;
	// E2..E6
	int tw_mx, tw_my, tw_ox, tw_oy;
	int da_x1, da_y1, da_x2, da_y2, off_x, off_y;
	int set_mask, check_mask;
	// GP1
	int disp_disable, dma_dir, irq, hres1, hres2, vres, pal, color24, interlace, reverse;
	int disp_x, disp_y, h1, h2, v1, v2;
	int odd_line, field;
	int allow_tex_disable;
	u32 info;
} g;

static u32 fifo[16];
static int fifo_n, fifo_need;
static enum { M_CMD, M_LOAD, M_POLYLINE } mode;
static int load_x, load_y, load_w, load_h, load_i, load_total;
static int store_x, store_y, store_w, store_h, store_i, store_total;
static u32 gpuread;
#ifdef PSX_GPU_REF
static unsigned ref_frames;   // para PSX_GPU_CHECK (ver pc/gpu_ref.inc)
#endif

// Los bucles de relleno por software iban en la ITCM; con la GPU 3D de la DS casi no se usan y ese sitio lo
// aprovecha la GTE. Con -DGPU_SW_ITCM vuelven a la ITCM.
#ifdef GPU_SW_ITCM
#define GPU_ITCM PSX_ITCM
#else
#define GPU_ITCM __attribute__((noinline))
#endif

#define VRAM(x, y) psx_vram[((y) & 511) * 1024 + ((x) & 1023)]

static inline int sx11(u32 v) { return ((s32)(v << 21)) >> 21; }

static void update_display(void)
{
	static const int widths[4] = { 256, 320, 512, 640 };
	gpu_disp.disp_x = g.disp_x;
	gpu_disp.disp_y = g.disp_y;
	gpu_disp.width = g.hres2 ? 368 : widths[g.hres1];
	gpu_disp.height = (g.vres && g.interlace) ? 480 : 240;
	gpu_disp.color24 = g.color24;
	gpu_disp.disabled = g.disp_disable;
	gpu_disp.interlace = g.interlace;
	gpu_disp.pal = g.pal;
}

void gpu_reset(void)
{
	memset(&g, 0, sizeof g);
	g.disp_disable = 1;
	g.h1 = 0x200;
	g.h2 = 0xc00;
	g.v1 = 0x10;
	g.v2 = 0x100;
	fifo_n = 0;
	mode = M_CMD;
	store_total = store_i = 0;
	update_display();
}

u32 gpu_stat(void)
{
	u32 s = g.tp_x | (g.tp_y << 4) | (g.semi << 5) | (g.depth << 7) | (g.dither << 9) | (g.draw_display << 10) |
	        (g.set_mask << 11) | (g.check_mask << 12) | ((g.interlace ? !g.field : 1) << 13) | (g.reverse << 14) | (g.tex_disable << 15) |
	        (g.hres2 << 16) | (g.hres1 << 17) | (g.vres << 19) | (g.pal << 20) | (g.color24 << 21) |
	        (g.interlace << 22) | (g.disp_disable << 23) | (g.irq << 24) | (g.dma_dir << 29) | ((u32)g.odd_line << 31);
	s |= 1 << 26;                          // listo para comandos
	s |= (store_i < store_total) << 27;    // listo para mandar VRAM a la CPU
	s |= 1 << 28;                          // listo para bloques DMA
	if (g.dma_dir == 1 || g.dma_dir == 2) s |= 1 << 25;
	else if (g.dma_dir == 3) s |= ((store_i < store_total) ? 1 : 0) << 25;
	return s;
}

// GPUSTAT.31: línea par/impar que se está mostrando; en 480i es el campo. Durante la vblank vale 0.
void gpu_set_line(int line)
{
	if (line >= PSX_VBLANK_LINE) g.odd_line = 0;
	else if (g.interlace && g.vres) g.odd_line = g.field;
	else g.odd_line = line & 1;
}

static void pend_keep(void);

void gpu_vblank(void)
{
#ifdef PSX_GPU_REF
	ref_frames++;
#endif
	if (g.interlace) g.field ^= 1; else g.field = 1;
	gpu_disp.frame_done = 1;
	if (++vbl_since_flip > 8) {   // el juego no cambia de buffer (o se ha parado): dibujar siempre
		skip_frame = 0;
		gpu_disp.stale = 0;
		if (gpu_hw) pend_keep();   // un borrado pendiente sigue tapando con el fondo del 3D
		if (gpu_hw) gpu_hw->flip(1);
	}
}

// ---- dibujo de píxeles ----
static inline u16 blend(u16 b, u16 f, int semi)
{
	int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
	int fr = f & 31, fg = (f >> 5) & 31, fb = (f >> 10) & 31;
	int r, gg, bl;
	switch (semi) {
	case 0: r = (br + fr) >> 1; gg = (bg + fg) >> 1; bl = (bb + fb) >> 1; break;
	case 1: r = br + fr; gg = bg + fg; bl = bb + fb; break;
	case 2: r = br - fr; gg = bg - fg; bl = bb - fb; break;
	default: r = br + (fr >> 2); gg = bg + (fg >> 2); bl = bb + (fb >> 2); break;
	}
	if (r > 31) r = 31; if (r < 0) r = 0;
	if (gg > 31) gg = 31; if (gg < 0) gg = 0;
	if (bl > 31) bl = 31; if (bl < 0) bl = 0;
	return (u16)(r | (gg << 5) | (bl << 10));
}

static inline void put(int x, int y, u16 c, int semi_on, int semi)
{
	u16 *p = &psx_vram[y * 1024 + x];
	if (g.check_mask && (*p & 0x8000)) return;
	if (semi_on) c = (c & 0x8000) | blend(*p, c, semi);
	*p = c | (g.set_mask << 15);
}

static inline u16 rgb15(u32 c) { return (u16)(((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10)); }

// Textura activa del primitivo que se está dibujando
static int t_px, t_py, t_depth, t_cx, t_cy;

static inline u16 texel(int u, int v)
{
	u = ((u & 0xff) & ~(g.tw_mx * 8)) | ((g.tw_ox & g.tw_mx) * 8);
	v = ((v & 0xff) & ~(g.tw_my * 8)) | ((g.tw_oy & g.tw_my) * 8);
	switch (t_depth) {
	case 0: {
		u16 w = VRAM(t_px + (u >> 2), t_py + v);
		return VRAM(t_cx + ((w >> ((u & 3) * 4)) & 0xf), t_cy);
	}
	case 1: {
		u16 w = VRAM(t_px + (u >> 1), t_py + v);
		return VRAM(t_cx + ((w >> ((u & 1) * 8)) & 0xff), t_cy);
	}
	default: return VRAM(t_px + u, t_py + v);
	}
}

static inline u16 modulate(u16 t, int r, int gg, int b)
{
	int tr = ((t & 31) * r) >> 7, tg = (((t >> 5) & 31) * gg) >> 7, tb = (((t >> 10) & 31) * b) >> 7;
	if (tr > 31) tr = 31;
	if (tg > 31) tg = 31;
	if (tb > 31) tb = 31;
	return (u16)(tr | (tg << 5) | (tb << 10) | (t & 0x8000));
}

static void set_texpage(u32 tp)
{
	g.tp_x = tp & 0xf;
	g.tp_y = (tp >> 4) & 1;
	g.semi = (tp >> 5) & 3;
	g.depth = (tp >> 7) & 3;
	g.tex_disable = g.allow_tex_disable ? (tp >> 11) & 1 : 0;
}

static void use_texture(u32 clut)
{
	t_px = g.tp_x * 64;
	t_py = g.tp_y * 256;
	t_depth = g.depth;
	t_cx = (clut & 0x3f) * 16;
	t_cy = (clut >> 6) & 0x1ff;
}

// ---- relleno rápido de tramos horizontales ----
// Un bucle especializado por combinación de textura (no/4/8/15 bits), gouraud, modulación y
// semitransparencia; lo que cambia poco (máscara, modo de mezcla, ventana) se mira dentro.
static struct {
	const u16 *clut;
	const u16 *page;              // esquina de la página de textura en la VRAM (bucles rápidos)
	int tx, ty, mu, ou, mv, ov;   // página de textura y ventana ya resuelta
	int semi, check;              // modo de mezcla; respetar el bit de máscara
	u16 set, flat;                // bit 15 que se escribe; color plano
	s32 du, dv, dr, dg, db;       // incrementos por píxel en 16.16
	u8 lr[32], lg[32], lb[32];    // modulación plana por tablas
} rs;
static u16 clut_copy[256];
static u32 lut_rgb = ~0u;

// Mezcla de la PS1; las dos más usadas, sin separar canales
static inline u16 blend_fast(u16 b, u16 f, int semi)
{
	if (semi == 0) {   // (B + F) / 2 en cada canal
		b &= 0x7fff;
		f &= 0x7fff;
		return (u16)((b & f) + (((b ^ f) & 0x7bde) >> 1));
	}
	if (semi == 1) {   // B + F con saturación en cada canal
		u32 rb = (b & 0x7c1f) + (f & 0x7c1f), gg = (b & 0x03e0) + (f & 0x03e0);   // R y B no se pisan; G aparte
		u32 orb = rb & 0x8020, og = gg & 0x0400;
		return (u16)(((rb | (orb - (orb >> 5))) & 0x7c1f) | ((gg | (og - (og >> 5))) & 0x03e0));
	}
	return blend(b, f, semi);
}

// Las escrituras que fallan en la caché de datos del ARM9 van una a una a la RAM principal (unos 20
// ciclos cada una): leer antes una vez cada línea de 32 bytes del tramo la mete en la caché y las
// escrituras de después la aciertan (la caché es de post-escritura)
static inline void prefetch_span(const u16 *d, int n)
{
#ifdef ARM9
	for (const u16 *p = (const u16 *)((uintptr_t)d & ~31u); p < d + n; p += 16) (void)*(const volatile u16 *)p;
#else
	(void)d; (void)n;
#endif
}

static inline __attribute__((always_inline)) void span_core(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b,
                                                            const int TEX, const int GOUR, const int RAW, const int SEMI)
{
	// todo a variables locales: si no, el compilador lo relee en cada píxel (las escrituras en la VRAM
	// podrían pisar rs, que tiene campos u16)
	const s32 du = rs.du, dv = rs.dv, dr = rs.dr, dg = rs.dg, db = rs.db;
	const int mu = rs.mu, ou = rs.ou, mv = rs.mv, ov = rs.ov, tx = rs.tx, ty = rs.ty, semi = rs.semi, check = rs.check;
	const u16 set = rs.set, flat = rs.flat;
	const u16 *const clut = rs.clut, *const vram = psx_vram;
	const u8 *const lr = rs.lr, *const lg = rs.lg, *const lb = rs.lb;
	prefetch_span(d, n);
#ifdef GPU_NOSPAN
	n = 0;
#endif
	for (; n > 0; n--, d++, u += du, v += dv, r += dr, gg += dg, b += db) {
		u16 c;
		int blend_on = SEMI;
		if (TEX) {
			int tu = ((u >> 16) & mu) | ou, tv = ((v >> 16) & mv) | ov;
			const u16 *row = vram + (((ty + tv) & 511) << 10);
			if (TEX == 1) c = clut[(row[(tx + (tu >> 2)) & 1023] >> ((tu & 3) << 2)) & 0xf];
			else if (TEX == 2) c = clut[(row[(tx + (tu >> 1)) & 1023] >> ((tu & 1) << 3)) & 0xff];
			else c = row[(tx + tu) & 1023];
			if (!c) continue;
			if (!RAW) {
				if (GOUR) c = modulate(c, r >> 16, gg >> 16, b >> 16);
				else c = (u16)(lr[c & 31] | (lg[(c >> 5) & 31] << 5) | (lb[(c >> 10) & 31] << 10) | (c & 0x8000));
			}
			if (SEMI) blend_on = c & 0x8000;
		} else if (GOUR) {
			int cr = r >> 16, cg = gg >> 16, cb = b >> 16;
			c = (u16)((cr >> 3) | ((cg >> 3) << 5) | ((cb >> 3) << 10));
		} else c = flat;
		if (check && (*d & 0x8000)) continue;
		if (blend_on) c = (c & 0x8000) | blend_fast(*d, c, semi);
		*d = c | set;
	}
}

typedef void (*span_fn)(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b);
// los de 15 bits (poco usados en 3D) se quedan en la RAM normal: la ITCM no da para todos
#define SPAN_ATTR_FAST GPU_ITCM
#define SPAN_ATTR_SLOW __attribute__((noinline))
#define SPAN_AT(K, T, G, R, S) \
	static SPAN_ATTR_##K void span_##T##G##R##S(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b) { span_core(d, n, u, v, r, gg, b, T, G, R, S); }
#define SPAN(T, G, R, S) SPAN_AT(FAST, T, G, R, S)
#define SPAN_SLOW(T, G, R, S) SPAN_AT(SLOW, T, G, R, S)
#define SPAN_S(T, G, R) SPAN(T, G, R, 0) SPAN(T, G, R, 1)
#define SPAN_RS(T, G) SPAN_S(T, G, 0) SPAN_S(T, G, 1)
SPAN_S(0, 0, 1) SPAN_S(0, 1, 1)
// textura: casi siempre van por span_f4/span_g4... (más abajo), estos se quedan fuera de la ITCM
#define SPAN_RS_SLOW(T, G) SPAN_SLOW(T, G, 0, 0) SPAN_SLOW(T, G, 0, 1) SPAN_SLOW(T, G, 1, 0) SPAN_SLOW(T, G, 1, 1)
SPAN_RS_SLOW(1, 0) SPAN_RS_SLOW(1, 1) SPAN_RS_SLOW(2, 0) SPAN_RS_SLOW(2, 1)
#define SPAN3(G, R) SPAN_SLOW(3, G, R, 0) SPAN_SLOW(3, G, R, 1)
SPAN3(0, 0) SPAN3(0, 1) SPAN3(1, 0) SPAN3(1, 1)
// [textura][gouraud][sin modular][semitransparente]
static const span_fn spans[4][2][2][2] = {
	{ { { span_0011, span_0011 }, { span_0010, span_0011 } }, { { span_0111, span_0111 }, { span_0110, span_0111 } } },
#define SPAN_ROW(T) { { { span_##T##000, span_##T##001 }, { span_##T##010, span_##T##011 } }, \
                      { { span_##T##100, span_##T##101 }, { span_##T##110, span_##T##111 } } }
	SPAN_ROW(1), SPAN_ROW(2), SPAN_ROW(3)
};

// ---- bucles rápidos: textura de 4/8 bits sin gouraud, sin ventana y sin bit de máscara ----
// Es lo que usan casi todos los polígonos 3D. La paleta ya modulada está en tex_tab (bit 16 =
// texel 0, transparente) y la dirección del texel sale de u y v en 16.16 con tres operaciones.
static u32 tex_tab[256];
static u16 tab_clut[256];          // la paleta con la que se hizo tex_tab
static int tab_n;                  // 16 o 256 entradas válidas (0 = ninguna)
static u32 tab_mod;                // color de modulación (1 << 24 = sin modular)

static void tex_tab_build(int ncl, u32 mod)
{
	const u16 *clut = rs.clut;
	if (tab_n == ncl && tab_mod == mod && !memcmp(tab_clut, clut, ncl * 2)) return;   // misma paleta y color
	for (int i = 0; i < ncl; i++) {
		u16 c = clut[i];
		tab_clut[i] = c;
		if (!c) tex_tab[i] = 0x10000;
		else if (mod >> 24) tex_tab[i] = c;
		else tex_tab[i] = rs.lr[c & 31] | (rs.lg[(c >> 5) & 31] << 5) | (rs.lb[(c >> 10) & 31] << 10) | (c & 0x8000);
	}
	tab_n = ncl;
	tab_mod = mod;
}

// Píxel final: c (bit 16 = transparente) sobre el fondo bg
static inline u32 shade(u32 c, u32 bg, const int SEMI, int semi)
{
	if (SEMI && !(c & 0x10000) && (c & 0x8000)) c = 0x8000 | blend_fast((u16)bg, (u16)c, semi);
	return c;
}

// Dos píxeles alineados de una vez (una escritura de 32 bits si los dos se ven: la mitad de accesos a la RAM)
static inline void put2(u16 *d, u32 c0, u32 c1)
{
	if (!((c0 | c1) & 0x10000)) *(u32 *)d = c0 | (c1 << 16);
	else {
		if (!(c0 & 0x10000)) d[0] = (u16)c0;
		if (!(c1 & 0x10000)) d[1] = (u16)c1;
	}
}

#define TEX_UV4(u, v) tex_tab[(page[(((v) >> 6) & 0x3fc00) | (((u) >> 18) & 0x3f)] >> (((u) >> 14) & 12)) & 15]
#define TEX_UV8(u, v) tex_tab[(page[(((v) >> 6) & 0x3fc00) | (((u) >> 17) & 0x7f)] >> (((u) >> 13) & 8)) & 255]
#define SPAN_FAST(NAME, TEX, SEMI) \
	static GPU_ITCM void NAME(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b) \
	{ \
		const u16 *page = rs.page; \
		const s32 du = rs.du, dv = rs.dv; \
		const int semi = rs.semi; \
		u32 c; \
		(void)r; (void)gg; (void)b; (void)semi; \
		if (n <= 0) return; \
		prefetch_span(d, n); \
		if ((uintptr_t)d & 2) { \
			c = shade(TEX(u, v), SEMI ? *d : 0, SEMI, semi); \
			if (!(c & 0x10000)) *d = (u16)c; \
			d++; n--; u += du; v += dv; \
		} \
		for (; n >= 2; n -= 2, d += 2) { \
			u32 c0 = TEX(u, v), c1 = TEX(u + du, v + dv), bg = SEMI ? *(u32 *)d : 0; \
			u += 2 * du; v += 2 * dv; \
			put2(d, shade(c0, bg, SEMI, semi), shade(c1, bg >> 16, SEMI, semi)); \
		} \
		if (n) { \
			c = shade(TEX(u, v), SEMI ? *d : 0, SEMI, semi); \
			if (!(c & 0x10000)) *d = (u16)c; \
		} \
	}
SPAN_FAST(span_f4, TEX_UV4, 0)
SPAN_FAST(span_f4s, TEX_UV4, 1)
SPAN_FAST(span_f8, TEX_UV8, 0)
SPAN_FAST(span_f8s, TEX_UV8, 1)

// Textura con gouraud: el color del texel (sin modular, de tex_tab) se multiplica por el color
// interpolado con una tabla en vez de multiplicar y saturar cada canal
static u8 mod_lut[256 * 32];   // [color 8 bits][canal 5 bits] = min(31, canal * color >> 7), como modulate()
static int mod_lut_ready;

static void mod_lut_init(void)
{
	for (int c = 0; c < 256; c++)
		for (int t = 0; t < 32; t++) {
			int m = (t * c) >> 7;
			mod_lut[c * 32 + t] = (u8)(m > 31 ? 31 : m);
		}
	mod_lut_ready = 1;
}

#define SPAN_GOUR(NAME, TEX, SEMI) \
	static GPU_ITCM void NAME(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b) \
	{ \
		const u16 *page = rs.page; \
		const s32 du = rs.du, dv = rs.dv, dr = rs.dr, dg = rs.dg, db = rs.db; \
		const int semi = rs.semi; \
		(void)semi; \
		if (n <= 0) return; \
		prefetch_span(d, n); \
		for (; n > 0; n--, d++, u += du, v += dv, r += dr, gg += dg, b += db) { \
			u32 c = TEX(u, v); \
			if (c & 0x10000) continue; \
			c = mod_lut[((r >> 11) & 0x1fe0) | (c & 31)] | (mod_lut[((gg >> 11) & 0x1fe0) | ((c >> 5) & 31)] << 5) | \
			    (mod_lut[((b >> 11) & 0x1fe0) | ((c >> 10) & 31)] << 10) | (c & 0x8000); \
			if (SEMI && (c & 0x8000)) c = 0x8000 | blend_fast(*d, (u16)c, semi); \
			*d = (u16)c; \
		} \
	}
SPAN_GOUR(span_g4, TEX_UV4, 0)
SPAN_GOUR(span_g4s, TEX_UV4, 1)
SPAN_GOUR(span_g8, TEX_UV8, 0)
SPAN_GOUR(span_g8s, TEX_UV8, 1)

// Sin textura con gouraud (degradados): el color de 8 bits por canal se queda en 5
#define SPAN_COLOR(NAME, SEMI) \
	static GPU_ITCM void NAME(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b) \
	{ \
		const s32 dr = rs.dr, dg = rs.dg, db = rs.db; \
		const int semi = rs.semi; \
		const u32 set = rs.set; \
		(void)u; (void)v; (void)semi; \
		if (n <= 0) return; \
		prefetch_span(d, n); \
		for (; n > 0; n--, d++, r += dr, gg += dg, b += db) { \
			u32 c = (u16)((r >> 19) | ((gg >> 19) << 5) | ((b >> 19) << 10)); \
			if (SEMI) c = blend_fast(*d, (u16)c, semi); \
			*d = (u16)(c | set); \
		} \
	}
SPAN_COLOR(span_cg, 0)
SPAN_COLOR(span_cgs, 1)

// Rectángulos (sprites) con textura de 4/8 bits sin voltear: la fila de textura es fija y u avanza de
// uno en uno, así que basta con el índice dentro de la fila
#define TEX_ROW(tu, PER, BITS) tex_tab[(row[((tu) & 0xff) / PER] >> (((tu) % PER) * BITS)) & ((1u << BITS) - 1)]
#define RECT_FAST(NAME, PER, BITS, SEMI) \
	static GPU_ITCM void NAME(u16 *d, int n, u32 tu, const u16 *row) \
	{ \
		const int semi = rs.semi; \
		u32 c; \
		(void)semi; \
		if (n <= 0) return; \
		prefetch_span(d, n); \
		if ((uintptr_t)d & 2) { \
			c = shade(TEX_ROW(tu, PER, BITS), SEMI ? *d : 0, SEMI, semi); \
			if (!(c & 0x10000)) *d = (u16)c; \
			d++; n--; tu++; \
		} \
		for (; n >= 2; n -= 2, d += 2, tu += 2) { \
			u32 c0 = TEX_ROW(tu, PER, BITS), c1 = TEX_ROW(tu + 1, PER, BITS), bg = SEMI ? *(u32 *)d : 0; \
			put2(d, shade(c0, bg, SEMI, semi), shade(c1, bg >> 16, SEMI, semi)); \
		} \
		if (n) { \
			c = shade(TEX_ROW(tu, PER, BITS), SEMI ? *d : 0, SEMI, semi); \
			if (!(c & 0x10000)) *d = (u16)c; \
		} \
	}
RECT_FAST(rect_f4, 4, 4, 0)
RECT_FAST(rect_f4s, 4, 4, 1)
RECT_FAST(rect_f8, 2, 8, 0)
RECT_FAST(rect_f8s, 2, 8, 1)

// ---- media resolución horizontal (gpu_half_x, solo triángulos) ----
// Se calcula un píxel de cada par alineado y se escribe en los dos: la mitad de texturas, mezclas y
// colores. Para semitransparencias se mezcla con el fondo del primero de los dos.
#define SPAN_FAST_H(NAME, TEX, SEMI) \
	static void NAME(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b) \
	{ \
		const u16 *page = rs.page; \
		const s32 du = rs.du, dv = rs.dv; \
		const int semi = rs.semi; \
		u32 c; \
		(void)r; (void)gg; (void)b; (void)semi; \
		if (n <= 0) return; \
		prefetch_span(d, n); \
		if ((uintptr_t)d & 2) { \
			c = shade(TEX(u, v), SEMI ? *d : 0, SEMI, semi); \
			if (!(c & 0x10000)) *d = (u16)c; \
			d++; n--; u += du; v += dv; \
		} \
		for (; n >= 2; n -= 2, d += 2, u += 2 * du, v += 2 * dv) { \
			c = shade(TEX(u, v), SEMI ? *d : 0, SEMI, semi); \
			if (!(c & 0x10000)) *(u32 *)d = (c & 0xffff) * 0x10001u; \
		} \
		if (n) { \
			c = shade(TEX(u, v), SEMI ? *d : 0, SEMI, semi); \
			if (!(c & 0x10000)) *d = (u16)c; \
		} \
	}
SPAN_FAST_H(span_f4_h, TEX_UV4, 0)
SPAN_FAST_H(span_f4s_h, TEX_UV4, 1)
SPAN_FAST_H(span_f8_h, TEX_UV8, 0)
SPAN_FAST_H(span_f8s_h, TEX_UV8, 1)

#define GOUR_PX(TEX, SEMI) \
	u32 c = TEX(u, v); \
	if (!(c & 0x10000)) { \
		c = mod_lut[((r >> 11) & 0x1fe0) | (c & 31)] | (mod_lut[((gg >> 11) & 0x1fe0) | ((c >> 5) & 31)] << 5) | \
		    (mod_lut[((b >> 11) & 0x1fe0) | ((c >> 10) & 31)] << 10) | (c & 0x8000); \
		if (SEMI && (c & 0x8000)) c = 0x8000 | blend_fast(*d, (u16)c, semi);
#define SPAN_GOUR_H(NAME, TEX, SEMI) \
	static void NAME(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b) \
	{ \
		const u16 *page = rs.page; \
		const s32 du = rs.du, dv = rs.dv, dr = rs.dr, dg = rs.dg, db = rs.db; \
		const int semi = rs.semi; \
		(void)semi; \
		if (n <= 0) return; \
		prefetch_span(d, n); \
		if ((uintptr_t)d & 2) { \
			GOUR_PX(TEX, SEMI) *d = (u16)c; } \
			d++; n--; u += du; v += dv; r += dr; gg += dg; b += db; \
		} \
		for (; n >= 2; n -= 2, d += 2, u += 2 * du, v += 2 * dv, r += 2 * dr, gg += 2 * dg, b += 2 * db) { \
			GOUR_PX(TEX, SEMI) *(u32 *)d = c * 0x10001u; } \
		} \
		if (n) { GOUR_PX(TEX, SEMI) *d = (u16)c; } } \
	}
SPAN_GOUR_H(span_g4_h, TEX_UV4, 0)
SPAN_GOUR_H(span_g4s_h, TEX_UV4, 1)
SPAN_GOUR_H(span_g8_h, TEX_UV8, 0)
SPAN_GOUR_H(span_g8s_h, TEX_UV8, 1)

#define SPAN_COLOR_H(NAME, SEMI) \
	static void NAME(u16 *d, int n, s32 u, s32 v, s32 r, s32 gg, s32 b) \
	{ \
		const s32 dr = rs.dr, dg = rs.dg, db = rs.db; \
		const int semi = rs.semi; \
		const u32 set = rs.set; \
		u32 c; \
		(void)u; (void)v; (void)semi; \
		if (n <= 0) return; \
		prefetch_span(d, n); \
		if ((uintptr_t)d & 2) { \
			c = (u16)((r >> 19) | ((gg >> 19) << 5) | ((b >> 19) << 10)); \
			if (SEMI) c = blend_fast(*d, (u16)c, semi); \
			*d = (u16)(c | set); \
			d++; n--; r += dr; gg += dg; b += db; \
		} \
		for (; n >= 2; n -= 2, d += 2, r += 2 * dr, gg += 2 * dg, b += 2 * db) { \
			c = (u16)((r >> 19) | ((gg >> 19) << 5) | ((b >> 19) << 10)); \
			if (SEMI) c = blend_fast(*d, (u16)c, semi); \
			*(u32 *)d = (c | set) * 0x10001u; \
		} \
		if (n) { \
			c = (u16)((r >> 19) | ((gg >> 19) << 5) | ((b >> 19) << 10)); \
			if (SEMI) c = blend_fast(*d, (u16)c, semi); \
			*d = (u16)(c | set); \
		} \
	}
SPAN_COLOR_H(span_cg_h, 0)
SPAN_COLOR_H(span_cgs_h, 1)

u8 gpu_half_x;

static span_fn half_of(span_fn fn)
{
	static const span_fn full[] = { span_f4, span_f4s, span_f8, span_f8s, span_g4, span_g4s, span_g8, span_g8s, span_cg, span_cgs };
	static const span_fn half[] = { span_f4_h, span_f4s_h, span_f8_h, span_f8s_h, span_g4_h, span_g4s_h, span_g8_h, span_g8s_h, span_cg_h, span_cgs_h };
	for (unsigned i = 0; i < sizeof full / sizeof full[0]; i++)
		if (fn == full[i]) return half[i];
	return fn;
}

// Prepara rs para un primitivo; tex: 0 sin textura, si no usa la textura activa (use_texture)
static span_fn span_setup(int tex, int gouraud, int raw, int semi_on, u32 rgb)
{
	int t = tex ? t_depth + 1 : 0;
	if (t > 3) t = 3;   // profundidad 3 (reservada) se dibuja como 15 bits
	rs.semi = g.semi;
	rs.check = g.check_mask;
	rs.set = (u16)(g.set_mask << 15);
	rs.flat = rgb15(rgb);
	if (t) {
		int ncl = t == 1 ? 16 : 256;
		rs.tx = t_px;
		rs.ty = t_py;
		rs.mu = 0xff & ~(g.tw_mx * 8);
		rs.ou = (g.tw_ox & g.tw_mx) * 8;
		rs.mv = 0xff & ~(g.tw_my * 8);
		rs.ov = (g.tw_oy & g.tw_my) * 8;
		if (t < 3) {
			if (t_cx + ncl <= 1024) rs.clut = &psx_vram[t_cy * 1024 + t_cx];
			else {   // la paleta da la vuelta a la VRAM
				for (int i = 0; i < ncl; i++) clut_copy[i] = VRAM(t_cx + i, t_cy);
				rs.clut = clut_copy;
			}
		}
		if (!raw && !gouraud) {
			if ((rgb & 0xffffff) == 0x808080) raw = 1;   // modular por 1.0 no cambia nada
			else if (rgb != lut_rgb) {
				int cr = rgb & 0xff, cg = (rgb >> 8) & 0xff, cb = (rgb >> 16) & 0xff;
				for (int i = 0; i < 32; i++) {
					int a = (i * cr) >> 7, bb = (i * cg) >> 7, c = (i * cb) >> 7;
					rs.lr[i] = (u8)(a > 31 ? 31 : a);
					rs.lg[i] = (u8)(bb > 31 ? 31 : bb);
					rs.lb[i] = (u8)(c > 31 ? 31 : c);
				}
				lut_rgb = rgb;
			}
		}
	} else raw = 1;
	if (!t && gouraud && !rs.check) return semi_on ? span_cgs : span_cg;   // degradados (los rectángulos nunca son gouraud)
	// el caso más común (ver span_f4): página que no da la vuelta a la VRAM por la derecha
	if ((t == 1 || (t == 2 && rs.tx <= 1024 - 128)) && !rs.check && !rs.set && rs.mu == 0xff && !rs.ou && rs.mv == 0xff &&
	    !rs.ov) {
		rs.page = &psx_vram[rs.ty * 1024 + rs.tx];
		if (gouraud && !raw) {   // gouraud: la paleta sin modular y el color por píxel (span_g4)
			if (!mod_lut_ready) mod_lut_init();
			tex_tab_build(t == 1 ? 16 : 256, 1u << 24);
			if (t == 1) return semi_on ? span_g4s : span_g4;
			return semi_on ? span_g8s : span_g8;
		}
		tex_tab_build(t == 1 ? 16 : 256, raw ? 1u << 24 : rgb & 0xffffff);
		if (t == 1) return semi_on ? span_f4s : span_f4;
		return semi_on ? span_f8s : span_f8;
	}
	return spans[t][gouraud ? 1 : 0][raw ? 1 : 0][semi_on ? 1 : 0];
}

// ---- triángulos ----
typedef struct { int x, y, r, g, b, u, v; } Vtx;

static inline s32 edge(const Vtx *a, const Vtx *b, int px, int py)
{
	return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static inline int top_left(const Vtx *a, const Vtx *b)
{
	int ey = b->y - a->y, ex = b->x - a->x;
	return ey < 0 || (ey == 0 && ex > 0);
}

// División 64/32 truncando hacia cero, como en C; en la DS con el divisor por hardware (unos 34 ciclos
// frente a cientos de la división de 64 bits por software)
static inline s32 div64_32(s64 n, s32 d)
{
#ifdef ARM9
	*(volatile u16 *)0x04000280 = 1;   // DIVCNT: 64 / 32
	*(volatile s64 *)0x04000290 = n;
	*(volatile s32 *)0x04000298 = d;
	while (*(volatile u16 *)0x04000280 & 0x8000) { }
	return *(volatile s32 *)0x040002a0;
#else
	return (s32)(n / d);
#endif
}

// Gradiente de un atributo en 16.16 a partir de los tres vértices
static void grad(int a0, int a1, int a2, const Vtx *v0, const Vtx *v1, const Vtx *v2, s32 area, s32 *dx, s32 *dy)
{
	s64 d1 = a1 - a0, d2 = a2 - a0;
	*dx = div64_32((d1 * (v2->y - v0->y) - d2 * (v1->y - v0->y)) << 16, area);
	*dy = div64_32((d2 * (v1->x - v0->x) - d1 * (v2->x - v0->x)) << 16, area);
}

#ifdef PSX_GPU_REF
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../pc/gpu_ref.inc"
#endif

// Límite de columnas de una arista en cada fila: q = floor(w / |d|) llevado con su resto r
typedef struct { s32 q, r, D, qs, rs; int kind; } EdgeStep;   // kind: 1 = límite izquierdo, -1 = derecho, 0 = horizontal

static inline s32 floordiv(s32 a, s32 d)   // d > 0
{
	s32 q = div64_32(a, d);
	if (q * d != a && a < 0) q--;
	return q;
}

static void edge_step_init(EdgeStep *s, s32 w, s32 d, s32 wdy)
{
	if (d == 0) {   // horizontal: solo importa el signo de w, que cambia wdy por fila
		s->kind = 0;
		s->q = w;
		s->qs = wdy;
		s->r = s->rs = 0;
		s->D = 1;
		return;
	}
	s->kind = d > 0 ? 1 : -1;
	s->D = d > 0 ? d : -d;
	s->q = floordiv(w, s->D);
	s->r = w - s->q * s->D;
	s->qs = floordiv(wdy, s->D);
	s->rs = wdy - s->qs * s->D;
}

static void triangle(Vtx *v0, Vtx *v1, Vtx *v2, int textured, int gouraud, int semi_on, int raw)
{
	s32 area;
	int minx, maxx, miny, maxy, b0, b1, b2, last;
	s32 drdx = 0, drdy = 0, dgdx = 0, dgdy = 0, dbdx = 0, dbdy = 0, dudx = 0, dudy = 0, dvdx = 0, dvdy = 0;
	span_fn fn;
#ifdef PSX_GPU_REF
	if (use_ref()) { triangle_ref(v0, v1, v2, textured, gouraud, semi_on, raw); return; }
	if (check_on() && !in_check) {
		Vtx a = *v0, b = *v1, c = *v2;
		in_check = 1;
		CHECK_BEGIN();
		triangle_ref(&a, &b, &c, textured, gouraud, semi_on, raw);
		CHECK_REF();
		triangle(v0, v1, v2, textured, gouraud, semi_on, raw);
		in_check = 0;
		check_end("triángulo");
		return;
	}
#endif

	area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
	if (area == 0) return;
	if (area < 0) { Vtx *t = v1; v1 = v2; v2 = t; area = -area; }

	minx = v0->x; if (v1->x < minx) minx = v1->x; if (v2->x < minx) minx = v2->x;
	maxx = v0->x; if (v1->x > maxx) maxx = v1->x; if (v2->x > maxx) maxx = v2->x;
	miny = v0->y; if (v1->y < miny) miny = v1->y; if (v2->y < miny) miny = v2->y;
	maxy = v0->y; if (v1->y > maxy) maxy = v1->y; if (v2->y > maxy) maxy = v2->y;
	if (maxx - minx >= 1024 || maxy - miny >= 512) return;   // la PS1 descarta polígonos gigantes
	if (minx < g.da_x1) minx = g.da_x1;
	if (miny < g.da_y1) miny = g.da_y1;
	if (maxx > g.da_x2) maxx = g.da_x2;
	if (maxy > g.da_y2) maxy = g.da_y2;
	if (minx > maxx || miny > maxy) return;

	if (gouraud) {
		grad(v0->r, v1->r, v2->r, v0, v1, v2, area, &drdx, &drdy);
		grad(v0->g, v1->g, v2->g, v0, v1, v2, area, &dgdx, &dgdy);
		grad(v0->b, v1->b, v2->b, v0, v1, v2, area, &dbdx, &dbdy);
	}
	if (textured) {
		grad(v0->u, v1->u, v2->u, v0, v1, v2, area, &dudx, &dudy);
		grad(v0->v, v1->v, v2->v, v0, v1, v2, area, &dvdx, &dvdy);
	}
	fn = span_setup(textured, gouraud, raw, semi_on, (u32)(v0->r | (v0->g << 8) | (v0->b << 16)));
	if (gpu_half_x) fn = half_of(fn);
	rs.du = dudx; rs.dv = dvdx; rs.dr = drdx; rs.dg = dgdx; rs.db = dbdx;

	b0 = top_left(v1, v2) ? 0 : -1;
	b1 = top_left(v2, v0) ? 0 : -1;
	b2 = top_left(v0, v1) ? 0 : -1;
	last = maxx - minx;

	{
		// funciones de arista en (minx, y) y su cambio por píxel (d) y por fila (dy)
		s32 w0r = edge(v1, v2, minx, miny) + b0, w1r = edge(v2, v0, minx, miny) + b1, w2r = edge(v0, v1, minx, miny) + b2;
		s32 d0 = -(v2->y - v1->y), d1 = -(v0->y - v2->y), d2 = -(v1->y - v0->y);
		s32 w0dy = v2->x - v1->x, w1dy = v0->x - v2->x, w2dy = v1->x - v0->x;
		int dx0 = minx - v0->x, dy0 = miny - v0->y;
		s32 rr = (v0->r << 16) + drdx * dx0 + drdy * dy0 + 0x8000;
		s32 gr = (v0->g << 16) + dgdx * dx0 + dgdy * dy0 + 0x8000;
		s32 br = (v0->b << 16) + dbdx * dx0 + dbdy * dy0 + 0x8000;
		s32 ur = (v0->u << 16) + dudx * dx0 + dudy * dy0 + 0x8000;
		s32 vr = (v0->v << 16) + dvdx * dx0 + dvdy * dy0 + 0x8000;
		u16 *line = &psx_vram[miny * 1024 + minx];
		// Columnas k = x - minx con w + d*k >= 0 en las tres aristas: con d > 0, k >= -floor(w/d); con
		// d < 0, k <= floor(w/-d). El cociente se lleva fila a fila con su resto (sin dividir en cada fila).
		EdgeStep es[3];
		edge_step_init(&es[0], w0r, d0, w0dy);
		edge_step_init(&es[1], w1r, d1, w1dy);
		edge_step_init(&es[2], w2r, d2, w2dy);

		for (int y = miny; y <= maxy; y++, line += 1024) {
			int lo = 0, hi = last;
			for (int e = 0; e < 3; e++) {
				EdgeStep *s = &es[e];
				if (s->kind > 0) { if (-s->q > lo) lo = -s->q; }
				else if (s->kind < 0) { if (s->q < hi) hi = s->q; }
				else if (s->q < 0) lo = hi + 1;
				s->q += s->qs;   // siguiente fila
				s->r += s->rs;
				if (s->r >= s->D) { s->r -= s->D; s->q++; }
			}
			if (lo <= hi && (!gpu_rows_on || gpu_row_draw[y & 511])) {
				STAT_ADD(gpu_pixels, hi - lo + 1);
				STAT_ADD(gpu_px_kind[fn == span_f4 || fn == span_f4s || fn == span_f8 || fn == span_f8s || fn == span_g4 || fn == span_g4s || fn == span_g8 || fn == span_g8s || fn == span_cg || fn == span_cgs ? 0 : 1], hi - lo + 1);
				fn(line + lo, hi - lo + 1, (s32)((u32)ur + (u32)dudx * lo), (s32)((u32)vr + (u32)dvdx * lo),
				   (s32)((u32)rr + (u32)drdx * lo), (s32)((u32)gr + (u32)dgdx * lo), (s32)((u32)br + (u32)dbdx * lo));
			}
			rr += drdy; gr += dgdy; br += dbdy; ur += dudy; vr += dvdy;
		}
	}
}

// ---- dibujo por hardware (gpu_hw) ----
// ¿El área de dibujo está dentro de un buffer de pantalla (el mostrado o el otro)? Si es así, deja en
// hw_ox/hw_oy la esquina de ese buffer (si cae en los dos, el de la esquina más cercana: la ventana
// mostrada puede empezar unas filas antes que el área donde se dibuja). Lo demás (texturas que dibuja
// el juego) va a la VRAM.
static int hw_ox, hw_oy;

static int buf_dist(int bx, int by)   // -1 si el área no empieza dentro del buffer
{
	int w = gpu_disp.width, h = gpu_disp.height;
	if (g.da_x1 < bx || g.da_x1 >= bx + w || g.da_y1 < by || g.da_y1 >= by + h) return -1;
	return (g.da_x1 - bx) + (g.da_y1 - by);
}

static int hw_screen(void)
{
	int a, b;
	if (!gpu_hw || gpu_disp.color24) return 0;   // en 24 bits (vídeos) lo que se dibuje va a la VRAM, como en la PS1
	a = buf_dist(g.disp_x, g.disp_y);
	b = buf_dist(prev_disp_x, prev_disp_y);
	if (a < 0 && b < 0) return 0;
	if (b >= 0 && (a < 0 || b < a)) { hw_ox = prev_disp_x; hw_oy = prev_disp_y; }
	else { hw_ox = g.disp_x; hw_oy = g.disp_y; }
	return 1;
}

static void hw_send(const Vtx *v, int n, int tex, int gour, int semi_on, int raw)
{
	GpuHwVtx hv[4];
	GpuHwPoly p;
#ifdef PSX_GPUSTAT
	{
		extern int gpudump;
		extern int printf(const char *, ...);
		if (gpudump) printf("MASK set=%d check=%d semi=%d tex=%d\n", g.set_mask, g.check_mask, semi_on ? g.semi : -1, tex);
	}
#endif
	for (int k = 0; k < n; k++) {
		hv[k].x = (s16)v[k].x; hv[k].y = (s16)v[k].y;
		hv[k].r = (u8)v[k].r; hv[k].g = (u8)v[k].g; hv[k].b = (u8)v[k].b;
		hv[k].u = (s16)v[k].u; hv[k].v = (s16)v[k].v;
	}
	p.tex = tex ? (u8)(t_depth > 2 ? 3 : t_depth + 1) : 0;
	p.raw = (u8)(raw != 0);
	p.gouraud = (u8)(gour != 0);
	p.semi = semi_on ? (u8)(g.semi + 1) : 0;
	p.mask = (u8)(g.check_mask && g.set_mask);
	p.tpx = (s16)t_px; p.tpy = (s16)t_py; p.cx = (s16)t_cx; p.cy = (s16)t_cy;
	p.ox = (s16)hw_ox; p.oy = (s16)hw_oy;
	gpu_hw->poly(hv, n, &p);
}

static void pend_touch(int x, int y, int w, int h);

static void hw_dirty_area(void)   // algo dibujado por software fuera de la pantalla
{
	if (gpu_hw) pend_touch(g.da_x1, g.da_y1, g.da_x2 - g.da_x1 + 1, g.da_y2 - g.da_y1 + 1);
	if (gpu_hw) gpu_hw->vram_written(g.da_x1, g.da_y1, g.da_x2 - g.da_x1 + 1, g.da_y2 - g.da_y1 + 1);
}

static void polygon(void)
{
	u32 cmd = fifo[0] >> 24;
	int quad = cmd & 8, tex = cmd & 4, gour = cmd & 0x10, semi_on = cmd & 2, raw = cmd & 1;
	int nv = quad ? 4 : 3, i = 1;
	Vtx v[4];
	u32 clut = 0, col = fifo[0] & 0xffffff;
#ifdef BENCH_NO_SEMI
	if (semi_on) return;   // medida: sin semitransparencias
#endif

	for (int k = 0; k < nv; k++) {
		u32 xy;
		if (k > 0 && gour) col = fifo[i++] & 0xffffff;
		xy = fifo[i++];
		v[k].x = sx11(xy) + g.off_x;
		v[k].y = sx11(xy >> 16) + g.off_y;
		v[k].r = col & 0xff;
		v[k].g = (col >> 8) & 0xff;
		v[k].b = (col >> 16) & 0xff;
		v[k].u = v[k].v = 0;
		if (tex) {
			u32 uv = fifo[i++];
			v[k].u = uv & 0xff;
			v[k].v = (uv >> 8) & 0xff;
			if (k == 0) clut = uv >> 16;
			if (k == 1) set_texpage(uv >> 16);
		}
	}
	if (tex) use_texture(clut);
	if (!gour) for (int k = 1; k < nv; k++) { v[k].r = v[0].r; v[k].g = v[0].g; v[k].b = v[0].b; }
	if (tex && !gour && raw) raw = 1;
#ifdef PSX_GPUSTAT
	{   // PC: qué usan los polígonos (colores por encima de 1.0, modos de mezcla)
		extern u32 gpustat[16];
		int mx = 0;
		for (int k = 0; k < nv; k++) { if (v[k].r > mx) mx = v[k].r; if (v[k].g > mx) mx = v[k].g; if (v[k].b > mx) mx = v[k].b; }
		gpustat[0]++;
		if (tex && !raw && mx > 128) gpustat[1]++;
		if (semi_on) gpustat[2 + g.semi]++;
		if (tex) gpustat[6 + t_depth]++;
		if (gour) gpustat[10]++;
		if (tex && semi_on) gpustat[11]++;
		{
			extern int gpudump;
			extern int printf(const char *, ...);
			int a = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
			if (a < 0) a = -a;
			if (gpudump && a > 300 && v[0].x < 168 && v[0].x > 100 && v[0].y > 40 && v[0].y < 210)
				printf("POLY nv=%d tex=%d d=%d semi=%d raw=%d gour=%d xy=%d,%d %d,%d %d,%d uv=%d,%d %d,%d col=%d,%d,%d clut=%d,%d tp=%d,%d\n",
				       nv, tex, t_depth, g.semi, raw, gour, v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, v[0].u, v[0].v,
				       v[2].u, v[2].v, v[0].r, v[0].g, v[0].b, t_cx, t_cy, t_px, t_py);
		}
		if (tex && semi_on && t_depth == 0) {
			int s = 0, nz = 0;
			for (int i = 0; i < 16; i++) { u16 c = VRAM(t_cx + i, t_cy); if (c) { nz++; if (c & 0x8000) s++; } }
			gpustat[12 + (s == 0 ? 0 : s == nz ? 2 : 1)]++;
		}
	}
#endif
	if (hw_screen()) { hw_send(v, nv, tex, gour, semi_on, raw); return; }
	hw_dirty_area();

	{
		Vtx a = v[0], b = v[1], c = v[2];
		triangle(&a, &b, &c, tex, gour, semi_on, raw);
	}
	if (quad) {
		Vtx a = v[1], b = v[2], c = v[3];
		triangle(&a, &b, &c, tex, gour, semi_on, raw);
	}
}

static void rectangle(void)
{
	u32 cmd = fifo[0] >> 24;
	int tex = cmd & 4, semi_on = cmd & 2, raw = cmd & 1, size = (cmd >> 3) & 3, i = 1;
	u32 col = fifo[0] & 0xffffff, xy = fifo[i++], uv = 0;
	int x0 = sx11(xy) + g.off_x, y0 = sx11(xy >> 16) + g.off_y, w, h, lo, hi, sgn_u, sgn_v;
	span_fn fn;
#ifdef PSX_GPU_REF
	if (use_ref()) { rectangle_ref(); return; }
	if (check_on() && !in_check) {
		in_check = 1;
		CHECK_BEGIN();
		rectangle_ref();
		CHECK_REF();
		rectangle();
		in_check = 0;
		check_end("rectángulo");
		return;
	}
#endif

	if (tex) uv = fifo[i++];
	if (size == 0) { u32 wh = fifo[i++]; w = wh & 0x3ff; h = (wh >> 16) & 0x1ff; }
	else if (size == 1) w = h = 1;
	else if (size == 2) w = h = 8;
	else w = h = 16;
	if (tex) use_texture(uv >> 16);
#ifdef PSX_GPUSTAT
	{
		extern int gpudump;
		extern int printf(const char *, ...);
		if (gpudump && w * h > 200)
			printf("RECT xy=%d,%d wh=%d,%d tex=%d d=%d semi=%d/%d raw=%d uv=%d,%d col=%06x clut=%d,%d tp=%d,%d\n", x0, y0, w, h, tex,
			       t_depth, semi_on, g.semi, raw, uv & 0xff, (uv >> 8) & 0xff, col, t_cx, t_cy, t_px, t_py);
	}
#endif
	if (hw_screen()) {
		Vtx q[4];
		int u0 = uv & 0xff, v0 = (uv >> 8) & 0xff;
		for (int k = 0; k < 4; k++) {   // orden de la PS1 para un cuadrado: arriba izq., arriba der., abajo izq., abajo der.
			q[k].x = x0 + ((k & 1) ? w : 0);
			q[k].y = y0 + ((k & 2) ? h : 0);
			q[k].u = u0 + ((k & 1) ? w : 0);
			q[k].v = v0 + ((k & 2) ? h : 0);
			q[k].r = col & 0xff; q[k].g = (col >> 8) & 0xff; q[k].b = (col >> 16) & 0xff;
		}
		hw_send(q, 4, tex, 0, semi_on, raw);
		return;
	}
	hw_dirty_area();

	// columnas dentro del área de dibujo
	lo = g.da_x1 - x0; if (lo < 0) lo = 0;
	hi = g.da_x2 - x0; if (hi > w - 1) hi = w - 1;
	if (lo > hi) return;
	fn = span_setup(tex, 0, raw, semi_on, col);
	sgn_u = g.flip_x ? -1 : 1;
	sgn_v = g.flip_y ? -1 : 1;
	rs.du = (s32)((u32)sgn_u << 16);
	rs.dv = rs.dr = rs.dg = rs.db = 0;
	for (int y = 0; y < h; y++) {
		int py = y0 + y;
		if (py < g.da_y1 || py > g.da_y2) continue;
		if (gpu_rows_on && !gpu_row_draw[py & 511]) continue;
		STAT_ADD(gpu_pixels, hi - lo + 1);
		STAT_ADD(gpu_px_kind[fn == span_f4 || fn == span_f4s || fn == span_f8 || fn == span_f8s ? 2 : 3], hi - lo + 1);
		if (sgn_u > 0 && (fn == span_f4 || fn == span_f4s || fn == span_f8 || fn == span_f8s)) {
			const u16 *row = rs.page + ((((uv >> 8) & 0xff) + sgn_v * y) & 0xff) * 1024;
			u16 *d = &psx_vram[py * 1024 + x0 + lo];
			u32 tu = (uv & 0xff) + lo;
			if (fn == span_f4) rect_f4(d, hi - lo + 1, tu, row);
			else if (fn == span_f4s) rect_f4s(d, hi - lo + 1, tu, row);
			else if (fn == span_f8) rect_f8(d, hi - lo + 1, tu, row);
			else rect_f8s(d, hi - lo + 1, tu, row);
			continue;
		}
		fn(&psx_vram[py * 1024 + x0 + lo], hi - lo + 1, (s32)((u32)((int)(uv & 0xff) + sgn_u * lo) << 16),
		   (s32)((u32)((int)((uv >> 8) & 0xff) + sgn_v * y) << 16), 0, 0, 0);
	}
}

static void line_seg(int x0, int y0, u32 c0, int x1, int y1, u32 c1, int semi_on)
{
	int dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
	if ((dx < 0 ? -dx : dx) >= 1024 || (dy < 0 ? -dy : dy) >= 512) return;
	for (int i = 0; i <= n; i++) {
		int x = n ? x0 + (dx * i + (dx > 0 ? n / 2 : -n / 2)) / n : x0;
		int y = n ? y0 + (dy * i + (dy > 0 ? n / 2 : -n / 2)) / n : y0;
		int r = n ? ((c0 & 0xff) * (n - i) + (c1 & 0xff) * i) / n : (c0 & 0xff);
		int gg = n ? (((c0 >> 8) & 0xff) * (n - i) + ((c1 >> 8) & 0xff) * i) / n : ((c0 >> 8) & 0xff);
		int b = n ? (((c0 >> 16) & 0xff) * (n - i) + ((c1 >> 16) & 0xff) * i) / n : ((c0 >> 16) & 0xff);
		if (x < g.da_x1 || x > g.da_x2 || y < g.da_y1 || y > g.da_y2) continue;
		put(x, y, rgb15(r | (gg << 8) | (b << 16)), semi_on, g.semi);
	}
}

// polilínea: último punto y color
static int pl_x, pl_y, pl_gour, pl_semi, pl_have_col;
static u32 pl_col, pl_next_col;

static void line(void)
{
	u32 cmd = fifo[0] >> 24;
	int gour = cmd & 0x10, semi_on = cmd & 2;
	u32 c0 = fifo[0] & 0xffffff, c1 = c0, xy0 = fifo[1], xy1;
	if (gour) { c1 = fifo[2] & 0xffffff; xy1 = fifo[3]; } else xy1 = fifo[2];
	line_seg(sx11(xy0) + g.off_x, sx11(xy0 >> 16) + g.off_y, c0, sx11(xy1) + g.off_x, sx11(xy1 >> 16) + g.off_y, c1, semi_on);
	if (cmd & 8) {
		mode = M_POLYLINE;
		pl_x = sx11(xy1) + g.off_x;
		pl_y = sx11(xy1 >> 16) + g.off_y;
		pl_col = c1;
		pl_gour = gour;
		pl_semi = semi_on;
		pl_have_col = 0;
	}
}

static void polyline_word(u32 v)
{
	if ((v & 0xf000f000) == 0x50005000) { mode = M_CMD; return; }
	if (pl_gour && !pl_have_col) { pl_next_col = v & 0xffffff; pl_have_col = 1; return; }
	{
		int x = sx11(v) + g.off_x, y = sx11(v >> 16) + g.off_y;
		u32 c = pl_gour ? pl_next_col : pl_col;
		line_seg(pl_x, pl_y, pl_col, x, y, c, pl_semi);
		pl_x = x;
		pl_y = y;
		pl_col = c;
		pl_have_col = 0;
	}
}

// Con dibujo por hardware, el borrado de un buffer de pantalla se queda pendiente y pasa a ser el color de
// fondo del 3D (no escribe la VRAM). Si luego algo escribe en esa zona (un vídeo que se sube encima, por
// ejemplo), entonces se borra de verdad y el fondo del 3D vuelve a ser transparente.
static struct { int on, x, y, w, h; u16 c; } pend;

static void fill_rect(int x0, int y0, int w, int h, u16 c)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) VRAM(x0 + x, y0 + y) = c;
}

static void pend_touch(int x, int y, int w, int h)   // algo va a escribir en la VRAM
{
	if (!pend.on || x >= pend.x + pend.w || x + w <= pend.x || y >= pend.y + pend.h || y + h <= pend.y) return;
	pend.on = 0;
	fill_rect(pend.x, pend.y, pend.w, pend.h, pend.c);
	gpu_hw->clear(0xffff);   // fondo transparente otra vez
	gpu_hw->vram_written(pend.x, pend.y, pend.w, pend.h);
}

static void pend_keep(void)
{
	if (pend.on) gpu_hw->clear(pend.c);
}

static void fill(void)
{
	u16 c = rgb15(fifo[0] & 0xffffff);
	int x0 = fifo[1] & 0x3f0, y0 = (fifo[1] >> 16) & 0x1ff;
	int w = ((fifo[2] & 0x3ff) + 0xf) & ~0xf, h = (fifo[2] >> 16) & 0x1ff;
	if (gpu_hw) pend_touch(x0, y0, w, h);
#ifdef PSX_GPUSTAT
	{ extern int gpudump; extern int printf(const char *, ...); if (gpudump) printf("FILL %d,%d %dx%d c=%04x\n", x0, y0, w, h, c); }
#endif
	if (gpu_hw && w >= gpu_disp.width - 16 && h >= gpu_disp.height * 3 / 4) {   // ¿borra un buffer de pantalla?
		int sx = g.da_x1, sy = g.da_y1, scr;
		g.da_x1 = x0; g.da_y1 = y0;
		scr = hw_screen();
		g.da_x1 = sx; g.da_y1 = sy;
		if (scr) {
			pend.on = 1; pend.x = x0; pend.y = y0; pend.w = w; pend.h = h; pend.c = c;
			gpu_hw->clear(c);
			return;
		}
	}
	fill_rect(x0, y0, w, h, c);
	if (gpu_hw) gpu_hw->vram_written(x0, y0, w, h);
}

static void vram_copy(void)
{
	int sx = fifo[1] & 0x3ff, sy = (fifo[1] >> 16) & 0x1ff;
	int dx = fifo[2] & 0x3ff, dy = (fifo[2] >> 16) & 0x1ff;
	int w = ((fifo[3] - 1) & 0x3ff) + 1, h = (((fifo[3] >> 16) - 1) & 0x1ff) + 1;
	if (gpu_hw) pend_touch(dx, dy, w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			u16 c = VRAM(sx + x, sy + y);
			u16 *d = &VRAM(dx + x, dy + y);
			if (g.check_mask && (*d & 0x8000)) continue;
			*d = c | (g.set_mask << 15);
		}
	if (gpu_hw) gpu_hw->vram_written(dx, dy, w, h);
}

static void load_pixel(u16 c)
{
	if (load_i < load_total) {
		int x = load_x + load_i % load_w, y = load_y + load_i / load_w;
		u16 *d = &VRAM(x, y);
		if (!(g.check_mask && (*d & 0x8000))) *d = c | (g.set_mask << 15);
		load_i++;
	}
}

// Palabras que necesita cada comando GP0 (0 = no es de dibujo/datos)
static int cmd_len(u32 cmd)
{
	if (cmd >= 0x20 && cmd < 0x40) {
		int nv = (cmd & 8) ? 4 : 3, tex = (cmd & 4) != 0, gour = (cmd & 0x10) != 0;
		return 1 + nv * (1 + tex) + (gour ? nv - 1 : 0);
	}
	if (cmd >= 0x40 && cmd < 0x60) return (cmd & 0x10) ? 4 : 3;
	if (cmd >= 0x60 && cmd < 0x80) {
		int n = 2 + ((cmd & 4) != 0);
		if (((cmd >> 3) & 3) == 0) n++;
		return n;
	}
	if (cmd >= 0x80 && cmd < 0xa0) return 4;
	if (cmd >= 0xa0 && cmd < 0xe0) return 3;
	if (cmd == 0x02) return 3;
	return 1;
}

static void execute_cmd(void)
{
	u32 cmd = fifo[0] >> 24, v = fifo[0];
#ifdef PSX_TRACE_IO
	{ extern u32 gp0_hist[256]; gp0_hist[cmd]++; }
#endif
	if (skip_frame && cmd >= 0x20 && cmd < 0x80 && (cmd < 0x40 || cmd >= 0x60)) return;   // frame saltado: sin polígonos ni sprites
	if (cmd >= 0x20 && cmd < 0x40) { STAT_ADD(gpu_tris, (cmd & 8) ? 2 : 1); polygon(); return; }
	if (cmd >= 0x40 && cmd < 0x60) { STAT_ADD(gpu_lines, 1); line(); return; }
	if (cmd >= 0x60 && cmd < 0x80) { STAT_ADD(gpu_rects, 1); rectangle(); return; }
	if (cmd >= 0x80 && cmd < 0xa0) { STAT_ADD(gpu_copies, 1); vram_copy(); return; }
	if (cmd >= 0xa0 && cmd < 0xc0) {
		load_x = fifo[1] & 0x3ff;
		load_y = (fifo[1] >> 16) & 0x1ff;
		load_w = ((fifo[2] - 1) & 0x3ff) + 1;
		load_h = (((fifo[2] >> 16) - 1) & 0x1ff) + 1;
		load_total = load_w * load_h;
		STAT_ADD(vram_upload, load_total);
		if (gpu_hw) pend_touch(load_x, load_y, load_w, load_h);
#ifdef PSX_GPUSTAT
		{ extern int gpudump; extern int printf(const char *, ...); if (gpudump) printf("LOAD %d,%d %dx%d\n", load_x, load_y, load_w, load_h); }
#endif
		if (gpu_hw) gpu_hw->vram_written(load_x, load_y, load_w, load_h);
		load_i = 0;
		mode = M_LOAD;
		return;
	}
	if (cmd >= 0xc0 && cmd < 0xe0) {
		store_x = fifo[1] & 0x3ff;
		store_y = (fifo[1] >> 16) & 0x1ff;
		store_w = ((fifo[2] - 1) & 0x3ff) + 1;
		store_h = (((fifo[2] >> 16) - 1) & 0x1ff) + 1;
		store_total = store_w * store_h;
		store_i = 0;
		return;
	}
	switch (cmd) {
	case 0x02: STAT_ADD(gpu_fills, 1); fill(); break;
	case 0x1f: g.irq = 1; psx_irq(PSX_IRQ_GPU); break;
	case 0xe1:
		set_texpage(v);
		g.dither = (v >> 9) & 1;
		g.draw_display = (v >> 10) & 1;
		g.flip_x = (v >> 12) & 1;
		g.flip_y = (v >> 13) & 1;
		break;
	case 0xe2: g.tw_mx = v & 0x1f; g.tw_my = (v >> 5) & 0x1f; g.tw_ox = (v >> 10) & 0x1f; g.tw_oy = (v >> 15) & 0x1f; break;
	case 0xe3: g.da_x1 = v & 0x3ff; g.da_y1 = (v >> 10) & 0x1ff; break;
	case 0xe4: g.da_x2 = v & 0x3ff; g.da_y2 = (v >> 10) & 0x1ff; break;
	case 0xe5: g.off_x = sx11(v); g.off_y = sx11(v >> 11); break;
	case 0xe6: g.set_mask = v & 1; g.check_mask = (v >> 1) & 1; break;
	}
}

void gpu_write_gp0(u32 v)
{
	if (mode == M_LOAD) {
		load_pixel((u16)v);
		load_pixel((u16)(v >> 16));
		if (load_i >= load_total) mode = M_CMD;
		return;
	}
	if (mode == M_POLYLINE) { polyline_word(v); return; }
	if (fifo_n == 0) fifo_need = cmd_len(v >> 24);
	fifo[fifo_n++] = v;
	if (fifo_n >= fifo_need) {
		{ STAT_ADD(gpu_cmds, 1); execute_cmd(); }   // el tiempo de la GPU se mide en la DMA (dma.c): medir cada comando costaba mucho
		fifo_n = 0;
	}
}

// Paquete de la lista enlazada (DMA): los comandos que vienen enteros se ejecutan sin pasar palabra a palabra
void gpu_write_gp0_n(const u32 *w, u32 n)
{
	while (n) {
		u32 need;
		if (mode != M_CMD || fifo_n || (need = (u32)cmd_len(w[0] >> 24)) > n) { gpu_write_gp0(*w++); n--; continue; }
		for (u32 i = 0; i < need; i++) fifo[i] = w[i];
		fifo_n = (int)need;
		STAT_ADD(gpu_cmds, 1);
		execute_cmd();
		fifo_n = 0;
		w += need;
		n -= need;
	}
}

void gpu_write_gp1(u32 v)
{
	switch (v >> 24) {
	case 0x00: gpu_reset(); break;
	case 0x01: fifo_n = 0; mode = M_CMD; break;
	case 0x02: g.irq = 0; break;
	case 0x03: g.disp_disable = v & 1; break;
	case 0x04: g.dma_dir = v & 3; break;
	case 0x05:
		if ((v & 0x3fe) != (u32)g.disp_x || ((v >> 10) & 0x1ff) != (u32)g.disp_y) {
			STAT_ADD(disp_flips, 1);
			prev_disp_y = g.disp_y;
			prev_disp_x = g.disp_x;
			if (rows_mode) need_rows = 1;
			pend.on = 0;   // el borrado pendiente ya lo hizo el fondo del 3D
			if (gpu_hw) gpu_hw->flip(!skip_frame);
			gpu_disp.stale = skip_frame;   // se va a ver el buffer que no se dibujó
			vbl_since_flip = 0;
			skip_frame = gpu_frameskip && ++flip_n % (gpu_frameskip + 1) != 0;
		}
		g.disp_x = v & 0x3fe;
		g.disp_y = (v >> 10) & 0x1ff;
		break;
	case 0x06: g.h1 = v & 0xfff; g.h2 = (v >> 12) & 0xfff; break;
	case 0x07: g.v1 = v & 0x3ff; g.v2 = (v >> 10) & 0x3ff; break;
	case 0x08:
		g.hres1 = v & 3;
		g.vres = (v >> 2) & 1;
		g.pal = (v >> 3) & 1;
		g.color24 = (v >> 4) & 1;
		g.interlace = (v >> 5) & 1;
		g.hres2 = (v >> 6) & 1;
		g.reverse = (v >> 7) & 1;
		break;
	case 0x09: g.allow_tex_disable = v & 1; break;
	case 0x10:
		switch (v & 7) {
		case 2: g.info = g.tw_mx | (g.tw_my << 5) | (g.tw_ox << 10) | (g.tw_oy << 15); break;
		case 3: g.info = g.da_x1 | (g.da_y1 << 10); break;
		case 4: g.info = g.da_x2 | (g.da_y2 << 10); break;
		case 5: g.info = (g.off_x & 0x7ff) | ((g.off_y & 0x7ff) << 11); break;
		case 7: g.info = 2; break;
		}
		gpuread = g.info;
		break;
	}
	update_display();
	if (rows_mode && (need_rows || (v >> 24) == 0x08)) {   // cambió el buffer mostrado o la resolución
		rows_update();
		need_rows = 0;
	}
}

u32 gpu_read(void)
{
	if (store_i < store_total) {
		u32 v = 0;
		for (int k = 0; k < 2; k++) {
			if (store_i < store_total) {
				v |= (u32)VRAM(store_x + store_i % store_w, store_y + store_i / store_w) << (k * 16);
				store_i++;
			}
		}
		gpuread = v;
	}
	return gpuread;
}

#ifdef PSX_TRACE_IO
u32 gp0_hist[256];
#endif

// Filas de la VRAM que hay que dibujar si la pantalla de destino tiene out_lines líneas y toma la fila
// y*alto/out_lines; se marcan en los dos buffers que alterna el juego.
// mode 0: todas; 1: solo las que se ven; 2: además solo pares (el frontend repite la de arriba)
static void rows_update(void)
{
	int h = gpu_disp.height, mode = rows_mode, out_lines = rows_lines;
	gpu_rows_on = mode != 0;
	if (!mode) return;
	memset(gpu_row_draw, 0, sizeof gpu_row_draw);
	for (int y = 0; y < out_lines; y++) {
		int r = y * h / out_lines;
		if (mode == 2) r &= ~1;
		gpu_row_draw[(g.disp_y + r) & 511] = 1;
		gpu_row_draw[(prev_disp_y + r) & 511] = 1;
	}
}

void gpu_rows_for(int mode, int out_lines)
{
	rows_mode = mode;
	rows_lines = out_lines;
	rows_update();
}

void gpu_state(PsxState *s)
{
	STATE(s, g);
	psx_state_io(s, psx_vram, 1024 * 512 * 2);
	STATE(s, fifo); STATE(s, fifo_n); STATE(s, fifo_need); STATE(s, mode);
	STATE(s, load_x); STATE(s, load_y); STATE(s, load_w); STATE(s, load_h); STATE(s, load_i); STATE(s, load_total);
	STATE(s, store_x); STATE(s, store_y); STATE(s, store_w); STATE(s, store_h); STATE(s, store_i); STATE(s, store_total);
	STATE(s, gpuread);
	STATE(s, pl_x); STATE(s, pl_y); STATE(s, pl_gour); STATE(s, pl_semi); STATE(s, pl_have_col);
	STATE(s, pl_col); STATE(s, pl_next_col);
	STATE(s, prev_disp_y);
	if (s->load) {
		lut_rgb = ~0u;   // cachés de dibujo
		skip_frame = 0;
		gpu_disp.stale = 0;
		update_display();
		if (rows_mode) rows_update();
	}
}
