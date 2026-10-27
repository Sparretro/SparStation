// Arranque mínimo para los EXE de prueba: la BIOS ya deja la pila puesta (cabecera del EXE)
typedef unsigned int u32;
int main(void);

void bios_putchar(int c)
{
	register int a0 __asm__("$4") = c;
	__asm__ volatile("li $9, 0x3c\n\tli $8, 0xa0\n\tjalr $8\n\tnop" : "+r"(a0) : : "$2", "$3", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "$31", "memory");
}

void puts_(const char *s) { while (*s) bios_putchar(*s++); }

void puthex(u32 v)
{
	for (int i = 28; i >= 0; i -= 4) bios_putchar("0123456789abcdef"[(v >> i) & 15]);
}

void _start(void) __attribute__((section(".text.start")));
void _start(void)
{
	main();
	for (;;) { }
}

void *memcpy(void *d, const void *s, unsigned n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned n) { char *a = d; while (n--) *a++ = (char)c; return d; }
