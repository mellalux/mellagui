#ifndef MELLAGUI_NETSTREAM_H
#define MELLAGUI_NETSTREAM_H

/*
 * Playing a file of a network drive without fetching it first. The file manager starts a copy of itself (`mellagui-files --stream`) that holds the one
 * connection to the server and gives the file out as http://127.0.0.1:PORT/TOKEN/f/NAME, with byte ranges: the viewer (and the ffmpeg it runs) reads
 * and seeks in it as in any file, and only what is asked for crosses the network. Nothing is written to a disk.
 *
 *  - SMB: libsmbclient (libsmbclient.so.0, part of the smbclient package), loaded when it is needed; $MELLAGUI_SMBLIB names another library.
 *  - SSH: OpenSSH's `ssh -s ... sftp` ($MELLAGUI_SSH names another program), spoken to in the SFTP protocol, version 3. Logging in as in sftp.h
 *    (keys, the agent, or the password through askpass).
 *
 * What to open goes to the helper on its standard input, so a password is never an argument. It answers one line, "ok URL" or "error WHY".
 * A request is served only to a connection of the same user (looked up in /proc/net/tcp) that knows the token. The helper ends when nothing has been
 * connected to it for a minute ($MELLAGUI_STREAM_IDLE_MS); whoever shows the file keeps a connection open meanwhile (netstream_url.h).
 */

#include <stdbool.h>
#include <stddef.h>

#include "smb.h"

/* Starts a helper for the file `path` of the connection: for a Windows share `share` and the path inside it, for an SSH server `share` is "" and
 * `path` the path below the root. true: `url` is ready. false: `err` says why (nothing is left running). Waits at most half a minute. */
bool netstream_start(const struct smb_conn *c, const char *share, const char *path, char *url, size_t urlsz, char *err, size_t errsz);
/* `mellagui-files --stream`. */
int netstream_serve_main(void);

#endif
