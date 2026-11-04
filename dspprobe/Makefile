#---------------------------------------------------------------------------------
# Prueba del DSP de la DSi (Teak): estado de la NWRAM y un programa mínimo que responde CMD0+1 en REP0.
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
GAME_TITLE     := SparStation DSP
GAME_SUBTITLE1 := Prueba del DSP de la DSi
GAME_SUBTITLE2 := by Spar Retro
include $(DEVKITARM)/ds_rules
TARGET   := DSPprueba
BUILD    := build
SOURCES  := source
INCLUDES :=
ARCH     := -marm -march=armv5te -mtune=arm946e-s
CFLAGS   := -Wall -O2 $(ARCH) $(INCLUDE) -DARM9
LDFLAGS   = -specs=ds_arm9.specs $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lfat -lnds9
LIBDIRS  := $(LIBNDS) $(PORTLIBS)
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
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
-include $(DEPSDIR)/*.d
endif
