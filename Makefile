# MellaGUI - a Wayland compositor + shell for Linux without X11.
#
#   make            build everything that has sources (the fuzzer, build/fuzz-server, too)
#   make mellagui      build/mellagui        (compositor, src/*.c)
#   make mellagui-term build/mellagui-term   (client/mellagui-term.c)
#   make mellagui-calc build/mellagui-calc   (client/mellagui-calc.c)
#   make mellagui-matrix build/mellagui-matrix (client/mellagui-matrix.c, the screensaver)
#   make mellagui-du   build/mellagui-du   (client/mellagui-du.c, disk usage: list and tree map)
#   make mellagui-top  build/mellagui-top  (client/mellagui-top.c, the process and service manager)
#   make mellagui-cam  build/mellagui-cam    (client/mellagui-cam.c, the webcam viewer, V4L2)
#   make mellagui-status   build/mellagui-status (client/mellagui-status.c, the battery and the network in the panel)
#   make mellagui-gadget  build/mellagui-gadget (client/mellagui-gadget.c, the desktop gadgets: clipboard, mixer, system, smart home)
#   make mellagui-notes    build/mellagui-notes (client/mellagui-notes.c, the history of notifications)
#   make mellagui-desktop  build/mellagui-desktop (client/mellagui-desktop.c, the icons of the folder ~/Desktop)
#   make mellagui-archive  build/mellagui-archive (client/mellagui-archive.c, the archiver: open, extract and make archives)
#   make mellagui-shot     build/mellagui-shot (client/mellagui-shot.c, a screenshot of an area with marks)
#   make mellagui-paint    build/mellagui-paint (client/mellagui-paint.c, a simple picture editor)
#   make mellagui-solitaire build/mellagui-solitaire (client/mellagui-solitaire.c, Klondike)
#   make mellagui-tetris   build/mellagui-tetris (client/mellagui-tetris.c, Tetris)
#   make mellagui-chess    build/mellagui-chess (client/mellagui-chess.c, Chess against the computer; rules chess.c, search chessai.c, pieces chessart.h)
#   make build/chessai-test the chess search and evaluation (client/chessai.c): tests, --match A B N (self play), --bench
#   make build/chess-test  the rules engine of the chess game (client/chess.c) and its tests: perft, FEN / SAN, draws
#   make mellagui-blocks   build/mellagui-blocks (client/mellagui-blocks.c, a Minecraft-style block game; world blocks.c, picture blocksview.c)
#   make build/blocks-test  the block world (client/blocks.c, the picture client/blocksview.c): tests, --bench, --shot, --info
#   make mellagui-smart  build/mellagui-smart (client/mellagui-smart.c, the devices of a Smart Life / Tuya account)
#   make mellagui-bluetooth  build/mellagui-bluetooth (client/mellagui-bluetooth.c, the Bluetooth devices window)
#   make mellagui-calendar build/mellagui-calendar (client/mellagui-calendar.c, the calendar of the panel's clock)
#   make mellagui-record build/mellagui-record (client/mellagui-record.c, the screen recorder: MP4 of JPEG pictures)
#   make mellagui-run  build/mellagui-run    (client/mellagui-run.c, the Run dialog)
#   make mellagui-settings build/mellagui-settings (client/mellagui-settings.c, the settings panel)
#   make mellagui-monitor build/mellagui-monitor (client/mellagui-monitor.c, the monitor's own settings over DDC/CI)
#   make run        build, then start the compositor on a free VT (see RUN_ARGS)
#   make asan       build/mellagui-asan   (compositor with ASan + UBSan)
#   make test       build, then run tests/run.sh
#   make fuzz       build/fuzz-server     (coverage-guided fuzzer of the whole compositor, tests/fuzz_server.c)
#   make fuzz-run   build it and fuzz for FUZZ_TIME seconds (default 60); the corpus is kept in build/fuzz/corpus
#   make install    build, then put EVERYTHING in place (asks for sudo for the copying): the programs, the login screen (greetd service, PAM files,
#                   the greeter user, the session entry) and the docs; PREFIX=/usr/local, DESTDIR= for packagers; see "install" below
#   make enable-greeter   make the login screen the display manager (takes tty1 from getty / gdm / lightdm / sddm; works from the next boot)
#   make uninstall  remove what install put in place
#   make clean

CC      := gcc
CSTD    := -std=gnu11
WARN    := -Wall -Wextra
OPT     := -O2 -g
CFLAGS  ?= $(OPT)
override CFLAGS := $(CSTD) $(WARN) $(CFLAGS)
PKG_CONFIG ?= pkg-config
LIBDRM_INC ?= /usr/include/libdrm
CPPFLAGS += -D_GNU_SOURCE -Isrc -Iclient -I$(BUILD) $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null || echo -I$(LIBDRM_INC)) -MMD -MP

# The one thing a build needs from outside the libc is libdrm's headers: say so plainly, with the package names, instead of "drm.h: No such file".
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(shell $(PKG_CONFIG) --exists libdrm 2>/dev/null && echo yes),)
ifeq ($(wildcard $(LIBDRM_INC)/drm.h),)
$(error The libdrm development headers are missing (drm.h was not found). Install them and run make again -- Debian / Ubuntu: sudo apt install build-essential pkg-config libdrm-dev   Fedora: sudo dnf install gcc make pkgconf-pkg-config libdrm-devel   Arch: sudo pacman -S base-devel libdrm   openSUSE: sudo zypper install gcc make pkg-config libdrm-devel)
endif
endif
endif
LDLIBS  := -ldrm -lm -ldl

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

BUILD := build

# ---- sources -----------------------------------------------------------------
# (mixer.c and smarthome.c belong to the gadget programs, mellagui-gadget: the compositor has no use for them)
COMP_SRCS := $(filter-out src/mixer.c src/smarthome.c,$(sort $(wildcard src/*.c)))
COMP_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(COMP_SRCS))
ASAN_OBJS := $(patsubst src/%.c,$(BUILD)/asan/%.o,$(COMP_SRCS))

# Modules shared with the clients (no dependency on server code).
CLIENT_COMMON_SRCS := src/wire.c src/protocol.c src/log.c src/loop.c src/font.c src/ttf.c src/keymap.c src/conf.c src/theme.c src/metrics.c src/clockfmt.c
CLIENT_COMMON_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(CLIENT_COMMON_SRCS))

TARGETS := $(BUILD)/mellagui $(BUILD)/mellagui-term $(BUILD)/mellagui-calc $(BUILD)/keymap-test $(BUILD)/logind-test $(BUILD)/gl-test $(BUILD)/screenoff-test \
           $(BUILD)/mellagui-run $(BUILD)/mellagui-settings $(BUILD)/mellagui-monitor $(BUILD)/mellagui-view $(BUILD)/mellagui-notify $(BUILD)/mellagui-edit $(BUILD)/mellagui-files $(BUILD)/mellagui-matrix $(BUILD)/mellagui-cam $(BUILD)/mellagui-record $(BUILD)/mellagui-calendar $(BUILD)/mellagui-quick $(BUILD)/mellagui-timer $(BUILD)/mellagui-keys $(BUILD)/mellagui-top $(BUILD)/mellagui-du $(BUILD)/mellagui-status $(BUILD)/mellagui-notes $(BUILD)/mellagui-desktop $(BUILD)/mellagui-archive $(BUILD)/mellagui-shot $(BUILD)/mellagui-paint $(BUILD)/mellagui-solitaire $(BUILD)/mellagui-tetris $(BUILD)/mellagui-chess $(BUILD)/mellagui-blocks $(BUILD)/chess-test $(BUILD)/chessai-test $(BUILD)/blocks-test $(BUILD)/mellagui-bluetooth $(BUILD)/mellagui-smart $(BUILD)/mellagui-gadget $(BUILD)/fuzz-server $(BUILD)/hwprobe $(BUILD)/mellagui-greetd $(BUILD)/mellagui-greeter

.PHONY: all mellagui mellagui-term mellagui-calc mellagui-run mellagui-settings mellagui-monitor mellagui-view mellagui-notify mellagui-edit mellagui-files mellagui-matrix mellagui-cam mellagui-record mellagui-calendar mellagui-quick mellagui-timer mellagui-keys mellagui-top mellagui-du mellagui-status mellagui-notes mellagui-desktop mellagui-archive mellagui-shot mellagui-paint mellagui-solitaire mellagui-tetris mellagui-chess mellagui-blocks mellagui-bluetooth mellagui-smart mellagui-gadget asan test run clean fuzz fuzz-run hwprobe
all: $(TARGETS)

mellagui: $(BUILD)/mellagui
mellagui-calc: $(BUILD)/mellagui-calc
mellagui-run: $(BUILD)/mellagui-run
mellagui-settings: $(BUILD)/mellagui-settings
mellagui-monitor: $(BUILD)/mellagui-monitor
mellagui-view: $(BUILD)/mellagui-view
mellagui-term: $(BUILD)/mellagui-term
asan: $(BUILD)/mellagui-asan

# ---- link rules ----------------------------------------------------------------
$(BUILD)/mellagui: $(COMP_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-asan: $(ASAN_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-calc: $(BUILD)/client/mellagui-calc.o $(BUILD)/client/wlclip.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/keymap-test: $(BUILD)/tests/keymap_test.o $(BUILD)/src/keymap.o $(BUILD)/src/font.o $(BUILD)/src/ttf.o $(BUILD)/src/cvt.o $(BUILD)/src/edid.o $(BUILD)/src/ddc.o $(BUILD)/src/mixer.o $(BUILD)/src/sysinfo.o $(BUILD)/src/sha256.o $(BUILD)/src/audio.o $(BUILD)/src/loop.o $(BUILD)/src/log.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/screenoff-test: $(BUILD)/tests/screenoff_test.o $(BUILD)/src/screenoff.o $(BUILD)/src/log.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/chess-test: $(BUILD)/tests/chess_test.o $(BUILD)/client/chess.o $(BUILD)/client/chessbook.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/chessai-test: $(BUILD)/tests/chessai_test.o $(BUILD)/client/chessai.o $(BUILD)/client/chess.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/blocks-test: $(BUILD)/tests/blocks_test.o $(BUILD)/client/blocks.o $(BUILD)/client/blocksview.o $(BUILD)/client/blockspets.o $(BUILD)/client/blocksmobs.o $(BUILD)/client/matrixcat.o $(BUILD)/client/matrixturtle.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/gl-test: $(BUILD)/tests/gl_test.o $(BUILD)/src/gl.o $(BUILD)/src/log.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -ldl

hwprobe: $(BUILD)/hwprobe

$(BUILD)/mellagui-greetd: $(BUILD)/client/mellagui-greetd.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ -ldl

$(BUILD)/mellagui-greeter: $(BUILD)/client/mellagui-greeter.o $(BUILD)/client/wlwin.o $(BUILD)/src/wlc.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/hwprobe: tools/hwprobe.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

$(BUILD)/logind-test: $(BUILD)/tests/logind_test.o $(BUILD)/src/logind.o $(BUILD)/src/loop.o $(BUILD)/src/log.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-matrix: $(BUILD)/client/mellagui-matrix.o $(BUILD)/client/matrixcat.o $(BUILD)/client/matrixturtle.o $(BUILD)/client/meow.o $(BUILD)/src/smartsun.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-du: $(BUILD)/client/mellagui-du.o $(BUILD)/client/wlclip.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-top: $(BUILD)/client/mellagui-top.o $(BUILD)/src/audio.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/src/sysinfo.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-keys: $(BUILD)/client/mellagui-keys.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-bluetooth: $(BUILD)/client/mellagui-bluetooth.o $(BUILD)/client/btctl.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-smart: $(BUILD)/client/mellagui-smart.o $(BUILD)/client/tuya.o $(BUILD)/client/smartsched.o $(BUILD)/client/tuyalan.o $(BUILD)/client/smartfav.o $(BUILD)/src/smartsun.o $(BUILD)/client/json.o $(BUILD)/client/qr.o $(BUILD)/src/aes.o $(BUILD)/src/md5.o $(BUILD)/src/sha256.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-gadget: $(BUILD)/client/mellagui-gadget.o $(BUILD)/src/mixer.o $(BUILD)/src/sysinfo.o $(BUILD)/src/smarthome.o $(BUILD)/src/audio.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-shot: $(BUILD)/client/mellagui-shot.o $(BUILD)/src/png.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-solitaire: $(BUILD)/client/mellagui-solitaire.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-tetris: $(BUILD)/client/mellagui-tetris.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-blocks: $(BUILD)/client/mellagui-blocks.o $(BUILD)/client/blocks.o $(BUILD)/client/blocksview.o $(BUILD)/client/blockspets.o $(BUILD)/client/blocksmobs.o $(BUILD)/client/matrixcat.o $(BUILD)/client/matrixturtle.o $(BUILD)/src/smartsun.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-chess: $(BUILD)/client/mellagui-chess.o $(BUILD)/client/chess.o $(BUILD)/client/chessai.o $(BUILD)/client/chessbook.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-paint: $(BUILD)/client/mellagui-paint.o $(BUILD)/client/imgwrite.o $(BUILD)/client/gifframes.o $(BUILD)/client/fontread.o $(BUILD)/client/arcio.o $(BUILD)/client/archread.o $(BUILD)/client/decomp.o $(BUILD)/client/zstddec.o $(BUILD)/client/deflate.o $(BUILD)/src/jpegenc.o $(BUILD)/src/img.o $(BUILD)/src/svg.o $(BUILD)/src/webp.o $(BUILD)/src/jpeg.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-archive: $(BUILD)/client/mellagui-archive.o $(BUILD)/client/arcio.o $(BUILD)/client/archread.o $(BUILD)/client/decomp.o $(BUILD)/client/zstddec.o $(BUILD)/client/deflate.o $(BUILD)/src/img.o $(BUILD)/src/svg.o $(BUILD)/src/webp.o $(BUILD)/src/jpeg.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-notes: $(BUILD)/client/mellagui-notes.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-desktop: $(BUILD)/client/mellagui-desktop.o $(BUILD)/src/dockicon.o $(BUILD)/src/iconfind.o $(BUILD)/src/img.o $(BUILD)/src/svg.o $(BUILD)/src/webp.o $(BUILD)/src/jpeg.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-status: $(BUILD)/client/mellagui-status.o $(BUILD)/client/btctl.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-timer: $(BUILD)/client/mellagui-timer.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-quick: $(BUILD)/client/mellagui-quick.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-calendar: $(BUILD)/client/mellagui-calendar.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-run: $(BUILD)/client/mellagui-run.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-settings: $(BUILD)/client/mellagui-settings.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/client/settingshelp.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-files: $(BUILD)/client/mellagui-files.o $(BUILD)/client/json.o $(BUILD)/client/yamltree.o $(BUILD)/client/xmltree.o $(BUILD)/client/tomltree.o $(BUILD)/client/initree.o $(BUILD)/client/sqliteread.o $(BUILD)/client/parquet.o $(BUILD)/client/zstddec.o $(BUILD)/client/xlsxread.o $(BUILD)/client/archread.o $(BUILD)/client/decomp.o $(BUILD)/client/certread.o $(BUILD)/client/fontread.o $(BUILD)/client/elfread.o $(BUILD)/client/dataread.o $(BUILD)/client/pdfread.o $(BUILD)/src/sha256.o $(BUILD)/client/syntax.o $(BUILD)/client/mdview.o $(BUILD)/client/ffdec.o $(BUILD)/client/ffaudio.o $(BUILD)/client/ffpipe.o $(BUILD)/src/img.o $(BUILD)/src/svg.o $(BUILD)/src/webp.o $(BUILD)/src/exif.o $(BUILD)/src/jpeg.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/client/smb.o $(BUILD)/client/sftp.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-edit: $(BUILD)/client/mellagui-edit.o $(BUILD)/client/syntax.o $(BUILD)/client/mdview.o $(BUILD)/src/img.o $(BUILD)/src/svg.o $(BUILD)/src/webp.o $(BUILD)/src/jpeg.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-notify: $(BUILD)/client/mellagui-notify.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-view: $(BUILD)/client/mellagui-view.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/src/exif.o $(BUILD)/src/h264dec.o $(BUILD)/src/h264.o $(BUILD)/client/ffdec.o $(BUILD)/client/ffaudio.o $(BUILD)/client/ffpipe.o $(BUILD)/src/img.o $(BUILD)/src/svg.o $(BUILD)/src/webp.o $(BUILD)/src/jpeg.o $(BUILD)/src/mp4read.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(BUILD)/src/png.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-cam: $(BUILD)/client/mellagui-cam.o $(BUILD)/client/camfx.o $(BUILD)/client/facedet.o $(BUILD)/client/camsound.o $(BUILD)/client/audiocap.o $(BUILD)/src/jpegenc.o $(BUILD)/src/mp4.o $(BUILD)/src/img.o $(BUILD)/src/svg.o $(BUILD)/src/webp.o $(BUILD)/src/jpeg.o $(BUILD)/src/png.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(BUILD)/client/ffpipe.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-record: $(BUILD)/client/mellagui-record.o $(BUILD)/client/audiocap.o $(BUILD)/client/ffpipe.o $(BUILD)/src/jpegenc.o $(BUILD)/src/h264.o $(BUILD)/src/h264enc.o $(BUILD)/src/h264dec.o $(BUILD)/src/mp4.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-monitor: $(BUILD)/client/mellagui-monitor.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(BUILD)/src/ddc.o $(BUILD)/src/edid.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-term: $(BUILD)/client/mellagui-term.o $(BUILD)/src/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# ---- version (the About dialog) -------------------------------------------------------
# build/version.h is made again on every make, but replaced only when its text changed, so only about.c is compiled again after a commit or a pull.
VERSION_H := $(BUILD)/version.h
$(VERSION_H): FORCE
	@mkdir -p $(BUILD)
	@tools/gen_version.sh > $@.tmp && { cmp -s $@.tmp $@ || mv $@.tmp $@; }; rm -f $@.tmp
.PHONY: FORCE
FORCE:
$(BUILD)/src/about.o $(BUILD)/asan/about.o $(BUILD)/fuzz/obj/about.o: $(VERSION_H)

# ---- compile rules ---------------------------------------------------------------
$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/asan/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ASAN_FLAGS) -c -o $@ $<

$(BUILD)/client/%.o: client/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# ---- fuzzer -----------------------------------------------------------------------
# Everything in src/ except main.c, with the sanitizers and edge coverage (gcc: -fsanitize-coverage=trace-pc), plus the harness.
FUZZ_DIR := $(BUILD)/fuzz
FUZZ_OBJS := $(patsubst src/%.c,$(FUZZ_DIR)/obj/%.o,$(filter-out src/main.c,$(COMP_SRCS)))
FUZZ_FLAGS := $(ASAN_FLAGS) -fsanitize-coverage=trace-pc -O1
FUZZ_TIME ?= 60

fuzz: $(BUILD)/fuzz-server

$(BUILD)/fuzz-server: $(FUZZ_OBJS) $(FUZZ_DIR)/obj/fuzz_server.o $(FUZZ_DIR)/obj/fuzz_cov.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ $^ $(LDLIBS)

$(FUZZ_DIR)/obj/fuzz_server.o: tests/fuzz_server.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) $(FUZZ_FLAGS) -c -o $@ $<

$(FUZZ_DIR)/obj/fuzz_cov.o: tests/fuzz_cov.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) -c -o $@ $<

$(FUZZ_DIR)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUZZ_FLAGS) -c -o $@ $<

fuzz-run: fuzz
	@mkdir -p $(FUZZ_DIR)/corpus
	$(BUILD)/fuzz-server -t $(FUZZ_TIME) -c $(FUZZ_DIR)/corpus -o $(FUZZ_DIR)/current.bin

# ---- tests ------------------------------------------------------------------------
test: all
	tests/run.sh

# make run                      real console (VT) session with the terminal; Ctrl+Alt+F3 first, see README
# make run EXEC=build/mellagui-calc  start another program instead of the terminal
# make run TIMEOUT=60            quit automatically after 60 s (safe first run)
# make run HEADLESS=1            headless backend instead (no display; control socket /tmp/mellagui.ctl)
EXEC ?= $(BUILD)/mellagui-term
RUN_LOG ?= $(HOME)/mellagui.log
RUN_ARGS = --exec '$(EXEC)' --terminal $(BUILD)/mellagui-term --run $(BUILD)/mellagui-run --log-file $(RUN_LOG) \
           $(if $(TIMEOUT),--timeout $(TIMEOUT))
run: all
	$(if $(HEADLESS),$(BUILD)/mellagui --backend headless --size 1280x720 --control /tmp/mellagui.ctl $(RUN_ARGS),$(BUILD)/mellagui $(RUN_ARGS))

clean:
	rm -rf $(BUILD)

# ---- install ------------------------------------------------------------------------
# make install                  build as you, then copy as root (sudo is asked for); `sudo make install` after a plain `make` works too
# make install PREFIX=/usr      other place (default /usr/local); DESTDIR=/some/dir stages it for a package (no root, no system changes)
# make enable-greeter           the login screen on tty1 from the next boot (it is NOT done by install: a wrong PAM file can lock a login out,
#                               read docs/GREETER.md and docs/HARDWARE.md "Login screen" first); make disable-greeter undoes it
# make uninstall
# What goes where: the programs (every mellagui-* program, also mellagui-greetd and mellagui-greeter; not the tests) in BINDIR;
# the unit mellagui-greetd.service in UNITDIR; /etc/pam.d/mellagui and mellagui-greeter (an existing file is kept, FORCE_PAM=1 replaces it;
# PAM_STYLE=debian|redhat picks the variant, the default looks at /etc/pam.d); the greeter user (sysusers.d + tmpfiles.d, made at once);
# the session entry wayland-sessions/mellagui.desktop; the docs in DOCDIR. Debug symbols are kept (the stall meter's addr2line needs them).
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share
DOCDIR     ?= $(DATADIR)/doc/mellagui
SYSCONFDIR ?= /etc
UNITDIR    ?= $(SYSCONFDIR)/systemd/system
PAMDIR     ?= $(SYSCONFDIR)/pam.d
PAM_STYLE  ?= auto
INSTALL    ?= install

TEST_TARGETS  := keymap-test logind-test gl-test screenoff-test chess-test chessai-test blocks-test fuzz-server hwprobe
INSTALL_PROGS := $(filter-out $(addprefix $(BUILD)/,$(TEST_TARGETS)),$(TARGETS))
INSTALL_DOCS  := README.md docs/ARCHITECTURE.md docs/GADGETS.md docs/GREETER.md docs/HARDWARE.md docs/SERVER.md docs/FUZZING.md
# the variables sudo would not hand on
INSTALL_VARS  := PREFIX='$(PREFIX)' BINDIR='$(BINDIR)' DATADIR='$(DATADIR)' DOCDIR='$(DOCDIR)' SYSCONFDIR='$(SYSCONFDIR)' UNITDIR='$(UNITDIR)' \
                 PAMDIR='$(PAMDIR)' PAM_STYLE='$(PAM_STYLE)' FORCE_PAM='$(FORCE_PAM)'

.PHONY: install install-files uninstall enable-greeter disable-greeter

install:
ifeq ($(shell id -u),0)
	@$(MAKE) --no-print-directory install-files
else ifneq ($(DESTDIR),)
	@$(MAKE) --no-print-directory $(INSTALL_PROGS)
	@$(MAKE) --no-print-directory install-files
else
	@$(MAKE) --no-print-directory $(INSTALL_PROGS)
	@echo "Copying the files needs root: sudo asks for the password."
	sudo $(MAKE) --no-print-directory install-files $(INSTALL_VARS)
endif

install-files:
	@for f in $(INSTALL_PROGS); do test -x $$f || { echo "$$f is missing: run 'make' as your own user first, then 'sudo make install'"; exit 1; }; done
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(UNITDIR) $(DESTDIR)$(PAMDIR) $(DESTDIR)$(DATADIR)/wayland-sessions $(DESTDIR)$(DOCDIR) \
	    $(DESTDIR)$(SYSCONFDIR)/sysusers.d $(DESTDIR)$(SYSCONFDIR)/tmpfiles.d
	$(INSTALL) -m 755 $(INSTALL_PROGS) $(DESTDIR)$(BINDIR)/
	sed 's|@BINDIR@|$(BINDIR)|g' contrib/mellagui-greetd.service > $(DESTDIR)$(UNITDIR)/mellagui-greetd.service
	chmod 644 $(DESTDIR)$(UNITDIR)/mellagui-greetd.service
	$(INSTALL) -m 644 contrib/mellagui-greeter.sysusers $(DESTDIR)$(SYSCONFDIR)/sysusers.d/mellagui-greeter.conf
	$(INSTALL) -m 644 contrib/mellagui-greeter.tmpfiles $(DESTDIR)$(SYSCONFDIR)/tmpfiles.d/mellagui-greeter.conf
	sed 's|^Exec=.*|Exec=$(BINDIR)/mellagui|' contrib/mellagui.desktop > $(DESTDIR)$(DATADIR)/wayland-sessions/mellagui.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/wayland-sessions/mellagui.desktop
	$(INSTALL) -m 644 $(INSTALL_DOCS) $(DESTDIR)$(DOCDIR)/
	@style="$(PAM_STYLE)"; \
	if [ "$$style" = auto ]; then \
	    if [ -e /etc/pam.d/common-auth ] || [ ! -e /etc/pam.d/system-auth ]; then style=debian; else style=redhat; fi; fi; \
	src=contrib/pam.d/mellagui; if [ "$$style" = redhat ]; then src=contrib/pam.d/mellagui.system-auth; fi; \
	for pair in mellagui:$$src mellagui-greeter:contrib/pam.d/mellagui-greeter; do \
	    name=$${pair%%:*}; file=$${pair#*:}; dest=$(DESTDIR)$(PAMDIR)/$$name; \
	    if [ -e $$dest ] && [ -z "$(FORCE_PAM)" ]; then echo "kept the existing $$dest (FORCE_PAM=1 replaces it)"; \
	    else $(INSTALL) -m 644 $$file $$dest && echo "installed $$dest ($$style)"; fi; \
	done
ifeq ($(DESTDIR),)
	@if command -v systemd-sysusers >/dev/null 2>&1; then systemd-sysusers $(SYSCONFDIR)/sysusers.d/mellagui-greeter.conf; \
	    else echo "systemd-sysusers is missing: make the user by hand (useradd --system mellagui-greeter)"; fi
	@if command -v systemd-tmpfiles >/dev/null 2>&1; then systemd-tmpfiles --create $(SYSCONFDIR)/tmpfiles.d/mellagui-greeter.conf; fi
	@if command -v systemctl >/dev/null 2>&1; then systemctl daemon-reload; fi
	@echo
	@echo "Installed in $(BINDIR). The login screen is in place but NOT switched on: it takes tty1 over from getty / gdm / lightdm / sddm."
	@echo "Read docs/GREETER.md, keep a root shell or ssh open, then: sudo make enable-greeter   (works from the next boot)"
	@echo "Or try it at once: sudo systemctl start mellagui-greetd"
endif

enable-greeter:
	@if [ "$$(id -u)" != 0 ]; then echo "run as root: sudo make enable-greeter"; exit 1; fi
	@test -e $(UNITDIR)/mellagui-greetd.service || { echo "run 'make install' first"; exit 1; }
	-systemctl disable getty@tty1.service gdm.service gdm3.service lightdm.service sddm.service 2>/dev/null
	systemctl enable mellagui-greetd.service
	@echo "The login screen replaces the text login on tty1 from the next boot (undo: sudo make disable-greeter)."

disable-greeter:
	@if [ "$$(id -u)" != 0 ]; then echo "run as root: sudo make disable-greeter"; exit 1; fi
	-systemctl disable mellagui-greetd.service
	systemctl enable getty@tty1.service
	@echo "tty1 has the text login again from the next boot."

# Leaves the PAM files (they are configuration), the user mellagui-greeter and its home folder; stop the service first if it runs.
uninstall:
	@if [ -z "$(DESTDIR)" ] && [ "$$(id -u)" != 0 ]; then echo "run as root: sudo make uninstall"; exit 1; fi
	@if [ -z "$(DESTDIR)" ] && systemctl is-enabled mellagui-greetd.service >/dev/null 2>&1; then \
	    echo "mellagui-greetd is enabled: run 'sudo make disable-greeter' first, or tty1 is left without a login"; exit 1; fi
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,$(notdir $(INSTALL_PROGS)))
	rm -f $(DESTDIR)$(UNITDIR)/mellagui-greetd.service $(DESTDIR)$(SYSCONFDIR)/sysusers.d/mellagui-greeter.conf \
	    $(DESTDIR)$(SYSCONFDIR)/tmpfiles.d/mellagui-greeter.conf $(DESTDIR)$(DATADIR)/wayland-sessions/mellagui.desktop
	rm -rf $(DESTDIR)$(DOCDIR)
ifeq ($(DESTDIR),)
	@if command -v systemctl >/dev/null 2>&1; then systemctl daemon-reload; fi
	@echo "Left in place: $(PAMDIR)/mellagui, $(PAMDIR)/mellagui-greeter, the user mellagui-greeter."
endif

# automatic header dependencies (-MMD -MP)
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
