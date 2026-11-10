# MellaGUI - a Wayland compositor + shell for Linux without X11.
#
#   make            build everything that has sources
#   make mellagui      build/mellagui        (compositor, src/*.c)
#   make mellagui-term build/mellagui-term   (client/mellagui-term.c)
#   make mellagui-calc build/mellagui-calc   (client/mellagui-calc.c)
#   make mellagui-matrix build/mellagui-matrix (client/mellagui-matrix.c, the screensaver)
#   make mellagui-top  build/mellagui-top  (client/mellagui-top.c, the process and service manager)
#   make mellagui-cam  build/mellagui-cam    (client/mellagui-cam.c, the webcam viewer, V4L2)
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
#   make clean

CC      := gcc
CSTD    := -std=gnu11
WARN    := -Wall -Wextra
OPT     := -O2 -g
CFLAGS  ?= $(OPT)
override CFLAGS := $(CSTD) $(WARN) $(CFLAGS)
CPPFLAGS += -D_GNU_SOURCE -Isrc -Iclient $(shell pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm) -MMD -MP
LDLIBS  := -ldrm -lm -ldl

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

BUILD := build

# ---- sources -----------------------------------------------------------------
COMP_SRCS := $(sort $(wildcard src/*.c))
COMP_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(COMP_SRCS))
ASAN_OBJS := $(patsubst src/%.c,$(BUILD)/asan/%.o,$(COMP_SRCS))

# Modules shared with the clients (no dependency on server code).
CLIENT_COMMON_SRCS := src/wire.c src/protocol.c src/log.c src/loop.c src/font.c src/keymap.c src/conf.c src/theme.c src/metrics.c src/clockfmt.c
CLIENT_COMMON_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(CLIENT_COMMON_SRCS))

TARGETS := $(BUILD)/mellagui $(BUILD)/mellagui-term $(BUILD)/mellagui-calc $(BUILD)/keymap-test $(BUILD)/logind-test $(BUILD)/gl-test \
           $(BUILD)/mellagui-run $(BUILD)/mellagui-settings $(BUILD)/mellagui-monitor $(BUILD)/mellagui-view $(BUILD)/mellagui-notify $(BUILD)/mellagui-edit $(BUILD)/mellagui-files $(BUILD)/mellagui-matrix $(BUILD)/mellagui-cam $(BUILD)/mellagui-record $(BUILD)/mellagui-calendar $(BUILD)/mellagui-keys $(BUILD)/mellagui-top

.PHONY: all mellagui mellagui-term mellagui-calc mellagui-run mellagui-settings mellagui-monitor mellagui-view mellagui-notify mellagui-edit mellagui-files mellagui-matrix mellagui-cam mellagui-record mellagui-calendar mellagui-keys mellagui-top asan test run clean fuzz fuzz-run
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

$(BUILD)/mellagui-calc: $(BUILD)/client/mellagui-calc.o $(BUILD)/client/wlclip.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/keymap-test: $(BUILD)/tests/keymap_test.o $(BUILD)/src/keymap.o $(BUILD)/src/font.o $(BUILD)/src/cvt.o $(BUILD)/src/edid.o $(BUILD)/src/ddc.o $(BUILD)/src/mixer.o $(BUILD)/src/sysinfo.o $(BUILD)/src/sha256.o $(BUILD)/src/audio.o $(BUILD)/src/loop.o $(BUILD)/src/log.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/gl-test: $(BUILD)/tests/gl_test.o $(BUILD)/src/gl.o $(BUILD)/src/log.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -ldl

$(BUILD)/logind-test: $(BUILD)/tests/logind_test.o $(BUILD)/src/logind.o $(BUILD)/src/loop.o $(BUILD)/src/log.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-matrix: $(BUILD)/client/mellagui-matrix.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-top: $(BUILD)/client/mellagui-top.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/src/sysinfo.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-keys: $(BUILD)/client/mellagui-keys.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-calendar: $(BUILD)/client/mellagui-calendar.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-run: $(BUILD)/client/mellagui-run.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-settings: $(BUILD)/client/mellagui-settings.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-files: $(BUILD)/client/mellagui-files.o $(BUILD)/src/img.o $(BUILD)/src/jpeg.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/client/smb.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-edit: $(BUILD)/client/mellagui-edit.o $(BUILD)/client/wlclip.o $(BUILD)/client/wltext.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-notify: $(BUILD)/client/mellagui-notify.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-view: $(BUILD)/client/mellagui-view.o $(BUILD)/src/h264dec.o $(BUILD)/src/h264.o $(BUILD)/client/ffdec.o $(BUILD)/client/ffaudio.o $(BUILD)/client/ffpipe.o $(BUILD)/src/img.o $(BUILD)/src/jpeg.o $(BUILD)/src/mp4read.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(BUILD)/src/png.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-cam: $(BUILD)/client/mellagui-cam.o $(BUILD)/src/img.o $(BUILD)/src/jpeg.o $(BUILD)/src/png.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-record: $(BUILD)/client/mellagui-record.o $(BUILD)/client/ffpipe.o $(BUILD)/src/jpegenc.o $(BUILD)/src/h264.o $(BUILD)/src/h264enc.o $(BUILD)/src/h264dec.o $(BUILD)/src/mp4.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -pthread

$(BUILD)/mellagui-monitor: $(BUILD)/client/mellagui-monitor.o $(BUILD)/client/wlc.o $(BUILD)/client/wlwin.o $(BUILD)/src/ddc.o $(BUILD)/src/edid.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/mellagui-term: $(BUILD)/client/mellagui-term.o $(BUILD)/client/wlc.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

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

# automatic header dependencies (-MMD -MP)
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
