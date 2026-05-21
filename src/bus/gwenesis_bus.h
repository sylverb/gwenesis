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
#ifndef _gwenesis_bus_H_
#define _gwenesis_bus_H_

#pragma once

#include <stdio.h>
#include <string.h>

/* 32 MB is the maximum supported size for SSF2 mapper */
#define MAX_ROM_SIZE 0x2000000
#define MAX_RAM_SIZE 0x10000
#define MAX_Z80_RAM_SIZE 8192

/* Cartridge SRAM support */
#define MAX_SRAM_SIZE  0x10000   /* 64 KB max (standard) */
/* Byte index mask for SRAM[] (odd-only uses packed index with same mask). */
#define GWENESIS_SRAM_MASK (MAX_SRAM_SIZE - 1)

// NTSC PAL timings
#define MCLOCK_PAL 53203424
#define MCLOCK_NTSC 53693175

#define MCYCLES_PER_FRAME_NTSC 896040
#define MCYCLES_PER_FRAME_PAL 1067040
#define LINES_PER_FRAME_NTSC 262
#define LINES_PER_FRAME_PAL 313

#define GWENESIS_REFRESH_RATE_NTSC 60
#define GWENESIS_AUDIO_FREQ_NTSC 53267

#define GWENESIS_REFRESH_RATE_PAL 50
#define GWENESIS_AUDIO_FREQ_PAL 52781

#define GWENESIS_AUDIO_ACCURATE 1

#define Z80_FREQ_DIVISOR 14     // Frequency divisor to Z80 clock
#define VDP_CYCLES_PER_LINE 3420// VDP Cycles per Line
#define SCREEN_WIDTH 320

#define AUDIO_FREQ_DIVISOR 1008
#define GWENESIS_AUDIO_BUFFER_LENGTH_NTSC 888
#define GWENESIS_AUDIO_BUFFER_LENGTH_PAL 1056

/* Internal buffer allocation must fit the max samples the PSG/YM can generate
 * in one frame.  PAL: ceil(313 * 3420 / 1008) = 1062.  Add a small margin. */
#define GWENESIS_AUDIO_BUFFER_CAPACITY ((LINES_PER_FRAME_PAL * VDP_CYCLES_PER_LINE / AUDIO_FREQ_DIVISOR) + 16)

/* 1 = printf each SSF2 bank write + OOB check vs ROM size (add -D to CFLAGS) */
#ifndef GWENESIS_DEBUG_SSF2_MAPPER
#define GWENESIS_DEBUG_SSF2_MAPPER 1
#endif

/* Audio buffer length */

enum gwenesis_bus_pad_button
{
    PAD_UP,
    PAD_DOWN,
    PAD_LEFT,
    PAD_RIGHT,
    PAD_B,
    PAD_C,
    PAD_A,
    PAD_S
};

#ifdef TARGET_GNW
void load_cartridge();
#else
void load_cartridge(unsigned char *buffer, size_t size);
#endif

void power_on();
void reset_emulation();

/* SRAM */
extern unsigned char *GWENESIS_SRAM;
extern int gwenesis_sram_enabled;          /* 1 = cartridge has SRAM (from ROM header) */
extern int gwenesis_sram_odd_only;         /* 1 = SRAM mapped on odd bytes only (e.g. Landstalker) */
extern int gwenesis_sram_active;           /* runtime: 1 = SRAM selected via reg 0xA130F1 */
extern int gwenesis_sram_write_protect;    /* runtime: 1 = SRAM write-protected via reg 0xA130F1 */
extern unsigned int gwenesis_sram_start;   /* first mapped address (always even) */
extern unsigned int gwenesis_sram_end;     /* last  mapped address */

/* SSF2 Mapper (Super Street Fighter II bankswitching)
 *
 * The 4 MB logical ROM space (0x000000-0x3FFFFF) is split into 8 slots of
 * 512 KB each.  Slot 0 (0x000000-0x07FFFF) is always wired to physical bank 0
 * and cannot be remapped.  Slots 1-7 are controlled by byte writes to the
 * odd addresses 0xA130F3, 0xA130F5, 0xA130F7, 0xA130F9, 0xA130FB, 0xA130FD,
 * 0xA130FF respectively.  The value written is the physical 512 KB page number
 * (0-15) to map into that slot.
 *
 * Reference: https://web.archive.org/web/20130731104452/http://emudocs.org/Genesis/ssf2.txt
 */
extern int           gwenesis_ssf2_enabled;  /* 1 = SSF2 mapper active */
extern unsigned char gwenesis_ssf2_banks[8]; /* logical slot → physical 512 KB page */
extern int           gwenesis_quackshot_map; /* 1 = QuackShot Rev A custom wiring */

/* Initialise the M68K memory_map table.
 * Must be called after load_cartridge() (SRAM/SSF2 flags are used)
 * and before reset_emulation() / m68k_pulse_reset(). */
void gwenesis_bus_init_memory_map(void);

/* Update ROM slots in memory_map after an SSF2 bank register write. */
void gwenesis_bus_ssf2_update_memory_map(void);

/* Update the SRAM overlay in memory_map (called when 0xA130F1 changes). */
void gwenesis_bus_sram_update_memory_map(void);

void gwenesis_bus_save_state(FILE *file);
void gwenesis_bus_load_state(FILE *file, int ss_version);

/* Region detected from the ROM header (updated by set_region and
 * gwenesis_apply_region_override).
 * 0 = USA (NTSC overseas), 1 = Europe (PAL), 2 = Japan (NTSC domestic). */
extern int gwenesis_detected_region;

/* Override the hardware region, bypassing ROM header detection.
 * region_code: 0=USA (NTSC overseas), 1=Europe (PAL), 2=Japan (NTSC domestic).
 * Call after load_cartridge() and before gwenesis_system_init() / power_on(). */
void gwenesis_apply_region_override(int region_code);

#endif
