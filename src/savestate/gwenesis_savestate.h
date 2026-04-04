/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"

*/
#ifndef _gwenesis_savestate_H_
#define _gwenesis_savestate_H_

#pragma once

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/** File header size (bytes) written before `gwenesis_save_state` payload. */
#define GWENESIS_SAVESTATE_HEADER_SIZE 8

/** On-disk header: prefix `Gene` (4 bytes) + 4 decimal digits (0000–9999), 8 bytes total, no NUL. */
#define GWENESIS_SAVESTATE_HEADER_PREFIX "Gene"
#define GWENESIS_SAVESTATE_HEADER_PREFIX_LEN 4

/** Value encoded in the last 4 digits of the header (e.g. 1 → `Gene0001`). */
#define GWENESIS_SAVESTATE_CURRENT_VERSION 1

/** Parse layout version from the first `GWENESIS_SAVESTATE_HEADER_SIZE` bytes; 0 = unknown or `Gene0000`. */
int gwenesis_savestate_version_from_header(const unsigned char header[GWENESIS_SAVESTATE_HEADER_SIZE]);

/** Write the 8-byte header: `Gene` + zero-padded 4-digit `GWENESIS_SAVESTATE_CURRENT_VERSION`. */
void gwenesis_savestate_write_file_header(FILE *file);

void gwenesis_save_state(FILE *file);
/** @param ss_version from `gwenesis_savestate_version_from_header` (0 = rewind file to start before load). */
void gwenesis_load_state(FILE *file, int ss_version);
#endif
