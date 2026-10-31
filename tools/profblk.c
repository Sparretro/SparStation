// Muestras por instrucción ARM de un bloque MIPS: profblk muestras.prof codigo.bin base_hex pc_mips_hex
// Escribe out/b/blk.bin (el trozo de código) y una lista «dirección muestras» para cruzar con objdump.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb"), *c = fopen(argv[2], "rb"), *o;
	uint32_t base = (uint32_t)strtoul(argv[3], 0, 16), want = (uint32_t)strtoul(argv[4], 0, 16), hdr[6], pair[2], lo = ~0u, hi = 0;
	static uint32_t cnt[1 << 20];
	long sz;
	uint32_t *code, n;
	if (!f || !c) return 1;
	fseek(c, 0, SEEK_END); sz = ftell(c); fseek(c, 0, SEEK_SET);
	code = malloc(sz); n = (uint32_t)(sz / 4);
	if (fread(code, 4, n, c) != n) return 1;
	if (fread(hdr, 4, 6, f) != 6) return 1;
	while (fread(pair, 4, 2, f) == 2) {
		if (pair[1] != want || pair[0] < base || pair[0] >= base + n * 4) continue;
		cnt[(pair[0] - base) / 4]++;
		if (pair[0] < lo) lo = pair[0];
		if (pair[0] > hi) hi = pair[0];
	}
	if (lo > hi) { printf("sin muestras\n"); return 1; }
	o = fopen("out/b/blk.bin", "wb");
	fwrite(code + (lo - base) / 4, 4, (hi - lo) / 4 + 1, o);
	fclose(o);
	printf("%08x\n", lo);
	for (uint32_t a = lo; a <= hi; a += 4) printf("%08x %u\n", a, cnt[(a - base) / 4]);
	return 0;
}
