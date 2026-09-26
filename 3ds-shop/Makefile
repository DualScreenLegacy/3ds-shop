#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
export DEVKITPRO := /opt/devkitpro
endif

ifeq ($(strip $(DEVKITARM)),)
export DEVKITARM := $(DEVKITPRO)/devkitARM
endif

ifeq ($(strip $(CTRULIB)),)
export CTRULIB  := $(DEVKITPRO)/libctru
endif

ifeq ($(strip $(PORTLIBS)),)
export PORTLIBS := $(DEVKITPRO)/portlibs/3ds
endif

PREFIX  := arm-none-eabi-
export CC      := $(PREFIX)gcc
export CXX     := $(PREFIX)g++
export AS      := $(PREFIX)as
export AR      := $(PREFIX)ar
export OBJCOPY := $(PREFIX)objcopy
export STRIP   := $(PREFIX)strip
export NM      := $(PREFIX)nm
export LD      := $(PREFIX)gcc

include $(DEVKITARM)/3ds_rules

TARGET      := $(notdir $(CURDIR))
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include

APP_TITLE       := 3DS Shop Client
APP_DESCRIPTION := Custom Local CIA Shop
APP_AUTHOR      := Homebrew

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations \
           -fomit-frame-pointer -ffunction-sections \
           $(ARCH)

CFLAGS  += $(INCLUDE) -D__3DS__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS  := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -L$(CTRULIB)/lib -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lcurl -ljansson -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

LIBDIRS := $(PORTLIBS) $(CTRULIB)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.png)
    ifneq (,$(findstring icon.png,$(icons)))
        export ICON_ARG := $(CURDIR)/icon.png
    else
        ifneq (,$(findstring $(TARGET).png,$(icons)))
            export ICON_ARG := $(CURDIR)/$(TARGET).png
        else
            export ICON_ARG := $(CTRULIB)/default_icon.png
        endif
    endif
else
    export ICON_ARG := $(ICON)
endif

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(data))

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))

export OFILES_BIN   := $(addsuffix .o,$(BINFILES))
export OFILES_SRC   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)
export OFILES       := $(OFILES_BIN) $(OFILES_SRC)

export DEPSDIR  := $(CURDIR)/$(BUILD)
export SMDHFILE := $(CURDIR)/$(TARGET).smdh

#---------------------------------------------------------------------------------
all: $(BUILD) $(TARGET).3dsx

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(TARGET).3dsx: $(BUILD) $(SMDHFILE)

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(OUTPUT).elf

$(SMDHFILE):
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(ICON_ARG) $@

#---------------------------------------------------------------------------------
else

.PHONY: all

all: $(OUTPUT).elf

$(OUTPUT).elf: $(OFILES)
	@echo linking $(notdir $@)
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------