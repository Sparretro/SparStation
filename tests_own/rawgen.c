// Genera rawfuzz.c: funciones de instrucciones MIPS aleatorias en código máquina (no las haría un compilador):
// cargas con su retraso, saltos con su delay slot, MULT/DIV/HI/LO, LWL/LWR/SWL/SWR y la GTE (MTC2/MFC2/CTC2/CFC2
// y comandos). Cada función recibe en a0 un búfer: 256 bytes de memoria, y detrás los registros de entrada, que
// se guardan al final. uso: rawgen <semilla> <funciones> > rawfuzz.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static uint32_t rs = 1;
static uint32_t rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }
static int R(int n) { return (int)(rnd() % (uint32_t)n); }

#define FP 30          // base del búfer (no se toca)
#define REGS 256       // desplazamiento de los registros en el búfer
#define MAXW 160

static uint32_t code[MAXW];
static int n;

static void e(uint32_t w) { code[n++] = w; }
static uint32_t rt3(int rs_, int rt, int rd, int sa, int fn) { return (uint32_t)rs_ << 21 | (uint32_t)rt << 16 | (uint32_t)rd << 11 | (uint32_t)sa << 6 | (uint32_t)fn; }
static uint32_t it(int op, int rs_, int rt, uint32_t imm) { return (uint32_t)op << 26 | (uint32_t)rs_ << 21 | (uint32_t)rt << 16 | (imm & 0xffff); }

static int wreg(void) { return 1 + R(25); }                        // at..t9: los que se pueden escribir
static int rreg(void) { int r = R(28); return r == 26 ? FP : r == 27 ? 0 : r; }   // también cero y la base

// Una instrucción que no salta
static void plain(void)
{
	int d = wreg(), a = rreg(), b = rreg();
	switch (R(22)) {
	case 0: { static const int f[] = { 0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2a, 0x2b }; e(rt3(a, b, d, 0, f[R(8)])); break; }
	case 1: { static const int f[] = { 0x04, 0x06, 0x07 }; e(rt3(a, b, d, 0, f[R(3)])); break; }
	case 2: { static const int f[] = { 0x00, 0x02, 0x03 }; e(rt3(0, b, d, R(32), f[R(3)])); break; }
	case 3: { static const int o[] = { 0x09, 0x0c, 0x0d, 0x0e, 0x0a, 0x0b }; e(it(o[R(6)], a, d, R(3) ? rnd() & 0xffff : (uint32_t)(R(64) - 32))); break; }
	case 4: e(it(0x0f, 0, d, rnd())); break;                                        // LUI
	case 5: { static const int f[] = { 0x18, 0x19, 0x1a, 0x1b }; e(rt3(a, b, 0, 0, f[R(4)])); break; }   // MULT/DIV
	case 6: e(rt3(0, 0, d, 0, R(2) ? 0x10 : 0x12)); break;                          // MFHI/MFLO
	case 7: e(rt3(a, 0, 0, 0, R(2) ? 0x11 : 0x13)); break;                          // MTHI/MTLO
	case 8: case 9: case 10: {   // cargas desde el búfer
		static const int o[] = { 0x20, 0x24, 0x21, 0x25, 0x23, 0x22, 0x26 };
		int k = R(7), off = R(256);
		if (k == 2 || k == 3) off &= ~1;
		if (k == 4) off &= ~3;
		e(it(o[k], FP, d, (uint32_t)off));
		break;
	}
	case 11: case 12: {   // escrituras en el búfer
		static const int o[] = { 0x28, 0x29, 0x2b, 0x2a, 0x2e };
		int k = R(5), off = R(256);
		if (k == 1) off &= ~1;
		if (k == 2) off &= ~3;
		e(it(o[k], FP, a, (uint32_t)off));
		break;
	}
	case 13: e(0x48000000u | 4u << 21 | (uint32_t)a << 16 | (uint32_t)R(32) << 11); break;   // MTC2
	case 14: e(0x48000000u | 0u << 21 | (uint32_t)d << 16 | (uint32_t)R(32) << 11); break;   // MFC2
	case 15: e(0x48000000u | 6u << 21 | (uint32_t)a << 16 | (uint32_t)R(32) << 11); break;   // CTC2
	case 16: e(0x48000000u | 2u << 21 | (uint32_t)d << 16 | (uint32_t)R(32) << 11); break;   // CFC2
	case 17: {   // comando de la GTE
		static const int c[] = { 0x01, 0x06, 0x0c, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1b, 0x1c, 0x1e, 0x20, 0x28, 0x29, 0x2a, 0x2d, 0x2e, 0x30, 0x3d, 0x3e, 0x3f };
		e(0x4a000000u | (uint32_t)R(2) << 19 | (uint32_t)R(4) << 17 | (uint32_t)R(4) << 15 | (uint32_t)R(4) << 13 | (uint32_t)R(2) << 10 | (uint32_t)c[R(22)]);
		break;
	}
	case 18: { int off = R(64) * 4; e(it(0x32, FP, R(32), (uint32_t)off)); break; }   // LWC2
	case 19: { int off = R(64) * 4; e(it(0x3a, FP, R(32), (uint32_t)off)); break; }   // SWC2
	default: e(0); break;
	}
}

static void body(int len)
{
	int start = n;
	while (n - start < len) {
		int left = len - (n - start);
		if (left > 2 && R(6) == 0) {   // salto hacia delante dentro del cuerpo, con su delay slot
			int skip = R(left - 1 < 5 ? left - 1 : 5) + 1, a = rreg(), b = rreg();
			switch (R(4)) {
			case 0: e(it(0x04, a, b, (uint32_t)skip)); break;   // BEQ
			case 1: e(it(0x05, a, b, (uint32_t)skip)); break;   // BNE
			case 2: e(it(R(2) ? 0x06 : 0x07, a, 0, (uint32_t)skip)); break;   // BLEZ/BGTZ
			default: e(it(0x01, a, R(2), (uint32_t)skip)); break;   // BLTZ/BGEZ
			}
			plain();   // delay slot
		} else plain();
	}
}

int main(int argc, char **argv)
{
	int nf = argc > 2 ? atoi(argv[2]) : 100;
	rs = argc > 1 ? (uint32_t)atoi(argv[1]) * 2654435761u + 1 : 1;
	printf("// generado por rawgen.c\n#include \"fuzz.h\"\n\nu32 raw_code[%d][%d] __attribute__((aligned(16))) = {\n", nf, MAXW);
	for (int f = 0; f < nf; f++) {
		n = 0;
		e(it(0x09, 29, 29, (uint32_t)-48));                 // addiu sp, sp, -48
		for (int r = 16; r <= 23; r++) e(it(0x2b, 29, r, (uint32_t)(r - 16) * 4));
		e(it(0x2b, 29, FP, 32));
		e(it(0x2b, 29, 31, 36));
		e(rt3(4, 0, FP, 0, 0x21));                          // move fp, a0
		e(it(0x23, FP, 1, REGS + 32 * 4)); e(0); e(rt3(1, 0, 0, 0, 0x11));   // HI
		e(it(0x23, FP, 1, REGS + 33 * 4)); e(0); e(rt3(1, 0, 0, 0, 0x13));   // LO
		for (int r = 1; r <= 25; r++) e(it(0x23, FP, r, (uint32_t)(REGS + r * 4)));
		e(0);
		body(R(40) + 30);
		e(0); e(0);                                          // que acaben las cargas pendientes
		for (int r = 1; r <= 25; r++) e(it(0x2b, FP, r, (uint32_t)(REGS + r * 4)));
		e(rt3(0, 0, 1, 0, 0x10)); e(it(0x2b, FP, 1, REGS + 32 * 4));
		e(rt3(0, 0, 1, 0, 0x12)); e(it(0x2b, FP, 1, REGS + 33 * 4));
		for (int r = 16; r <= 23; r++) e(it(0x23, 29, r, (uint32_t)(r - 16) * 4));
		e(it(0x23, 29, FP, 32));
		e(it(0x23, 29, 31, 36));
		e(0);
		e(rt3(31, 0, 0, 0, 0x08));                           // jr ra
		e(it(0x09, 29, 29, 48));                             // addiu sp, sp, 48 (delay slot)
		printf("\t{");
		for (int i = 0; i < n; i++) printf("0x%08x,%s", code[i], i % 8 == 7 ? "\n\t " : " ");
		printf("},\n");
	}
	printf("};\nconst int raw_count = %d;\n", nf);
	return 0;
}
