// Medidor de pruebas en la pantalla de abajo: fps, reparto del tiempo de la DS, contadores y log en la SD
#ifndef HUD_H
#define HUD_H
#include <nds.h>

void hud_start(const char *game, int use_log, const char *settings);   // use_log: escribir sd:/ps1/sparstation_log.txt
void hud_frame(u32 ticks);                        // duración de un frame de la PS1 (ticks de cpuGetTiming)
void hud_second(int frames, u32 vblanks);         // una vez por segundo real: calcula, dibuja y apunta
int hud_input(void);                              // tras scanKeys(): toques en la pantalla de abajo; 1 = abrir el menú
void hud_redraw(void);                            // vuelve a pintar el medidor (tras el menú)

#endif
