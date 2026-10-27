// Informe del perfilador: cruza las muestras (pares dirección ARM, PC MIPS) con los símbolos del .elf.
// uso: profrep <muestras.bin> <símbolos.txt de «arm-none-eabi-nm -n --defined-only»> [n]
// La cabecera de dsbench (DSB_PROF) son 6 palabras: zonas del dynarec (ver dyn_regions).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint32_t addr; char name[64]; uint32_t n; } Sym;
typedef struct { uint32_t pc, n; } Blk;

static Sym *syms;
static int nsyms;

static int by_n_sym(const void *a, const void *b) { return (int)((const Sym *)b)->n - (int)((const Sym *)a)->n; }
static int by_n_blk(const void *a, const void *b) { return (int)((const Blk *)b)->n - (int)((const Blk *)a)->n; }

static Sym *find(uint32_t a)
{
	int lo = 0, hi = nsyms;
	if (!nsyms || a < syms[0].addr) return NULL;
	while (hi - lo > 1) {
		int m = (lo + hi) / 2;
		if (syms[m].addr <= a) lo = m; else hi = m;
	}
	return &syms[lo];
}

int main(int argc, char **argv)
{
	FILE *f, *sf;
	uint32_t reg[6], pair[2], total = 0, cat[8] = { 0 };
	static const char *cn[8] = { "bloques traducidos", "zona fria", "trampolines", "rutinas de memoria", "stubs", "BIOS", "funciones C", "?" };
	static Blk blk[65536];
	int nblk = 0, top = argc > 3 ? atoi(argv[3]) : 30;
	char line[512];
	if (argc < 3) { fprintf(stderr, "uso: profrep muestras.bin simbolos.txt [n]\n"); return 1; }
	sf = fopen(argv[2], "r");
	if (!sf) return 1;
	syms = calloc(200000, sizeof(Sym));
	while (fgets(line, sizeof line, sf) && nsyms < 200000) {
		unsigned a;
		char t, name[256];
		if (sscanf(line, "%x %c %255s", &a, &t, name) != 3) continue;
		if (t != 'T' && t != 't' && t != 'W' && t != 'w') continue;
		if (name[0] == '$') continue;
		syms[nsyms].addr = a;
		snprintf(syms[nsyms].name, 64, "%s", name);
		nsyms++;
	}
	fclose(sf);
	f = fopen(argv[1], "rb");
	if (!f || fread(reg, 4, 6, f) != 6) return 1;
	while (fread(pair, 4, 2, f) == 2) {
		uint32_t a = pair[0];
		int c;
		total++;
		if (a >= reg[3] && a < reg[4]) c = 0;
		else if (a >= reg[4] && a < reg[5]) c = 1;
		else if (a >= reg[1] && a < reg[2]) c = 2;
		else if (a >= reg[2] && a < reg[3]) c = 3;
		else if (a >= reg[0] && a < reg[1]) c = 4;
		else if (a >= 0xffff0000u) c = 5;
		else {
			Sym *s = find(a);
			c = s ? 6 : 7;
			if (s) s->n++;
		}
		cat[c]++;
		if (c == 0 && pair[1]) {
			int i;
			for (i = 0; i < nblk && blk[i].pc != pair[1]; i++) { }
			if (i == nblk && nblk < 65536) { blk[nblk].pc = pair[1]; blk[nblk++].n = 0; }
			if (i < 65536) blk[i].n++;
		}
	}
	fclose(f);
	if (!total) { printf("sin muestras\n"); return 0; }
	printf("%u muestras\n", total);
	for (int c = 0; c < 8; c++) if (cat[c]) printf("  %-20s %5.1f%%\n", cn[c], cat[c] * 100.0 / total);
	qsort(syms, nsyms, sizeof(Sym), by_n_sym);
	printf("funciones:\n");
	for (int i = 0; i < top && i < nsyms && syms[i].n; i++) printf("  %5.1f%%  %s\n", syms[i].n * 100.0 / total, syms[i].name);
	qsort(blk, nblk, sizeof(Blk), by_n_blk);
	printf("bloques MIPS (%d distintos):\n", nblk);
	for (int i = 0; i < 15 && i < nblk; i++) printf("  %5.1f%%  %08x\n", blk[i].n * 100.0 / total, blk[i].pc);
	return 0;
}
