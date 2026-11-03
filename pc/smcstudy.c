// Estudio (solo PC, -DSMC_STUDY): cuántas escrituras invalidarían código en la DS (trozos de 1 KB con rango
// lo/hi del código ejecutado) y cuántas pisan de verdad una instrucción ya ejecutada.
#ifdef SMC_STUDY
#include <stdio.h>
#include <stdlib.h>
#include "psx.h"
static unsigned char ex[0x200000 / 4];
static unsigned short lo[2048], hi[2048];
static unsigned char act[2048];
static unsigned long tpc[0x80000], taddr[0x80000];
static unsigned long ds_hits, true_hits, wpc[0x200000 / 4], waddr[2048];
void smc_study_exec(u32 a)
{
	u32 pg = a >> 10, o = a & 1023;
	ex[a >> 2] = 1;
	if (!act[pg]) { act[pg] = 1; lo[pg] = o; hi[pg] = o + 4; }
	else { if (o < lo[pg]) lo[pg] = o; if (o + 4 > hi[pg]) hi[pg] = o + 4; }
}
void smc_study_write(u32 a)
{
	u32 pg = a >> 10, o = a & 1023;
	if (ex[a >> 2]) { true_hits++; ex[a >> 2] = 0; tpc[(cpu.pc & 0x1ffffc) >> 2]++; taddr[a >> 2]++; }
	if (act[pg] && o + 4 > lo[pg] && o < hi[pg]) {
		ds_hits++; act[pg] = 0; waddr[pg]++;
		wpc[(cpu.pc & 0x1ffffc) >> 2]++;
	}
}
void smc_study_report(void)
{
	printf("SMC ds_hits=%lu true_hits=%lu\n", ds_hits, true_hits);
	for (int k = 0; k < 12; k++) {
		u32 best = 0; unsigned long bv = 0;
		for (u32 i = 0; i < 2048; i++) if (waddr[i] > bv) { bv = waddr[i]; best = i; }
		if (!bv) break;
		printf("  pagina %06x: %lu\n", best << 10, bv); waddr[best] = 0;
	}
	for (int k = 0; k < 12; k++) {
		u32 best = 0; unsigned long bv = 0;
		for (u32 i = 0; i < 0x80000; i++) if (wpc[i] > bv) { bv = wpc[i]; best = i; }
		if (!bv) break;
		printf("  pc %08x: %lu\n", 0x80000000 | best << 2, bv); wpc[best] = 0;
	}
}
#endif
#ifdef SMC_STUDY
static unsigned long dma_hits, dma_true, dma_calls;
void smc_study_dma(u32 off, u32 len)
{
	int hit = 0, tr = 0;
	dma_calls++;
	for (u32 a = off & ~3u; a < off + len && a < 0x200000; a += 4) {
		u32 pg = a >> 10, o = a & 1023;
		if (ex[a >> 2]) { tr = 1; ex[a >> 2] = 0; }
		if (act[pg] && o + 4 > lo[pg] && o < hi[pg]) { act[pg] = 0; hit++; waddr[pg]++; }
	}
	dma_hits += hit; dma_true += tr;
}
void smc_study_report2(void) { printf("DMA calls=%lu page_hits=%lu calls_overwriting_code=%lu\n", dma_calls, dma_hits, dma_true); }
#endif
#ifdef SMC_STUDY
void smc_study_report3(void)
{
	for (int k = 0; k < 10; k++) { u32 b = 0; unsigned long v = 0; for (u32 i = 0; i < 0x80000; i++) if (tpc[i] > v) { v = tpc[i]; b = i; } if (!v) break; printf("  escribe pc %08x: %lu\n", 0x80000000 | b << 2, v); tpc[b] = 0; }
	for (int k = 0; k < 16; k++) { u32 b = 0; unsigned long v = 0; for (u32 i = 0; i < 0x80000; i++) if (taddr[i] > v) { v = taddr[i]; b = i; } if (!v) break; printf("  codigo pisado %08x: %lu\n", 0x80000000 | b << 2, v); taddr[b] = 0; }
}
#endif
