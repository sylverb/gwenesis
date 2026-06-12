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

__author__ = "Sylver Bruneau"
__contact__ = "https://github.com/sylverb"
__license__ = "GPLv3"

SRAM persistence: raw battery-backed cartridge data (no header), one file per game.

Usage:
  - After load_cartridge() / power_on(), call gwenesis_set_sram_path() with the
    save file path (e.g. from odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ...)),
    then gwenesis_sram_load().
  - Call gwenesis_sram_save() when leaving or when appropriate; no-op if not dirty.
  - Load/save are no-ops when gwenesis_sram_enabled == 0 or path was not set.
*/

#ifndef _GWENESIS_SRAM_H_
#define _GWENESIS_SRAM_H_

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GWENESIS_SRAM_PATH_MAX 512

/*
 * Set the path for the raw .sram file (plain data, size = cartridge SRAM size).
 * Pass NULL or "" to disable load/save until set again.
 */
void gwenesis_set_sram_path(const char *path);

/*
 * Load SRAM from file into SRAM[]. Missing or short file: pads with 0 up to
 * expected size. Returns true if the file existed and was read (even if 0 bytes).
 */
bool gwenesis_sram_load(void);

/*
 * Write current SRAM[] to file (expected byte count only). Returns true on success.
 */
bool gwenesis_sram_save(void);

void gwenesis_sram_mark_dirty(void);
bool gwenesis_sram_is_dirty(void);
void gwenesis_sram_clear_dirty(void);

#endif /* _GWENESIS_SRAM_H_ */
