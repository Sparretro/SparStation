// Prueba de CPU: ejecuta las funciones aleatorias de fuzz.c con varias entradas y deja los resultados
// en RAM (0x80180000: "FZOK", cuántos, resultados) y el total en la salida de texto.
#include "fuzz.h"
void puts_(const char *s);
void puthex(u32 v);
extern u32 (*const fz_table[])(u32, u32, u32, u32);
extern const int fz_count;

u8 b8[64];
u16 h16[32];
u32 w32[32];
struct pk_t pk[8];
volatile u32 *mpx[2] = { (volatile u32 *)0x1f800200, w32 };   // base en un registro: scratchpad o RAM

__attribute__((noinline)) u32 hlp0(u32 a, u32 b) { return a * 31 + b; }
__attribute__((noinline)) u32 hlp1(u32 a, u32 b) { return (a ^ b) >> 3 | a << 7; }
__attribute__((noinline)) u32 hlp2(u32 a, u32 b) { return b ? a % b : a; }
__attribute__((noinline)) u32 hlp3(u32 a, u32 b) { u32 s = 0; for (u32 i = 0; i < (b & 15); i++) s += a >> i; return s; }
u32 (*const hlp[4])(u32, u32) = { hlp0, hlp1, hlp2, hlp3 };

// Código que se reescribe a sí mismo: addiu v0, a0, imm; jr ra; nop
static u32 smc_buf[16] __attribute__((aligned(16)));
static u32 smc_test(void)
{
	u32 s = 0;
	for (u32 k = 0; k < 40; k++) {
		u32 (*fn)(u32) = (u32 (*)(u32))(void *)smc_buf;
		smc_buf[0] = 0x24820000u | ((k * 37) & 0x7fff);
		smc_buf[1] = 0x03e00008u;
		smc_buf[2] = 0;
		s = s * 3 + fn(k);
	}
	return s;
}

int main(void)
{
	volatile u32 *res = (volatile u32 *)0x80180000;
	u32 total = 0;
	static const u32 seeds[4][4] = {
		{ 1, 2, 3, 4 }, { 0x80000000u, 0x7fffffffu, 0xffffffffu, 0 }, { 0x12345678u, 0x9abcdef0u, 77, 0xfffffff0u }, { 255, 65535, 0x10000, 0x8000 },
	};
	for (int i = 0; i < 64; i++) b8[i] = (u8)(i * 13 + 7);
	for (int i = 0; i < 32; i++) { h16[i] = (u16)(i * 4099); w32[i] = i * 0x01010101u; }
	for (int i = 0; i < 256; i++) sp[i] = i * 3;
	res[0] = 0;
	for (int f = 0; f < fz_count; f++) {
		u32 h = 0;
		for (int s = 0; s < 4; s++) h = h * 0x9e3779b1u + fz_table[f](seeds[s][0], seeds[s][1], seeds[s][2], seeds[s][3]);
		for (int i = 0; i < 32; i++) h += w32[i] * (i + 1) + h16[i];
		res[2 + f] = h;
		total = total * 31 + h;
	}
	res[2 + fz_count] = smc_test();
	total = total * 31 + res[2 + fz_count];
	res[1] = fz_count + 1;
	res[0] = 0x4b4f5a46;   // "FZOK"
	puts_("FUZZ ");
	puthex(total);
	puts_("\n");
	return 0;
}
