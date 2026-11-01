// CPU R3000A (MIPS I) por intérprete. Respeta el delay slot de los saltos y el de las cargas,
// que muchos juegos y la BIOS dan por hecho.
#include "psx.h"

PSX_FAST PsxCore psx_core;   // registros de la GTE y de la CPU (ver psx.h)
u32 psx_hook_pc;
void (*psx_tty)(int c);

#define R(n) cpu.r[n]
#define SR cpu.cop0[12]
#define CAUSE cpu.cop0[13]
#define EPC cpu.cop0[14]

enum { EX_INT = 0, EX_ADEL = 4, EX_ADES = 5, EX_SYS = 8, EX_BP = 9, EX_RI = 10, EX_CPU = 11, EX_OV = 12 };

PSX_FAST static u32 nlr, nlv;   // carga emitida por la instrucción actual

void cpu_reset(void)
{
	memset(&cpu, 0, sizeof cpu);
	cpu.pc = 0xbfc00000;
	cpu.next_pc = cpu.pc + 4;
	SR = 0x10900000;
	cpu.cop0[15] = 0x00000002;   // PRId
	nlr = 0;
}

static inline void set_reg(u32 r, u32 v)
{
	cpu.r[r] = v;
	if (cpu.lr == r) cpu.lr = 0;   // la escritura directa gana a la carga pendiente
}

static inline void load_reg(u32 r, u32 v)
{
	nlr = r;
	nlv = v;
}

#ifdef PSX_TRACE_PC
#include <stdio.h>
u32 trace_ring[256];
u32 trace_pos;
int trace_live;
void mips_dis(u32 pc, u32 op);
static void dump_ring(u32 code)
{
	static int done;
	if (done++) return;
	printf("\n=== excepción %u en %08x; últimas instrucciones: ===\n", code, cpu.cur_pc);
	for (u32 i = 0; i < 256; i++) {
		u32 pc = trace_ring[(trace_pos + i) & 255];
		if (pc) mips_dis(pc, bus_read32(pc));
	}
	for (int i = 0; i < 32; i++) printf("r%-2d=%08x%s", i, cpu.r[i], i % 4 == 3 ? "\n" : "  ");
}
#endif

static void exception(u32 code)
{
#ifdef DBG_IRQCOUNT
	{ extern u32 dbg_exc_count; dbg_exc_count++; }
#endif
#ifdef PSX_TRACE_PC
	if (code == EX_RI || code == EX_ADEL || code == EX_ADES) dump_ring(code);
#endif
	u32 handler = (SR & (1 << 22)) ? 0xbfc00180 : 0x80000080;
	SR = (SR & ~0x3f) | ((SR << 2) & 0x3f);
	CAUSE = (CAUSE & ~0x7c) | (code << 2);
	if (cpu.in_delay) {
		EPC = cpu.cur_pc - 4;
		CAUSE |= 1u << 31;
	} else {
		EPC = cpu.cur_pc;
		CAUSE &= ~(1u << 31);
	}
	cpu.pc = handler;
	cpu.next_pc = handler + 4;
	cpu.branching = 0;
}

static inline void branch(u32 target)
{
	cpu.next_pc = target;
	cpu.branching = 1;
}

static inline u32 fetch(u32 pc)
{
	u32 p = pc & 0x1fffffff;
	if (p < 0x800000) return *(u32 *)(psx_ram + (p & 0x1fffff));
	if (p >= 0x1fc00000 && p < 0x1fc80000) return *(u32 *)(psx_bios + (p & 0x7ffff));
	return bus_read32(pc);
}

static inline int isolated(void) { return SR & (1 << 16); }

// Atajos para RAM y scratchpad, que son casi todos los accesos; el resto va al bus
#define RAM_ADDR(a) (((a) & 0x1fffffff) < 0x800000)
#define SCRATCH_ADDR(a) (((a) & 0x1ffffc00) == 0x1f800000)

static inline u32 rd32(u32 a)
{
	if (RAM_ADDR(a)) return *(u32 *)(psx_ram + (a & 0x1ffffc));
	if (SCRATCH_ADDR(a)) return *(u32 *)(psx_scratch + (a & 0x3fc));
	return bus_read32(a);
}

static inline u32 rd16(u32 a)
{
	if (RAM_ADDR(a)) return *(u16 *)(psx_ram + (a & 0x1ffffe));
	if (SCRATCH_ADDR(a)) return *(u16 *)(psx_scratch + (a & 0x3fe));
	return bus_read16(a);
}

static inline u32 rd8(u32 a)
{
	if (RAM_ADDR(a)) return psx_ram[a & 0x1fffff];
	if (SCRATCH_ADDR(a)) return psx_scratch[a & 0x3ff];
	return bus_read8(a);
}

static inline void wr32(u32 a, u32 v)
{
	if (RAM_ADDR(a)) { *(u32 *)(psx_ram + (a & 0x1ffffc)) = v; DYN_CHECK_WRITE(a); }
	else if (SCRATCH_ADDR(a)) *(u32 *)(psx_scratch + (a & 0x3fc)) = v;
	else bus_write32(a, v);
}

static inline void wr16(u32 a, u32 v)
{
	if (RAM_ADDR(a)) { *(u16 *)(psx_ram + (a & 0x1ffffe)) = (u16)v; DYN_CHECK_WRITE(a); }
	else if (SCRATCH_ADDR(a)) *(u16 *)(psx_scratch + (a & 0x3fe)) = (u16)v;
	else bus_write16(a, v);
}

static inline void wr8(u32 a, u32 v)
{
	if (RAM_ADDR(a)) { psx_ram[a & 0x1fffff] = (u8)v; DYN_CHECK_WRITE(a); }
	else if (SCRATCH_ADDR(a)) psx_scratch[a & 0x3ff] = (u8)v;
	else bus_write8(a, v);
}

static void op_cop0(u32 op)
{
	u32 rt = (op >> 16) & 31, rd = (op >> 11) & 31;
	switch ((op >> 21) & 31) {
	case 0x00: load_reg(rt, cpu.cop0[rd]); break;   // MFC0
	case 0x04:                                        // MTC0
		if (rd == 13) CAUSE = (CAUSE & ~0x300) | (R(rt) & 0x300);
		else cpu.cop0[rd] = R(rt);
		break;
	case 0x10:                                        // RFE
		if ((op & 0x3f) == 0x10) SR = (SR & ~0xf) | ((SR >> 2) & 0xf);
		break;
	default: break;
	}
}

// Coprocesador deshabilitado en SR: excepción «Coprocessor Unusable» con su número en CAUSE.CE
static int cop_unusable(int n)
{
	if (SR & (1u << (28 + n))) return 0;
	exception(EX_CPU);
	CAUSE = (CAUSE & ~(3u << 28)) | ((u32)n << 28);
	return 1;
}

static void op_cop2(u32 op)
{
	u32 rt = (op >> 16) & 31, rd = (op >> 11) & 31;
	if (cop_unusable(2)) return;
	if (op & (1 << 25)) { gte_command(op); return; }
	switch ((op >> 21) & 31) {
	case 0x00: load_reg(rt, gte_read(rd)); break;          // MFC2
	case 0x02: load_reg(rt, gte_read(rd + 32)); break;     // CFC2
	case 0x04: gte_write(rd, R(rt)); break;                // MTC2
	case 0x06: gte_write(rd + 32, R(rt)); break;           // CTC2
	default: exception(EX_RI); break;
	}
}

static void execute(u32 op)
{
	u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
	u32 imm = op & 0xffff, simm = (u32)(s32)(s16)imm;
	u32 a, v;

	switch (op >> 26) {
	case 0x00:   // SPECIAL
		switch (op & 0x3f) {
		case 0x00: set_reg(rd, R(rt) << ((op >> 6) & 31)); break;               // SLL
		case 0x02: set_reg(rd, R(rt) >> ((op >> 6) & 31)); break;               // SRL
		case 0x03: set_reg(rd, (u32)((s32)R(rt) >> ((op >> 6) & 31))); break;   // SRA
		case 0x04: set_reg(rd, R(rt) << (R(rs) & 31)); break;                    // SLLV
		case 0x06: set_reg(rd, R(rt) >> (R(rs) & 31)); break;                    // SRLV
		case 0x07: set_reg(rd, (u32)((s32)R(rt) >> (R(rs) & 31))); break;        // SRAV
		case 0x08: branch(R(rs)); break;                                         // JR
		case 0x09: v = R(rs); set_reg(rd, cpu.pc + 4); branch(v); break;         // JALR
		case 0x0c: exception(EX_SYS); break;                                     // SYSCALL
		case 0x0d: exception(EX_BP); break;                                      // BREAK
		case 0x10: set_reg(rd, cpu.hi); break;                                   // MFHI
		case 0x11: cpu.hi = R(rs); break;                                        // MTHI
		case 0x12: set_reg(rd, cpu.lo); break;                                   // MFLO
		case 0x13: cpu.lo = R(rs); break;                                        // MTLO
		case 0x18: { s64 m = (s64)(s32)R(rs) * (s32)R(rt); cpu.lo = (u32)m; cpu.hi = (u32)(m >> 32); break; }   // MULT
		case 0x19: { u64 m = (u64)R(rs) * R(rt); cpu.lo = (u32)m; cpu.hi = (u32)(m >> 32); break; }             // MULTU
		case 0x1a: {   // DIV
			s32 n = (s32)R(rs), d = (s32)R(rt);
			if (d == 0) { cpu.hi = (u32)n; cpu.lo = n >= 0 ? 0xffffffff : 1; }
			else if ((u32)n == 0x80000000 && d == -1) { cpu.hi = 0; cpu.lo = 0x80000000; }
			else { cpu.lo = (u32)(n / d); cpu.hi = (u32)(n % d); }
			break;
		}
		case 0x1b: {   // DIVU
			u32 n = R(rs), d = R(rt);
			if (d == 0) { cpu.hi = n; cpu.lo = 0xffffffff; }
			else { cpu.lo = n / d; cpu.hi = n % d; }
			break;
		}
		case 0x20: {   // ADD
			u32 x = R(rs), y = R(rt), s = x + y;
			if (~(x ^ y) & (x ^ s) & 0x80000000) exception(EX_OV); else set_reg(rd, s);
			break;
		}
		case 0x21: set_reg(rd, R(rs) + R(rt)); break;   // ADDU
		case 0x22: {   // SUB
			u32 x = R(rs), y = R(rt), s = x - y;
			if ((x ^ y) & (x ^ s) & 0x80000000) exception(EX_OV); else set_reg(rd, s);
			break;
		}
		case 0x23: set_reg(rd, R(rs) - R(rt)); break;                      // SUBU
		case 0x24: set_reg(rd, R(rs) & R(rt)); break;                      // AND
		case 0x25: set_reg(rd, R(rs) | R(rt)); break;                      // OR
		case 0x26: set_reg(rd, R(rs) ^ R(rt)); break;                      // XOR
		case 0x27: set_reg(rd, ~(R(rs) | R(rt))); break;                   // NOR
		case 0x2a: set_reg(rd, (s32)R(rs) < (s32)R(rt)); break;            // SLT
		case 0x2b: set_reg(rd, R(rs) < R(rt)); break;                      // SLTU
		default: exception(EX_RI); break;
		}
		break;
	case 0x01: {   // REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL
		int ge = (op >> 16) & 1, link = ((op >> 17) & 0xf) == 8;
		int take = ge ? (s32)R(rs) >= 0 : (s32)R(rs) < 0;
		u32 ret = cpu.pc + 4;
		if (link) set_reg(31, ret);
		if (take) branch(cpu.pc + (simm << 2)); else cpu.branching = 1;
		break;
	}
	case 0x02: branch((cpu.pc & 0xf0000000) | ((op & 0x3ffffff) << 2)); break;                       // J
	case 0x03: set_reg(31, cpu.pc + 4); branch((cpu.pc & 0xf0000000) | ((op & 0x3ffffff) << 2)); break;   // JAL
	case 0x04: if (R(rs) == R(rt)) branch(cpu.pc + (simm << 2)); else cpu.branching = 1; break;      // BEQ
	case 0x05: if (R(rs) != R(rt)) branch(cpu.pc + (simm << 2)); else cpu.branching = 1; break;      // BNE
	case 0x06: if ((s32)R(rs) <= 0) branch(cpu.pc + (simm << 2)); else cpu.branching = 1; break;     // BLEZ
	case 0x07: if ((s32)R(rs) > 0) branch(cpu.pc + (simm << 2)); else cpu.branching = 1; break;      // BGTZ
	case 0x08: {   // ADDI
		u32 x = R(rs), s = x + simm;
		if (~(x ^ simm) & (x ^ s) & 0x80000000) exception(EX_OV); else set_reg(rt, s);
		break;
	}
	case 0x09: set_reg(rt, R(rs) + simm); break;                   // ADDIU
	case 0x0a: set_reg(rt, (s32)R(rs) < (s32)simm); break;         // SLTI
	case 0x0b: set_reg(rt, R(rs) < simm); break;                   // SLTIU
	case 0x0c: set_reg(rt, R(rs) & imm); break;                    // ANDI
	case 0x0d: set_reg(rt, R(rs) | imm); break;                    // ORI
	case 0x0e: set_reg(rt, R(rs) ^ imm); break;                    // XORI
	case 0x0f: set_reg(rt, imm << 16); break;                      // LUI
	case 0x10: op_cop0(op); break;
	case 0x11: case 0x13: cop_unusable((op >> 26) & 3); break;   // COP1 y COP3 no existen: no hacen nada
	case 0x30: case 0x31: case 0x33: case 0x38: case 0x39: case 0x3b:   // LWCn / SWCn de coprocesadores vacíos
		cop_unusable((op >> 26) & 3);
		break;
	case 0x12: op_cop2(op); break;
	case 0x20:   // LB
		a = R(rs) + simm;
		load_reg(rt, (u32)(s32)(s8)rd8(a));
		break;
	case 0x21:   // LH
		a = R(rs) + simm;
		if (a & 1) { cpu.cop0[8] = a; exception(EX_ADEL); break; }
		load_reg(rt, (u32)(s32)(s16)rd16(a));
		break;
	case 0x22: case 0x26: {   // LWL, LWR
		u32 cur = (cpu.lr == rt) ? cpu.lv : R(rt);
		u32 w, sh;
		a = R(rs) + simm;
		w = rd32(a & ~3);
		sh = (a & 3) * 8;
		if ((op >> 26) == 0x22) v = (cur & (0x00ffffff >> sh)) | (w << (24 - sh));
		else v = (cur & ~(0xffffffffu >> sh)) | (w >> sh);
		load_reg(rt, v);
		break;
	}
	case 0x23:   // LW
		a = R(rs) + simm;
		if (a & 3) { cpu.cop0[8] = a; exception(EX_ADEL); break; }
		load_reg(rt, rd32(a));
		break;
	case 0x24: a = R(rs) + simm; load_reg(rt, rd8(a)); break;   // LBU
	case 0x25:   // LHU
		a = R(rs) + simm;
		if (a & 1) { cpu.cop0[8] = a; exception(EX_ADEL); break; }
		load_reg(rt, rd16(a));
		break;
	case 0x28: if (!isolated()) wr8(R(rs) + simm, R(rt)); break;   // SB
	case 0x29:   // SH
		a = R(rs) + simm;
		if (a & 1) { cpu.cop0[8] = a; exception(EX_ADES); break; }
		if (!isolated()) wr16(a, R(rt));
		break;
	case 0x2a: case 0x2e: {   // SWL, SWR
		u32 w, sh;
		if (isolated()) break;
		a = R(rs) + simm;
		w = rd32(a & ~3);
		sh = (a & 3) * 8;
		if ((op >> 26) == 0x2a) v = (w & (0xffffff00u << sh)) | (R(rt) >> (24 - sh));
		else v = (w & ~(0xffffffffu << sh)) | (R(rt) << sh);
		wr32(a & ~3, v);
		break;
	}
	case 0x2b:   // SW
		a = R(rs) + simm;
		if (a & 3) { cpu.cop0[8] = a; exception(EX_ADES); break; }
		if (!isolated()) wr32(a, R(rt));
		break;
	case 0x32:   // LWC2
		if (cop_unusable(2)) break;
		a = R(rs) + simm;
		if (a & 3) { cpu.cop0[8] = a; exception(EX_ADEL); break; }
		gte_write(rt, rd32(a));
		break;
	case 0x3a:   // SWC2
		if (cop_unusable(2)) break;
		a = R(rs) + simm;
		if (a & 3) { cpu.cop0[8] = a; exception(EX_ADES); break; }
		wr32(a, gte_read(rt));
		break;
	default:
		exception(EX_RI);
		break;
	}
}

#ifdef PSX_OPSTAT
u32 psx_opstat[18];   // perfil de instrucciones en el PC (ver pc/main.c --opstat)
int psx_opstat_on;
#endif

#ifdef PSX_BLOCKY
// Imita los tiempos del dynarec en el PC: los ciclos se suman por bloques y las interrupciones
// y eventos solo se atienden entre bloques (tras el delay slot de un salto o cada 64 instrucciones).
static u32 blk_pending;
static int blk_len;
#endif

void cpu_run(u32 until)
{
	psx_run_until = until;
#ifdef PSX_BLOCKY
	while (blk_len || TIME_BEFORE(psx_cycles, psx_run_until)) {
#else
	while (TIME_BEFORE(psx_cycles, psx_run_until)) {
#endif
		u32 op;

		if (psx_hook_pc && cpu.pc == psx_hook_pc) psx_hook();
#ifdef PSX_TTY
		if (psx_tty) {
			u32 p = cpu.pc & 0x1fffffff;
			if ((p == 0xa0 && R(9) == 0x3c) || (p == 0xb0 && R(9) == 0x3d)) psx_tty(R(4) & 0xff);
		}
#endif

		cpu.cur_pc = cpu.pc;
		cpu.in_delay = cpu.branching;

#ifdef PSX_BLOCKY
		if (!blk_len && (SR & 1) && (SR & CAUSE & 0x700)) {
#else
		if ((SR & 1) && (SR & CAUSE & 0x700)) {
#endif
			// Una instrucción del GTE en la boca de la interrupción se ejecuta igual:
			// la BIOS la salta al volver, así que hay que hacerla ahora.
			if (cpu.lr) { cpu.r[cpu.lr] = cpu.lv; cpu.lr = 0; }
			op = fetch(cpu.pc);
			if ((op >> 25) == 0x25) gte_command(op);
			exception(EX_INT);
			psx_cycles += 2;
			continue;
		}

		if (cpu.pc & 3) {
			cpu.cop0[8] = cpu.pc;
			exception(EX_ADEL);
			continue;
		}

		op = fetch(cpu.pc);
#ifdef PSX_TRACE_PC
		trace_ring[trace_pos++ & 255] = cpu.pc;
		{
			extern int trace_live;
			if (trace_live && cpu.pc >= 0x80093000 && cpu.pc < 0x80094000) {
				mips_dis(cpu.pc, op);
				if (--trace_live == 0) printf("--- fin de traza ---\n");
			}
		}
#endif
#ifdef PSX_OPSTAT
		if (psx_opstat_on) {
			u32 o = op >> 26, base = (op >> 21) & 31, a = R(base) + (u32)(s32)(s16)op, k;
			int ram = (a & 0x1fffffff) < 0x800000, scr = (a & 0x1ffffc00) == 0x1f800000;
			psx_opstat[0]++;
			if (cpu.in_delay) psx_opstat[1]++;
			if (op == 0) k = 2;
			else if (o >= 0x20 && o <= 0x26) k = base == 29 ? 3 : ram ? 4 : scr ? 5 : 6;
			else if (o >= 0x28 && o <= 0x2e) k = base == 29 ? 7 : ram ? 8 : scr ? 9 : 10;
			else if (o == 0x12) k = (op & (1 << 25)) ? 11 : 12;
			else if (o == 0x32 || o == 0x3a) k = 12;
			else if (o == 0 && (op & 0x3f) >= 0x10 && (op & 0x3f) <= 0x1b) k = 13;
			else if (o == 0 && ((op & 0x3f) == 8 || (op & 0x3f) == 9)) k = 14;
			else if (o >= 1 && o <= 7) k = 15;
			else if (o == 0x10) k = 16;
			else k = 17;
			psx_opstat[k]++;
		}
#endif
		cpu.pc = cpu.next_pc;
		cpu.next_pc = cpu.pc + 4;
		cpu.branching = 0;

		execute(op);

		if (cpu.lr) cpu.r[cpu.lr] = cpu.lv;
		cpu.lr = nlr;
		cpu.lv = nlv;
		nlr = 0;
#ifdef PSX_BLOCKY
		// como el dynarec: las cargas llegan ya, sin retraso
		if (cpu.lr) { cpu.r[cpu.lr] = cpu.lv; cpu.lr = 0; }
#endif
		cpu.r[0] = 0;
#ifdef DBG_IDLE
		{ extern void dbg_idle_step(u32 op, u32 cur, u32 next); dbg_idle_step(op, cpu.cur_pc, cpu.pc); }
		{ extern void dbg_fn_step(u32 op, u32 cur, u32 next); dbg_fn_step(op, cpu.cur_pc, cpu.pc); }
#endif

#ifdef PSX_BLOCKY
		blk_pending += 2;
		if (cpu.in_delay || ++blk_len >= 64 || (op >> 26) == 0x10 || (op & 0xfc00003e) == 0x0000000c) {
			psx_cycles += blk_pending;
			blk_pending = 0;
			blk_len = 0;
		}
#else
		psx_cycles += 2;
#endif
	}
}

// ---- servicios para el dynarec ----

// Ejecuta con el intérprete una instrucción que no es salto (GTE, COP0, LWL/SWR, DIV...).
// En el dynarec las cargas no tienen retraso. Devuelve 1 si la instrucción provocó una excepción.
int cpu_dyn_interp(u32 op, u32 pc)
{
	STAT_ADD(dyn_interp_ops, 1);
	cpu.cur_pc = pc;
	cpu.in_delay = 0;
	cpu.pc = pc + 4;
	cpu.next_pc = pc + 8;
	cpu.branching = 0;
	cpu.lr = 0;
	nlr = 0;
	execute(op);
	if (nlr) cpu.r[nlr] = nlv;
	nlr = 0;
	cpu.r[0] = 0;
	return cpu.pc != pc + 4;
}

// Lanza una excepción (SYSCALL, BREAK...) en la instrucción pc
void cpu_dyn_exception(u32 code, u32 pc)
{
	cpu.cur_pc = pc;
	cpu.in_delay = 0;
	exception(code);
}

// Atiende una interrupción pendiente entre bloques; devuelve 1 si saltó al manejador
int cpu_dyn_irq(void)
{
	if ((SR & 1) && (SR & CAUSE & 0x700)) {
		u32 op = fetch(cpu.pc);
		if ((op >> 25) == 0x25) gte_command(op);
		cpu.cur_pc = cpu.pc;
		cpu.in_delay = 0;
		exception(EX_INT);
		return 1;
	}
	return 0;
}

// Avanza con el intérprete hasta un punto «limpio» (sin delay slot ni carga pendiente)
void cpu_interp_step(void)
{
	u32 saved_until = psx_run_until;   // cpu_run lo cambia; el despachador del dynarec lo necesita
	psx_sched_req = saved_until;
	STAT_ADD(dyn_interp_steps, 1);
	cpu.next_pc = cpu.pc + 4;
	cpu.branching = 0;
	cpu.lr = 0;
	do cpu_run(psx_cycles + 2);
	while (cpu.branching || cpu.next_pc != cpu.pc + 4);
	if (cpu.lr) cpu.r[cpu.lr] = cpu.lv;
	cpu.lr = 0;
	cpu.r[0] = 0;
	psx_run_until = (s32)(psx_sched_req - saved_until) < 0 ? psx_sched_req : saved_until;   // sin perder lo que pidió una E/S
}

void cpu_state(PsxState *s)
{
	STATE(s, cpu);
	STATE(s, nlr);
	STATE(s, nlv);
}
