// Mide en el PC el tiempo de dibujo de la GPU (compilar con -DPSX_PROFILE junto al banco de pruebas)
#include "psx.h"
#include <stdio.h>
#include <string.h>
#include <x86intrin.h>
u32 psx_prof[PROF_N];
unsigned long long bench_pixels;
u32 psx_prof_ticks(void) { return (u32)(__rdtsc() >> 4); }
static void line(void)
{
	static int n;
	static unsigned long long gpu;
	if (++n % 263) return;
	gpu += psx_prof[PROF_GPU];
	bench_pixels += psx_stat.gpu_pixels;
	psx_stat.gpu_pixels = 0;
	psx_prof[PROF_GPU] = 0;
	if (n % (263 * 600) == 0) {
		fprintf(stderr, "frames %d: gpu %llu Mticks, %llu Kpix, %.1f ticks/pix\n", n / 263, gpu >> 20, bench_pixels >> 10,
		        bench_pixels ? (double)gpu / bench_pixels : 0.0);
		gpu = 0;
		bench_pixels = 0;
	}
}
__attribute__((constructor)) static void reg(void) { psx_line_cb = line; }
