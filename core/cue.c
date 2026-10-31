// Lectura de .cue con varias pistas (una por archivo, como los volcados de redump): dónde empieza cada archivo en
// el disco y dónde cada pista (INDEX 00 = su pausa previa, INDEX 01 = su comienzo). Cada archivo sigue al anterior.
#include "psx.h"
#include <stdio.h>
#include <stdlib.h>

static u32 msf(const char *s)   // "mm:ss:ff" -> sectores
{
	unsigned m = 0, sec = 0, f = 0;
	if (sscanf(s, "%u:%u:%u", &m, &sec, &f) != 3) return 0;
	return (m * 60 + sec) * 75 + f;
}

// dir: carpeta del .cue (con la barra final) para los nombres de los FILE. file_sectors(ruta) da el tamaño de
// cada archivo en sectores (0 si no existe). Devuelve 1 si hay al menos un archivo.
int cue_parse(const char *cue_path, const char *dir, CueDisc *d, u32 (*file_sectors)(const char *path))
{
	FILE *c = fopen(cue_path, "r");
	char line[512];
	int cur_audio = 0, have_pre = 0;
	memset(d, 0, sizeof *d);
	if (!c) return 0;
	while (fgets(line, sizeof line, c)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (!strncmp(p, "FILE", 4)) {
			char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
			CueFile *f;
			if (!q1 || !q2 || d->nfile >= CUE_MAX_FILES) continue;
			*q2 = 0;
			f = &d->file[d->nfile];
			snprintf(f->path, sizeof f->path, "%s%s", dir, q1 + 1);
			f->start = d->total;
			f->sectors = file_sectors(f->path);
			d->total += f->sectors;
			d->nfile++;
		} else if (!strncmp(p, "TRACK", 5) && d->nfile && d->ntrk < 99) {
			cur_audio = strstr(p, "AUDIO") != NULL;
			have_pre = 0;
			d->trk[d->ntrk].audio = (u8)cur_audio;
			d->trk[d->ntrk].start = d->trk[d->ntrk].pre = d->file[d->nfile - 1].start;
			d->ntrk++;
		} else if (!strncmp(p, "INDEX", 5) && d->ntrk) {
			unsigned idx;
			char t[16];
			PsxTrack *k = &d->trk[d->ntrk - 1];
			if (sscanf(p + 5, "%u %15s", &idx, t) != 2) continue;
			if (idx == 0) { k->pre = d->file[d->nfile - 1].start + msf(t); have_pre = 1; }
			else if (idx == 1) { k->start = d->file[d->nfile - 1].start + msf(t); if (!have_pre) k->pre = k->start; }
		}
	}
	fclose(c);
	return d->nfile > 0;
}

// Archivo (y sector dentro de él) donde cae lba; -1 si se sale del disco
int cue_locate(const CueDisc *d, u32 lba, u32 *sector_in_file)
{
	for (int i = 0; i < d->nfile; i++)
		if (lba >= d->file[i].start && lba < d->file[i].start + d->file[i].sectors) {
			*sector_in_file = lba - d->file[i].start;
			return i;
		}
	return -1;
}
