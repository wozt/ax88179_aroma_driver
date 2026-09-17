#-------------------------------------------------------------------------------
.SUFFIXES:
#-------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

# Anchored on this Makefile's own location, not CURDIR: the devkitPPC
# rules recurse into build/ and move CURDIR with them, which broke a
# relative path here. The vendored libs sit beside the project.
THIS_DIR   := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
WUMS_ROOT  ?= $(THIS_DIR)../vendor/wums
MOCHA_ROOT ?= $(THIS_DIR)../vendor/libmocha
FUNCTIONPATCHER_ROOT ?= $(THIS_DIR)../vendor/functionpatcher
include $(WUMS_ROOT)/share/wums_rules
WUT_ROOT := $(DEVKITPRO)/wut
#-------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#-------------------------------------------------------------------------------
# SHIM=1 installs nsysnet hooks automatically at boot for all GAME titles.
# SHIM=0 keeps hooks off (probe-only mode for diagnostic builds).
SHIM ?= 1
TARGET		:=	AX88179Module
BUILD		:=	build
SOURCES		:=	. ../driver ../net ../net/port \
			../vendor/lwip/src/core ../vendor/lwip/src/core/ipv4 \
			../vendor/lwip/src/api ../vendor/lwip/src/netif
DATA		:=	data
INCLUDES	:=	. ../net ../net/port ../vendor/lwip/src/include

#-------------------------------------------------------------------------------
# options for code generation
#-------------------------------------------------------------------------------
# -fcommon: lwIP's netdb.c and wut's wutsocket both define h_errno; let the
# linker merge them instead of erroring (GCC 10+ defaults to -fno-common).
CFLAGS	:=	-Wall -Wextra -O2 -ffunction-sections -fcommon\
			$(MACHDEP)

CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__

CXXFLAGS	:= $(CFLAGS) -std=c++17 

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map) $(WUMSSPECS) 

ifeq ($(SHIM),0)
CFLAGS += -DAX_DISABLE_SHIM=1
endif

ifeq ($(DEBUG),1)
CXXFLAGS += -DDEBUG -g
CFLAGS += -DDEBUG -g
endif

LIBS	:= -lwums -lfunctionpatcher -lmocha -lwut

#-------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#-------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(WUT_ROOT) $(WUMS_ROOT) $(MOCHA_ROOT) $(FUNCTIONPATCHER_ROOT)

#-------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#-------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#-------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#-------------------------------------------------------------------------------
	export LD	:=	$(CC)
#-------------------------------------------------------------------------------
else
#-------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

#-------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@if [ ! -f $(BUILD)/shim.flag ] || [ "$$(cat $(BUILD)/shim.flag)" != "$(SHIM)" ]; then \
		rm -f $(BUILD)/*.o; echo "$(SHIM)" > $(BUILD)/shim.flag; \
	fi
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#-------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wms $(TARGET).elf

#-------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#-------------------------------------------------------------------------------
# main targets
#-------------------------------------------------------------------------------
all	:	 $(OUTPUT).wms

$(OUTPUT).wms	:	$(OUTPUT).elf
$(OUTPUT).elf	:   $(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#-------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#-------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#-------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
%.o: %.s
	@echo $(notdir $<)
	@$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d -x assembler-with-cpp $(ASFLAGS) -c $< -o $@ $(ERROR_FILTER)

-include $(DEPENDS)

#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------
