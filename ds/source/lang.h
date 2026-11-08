// Idiomas de los menús y del medidor: español, inglés y ruso (ver lang.c)
#pragma once

enum { LANG_ES, LANG_EN, LANG_RU, LANG_N };
extern int lang;

// El texto en el idioma elegido. Los textos van en UTF-8 en el código; la consola los pasa a la fuente (font.c)
#define T(es, en, ru) (lang == LANG_EN ? (en) : lang == LANG_RU ? (ru) : (es))

const char *lang_name(int l);   // «Español», «English», «Русский»
void lang_console_init(void);   // fuente con tildes y cirílico, y la salida de la consola en UTF-8
// Pasa un texto UTF-8 a los códigos de la fuente (un byte por letra); devuelve la longitud
int lang_to_font(const char *in, char *out, int max);
