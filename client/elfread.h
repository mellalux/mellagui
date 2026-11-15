#ifndef MELLAGUI_ELFREAD_H
#define MELLAGUI_ELFREAD_H

/*
 * What an executable, a shared library or an object file says about itself, for the file manager's preview, with no library: ELF (32 / 64 bit, either byte
 * order: type, machine, ABI, entry point, the interpreter, the libraries it needs, SONAME, RPATH / RUNPATH, the build id, the note of the minimum kernel,
 * the compiler comment, the program and section headers, whether it is stripped and has debug information, and the hardening a checksec would list: PIE,
 * RELRO, BIND_NOW, a non-executable stack, stack protector and FORTIFY symbols) and PE (.exe / .dll: machine, subsystem, sections, imported DLLs). The result
 * is JSON text for the preview's tree. Every offset and size from the file is checked against the file's size.
 */

#include <stddef.h>

/* The facts as JSON text (malloc'd), or NULL when the file is no ELF / PE file. `info` gets one line ("ELF 64-bit x86-64 executable, dynamically linked"). */
char *exe_json(const char *path, char *info, size_t infon);

#endif
