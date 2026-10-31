// Prueba de instrucciones en crudo: ejecuta las funciones de rawfuzz.c y deja en RAM (0x80180000: "FZOK", cuántas,
// y por función su búfer entero: 64 palabras de memoria + 34 registros) para comparar el intérprete con el dynarec.
#include "fuzz.h"
void puts_(const char *s);
void puthex(u32 v);
extern u32 raw_code[][160];
extern const int raw_count;
#define BW (64 + 34)

static u32 buf[BW] __attribute__((aligned(8)));

int main(void)
{
	volatile u32 *res = (volatile u32 *)0x80180000;
	u32 total = 0, x = 12345;
	res[0] = 0;
	for (int f = 0; f < raw_count; f++) {
		void (*fn)(u32 *) = (void (*)(u32 *))(void *)raw_code[f];
		for (int i = 0; i < BW; i++) {
			x = x * 1103515245u + 12345u;
			buf[i] = (i & 3) == 0 ? x >> 16 : (i & 3) == 1 ? 0x80000000u ^ x : x;
		}
		fn(buf);
		for (int i = 0; i < BW; i++) { res[2 + f * BW + i] = buf[i]; total = total * 31 + buf[i]; }
	}
	res[1] = raw_count;
	res[0] = 0x4b4f5a46;   // "FZOK"
	puts_("RAW ");
	puthex(total);
	puts_("\n");
	return 0;
}
