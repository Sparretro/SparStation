// Convierte un ELF de MIPS (little endian, enlazado en KSEG0) en un PS-X EXE.
// uso: elf2exe <entrada.elf> <salida.exe>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *e, *img;
	long n;
	uint32_t lo = ~0u, hi = 0, phoff, phnum, phsz;
	if (argc < 3) return 1;
	f = fopen(argv[1], "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	e = malloc(n);
	if (fread(e, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	phoff = r32(e + 28);
	phsz = r16(e + 42);
	phnum = r16(e + 44);
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = e + phoff + i * phsz;
		if (r32(ph) != 1 || !r32(ph + 20)) continue;
		if (r32(ph + 8) < lo) lo = r32(ph + 8);
		if (r32(ph + 8) + r32(ph + 20) > hi) hi = r32(ph + 8) + r32(ph + 20);
	}
	hi = (hi + 0x7ff) & ~0x7ffu;
	img = calloc(1, 0x800 + hi - lo);
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = e + phoff + i * phsz;
		if (r32(ph) != 1) continue;
		memcpy(img + 0x800 + r32(ph + 8) - lo, e + r32(ph + 4), r32(ph + 16));
	}
	memcpy(img, "PS-X EXE", 8);
	w32(img + 0x10, r32(e + 24) ? r32(e + 24) : lo);   // pc0 (sin entrada: el principio, donde va _start)
	w32(img + 0x18, lo);
	w32(img + 0x1c, hi - lo);
	w32(img + 0x30, 0x801fff00);   // pila
	f = fopen(argv[2], "wb");
	fwrite(img, 1, 0x800 + hi - lo, f);
	fclose(f);
	printf("%s: %08x-%08x entrada %08x\n", argv[2], lo, hi, r32(e + 24));
	return 0;
}
