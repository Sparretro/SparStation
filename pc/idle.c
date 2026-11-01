// Detector de bucles de espera (solo PC, -DDBG_IDLE): en cada vuelta de un bucle (salto hacia atrás) se
// compara el estado con el de la vuelta anterior. Si no se escribió memoria y los registros son iguales, esa
// vuelta no hizo nada: el programa estaba esperando (a una interrupción, a un registro de hardware o a la
// memoria). Cuenta las instrucciones gastadas así por cabeza de bucle.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"

void mips_dis(u32 pc, u32 op);
int dbg_idle_on;
static u64 total, stores, excs;
static u64 kern, rom;
static u32 last_cur;

typedef struct {
	u32 head, branch;
	u32 regs[34];
	u64 snap_insn, snap_stores, snap_exc;
	u64 idem, nostore, all;
	u32 iters, idem_iters;
	int snap;
} Loop;
#define NL 8192
static Loop loops[NL];

static Loop *find(u32 head)
{
	u32 h = (head >> 2) * 2654435761u % NL;
	for (int i = 0; i < NL; i++, h = (h + 1) % NL) {
		if (loops[h].head == head) return &loops[h];
		if (!loops[h].head) { loops[h].head = head; return &loops[h]; }
	}
	return NULL;
}

void dbg_idle_step(u32 op, u32 cur, u32 next)
{
	u32 o = op >> 26;
	if (!dbg_idle_on) return;
	total++;
	{ u32 p = cur & 0x1fffffff; if (p < 0x10000) kern++; else if (p >= 0x1fc00000) rom++; }
	if ((o >= 0x28 && o <= 0x2e) || o == 0x3a) stores++;
	if (cur != last_cur + 4 && !cpu.in_delay && (cur & 0x1fffffff) == 0x80) excs++;   // entrada al manejador
	if ((cur & 0x1fffffff) == 0x80 || (cur & 0x1fffffff) == 0x1fc00180) excs++;
	last_cur = cur;
	if (!cpu.in_delay || next > cur - 4 || cur - next > 8192) return;
	Loop *l = find(next);
	if (!l) return;
	u32 regs[34];
	memcpy(regs, cpu.r, 32 * 4);
	regs[32] = cpu.hi; regs[33] = cpu.lo;
	if (l->snap && total - l->snap_insn < 20000) {
		u64 len = total - l->snap_insn;
		l->all += len;
		l->iters++;
		if (stores == l->snap_stores && excs == l->snap_exc) {
			l->nostore += len;
			if (!memcmp(regs, l->regs, sizeof regs)) { l->idem += len; l->idem_iters++; }
		}
	}
	l->branch = cur - 4;
	memcpy(l->regs, regs, sizeof regs);
	l->snap_insn = total; l->snap_stores = stores; l->snap_exc = excs;
	l->snap = 1;
}

static int cmp_idem(const void *a, const void *b)
{
	const Loop *x = a, *y = b;
	u64 kx = x->idem > x->nostore / 2 ? x->idem : x->nostore, ky = y->idem > y->nostore / 2 ? y->idem : y->nostore;
	if (getenv("IDLE_ALL")) { kx = x->all; ky = y->all; }
	return kx < ky ? 1 : kx > ky ? -1 : 0;
}

void dbg_idle_report(void)
{
	u64 idem = 0, nost = 0;
	for (int i = 0; i < NL; i++) { idem += loops[i].idem; nost += loops[i].nostore; }
	qsort(loops, NL, sizeof(Loop), cmp_idem);
	printf("\n--- bucles de espera: %llu instrucciones; vueltas sin efecto %.1f%%, sin escribir memoria %.1f%% ---\n",
	       (unsigned long long)total, 100.0 * idem / total, 100.0 * nost / total);
	printf("kernel en RAM (<0x10000) %.2f%%, ROM %.2f%%\n", 100.0 * kern / total, 100.0 * rom / total);
	int nshow = getenv("IDLE_TOP") ? atoi(getenv("IDLE_TOP")) : 8;
	for (int i = 0; i < nshow && loops[i].head; i++) {
		Loop *l = &loops[i];
		if (!l->nostore && !getenv("IDLE_ALL")) break;
		printf("\n[%d] bucle %08x..%08x  sin efecto %.2f%%  sin escribir %.2f%%  total %.2f%%  vueltas %u (%u sin efecto), %.1f instr/vuelta\n",
		       i, l->head, l->branch, 100.0 * l->idem / total, 100.0 * l->nostore / total, 100.0 * l->all / total,
		       l->iters, l->idem_iters, l->iters ? (double)l->all / l->iters : 0);
		if (l->branch - l->head <= 48 * 4)
			for (u32 a = l->head; a <= l->branch + 4; a += 4) mips_dis(a, bus_read32(a));
	}
}

// ---- perfil por función: cada instrucción cuenta para el destino de «jal» más cercano por debajo ----
static u32 *pc_hist;     // instrucciones por dirección de la RAM (2 MB / 4)
static u8 *is_fn;        // destinos de jal/jalr vistos
void dbg_fn_step(u32 op, u32 cur, u32 next)
{
	u32 p = cur & 0x1fffff;
	if (!dbg_idle_on || (cur & 0x1fffffff) >= 0x800000) return;
	if (!pc_hist) { pc_hist = calloc(1 << 19, 4); is_fn = calloc(1 << 19, 1); }
	pc_hist[p >> 2]++;
	{ static u32 want = 1; if (want == 1) want = getenv("FN_CALLERS") ? (u32)strtoul(getenv("FN_CALLERS"), NULL, 16) : 0;
	  if (want && cur == want) { static u32 seen[16], cnt[16]; for (int k = 0; k < 16; k++) if (seen[k] == cpu.r[31] || !seen[k]) { if (!seen[k]) printf("llamada desde %08x\n", cpu.r[31] - 8); seen[k] = cpu.r[31]; cnt[k]++; break; } } }
	if (cpu.in_delay && (next & 0x1fffffff) < 0x800000) {
		u32 prev = bus_read32(cur - 4);
		if ((prev >> 26) == 3 || (prev >> 26 == 0 && (prev & 0x3f) == 9)) is_fn[(next & 0x1fffff) >> 2] = 1;
	}
}
typedef struct { u32 start, end; u64 n; } Fn;
static int cmp_fn(const void *a, const void *b) { const Fn *x = a, *y = b; return x->n < y->n ? 1 : x->n > y->n ? -1 : 0; }
void dbg_fn_report(void)
{
	static Fn fns[1 << 16];
	int nf = 0;
	u64 tot = 0;
	if (!pc_hist) return;
	for (u32 i = 0, cur = 0; i < (1u << 19); i++) {
		if (is_fn[i] || i == 0) { if (nf < (1 << 16)) { fns[nf].start = 0x80000000 | i << 2; fns[nf].n = 0; nf++; } }
		fns[nf - 1].end = 0x80000000 | i << 2;
		fns[nf - 1].n += pc_hist[i];
		tot += pc_hist[i];
		(void)cur;
	}
	qsort(fns, nf, sizeof(Fn), cmp_fn);
	int top = getenv("FN_TOP") ? atoi(getenv("FN_TOP")) : 25;
	printf("\n--- funciones (RAM) ---\n");
	for (int i = 0; i < top && i < nf; i++) {
		u32 e = fns[i].start;   // fin real: el último pc con muestras antes de la siguiente función
		for (u32 a = fns[i].start; a <= fns[i].end; a += 4) if (pc_hist[(a & 0x1fffff) >> 2]) e = a;
		printf("%08x..%08x  %5.2f%%  (%u instr)\n", fns[i].start, e, 100.0 * fns[i].n / tot, (e - fns[i].start) / 4 + 1);
	}
}
void dbg_fn_dump(void)
{
	char *s = getenv("FN_DUMP");
	if (!s || !pc_hist) return;
	u32 a0 = (u32)strtoul(s, NULL, 16);
	for (u32 a = a0; a < a0 + 0x200; a += 4) if (pc_hist[(a & 0x1fffff) >> 2]) printf("%08x %u\n", a, pc_hist[(a & 0x1fffff) >> 2]);
}
