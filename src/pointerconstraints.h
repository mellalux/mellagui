#ifndef MELLAGUI_POINTERCONSTRAINTS_H
#define MELLAGUI_POINTERCONSTRAINTS_H

/*
 * zwp_pointer_constraints_v1 and zwp_relative_pointer_manager_v1: what games, 3D programs, remote desktop windows and virtual machines need.
 *
 * A program asks to LOCK the pointer (it stays where it is; the program gets only relative motion) or to CONFINE it (it can move but not
 * out of a region of the surface), for a surface of its own. The constraint is ACTIVE while the surface has the pointer focus and the
 * program has the keyboard focus (Alt+Tab, a click on another window, a dialog of the shell, a workspace switch all end it by themselves
 * - the compositor's way out of a program that grabbed the pointer), the compositor is not moving or resizing a window, the screen is not
 * locked and, for a region, the pointer is inside it. Becoming active is told with `locked` / `confined`, ending with `unlocked` /
 * `unconfined`; a ONESHOT constraint is then spent, a PERSISTENT one is active again when the conditions come back. The region is double
 * buffered by wl_surface.commit (the one given at the start takes effect at once). When a lock ends the pointer goes to the program's
 * cursor position hint (set_cursor_position_hint) when it gave one.
 *
 * The seat reports every relative motion of a mouse (`lm_pconstraints_relative`, to the pointer focus program's relative_pointer objects,
 * constrained or not), asks whether the pointer is locked and where a confined one may go. Owner: pointerconstraints.c.
 */

#include <stdbool.h>
#include <stdint.h>

struct lm_server;
struct lm_pconstraints;

struct lm_pconstraints *lm_pconstraints_create(struct lm_server *srv);
void lm_pconstraints_destroy(struct lm_pconstraints *pc);

/* Something that decides activation changed (pointer focus, keyboard focus, the pointer's place, the lock): look at the constraints again. */
void lm_pconstraints_changed(struct lm_pconstraints *pc);
/* The pointer is locked right now: motion must not move it. */
bool lm_pconstraints_locked(const struct lm_pconstraints *pc);
/* Moves the target (tx, ty) (global) of a pointer going from (x, y) so that it stays in the confining region (no-op without an active confine). */
void lm_pconstraints_confine(const struct lm_pconstraints *pc, double x, double y, double *tx, double *ty);
/* A mouse moved by (dx, dy) logical pixels (ux, uy: the device's own units): tell the pointer focus program's relative pointers. */
void lm_pconstraints_relative(struct lm_pconstraints *pc, uint32_t time_ms, double dx, double dy, double ux, double uy);

/* Control socket `pconstraint`: "pconstraint constraints=N active=none|locked|confined relatives=N". */
int lm_pconstraints_count(const struct lm_pconstraints *pc);
const char *lm_pconstraints_active(const struct lm_pconstraints *pc);
int lm_pconstraints_relatives(const struct lm_pconstraints *pc);

#endif
