// PDSX en SparStation: intro y menús con el aspecto de la BIOS de la PS1 (ver pdsx.c)
#pragma once
void pdsx_start(void);                                        // 3D, texturas y fondo (antes del menú)
void pdsx_intro(void (*info)(int cursor));                   // logo de SCE y menú principal hasta elegir «CD Player»
void pdsx_list_frame(const char (*names)[128], int n, int sel);   // una imagen de la lista de juegos (espera la vblank)
void pdsx_boot_logo(void);                                    // logo de PlayStation con su sonido
void pdsx_stop(void);                                         // fundido y la pantalla como la espera el emulador
