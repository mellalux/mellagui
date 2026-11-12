#ifndef MELLAGUI_SMARTFAV_H
#define MELLAGUI_SMARTFAV_H

/*
 * The smart home devices the user pinned to the desktop card (the "Smart home" gadget of the compositor): one device id per line of
 * $XDG_CONFIG_HOME/mellagui/smart-favourites.conf (default ~/.config/mellagui/), '#' lines are comments. Shared by mellagui-smart
 * (the pin button of a device's view, --fav, --favourites).
 */

#include <stdbool.h>

#define FAV_MAX 8
#define FAV_ID_MAX 40

int fav_load(char ids[][FAV_ID_MAX], int max);                  /* how many were read */
bool fav_save(char ids[][FAV_ID_MAX], int n);
int fav_find(char ids[][FAV_ID_MAX], int n, const char *id);    /* index or -1 */
/* Pins (on = true) or unpins one device and saves; false when the file cannot be written or the list is full. */
bool fav_set(const char *id, bool on);

#endif
