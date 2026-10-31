// Clasifica las muestras que caen en el código traducido según la instrucción ARM que se ejecutaba.
// uso: profops muestras.prof codigo.bin base_hex   (codigo.bin = DSB_CODE de dsbench, base = su dirección)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb"), *c = fopen(argv[2], "rb");
	uint32_t base = (uint32_t)strtoul(argv[3], 0, 16), hdr[6], pair[2], *code, n, total = 0, k[16] = { 0 };
	static const char *nm[16] = { "ldr/str [r11] (regs MIPS, cpu)", "ldr/str [r10..] (RAM PS1)", "otros ldr/str", "ldrh/strh/ldrsb...", "ALU", "MUL", "salto B/BL", "ldm/stm", "condicional no ejecutable?", "otro" };
	long sz;
	if (!f || !c) return 1;
	fseek(c, 0, SEEK_END); sz = ftell(c); fseek(c, 0, SEEK_SET);
	code = malloc(sz); n = (uint32_t)(sz / 4);
	if (fread(code, 4, n, c) != n) return 1;
	if (fread(hdr, 4, 6, f) != 6) return 1;   // cabecera de dsbench: zonas del dynarec
	while (fread(pair, 4, 2, f) == 2) {
		uint32_t a = pair[0], w, cls;
		if ((pair[0] >> 16) == 0xfffe || (pair[0] >> 16) == 0xffff) continue;
		if (a < base || a >= base + n * 4) continue;
		w = code[(a - base) / 4];
		if (((w >> 25) & 7) == 5) cls = 6;
		else if (((w >> 25) & 7) == 4) cls = 7;
		else if (((w >> 26) & 3) == 1) cls = ((w >> 16) & 15) == 11 ? 0 : ((w >> 16) & 15) == 10 ? 1 : 2;
		else if (((w >> 25) & 7) == 0 && (w & 0x90) == 0x90 && ((w >> 5) & 3)) cls = 3;
		else if (((w >> 25) & 7) == 0 && (w & 0xf0) == 0x90) cls = 5;
		else if (((w >> 26) & 3) == 0) cls = 4;
		else cls = 9;
		k[cls]++;
		total++;
	}
	for (int i = 0; i < 10; i++) if (k[i]) printf("%6.1f%%  %s\n", k[i] * 100.0 / total, nm[i]);
	printf("%u muestras en el código traducido\n", total);
	return 0;
}
