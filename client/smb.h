#ifndef MELLAGUI_SMB_H
#define MELLAGUI_SMB_H

/*
 * Network drives (SMB / Windows shares, a router's USB disk, a NAS) without mounting anything: every operation runs `smbclient` (samba-client,
 * found in PATH or named by $MELLAGUI_SMBCLIENT) as an ordinary user, so it needs no root, no /etc/fstab line and no gvfs. Used by the file
 * manager (mellagui-files.c), which shows a share as a folder.
 *
 * A connection is a server (host name or address), who to be (empty user = guest) and, for this run only, the password. Passwords never go on a
 * command line: a user and a password are handed to smbclient in a temporary file ($XDG_RUNTIME_DIR, mode 0600, removed at once). Names that
 * smbclient's own command parser could misread (a quote, ';', a back slash, a line end) are refused for the operations that put them in a
 * command (and for opening such a folder); a listing shows every name.
 *
 * Every call runs the program with a time limit and an optional `poll` function that the caller can use to keep its window alive; the call is
 * given up when `poll` returns false. Owner: smb.c; the parts that need no program (parsing, the default gateway) are tested by `smb_selftest`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

struct smb_conn {
    char name[48];                      /* what the connection is called (the folder it shows in) */
    char host[128];
    char user[64];                      /* "" = guest */
    char pass[128];                     /* for this run only (never saved by smb.c) */
    bool legacy;                        /* allow the old SMB1 protocol (some old routers; not recommended) */
    bool sftp;                          /* an SSH server (sftp.c), not a Windows share: `host` may carry a port, `user` empty = ssh's own choice */
    int port;                           /* sftp: 0 = 22 */
};

struct smb_item {
    char name[256];
    bool dir;
    long long size;
    time_t mtime;
};

/* Called about every 50 ms while smbclient runs; false = give up (the child is killed). May be NULL. */
typedef bool (*smb_poll_fn)(void *data);

#define SMB_ERR 240

bool smb_available(void);
/* The pid of the smbclient this thread is running right now (0 when none): a `poll` function can look at its open files to see how far a transfer is. */
pid_t smb_child_pid(void);
/* The shares of the server that are disks (not IPC$ / printers / hidden $ ones). 0, or -1 with a short reason in err. Free *out with free(). */
int smb_shares(const struct smb_conn *c, char (**out)[64], int *n, char *err, size_t errsz, smb_poll_fn poll, void *pd);
/* The files and folders in `dir` of `share` ("" = its top; no "." / ".."). Free *out with free(). */
int smb_ls(const struct smb_conn *c, const char *share, const char *dir, struct smb_item **out, int *n, char *err, size_t errsz,
           smb_poll_fn poll, void *pd);
/* Copy a file from the share to `local` / from `local` to the share (`remote` is a path inside the share). The progress of a download is the size of `local`. */
int smb_get(const struct smb_conn *c, const char *share, const char *remote, const char *local, char *err, size_t errsz, smb_poll_fn poll, void *pd);
int smb_put(const struct smb_conn *c, const char *share, const char *local, const char *remote, char *err, size_t errsz, smb_poll_fn poll, void *pd);
int smb_del(const struct smb_conn *c, const char *share, const char *remote, bool dir, char *err, size_t errsz, smb_poll_fn poll, void *pd);
int smb_mkdir(const struct smb_conn *c, const char *share, const char *remote, char *err, size_t errsz, smb_poll_fn poll, void *pd);
int smb_rename(const struct smb_conn *c, const char *share, const char *from, const char *to, char *err, size_t errsz, smb_poll_fn poll, void *pd);
/* Runs `prog_name argv...` (argv[0] is filled in) with `input` on its standard input (may be NULL) and the "NAME=value" strings of `env` added to its
 * environment (may be NULL): stdout and stderr come back in one text in *out (malloc'd). Exit status, or -1 (could not run: err, `missing_text` when
 * the program is not there; cancelled / timed out). The process of the call is known to smb_child_pid() while it runs. Used by sftp.c, too. */
int smb_run_program(const char *prog_name, char **argv, const char *input, const char *const *env, int timeout_ms, char **out, char *err, size_t errsz,
                    smb_poll_fn poll_fn, void *pd, const char *missing_text);

/* True when the name may be used in an operation (no quote, ';', back slash, control character). */
bool smb_name_ok(const char *name);

/* The address of the default gateway (a router often shares a disk): "" when there is none. */
void smb_default_gateway(char *out, size_t n);

/* ---- the parts that need no program (also for the selftest) ---------------------------------------------------------------- */
/* One line of smbclient's `ls`: true and the item when it describes a file or folder (not "." / "..", not the summary line). */
bool smb_parse_ls_line(const char *line, struct smb_item *it);
/* One line of `smbclient -L`: true and the name when it is a disk share. */
bool smb_parse_share_line(const char *line, char *name, size_t n);
/* What went wrong, in words, from smbclient's output ("NT_STATUS_LOGON_FAILURE" -> "Wrong user name or password"). */
void smb_explain(const char *output, char *out, size_t n);
int smb_selftest(void);

#endif
