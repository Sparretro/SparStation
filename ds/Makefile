#---------------------------------------------------------------------------------
# SparStation para DSi: núcleo de ../core + frontend de source/. Usa el ARM7 por defecto de calico (con NWRAM7=1, el de arm7/).
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

GAME_TITLE     := SparStation
GAME_SUBTITLE1 := PlayStation en tu DSi (v39)
GAME_SUBTITLE2 := by Spar Retro
# make SELFTEST=1 [NITRODIR=carpeta] mete esa carpeta como NitroFS (selftest.exe o disco reducido) para probar en modo DS
ifeq ($(SELFTEST),1)
NITRODIR       ?= nitrofiles
NITRO_FILES    := $(abspath $(dir $(firstword $(MAKEFILE_LIST))))/$(NITRODIR)
endif

include $(DEVKITARM)/ds_rules

TARGET   := SparStation
BUILD    := build
SOURCES  := source ../core hot
INCLUDES := ../core
DATA     := data

ARCH     := -marm -march=armv5te -mtune=arm946e-s
CFLAGS   := -Wall -O2 -ffunction-sections -fdata-sections -fomit-frame-pointer $(ARCH) $(INCLUDE) -DARM9 -DPSX_PROFILE -DPSX_DYNAREC -DPSX_TTY
ASFLAGS  := $(ARCH)
LDFLAGS   = -specs=ds_arm9.specs $(ARCH) -Wl,-Map,$(notdir $*.map) $(EXTRA_LD)
LIBS     := -lfat -lnds9 -lm
# Pruebas en melonDS (modo DS, 4 MB): la BIOS va en nitro:/bios.bin y se copia al Expansion Pak de la ranura
# de GBA, que deja más memoria para la caché de código. Compila aparte (build_test, SparStation_test.nds).
ifeq ($(SELFTEST),1)
CFLAGS   += -DPSX_SELFTEST $(EXTRA)
DATA     :=
BUILD    := build_test
TARGET   := SparStation_test
endif
# make PERFIL=1: ROM con el perfilador por muestreo (sd:/ps1/sparstation_prof.bin, ver source/prof.c)
ifeq ($(PERFIL),1)
CFLAGS   += -DSPAR_PROF
BUILD    := build_perfil
TARGET   := SparStation_perfil
endif
# make NWRAM7=1 (con make -C arm7 antes): el mismo ARM9 con el ARM7 de arm7/, que deja la NWRAM B y C al ARM9
# (TWiLight Menu++ se la da al ARM7 protegida). Sale SparStation_nwram7.nds; con PERFIL=1, SparStation_perfil_nwram7.
ifeq ($(NWRAM7),1)
TARGET   := $(TARGET)_nwram7
_ARM7_ELF := -7 $(dir $(abspath $(firstword $(MAKEFILE_LIST))))arm7/arm7.elf
endif

LIBDIRS  := $(LIBNDS) $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

HOT      := bus.c gte.c
CFILES   := $(filter-out $(HOT),$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD := $(CC)
export OFILES_BIN     := $(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CFILES:.c=.o)
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES         := $(addsuffix .h,$(subst .,_,$(BINFILES)))
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nds

else

$(OUTPUT).nds: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
$(OFILES_SOURCES): $(HFILES)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

endif
