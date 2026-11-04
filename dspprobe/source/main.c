// Prueba del DSP (Teak) de la DSi para SparStation.
// 1) Cómo llega la NWRAM (B = memoria de programa del DSP, C = de datos) y si el ARM9 puede reasignarla (MBK9).
// 2) Si se puede: carga en el hueco B0 un programa de 7 palabras que lee CMD0, le suma 1 y lo deja en REP0, en
//    bucle; enciende el DSP y comprueba las respuestas (y cuántas idas y vueltas caben en un segundo).
// Todo va a la pantalla y a sd:/ps1/dsp_prueba.txt (se añade al final: así se comparan TWiLight y Unlaunch).
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>

#define SCFG_RST9 (*(vu16 *)0x04004006)
#define DSP_PCFG (*(vu16 *)0x04004308)
#define DSP_PSTS (*(vu16 *)0x0400430c)
#define DSP_CMD0 (*(vu16 *)0x04004320)
#define DSP_REP0 (*(vu16 *)0x04004324)

// mov [80C2h], a0 / inc a0 / mov a0l, [80C0h] / br 0  (codificación de GBATEK, «DSi TeakLite II Instruction Set»)
static const u16 prog[7] = { 0xd4b8, 0x80c2, 0x67d0, 0xd4bc, 0x80c0, 0x4180, 0x0000 };

static FILE *lf;
#define P(...) do { printf(__VA_ARGS__); if (lf) fprintf(lf, __VA_ARGS__); } while (0)

static void delay(u32 n) { for (vu32 i = 0; i < n; i++) { } }

static void timer_start(void)
{
	TIMER_CR(0) = 0; TIMER_CR(1) = 0; TIMER_DATA(0) = 0; TIMER_DATA(1) = 0;
	TIMER_CR(1) = TIMER_ENABLE | TIMER_CASCADE;
	TIMER_CR(0) = TIMER_ENABLE | ClockDivider_1;   // 33,51 MHz
}
static u32 ticks(void) { u32 hi, lo; do { hi = TIMER_DATA(1); lo = TIMER_DATA(0); } while (hi != TIMER_DATA(1)); return hi << 16 | lo; }

static void regs(const char *when)
{
	P("[%s]\nSCFG_EXT=%08lx CLK=%04x RST=%04x\n", when, REG_SCFG_EXT, REG_SCFG_CLK, SCFG_RST9);
	P("MBK1=%08lx\nMBK2/3(B)=%08lx %08lx\nMBK4/5(C)=%08lx %08lx\n", *(vu32 *)0x04004040, *(vu32 *)0x04004044,
	  *(vu32 *)0x04004048, *(vu32 *)0x0400404c, *(vu32 *)0x04004050);
	P("MBK6-8=%08lx %08lx %08lx\nMBK9=%08lx\n", REG_MBK_MAP_A, REG_MBK_MAP_B, REG_MBK_MAP_C, REG_MBK_SLOTWRPROT);
}

static int wait_rep(u16 want, u32 max_ticks)
{
	u32 t0 = ticks();
	while (DSP_REP0 != want) if (ticks() - t0 > max_ticks) return 0;
	return 1;
}

static void test(void)
{
	vu16 *code = (vu16 *)0x03740000;   // hueco B0 visto por el ARM9 (mapa B en 03740000h)
	int i, ok;
	if (!isDSiMode()) { P("No es modo DSi: no hay DSP.\n"); return; }
	regs("al arrancar");
	if (!(REG_SCFG_EXT & (1u << 18))) { P("SCFG_EXT: sin acceso al DSP (bit 18).\n"); return; }
	if (!(REG_SCFG_EXT & (1u << 25))) { P("SCFG_EXT: sin acceso a la NWRAM (bit 25).\n"); return; }
	if (REG_MBK_SLOTWRPROT & ((1u << 8) | (1u << 16))) {
		P("\nNWRAM B0/C0 PROTEGIDA (MBK9): el ARM9 no puede\ndarsela al DSP. Con TWiLight pasa esto;\nprueba a lanzar esta ROM desde Unlaunch.\n");
		return;
	}
	// B0 al ARM9 para copiar el programa
	REG_MBK_SLOT_Bx(0) = 0x80;   // activo, ARM9, posición 0
	REG_MBK_MAP_B = MBK_MAP_START(0xe8) | MBK_MAP_SIZE(MbkMapSize_256K) | MBK_MAP_END(0xf0);
	// región 3 del MPU sin caché para 03700000h-0377FFFFh (calico no la da)
	armMpuSetDataPerm((armMpuGetDataPerm() & ~0xf000u) | 0x3000u);
	armMpuSetDCacheConfig(armMpuGetDCacheConfig() & ~8u);
	armMpuSetWrBufConfig(armMpuGetWrBufConfig() & ~8u);
	armMpuSetRegion3(armMpuDefineRegion(0x03700000, CP15_PU_512K));
	armDrainWriteBuffer();
	for (i = 0; i < 64; i++) code[i] = i < 7 ? prog[i] : 0;
	for (ok = 1, i = 0; i < 7; i++) if (code[i] != prog[i]) ok = 0;
	P("\nPrograma en B0: %s (MBK2=%08lx)\n", ok ? "escrito y leido bien" : "NO SE LEE BIEN", *(vu32 *)0x04004044);
	if (!ok) return;
	// B0 = programa del DSP, C0 = datos del DSP
	REG_MBK_SLOT_Bx(0) = 0x82;
	REG_MBK_SLOT_Cx(0) = 0x82;
	P("B0/C0 al DSP: MBK2=%08lx MBK4=%08lx\n", *(vu32 *)0x04004044, *(vu32 *)0x0400404c);
	if ((REG_MBK_SLOT_Bx(0) & 3) < 2 || (REG_MBK_SLOT_Cx(0) & 3) < 2) { P("No acepta el cambio de dueño.\n"); return; }
	// encender
	REG_SCFG_CLK |= 2;            // reloj del DSP
	delay(1000);
	SCFG_RST9 = 1;             // fuera el reset del bloque
	delay(1000);
	DSP_PCFG = 1;                 // reset del núcleo...
	delay(1000);
	DSP_PCFG = 0;                 // ...y suelta: empieza en la dirección 0
	timer_start();
	{
		u32 t0 = ticks();
		while ((DSP_PSTS & 4) && ticks() - t0 < 33513982 / 10) { }
		P("PSTS=%04x tras soltar el reset\n", DSP_PSTS);
	}
	{
		static const u16 vals[5] = { 0x1234, 0x0000, 0x7ffe, 0xbeef, 0xfffe };
		int good = 0;
		for (i = 0; i < 5; i++) {
			DSP_CMD0 = vals[i];
			int r = wait_rep((u16)(vals[i] + 1), 33513982 / 100);   // 10 ms
			P("CMD0=%04x -> REP0=%04x %s\n", vals[i], DSP_REP0, r ? "OK" : "MAL");
			good += r;
		}
		if (good == 5) {
			u32 t0 = ticks(), n = 0;
			while (ticks() - t0 < 33513982) {   // un segundo de idas y vueltas
				u16 v = (u16)(n * 2);
				DSP_CMD0 = v;
				if (!wait_rep((u16)(v + 1), 33513982 / 100)) break;
				n++;
			}
			P("\nEL DSP FUNCIONA. Idas y vueltas en 1 s: %lu\n", n);
		} else P("\nEl DSP no responde bien (%d/5).\n", good);
	}
	// apagar y devolver la NWRAM al ARM9
	DSP_PCFG = 1;
	SCFG_RST9 = 0;
	REG_SCFG_CLK &= ~2;
	REG_MBK_SLOT_Bx(0) = 0x80;
	REG_MBK_SLOT_Cx(0) = 0x80;
	regs("al terminar");
}

int main(void)
{
	consoleDemoInit();
	printf("SparStation - prueba del DSP\n\n");
	if (fatInitDefault()) {
		mkdir("sd:/ps1", 0777);
		lf = fopen("sd:/ps1/dsp_prueba.txt", "a");
		if (lf) fprintf(lf, "\n===== prueba del DSP =====\n");
	} else printf("(sin SD: solo en pantalla)\n");
	test();
	if (lf) { fclose(lf); printf("\nGuardado en sd:/ps1/dsp_prueba.txt\n"); }
	printf("\nSTART para salir\n");
	for (;;) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_START) break;
	}
	return 0;
}
