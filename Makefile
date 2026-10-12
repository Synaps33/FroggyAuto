# re3 (GTA III) for the Data Frog SF2000 / GB300.
#
#   make native            -> build/native/libretro_re3.so         (x86-64, PC runner)
#   make runner            -> build/native/test_runner_re3
#   make sf2000            -> build/sf2000/libretro_re3.a          (MIPS32r2 soft-float)
#   make gb300             -> build/gb300/libretro_re3.a           (same, GB300 defines)
#   make sf2000-core       -> MIPS static lib + the multicore core_87000000 / bisrv.asd
#   make gb300-core        -> GB300V2 core_87000000 / bisrv.asd
#   make clean
#
# The console has a ~900 MHz MIPS32r2 CPU with *no FPU* and *no GPU*, so:
#   - every float is soft-float (-msoft-float), which is why the MIPS build is
#     much slower than the desktop one;
#   - rendering goes through librw's new "soft" backend (vendor/librw/src/soft),
#     a plain CPU rasteriser that writes RGB565 straight into the host's
#     framebuffer.

SHELL := /bin/bash
TOP   := $(CURDIR)

ENGINE   := $(TOP)/re3-symbian
LIBRW    := $(ENGINE)/vendor/librw
MULTICORE:= /home/Sajnaps/gb300/sf2000_multicore
FROG     := /opt/frog-toolchain/mipsel-mti-elf_stable/bin/mipsel-mti-elf-
BINUTIL  := /opt/mips32-mti-elf/2019.09-03-2/bin/mips-mti-elf-

# The SF2000 platform layer replaces src/skel/{glfw,win,symbian}.
RE3_PLATFORM_SRC := src/skel/sf2000/sf2000.cpp

include $(ENGINE)/port/re3_sources.mk
include $(LIBRW)/port/librw_sources.mk

RE3_ALL    := $(addprefix $(ENGINE)/,$(RE3_SOURCES) $(RE3_PLATFORM_SRC))
LIBRW_CPP  := $(addprefix $(LIBRW)/,$(filter %.cpp,$(LIBRW_SOURCES)))
LIBRW_C    := $(addprefix $(LIBRW)/,$(filter %.c,$(LIBRW_SOURCES)))

# ---------------------------------------------------------------- includes
# re3 includes its headers by bare directory name, exactly like the Symbian
# build's USERINCLUDE list in group/re3.mmh.
RE3_INC := $(ENGINE)/src $(ENGINE)/gamefiles $(ENGINE)/src/skel/sf2000
RE3_INC += $(foreach d,animation audio audio/oal buildings collision control core \
                      entities extras fakerw math modelinfo objects peds renderer \
                      rw save skel text vehicles weapons,$(ENGINE)/src/$(d))
LIBRW_INC := $(LIBRW) $(LIBRW)/src

# ---------------------------------------------------------------- defines
# LIBRW                use librw instead of the original RenderWare
# RW_SOFT              select the new software rasteriser backend
# MASTER               what the Symbian build defines: no debug renderers
# USE_OUR_VERSIONING / CMAKE_BUILD   match upstream re3's CMake build
# SF2000 / GB300V2     which console (SF2000 also enables CONSOLE_NO_MOUSE)
COMMON_DEFS := -DLIBRW -DRW_SOFT -DMASTER -DUSE_OUR_VERSIONING -DCMAKE_BUILD \
               -DLODEPNG_NO_COMPILE_CPP -DMORE_LANGUAGES -UDEBUG

WARNINGS := -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
            -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
            -Wno-parentheses -Wno-char-subscripts -Wno-misleading-indentation \
            -Wno-implicit-fallthrough -Wno-class-memaccess -Wno-stringop-truncation \
            -Wno-format-truncation -Wno-array-bounds -Wno-stringop-overflow \
            -Wno-unknown-pragmas -Wno-strict-aliasing

INCLUDES := $(foreach i,$(RE3_INC),-I$(i)) $(foreach i,$(LIBRW_INC),-I$(i)) -I$(TOP)/libretro

# ================================================================= native ==
# This machine's /usr/local/bin/gcc is a wrapper that forces -m32, so the host
# compiler is named explicitly here.
HOSTCC  := /usr/bin/gcc
HOSTCXX := /usr/bin/g++
NATIVE_DIR := $(TOP)/build/native
# NATIVE_SAN=address builds an AddressSanitizer variant, which is how the heap
# problems in the texture path were found.
NATIVE_SAN ?=
NATIVE_CXXFLAGS := -std=gnu++11 -O1 -g -fPIC $(NATIVE_SAN) \
                   -fno-omit-frame-pointer -MMD -MP $(COMMON_DEFS) -DSF2000 \
                   $(WARNINGS) $(INCLUDES)
NATIVE_ENGINE := $(patsubst $(ENGINE)/%.cpp,$(NATIVE_DIR)/engine/%.o,$(RE3_ALL))
NATIVE_LIBRW  := $(patsubst $(LIBRW)/%.cpp,$(NATIVE_DIR)/librw/%.o,$(LIBRW_CPP)) \
                 $(patsubst $(LIBRW)/%.c,$(NATIVE_DIR)/librw/%.o,$(LIBRW_C))

# ================================================================= sf2000 ==
# -msoft-float is the important one: the HiChip B210 has no FPU.
MIPS_ARCH := -EL -march=mips32 -mtune=mips32r2 -msoft-float -G0 -mno-abicalls -fno-pic
# The console SDK's headers are not in the toolchain's sysroot: it supplies
# dirent.h, the fs_* and os_* calls and the pad API.  RE3_CONSOLE_SDK tells the
# port that those are available (multicore's os_get_tick_count/dly_tsk replace
# clock_gettime/nanosleep, which this newlib does not have at all).
MIPS_CXXFLAGS := -std=gnu++11 -O2 -fno-strict-aliasing $(MIPS_ARCH) -ffunction-sections -fdata-sections -MMD -MP \
                 -I$(MULTICORE)/include -DRE3_CONSOLE_SDK \
                 $(COMMON_DEFS) $(WARNINGS) $(INCLUDES)
SF2000_DEFS := -DSF2000
GB300_DEFS  := -DGB300V2

MIPS_AR := $(BINUTIL)ar

# ================================================================== targets ==

.PHONY: all native runner test softtest sf2000 gb300 sf2000-core gb300-core \
        deploy-sf2000 deploy-gb300 clean distclean cores gbsim gb300runner

all: native runner softtest sf2000 gb300

native: $(NATIVE_DIR)/libretro_re3.so
runner: $(NATIVE_DIR)/test_runner_re3
softtest: $(NATIVE_DIR)/soft_render_test
test: native runner softtest

# ============================================================ gb300 sim ====
# The console is a ~900 MHz MIPS32r2 with no FPU and a 32-bit address space.
# A native -m32 build with x87 and no SSE is the closest stand-in available on
# this host: 32-bit pointers, every float in memory rather than in a vector
# register, and no auto-vectorised inner loops.  It still runs on real silicon,
# so it is a *model* of the console, not the console - runner/gb300_runner.c
# adds the missing part (RAM cap and CPU throttle) and scales the measured time
# by GBSIM_CPU_FACTOR to predict console frame rate.
#
#   make gbsim        -> build/gbsim/libretro_re3.so   (32-bit, no FPU/SIMD)
#   make gb300runner  -> build/gbsim/gb300_runner     (throttling test runner)
GBSIM_DIR   := $(TOP)/build/gbsim
GBSIM_DEFS  := -DSF2000 -DGB300V2
# -m32: 32-bit pointers and ABI, like the console.
# -mfpmath=387 -mno-sse*: x87 only.  No SSE means no vector registers, no
#   auto-vectorisation and no hardware sqrt/rcp, so the float traffic in the
#   software rasteriser costs what it costs on a FPU-less part.
GBSIM_ARCH  := -m32 -mfpmath=387 -mno-sse -mno-sse2 -mno-mmx -mno-3dnow \
               -fno-tree-vectorize
GBSIM_CXXFLAGS := -std=gnu++11 -O2 -g -fPIC -fno-strict-aliasing $(GBSIM_ARCH) \
                  -MMD -MP $(COMMON_DEFS) $(GBSIM_DEFS) \
                  $(WARNINGS) $(INCLUDES)
GBSIM_ENGINE := $(patsubst $(ENGINE)/%.cpp,$(GBSIM_DIR)/engine/%.o,$(RE3_ALL))
GBSIM_LIBRW  := $(patsubst $(LIBRW)/%.cpp,$(GBSIM_DIR)/librw/%.o,$(LIBRW_CPP)) \
                $(patsubst $(LIBRW)/%.c,$(GBSIM_DIR)/librw/%.o,$(LIBRW_C))
GBSIM_CORE   := $(GBSIM_DIR)/libretro_core.o $(GBSIM_DIR)/hacker_loader.o

gbsim: $(GBSIM_DIR)/libretro_re3.so
gb300runner: $(GBSIM_DIR)/gb300_runner

$(GBSIM_DIR):
	@mkdir -p $@

$(GBSIM_DIR)/engine/%.o: $(ENGINE)/%.cpp | $(GBSIM_DIR)
	@mkdir -p $(dir $@)
	@$(HOSTCXX) $(GBSIM_CXXFLAGS) -c $< -o $@

$(GBSIM_DIR)/librw/%.o: $(LIBRW)/%.cpp | $(GBSIM_DIR)
	@mkdir -p $(dir $@)
	@$(HOSTCXX) $(GBSIM_CXXFLAGS) -c $< -o $@

$(GBSIM_DIR)/librw/%.o: $(LIBRW)/%.c | $(GBSIM_DIR)
	@mkdir -p $(dir $@)
	@$(HOSTCC) $(GBSIM_CXXFLAGS) -c $< -o $@

$(GBSIM_DIR)/libretro_core.o: $(TOP)/libretro/libretro_core.cpp | $(GBSIM_DIR)
	@$(HOSTCXX) $(GBSIM_CXXFLAGS) -c $< -o $@

$(GBSIM_DIR)/hacker_loader.o: $(TOP)/libretro/hacker_loader.c | $(GBSIM_DIR)
	@$(HOSTCC) $(GBSIM_CXXFLAGS) -I$(TOP)/libretro -c $< -o $@

$(GBSIM_DIR)/libretro_re3.so: $(GBSIM_ENGINE) $(GBSIM_LIBRW) $(GBSIM_CORE)
	@echo "  LD      $@"
	@mkdir -p $(dir $@)
	@$(HOSTCXX) $(GBSIM_CXXFLAGS) -shared -o $@ $(GBSIM_ENGINE) $(GBSIM_LIBRW) \
	    $(GBSIM_DIR)/libretro_core.o $(GBSIM_DIR)/hacker_loader.o -lm -ldl

$(GBSIM_DIR)/gb300_runner: $(TOP)/runner/gb300_runner.c | $(GBSIM_DIR)
	@echo "  LD      $@"
	@$(HOSTCC) -m32 -O2 -g -I$(TOP)/libretro -o $@ $< -ldl

-include $(GBSIM_ENGINE:.o=.d) $(GBSIM_LIBRW:.o=.d) $(GBSIM_DIR)/libretro_core.d \
         $(GBSIM_DIR)/hacker_loader.d

$(NATIVE_DIR):
	@mkdir -p $@

$(NATIVE_DIR)/engine/%.o: $(ENGINE)/%.cpp | $(NATIVE_DIR)
	@mkdir -p $(dir $@)
	@$(HOSTCXX) $(NATIVE_CXXFLAGS) -c $< -o $@

$(NATIVE_DIR)/librw/%.o: $(LIBRW)/%.cpp | $(NATIVE_DIR)
	@mkdir -p $(dir $@)
	@$(HOSTCXX) $(NATIVE_CXXFLAGS) -c $< -o $@

$(NATIVE_DIR)/librw/%.o: $(LIBRW)/%.c | $(NATIVE_DIR)
	@mkdir -p $(dir $@)
	@$(HOSTCC) $(NATIVE_CXXFLAGS) -c $< -o $@

$(NATIVE_DIR)/libretro_re3.so: $(NATIVE_ENGINE) $(NATIVE_LIBRW) $(NATIVE_DIR)/libretro_core.o $(NATIVE_DIR)/hacker_loader.o
	@echo "  LD      $@"
	@mkdir -p $(dir $@)
	@$(HOSTCXX) $(NATIVE_CXXFLAGS) -shared -o $@ $(NATIVE_ENGINE) $(NATIVE_LIBRW) \
	    $(NATIVE_DIR)/libretro_core.o $(NATIVE_DIR)/hacker_loader.o -lm -lpthread -ldl

$(NATIVE_DIR)/libretro_core.o: $(TOP)/libretro/libretro_core.cpp | $(NATIVE_DIR)
	@$(HOSTCXX) $(NATIVE_CXXFLAGS) -c $< -o $@

# The on-screen boot log (libretro/hacker_loader.c).  Kept out of the engine
# objects because it is purely a libretro-layer debugging aid.
$(NATIVE_DIR)/hacker_loader.o: $(TOP)/libretro/hacker_loader.c | $(NATIVE_DIR)
	@$(HOSTCC) $(NATIVE_CXXFLAGS) -I$(TOP)/libretro -c $< -o $@

$(NATIVE_DIR)/test_runner_re3: $(TOP)/runner/test_runner_re3.c | $(NATIVE_DIR)
	@echo "  LD      $@"
	@$(HOSTCC) -O2 -g -I$(TOP)/libretro -o $@ $< -ldl

# The software rasteriser has its own test program: the engine needs GTA III's
# data files before it draws anything, so this drives librw's soft backend
# directly and checks the output pixel by pixel.
$(NATIVE_DIR)/soft_render_test: $(TOP)/runner/soft_render_test.cpp $(NATIVE_LIBRW) | $(NATIVE_DIR)
	@echo "  LD      $@"
	@$(HOSTCXX) $(NATIVE_CXXFLAGS) -o $@ $(TOP)/runner/soft_render_test.cpp \
	    $(NATIVE_LIBRW) -lm

# ---- MIPS static library ---------------------------------------------------
SF2000_DIR := $(TOP)/build/sf2000
GB300_DIR  := $(TOP)/build/gb300
SF2000_CXXFLAGS := $(MIPS_CXXFLAGS) $(SF2000_DEFS)
GB300_CXXFLAGS  := $(MIPS_CXXFLAGS) $(GB300_DEFS)

# $(1) = target prefix (sf2000/gb300), $(2) = its build directory,
# $(3) = its CXXFLAGS.  Inside a define the automatic variables and the
# arguments of $@/$$< have to be written with a second $, because the body is
# expanded once by eval before make ever sees it as a rule.
define MIPS_RULES
$(1)_ENGINE := $$(patsubst $$(ENGINE)/%.cpp,$(2)/engine/%.o,$$(RE3_ALL))
$(1)_LIBRW := $$(patsubst $$(LIBRW)/%.cpp,$(2)/librw/%.o,$$(LIBRW_CPP)) \
              $$(patsubst $$(LIBRW)/%.c,$(2)/librw/%.o,$$(LIBRW_C))

$(2)/engine/%.o: $$(ENGINE)/%.cpp
	@mkdir -p $$(dir $$@)
	@$$(FROG)g++ $(3) -c $$< -o $$@

$(2)/librw/%.o: $$(LIBRW)/%.cpp
	@mkdir -p $$(dir $$@)
	@$$(FROG)g++ $(3) -c $$< -o $$@

$(2)/librw/%.o: $$(LIBRW)/%.c
	@mkdir -p $$(dir $$@)
	@$$(FROG)gcc $(3) -c $$< -o $$@

$(2)/libretro_re3.a: $$($(1)_ENGINE) $$($(1)_LIBRW) $(2)/libretro_core.o $(2)/hacker_loader.o
	@echo "  AR      $$@"
	@$$(MIPS_AR) rcs $$@ $$($(1)_ENGINE) $$($(1)_LIBRW) $(2)/libretro_core.o $(2)/hacker_loader.o
endef

$(eval $(call MIPS_RULES,sf2000,$(SF2000_DIR),$(SF2000_CXXFLAGS)))
$(eval $(call MIPS_RULES,gb300,$(GB300_DIR),$(GB300_CXXFLAGS)))

# Every object writes a .d file next to itself, listing the headers it read.
-include $(NATIVE_ENGINE:.o=.d) $(NATIVE_LIBRW:.o=.d) $(NATIVE_DIR)/libretro_core.d $(NATIVE_DIR)/hacker_loader.d
-include $(sf2000_ENGINE:.o=.d) $(sf2000_LIBRW:.o=.d) $(SF2000_DIR)/libretro_core.d $(SF2000_DIR)/hacker_loader.d
-include $(gb300_ENGINE:.o=.d) $(gb300_LIBRW:.o=.d) $(GB300_DIR)/libretro_core.d $(GB300_DIR)/hacker_loader.d

$(SF2000_DIR)/hacker_loader.o: $(TOP)/libretro/hacker_loader.c
	@mkdir -p $(dir $@)
	@$(FROG)gcc $(SF2000_CXXFLAGS) -I$(TOP)/libretro -c $< -o $@

$(GB300_DIR)/hacker_loader.o: $(TOP)/libretro/hacker_loader.c
	@mkdir -p $(dir $@)
	@$(FROG)gcc $(GB300_CXXFLAGS) -I$(TOP)/libretro -c $< -o $@

$(SF2000_DIR)/libretro_core.o: $(TOP)/libretro/libretro_core.cpp
	@mkdir -p $(dir $@)
	@$(FROG)g++ $(SF2000_CXXFLAGS) -c $< -o $@

$(GB300_DIR)/libretro_core.o: $(TOP)/libretro/libretro_core.cpp
	@mkdir -p $(dir $@)
	@$(FROG)g++ $(GB300_CXXFLAGS) -c $< -o $@

sf2000: $(SF2000_DIR)/libretro_re3.a
gb300:  $(GB300_DIR)/libretro_re3.a

# ---- multicore packaging ---------------------------------------------------
# The multicore turns the static library into core_87000000 plus a patched
# bisrv.asd.  CORE/CONSOLE/FROGGY_TYPE are what its Makefile expects.
cores:
	@mkdir -p $(TOP)/cores/re3

sf2000-core: sf2000 cores
	@$(MAKE) -C $(MULTICORE) CORE=$(TOP)/cores/re3 CONSOLE=re3 \
	    FROGGY_TYPE=SF2000 -j$(shell nproc)

gb300-core: gb300 cores
	@$(MAKE) -C $(MULTICORE) CORE=$(TOP)/cores/re3 CONSOLE=re3 \
	    FROGGY_TYPE=GB300V2 -j$(shell nproc)

# ---- card layout ----------------------------------------------------------
# The stock FrogOS frontend on this card resolves a launch entry like this:
#
#   ROMS/<name>.gba               the stub *file name* is what FrogUI lists
#   ROMS/<corename>;<dir>;<rom>.gba  ...and its *contents* are the same triple
#   cores/<corename>/core_87000000    the binary, loaded at 0x87000000
#   ROMS/<dir>/<rom>                  handed to retro_load_game()
#
# See for instance cores/classicube/core_87000000 driven by the stub
# "ROMS/classicube;mc.gba", whose 28-byte body reads "opticraft;opticraft;opticlay".
# The core only needs a file that exists inside the data directory - it strips
# the name and keeps the directory - so the stub names a placeholder and the
# game files live in ROMS/re3/.
#
# Note this is NOT the multicore's own layout: sf2000_multicore/src/main.c:124
# reads system/Deimos/cores/<corename>.sf2k instead.  deploy-deimos installs
# that variant for anyone launching from the Deimos frontend.
SDCARD ?= /media/Sajnaps/GB300
RE3_DATA_DIR ?= $(SDCARD)/ROMS/re3
RE3_STUB ?= $(SDCARD)/ROMS/re3;re3;re3.gba
RE3_CORE_DIR ?= $(SDCARD)/cores/re3
RE3_CORE ?= $(RE3_CORE_DIR)/core_87000000

deploy-sf2000: sf2000-core install-card

# Same install; the two builds differ only in the defines they compile with.
deploy-gb300: gb300-core install-card

install-card:
	@mkdir -p $(RE3_CORE_DIR) $(RE3_DATA_DIR) $(DEIMOS_CORES_DIR) $(SDCARD)/cores
	@cp -f $(MULTICORE)/build/core_87000000 $(RE3_CORE)
	@cp -f $(MULTICORE)/build/core_87000000 $(SDCARD)/cores/re3.sf2k
	@cp -f $(MULTICORE)/build/core_87000000 $(DEIMOS_CORES_DIR)/re3.sf2k
	@printf 're3;re3;re3' > "$(RE3_STUB)"
	@touch $(RE3_DATA_DIR)/re3
	@echo "installed:"
	@echo "  core       $(RE3_CORE)"
	@echo "  sf2k core  $(SDCARD)/cores/re3.sf2k"
	@echo "  deimos     $(DEIMOS_CORES_DIR)/re3.sf2k"
	@echo "  stub       \"$(RE3_STUB)\"  ->  re3;re3;re3"
	@echo "  data dir   $(RE3_DATA_DIR)  (put models/, data/, audio/ here)"

# The multicore's own frontend (system/Deimos/cores/<corename>.sf2k).
DEIMOS_CORES_DIR ?= $(SDCARD)/system/Deimos/cores
deploy-deimos: gb300-core
	@mkdir -p $(DEIMOS_CORES_DIR)
	@cp -f $(MULTICORE)/build/core_87000000 $(DEIMOS_CORES_DIR)/re3.sf2k
	@echo "installed $(DEIMOS_CORES_DIR)/re3.sf2k"

clean:
	@rm -rf $(TOP)/build

distclean: clean
	@rm -rf $(MULTICORE)/build

