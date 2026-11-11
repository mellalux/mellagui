#ifndef MELLAGUI_BTCTL_H
#define MELLAGUI_BTCTL_H

/*
 * btctl - talking to `bluetoothctl` for mellagui-status (the panel button) and mellagui-bluetooth (the devices window). One-shot calls only:
 * `bluetoothctl show`, `devices`, `info MAC` ... with the colour codes of newer versions stripped. $MELLAGUI_BLUETOOTHCTL names another program.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_BT 24

struct btdev {
    char mac[18];
    char name[64];
    bool paired, connected;
    int battery;                        /* the device's own battery, 0..100, -1 = it does not say (bluez: "Battery Percentage: 0x56 (86)") */
};

struct btinfo {
    bool have;                          /* a controller answers */
    bool powered;
    struct btdev dev[MAX_BT];           /* connected first, then paired, then what a scan found */
    int n;
    int nconn;
    char first[64];                     /* the name of the first connected device */
    int first_battery;                  /* ... and its battery (-1 = unknown) */
};

const char *bluetoothctl_program(void);
/* Runs bluetoothctl with the NULL-terminated arguments; the output (colour codes and CRs removed) goes to out; returns the exit status, -1 when it
 * could not run, -2 on a timeout. */
int btctl(char *out, size_t n, int timeout_ms, ...);
bool mac_ok(const char *s);
/* "Device AA:BB:CC:DD:EE:FF Some name" lines -> devices (name = the rest of the line). */
int parse_bt_devices(const char *text, struct btdev *out, int max);
/* The value of "\tKey: value" in `bluetoothctl show` / `info` output. */
bool bt_field(const char *text, const char *key, char *out, size_t n);
bool bt_yes(const char *text, const char *key);
/* The controller and the known devices (connected first, then paired, then what a scan found, beacons without a name left out). */
void read_bt(struct btinfo *bi);
/* Is the sound server without its Bluetooth plugin (PipeWire's libspa-bluez5.so)? Then speakers and headphones pair but never connect:
 * bluez says "br-connection-profile-unavailable" / "Protocol not available" because nobody offers the audio profile. $MELLAGUI_SPA_DIR
 * (a folder with bluez5/libspa-bluez5.so) replaces the search of /usr/lib*; tests use it. */
bool bt_audio_plugin_missing(void);
/* bluetoothctl's failure text -> a sentence that says what to do (NULL when the text is not one we know): the text of a failed connect / pair, the
 * org.bluez.Error name or the reason after it. */
const char *bt_explain(const char *text);
/* The battery percentage in `bluetoothctl info` output (or a `[CHG] Device ... Battery Percentage: 0x56 (86)` line): -1 when there is none. */
int bt_battery(const char *text);
/* Removes the terminal colour codes and carriage returns from `s` in place. */
void bt_strip(char *s);

#endif
