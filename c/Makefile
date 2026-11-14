# Paint, the C port.
#
#   make [TARGET=linux|win64|win32|mac] build/<target>/paint (or paint.exe)
#                                       mac: built natively on a Mac with clang, MAC_ARCH=arm64|x86_64
#   make test                           every tests/test_*.c, run (win: under wine)
#   make bench                          timings (tests/bench.c), to decide what is HOT
#   make TARGET=win32|win64 check       can the .exe load on XP (win32) / Windows 7 (win64)? (tools/check_imports.sh)
#   make run ARGS=...                   build and start it
#   make TARGET=mac bundle              build/mac/Paint.app (tools/mac_bundle.sh)
#   make clean
#   make DEBUG=1                        unstripped, with symbols (build/<target>-debug)
#
# Size and memory are goals: the binary is built for size (-Oz), except the
# pixel loops (HOT below, -O2); unused functions and data are dropped at link
# time (with gcc, by link-time optimization), and the release binary is stripped.
#
# Every .c under src/ is built, except that a file named *_<plat>.c is built
# only where <plat> is one of the target's PLATS (posix, linux, win). The
# app links the platform's host; the tests link src/host/headless.c instead
# and draw into memory.

TARGET ?= linux
comma := ,
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare

ifeq ($(TARGET),linux)
  CC       ?= cc
  PLATS    := posix linux
  # Code and data packed in the file without page padding between them. (Still
  # position-independent: non-PIE saved ~6 KB but loads Paint's code at a fixed
  # address, and Paint listens on the network when drawing together.)
  CFLAGS_T := -D_GNU_SOURCE
  LDFLAGS_T := -Wl,-z,noseparate-code -Wl,-z,common-page-size=64
  SYS_LIBS := -lpthread -lm
  EXE      := paint
  RUN      :=
else ifeq ($(TARGET),win64)
  # Windows 7 and later. Both Windows builds use the system's msvcrt.dll, not the UCRT
  # (which XP doesn't have, and Windows 7 only with an update), and its printf (mingw's
  # own C99 one adds 60 KB): so no printf formats XP's msvcrt lacks (`make check` looks).
  CROSS    ?= x86_64-w64-mingw32-
  CC       := $(CROSS)gcc
  WINDRES  := $(CROSS)windres
  PLATS    := win
  CRT      := -mcrtdll=msvcrt-os
  CFLAGS_T := -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DUNICODE -D_UNICODE $(CRT) -D__USE_MINGW_ANSI_STDIO=0
  LDFLAGS_T := -static -static-libgcc -mwindows $(CRT)
  SYS_LIBS := -lws2_32 -liphlpapi -lgdi32 -luser32 -lshell32 -lcomdlg32 -lole32 -luuid -ladvapi32 -limm32 -lm
  EXE      := paint.exe
  RUN      := WINEDEBUG=-all wine
else ifeq ($(TARGET),win32)
  # Windows XP SP3 and later, on any i686 CPU (no SSE)
  CROSS    ?= i686-w64-mingw32-
  CC       := $(CROSS)gcc
  WINDRES  := $(CROSS)windres
  PLATS    := win
  CRT      := -mcrtdll=msvcrt-os
  CFLAGS_T := -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DUNICODE -D_UNICODE -mstackrealign $(CRT) -D__USE_MINGW_ANSI_STDIO=0
  LDFLAGS_T := -static -static-libgcc -mwindows $(CRT)
  SYS_LIBS := -lws2_32 -liphlpapi -lgdi32 -luser32 -lshell32 -lcomdlg32 -lole32 -luuid -ladvapi32 -limm32 -lm
  EXE      := paint.exe
  RUN      := WINEDEBUG=-all wine
else ifeq ($(TARGET),mac)
  # macOS 10.13 and later. The Cocoa host is C calling the Objective-C runtime.
  CC       := clang
  MAC_ARCH ?= $(shell uname -m)
  PLATS    := posix mac
  CFLAGS_T := -arch $(MAC_ARCH) -mmacosx-version-min=10.13 -D_DARWIN_C_SOURCE
  LDFLAGS_T := -arch $(MAC_ARCH) -mmacosx-version-min=10.13
  SYS_LIBS := -framework Cocoa -framework CoreGraphics -framework Carbon -framework IOSurface -lobjc -lpthread -lm
  EXE      := paint
  RUN      :=
else
  $(error unknown TARGET '$(TARGET)': linux, win64, win32, mac)
endif

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
BUILD   ?= build/$(TARGET)$(if $(DEBUG),-debug)
ALL_PLATS := posix linux win mac
OTHER   := $(filter-out $(PLATS),$(ALL_PLATS))
FOUND   := $(shell find src -name '*.c')
SRCS    := $(filter-out $(foreach p,$(OTHER),%_$(p).c),$(FOUND))
APP_SRCS := $(filter-out src/host/headless.c,$(SRCS))
OBJS    := $(patsubst %,$(BUILD)/%.o,$(APP_SRCS))
# Tests: everything but main.c and the real hosts
TEST_OBJS := $(patsubst %,$(BUILD)/%.o,$(filter-out src/main.c $(filter-out src/host/headless.c,$(filter src/host/%,$(SRCS))),$(SRCS)))

# Files whose loops run per pixel, every frame, every mouse move or every undo step, built
# -O2: each is here because `make bench` showed -Oz making a frame, a stroke, the undo step,
# text dragged on the picture, or opening or saving a big picture 15% slower or more
HOT     := src/ui/gfx.c src/paint/raster.c src/paint/brush.c src/paint/text.c src/paint/history.c src/core/stb_impl.c \
           src/collab/patch.c
OPT     ?= -Oz
# (No -fdata-sections on Windows: mingw's gcc then puts zeroed data in .data$name, 90 KB of zeros in the .exe)
SIZE_CF := -ffunction-sections $(if $(filter win,$(PLATS)),,-fdata-sections) -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -fno-stack-protector -fno-ident -fmerge-all-constants $(if $(filter linux,$(PLATS)),-fno-plt)
ifeq ($(TARGET),mac)
# (-no_data_const: no separate read-only-after-start segment, a 16 KB page of the file; as -z norelro on Linux)
SIZE_LD := -Wl,-dead_strip -Wl,-no_data_const $(if $(DEBUG),,-Wl$(comma)-x -Wl$(comma)-no_function_starts)
else
SIZE_LD := -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed $(if $(DEBUG),,-s) $(if $(filter linux,$(PLATS)),-Wl$(comma)--build-id=none -Wl$(comma)-z$(comma)norelro -Wl$(comma)--hash-style=gnu)
endif
# Link-time optimization (gcc): whole-program inlining and dead code removal, each
# function still built at its file's -Oz or -O2. Not on the Mac: Apple clang's LTO
# ignores the files' size options, and the binary grows.
LTO     ?= $(if $(or $(DEBUG),$(filter mac,$(PLATS))),,1)
LTO_LD  := $(if $(LTO),-flto=auto $(OPT))
CFLAGS  := -std=c11 $(OPT) $(if $(DEBUG),-g) -pipe -MMD -MP $(WARN) $(SIZE_CF) $(CFLAGS_T) -Isrc -Ithird_party/stb \
           $(if $(LTO),-flto) -DPAINT_VERSION='"$(VERSION)"' $(EXTRA_CFLAGS)
LDFLAGS := $(LDFLAGS_T) $(SIZE_LD) $(LTO_LD) $(EXTRA_LDFLAGS)
RES     := $(if $(filter win,$(PLATS)),$(wildcard res/paint.rc))
RES_O   := $(if $(RES),$(BUILD)/res.o)

.PHONY: all test bench check run size bundle clean
all: $(BUILD)/$(EXE)

$(BUILD)/$(EXE): $(OBJS) $(RES_O)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(RES_O) $(SYS_LIBS)
	$(if $(and $(filter mac,$(PLATS)),$(if $(DEBUG),,1)),strip $@)
	@echo "built $@ ($(VERSION))"

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter $<,$(HOT)),-O2) -c $< -o $@

$(BUILD)/res.o: $(RES) $(BUILD)/paint.ico $(wildcard res/*.manifest)
	@mkdir -p $(dir $@)
	$(WINDRES) -I $(BUILD) -I res $< $@

# The Windows icon, drawn from the logo by a little tool built for (and run on)
# the build machine, since the cross-compiled paint.exe can't run here. The
# 64-bit build (Windows 7 and later) holds it as PNGs; XP reads only bitmaps.
HOST_CC ?= cc
MKICO_SRCS := tools/mkico.c src/paint/logo.c src/paint/raster.c src/paint/transform.c src/core/img.c src/core/stb_impl.c src/os/os_posix.c
$(BUILD)/paint.ico: $(MKICO_SRCS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -std=c11 -O2 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Isrc -Ithird_party/stb -o $(BUILD)/mkico $(MKICO_SRCS) -lpthread -lm
	$(BUILD)/mkico $(if $(filter win64,$(TARGET)),--png) $@

# ---- tests: tests/test_<name>.c, each its own program ----
TESTS := $(patsubst tests/%.c,$(BUILD)/tests/%$(suffix $(EXE)),$(wildcard tests/test_*.c))

$(BUILD)/tests/%$(suffix $(EXE)): $(BUILD)/tests/%.c.o $(TEST_OBJS)
	$(CC) $(filter-out -mwindows,$(LDFLAGS)) -o $@ $^ $(SYS_LIBS)

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $(RUN) $$t; done

# Timings of what a user feels (frames, strokes, fills, opening and saving), for choosing HOT
bench: $(BUILD)/tests/bench$(suffix $(EXE))
	$(RUN) $<

# Windows: the .exe imports nothing newer than XP (win32) or Windows 7 (win64), and no
# printf format in the code needs more than XP's msvcrt.dll (no %lld, %zu, %hhd, ...)
check: $(BUILD)/$(EXE)
	$(if $(filter win,$(PLATS)),,$(error make check is for TARGET=win32 or win64))
	OBJDUMP=$(CROSS)objdump tools/check_imports.sh $< $(if $(filter win32,$(TARGET)),xp,win7)
	@if grep -nE '^([^"]*"[^"]*")*[^"]*"[^"]*%[-+#0-9.*]*(ll|hh|z|j|t|L)[diouxXeEfgGaAcsn]' $(SRCS) tests/*.c; then \
	    echo "printf formats that XP's msvcrt.dll can't print"; exit 1; fi

run: $(BUILD)/$(EXE)
	$(RUN) ./$(BUILD)/$(EXE) $(ARGS)

# macOS: the app bundle around the binary (Info.plist, icon, document types)
bundle: $(BUILD)/$(EXE)
	tools/mac_bundle.sh $(BUILD)/$(EXE) $(BUILD)/Paint.app $(VERSION)

# Bytes per object, biggest first, and the binary's size. LTO objects hold no
# code yet, so the objects are counted from a build without LTO (in $(BUILD)-nolto).
size: $(BUILD)/$(EXE)
	@$(if $(LTO),$(MAKE) -s LTO= BUILD=$(BUILD)-nolto $(BUILD)-nolto/$(EXE) >/dev/null;) \
	 size $(patsubst $(BUILD)/%,$(BUILD)$(if $(LTO),-nolto)/%,$(OBJS)) 2>/dev/null | sort -k4 -n -r | head -25; ls -l $(BUILD)/$(EXE)

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
