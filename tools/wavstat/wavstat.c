// Nivel de un WAV de 16 bits estéreo por tramos: wavstat archivo.wav [segundos_por_tramo]
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	double seg = argc > 2 ? atof(argv[2]) : 1.0, sum = 0;
	short s[2];
	long n = 0, per, peak = 0, t = 0;
	if (!f) return 1;
	fseek(f, 44, SEEK_SET);
	per = (long)(44100 * seg);
	while (fread(s, 2, 2, f) == 2) {
		sum += (double)s[0] * s[0] + (double)s[1] * s[1];
		if (abs(s[0]) > peak) peak = abs(s[0]);
		if (++n == per) {
			printf("%6.1f s  rms %6.0f  pico %5ld  %s\n", t * seg, sqrt(sum / (2 * n)), peak, "");
			t++; n = 0; sum = 0; peak = 0;
		}
	}
	return 0;
}
