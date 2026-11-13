# Fuzzing the compositor

Every Wayland client is untrusted input, and the request handlers (`server.c`, `surface.c`, `shm.c`, `xdg.c`, `seat.c`, `dmabuf.c`,
`settings.c` ...) are hand-written C. `tests/fuzz_server.c` hunts for memory errors, undefined behaviour and leaks of descriptors in
them. A plain `make` builds `build/fuzz-server` too (so `TestFuzz` in the suite runs for 20 s instead of being skipped).
The harness sets `MELLAGUI_NO_GADGET_PROCS=1`: the settings `gadget` / `mixer` / `sysinfo` would otherwise start `mellagui-gadget`, which connects
to the fuzz server as a client.

    make fuzz-run                         # 60 s (FUZZ_TIME=600 for longer); the corpus is kept in build/fuzz/corpus
    build/fuzz-server -t 300 -s 7 -c DIR  # seed 7, corpus DIR
    build/fuzz-server -r FILE             # replay one input (a crash file)

There is no clang / libFuzzer / AFL on the dev machine, so the fuzzer is built from what gcc has: the whole compositor (everything in
`src/` except `main.c`) is compiled into one binary with ASan + UBSan and `-fsanitize-coverage=trace-pc`; `tests/fuzz_cov.c` turns the
callback into an edge bitmap. A run keeps an input when it reaches new edges and mutates the kept ones.

**How an input becomes a conversation.** An input is only bytes, but they drive a generator that knows every interface, request and
argument type from `protocol.c`: it binds the globals, creates objects and tracks them, answers configure serials the compositor really
sent, and mixes in nasty values (huge or negative sizes, dead and foreign object ids, wrong versions, memfds of odd sizes that are
truncated while the server has them mapped, garbage frames, an id guessed in the server's range). *Macros* build the valid chains that
random requests never would (a window with a buffer on the screen, a popup with a grab, a subsurface, a clipboard selection). Between
requests it drives the compositor side too: keys, pointer and touch, the lock screen, settings keys (also `lock-set`) and a render flush.
When a protocol error disconnects the client it connects again and goes on.

**Reading a result.** The input being run is written to the crash file (`build/fuzz/current.bin`) *before* each run. When ASan, UBSan or
glibc aborts, that file is the reproducer: `build/fuzz-server -r build/fuzz/current.bin`. Fix the bug, then add a regression test to
`tests/test_mellagui.py` (the raw wire client there can send the same bytes) and keep the input if it is small.
`tests/test_mellagui.py` runs a 20 s fuzz as `TestFuzz` when `build/fuzz-server` exists.

**What it cannot reach.** Real dmabuf descriptors (a memfd is rejected by the sync ioctl), anything that needs the DRM backend, the real
evdev devices and programs the compositor spawns (`PATH` is emptied). The state between runs is reset only partly (windows and clients,
the lock), so a rare crash may need the last few inputs and not just the file.

**Validating the fuzzer.** Loosen a length check on purpose (e.g. the `lock-set` buffer in `settings.c`) and it stops within a minute with
"buffer overflow detected" and a reproducer; with the check back the same file runs clean.
