// Pasa el perfil de la DSi (sd:/ps1/sparstation_prof.bin, ver ds/source/prof.c) al formato de dsbench para profrep.
// uso: profdsi entrada.bin salida.prof [sesión (1 = la primera; 0 = la última)] [desde_s] [hasta_s]
// Cada sesión empieza con 16 palabras («SPRF» «ROF1», Hz, zonas del dynarec) y 64 bytes con el nombre del juego;
// luego pares (dirección ARM, PC MIPS) y marcas: 0xFFFExxxx = segundo xxxx, 0xFFFFxxxx = MARCA del medidor.
// Con la zona caliente en la NWRAM (v20) hay 8 zonas y la fría está aparte, en la RAM principal: sus muestras se
// mueven justo detrás de la caliente para que profrep (6 zonas, bloques [3, 4) y fría [4, 5)) las cuente bien.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv)
{
	FILE *f, *o;
	uint32_t w[2], hdr[16], r[8], sec = 0, from, to, kept = 0, total = 0;
	char name[65] = { 0 };
	int want, ses = 0, on = 0, nses = 0;
	if (argc < 3) { fprintf(stderr, "uso: profdsi entrada.bin salida.prof [sesion] [desde_s] [hasta_s]\n"); return 1; }
	want = argc > 3 ? atoi(argv[3]) : 0;
	from = argc > 4 ? (uint32_t)atoi(argv[4]) : 0;
	to = argc > 5 ? (uint32_t)atoi(argv[5]) : 0xffffffffu;
	if (!(f = fopen(argv[1], "rb"))) return 1;
	// primera pasada: cuántas sesiones hay (para «0 = la última») y cuáles son
	while (fread(w, 4, 2, f) == 2) {
		if (w[0] == 0x46525053 && w[1] == 0x31464f52) {
			nses++;
			if (fread(hdr + 2, 4, 14, f) != 14 || fread(name, 1, 64, f) != 64) break;
			name[64] = 0;
			printf("sesión %d: %s (%u Hz)\n", nses, name, hdr[2]);
		}
	}
	if (!nses) { fprintf(stderr, "no hay sesiones\n"); return 1; }
	if (want <= 0 || want > nses) want = nses;
	rewind(f);
	o = fopen(argv[2], "wb");
	if (!o) return 1;
	while (fread(w, 4, 2, f) == 2) {
		if (w[0] == 0x46525053 && w[1] == 0x31464f52) {
			if (fread(hdr + 2, 4, 14, f) != 14 || fread(name, 1, 64, f) != 64) break;
			on = ++ses == want;
			if (on) {
				uint32_t h6[6];
				memcpy(r, hdr + 3, sizeof r);
				if (!r[7] && r[6] && (r[5] < r[3] || r[5] >= r[4] + 0x100000)) {   // v20: fría aparte
					h6[4] = r[4]; h6[5] = r[4] + (r[6] - r[5]);
				} else if (!r[6]) {   // v19 y antes: 6 zonas seguidas (bloques [3, 4), fría [4, 5))
					r[6] = r[5]; r[5] = r[4];
					h6[4] = r[4]; h6[5] = r[6];
				} else { h6[4] = r[4]; h6[5] = r[6]; }
				h6[0] = r[0]; h6[1] = r[1]; h6[2] = r[2]; h6[3] = r[3];
				fwrite(h6, 4, 6, o);
				printf("usando la sesión %d (%s); zonas %08x %08x %08x %08x-%08x fría %08x-%08x\n", ses, name, r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
			}
			continue;
		}
		if ((w[0] >> 16) == 0xfffe) { sec = w[0] & 0xffff; continue; }
		if ((w[0] >> 16) == 0xffff) { if (on) printf("MARCA %u en el segundo %u\n", w[0] & 0xffff, sec); continue; }
		if (!on) continue;
		total++;
		if (sec < from || sec >= to) continue;
		if (w[0] >= r[5] && w[0] < r[6] && r[5] != r[4]) w[0] = r[4] + (w[0] - r[5]);   // fría aparte: detrás de la caliente
		fwrite(w, 4, 2, o);
		kept++;
	}
	printf("%u muestras en la sesión, %u en [%u, %u) s\n", total, kept, from, to);
	fclose(o);
	fclose(f);
	return 0;
}
