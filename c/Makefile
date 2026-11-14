# Paint, the C port.
#
#   make [TARGET=linux|win64|win32|mac] build/<target>/paint (or paint.exe)
#                                       mac: built natively on a Mac with clang, MAC_ARCH=arm64|x86_64
#   make test                           every tests/test_*.c, run (win: under wine)
#   make run ARGS=...                   build and start it
#   make TARGET=mac bundle              build/mac/Paint.app (tools/mac_bundle.sh)
#   make clean
#   make DEBUG=1                        unstripped, with symbols (build/<target>-debug)
#
# Size and memory are goals: the binary is built for size (-Os), except the
# pixel loops (HOT below, -O2); unused functions and data are dropped at link
# time, and the release binary is stripped.
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
  CFLAGS_T := -D_GNU_SOURCE
  LDFLAGS_T :=
  SYS_LIBS := -lpthread -lm
  EXE      := paint
  RUN      :=
else ifeq ($(TARGET),win64)
  CROSS    ?= x86_64-w64-mingw32-
  CC       := $(CROSS)gcc
  WINDRES  := $(CROSS)windres
  PLATS    := win
  CFLAGS_T := -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DUNICODE -D_UNICODE -D__USE_MINGW_ANSI_STDIO=1
  LDFLAGS_T := -static -static-libgcc -mwindows
  SYS_LIBS := -lws2_32 -liphlpapi -lgdi32 -luser32 -lshell32 -lcomdlg32 -lole32 -luuid -ladvapi32 -limm32 -lm
  EXE      := paint.exe
  RUN      := WINEDEBUG=-all wine
else ifeq ($(TARGET),win32)
  CROSS    ?= i686-w64-mingw32-
  CC       := $(CROSS)gcc
  WINDRES  := $(CROSS)windres
  PLATS    := win
  CFLAGS_T := -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DUNICODE -D_UNICODE -D__USE_MINGW_ANSI_STDIO=1 -mstackrealign
  LDFLAGS_T := -static -static-libgcc -mwindows
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

# Files whose loops run per pixel, every frame or every mouse move
HOT     := src/ui/gfx.c src/paint/raster.c src/paint/brush.c src/paint/text.c src/core/stb_impl.c src/collab/patch.c
OPT     ?= -Os
SIZE_CF := -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector \
           -fno-ident -fmerge-all-constants $(if $(filter linux,$(PLATS)),-fno-plt)
ifeq ($(TARGET),mac)
SIZE_LD := -Wl,-dead_strip $(if $(DEBUG),,-Wl$(comma)-x)
else
SIZE_LD := -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed $(if $(DEBUG),,-s) $(if $(filter linux,$(PLATS)),-Wl$(comma)--build-id=none -Wl$(comma)-z$(comma)norelro -Wl$(comma)--hash-style=gnu)
endif
CFLAGS  := -std=c11 $(OPT) $(if $(DEBUG),-g) -pipe -MMD -MP $(WARN) $(SIZE_CF) $(CFLAGS_T) -Isrc -Ithird_party/stb \
           -DPAINT_VERSION='"$(VERSION)"' $(EXTRA_CFLAGS)
LDFLAGS := $(LDFLAGS_T) $(SIZE_LD)
RES     := $(if $(filter win,$(PLATS)),$(wildcard res/paint.rc))
RES_O   := $(if $(RES),$(BUILD)/res.o)

.PHONY: all test run size bundle clean
all: $(BUILD)/$(EXE)

$(BUILD)/$(EXE): $(OBJS) $(RES_O)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(RES_O) $(SYS_LIBS)
	$(if $(and $(filter mac,$(PLATS)),$(if $(DEBUG),,1)),strip -x $@)
	@echo "built $@ ($(VERSION))"

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter $<,$(HOT)),-O2) -c $< -o $@

$(BUILD)/res.o: $(RES) res/paint.ico $(wildcard res/*.manifest)
	@mkdir -p $(dir $@)
	$(WINDRES) -I res $< $@

# The Windows icon, drawn from the logo by a little tool built for (and run on)
# the build machine, since the cross-compiled paint.exe can't run here
HOST_CC ?= cc
MKICO_SRCS := tools/mkico.c src/paint/logo.c src/paint/raster.c src/paint/transform.c src/core/img.c src/core/stb_impl.c src/os/os_posix.c
res/paint.ico: $(MKICO_SRCS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -std=c11 -O2 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Isrc -Ithird_party/stb -o $(BUILD)/mkico $(MKICO_SRCS) -lpthread -lm
	$(BUILD)/mkico $@

# ---- tests: tests/test_<name>.c, each its own program ----
TESTS := $(patsubst tests/%.c,$(BUILD)/tests/%$(suffix $(EXE)),$(wildcard tests/test_*.c))

$(BUILD)/tests/%$(suffix $(EXE)): $(BUILD)/tests/%.c.o $(TEST_OBJS)
	$(CC) $(filter-out -mwindows,$(LDFLAGS)) -o $@ $^ $(SYS_LIBS)

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $(RUN) $$t; done

run: $(BUILD)/$(EXE)
	$(RUN) ./$(BUILD)/$(EXE) $(ARGS)

# macOS: the app bundle around the binary (Info.plist, icon, document types)
bundle: $(BUILD)/$(EXE)
	tools/mac_bundle.sh $(BUILD)/$(EXE) $(BUILD)/Paint.app $(VERSION)

# Bytes per object, biggest first, and the binary's size
size: $(BUILD)/$(EXE)
	@size $(OBJS) 2>/dev/null | sort -k4 -n -r | head -25; ls -l $(BUILD)/$(EXE)

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
