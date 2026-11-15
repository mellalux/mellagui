#ifndef MELLAGUI_DATAREAD_H
#define MELLAGUI_DATAREAD_H

/*
 * Three small formats for the file manager's preview: Apple's binary property lists ("bplist00": every object type, as JSON for the tree), BitTorrent
 * metainfo files (bencode: name, size, files, trackers, the info hash made with an own SHA-1, a magnet link; as JSON) and Jupyter notebooks (the cells as
 * Markdown: text cells as they are, code in fenced blocks with the kernel's language, the text outputs in blocks under them). No library; every length from the
 * file is checked and the sizes, depths and counts are limited.
 */

#include <stddef.h>
#include <stdint.h>

/* The property list as JSON text (malloc'd), or NULL when it is no binary plist or is damaged. */
char *bplist_json(const uint8_t *d, size_t n, char *info, size_t infon);

/* The metainfo file as JSON text (malloc'd), or NULL when it is no torrent file. */
char *torrent_json(const uint8_t *d, size_t n, char *info, size_t infon);

/* SHA-1 of data (20 bytes into out). */
void data_sha1(const uint8_t *d, size_t n, uint8_t out[20]);

/* The notebook as Markdown (malloc'd, *len its length), or NULL when the text is no notebook. */
char *ipynb_markdown(const char *text, size_t n, size_t *len, char *info, size_t infon);

#endif
