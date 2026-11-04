// PDSX en SparStation: la «BIOS» de decoración antes de cada juego (logo de SCE, menú principal, lista de juegos
// en el «CD Player» y logo de PlayStation al arrancar). Pura decoración: la emulación usa OpenBIOS como siempre.
//
// Basado en PDSX de RocketRobz (https://github.com/RocketRobz/PDSX, licencia MIT, ver LICENSE-PDSX): de ahí salen
// las animaciones del logo de SCE y del de PlayStation, el menú principal, el fondo azul con su tramado alterno, los
// gráficos y los sonidos. El dibujo (gl2d) y el texto (la fuente de la consola de libnds) están hechos aquí.
#ifndef PSX_SELFTEST
#include <nds.h>
#include <gl2d.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "pdsx.h"

#include "logo_sce.h"
#include "logo_scetm.h"
#include "logo_ps.h"
#include "logo_playstation.h"
#include "text_scea.h"
#include "bg_blueballs.h"
#include "text_mainmenu.h"
#include "button_inked.h"
#include "text_memandcd.h"
#include "cursor_psx.h"
#include "pdsx_sce_bin.h"
#include "pdsx_ps_bin.h"
#include "pdsx_bgblue_bin.h"

static glImage sce_img[4], scetm_img[1], ps_img[1], playstation_img[2], scea_img[1];
static glImage balls_img[2], mainmenu_img[1], button_img[1], memcd_img[4], cursor_img[1], font_img[128];
static u16 *blue_bg[2];   // fondo del menú: dos tramados que se alternan en cada imagen (como en PDSX)
static int frame_no, bg_mode;   // bg_mode: 0 = negro, 1 = azul
static u16 *bg_vram;

// ---- sonido: los dos jingles en PCM de 8 bits a 16384 Hz, por el canal 0 ----
static void play_pcm(const u8 *data, u32 len)
{
	soundKill(0);
	DC_FlushRange(data, len);
	soundPreparePcm(0 | SOUND_START, 2047, 64, soundTimerFromHz(16384), SoundMode_OneShot, SoundFmt_Pcm8, data, 0, len / 4);
}
static void stop_pcm(void) { soundKill(0); }

// ---- texturas ----
static void load_textures(void)
{
	glResetTextures();
	glLoadTileSet(sce_img, 128, 24, 128, 96, GL_RGB16, TEXTURE_SIZE_128, TEXTURE_SIZE_128,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)logo_scePal, (u8 *)logo_sceBitmap);
	glLoadTileSet(scetm_img, 16, 16, 16, 16, GL_RGB16, TEXTURE_SIZE_16, TEXTURE_SIZE_16,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)logo_scetmPal, (u8 *)logo_scetmBitmap);
	glLoadTileSet(ps_img, 128, 80, 128, 80, GL_RGB256, TEXTURE_SIZE_128, TEXTURE_SIZE_128,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 256, (u16 *)logo_psPal, (u8 *)logo_psBitmap);
	glLoadTileSet(playstation_img, 128, 16, 128, 32, GL_RGB16, TEXTURE_SIZE_128, TEXTURE_SIZE_32,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)logo_playstationPal, (u8 *)logo_playstationBitmap);
	glLoadTileSet(scea_img, 256, 64, 256, 64, GL_RGB16, TEXTURE_SIZE_256, TEXTURE_SIZE_64,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)text_sceaPal, (u8 *)text_sceaBitmap);
	glLoadTileSet(balls_img, 64, 64, 64, 128, GL_RGB16, TEXTURE_SIZE_64, TEXTURE_SIZE_128,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)bg_blueballsPal, (u8 *)bg_blueballsBitmap);
	glLoadTileSet(button_img, 128, 32, 128, 32, GL_RGB256, TEXTURE_SIZE_128, TEXTURE_SIZE_32,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 256, (u16 *)button_inkedPal, (u8 *)button_inkedBitmap);
	glLoadTileSet(mainmenu_img, 128, 32, 128, 32, GL_RGB16, TEXTURE_SIZE_128, TEXTURE_SIZE_32,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)text_mainmenuPal, (u8 *)text_mainmenuBitmap);
	glLoadTileSet(memcd_img, 128, 16, 128, 64, GL_RGB16, TEXTURE_SIZE_128, TEXTURE_SIZE_64,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)text_memandcdPal, (u8 *)text_memandcdBitmap);
	glLoadTileSet(cursor_img, 16, 16, 16, 16, GL_RGB16, TEXTURE_SIZE_16, TEXTURE_SIZE_16,
	              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)cursor_psxPal, (u8 *)cursor_psxBitmap);
	{   // fuente: la de la consola de libnds (8x8), en una textura de 128x64 con los caracteres 0-127
		static u8 tex[128 * 64 / 2] __attribute__((aligned(4)));
		static const u16 pal[16] = { 0, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff,
		                             0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff };
		const ConsoleFont *f = &consoleGetDefault()->font;
		const u8 *g = (const u8 *)f->gfx;
		memset(tex, 0, sizeof tex);
		for (int c = 0; c < 128; c++) {
			int k = c - f->asciiOffset;
			if (!g || k < 0 || k >= f->numChars) continue;
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++) {
					int on;
					if (f->bpp == 8) on = g[k * 64 + y * 8 + x] != 0;
					else if (f->bpp == 1) on = (g[k * 8 + y] >> x) & 1;
					else on = (g[k * 32 + y * 4 + x / 2] >> ((x & 1) * 4)) & 15;
					if (on) {
						int tx = (c & 15) * 8 + x, ty = (c >> 4) * 8 + y, i = ty * 128 + tx;
						tex[i >> 1] |= (u8)(1 << ((i & 1) * 4));
					}
				}
		}
		glLoadTileSet(font_img, 8, 8, 128, 64, GL_RGB16, TEXTURE_SIZE_128, TEXTURE_SIZE_64,
		              GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | TEXGEN_OFF | GL_TEXTURE_COLOR0_TRANSPARENT, 16, (u16 *)pal, tex);
	}
}

// Texto con la fuente de 8x8 (escala en 1/4096), con sombra
static void text(int x, int y, const char *s, int max, u16 color, int scale)
{
	int step = (8 * scale) >> 12;
	for (int pass = 0; pass < 2; pass++) {
		int cx = x + (pass ? 0 : 1), cy = y + (pass ? 0 : 1);
		glColor(pass ? color : RGB15(2, 2, 6));
		for (int i = 0; s[i] && i < max; i++, cx += step) {
			unsigned char c = (unsigned char)s[i];
			if (c >= 128) c = '?';
			if (c == ' ') continue;
			if (scale == 4096) glSprite(cx, cy, GL_FLIP_NONE, &font_img[c]);
			else glSpriteScale(cx, cy, scale, GL_FLIP_NONE, &font_img[c]);
		}
	}
	glColor(RGB15(31, 31, 31));
}

// ---- fondo azul del menú (psxMenuInit de PDSX, desde el PNG ya pasado a RGB: 128x192, la otra mitad en espejo) ----
static void make_blue_bg(void)
{
	const u8 *src = pdsx_bgblue_bin;
	int x = 0, x2 = 255, y = 0, alt = 0;
	for (int i = 0; i < 128 * 192; i++) {
		int c[3] = { src[i * 3], src[i * 3 + 1], src[i * 3 + 2] }, carry = 0;
		if (alt)
			for (int k = 0; k < 3; k++) if (c[k] >= 4) { c[k] -= 4; carry |= 1 << k; }
		blue_bg[0][y * 256 + x] = (u16)(c[0] >> 3 | (c[1] >> 3) << 5 | (c[2] >> 3) << 10 | BIT(15));
		blue_bg[0][y * 256 + x2] = blue_bg[0][y * 256 + x];
		if (alt) { for (int k = 0; k < 3; k++) if (carry & (1 << k)) c[k] += 4; }
		else for (int k = 0; k < 3; k++) if (c[k] >= 4) c[k] -= 4;
		blue_bg[1][y * 256 + x] = (u16)(c[0] >> 3 | (c[1] >> 3) << 5 | (c[2] >> 3) << 10 | BIT(15));
		blue_bg[1][y * 256 + x2] = blue_bg[1][y * 256 + x];
		if ((i % 128) == 127) alt = !alt;
		alt = !alt;
		x++;
		x2--;
		if (x == 128) { x = 0; x2 = 255; y++; }
	}
}

// Una imagen: fondo (BG3) y lo que haya dibujado draw() con el 3D
static void frame_end(void)
{
	glEnd2D();
	glFlush(0);
	swiWaitForVBlank();
	if (bg_mode == 1 && blue_bg[0]) dmaCopyWords(3, blue_bg[frame_no & 1], bg_vram, 256 * 192 * 2);
	frame_no++;
	scanKeys();
}

static void fill_black(void) { dmaFillHalfWords(0x8000, bg_vram, 256 * 192 * 2); }

// ---- arranque y final ----
void pdsx_start(void)
{
	videoSetMode(MODE_5_3D | DISPLAY_BG3_ACTIVE);
	vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
	vramSetBankB(VRAM_B_TEXTURE);
	vramSetBankF(VRAM_F_TEX_PALETTE);
	{
		int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
		bgSetPriority(bg, 1);
		bg_vram = bgGetGfxPtr(bg);
	}
	glScreen2D();
	glClearColor(0, 0, 0, 0);   // el 3D transparente: detrás se ve el BG3
	glDisable(GL_CLEAR_BMP);
	glEnable(GL_BLEND);
	REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG0 | BLEND_DST_BG3 | BLEND_DST_BACKDROP;   // lo translúcido del 3D, sobre el fondo
	load_textures();
	blue_bg[0] = malloc(256 * 192 * 2);
	blue_bg[1] = malloc(256 * 192 * 2);
	if (blue_bg[0] && blue_bg[1]) make_blue_bg();
	else { free(blue_bg[0]); free(blue_bg[1]); blue_bg[0] = blue_bg[1] = NULL; }
	fill_black();
	bg_mode = 0;
	setBrightness(1, 0);
}

void pdsx_stop(void)
{
	for (int i = 0; i <= 16; i++) { setBrightness(1, -i); swiWaitForVBlank(); }
	stop_pcm();
	glResetTextures();
	free(blue_bg[0]);
	free(blue_bg[1]);
	blue_bg[0] = blue_bg[1] = NULL;
	// lo que espera SparStation: modo de mapa de bits en los bancos A y B
	videoSetMode(MODE_5_2D);
	REG_BLDCNT = 0;
	vramSetBankF(VRAM_F_LCD);
	vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
	vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
	bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	dmaFillHalfWords(0x8000, (void *)0x06000000, 2 * 256 * 256 * 2);
	setBrightness(1, 0);
}

// ---- logo de Sony Computer Entertainment (sceSplash de PDSX) ----
static void sce_splash(void)
{
	int fade0 = 0, fade1 = 0, fadedin = 0, bgc = 0;
	int t1x[2] = { 77, 127 }, t1y[3] = { 95, 45, 145 }, t2x[2] = { 177, 127 }, t2y[3] = { 95, 45, 145 };
	int zd[5] = { 0 }, zd3add = 0, formed = 0;
	bg_mode = 0;
	fill_black();
	play_pcm(pdsx_sce_bin, pdsx_sce_bin_size);
	for (int loop = 0; loop < 60 * 6; loop++) {
		if (!fadedin) {
			if (fade0 == 10) { if (fade1 == 60) fadedin = 1; else { fade1++; bgc += 3; } }
			else fade0++;
		} else {
			bgc = 185;
			if (++zd[0] == 2) { zd[0] = 0; if (t1x[0] != 109) t1x[0]++; if (t2x[0] != 146) t2x[0]--; }
			if (++zd[1] == 10) { zd[1] = 0; if (t1x[1] != 131) t1x[1]++; if (t2x[1] != 123) t2x[1]--; }
			if (++zd[2] == 3) { zd[2] = 0; if (t1y[0] != 76) t1y[0]--; if (t2y[0] != 113) t2y[0]++; }
			if (++zd[3] == 1 + zd3add) {
				zd[3] = 0;
				zd3add = !zd3add;
				if (t1y[2] != 98) t1y[2]--;
				if (t2y[1] != 90) t2y[1]++; else formed = 1;
			}
			if (++zd[4] == 5) { zd[4] = 0; if (t1y[1] != 54) t1y[1]++; if (t2y[2] != 136) t2y[2]--; }
		}
		glBegin2D();
		glBoxFilled(0, 0, 256, 192, RGB15(bgc / 8, bgc / 8, bgc / 8));
		if (fadedin) {
			u16 a = RGB15(184 / 8, 14 / 8, 9 / 8), b = RGB15(189 / 8, 144 / 8, 0);
			glTriangleFilledGradient(77, 95, 127, 45, 127, 145, a, b, b);
			glTriangleFilledGradient(177, 95, 127, 45, 127, 145, a, b, b);
			glTriangleFilledGradient(t2x[0], t2y[0], t2x[1], t2y[1], t2x[1], t2y[2], a, b, b);
			glTriangleFilledGradient(t1x[0], t1y[0], t1x[1], t1y[1], t1x[1], t1y[2], a, b, b);
			if (formed) {
				glSprite(80, 23, GL_FLIP_NONE, &sce_img[0]);
				glSprite(80, 154, GL_FLIP_NONE, &sce_img[1]);
				glSprite(136, 144, GL_FLIP_NONE, scetm_img);
			}
		}
		glColor(RGB15(31, 31, 31));
		frame_end();
		if (loop > 60 && (keysDown() & (KEY_START | KEY_A))) break;   // se puede saltar
	}
}

// ---- menú principal (psxMenu de PDSX): 0 = Memory Card, 1 = CD Player ----
static int main_menu(void (*info)(int))
{
	int cursor = 1, fading = 0, fadec = 255, shown = -1;
	bg_mode = 1;
	for (int i = 0;; i++) {
		if (i == 30) stop_pcm();   // como PDSX: la música del logo se corta al entrar al menú
		if (shown != cursor) { shown = cursor; if (info) info(cursor); }
		if (fading) {
			fadec -= 10;
			if (fadec < 0) {
				fadec = 255;
				fading = 0;
				if (cursor == 1) return 1;
			}
		} else {
			u32 k = keysDown();
			if ((k & KEY_UP) && cursor > 0) cursor--;
			if ((k & KEY_DOWN) && cursor < 1) cursor++;
			if (k & KEY_A) fading = 1;
		}
		glBegin2D();
		glSprite(39, 6, GL_FLIP_NONE, &balls_img[0]);
		glSprite(83, 128, GL_FLIP_NONE, &balls_img[1]);
		glSprite(181, 17, GL_FLIP_NONE, mainmenu_img);
		glSprite(22, 79, GL_FLIP_NONE, button_img);
		glSprite(22, 119, GL_FLIP_NONE, button_img);
		{
			u16 fc = RGB15(fadec / 8, fadec / 8, fadec / 8);
			if (cursor == 0) {
				glColor(fc); glSprite(10, 87, GL_FLIP_NONE, &memcd_img[0]);
				glColor(RGB15(31, 31, 31)); glSprite(26, 127, GL_FLIP_NONE, &memcd_img[3]);
				if (!fading) glSprite(66, 94, GL_FLIP_NONE, cursor_img);
			} else {
				glSprite(10, 87, GL_FLIP_NONE, &memcd_img[1]);
				glColor(fc); glSprite(26, 127, GL_FLIP_NONE, &memcd_img[2]);
				glColor(RGB15(31, 31, 31));
				if (!fading) glSprite(66, 135, GL_FLIP_NONE, cursor_img);
			}
		}
		glColor(RGB15(31, 31, 31));
		frame_end();
	}
}

void pdsx_intro(void (*info)(int))
{
	sce_splash();
	main_menu(info);
}

// Nombre para enseñar: sin la extensión ni las etiquetas de región ([NTSC-U], (USA), (PS1)...), pero con el disco
static void clean_name(const char *s, char *out, int max)
{
	int n = 0, depth = 0, keep = 0;
	const char *dot = strrchr(s, '.'), *end = dot ? dot : s + strlen(s);
	for (const char *p = s; p < end && n < max - 1; p++) {
		if ((*p == '(' || *p == '[') && !depth) {
			keep = !strncasecmp(p + 1, "disc", 4) || !strncasecmp(p + 1, "disco", 5);   // (Disc 1) se queda
			if (!keep) { depth = 1; continue; }
		}
		if (depth) { if (*p == ')' || *p == ']') depth = 0; continue; }
		if (*p == ' ' && (n == 0 || out[n - 1] == ' ')) continue;   // sin espacios dobles ni al principio
		out[n++] = *p;
	}
	while (n && out[n - 1] == ' ') n--;
	if (!n) {   // todo eran etiquetas: el nombre tal cual
		n = (int)(end - s) < max - 1 ? (int)(end - s) : max - 1;
		memcpy(out, s, (size_t)n);
	}
	out[n] = 0;
}

// ---- lista de juegos, al estilo del menú: una franja por juego, el trazo de tinta y la mano en el elegido ----
void pdsx_list_frame(const char (*names)[128], int n, int sel)
{
	enum { ROWS = 4, Y0 = 44, DY = 36, MAXC = 27 };
	int top = sel - 1;
	bg_mode = 1;
	if (top > n - ROWS) top = n - ROWS;
	if (top < 0) top = 0;
	glBegin2D();
	glSprite(39, 6, GL_FLIP_NONE, &balls_img[0]);
	glSprite(150, 128, GL_FLIP_NONE, &balls_img[1]);
	glSprite(8, 10, GL_FLIP_NONE, &memcd_img[2]);   // «CD Player» de título
	{
		char cnt[16];
		int len;
		snprintf(cnt, sizeof cnt, "%d/%d", sel + 1, n);
		len = (int)strlen(cnt);
		text(248 - len * 8, 14, cnt, 8, RGB15(20, 24, 31), 4096);
	}
	for (int r = 0; r < ROWS && top + r < n; r++) {
		int i = top + r, y = Y0 + r * DY, len;
		char name[64];
		clean_name(names[i], name, sizeof name);
		len = (int)strlen(name);
		if (len > MAXC) { name[MAXC - 2] = '.'; name[MAXC - 1] = '.'; name[MAXC] = 0; }
		glPolyFmt(POLY_ALPHA(i == sel ? 20 : 12) | POLY_CULL_NONE | POLY_ID(2 + r));   // franja translúcida
		glBoxFilled(22, y + 4, 250, y + 27, i == sel ? RGB15(4, 6, 16) : RGB15(1, 2, 8));
		glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
		if (i == sel) glSprite(18, y, GL_FLIP_NONE, button_img);   // el trazo de tinta de PDSX, a su tamaño
		text(30, y + 12, name, MAXC, i == sel ? RGB15(31, 31, 31) : RGB15(18, 20, 26), 4096);
		if (i == sel) glSprite(2 + ((frame_no >> 3) & 1), y + 8, GL_FLIP_NONE, cursor_img);
	}
	glColor(RGB15(31, 31, 31));
	frame_end();
}

// ---- logo de PlayStation al arrancar el juego (psxSplash de PDSX) ----
void pdsx_boot_logo(void)
{
	int logo = 0, txt = 0;
	bg_mode = 0;
	glBegin2D();
	glEnd2D();
	glFlush(0);
	swiWaitForVBlank();
	fill_black();
	for (int i = 0; i < 20; i++) { glBegin2D(); frame_end(); }   // un momento en negro, como al leer el disco
	play_pcm(pdsx_ps_bin, pdsx_ps_bin_size);
	for (int t = 0; t < 60 * 5; t++) {
		logo += 8; if (logo > 255) logo = 255;
		if (logo == 255) { txt += 8; if (txt > 255) txt = 255; }
		glBegin2D();
		glColor(RGB15(logo / 8, logo / 8, logo / 8));
		glSprite(85, 25, GL_FLIP_NONE, ps_img);
		if (logo == 255) {
			glColor(RGB15(txt / 8, txt / 8, txt / 8));
			glSprite(91, 109, GL_FLIP_NONE, &playstation_img[0]);
			glColor(RGB15(31, 31, 31));
			glSprite(0, 128, GL_FLIP_NONE, scea_img);
		}
		glColor(RGB15(31, 31, 31));
		frame_end();
		if (t > 60 && (keysDown() & (KEY_START | KEY_A))) break;
	}
}
#endif
