// Prueba diferencial: rtp12f (rápida) contra rtp12 (general) con entradas aleatorias, incluidos extremos
#include "../../core/gte.c"
#include <stdio.h>
#include <stdlib.h>
static unsigned rs = 777;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs ^ (rs >> 13); }
u32 gte_hist[64];
PsxCore psx_core;
void psx_state_io(PsxState *s, void *p, u32 len) { (void)s; (void)p; (void)len; }
static s32 pick32(void) { switch (rnd() & 3) { case 0: return (s32)rnd(); case 1: return (s32)(rnd() % 0x20000) - 0x10000; default: return (s32)(rnd() % 0x2000) - 0x1000; } }
static s16 pick16(void) { return (rnd() & 3) ? (s16)((int)(rnd() % 0x2000) - 0x1000) : (s16)rnd(); }
#define SNAP(b) do { memcpy(b.mac, MAC, sizeof MAC); memcpy(b.ir, IR, sizeof IR); memcpy(b.sx, SX, sizeof SX); memcpy(b.sy, SY, sizeof SY); memcpy(b.sz, SZ, sizeof SZ); b.flag = FLAG; } while (0)
typedef struct { s32 mac[4]; s16 ir[4]; s16 sx[3], sy[3]; u16 sz[4]; u32 flag; } Snap;
int main(void)
{
	int bad = 0;
	gte_reset();
	sf = 12;
	for (int it = 0; it < 5000000; it++) {
		Snap a, b;
		int vi = rnd() % 3, last = rnd() & 1;
		s16 sv[3][3]; s16 sx0[3], sy0[3]; u16 sz0[4]; s16 ir0[4]; s32 mac0[4];
		for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { V[i][j] = pick16(); RT[i][j] = pick16(); }
		for (int i = 0; i < 3; i++) TR[i] = (rnd() & 15) ? (s32)(rnd() % 0x80000) - 0x40000 : pick32();
		OFX = pick32(); OFY = pick32(); DQB = pick32(); DQA = pick16(); H = (u16)((rnd() & 3) ? rnd() % 0x400 : rnd());
		lm = rnd() & 1;
		for (int i = 0; i < 3; i++) { SX[i] = (s16)rnd(); SY[i] = (s16)rnd(); }
		for (int i = 0; i < 4; i++) { SZ[i] = (u16)rnd(); IR[i] = (s16)rnd(); MAC[i] = (s32)rnd(); }
		memcpy(sv, V, sizeof sv); memcpy(sx0, SX, sizeof sx0); memcpy(sy0, SY, sizeof sy0); memcpy(sz0, SZ, sizeof sz0); memcpy(ir0, IR, sizeof ir0); memcpy(mac0, MAC, sizeof mac0);
		FLAG = rnd() & 0x1000;
		{ u32 f0 = FLAG; rtp12(vi, last); SNAP(a);
		  memcpy(SX, sx0, sizeof sx0); memcpy(SY, sy0, sizeof sy0); memcpy(SZ, sz0, sizeof sz0); memcpy(IR, ir0, sizeof ir0); memcpy(MAC, mac0, sizeof mac0);
		  FLAG = f0; rtp12f(vi, last); SNAP(b); }
		if (memcmp(&a, &b, sizeof a)) {
			if (bad++ < 5) printf("DISTINTO it=%d vi=%d last=%d lm=%d flag %08x/%08x mac0 %d/%d ir %d,%d,%d/%d,%d,%d\n", it, vi, last, lm, a.flag, b.flag, a.mac[0], b.mac[0], a.ir[1], a.ir[2], a.ir[3], b.ir[1], b.ir[2], b.ir[3]);
		}
	}
	printf("%s (%d fallos)\n", bad ? "MAL" : "BIEN", bad);
	return bad != 0;
}
