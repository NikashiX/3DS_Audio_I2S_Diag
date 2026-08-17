ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set. Install devkitPro/devkitARM + 3ds-dev first")
endif

include $(DEVKITARM)/3ds_rules

TARGET      := audio_i2s_diag
BUILD       := build
SOURCES     := source
INCLUDES    :=
DATA        :=
ROMFS       :=

ARCH        := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS      := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS      += $(INCLUDE) -D__3DS__
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS        := -lctru -lm
LIBDIRS     := $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
export LD := $(CC)
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_SRC)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean cia resources
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

cia: all resources
	@command -v makerom >/dev/null || (echo "makerom not found"; exit 1)
	makerom -f cia -o $(TARGET).cia -rsf app.rsf -target t -exefslogo \
		-elf $(TARGET).elf -icon $(BUILD)/$(TARGET).smdh -banner $(BUILD)/$(TARGET).bnr

resources: $(BUILD)
	@command -v bannertool >/dev/null || (echo "bannertool not found"; exit 1)
	bannertool makesmdh -s "I2S Audio Diag" -l "Read-only DSP/I2S diagnostic" \
		-p "3DS Audio Diagnostic" -i resources/icon.png -o $(BUILD)/$(TARGET).smdh
	bannertool makebanner -i resources/banner.png -a resources/banner.wav -o $(BUILD)/$(TARGET).bnr

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).cia $(TARGET).smdh

else
DEPENDS := $(OFILES:.o=.d)
$(OUTPUT).3dsx : $(OUTPUT).elf
$(OUTPUT).elf : $(OFILES)
	$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@
-include $(DEPENDS)
endif
