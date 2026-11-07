/*
 * logind.h - device access through systemd-logind, so that an ordinary user needs neither the `video` nor the `input`
 * group and VT switching is done by the session manager. No libsystemd / libdbus: a tiny D-Bus client of its own
 * (EXTERNAL authentication, UNIX fd passing, the few calls and signals below). Owner: logind.c.
 *
 *   system bus  --Hello-->  org.freedesktop.login1 Manager.GetSession(XDG_SESSION_ID) / GetSessionByPID
 *               Session.TakeControl(false)
 *               Session.TakeDevice(major, minor) -> fd (DRM: logind makes it master; evdev: revoked while paused)
 *               Session.ReleaseDevice(major, minor)
 *               signals PauseDevice(major, minor, "pause"|"force"|"gone") / ResumeDevice(major, minor, fd)
 *               Session.PauseDeviceComplete(major, minor)  (the ack logind waits for before it finishes a VT switch)
 *               Seat.SwitchTo(vt)
 *
 * Calls are synchronous (a few ms on the local bus, 3 s timeout); signals that arrive meanwhile are queued and delivered
 * from the event loop. The bus is DBUS_SYSTEM_BUS_ADDRESS ("unix:path=...") or /run/dbus/system_bus_socket.
 *
 * Users call lm_dev_open / lm_dev_close with a possibly NULL `struct lm_logind *`: without logind they are plain
 * open(2) / close(2), so backends have one code path.
 */
#ifndef MELLAGUI_LOGIND_H
#define MELLAGUI_LOGIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "loop.h"

struct lm_logind;

/* Connects, finds the session of this process and takes control of it. NULL with a short reason in err when logind is not
 * usable (no bus, not in a session, another controller ...). */
struct lm_logind *lm_logind_open(struct lm_loop *loop, char *err, size_t errsz);
/* Drops control (the session manager restores the console) and closes the connection. */
void lm_logind_close(struct lm_logind *l);

/* One device whose pause / resume the owner wants to handle itself (the DRM device): on PauseDevice "pause" the owner must
 * stop using it and then call lm_logind_pause_done(); on ResumeDevice `fd` is a fresh descriptor of the device that the
 * callee must close or keep. Every other device is acknowledged / closed by logind.c itself. */
typedef void (*lm_logind_pause_fn)(void *data, const char *type);
typedef void (*lm_logind_resume_fn)(void *data, int fd);
void lm_logind_watch(struct lm_logind *l, dev_t dev, lm_logind_pause_fn pause, lm_logind_resume_fn resume, void *data);
void lm_logind_pause_done(struct lm_logind *l, dev_t dev);

/* Switch the seat to virtual terminal `vt` (1..12). 0 or -1. */
int lm_logind_switch_vt(struct lm_logind *l, int vt);

/* The device file `path` as an open descriptor: logind's TakeDevice, or open(2) when l is NULL. Returns the fd or -errno;
 * *inactive (may be NULL) is set when the session is not active (the fd is then paused / useless). */
int lm_dev_open(struct lm_logind *l, const char *path, int flags, bool *inactive);
/* ReleaseDevice (when l) and close(fd). */
void lm_dev_close(struct lm_logind *l, int fd);

#endif
