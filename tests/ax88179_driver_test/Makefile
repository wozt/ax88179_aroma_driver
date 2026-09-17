#---------------------------------------------------------------------------------
# capture2cloud, running ON a Wii U console.
#
# Not to be confused with ../wiiu_gamepad, which runs on the PC and talks
# to a GamePad over a radio. This one is a .rpx for the console itself.
#
# Built with devkitPPC + wut:
#     sudo dkp-pacman -S wiiu-dev
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/wut/share/wut_rules

TARGET		:=	ax88179_driver
BUILD		:=	build
SOURCES		:=	. ../net ../vendor/lwip/src/core ../vendor/lwip/src/core/ipv4 ../vendor/lwip/src/netif
DATA		:=	data
# The parent directory holds c2s_protocol.h, shared verbatim with the
# host so the wire format cannot drift between the two. Exactly what the
# Switch client does.
INCLUDES	:=	../net ../net/port ../vendor/lwip/src/include

# These become the channel's name, icon and splash on the Wii U menu.
# A .wuhb is an .rpx plus this metadata, and the homebrew_on_menu plugin
# is what puts one on the menu as a channel -- see README.md.
APP_NAME	:=	AX88179 Ethernet
APP_SHORTNAME	:=	AX88179 Ethernet
APP_AUTHOR	:=	wozt
APP_ICON	:=	$(TOPDIR)/../../assets/icon-128.png

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
CFLAGS	:=	-g -Wall -Wextra -O2 -ffunction-sections \
			$(MACHDEP)

CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(MACHDEP)
LDFLAGS	=	-g $(MACHDEP) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

# No SDL2 here, deliberately.
#
# SDL2's Wii U renderer is real GX2, but it has no YUV shader -- an NV12
# texture would go through SDL's software conversion, and 720p60 of that
# on a 1.24 GHz PowerPC is not a thing that happens. The console's
# decoder hands us NV12 and GX2 can sample it directly, so the picture
# path is GX2 and there is nothing for SDL2 to do.
LIBS	:=	-lSDL2_ttf -lfreetype -lharfbuzz -lfreetype -lbrotlidec -lbrotlicommon -lpng -lbz2 -lz -lSDL2 -lwut -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(WUT_ROOT)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add
# additional rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).rpx $(TARGET).elf $(TARGET).wuhb

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
# The .wuhb as well as the .rpx: the .rpx is what wiiload sends over the
# network while developing, the .wuhb is what goes on the SD card and
# shows up as a channel.
all	:	$(OUTPUT).wuhb

$(OUTPUT).wuhb	:	$(OUTPUT).rpx
$(OUTPUT).rpx	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
