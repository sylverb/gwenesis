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
#define MAX_SRAM_SIZE  16*1024 // 0x10000   /* 64 KB max (standard) */
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

#define AUDIO_FREQ_DIVISOR 1009
#define GWENESIS_AUDIO_BUFFER_LENGTH_NTSC 888
#define GWENESIS_AUDIO_BUFFER_LENGTH_PAL 1056

/* Audio buffer length */

enum mapped_address
{
    NONE = 0,
    ROM_ADDR,
    ROM_ADDR_MIRROR,
    Z80_RAM_ADDR,
    Z80_RAM_ADDR1K,
    Z80_YM2612_ADDR,
    Z80_BANK_ADDR,
    Z80_VDP_ADDR,
    Z80_SN76489_ADDR,
    IO_CTRL,
    Z80_CTRL,
    TMSS_CTRL,
    VDP_ADDR,
    RAM_ADDR,
    SRAM_ADDR,
    SRAM_CTRL,
    SSF2_BANK_CTRL  /* Super Street Fighter II bankswitching registers 0xA130F3..0xA130FF */
};

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
void set_region();

/* SRAM */
extern unsigned char GWENESIS_SRAM[MAX_SRAM_SIZE];
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

void gwenesis_bus_save_state(FILE *file);
void gwenesis_bus_load_state(FILE *file);

#endif
