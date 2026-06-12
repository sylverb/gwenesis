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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "m68k.h"
#include "gwenesis_io.h"
#include "gwenesis_bus.h"
#include "gwenesis_vdp.h"
#include "z80inst.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"

#include "gwenesis_savestate.h"

#include <assert.h>

int gwenesis_savestate_version_from_header(const unsigned char header[GWENESIS_SAVESTATE_HEADER_SIZE])
{
  if (memcmp(header, GWENESIS_SAVESTATE_HEADER_PREFIX, GWENESIS_SAVESTATE_HEADER_PREFIX_LEN) == 0) {
    int i;
    int n = 0;
    for (i = 4; i < GWENESIS_SAVESTATE_HEADER_SIZE; i++) {
      if (header[i] < '0' || header[i] > '9')
        return 0;
      n = n * 10 + (header[i] - '0');
    }
    return n;
  }
  return 0;
}

void gwenesis_savestate_write_file_header(FILE *file)
{
  unsigned char h[GWENESIS_SAVESTATE_HEADER_SIZE];
  int v = GWENESIS_SAVESTATE_CURRENT_VERSION;
  memcpy(h, GWENESIS_SAVESTATE_HEADER_PREFIX, GWENESIS_SAVESTATE_HEADER_PREFIX_LEN);
  h[7] = (unsigned char)('0' + (v % 10));
  v /= 10;
  h[6] = (unsigned char)('0' + (v % 10));
  v /= 10;
  h[5] = (unsigned char)('0' + (v % 10));
  v /= 10;
  h[4] = (unsigned char)('0' + (v % 10));
  fwrite(h, 1, sizeof(h), file);
}

void gwenesis_save_state(FILE *file) {
  gwenesis_m68k_save_state(file);
  gwenesis_io_save_state(file);
  gwenesis_bus_save_state(file);
  gwenesis_vdp_gfx_save_state(file);
  gwenesis_vdp_mem_save_state(file);
  gwenesis_z80inst_save_state(file);
  gwenesis_ym2612_save_state(file);
  gwenesis_sn76489_save_state(file);
}

void gwenesis_load_state(FILE *file, int ss_version) {
  gwenesis_m68k_load_state(file, ss_version);
  gwenesis_io_load_state(file, ss_version);
  gwenesis_bus_load_state(file, ss_version);
  gwenesis_vdp_gfx_load_state(file, ss_version);
  gwenesis_vdp_mem_load_state(file, ss_version);
  gwenesis_z80inst_load_state(file, ss_version);
  gwenesis_ym2612_load_state(file, ss_version);
  gwenesis_sn76489_load_state(file, ss_version);
}
