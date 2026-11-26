#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
TARGET		:=	OpenShop
BUILD		:=	build
# Every directory under src/, found at make time, so a new source folder can't be silently left out of the Wii build.
# Objects land flattened in build/ by file name, so two .cpp files must never share a basename.
# (The inner make pass runs inside build/, where src/ does not exist and SOURCES is unused: hence 2>/dev/null.)
SOURCES		:=	$(shell find src -type d 2>/dev/null)
DATA		:=
INCLUDES	:=	src

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------

PKGCONF	:=	$(DEVKITPRO)/portlibs/wii/bin/powerpc-eabi-pkg-config
PKGS	:=	sdl2 SDL2_ttf SDL2_mixer SDL2_image jansson

# pkg-config runs once here (":="), not once per compile through the recursive CFLAGS below.
PKG_CFLAGS	:=	$(shell $(PKGCONF) --cflags $(PKGS))
PKG_LIBS	:=	$(shell $(PKGCONF) --libs $(PKGS))

# libzip (M3 archive extraction). Checked inside the docker.io/devkitpro/devkitppc image: ppc-libzip 1.10.1-1,
# libzip.pc has "Libs: -L${libdir} -lzip" and "Libs.private: -lbz2 -lz", so `pkg-config --static --libs libzip` gives
# "-L/opt/devkitpro/portlibs/ppc/lib -lzip -lbz2 -lz". No lzma, zstd or crypto library is linked. It comes after PKG_LIBS
# on the link line and is kept as its own variable so its flags stay visible. (zlib and bzip2 are in the image already.)
LIBZIP_CFLAGS	:=	$(shell $(PKGCONF) --cflags libzip)
LIBZIP_LIBS	:=	$(shell $(PKGCONF) --static --libs libzip)

# WERROR=1 (e.g. `./dev build WERROR=1`) turns any warning into an error: the zero-warning rule.
WARN	:=	-Wall -Wextra
ifeq ($(WERROR),1)
WARN	+=	-Werror
endif

# DEBUGTOOLS=1 (e.g. `./dev build DEBUGTOOLS=1`) builds the hardware test build: on-screen network test bench and
# debuglog.txt (see src/core/DebugLog.h). Objects are tracked against the flags through build/.flags (see below), so
# switching DEBUGTOOLS or WERROR rebuilds by itself.
DEFS	:=
ifeq ($(DEBUGTOOLS),1)
DEFS	+=	-DOPENSHOP_DEBUGTOOLS
endif

CFLAGS	= -g -O2 $(WARN) $(DEFS) $(MACHDEP) $(INCLUDE) $(PKG_CFLAGS) $(LIBZIP_CFLAGS)
CXXFLAGS	=	$(CFLAGS) -std=gnu++20
ASFLAGS	=	-g $(MACHDEP) -Wa,-I$(ROOTDIR)

LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS	:=	$(PKG_LIBS) $(LIBZIP_LIBS)

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export ROOTDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES := $(addsuffix .h,$(subst .,_,$(BINFILES)))

#---------------------------------------------------------------------------------
# build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE	:=	$(foreach dir,$(INCLUDES), -iquote $(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD) \
					-I$(LIBOGC_INC)

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS	:= -L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export OUTPUT	:=	$(CURDIR)/$(TARGET)
.PHONY: $(BUILD) clean

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

#---------------------------------------------------------------------------------
run:
	wiiload $(TARGET).dol


#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)
FLAGS_STAMP	:=	.flags

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES) : $(HFILES) $(FLAGS_STAMP)

# .incbin'd assets are not seen by the compiler's dependency tracking.
assets.o : $(wildcard $(ROOTDIR)/assets/fonts/*.ttf)

#---------------------------------------------------------------------------------
# This rule links in binary data with the .jpg extension
#---------------------------------------------------------------------------------
%.jpg.o	%_jpg.h :	%.jpg
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

# The compiler flags this build was made with. The stamp is rewritten only when they differ from the last build, so
# every source object depending on it recompiles exactly when DEBUGTOOLS / WERROR (or any other flag) changed, and
# not otherwise. Chosen over "./dev build cleans on change" because it lives in make itself: plain `make`, ./dev,
# and any other caller all get the same answer, and nothing is rebuilt that did not need to be.
# (FLAGS_STAMP is set at the top of this section: prerequisites expand immediately.)
.PHONY: FORCE
FORCE:
# (A dry run, make -n, skips the comparison: a FORCE'd stamp would otherwise make every object look out of date.)
ifneq ($(findstring n,$(firstword -$(MAKEFLAGS))),)
STAMPDEP :=
else
STAMPDEP := FORCE
endif
$(FLAGS_STAMP): $(STAMPDEP)
	@printf '%s\n' '$(DEFS) | $(WARN) | $(MACHDEP)' | cmp -s - $@ || printf '%s\n' '$(DEFS) | $(WARN) | $(MACHDEP)' > $@

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
