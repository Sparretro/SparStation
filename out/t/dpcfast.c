// Prueba diferencial: dpc12f contra dpc() general (y dpc_fast), DPCS y DPCT
#include "../../core/gte.c"
#include <stdio.h>
#include <stdlib.h>
static unsigned rs = 99;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs ^ (rs >> 13); }
u32 gte_hist[64];
PsxCore psx_core;
void psx_state_io(PsxState *s, void *p, u32 len) { (void)s; (void)p; (void)len; }
int main(void)
{
	int bad = 0;
	gte_reset();
	for (int it = 0; it < 5000000; it++) {
		u8 cf0[3][4], rgb[4];
		s32 mac[4]; s16 ir[4]; u8 cf[3][4]; u32 fl;
		int big = rnd() & 7, t = rnd() & 1;
		for (int i = 0; i < 3; i++) FC[i] = big ? (s32)(rnd() % 0x80000) - 0x40000 : (s32)(rnd() & 0xff) << 4;
		IR[0] = (rnd() & 3) ? (s16)(rnd() % 0x1001) : (s16)rnd();
		lm = rnd() & 1; sf = 12;
		if (!dpc_fast_ok()) continue;
		for (int i = 0; i < 12; i++) ((u8 *)cf0)[i] = (u8)rnd();
		for (int i = 0; i < 4; i++) rgb[i] = (u8)rnd();
		memcpy(CF, cf0, 12); memcpy(RGBC, rgb, 4); FLAG = 0;
		if (t) { dpc(CF[0]); dpc(CF[0]); dpc(CF[0]); } else dpc(RGBC);
		memcpy(mac, MAC, 16); memcpy(ir, IR, 8); memcpy(cf, CF, 12); fl = FLAG;
		memcpy(CF, cf0, 12); memcpy(RGBC, rgb, 4); FLAG = 0;
		if (t) { dpc12f(CF[0]); dpc12f(CF[0]); dpc12f(CF[0]); } else dpc12f(RGBC);
		if (memcmp(mac + 1, MAC + 1, 12) || memcmp(ir, IR, 8) || memcmp(cf, CF, 12) || fl != FLAG)
			if (bad++ < 5) printf("DISTINTO t=%d lm=%d flag %08x/%08x\n", t, lm, fl, FLAG);
	}
	printf("%s (%d fallos)\n", bad ? "MAL" : "BIEN", bad);
	return bad != 0;
}
