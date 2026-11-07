/*
 * ddc.h - DDC/CI (VESA MCCS) over I2C: reading and writing a monitor's own settings (brightness, contrast, volume, input ...)
 * through the "VCP codes" it implements. Pure packet helpers (unit-tested in tests/keymap_test.c) plus a small blocking I/O
 * layer on /dev/i2c-N (slave address 0x37). No dependencies, linkable into clients.
 *
 * Needs the kernel module i2c-dev and read/write access to /dev/i2c-N (group "i2c", or a logind ACL). The bus of a connected
 * monitor is the target of /sys/class/drm/cardN-<connector>/ddc (see lumen-monitor.c).
 */
#ifndef LM_DDC_H
#define LM_DDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Common VCP codes. */
#define LM_VCP_BRIGHTNESS   0x10
#define LM_VCP_CONTRAST     0x12
#define LM_VCP_PRESET       0x14        /* colour temperature / preset: 1 sRGB, 2 native, 4 5000K, 5 6500K, 6 7500K, 8 9300K, 0x0b user 1 */
#define LM_VCP_INPUT        0x60        /* input source: 1 VGA, 3 DVI, 0x0f DP-1, 0x10 DP-2, 0x11 HDMI-1, 0x12 HDMI-2 */
#define LM_VCP_VOLUME       0x62
#define LM_VCP_MUTE         0x8d        /* 1 = muted, 2 = not muted */
#define LM_VCP_POWER        0xd6        /* 1 = on, 4 = standby */

struct lm_ddc_vcp {
    int code;
    int max, cur;
};

enum lm_ddc_result { LM_DDC_OK, LM_DDC_UNSUPPORTED, LM_DDC_BAD };

/* "Get VCP feature" request: 5 bytes into out. Returns the length. */
int lm_ddc_build_get(uint8_t code, uint8_t out[5]);
/* "Set VCP feature" request: 7 bytes into out. Returns the length. */
int lm_ddc_build_set(uint8_t code, int value, uint8_t out[7]);
/* Checks the 11-byte reply to a get request (source address, length, opcode, result, code, type, max, current, checksum). */
enum lm_ddc_result lm_ddc_parse_get(const uint8_t *reply, size_t len, uint8_t code, struct lm_ddc_vcp *out);

/*
 * Capabilities: the monitor's own list of the features it really has, e.g.
 *   "(prot(monitor)type(LCD)model(U4025QW)cmds(01 02 03)vcp(02 10 12 14(01 04 05) 60(0F 11 19) 62 D6(01 04 05))mccs_ver(2.1))"
 * Monitors answer a get-request for a feature they lack with "supported" and nonsense values (a Dell reports mute 0x8d with
 * maximum 65535), so the list is the thing to trust. A feature in it may name the values it accepts in parentheses.
 */
/* "Capabilities request" for the chunk starting at `offset`: 6 bytes into out. Returns the length. */
int lm_ddc_build_caps(int offset, uint8_t out[6]);
/* Checks one chunk reply (source, length, opcode 0xe3, offset, data, checksum); copies the data (not NUL terminated) into
 * `data` (max bytes) and returns its length (0 = the end), -1 for a damaged reply or a different offset. */
int lm_ddc_parse_caps(const uint8_t *reply, size_t len, int offset, char *data, int max);
/* Does the capabilities string list `code` among the vcp(...) features? */
bool lm_ddc_caps_has(const char *caps, uint8_t code);
/* The values listed for `code` (e.g. 60(0F 11) -> 0x0f, 0x11): count, at most `max` stored; 0 = none listed. */
int lm_ddc_caps_values(const char *caps, uint8_t code, int *values, int max);

/* I2C bus access. open: fd or -1 (errno says why, e.g. EACCES). */
int lm_ddc_open(int bus);
void lm_ddc_close(int fd);
/* Reads one feature (up to 3 tries with the pauses the monitors need). False = not supported or no answer. */
bool lm_ddc_get(int fd, uint8_t code, struct lm_ddc_vcp *out);
/* Reads the whole capabilities string (NUL terminated). False when the monitor does not give one. Takes a second or two. */
bool lm_ddc_capabilities(int fd, char *out, size_t n);
/* Writes one feature. False when the write fails (a monitor sends no acknowledgement of the value). */
bool lm_ddc_set(int fd, uint8_t code, int value);

#endif
