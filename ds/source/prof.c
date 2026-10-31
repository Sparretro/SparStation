// Perfilador por muestreo: una tarea periódica de calico (sus ticks usan los temporizadores 2 y 3; el 0 y
// el 1 los usa cpuStartTiming) interrumpe unas 4000 veces por segundo y se apunta dónde estaba el ARM9.
// La rutina de interrupciones de calico guarda {r0-r3, r12, lr} en lo alto de la pila IRQ con lr = la
// instrucción interrumpida, así que basta con leer __sp_irq - 4. Con cada dirección va el PC MIPS del
// bloque traducido que la contiene (0 si no es código traducido).
//
// En la DSi: cada segundo se escribe lo nuevo en sd:/ps1/sparstation_prof.bin (pares; las palabras
// 0xFFFExxxx = segundo, 0xFFFFxxxx = marca, con 0 detrás). En melonDS (autoprueba): tools/dsbench con
// DSB_PROF=archivo lee el anillo de la RAM en cada frame (busca la marca «SPPROFRG»).
#include <nds.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "psx.h"
#include "prof.h"

#define PROF_PATH "sd:/ps1/sparstation_prof.bin"
#ifdef PSX_SELFTEST
#define RING 2048    // pares; dsbench lo vacía en cada frame (unas 66 muestras)
#else
#define RING 8192    // pares (2 s de margen)
#endif
#define HZ 3989      // no múltiplo de 60: que no vaya al compás de la vblank

extern char __sp_irq[];
static struct {
	char magic[8];
	volatile u32 wpos;
	u32 regions[6];
	u32 ring[RING * 2];
} pr = { "SPPROFRG" };
static u32 rpos;
static FILE *pf;
static TickTask task;

static void prof_tick(TickTask *t)
{
	u32 a = *(volatile u32 *)((uintptr_t)__sp_irq - 4), i = (pr.wpos & (RING - 1)) * 2;
	(void)t;
	pr.ring[i] = a;
	pr.ring[i + 1] = dyn_block_of(a);
	pr.wpos++;
}

static void put_tag(u32 tag)
{
	u32 w[2] = { tag, 0 };
	if (pf) fwrite(w, 4, 2, pf);
}

void prof_start(const char *game)
{
	u32 hdr[16] = { 0x46525053, 0x31464f52 };   // "SPRFROF1": marca de formato
	dyn_regions(pr.regions);
	rpos = pr.wpos = 0;
	tickTaskStart(&task, prof_tick, TICK_FREQ / HZ, TICK_FREQ / HZ);
#ifndef PSX_SELFTEST
	pf = fopen(PROF_PATH, "ab");
	if (!pf) return;
	hdr[2] = HZ;
	memcpy(hdr + 3, pr.regions, sizeof pr.regions);
	fwrite(hdr, 4, 16, pf);
	{
		char name[64] = { 0 };
		snprintf(name, sizeof name, "%s", game);
		fwrite(name, 1, sizeof name, pf);
	}
#else
	(void)hdr; (void)game;
#endif
}

// Escribe lo muestreado desde la última vez (lo llama el medidor una vez por segundo)
void prof_second(u32 second)
{
	u32 w = pr.wpos;
	if (!pf) return;
	if (w - rpos > RING) rpos = w - RING;   // no debería pasar: se perdieron muestras
	put_tag(0xfffe0000u | (second & 0xffff));
	while (rpos != w) {   // en trozos contiguos del anillo
		u32 i = rpos & (RING - 1), n = RING - i;
		if (n > w - rpos) n = w - rpos;
		fwrite(&pr.ring[i * 2], 8, n, pf);
		rpos += n;
	}
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

#ifdef DIV_TRACE
// Pruebas: quién llama a la división por software (con -Wl,--wrap=__aeabi_idiv); dsbench lo saca con DSB_MAGIC=SPDIVTRC
int __real___aeabi_idiv(int a, int b);
struct { char magic[8]; u32 n, pc[256], cnt[256]; } div_trace = { "SPDIVTRC" };
int __wrap___aeabi_idiv(int a, int b)
{
	u32 ra = (u32)__builtin_return_address(0), i;
	for (i = 0; i < div_trace.n && div_trace.pc[i] != ra; i++) { }
	if (i < 256) { if (i == div_trace.n) { div_trace.pc[i] = ra; div_trace.n++; } div_trace.cnt[i]++; }
	return __real___aeabi_idiv(a, b);
}
#endif
