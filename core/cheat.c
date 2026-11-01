// Parches por juego con códigos GameShark / Action Replay de PS1 (solución 10 de la lista).
// Se cargan de un archivo de texto (Juego.cht junto al Juego.cue) y se aplican en cada vblank, como
// hacía el cartucho. Líneas: «AAAAAAAA VVVV» (8+4 cifras hexadecimales); lo demás es comentario.
// Una línea que empieza por «#» o «;» es comentario; «[nombre]» empieza un grupo (solo informativo).
//
// Tipos soportados:
//   80aaaaaa vvvv  escribe 16 bits          30aaaaaa 00vv  escribe 8 bits
//   10aaaaaa vvvv  suma a 16 bits           11aaaaaa vvvv  resta a 16 bits
//   20aaaaaa 00vv  suma a 8 bits            21aaaaaa 00vv  resta a 8 bits
//   D0aaaaaa vvvv  si 16 bits == v, aplica la siguiente     E0aaaaaa 00vv  ídem con 8 bits
//   D1 / E1 si !=,  D2 / E2 si <,  D3 / E3 si >
//   D4000000 vvvv  si los botones pulsados (máscara GameShark) == v, aplica la siguiente
//   50ccssii dddd + código siguiente: repite el siguiente cc veces, sumando ss*2 a la dirección (en
//                  realidad ss es el paso en bytes) y dddd al valor
//   C0aaaaaa vvvv  si 16 bits == v, activa el resto de códigos (si no, no se aplica ninguno más)
//   C1000000 nnnn  retrasa los códigos nnnn vblanks desde el arranque
// Escribir sobre código ya traducido lo invalida (los parches de instrucciones funcionan).
#include <string.h>
#include "psx.h"

#define CHEAT_MAX 512
static u32 ch_addr[CHEAT_MAX];
static u16 ch_val[CHEAT_MAX];
static int ch_n;
static u32 ch_frames;
int cheat_count(void) { return ch_n; }

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

void cheat_clear(void) { ch_n = 0; ch_frames = 0; }

// Añade los códigos de un texto (el contenido del .cht). Devuelve cuántos códigos se leyeron.
int cheat_parse(const char *t)
{
	int added = 0;
	while (*t) {
		const char *e = t;
		while (*e && *e != '\n') e++;
		// una línea vale si empieza (tras espacios) por 8 cifras, un separador y 4 cifras
		const char *p = t;
		while (p < e && (*p == ' ' || *p == '\t')) p++;
		if (p < e && *p != '#' && *p != ';' && *p != '[') {
			u32 a = 0, v = 0;
			int i, ok = 1;
			for (i = 0; i < 8 && ok; i++) { int h = p + i < e ? hexval(p[i]) : -1; if (h < 0) ok = 0; else a = a << 4 | (u32)h; }
			if (ok) {
				const char *q = p + 8;
				while (q < e && (*q == ' ' || *q == '\t' || *q == '-' || *q == ':')) q++;
				for (i = 0; i < 4 && ok; i++) { int h = q + i < e ? hexval(q[i]) : -1; if (h < 0) ok = 0; else v = v << 4 | (u32)h; }
				if (ok && q + 4 < e && hexval(q[4]) >= 0) ok = 0;   // más de 4 cifras: no es un código
			}
			if (ok && ch_n < CHEAT_MAX) { ch_addr[ch_n] = a; ch_val[ch_n] = (u16)v; ch_n++; added++; }
		}
		t = *e ? e + 1 : e;
	}
	return added;
}

static u8 *mem8(u32 a) { return psx_ram + (a & 0x1fffff); }

static u16 rd16(u32 a) { u8 *m = mem8(a & ~1u); return (u16)(m[0] | m[1] << 8); }
static u8 rd8(u32 a) { return *mem8(a); }
static void wr16(u32 a, u16 v)
{
	u8 *m = mem8(a & ~1u);
	if ((u16)(m[0] | m[1] << 8) == v) return;   // sin cambios: no invalidar nada
	m[0] = (u8)v; m[1] = (u8)(v >> 8);
	DYN_CHECK_WRITE((u32)(m - psx_ram));
}
static void wr8(u32 a, u8 v)
{
	u8 *m = mem8(a);
	if (*m == v) return;
	*m = v;
	DYN_CHECK_WRITE((u32)(m - psx_ram));
}

// Botones de la PS1 (activos a 0 en psx_pad_buttons) -> máscara de los códigos D4 (activos a 1)
static u16 pad_mask(void) { return (u16)~psx_pad_buttons; }

static int cond(u32 type, u32 a, u16 v)
{
	switch (type) {
	case 0xd0: return rd16(a) == v;
	case 0xd1: return rd16(a) != v;
	case 0xd2: return rd16(a) < v;
	case 0xd3: return rd16(a) > v;
	case 0xe0: return rd8(a) == (u8)v;
	case 0xe1: return rd8(a) != (u8)v;
	case 0xe2: return rd8(a) < (u8)v;
	case 0xe3: return rd8(a) > (u8)v;
	case 0xd4: return pad_mask() == v;
	}
	return 1;
}

static void apply_one(u32 type, u32 a, u16 v)
{
	switch (type) {
	case 0x80: wr16(a, v); break;
	case 0x30: wr8(a, (u8)v); break;
	case 0x10: wr16(a, (u16)(rd16(a) + v)); break;
	case 0x11: wr16(a, (u16)(rd16(a) - v)); break;
	case 0x20: wr8(a, (u8)(rd8(a) + v)); break;
	case 0x21: wr8(a, (u8)(rd8(a) - v)); break;
	}
}

// En cada vblank
void cheat_apply(void)
{
	ch_frames++;
	for (int i = 0; i < ch_n; i++) {
		u32 type = ch_addr[i] >> 24, a = ch_addr[i] & 0xffffff;
		u16 v = ch_val[i];
		if (type == 0xc1) { if (ch_frames < v) return; continue; }
		if (type == 0xc0) { if (rd16(a) != v) return; continue; }
		if ((type & 0xf0) == 0xd0 || (type & 0xf0) == 0xe0) {
			if (!cond(type, a, v)) i++;   // salta el código siguiente
			continue;
		}
		if (type == 0x50 && i + 1 < ch_n) {
			u32 cnt = (ch_addr[i] >> 8) & 0xff, step = ch_addr[i] & 0xff, nt = ch_addr[i + 1] >> 24, na = ch_addr[i + 1] & 0xffffff;
			u16 nv = ch_val[i + 1];
			for (u32 k = 0; k < cnt; k++, na += step, nv = (u16)(nv + v)) apply_one(nt, na, nv);
			i++;
			continue;
		}
		apply_one(type, a, v);
	}
}
