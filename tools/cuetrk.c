// Índice de pistas de un .cue para el disco reducido de las pruebas en melonDS: cuetrk <juego.cue> <disc.trk>
// Escribe el número de pistas (u32) y luego audio, start y pre de cada una (u32), lo que lee open_subset en la DS
#include "psx.h"
#include <stdio.h>

static u32 file_sectors(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return (u32)(n / 2352);
}

int main(int argc, char **argv)
{
	static CueDisc d;
	char dir[1024];
	const char *slash, *bs;
	FILE *o;
	u32 n;
	if (argc < 3) return 1;
	slash = strrchr(argv[1], '/');
	bs = strrchr(argv[1], 0x5c);   // barra invertida
	if (bs > slash) slash = bs;
	snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - argv[1] + 1) : 0, argv[1]);
	if (!cue_parse(argv[1], dir, &d, file_sectors)) { fprintf(stderr, "no leo %s\n", argv[1]); return 1; }
	o = fopen(argv[2], "wb");
	if (!o) return 1;
	n = (u32)d.ntrk;
	fwrite(&n, 4, 1, o);
	for (int i = 0; i < d.ntrk; i++) {
		u32 t[3] = { d.trk[i].audio, d.trk[i].start, d.trk[i].pre };
		fwrite(t, 4, 3, o);
	}
	fclose(o);
	printf("%d pistas, %u sectores en total\n", d.ntrk, (unsigned)d.total);
	return 0;
}
