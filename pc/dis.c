// Desensamblador MIPS mínimo para depurar
#include "../core/psx.h"
#include <stdio.h>
#include <stdlib.h>

static const char *rn[32] = { "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra" };

void mips_dis(u32 pc, u32 op)
{
	u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sh = (op >> 6) & 31;
	s32 si = (s16)op;
	u32 ui = op & 0xffff, tgt = pc + 4 + (si << 2);
	static const char *sp[64] = { [0] = "sll", [2] = "srl", [3] = "sra", [4] = "sllv", [6] = "srlv", [7] = "srav", [8] = "jr",
		[9] = "jalr", [12] = "syscall", [13] = "break", [16] = "mfhi", [17] = "mthi", [18] = "mflo", [19] = "mtlo",
		[24] = "mult", [25] = "multu", [26] = "div", [27] = "divu", [32] = "add", [33] = "addu", [34] = "sub", [35] = "subu",
		[36] = "and", [37] = "or", [38] = "xor", [39] = "nor", [42] = "slt", [43] = "sltu" };
	static const char *pr[64] = { [2] = "j", [3] = "jal", [4] = "beq", [5] = "bne", [6] = "blez", [7] = "bgtz", [8] = "addi",
		[9] = "addiu", [10] = "slti", [11] = "sltiu", [12] = "andi", [13] = "ori", [14] = "xori", [15] = "lui", [16] = "cop0",
		[18] = "cop2", [32] = "lb", [33] = "lh", [34] = "lwl", [35] = "lw", [36] = "lbu", [37] = "lhu", [38] = "lwr",
		[40] = "sb", [41] = "sh", [42] = "swl", [43] = "sw", [46] = "swr", [50] = "lwc2", [58] = "swc2" };
	u32 o = op >> 26;
	printf("%08x: %08x  ", pc, op);
	if (op == 0) { printf("nop\n"); return; }
	if (o == 0) {
		const char *n = sp[op & 63] ? sp[op & 63] : "???";
		if ((op & 63) < 4) printf("%s %s, %s, %u\n", n, rn[rd], rn[rt], sh);
		else if ((op & 63) == 8) printf("jr %s\n", rn[rs]);
		else printf("%s %s, %s, %s\n", n, rn[rd], rn[rs], rn[rt]);
	} else if (o == 1) printf("%s %s, %08x\n", (rt & 1) ? "bgez" : "bltz", rn[rs], tgt);
	else if (o == 2 || o == 3) printf("%s %08x\n", pr[o], (pc & 0xf0000000) | ((op & 0x3ffffff) << 2));
	else if (o >= 4 && o <= 7) printf("%s %s, %s, %08x\n", pr[o], rn[rs], rn[rt], tgt);
	else if (o == 15) printf("lui %s, 0x%04x\n", rn[rt], ui);
	else if (o >= 8 && o <= 14) printf("%s %s, %s, %d\n", pr[o], rn[rt], rn[rs], (o >= 12) ? (s32)ui : si);
	else if (o >= 32) printf("%s %s, %d(%s)\n", pr[o] ? pr[o] : "???", rn[rt], si, rn[rs]);
	else printf("%s\n", pr[o] ? pr[o] : "???");
}

