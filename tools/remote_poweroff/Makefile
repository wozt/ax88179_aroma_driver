ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set)
endif

ifeq ($(strip $(DEVKITPPC)),)
$(error DEVKITPPC is not set)
endif

TOPDIR ?= $(CURDIR)

include $(DEVKITPPC)/base_tools

WUT_ROOT := $(DEVKITPRO)/wut

TARGET  := poweroff
BUILD   := build
SOURCES := .
INCLUDES :=

MACHDEP  := -DESPRESSO -mcpu=750 -meabi -mhard-float
RPXSPECS := -specs=$(WUT_ROOT)/share/wut.specs

CFLAGS := -O2 -Wall -ffunction-sections -fdata-sections $(MACHDEP)
CFLAGS += $(INCLUDE) -D__WIIU__ -D__WUT__

LDFLAGS := $(MACHDEP) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS := -lwut
LIBDIRS := $(WUT_ROOT)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export OFILES := $(CFILES:.c=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(TARGET).rpx $(TARGET).elf $(TARGET).map

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).rpx: $(OUTPUT).elf
	@elf2rpl $< $@

$(OUTPUT).elf: $(OFILES)
	$(CC) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDE) -MMD -MP -c $< -o $@

-include $(DEPENDS)

endif
