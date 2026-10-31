// Desensambla un volcado de la RAM de la PS1: disfile <ram.bin> <dirección_hex> <instrucciones>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef uint32_t u32;
void mips_dis(u32 pc, u32 op);
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	u32 a = (u32)strtoul(argv[2], 0, 16), n = (u32)atoi(argv[3]), w;
	if (!f) return 1;
	for (u32 i = 0; i < n; i++) {
		fseek(f, (long)((a + i * 4) & 0x1ffffc), SEEK_SET);
		if (fread(&w, 4, 1, f) != 1) return 1;
		mips_dis(a + i * 4, w);
	}
	return 0;
}
