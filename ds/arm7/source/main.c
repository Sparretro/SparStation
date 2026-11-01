// ARM7 de SparStation: el ARM7 por defecto de calico (plantilla «combined» de devkitPro, ver su licencia abajo)
// con un único añadido al principio para la NWRAM de la DSi (sd:/ps1/nwram_info.txt de la v29 lo mostró):
// con TWiLight Menu++ los huecos de la NWRAM B y C llegan asignados al ARM7 y protegidos (MBK9 = 00FFFF0F), y el
// ARM9 no puede quitárselos. El ARM7 no los usa (su memoria propia de la DSi es la NWRAM A, «twlwram» en ds7.ld):
// aquí solo se quita la protección de B y C, y el ARM9 se los asigna (ds/source/main.c, nwram_init).
/*---------------------------------------------------------------------------------

	default ARM7 core

		Copyright (C) 2005 - 2010
		Michael Noland (joat)
		Jason Rogers (dovoto)
		Dave Murphy (WinterMute)

	This software is provided 'as-is', without any express or implied
	warranty.  In no event will the authors be held liable for any
	damages arising from the use of this software.

	Permission is granted to anyone to use this software for any
	purpose, including commercial applications, and to alter it and
	redistribute it freely, subject to the following restrictions:

	1.	The origin of this software must not be misrepresented; you
		must not claim that you wrote the original software. If you use
		this software in a product, an acknowledgment in the product
		documentation would be appreciated but is not required.

	2.	Altered source versions must be plainly marked as such, and
		must not be misrepresented as being the original software.

	3.	This notice may not be removed or altered from any source
		distribution.

---------------------------------------------------------------------------------*/
#include <calico.h>
#include <nds.h>
#include <maxmod7.h>

//---------------------------------------------------------------------------------
int main() {
//---------------------------------------------------------------------------------

	// SparStation: NWRAM B y C sin proteger para que el ARM9 se las asigne (la A sigue protegida: es del ARM7)
	if (systemIsTwlMode()) REG_MBK_SLOTWRPROT &= 0x0000000f;

	// Read settings from NVRAM
	envReadNvramSettings();

	// Set up extended keypad server (X/Y/hinge)
	keypadStartExtServer();

	// Configure and enable VBlank interrupt
	lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
	irqEnable(IRQ_VBLANK);

	// Set up RTC
	rtcInit();
	rtcSyncTime();

	// Initialize power management
	pmInit();

	// Set up block device peripherals
	blkInit();

	// Set up touch screen driver
	touchInit();
	touchStartServer(80, MAIN_THREAD_PRIO);

	// Set up sound and mic driver
	soundStartServer(MAIN_THREAD_PRIO-0x10);
	micStartServer(MAIN_THREAD_PRIO-0x18);

	// Set up wireless manager
	wlmgrStartServer(MAIN_THREAD_PRIO-8);

	// Set up Maxmod
	mmInstall(MAIN_THREAD_PRIO+1);

	// Keep the ARM7 mostly idle
	while (pmMainLoop()) {
		threadWaitForVBlank();
	}

	return 0;
}
