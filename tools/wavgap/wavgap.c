// Continuidad de un tono en un WAV de 16 bits estéreo (prueba del hilo de la música, DA_TEST en main.c):
// potencia de la frecuencia pedida en ventanas de 10 ms del canal izquierdo (Goertzel). Desde que aparece,
// cuenta las ventanas por debajo de la mitad de la mediana (huecos) y dice dónde están.
// uso: wavgap archivo.wav [hz]
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
	FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
	double hz = argc > 2 ? atof(argv[2]) : 441.0, *mag, *sorted, med;
	unsigned hdr[11];
	long rate, win, nwin = 0, cap = 1 << 16, first = -1, gaps = 0, shown = 0, run = 0, worst_run = 0;
	short s[2];
	if (!f || fread(hdr, 4, 11, f) != 11) { fprintf(stderr, "uso: wavgap archivo.wav [hz]\n"); return 1; }
	rate = (long)hdr[6];
	win = rate / 100;
	mag = malloc(sizeof *mag * cap);
	for (;;) {   // Goertzel por ventana
		double w = 2 * cos(2 * M_PI * hz / rate), s1 = 0, s2 = 0;
		long i;
		for (i = 0; i < win && fread(s, 2, 2, f) == 2; i++) {
			double s0 = s[0] + w * s1 - s2;
			s2 = s1;
			s1 = s0;
		}
		if (i < win) break;
		if (nwin == cap) mag = realloc(mag, sizeof *mag * (cap *= 2));
		mag[nwin++] = sqrt(s1 * s1 + s2 * s2 - w * s1 * s2) / win;
	}
	if (!nwin) return 1;
	sorted = malloc(sizeof *sorted * nwin);
	for (long i = 0; i < nwin; i++) sorted[i] = mag[i];
	qsort(sorted, nwin, sizeof *sorted, cmp);
	med = sorted[nwin * 3 / 4];   // el tono ocupa casi todo: el cuartil alto es su nivel
	for (long i = 0; i < nwin; i++) if (first < 0 && mag[i] > med / 2) first = i;
	if (first < 0) { printf("%ld Hz, %.1f s: no hay tono de %.0f Hz\n", rate, nwin / 100.0, hz); return 2; }
	for (long i = first; i < nwin; i++) {
		if (mag[i] < med / 2) {
			gaps++;
			run++;
			if (run > worst_run) worst_run = run;
			if (run == 1 && shown++ < 12) printf("  hueco en %.2f s (nivel %.0f de %.0f)\n", i / 100.0, mag[i], med);
		} else run = 0;
	}
	printf("%ld Hz, %.1f s; tono desde %.2f s, nivel %.0f; ventanas con hueco %ld de %ld (el más largo %ld0 ms)\n",
	       rate, nwin / 100.0, first / 100.0, med, gaps, nwin - first, worst_run);
	return gaps ? 3 : 0;
}
