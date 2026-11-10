#ifndef MELLAGUI_SFTP_H
#define MELLAGUI_SFTP_H

/*
 * SSH servers for the file manager, like smb.h for Windows shares: every operation runs OpenSSH's `sftp` in batch mode (`sftp -b -`, the commands on
 * its standard input) as the user, so nothing is mounted and no library is needed. A connection is the same `struct smb_conn`: `host`, `user`
 * (empty = what ~/.ssh/config or the current user says), `port` (0 = 22) and, for this run only, `pass`.
 *
 * Logging in: keys and the ssh agent work as they do in a terminal (BatchMode, nothing is ever asked on the program's terminal). A password (or a key's
 * passphrase) is handed over through ssh's own askpass mechanism: SSH_ASKPASS names this very program (`mellagui-files` answers when
 * $MELLAGUI_ASKPASS is set), SSH_ASKPASS_REQUIRE=force, and the secret travels in the environment of that one child - never on a command line, never in
 * a file. A host that is not known yet is added to known_hosts (StrictHostKeyChecking=accept-new); a host whose key has CHANGED is refused with a message.
 *
 * Paths are absolute on the server and written without quoting problems: a name with a quote, a back slash or a control character is refused for the
 * operations that put it in a command (`sftp_name_ok`). $MELLAGUI_SFTP names another program (the tests use a stand-in). Owner: sftp.c.
 */

#include <stdbool.h>
#include <stddef.h>

#include "smb.h"

bool sftp_available(void);                                      /* sftp (and ssh) are installed */

/* The server's home folder ("/home/me"). 0, or -1 with a reason in err (and *need_login set when logging in is what is missing). */
int sftp_pwd(const struct smb_conn *c, char *out, size_t n, char *err, size_t errsz, bool *need_login, smb_poll_fn poll, void *pd);
/* The files and folders of `dir` (absolute; "/" = the root). Free *out with free(). */
int sftp_ls(const struct smb_conn *c, const char *dir, struct smb_item **out, int *n, char *err, size_t errsz, bool *need_login, smb_poll_fn poll, void *pd);
/* `dir` = the thing is a folder (copied with everything in it). The progress is the size of `local` (a download) / the read position (an upload). */
int sftp_get(const struct smb_conn *c, const char *remote, const char *local, bool dir, char *err, size_t errsz, smb_poll_fn poll, void *pd);
int sftp_put(const struct smb_conn *c, const char *local, const char *remote, bool dir, char *err, size_t errsz, smb_poll_fn poll, void *pd);
/* A folder is deleted with what is in it (listed first, then one batch of rm / rmdir). */
int sftp_del(const struct smb_conn *c, const char *remote, bool dir, char *err, size_t errsz, smb_poll_fn poll, void *pd);
int sftp_mkdir(const struct smb_conn *c, const char *remote, char *err, size_t errsz, smb_poll_fn poll, void *pd);
int sftp_rename(const struct smb_conn *c, const char *from, const char *to, char *err, size_t errsz, smb_poll_fn poll, void *pd);

bool sftp_name_ok(const char *path);                            /* every part of the path may go into a command */

/* ---- no program needed (also for the selftest) ---- */
/* One line of `ls -la`: true and the item for a file / folder / link (not "." / ".."). Links count as folders (the listing cannot tell). */
bool sftp_parse_ls_line(const char *line, struct smb_item *it);
/* What ssh / sftp said, in words; *need_login when the server refused the login (not a file's permissions). */
void sftp_explain(const char *output, char *out, size_t n, bool *need_login);
/* The answer of the askpass call (set $MELLAGUI_ASKPASS): prints $MELLAGUI_SFTP_PASS. */
int sftp_askpass_main(void);
int sftp_selftest(void);

#endif
