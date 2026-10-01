# primeLua -- Lua 5.4 on the HP Prime G1 calculator
#
# What this builds
#   build/primeLua/lua.elf   the interpreter: upstream Lua 5.4.7, cross
#                            compiled to ARMv5TEJ (soft float), position
#                            independent, entry plua_entry -- the shape the
#                            calculator's MicroPython shellcode loader can
#                            load and run (DOOM / primetcc convention).
#   ../primeLua.hpappdir/    the flat deployment folder for C:\DATA\
#
# Toolchain: arm-none-eabi-gcc (Ubuntu package gcc-arm-none-eabi).
#
# Two deliberate dependencies on the sibling primetcc project:
#   * its runtime (rt/hp_rt.c printf engine, rt/hp_string.c ftoa/strtod,
#     the firmware heap allocator) is compiled in as plua_rt.o -- reusing a
#     runtime already proven on hardware beats re-deriving it;
#   * its openlibm build (rt_math.o) supplies sin/cos/... at ~1ulp so Lua's
#     math.* matches host Lua bit for bit.
# primetcc itself is not modified: primeLua compiles those sources into its
# own objects with its own flags.

TOPDIR   := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
LUASRC   := $(TOPDIR)/src/lua-5.4.7/src
# primetcc's runtime (heap allocator, string core, openlibm) is compiled into
# primeLua; normally that lives in the sibling checkout, but a source archive
# carries the handful of files needed under third_party/ so that an extracted
# copy still builds.
PRIMETCC ?= $(firstword $(wildcard $(TOPDIR)/../primetcc \
                                    $(TOPDIR)/third_party/primetcc))
BUILD    := $(TOPDIR)/build
OBJ      := $(BUILD)/obj
APPNAME  := primeLua.hpappdir
DEPLOY   := $(TOPDIR)/../$(APPNAME)

CC       := arm-none-eabi-gcc
CC_HOST  := cc
AS       := arm-none-eabi-as
LD       := arm-none-eabi-ld
AR       := arm-none-eabi-ar
OBJCOPY  := arm-none-eabi-objcopy
READELF  := arm-none-eabi-readelf
SIZE     := arm-none-eabi-size
PYTHON   := python3

# newlib's headers provide the *declarations* for anything Lua includes that
# primeLua does not implement itself; port/include comes first and shadows the
# ones that must be ours (stdio.h, stdlib.h, string.h, math.h, ...).
SYSINC   := -isystem /usr/lib/arm-none-eabi/include

ARCH     := -mcpu=arm926ej-s -marm -mfloat-abi=soft

# Lua randomizes string hashes per state (lstate.c luai_makeseed), so `pairs`
# order differs between runs of the same program.  On a calculator that is a
# usability bug, not a security feature -- the same script should print the
# same thing every time -- and it would also make the differential tests
# against the reference binary meaningless.  Both builds use this seed.
HOST_SEED := -Dluai_makeseed(L)=0x9e3779b9u

# -ffunction-sections/-fdata-sections + --gc-sections: Lua has several
# hosted-only corners (dlopen stubs, readline) and plua_rt.o carries the
# whole TCC runtime; only what the interpreter actually reaches is kept.
LUAFLAGS := $(ARCH) -ffreestanding -fPIC -ffunction-sections -fdata-sections \
            -O2 -g -fno-strict-aliasing -Wall -Wno-unused-parameter \
            -I$(TOPDIR)/port/include -I$(TOPDIR)/port -I$(LUASRC) \
            -I$(PRIMETCC)/rt $(SYSINC) \
            -DLUA_COMPAT_5_3 '$(HOST_SEED)'
# -DLUA_COMPAT_5_3 is what the upstream Makefile passes (it is what gives the
# reference `lua` binary math.log10/math.pow), so primeLua compiles with it too:
# same source, same macros, same behaviour.

# Every path the calculator will use: the app folder, flat (the firmware
# cannot make subfolders), plus the C:\DATA root for shared modules.
CONF     := -DLUA_DIRSEP='"\\"' -DLUA_CPATH_DEFAULT='""' \
            -DLUA_PATH_DEFAULT='"C:\\DATA\\$(APPNAME)\\?.lua;C:\\DATA\\$(APPNAME)\\?\\init.lua;C:\\DATA\\?.lua"'

# ---------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------
# Lua core + standard library, exactly the set the reference `lua` binary
# uses.  luac.c (the bytecode compiler) is deliberately absent: primeLua runs
# sources, and the loader would not know what to do with a second main().
LUA_CORE := lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c \
            llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c \
            ltable.c ltm.c lundump.c lvm.c lzio.c
LUA_LIB  := lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c lmathlib.c \
            loslib.c lstrlib.c ltablib.c lutf8lib.c loadlib.c linit.c
LUA_MAIN := lua.c

LUA_OBJS := $(patsubst %.c,$(OBJ)/lua/%.o,$(LUA_CORE) $(LUA_LIB))
DRV_OBJS := $(OBJ)/lua/lua.o

# primeLua's own platform layer
#   plua_run.c    how a run starts (firmware thread + run state) and how the
#                 launcher streams it: see port/plua_run.h
PORT_SRCS := plua_fwmode.c plua_file.c plua_libc.c plua_math.c plua_time.c plua_main.c \
             plua_dd.c plua_font_montserrat_data.c plua_font_cascadia_data.c \
             plua_font_big_data.c plua_input.c plua_run.c \
             plua_format.c plua_strtod.c plua_diag.c plua_hp.c
PORT_OBJS := $(patsubst %.c,$(OBJ)/port/%.o,$(PORT_SRCS)) $(OBJ)/port/plua_svc.o

# primetcc's runtime, compiled here into primeLua's own objects.
#   hp_entry       -> renamed: primeLua has its own entry (bigger stack)
#   printf/vprintf/puts/sprintf/snprintf/vsnprintf/vsprintf
#                  -> renamed: primeLua's own engine (port/plua_format.c) must
#                     win.  The runtime's %g ignores precision and its %f
#                     stops at 15 decimals, and Lua prints every number with
#                     "%.14g" -- see plua_format.c for the full story.
#   strtod/atof    -> renamed for the same reason in the other direction:
#                     Lua's lexer parses every numeric literal with strtod, and
#                     the runtime's version accumulates in floating point (off
#                     by several ulp, no hex floats).  port/plua_strtod.c
#                     replaces it.  What IS still reused from the runtime: the
#                     firmware heap allocator and the string/memory core.
RT_DEFS  := -Dhp_entry=hp_rt_entry_unused -Dprintf=hp_rt_printf_unused \
            -Dvprintf=hp_rt_vprintf_unused -Dputs=hp_rt_puts_unused \
            -Dsprintf=hp_rt_sprintf_unused -Dsnprintf=hp_rt_snprintf_unused \
            -Dvsprintf=hp_rt_vsprintf_unused \
            -Dvsnprintf=hp_rt_vsnprintf_unused \
            -Dstrtod=hp_rt_strtod_unused -Datof=hp_rt_atof_unused \
            -Ddiag_log=hp_rt_diag_log_unused \
            -Dprints=hp_rt_prints_unused \
            -Dhp_log_write=plua_hw_log_write \
            -Dhp_log_char=plua_hw_log_char
RT_FLAGS := $(ARCH) -ffreestanding -fPIC -ffunction-sections -fdata-sections \
            -O2 -fno-strict-aliasing -Wall -Wno-unused-parameter \
            -I$(PRIMETCC)/rt -I$(PRIMETCC)/hp -I$(TOPDIR)/port/include $(SYSINC)

# The input path is primeLua's own (port/plua_input.c) and is NOT built from
# primetcc, which is the point: hp_input.c's slot write there flushes "the whole
# D-cache + the whole TLB" (its current default), and that RESETS this
# calculator the moment a script calls key.install() -- the build that is known
# good on hardware differs from it in exactly those CP15 operations.  primetcc
# keeps its version for its own build (see the HP_INPUT_MINIMAL_FLUSH note in
# primetcc/rt/hp_input.c); primeLua no longer links it at all.
HP_INPUT_DEFS :=

# the remaining primetcc sources log through diag_log/prints; send that to
# primeLua's own log instead of primetcc's crash.log path
HP_DEFS :=
RT_SRCS  := hp_rt.c hp_string.c
RT_OBJS  := $(patsubst %.c,$(OBJ)/rt/%.o,$(RT_SRCS)) $(OBJ)/rt/rt_math.o

# primetcc's hardware libraries, exposed to Lua by port/plua_hp.c:
#   hp_gfx    drawing (GROB double buffering, primitives, 5x8 text)
#   (keyboard/touch: primeLua's own port/plua_input.c, see below)
#   hp_sys    heap figures, sleep
#   hp_random PCG32 + value noise
#   hp_codec  crc32/adler32/hex/base64
#   hp_fixmath Q16.16 fixed point
#   hp_fonts   the real font engine: Montserrat (prop) / Cascadia Code Light
#              (mono) glyph rendering (hp_fonts.c is 892 B of code; the GLYPH
#              DATA is what costs, and primeLua brings those two families at
#              16/24/32 px only -- see port/plua_font_montserrat_data.c and
#              port/plua_font_cascadia_data.c, generated by
#              tools/make_font_montserrat.py and tools/make_font_cascadia.py,
#              ~26 KB + ~17 KB instead of the 70 KB that linking
#              hp_fonts_data.c's eight faces would pull in)
# hp_gui/hp_image are NOT linked: the calculator's free heap is better spent on
# the script's own data.  Add them here if wanted.
# hp_input.c / hp_input_svc.c are deliberately NOT here: primeLua owns its
# input path now (port/plua_input.c).  Those two files are shared with another
# project and have been rewritten under us once already -- with whole-D-cache
# and whole-TLB maintenance in the slot write that resets this calculator (see
# the note in primetcc/rt/hp_input.c and README "输入钩子").  Everything else
# from primetcc is still compiled as before.
HP_SRCS  := hp_gfx.c hp_sys.c hp_random.c \
            hp_codec.c hp_fixmath.c hp_fonts.c
HP_OBJS  := $(patsubst %.c,$(OBJ)/rt/%.o,$(HP_SRCS))   # the generated font data is
                                                       # a port source (PORT_SRCS)

# openlibm (BSD), renamed to hp_* -- same recipe as primetcc's Makefile so the
# two projects cannot drift apart numerically.
OLM        := $(PRIMETCC)/rt/openlibm
OLM_FLAGS  := -O2 -ffreestanding -fPIC -mcpu=arm926ej-s -marm -mfloat-abi=soft -I$(OLM)
OLM_RENAMES := -Dsin=hp_sin -Dcos=hp_cos -Dtan=hp_tan -Datan=hp_atan \
	-Datan2=hp_atan2 -Dasin=hp_asin -Dacos=hp_acos -Dexp=hp_exp \
	-Dlog=hp_log -Dlog10=hp_log10 -Dpow=hp_pow -Dsqrt=hp_sqrt \
	-Dsinh=hp_sinh -Dcosh=hp_cosh -Dtanh=hp_tanh -Dfloor=hp_floor \
	-Dceil=hp_ceil -Dfabs=hp_fabs -Dtrunc=hp_trunc -Dround=hp_round \
	-Dfmod=hp_fmod -Dfrexp=hp_frexp -Dmodf=hp_modf \
	-Dscalbn=hp_scalbn -Dexpm1=hp_expm1 -Dlog2=hp_log2
# primetcc's openlibm subset was cut down to what TCC programs used, which
# left out log2 -- Lua's math.log(x, 2) needs it.  primeLua vendors the one
# missing upstream file (BSD, same licence as the rest of openlibm) instead
# of approximating log2 with log(x)/ln2, so results stay ~1ulp like the rest.
OLM_EXTRA := $(TOPDIR)/port/openlibm-extra/e_log2.c
OLM_SRCS   := k_sin.c k_cos.c k_tan.c s_sin.c s_cos.c s_tan.c s_atan.c \
	e_atan2.c e_asin.c e_acos.c e_rem_pio2.c k_rem_pio2.c e_exp.c \
	e_log.c e_log10.c e_pow.c e_sqrt.c e_sinh.c e_cosh.c s_tanh.c \
	s_floor.c s_ceil.c s_fabs.c s_trunc.c s_round.c e_fmod.c s_frexp.c \
	s_modf.c k_exp.c s_scalbn.c s_expm1.c

ELF      := $(BUILD)/$(APPNAME)/lua.elf
HOSTDIR  := $(BUILD)/host
REF_LUA  := $(HOSTDIR)/src/lua

# Lua's own sources include primeLua's headers (stdio.h, stdlib.h, time.h, ...),
# so changing a header must rebuild them.  Without this, a rebuilt plua_time.c
# with a 64-bit time_t got linked against a stale loslib.o that still used a
# 32-bit one -- and os.date() printed dates in the year 253868309.
PORT_HEADERS := $(wildcard $(TOPDIR)/port/include/*.h) $(TOPDIR)/port/plua_port.h

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
all: $(ELF) deploy

$(ELF): $(PORT_OBJS) $(LUA_OBJS) $(DRV_OBJS) $(RT_OBJS) $(HP_OBJS) | $(BUILD)/$(APPNAME)
	$(CC) -nostdlib -fPIC -pie -Wl,-e,plua_entry -Wl,--gc-sections \
	    $(PORT_OBJS) $(LUA_OBJS) $(DRV_OBJS) $(RT_OBJS) $(HP_OBJS) -lgcc -o $@
	@$(PYTHON) $(TOPDIR)/tools/fix_relocs.py $@
	@echo "==> $@ built"
	@$(READELF) -h $@ | grep -E 'Type|Entry|Machine'
	@$(READELF) -r $@ | awk 'NF>2 {print $$3}' | sort | uniq -c | sort -rn | head -5
	@$(SIZE) $@

# linit.c walks the standard libraries; primeLua adds its own opener there so
# the hardware libraries are already in place when a chunk runs (and so they
# are reachable as ordinary globals, like the stdlib).
$(OBJ)/lua/linit.o: $(FLAGS_STAMP) $(LUASRC)/linit.c $(PORT_HEADERS) | $(OBJ)/lua
	$(CC) $(LUAFLAGS) $(CONF) -DluaL_openlibs=plua_openlibs -c $< -o $@

$(OBJ)/lua/%.o: $(FLAGS_STAMP) $(LUASRC)/%.c $(PORT_HEADERS) | $(OBJ)/lua
	$(CC) $(LUAFLAGS) $(CONF) -c $< -o $@

# lua.c's main() becomes a library function: primeLua's own entry calls it
# after resolving argc/argv from the launch config.
$(OBJ)/lua/lua.o: $(FLAGS_STAMP) $(LUASRC)/lua.c $(PORT_HEADERS) | $(OBJ)/lua
	$(CC) $(LUAFLAGS) $(CONF) -Dmain=plua_lua_standalone_main -c $< -o $@

$(OBJ)/port/%.o: $(FLAGS_STAMP) $(TOPDIR)/port/%.c $(PORT_HEADERS) | $(OBJ)/port
	$(CC) $(LUAFLAGS) -c $< -o $@

$(OBJ)/port/plua_svc.o: $(FLAGS_STAMP) $(TOPDIR)/port/plua_svc.s | $(OBJ)/port
	$(AS) -mcpu=arm926ej-s $< -o $@

$(OBJ)/rt/%.o: $(FLAGS_STAMP) $(PRIMETCC)/rt/%.c | $(OBJ)/rt
	$(CC) $(RT_FLAGS) $(RT_DEFS) -c $< -o $@

# the hardware libraries: same flags, plus the diag_log redirect so their
# diagnostics land in primeLua's log rather than a primetcc path
$(OBJ)/rt/hp_gfx.o $(OBJ)/rt/hp_sys.o $(OBJ)/rt/hp_random.o \
$(OBJ)/rt/hp_codec.o $(OBJ)/rt/hp_fixmath.o: $(FLAGS_STAMP) $(OBJ)/rt/%.o: $(PRIMETCC)/rt/%.c | $(OBJ)/rt
	$(CC) $(RT_FLAGS) $(RT_DEFS) $(HP_DEFS) $(HP_INPUT_DEFS) -c $< -o $@

# openlibm: compile in its own directory (the sources include sibling files),
# then merge into a single relocatable object.
$(OBJ)/rt/rt_math.o: $(addprefix $(OLM)/,$(OLM_SRCS)) $(OLM_EXTRA) | $(OBJ)/rt
	@rm -rf $(OBJ)/olmx && mkdir -p $(OBJ)/olmx
	@cd $(OLM) && $(CC) $(OLM_FLAGS) $(OLM_RENAMES) -c $(OLM_SRCS)
	@mv $(OLM)/*.o $(OBJ)/olmx/
	@$(CC) $(OLM_FLAGS) $(OLM_RENAMES) -c $(OLM_EXTRA) -o $(OBJ)/olmx/e_log2.o
	@$(LD) -r $(OBJ)/olmx/*.o -o $@

# Build flags as a dependency.  Without this, editing a -D (which is how the
# runtime's symbols get renamed, and how linit.c gets its opener wrapped)
# leaves stale objects behind and links a mix of old and new code -- a 64-bit
# time_t against a 32-bit loslib.o cost an afternoon once already.
FLAGS_STAMP := $(BUILD)/flags.stamp
BUILD_FLAGS := $(LUAFLAGS) $(CONF) $(RT_FLAGS) $(RT_DEFS) $(HP_DEFS) $(HP_INPUT_DEFS)

$(FLAGS_STAMP): FORCE | $(BUILD)
	@printf '%s' '$(BUILD_FLAGS)' | cmp -s - $@ || \
	    printf '%s' '$(BUILD_FLAGS)' > $@

FORCE:

$(OBJ)/lua $(OBJ)/port $(OBJ)/rt:
	@mkdir -p $@

$(BUILD)/$(APPNAME):
	@mkdir -p $@

# ---------------------------------------------------------------------------
# deployment: flat folder, dropped into the calculator's C:\DATA\
#
# The deploy folder is a BUILD PRODUCT: it is wiped and rebuilt from scratch on
# every deploy.  That is deliberate -- an incremental copy leaves files behind
# that this version no longer ships (an old ti.lua, a retired example), and
# those leftovers are exactly what a clean rebuild is supposed to rule out.
# Scripts of your own belong in C:\DATA\, not in here.
# ---------------------------------------------------------------------------
deploy: $(ELF)
	@rm -rf $(DEPLOY)
	@mkdir -p $(DEPLOY)
# Ship a stripped lua.elf: the debug info is 1.2 MB of the 1.5 MB file and the
# calculator only ever reads the PT_LOAD segments (the loader copies 298 KB).
	@$(OBJCOPY) --strip-debug $(ELF) $(DEPLOY)/lua.elf
# Every calculator-side Python tool, not just the launcher: the probe scripts
# (calc/svcprobe.py, calc/timeprobe.py) are run from the device's Python app and
# have to live next to main.py to be importable.
	@cp $(TOPDIR)/calc/*.py $(DEPLOY)/
# main.py's prose travels with it (it is not decoration: the calculator charges
# ~6 MB of RAM for main.py's 156 KB of source, so the comments live in this file
# and are stripped out of the code by tools/strip_main.py)
	@cp $(TOPDIR)/calc/*.md $(DEPLOY)/
	@cp $(TOPDIR)/examples/*.lua $(DEPLOY)/
	@cp $(TOPDIR)/README.md $(DEPLOY)/primeLua.md
	@cp $(TOPDIR)/api.md $(DEPLOY)/api.md
	@echo "==> deployed to $(DEPLOY)"
	@ls -la $(DEPLOY)

# ---------------------------------------------------------------------------
# the zip that gets copied to the calculator (or handed to someone else).
# Python's zipfile rather than the zip(1) program: the rest of this workspace
# packages that way, and zip(1) is not installed here.
# ---------------------------------------------------------------------------
DIST := $(TOPDIR)/dist

dist: deploy
	@$(PYTHON) $(TOPDIR)/tools/make_dist.py
	@ls -la $(DIST)

# ---------------------------------------------------------------------------
# the clock's native digit face.  It is NOT one of the two text families: it is
# a single-size, glyph-subset face ('-' '.' '/' 0-9 ':') rasterized at the size
# it is drawn, because magnifying a small face on the calculator looks blurry.
# Regenerate after a font change, or with a different size:
#
#     make font-big              # 56 px, cell 33 px -> 8 cells = 264 px of 320
#     make font-big BIGSIZE=64   # 64 px, cell 38 px -> 304 px, the maximum
# ---------------------------------------------------------------------------
BIGSIZE ?= 56

font-big:
	@$(PYTHON) $(TOPDIR)/tools/make_font_big.py $(BIGSIZE)
	@echo "==> now rebuild: the face is compiled into lua.elf"

# ---------------------------------------------------------------------------
# main.py's prose, out of main.py.
#
# WHY: `import main` is the first thing the calculator's Python app does, and the
# firmware compiles the whole source into its own memory -- 156 KB of source cost
# about 6 MB of heap before a single Lua script runs.  Comments and docstrings
# were 45% of those bytes.  Write them in calc/main.py as usual and run this: it
# MOVES them into calc/main_notes.md (in the order they appeared, grouped by the
# definition they sat in) and leaves the code.  Nothing is deleted.
# ---------------------------------------------------------------------------
strip-main:
	@$(PYTHON) $(TOPDIR)/tools/strip_main.py

# fails when calc/main.py has grown prose back into it (for a pre-commit check)
check-main:
	@$(PYTHON) $(TOPDIR)/tools/strip_main.py --check

# ---------------------------------------------------------------------------
# host reference interpreter: the same upstream sources, built for the host
# with the same string-hash seed.  tests/harness_lua.py runs every test script
# under this binary and under lua.elf and requires identical output -- that
# comparison is what makes "primeLua is Lua" a measurable claim rather than a
# hope.
# ---------------------------------------------------------------------------


hostref: | $(BUILD)
	@rm -rf $(HOSTDIR)
	@mkdir -p $(HOSTDIR)
	@cp -r $(LUASRC)/../* $(HOSTDIR)/
	@cd $(HOSTDIR) && $(MAKE) --no-print-directory posix \
	    MYCFLAGS='-Dluai_makeseed\(L\)=0x9e3779b9u' >/dev/null
	@echo "==> host reference: $(REF_LUA)"
	@$(REF_LUA) -v

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
test: $(ELF) hostref
	@$(PYTHON) $(TOPDIR)/tests/harness_lua.py

# the launcher itself (calc/main.py) against the emulator, with a mock
# MicroPython whose file model behaves like the device's
test-launcher: $(ELF)
	@$(PYTHON) $(TOPDIR)/tests/test_launcher.py

# the hardware libraries (gfx/key/sys/rand/codec/fx) through the emulator's
# framebuffer and input slot, with the key hook installed
test-libs: $(ELF)
	@$(PYTHON) $(TOPDIR)/tests/test_hp_libs.py

# the input path's safety property: after ANY run -- including one that dies
# with the hook installed -- the firmware's get_event slot holds the firmware's
# own bytes again and the hook is disarmed.  This is the failure that broke the
# real machine (see tests/test_input.py's header).  Also covers the touch decode
# (press / jitter-filtered move / move / release + key.touch()).
test-input: $(ELF)
	@$(PYTHON) $(TOPDIR)/tests/test_input.py

# STREAMING: the program runs on a firmware thread and the launcher prints its
# output as it is produced -- and can stop it with ESC.  The suite pins both
# down through the real calc/main.py (tests/test_stream.py)
test-stream: $(ELF)
	@$(PYTHON) $(TOPDIR)/tests/test_stream.py

# repeated runs and RESTARTS must not consume firmware heap.  The calculator
# dropped 15 MB -> 13 MB -> 12.8 MB over three runs; the emulator says where
# the bytes go, and that a later session adopts the resident image (lua.slot)
# instead of loading a second copy of it.
test-leak: $(ELF)
	@$(PYTHON) $(TOPDIR)/tests/leak_check.py

# everything that runs on the HOST, no emulator -- seconds, no calculator.
# The emulator suites are separate and take minutes; this is the loop to run
# while editing C.
test-fast: test-host test-modes

# the printf engine, differentially against glibc (seconds instead of minutes)
test-format:
	@mkdir -p $(OBJ)/hosttest
	$(CC_HOST) -O1 -g -Wall -c $(TOPDIR)/port/plua_format.c \
	    -Dvsnprintf=pl_vsnprintf -Dsnprintf=pl_snprintf \
	    -Dsprintf=pl_sprintf -Dvsprintf=pl_vsprintf \
	    -o $(OBJ)/hosttest/fmt_engine.o
	$(CC_HOST) -O1 -g -Wall -c $(TOPDIR)/tests/test_format_host.c \
	    -o $(OBJ)/hosttest/fmt_test.o
	$(CC_HOST) $(OBJ)/hosttest/fmt_engine.o $(OBJ)/hosttest/fmt_test.o \
	    -o $(OBJ)/hosttest/fmt -lm
	@$(OBJ)/hosttest/fmt

# the string-to-double converter, differentially against glibc.  The two
# translation units MUST be compiled separately: the -D renames apply to the
# engine only, so the test file still calls the real glibc strtod.
test-strtod:
	@mkdir -p $(OBJ)/hosttest
	$(CC_HOST) -O2 -Wall -c $(TOPDIR)/port/plua_strtod.c \
	    -Dstrtod=pl_strtod -Datof=pl_atof -o $(OBJ)/hosttest/sd_engine.o
	$(CC_HOST) -O2 -Wall -c $(TOPDIR)/tests/test_strtod_host.c \
	    -o $(OBJ)/hosttest/sd_test.o
	$(CC_HOST) $(OBJ)/hosttest/sd_engine.o $(OBJ)/hosttest/sd_test.o \
	    -o $(OBJ)/hosttest/strtod -lm
	@$(OBJ)/hosttest/strtod

# strftime / gmtime / mktime, differentially against glibc (including the
# aliasing trick os.date() plays: strftime(buff, n, buff, stm))
test-time:
	@mkdir -p $(OBJ)/hosttest
	$(CC_HOST) -O1 -g -Wall -I$(TOPDIR)/port -c $(TOPDIR)/port/plua_time.c \
	    -Dstrftime=pl_strftime -Dlocaltime=pl_localtime -Dgmtime=pl_gmtime \
	    -Dmktime=pl_mktime -Dtime=pl_time -Dclock=pl_clock \
	    -Ddifftime=pl_difftime \
	    -D'PLUA_GETTIME_CALL(buf)=plua_test_gettime(buf)' \
	    -D'PLUA_GETTIME_DECL=unsigned plua_test_gettime(unsigned short *)' \
	    -o $(OBJ)/hosttest/tm_engine.o
	$(CC_HOST) -O1 -g -Wall -c $(TOPDIR)/tests/test_time_host.c \
	    -o $(OBJ)/hosttest/tm_test.o
	$(CC_HOST) $(OBJ)/hosttest/tm_engine.o $(OBJ)/hosttest/tm_test.o \
	    -o $(OBJ)/hosttest/time
	@$(OBJ)/hosttest/time

# the double-double library, differentially against mpmath at 60 digits
test-dd:
	@$(PYTHON) $(TOPDIR)/tools/gen_dd_expected.py
	$(CC_HOST) -O2 -ffp-contract=off -fno-fast-math -I$(TOPDIR)/port \
	    -I$(TOPDIR)/tests $(TOPDIR)/tests/test_dd_host.c \
	    $(TOPDIR)/port/plua_dd.c -lm -o $(OBJ)/hosttest/dd
	@$(OBJ)/hosttest/dd

# the FILE-mode translation (the firmware only accepts "rb" and "wb+")
test-modes:
	@mkdir -p $(OBJ)/hosttest
	$(CC_HOST) -O1 -Wall -I$(TOPDIR)/port -c $(TOPDIR)/port/plua_fwmode.c \
	    -o $(OBJ)/hosttest/mode_engine.o
	$(CC_HOST) -O1 -Wall -c $(TOPDIR)/tests/test_modes_host.c \
	    -o $(OBJ)/hosttest/mode_test.o
	$(CC_HOST) $(OBJ)/hosttest/mode_engine.o $(OBJ)/hosttest/mode_test.o \
	    -o $(OBJ)/hosttest/modes
	@$(OBJ)/hosttest/modes

# every host-side differential test that does not need the emulator
test-host: test-format test-strtod test-time test-modes test-dd

clean:
	rm -rf $(BUILD) $(DEPLOY)/lua.elf

distclean: clean
	rm -rf $(DEPLOY)

# ---------------------------------------------------------------------------
# the same differential tests, but on 32-bit ARM under qemu-arm.
#
# Worth the extra toolchain: the host machine is aarch64, where long is 64 bits
# and the code generation has nothing to do with the calculator's.  These
# builds run on qemu-arm, an independent, industrially-proven ARM
# implementation -- and that is exactly how the Python firmware emulator's
# missing ARMv5TE DSP multiplies were found (see tests/qemu_arm/).
# ---------------------------------------------------------------------------
test-arm:
	@$(PYTHON) $(TOPDIR)/tests/qemu_arm/run_arm_tests.py

.PHONY: all deploy dist clean distclean strip-main check-main hostref \
	test test-launcher \
	test-libs test-input test-stream test-leak test-format test-strtod test-time \
	test-modes test-dd test-host test-arm test-fast font-big FORCE
