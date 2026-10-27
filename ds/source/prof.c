// Perfilador por muestreo para la DSi real: una tarea periódica de calico (sus ticks usan los temporizadores
// 2 y 3; el 0 y el 1 los usa cpuStartTiming) interrumpe unas 4000 veces por segundo y
// se apunta dónde estaba el ARM9. La rutina de interrupciones de calico guarda {r0-r3, r12, lr} en lo
// alto de la pila IRQ con lr = la instrucción interrumpida, así que basta con leer __sp_irq - 4.
// Cada segundo se escribe lo nuevo en sd:/ps1/sparstation_prof.bin como pares (dirección ARM, PC MIPS
// del bloque traducido o 0); las palabras 0xFFFExxxx = segundo, 0xFFFFxxxx = marca (con 0 detrás).
// tools/profrep lo cruza con el .elf para sacar las funciones y bloques que más tiempo se llevan.
#include <nds.h>
#include <stdio.h>
#include <unistd.h>
#include "psx.h"
#include "prof.h"

#define PROF_PATH "sd:/ps1/sparstation_prof.bin"
#define RING 32768   // muestras (8 s de margen)
#define HZ 3989      // no múltiplo de 60: que no vaya al compás de la vblank

extern char __sp_irq[];
static u32 ring[RING];
static volatile u32 wpos;
static u32 rpos;
static FILE *pf;

static TickTask task;

static void prof_tick(TickTask *t)
{
	(void)t;
	ring[wpos & (RING - 1)] = *(volatile u32 *)((uintptr_t)__sp_irq - 4);
	wpos++;
}

static void put_tag(u32 tag)
{
	u32 w[2] = { tag, 0 };
	if (pf) fwrite(w, 4, 2, pf);
}

void prof_start(const char *game)
{
	u32 hdr[16] = { 0x46525053, 0x31464f52 };   // "SPRFROF1": marca de formato
	rpos = wpos = 0;
	tickTaskStart(&task, prof_tick, TICK_FREQ / HZ, TICK_FREQ / HZ);
	pf = fopen(PROF_PATH, "ab");
	if (!pf) return;
	hdr[2] = HZ;
	dyn_regions(hdr + 3);   // 3..8
	fwrite(hdr, 4, 16, pf);
	{
		char name[64] = { 0 };
		snprintf(name, sizeof name, "%s", game);
		fwrite(name, 1, sizeof name, pf);
	}
}

// Escribe lo muestreado desde la última vez (lo llama el medidor una vez por segundo)
void prof_second(u32 second)
{
	u32 w = wpos, buf[256];
	int n = 0;
	if (!pf) return;
	if (w - rpos > RING) rpos = w - RING;   // no debería pasar: se perdieron muestras
	put_tag(0xfffe0000u | (second & 0xffff));
	for (; rpos != w; rpos++) {
		u32 a = ring[rpos & (RING - 1)];
		buf[n++] = a;
		buf[n++] = dyn_block_of(a);
		if (n == 256) { fwrite(buf, 4, n, pf); n = 0; }
	}
	if (n) fwrite(buf, 4, n, pf);
}

void prof_mark(u32 mark)
{
	put_tag(0xffff0000u | (mark & 0xffff));
}

void prof_sync(void)
{
	if (!pf) return;
	fflush(pf);
	fsync(fileno(pf));
}

#ifdef PROF_TEST
// Prueba en melonDS: cuántas muestras caen en cada zona (sin SD)
void prof_test_report(void)
{
	u32 r[6], n[6] = { 0 }, w = wpos;
	extern char __itcm_start[], __text_start[], __end__[];
	dyn_regions(r);
	for (u32 i = w > RING ? w - RING : 0; i < w; i++) {
		u32 a = ring[i & (RING - 1)];
		if (a >= r[3] && a < r[5]) n[0]++;                         // bloques traducidos
		else if (a >= r[0] && a < r[3]) n[1]++;                    // trampolines y rutinas
		else if (a < 0x02000000) n[2]++;                           // ITCM
		else if (a >= 0x02000000 && a < (u32)(uintptr_t)__end__) n[3]++;   // programa
		else if (a >= 0xffff0000) n[4]++;                          // BIOS
		else n[5]++;
	}
	printf("PROF %lu: cod %lu stub %lu itcm %lu prog %lu bios %lu otro %lu\n", w, n[0], n[1], n[2], n[3], n[4], n[5]);
	printf("ej %08lx %08lx %08lx\n", ring[(w - 1) & (RING - 1)], ring[(w - 2) & (RING - 1)], ring[(w - 3) & (RING - 1)]);
	fflush(stdout);
}
#endif
