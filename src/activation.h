#ifndef MELLAGUI_ACTIVATION_H
#define MELLAGUI_ACTIVATION_H

/*
 * xdg_activation_v1: a program asks for a token (`xdg_activation_token_v1`: set_serial, set_app_id, set_surface, commit -> done(token)) and hands it
 * to another program, which gives it back with `activate(token, surface)`; then the window of that surface comes forward (its workspace is
 * shown, it is un-minimized, raised and focused). This is how a browser started from a mail program, or a second copy of a program, asks the
 * existing window to come to the front.
 *
 * Focus must not be stolen, so a token only counts when the program that asked for it had a reason: it gave the serial of a real input event
 * (a click or a key that the seat handed to that program) or its own window has the keyboard focus right now. A token made with neither is
 * valid but harmless: `activate` with it does nothing. A token works once and ends 30 seconds after it was made. Owner: activation.c.
 */

struct lm_server;
struct lm_activation;

struct lm_activation *lm_activation_create(struct lm_server *srv);
void lm_activation_destroy(struct lm_activation *a);

/* Control socket `activation`: how many tokens wait, how many activations worked / were refused so far. */
int lm_activation_pending(const struct lm_activation *a);
int lm_activation_done(const struct lm_activation *a);
int lm_activation_refused(const struct lm_activation *a);

#endif
