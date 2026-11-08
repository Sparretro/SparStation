// Idiomas: el texto se elige con T(es, en, ru) (lang.h) y va en UTF-8 en el código. La consola de libnds pinta un
// byte por letra con su fuente de 256 (página 437, que ya trae á é í ó ú ñ ¿ ¡ ü); el cirílico está dibujado en la
// zona de cajas (font.c, de tools/mkfont.py). Aquí se cambia la fuente y se envuelve la salida de la consola para
// pasar el UTF-8 a esos códigos, así que todos los printf sirven tal cual.
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include "lang.h"

#ifndef LANG_DEFAULT
#define LANG_DEFAULT LANG_ES   // pruebas: EXTRA=-DLANG_DEFAULT=2 para ver el ruso en melonDS
#endif
int lang = LANG_DEFAULT;
extern const u8 spar_font[2048];

const char *lang_name(int l)
{
	static const char *const n[LANG_N] = { "Español", "English", "Русский" };
	return n[l >= 0 && l < LANG_N ? l : 0];
}

// Un carácter Unicode a su código en la fuente (0 = no lo hay)
static int glyph(u32 cp)
{
	if (cp < 0x80) return (int)cp;
	if (cp >= 0x410 && cp <= 0x44f) return 0xb0 + (int)(cp - 0x410);   // А..я
	switch (cp) {
	case 0x401: return 0xf0;   // Ё
	case 0x451: return 0xf1;   // ё
	case 0xe1: return 0xa0;    // á
	case 0xe9: return 0x82;    // é
	case 0xed: return 0xa1;    // í
	case 0xf3: return 0xa2;    // ó
	case 0xfa: return 0xa3;    // ú
	case 0xf1: return 0xa4;    // ñ
	case 0xd1: return 0xa5;    // Ñ
	case 0xfc: return 0x81;    // ü
	case 0xdc: return 0x9a;    // Ü
	case 0xc9: return 0x90;    // É
	case 0xc1: return 'A';     // sin mayúsculas con tilde en la fuente
	case 0xcd: return 'I';
	case 0xd3: return 'O';
	case 0xda: return 'U';
	case 0xbf: return 0xa8;    // ¿
	case 0xa1: return 0xad;    // ¡
	case 0xab: return 0xae;    // «
	case 0xbb: return 0xaf;    // »
	case 0xba: return 0xa7;    // º
	case 0xaa: return 0xa6;    // ª
	case 0xb0: return 0xf8;    // °
	case 0x2026: return '.';   // …
	case 0x2014: case 0x2013: return '-';
	case 0x25c0: return '<';   // ◀ ▶
	case 0x25b6: return '>';
	}
	return '?';
}

// Decodificador con estado (un carácter puede llegar partido entre dos escrituras)
typedef struct { u32 cp; int need; } Utf8;

static int utf8_step(Utf8 *u, u8 b)   // devuelve el código de la fuente, o -1 si falta algún byte
{
	if (u->need && (b & 0xc0) == 0x80) {
		u->cp = (u->cp << 6) | (b & 0x3f);
		if (--u->need) return -1;
		return glyph(u->cp);
	}
	u->need = 0;
	if (b < 0x80) return b;
	if ((b & 0xe0) == 0xc0) { u->cp = b & 0x1f; u->need = 1; return -1; }
	if ((b & 0xf0) == 0xe0) { u->cp = b & 0x0f; u->need = 2; return -1; }
	if ((b & 0xf8) == 0xf0) { u->cp = b & 0x07; u->need = 3; return -1; }
	return '?';
}

int lang_to_font(const char *in, char *out, int max)
{
	Utf8 u = { 0, 0 };
	int n = 0;
	for (; *in && n < max - 1; in++) {
		int c = utf8_step(&u, (u8)*in);
		if (c >= 0) out[n++] = (char)c;
	}
	out[n] = 0;
	return n;
}

// ---- la consola ----
static devoptab_t con_utf8;
static ssize_t (*con_write)(struct _reent *r, void *fd, const char *ptr, size_t len);
static Utf8 con_state;

static ssize_t write_utf8(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	char buf[128];
	size_t i = 0;
	while (i < len) {
		int n = 0;
		while (i < len && n < (int)sizeof buf) {
			int c = utf8_step(&con_state, (u8)ptr[i++]);
			if (c >= 0) buf[n++] = (char)c;
		}
		if (n) con_write(r, fd, buf, (size_t)n);
	}
	return (ssize_t)len;
}

void lang_console_init(void)
{
	PrintConsole *pc = consoleGetDefault();
	ConsoleFont f = pc->font;
	f.gfx = (u16 *)spar_font;
	f.bpp = 1;
	f.asciiOffset = 0;
	f.numChars = 256;
	f.convertSingleColor = true;
	consoleSetFont(NULL, &f);
	if (!con_write && devoptab_list[STD_OUT] && devoptab_list[STD_OUT]->write_r) {
		con_utf8 = *devoptab_list[STD_OUT];
		con_write = con_utf8.write_r;
		con_utf8.write_r = write_utf8;
		devoptab_list[STD_OUT] = &con_utf8;
		if (devoptab_list[STD_ERR] && devoptab_list[STD_ERR]->write_r == con_write) devoptab_list[STD_ERR] = &con_utf8;
	}
}
