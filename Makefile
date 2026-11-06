# Lumen - a Wayland compositor + shell for Linux without X11.
#
#   make            build everything that has sources
#   make lumen      build/lumen        (compositor, src/*.c)
#   make lumen-demo build/lumen-demo   (client/lumen-demo.c)
#   make lumen-term build/lumen-term   (client/lumen-term.c, only if it exists)
#   make lumen-calc build/lumen-calc   (client/lumen-calc.c, only if it exists)
#   make lumen-web  build/lumen-web    (client/lumen-web.c, the browser, only if it exists)
#   make lumen-run  build/lumen-run    (client/lumen-run.c, the Run dialog, only if it exists)
#   make lumen-settings build/lumen-settings (client/lumen-settings.c, the settings panel, only if it exists)
#                   (also builds build/js-test, the JavaScript interpreter test driver: tests/js/*.js)
#   make run        build, then start the compositor on a free VT (see RUN_ARGS)
#   make asan       build/lumen-asan   (compositor with ASan + UBSan)
#   make test       build, then run tests/run.sh
#   make clean

CC      := gcc
CSTD    := -std=gnu11
WARN    := -Wall -Wextra
OPT     := -O2 -g
CFLAGS  ?= $(OPT)
override CFLAGS := $(CSTD) $(WARN) $(CFLAGS)
CPPFLAGS += -D_GNU_SOURCE -Isrc -Iclient $(shell pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm) -MMD -MP
LDLIBS  := -ldrm -lm -lpthread -ldl

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

BUILD := build

# ---- sources -----------------------------------------------------------------
COMP_SRCS := $(sort $(wildcard src/*.c))
COMP_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(COMP_SRCS))
ASAN_OBJS := $(patsubst src/%.c,$(BUILD)/asan/%.o,$(COMP_SRCS))

# Modules shared with the clients (no dependency on server code).
CLIENT_COMMON_SRCS := $(wildcard src/wire.c src/protocol.c src/log.c src/loop.c src/font.c src/keymap.c)
CLIENT_COMMON_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(CLIENT_COMMON_SRCS))

TARGETS := $(BUILD)/lumen
ifneq ($(wildcard client/lumen-demo.c),)
TARGETS += $(BUILD)/lumen-demo
endif
ifneq ($(wildcard client/lumen-term.c),)
TARGETS += $(BUILD)/lumen-term
endif
ifneq ($(wildcard client/lumen-calc.c),)
TARGETS += $(BUILD)/lumen-calc
endif
ifneq ($(wildcard tests/js_test.c),)
TARGETS += $(BUILD)/js-test
endif
ifneq ($(wildcard tests/image_test.c),)
TARGETS += $(BUILD)/image-test
endif
ifneq ($(wildcard tests/keymap_test.c),)
TARGETS += $(BUILD)/keymap-test
endif
ifneq ($(wildcard client/lumen-web.c),)
TARGETS += $(BUILD)/lumen-web
endif
ifneq ($(wildcard client/lumen-run.c),)
TARGETS += $(BUILD)/lumen-run
endif
ifneq ($(wildcard client/lumen-settings.c),)
TARGETS += $(BUILD)/lumen-settings
endif

.PHONY: all lumen lumen-demo lumen-term lumen-calc lumen-web lumen-run lumen-settings asan test run clean
all: $(TARGETS)

lumen: $(BUILD)/lumen
lumen-demo: $(BUILD)/lumen-demo
lumen-calc: $(BUILD)/lumen-calc
lumen-run: $(BUILD)/lumen-run
lumen-settings: $(BUILD)/lumen-settings
lumen-web: $(BUILD)/lumen-web
lumen-term:
	@if [ -f client/lumen-term.c ]; then $(MAKE) --no-print-directory $(BUILD)/lumen-term; \
	else echo "client/lumen-term.c does not exist yet - skipping lumen-term"; fi
asan: $(BUILD)/lumen-asan

# ---- link rules ----------------------------------------------------------------
$(BUILD)/lumen: $(COMP_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/lumen-asan: $(ASAN_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/lumen-demo: $(BUILD)/client/lumen-demo.o $(BUILD)/client/wlc.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/lumen-calc: $(BUILD)/client/lumen-calc.o $(BUILD)/client/wlc.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/keymap-test: $(BUILD)/tests/keymap_test.o $(BUILD)/src/keymap.o $(BUILD)/src/font.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

JS_OBJS := $(patsubst %,$(BUILD)/client/%.o,js_core js_parse js_eval js_lib js_lib2 js_regex js_coro js_poly)
WEB_OBJS := $(BUILD)/client/tls.o $(BUILD)/client/image.o $(BUILD)/client/dom_css.o $(BUILD)/client/ttf.o $(BUILD)/client/dom.o $(BUILD)/client/dom_js.o $(JS_OBJS)
$(BUILD)/lumen-web: $(BUILD)/client/lumen-web.o $(BUILD)/client/wlc.o $(WEB_OBJS) $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/js-test: $(BUILD)/tests/js_test.o $(JS_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/image-test: $(BUILD)/tests/image_test.o $(BUILD)/client/image.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/lumen-run: $(BUILD)/client/lumen-run.o $(BUILD)/client/wlc.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/lumen-settings: $(BUILD)/client/lumen-settings.o $(BUILD)/client/wlc.o $(CLIENT_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/lumen-term: $(BUILD)/client/lumen-term.o $(BUILD)/client/wlc.o $(CLIENT_COMMON_OBJS)
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

# ---- tests ------------------------------------------------------------------------
test: all
	@if [ -x tests/run.sh ]; then tests/run.sh; \
	else echo "tests/run.sh not found or not executable"; exit 1; fi

# make run                      real console (VT) session with the terminal; Ctrl+Alt+F3 first, see README
# make run EXEC=build/lumen-calc  start another program instead of the terminal
# make run TIMEOUT=60            quit automatically after 60 s (safe first run)
# make run HEADLESS=1            headless backend instead (no display; control socket /tmp/lumen.ctl)
EXEC ?= $(BUILD)/lumen-term
RUN_LOG ?= $(HOME)/lumen.log
RUN_ARGS = --exec '$(EXEC)' --terminal $(BUILD)/lumen-term --run $(BUILD)/lumen-run --log-file $(RUN_LOG) \
           $(if $(TIMEOUT),--timeout $(TIMEOUT))
run: all
	$(if $(HEADLESS),$(BUILD)/lumen --backend headless --size 1280x720 --control /tmp/lumen.ctl $(RUN_ARGS),$(BUILD)/lumen $(RUN_ARGS))

clean:
	rm -rf $(BUILD)

# automatic header dependencies (-MMD -MP)
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
