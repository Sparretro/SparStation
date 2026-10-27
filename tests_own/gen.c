// Genera fuzz.c: muchas funciones aleatorias (aritmética, memoria, saltos, llamadas, switch, bucles)
// para comparar el intérprete del PC con el dynarec de la DS. uso: gen <semilla> <funciones> > fuzz.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static uint32_t rs = 1;
static uint32_t rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }
static int R(int n) { return (int)(rnd() % (uint32_t)n); }

#define NV 8
static const char *ops[] = { "+", "-", "*", "&", "|", "^" };

static void ind(int d) { for (int i = 0; i < d; i++) printf("\t"); }

static void expr(void)
{
	int a = R(NV), b = R(NV);
	switch (R(16)) {
	case 0: case 1: case 2: printf("v%d %s v%d", a, ops[R(6)], b); break;
	case 3: printf("v%d %s 0x%xu", a, ops[R(6)], R(4) ? rnd() & 0xffff : rnd() * 7919u); break;
	case 4: printf("v%d << (v%d & 31)", a, b); break;
	case 5: printf("v%d >> (v%d & 31)", a, b); break;
	case 6: printf("(u32)((s32)v%d >> (v%d & 31))", a, b); break;
	case 7: printf("v%d << %d | v%d >> %d", a, R(31) + 1, b, R(31) + 1); break;
	case 8: printf("(u32)((s32)v%d < (s32)v%d)", a, b); break;
	case 9: printf("(u32)(v%d < v%d) + (u32)((s32)v%d < %d)", a, b, a, R(2000) - 1000); break;
	case 10: printf("v%d / (v%d | 1)", a, b); break;
	case 11: printf("(u32)((s32)v%d / (s32)((v%d & 0x7fff) | 1))", a, b); break;
	case 12: printf("v%d %% (v%d | 3)", a, b); break;
	case 13: printf("(u32)(((u64)v%d * v%d) >> 32)", a, b); break;
	case 14: printf("(u32)(((s64)(s32)v%d * (s32)v%d) >> 29)", a, b); break;
	default: printf("~v%d + %d", a, R(100)); break;
	}
}

static void stmt(int d, int depth)
{
	int x = R(NV), y = R(NV);
	ind(d);
	switch (R(depth < 2 ? 22 : 17)) {
	case 0: case 1: case 2: case 3: case 4: case 5: printf("v%d = ", x); expr(); printf(";\n"); break;
	case 6: printf("b8[(v%d + %d) & 63] = (u8)v%d;\n", y, R(64), x); break;
	case 7: printf("v%d += b8[(v%d + %d) & 63];\n", x, y, R(64)); break;
	case 8: printf("v%d ^= (u32)(s8)b8[v%d & 63];\n", x, y); break;
	case 9: printf("h16[(v%d >> 3) & 31] = (u16)v%d;\n", y, x); break;
	case 10: printf("v%d -= (u32)(s16)h16[v%d & 31] + h16[%d];\n", x, y, R(32)); break;
	case 11: printf("w32[v%d & 31] = v%d + %d;\n", y, x, R(9)); break;
	case 12: printf("v%d = w32[(v%d >> %d) & 31] ^ v%d;\n", x, y, R(8), x); break;
	case 13: printf("pk[v%d & 7].w = v%d; v%d += pk[(v%d + 1) & 7].w;\n", y, x, y, x); break;
	case 14: printf("v%d = hlp[v%d & 3](v%d, v%d);\n", x, y, x, R(NV)); break;
	case 15: printf("v%d = hlp%d(v%d, v%d) + v%d;\n", x, R(4), y, x, R(NV)); break;
	case 16: printf("sp[v%d & 255] = v%d; v%d += sp[(v%d + 3) & 255];\n", y, x, x, y); break;
	case 17: case 18:
		printf("if ((s32)v%d %s (s32)v%d) {\n", x, R(2) ? "<" : ">=", y);
		for (int i = R(3) + 1; i; i--) stmt(d + 1, depth + 1);
		ind(d); printf("} else {\n");
		for (int i = R(3) + 1; i; i--) stmt(d + 1, depth + 1);
		ind(d); printf("}\n");
		break;
	case 19: {
		int k = R(1000);
		printf("for (u32 i%d = 0; i%d < (v%d & 3) + 1; i%d++) {\n", k, k, y, k);
		for (int i = R(3) + 1; i; i--) stmt(d + 1, depth + 1);
		ind(d + 1); printf("v%d += i%d;\n", x, k);
		ind(d); printf("}\n");
		break;
	}
	case 20:
		printf("switch (v%d & 7) {\n", y);
		for (int c = 0; c < 8; c++) {
			ind(d); printf("case %d: v%d = ", c, R(NV)); expr(); printf("; %s\n", R(3) ? "break;" : "");
		}
		ind(d); printf("}\n");
		break;
	default:
		printf("if (v%d %s v%d) v%d = ", x, R(2) ? "==" : "!=", y, R(NV)); expr(); printf(";\n");
		break;
	}
}

int main(int argc, char **argv)
{
	int nf = argc > 2 ? atoi(argv[2]) : 100;
	rs = argc > 1 ? (uint32_t)atoi(argv[1]) : 1;
	printf("// generado por gen.c (semilla %u)\n#include \"fuzz.h\"\n\n", rs);
	for (int f = 0; f < nf; f++) {
		printf("static __attribute__((noinline)) u32 fz%d(u32 a, u32 b, u32 c, u32 d)\n{\n", f);
		printf("\tu32 v0 = a, v1 = b, v2 = c, v3 = d, v4 = a ^ b, v5 = c + d, v6 = a * 3, v7 = b - d;\n");
		for (int i = R(12) + 6; i; i--) stmt(1, 0);
		printf("\treturn v0 ^ v1 * 3 ^ v2 * 5 ^ v3 * 7 ^ v4 * 11 ^ v5 * 13 ^ v6 * 17 ^ v7 * 19;\n}\n\n");
	}
	printf("u32 (*const fz_table[])(u32, u32, u32, u32) = {\n");
	for (int f = 0; f < nf; f++) printf("\tfz%d,\n", f);
	printf("};\nconst int fz_count = %d;\n", nf);
	return 0;
}
