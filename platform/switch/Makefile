#---------------------------------------------------------------------------------
# Fresh Adventures - Nintendo Switch homebrew build
#
# The NRO links the actual Fresh Adventures entry point and the portable engine.
# User-owned game data is loaded from GData beside the NRO on sdmc:.
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment")
endif

TOPDIR      ?= $(CURDIR)
TARGET      := freshadventures
BUILD       := build
SOURCES     := source ../../src/app ../../src/core ../../src/game
DATA        := data
INCLUDES    :=

APP_TITLE   := Fresh Adventures
APP_AUTHOR  := OpenFA
APP_VERSION := 0.1.0

include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# Switch code-generation options. These match the devkitPro application template.
#---------------------------------------------------------------------------------
ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -Wextra -O2 -ffunction-sections \
			$(ARCH) $(DEFINES) -DFA_SWITCH_BUILD
CFLAGS      += $(INCLUDE) -D__SWITCH__
ASFLAGS     := -g $(ARCH)
LDFLAGS     = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
			-Wl,-Map,$(notdir $*.map)
LIBS        := -lSDL2 -lEGL -lglapi -ldrm_nouveau -lnx -lpthread -lm

# Only these platform files belong in the Switch target. In particular, do
# not compile the SDL2 or desktop clock/path backends into the NRO.
SWITCH_PLATFORM_SOURCES := \
	../../src/platform/fa_platform.c \
	../../src/platform/fa_backend_null.c \
	../../src/platform/fa_backend_switch.c \
	../../src/platform/fa_paths_switch.c

LIBDIRS     := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# The remainder follows the standard devkitPro application template.
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			   $(CURDIR)/../../src/platform \
			   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))) \
			$(notdir $(SWITCH_PLATFORM_SOURCES))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

# The Switch SDL2 port pulls in C++ runtime support even for a C entry point.
# Match the devkitPro SDL2 examples and link through the C++ driver.
export LD := $(CXX)

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			  -I$(CURDIR)/../../include \
			  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all $(BUILD) clean

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $@ -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
