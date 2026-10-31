// Muestras del perfilador por dirección dentro de un rango: profrange muestras.prof desde_hex hasta_hex
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	uint32_t lo = (uint32_t)strtoul(argv[2], 0, 16), hi = (uint32_t)strtoul(argv[3], 0, 16), hdr[6], pair[2];
	static uint32_t cnt[65536];
	if (!f || fread(hdr, 4, 6, f) != 6) return 1;
	while (fread(pair, 4, 2, f) == 2)
		if (pair[0] >= lo && pair[0] < hi && (pair[0] - lo) / 4 < 65536) cnt[(pair[0] - lo) / 4]++;
	for (uint32_t i = 0; i < (hi - lo) / 4 && i < 65536; i++) if (cnt[i]) printf("%08x %u\n", lo + i * 4, cnt[i]);
	return 0;
}
