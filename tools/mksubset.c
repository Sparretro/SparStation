// Monta un «disco reducido» con solo los sectores que un juego lee, para probarlo dentro de melonDS
// en modo DS (sin SD). uso: mksubset <juego.bin> <lbas.txt ordenados> <carpeta_salida>
// Salida: disc.idx (nº de sectores del disco original + LBAs u32 ordenados) y disc.dat (los sectores de 2352 bytes en ese orden).
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv)
{
	FILE *bin, *lf, *idx, *dat;
	char path[1024];
	unsigned lba;
	static unsigned char sec[2352];
	int n = 0;
	if (argc < 4) { fprintf(stderr, "uso: mksubset <juego.bin> <lbas.txt> <carpeta>\n"); return 1; }
	bin = fopen(argv[1], "rb");
	lf = fopen(argv[2], "r");
	snprintf(path, sizeof path, "%s/disc.idx", argv[3]);
	idx = fopen(path, "wb");
	snprintf(path, sizeof path, "%s/disc.dat", argv[3]);
	dat = fopen(path, "wb");
	if (!bin || !lf || !idx || !dat) { fprintf(stderr, "no puedo abrir algún archivo\n"); return 1; }
	{   // primera palabra: sectores del disco original (el juego puede preguntarlo)
		uint32_t total;
		_fseeki64(bin, 0, SEEK_END);
		total = (uint32_t)(_ftelli64(bin) / 2352);
		fwrite(&total, 4, 1, idx);
	}
	while (fscanf(lf, "%u", &lba) == 1) {
		uint32_t le = lba;
		if (_fseeki64(bin, (long long)lba * 2352, SEEK_SET) || fread(sec, 1, 2352, bin) != 2352) continue;
		fwrite(&le, 4, 1, idx);
		fwrite(sec, 1, 2352, dat);
		n++;
	}
	printf("%d sectores\n", n);
	fclose(idx);
	fclose(dat);
	return 0;
}
