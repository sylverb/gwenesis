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

*/

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "gwenesis_bus.h"
#include "gwenesis_sram.h"

static char gwenesis_sram_path[GWENESIS_SRAM_PATH_MAX];

static volatile int sram_dirty;

static uint32_t gwenesis_sram_byte_count(void)
{
    if (!gwenesis_sram_enabled)
        return 0;
    uint32_t addr_range = (uint32_t)(gwenesis_sram_end - gwenesis_sram_start + 1);
    uint32_t sz = gwenesis_sram_odd_only ? (addr_range / 2) : addr_range;
    if (sz > MAX_SRAM_SIZE)
        sz = MAX_SRAM_SIZE;
    return sz;
}

void gwenesis_set_sram_path(const char *path)
{
    if (!path || !path[0]) {
        gwenesis_sram_path[0] = '\0';
        return;
    }
    strncpy(gwenesis_sram_path, path, sizeof(gwenesis_sram_path) - 1);
    gwenesis_sram_path[sizeof(gwenesis_sram_path) - 1] = '\0';
}

void gwenesis_sram_mark_dirty(void)
{
    sram_dirty = 1;
}

bool gwenesis_sram_is_dirty(void)
{
    return (bool)sram_dirty;
}

void gwenesis_sram_clear_dirty(void)
{
    sram_dirty = 0;
}

bool gwenesis_sram_load(void)
{
    if (!gwenesis_sram_enabled)
        return false;

    uint32_t sz = gwenesis_sram_byte_count();
    if (sz == 0)
        return false;

    if (!gwenesis_sram_path[0]) {
        memset(GWENESIS_SRAM, 0x00, MAX_SRAM_SIZE);
        return false;
    }

    FILE *f = fopen(gwenesis_sram_path, "rb");
    if (!f) {
        printf("SRAM: open read failed, starting fresh\n");
        memset(GWENESIS_SRAM, 0x00, MAX_SRAM_SIZE);
        return false;
    }

    size_t n = fread(GWENESIS_SRAM, 1, sz, f);
    fclose(f);

    if (n < sz)
        memset(GWENESIS_SRAM + n, 0x00, sz - (uint32_t)n);
    if (sz < MAX_SRAM_SIZE)
        memset(GWENESIS_SRAM + sz, 0x00, MAX_SRAM_SIZE - sz);

    printf("SRAM: loaded %u bytes from %s\n", (unsigned)n, gwenesis_sram_path);
    gwenesis_sram_clear_dirty();
    return true;
}

bool gwenesis_sram_save(void)
{
    if (!gwenesis_sram_enabled)
        return false;
    if (!sram_dirty)
        return true;
    if (!gwenesis_sram_path[0]) {
        printf("SRAM: no path set, skip save\n");
        return false;
    }

    uint32_t sz = gwenesis_sram_byte_count();
    if (sz == 0)
        return false;

    FILE *f = fopen(gwenesis_sram_path, "wb");
    if (!f) {
        printf("SRAM: open write FAILED %s\n", gwenesis_sram_path);
        return false;
    }

    size_t n = fwrite(GWENESIS_SRAM, 1, sz, f);
    fclose(f);

    if (n != sz) {
        printf("SRAM: write incomplete (%u / %u)\n", (unsigned)n, (unsigned)sz);
        return false;
    }

    printf("SRAM: saved %u bytes to %s\n", (unsigned)sz, gwenesis_sram_path);
    gwenesis_sram_clear_dirty();
    return true;
}
