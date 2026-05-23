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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "m68k.h"

#include "ym2612.h"
#include "z80inst.h"
#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_savestate.h"
#include "gwenesis_sram.h"

#ifdef TARGET_GNW
#include "gw_malloc.h"
  #pragma GCC optimize("Ofast")
#endif

/* Defined in gwenesis_vdp_gfx.c — mirrors VDP REG1_PAL, set by set_region() */
extern int mode_pal;
extern unsigned short gwenesis_vdp_status;

/* Region last applied by set_region() or gwenesis_apply_region_override().
 * 0 = USA (NTSC overseas), 1 = Europe (PAL), 2 = Japan (NTSC domestic). */
int gwenesis_detected_region = 0;

#define BUS_DISABLE_LOGGING 1

#if !BUS_DISABLE_LOGGING
#include <stdarg.h>
void bus_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s] vc:%04x hc:%04x hv:%04x ", frame_counter, scan_line, subs,gwenesis_vdp_vcounter(),gwenesis_vdp_hcounter(),gwenesis_vdp_hvcounter());

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define bus_log(...)  do {} while(0)
#endif

// Setup M68k memories ROM & RAM
#ifdef TARGET_GNW

#include "rom_manager.h"
unsigned char *M68K_RAM; // M68K RAM (64KB)
#else

unsigned char ROM_DATA[MAX_ROM_SIZE]; // 68K Main Program (uncompressed)
unsigned char M68K_RAM[MAX_RAM_SIZE];    // 68K RAM
#endif


// Setup Z80 Memory
unsigned char ZRAM[MAX_Z80_RAM_SIZE]; // Z80 RAM
unsigned char TMSS[0x4];
extern unsigned short gwenesis_vdp_status;

/* Cartridge SRAM — static buffer in normal RAM (not ITCRAM which is at 0x0) */
unsigned char *GWENESIS_SRAM;
int           gwenesis_sram_enabled      = 0; // 1 if the cartridge has SRAM (from ROM header)
int           gwenesis_sram_odd_only     = 0; // 1 = data on odd bytes only (e.g. Landstalker)
int           gwenesis_sram_active       = 0; // runtime: register 0xA130F1 bit0 (1=SRAM, 0=ROM)
int           gwenesis_sram_write_protect= 0; // runtime: register 0xA130F1 bit1 (1=read-only)
unsigned int  gwenesis_sram_start        = 0; // SRAM start address, forced even (from ROM header)
unsigned int  gwenesis_sram_end          = 0; // SRAM end   address (from ROM header)

/* SSF2 Mapper
 * 8 slots of 512 KB cover the 4 MB logical ROM space (0x000000-0x3FFFFF).
 * Slot 0 is always physical bank 0 (fixed). Slots 1-7 are remappable via
 * byte writes to 0xA130F3, 0xA130F5, ..., 0xA130FF.
 */
int           gwenesis_ssf2_enabled = 0;
unsigned char gwenesis_ssf2_banks[8];   /* logical slot → physical 512 KB page */
int           gwenesis_quackshot_map = 0; /* QuackShot Rev A custom ROM wiring */

// TMSS
int tmss_state = 0;
int tmss_count = 0;

/* Read one logical byte from the ROM header at load time.
 * ROM_DATA is byte-swapped (ROM_SWAP), so logical byte i is at ROM_DATA[i ^ 1].
 * Only used during load_cartridge / set_region before memory_map is built. */
#define ROM_HEADER_BYTE(i) (ROM_DATA[(i) ^ 1u])

/* ROM metadata for SRAM autodetect (Genesis Plus GX core/cart_hw/sram.c). */
typedef struct {
  unsigned int rom_size;
  unsigned short checksum;
  unsigned short realchecksum;
  char product[16];
  char international[50];
  char region[3];
} gwenesis_rom_info_t;

static void set_region(gwenesis_rom_info_t *info);

static unsigned short gwenesis_rom_realchecksum(unsigned int rom_size)
{
  unsigned int sum = 0;
  unsigned int i;

  if (rom_size < 0x202u)
    return 0;

  for (i = 0x200; i + 1u < rom_size; i += 2)
    sum += ((unsigned int)ROM_HEADER_BYTE(i) << 8) | ROM_HEADER_BYTE(i + 1);

  return (unsigned short)sum;
}

static void gwenesis_rom_load_info(gwenesis_rom_info_t *info, unsigned int rom_size)
{
  int i, j;

  memset(info, 0, sizeof(*info));
  info->rom_size = rom_size;
  info->checksum = (unsigned short)(((unsigned int)ROM_HEADER_BYTE(0x18E) << 8) |
                                    ROM_HEADER_BYTE(0x18F));
  info->realchecksum = gwenesis_rom_realchecksum(rom_size);

  for (i = 0; i < 14; i++)
    info->product[i] = (char)ROM_HEADER_BYTE(0x180 + i);
  info->product[14] = '\0';

  info->international[0] = (char)ROM_HEADER_BYTE(0x150);
  j = 1;
  for (i = 1; i < 48 && (0x150 + i) < 0x180; i++) {
    char c = (char)ROM_HEADER_BYTE(0x150 + i);

    if (info->international[j - 1] != ' ' || c != ' ')
      info->international[j++] = c;
  }
  info->international[j] = '\0';
  info->region[0] = (char)ROM_HEADER_BYTE(0x1F0);
  info->region[1] = (char)ROM_HEADER_BYTE(0x1F1);
  info->region[2] = (char)ROM_HEADER_BYTE(0x1F2);

  printf("Name: %s\n", info->international);
  printf("Product: %s\n", info->product);
  printf("Rom size: %u\n", info->rom_size);
  printf("Region: %c%c%c\n", info->region[0], info->region[1], info->region[2]);
  printf("Checksum: %04X / Real: %04X\n", info->checksum, info->realchecksum);
}

static void gwenesis_sram_set_range(unsigned int start, unsigned int end, int odd_only)
{
  gwenesis_sram_start = start;
  gwenesis_sram_end = end;
  gwenesis_sram_odd_only = odd_only ? 1 : 0;
}

static unsigned int gwenesis_sram_effective_size(void)
{
  unsigned int addr_range = gwenesis_sram_end - gwenesis_sram_start + 1;
  unsigned int sram_size = gwenesis_sram_odd_only ? (addr_range / 2) : addr_range;

  if (sram_size > MAX_SRAM_SIZE)
    sram_size = MAX_SRAM_SIZE;
  return sram_size;
}

static void gwenesis_sram_enable_backup(int init_ff)
{
  gwenesis_sram_enabled = 1;
  gwenesis_sram_active = 1;
  if (init_ff)
    memset(GWENESIS_SRAM, 0xFF, MAX_SRAM_SIZE);
}

/* Wrong "RA" header fixes (SRAM only — flash saves not emulated). */
static void gwenesis_sram_fix_ra_header(gwenesis_rom_info_t *info)
{
  if (strstr(info->product, "T-26013") != NULL) {
    /* Psy-O-Blade */
    gwenesis_sram_set_range(0x200001, 0x203FFF, 1);
    return;
  }

  if (gwenesis_sram_start == 0xFF0000) {
    /* Feng Kuang Tao Hua Yuan — internal RAM, not cart SRAM */
    gwenesis_sram_enabled = 0;
    gwenesis_sram_active = 0;
    return;
  }

  if (gwenesis_sram_start >= 0x800000) {
    gwenesis_sram_set_range(0x200000, 0x20FFFF, 0);
    return;
  }

  if (gwenesis_sram_start > gwenesis_sram_end ||
      (gwenesis_sram_end - gwenesis_sram_start) >= 0x10000u) {
    gwenesis_sram_end = gwenesis_sram_start + 0xFFFF;
  }
}

/* Missing "RA" header: 1=SRAM on, 0=no match, -1=force disable. */
static int gwenesis_sram_autodetect_no_header(const gwenesis_rom_info_t *info)
{
  if (strstr(info->product, "T-50086") != NULL) {
    /* PGA Tour Golf */
    gwenesis_sram_set_range(0x200001, 0x203FFF, 1);
    return 1;
  }
  if (strstr(info->product, "ACLD007") != NULL) {
    /* Winter Challenge */
    gwenesis_sram_set_range(0x200001, 0x200FFF, 1);
    return 1;
  }
  if (strstr(info->product, "T-50286") != NULL) {
    /* Buck Rogers - Countdown to Doomsday */
    gwenesis_sram_set_range(0x200001, 0x203FFF, 1);
    return 1;
  }
  if (((info->realchecksum == 0xAEAA) || (info->realchecksum == 0x8DBA)) &&
      info->checksum == 0x8104) {
    /* Xin Qigai Wangzi */
    gwenesis_sram_set_range(0x400001, 0x40FFFF, 1);
    return 1;
  }
  if (info->checksum == 0x0000 && info->realchecksum == 0x1F7F &&
      info->rom_size > 0x80000u + 0x1B2u &&
      ROM_HEADER_BYTE(0x80000 + 0x1B0) == 0x52 &&
      ROM_HEADER_BYTE(0x80000 + 0x1B1) == 0x41) {
    /* Radica - Sensible Soccer Plus */
    gwenesis_sram_set_range(0x200001, 0x203FFF, 1);
    return 1;
  }
  if (strstr(info->international, "SONIC & KNUCKLES") != NULL) {
    if (info->rom_size == 0x400000u) {
      /* Sonic 3 & Knuckles combined — FRAM from S3 cart */
      gwenesis_sram_set_range(0x200001, 0x203FFF, 1);
      return 1;
    }
  }

  if (strstr(info->product, "T-113016") != NULL) {
    /* Pugsy — no SRAM (copy protection writes) */
    return -1;
  }
  if (strstr(info->international, "SONIC THE HEDGEHOG 2") != NULL) {
    /* S&K lock-on: do not map SRAM over mirrored ROM */
    return -1;
  }

  if (info->rom_size <= 0x200000u) {
    /* GPGX default for small ROMs without header */
    gwenesis_sram_set_range(0x200000, 0x20FFFF, 0);
    return 1;
  }

  return 0;
}

static void gwenesis_sram_detect_from_rom(gwenesis_rom_info_t *info)
{
  if (ROM_HEADER_BYTE(0x1B0) == 0x52 && ROM_HEADER_BYTE(0x1B1) == 0x41) {
    unsigned char sram_type = ROM_HEADER_BYTE(0x1B2);

    gwenesis_sram_odd_only = (sram_type & 0x08) ? 1 : 0;
    gwenesis_sram_start = ((unsigned int)ROM_HEADER_BYTE(0x1B4) << 24) |
                          ((unsigned int)ROM_HEADER_BYTE(0x1B5) << 16) |
                          ((unsigned int)ROM_HEADER_BYTE(0x1B6) <<  8) |
                           (unsigned int)ROM_HEADER_BYTE(0x1B7);
    gwenesis_sram_end   = ((unsigned int)ROM_HEADER_BYTE(0x1B8) << 24) |
                          ((unsigned int)ROM_HEADER_BYTE(0x1B9) << 16) |
                          ((unsigned int)ROM_HEADER_BYTE(0x1BA) <<  8) |
                           (unsigned int)ROM_HEADER_BYTE(0x1BB);
    gwenesis_sram_start &= ~1u;

    gwenesis_sram_enable_backup(0);
    gwenesis_sram_fix_ra_header(info);

    if (gwenesis_sram_enabled) {
      printf("SRAM detected: start=0x%06X end=0x%06X size=%u bytes mode=%s\n",
             gwenesis_sram_start, gwenesis_sram_end, gwenesis_sram_effective_size(),
             gwenesis_sram_odd_only ? "odd-only" : "full");
    } else {
      printf("SRAM disabled (header fix)\n");
    }
  } else if (ROM_HEADER_BYTE(0x1B0) == 0x46 && ROM_HEADER_BYTE(0x1B1) == 0x4C) {
  /* "FL" — SGDK flash save (not emulated) */
    printf("SRAM FL/flash header not emulated\n");
  } else {
    int autodetect = gwenesis_sram_autodetect_no_header(info);

    if (autodetect == 1) {
      gwenesis_sram_enable_backup(1);
      printf("SRAM autodetect (%s): start=0x%06X end=0x%06X size=%u bytes mode=%s\n",
             info->product, gwenesis_sram_start, gwenesis_sram_end,
             gwenesis_sram_effective_size(),
             gwenesis_sram_odd_only ? "odd-only" : "full");
    } else if (autodetect == -1) {
      printf("SRAM disabled (%s)\n", info->product[0] ? info->product : info->international);
    } else {
      printf("No SRAM detected in ROM header\n");
    }
  }
}

/******************************************************************************
 *
 *   Load a Sega Genesis Cartridge into CPU Memory
 *
 ******************************************************************************/


 #ifdef TARGET_GNW

void load_cartridge()
{
    gwenesis_rom_info_t info;

    // Clear all volatile memory
    M68K_RAM = itc_malloc(MAX_RAM_SIZE); // M68K RAM 
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // Set Z80 Memory as Z80_RAM
    z80_set_memory(ZRAM);

    z80_pulse_reset();

    gwenesis_rom_load_info(&info, ROM_DATA_LENGTH);
    set_region(&info);

    /* QuackShot Rev A (00004054-01, checksum A4B3) needs custom ROM wiring. */
    gwenesis_quackshot_map = 0;
    if (ROM_DATA_LENGTH == 0x80000) {
        if (strstr(info.product, "00004054-01") && info.checksum == 0xA4B3) {
            gwenesis_quackshot_map = 1;
            printf("QuackShot Rev A custom ROM mapping enabled\n");
        } else {
            printf("QuackShot map check: product='%s' checksum=%04X (no match)\n",
                   info.product, info.checksum);
        }
    }
    /* ------ SRAM detection from ROM header ------ */
    /*
     * Mega Drive ROM header (big-endian):
     *   0x1B0  : "RA" magic (0x52 0x41) -> SRAM present
     *   0x1B2  : type byte  (0xF8 = even+odd, 0xF9 = odd bytes only)
     *                        bit 3 of byte at 0x1B2 set -> byte-wide (odd only)
     *   0x1B4  : 4 bytes SRAM start address
     *   0x1B8  : 4 bytes SRAM end   address
     */
    gwenesis_sram_enabled  = 0;
    gwenesis_sram_odd_only = 0;
    gwenesis_sram_active   = 0;
    gwenesis_sram_write_protect = 0;
    GWENESIS_SRAM = ahb_malloc(MAX_SRAM_SIZE);
    gwenesis_sram_detect_from_rom(&info);

    /* ------ SSF2 mapper detection ------ */
    /*
     * Any ROM larger than 4 MB requires the Super Street Fighter II
     * bankswitching mapper.  Initialise all slots to the identity mapping so
     * that normal (non-banked) ROM access works unchanged until the game writes
     * to the bank registers at 0xA130F3..0xA130FF.
     * Slot 0 (0x000000-0x07FFFF) is always fixed to physical bank 0.
     */
     gwenesis_ssf2_enabled = 0;
     for (int i = 0; i < 8; i++) gwenesis_ssf2_banks[i] = (unsigned char)i;

     if (ROM_DATA_LENGTH > 0x400000) {
         gwenesis_ssf2_enabled = 1;
         printf("SSF2 mapper enabled (ROM size: %d KB)\n",
               ROM_DATA_LENGTH / 1024);
     }
 }
#else

void load_cartridge(unsigned char *buffer, size_t size)
{
    gwenesis_rom_info_t info;

    // Clear all volatile memory
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);
    memset(ROM_DATA, 0, MAX_ROM_SIZE);

    // Set Z80 Memory as ZRAM
    z80_set_memory(ZRAM);
    z80_pulse_reset();

    gwenesis_rom_load_info(&info, size);
    set_region(&info);

    // Copy file contents to CPU ROM memory
    if (size > MAX_ROM_SIZE) {
        printf("WARNING: ROM too large (%zu KB), truncating to %d KB\n",
               size / 1024, MAX_ROM_SIZE / 1024);
        size = MAX_ROM_SIZE;
    }
    memcpy(ROM_DATA, buffer, size);

    #ifdef ROM_SWAP
    bus_log(__FUNCTION__,"--ROM swap mode--");
    for (int i=0; i < size;i+=2 )
    {   
        char z = ROM_DATA[i];
        ROM_DATA[i]=ROM_DATA[i+1];
        ROM_DATA[i+1]=z;
    }
    #endif


    /* QuackShot Rev A (00004054-01, checksum A4B3) needs custom ROM wiring. */
    gwenesis_quackshot_map = 0;
    if (ROM_DATA_LENGTH == 0x80000) {
      if (strstr(info.product, "00004054-01") && info.checksum == 0xA4B3) {
        gwenesis_quackshot_map = 1;
        printf("QuackShot Rev A custom ROM mapping enabled\n");
      } else {
        printf("QuackShot map check: product='%s' checksum=%04X (no match)\n",
               info.product, info.checksum);
      }
    }
    /* ------ SRAM detection from ROM header ------ */
    gwenesis_sram_enabled  = 0;
    gwenesis_sram_odd_only = 0;
    gwenesis_sram_active   = 0;
    gwenesis_sram_write_protect = 0;
    GWENESIS_SRAM = ahb_malloc(MAX_SRAM_SIZE);
    gwenesis_sram_detect_from_rom(&info);

    /* ------ SSF2 mapper detection ------ */
    /*
     * Any ROM larger than 4 MB requires the Super Street Fighter II
     * bankswitching mapper.  Initialise all slots to the identity mapping so
     * that normal (non-banked) ROM access works unchanged until the game writes
     * to the bank registers at 0xA130F3..0xA130FF.
     * Slot 0 (0x000000-0x07FFFF) is always fixed to physical bank 0.
     */
    gwenesis_ssf2_enabled = 0;
    for (int i = 0; i < 8; i++) gwenesis_ssf2_banks[i] = (unsigned char)i;

    if (size > 0x400000) {
        gwenesis_ssf2_enabled = 1;
        printf("SSF2 mapper enabled (ROM size: %zu bytes / %zu KB)\n",
               size, size / 1024);
    }
}

#endif

/******************************************************************************
 *
 *   Power ON the CPU
 *   Initialize 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void power_on() {
  // Set M68K CPU as original MOTOROLA 68000
  //m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  // Initialize M68K CPU
  m68k_init();
  // Build the memory_map table (must be after load_cartridge and m68k_init)
  gwenesis_bus_init_memory_map();
  // Initialize Z80 CPU
  z80_start();
  // Initialize YM2612 chip
  YM2612Init();
  YM2612Config(YM2612_DISCRETE);
  // Initialize PSG SN76489 chip
  // CLOCK_NTSC = 3579545, CLOCK_PAL = 3546895
  if (mode_pal) {
    gwenesis_SN76489_Init(3546895, GWENESIS_AUDIO_BUFFER_LENGTH_PAL * 50, AUDIO_FREQ_DIVISOR);
  } else {
    gwenesis_SN76489_Init(3579545, GWENESIS_AUDIO_BUFFER_LENGTH_NTSC * 60, AUDIO_FREQ_DIVISOR);
  }

}

/******************************************************************************
 *
 *   Reset the CPU Emulation
 *   Send a pulse reset to 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void reset_emulation() {
  // Send a reset pulse to Z80 CPU
  z80_pulse_reset();
  // Send a reset pulse to Z80 M68K
  m68k_pulse_reset();
  // Send a reset pulse to YM2612 chip
  YM2612ResetChip();
  // Send a reset pulse to SEGA 315-5313 chip
  gwenesis_vdp_reset();
  gwenesis_SN76489_Reset();
}

/******************************************************************************
 *
 *   Set Region
 *   Look at ROM to set console compatible region
 *
 ******************************************************************************/
void set_region(gwenesis_rom_info_t *info)
{    
  /*
    old style : JUE characters
    J : Domestic 60Hz (Asia)
    U : Oversea  60Hz (USA) 
    E : Oversea  50Hz (Europe) 

    new style : 1st character
    bit 0 : +1 Domestic 60Hz (Asia)
    bit 1 : +2 Domestc  50Hz (Asia)
    bit 2:  +4 Oversea  60Hz (USA) 
    bit 3:  +4 Oversea  50Hz (Europe) 
  */

  int country = 0;

    /* from Gens */
    if (!memcmp(info->region, "eur", 3)) country |= 8;
    else if (!memcmp(info->region, "EUR", 3)) country |= 8;
    else if (!memcmp(info->region, "Europe", 3)) country |= 8;
    else if (!memcmp(info->region, "jap", 3)) country |= 1;
    else if (!memcmp(info->region, "JAP", 3)) country |= 1;
    else if (!memcmp(info->region, "usa", 3)) country |= 4;
    else if (!memcmp(info->region, "USA", 3)) country |= 4;
    else
    {
      int i;
      unsigned char c;

      /* look for each characters */
      for(i = 0; i < 3; i++)
      {
        c = info->region[i];

        if (c == 'U') country |= 4;
        else if (c == 'E' || c == 'e' ) country |= 8;
        else if (c == 'J' || c == 'j' ) country |= 1;
        else if (c == 'K' || c == 'k' ) country |= 1;
        else if (c < 16) country |= c;
        else if ((c >= '0') && (c <= '9')) country |= c - '0';
        else if ((c >= 'A') && (c <= 'F')) country |= c - 'A' + 10;
      }
    }
    printf("country code=%01x : ",country);
      /* set default console region (USA > EUROPE > JAPAN) */
      /*
      IO REG0	:	MODE 	VMOD 	DISK 	RSV 	VER3 	VER2 	VER1 	VER0
      MODE (R) 	0: Domestic Model
  	            1: Overseas Model
      VMOD (R) 	0: NTSC CPU clock 7.67 MHz
  	            1: PAL CPU clock 7.60 MHz
      */

    /* USA 60Hz*/
    if (country & 4){
      printf("Oversea-NTSC USA 60Hz\n");
      gwenesis_io_set_reg(0, 0x81);
      gwenesis_vdp_status &= 0xFFFE;
      mode_pal = 0;
      gwenesis_detected_region = 0;
      return;
    }
    /* EUROPE 50Hz */
    if (country & 8){
      printf("Oversea-PAL Europe 50Hz\n");
      gwenesis_io_set_reg(0, 0xC1);
      gwenesis_vdp_status |= 0x1;
      mode_pal = 1;
      gwenesis_detected_region = 1;
      return;
    }
    /* set Asia 60HZ */
    if (country & 1){
      printf("Domestic-NTSC Asia 60Hz\n");
      gwenesis_io_set_reg(0, 0x1);
      gwenesis_vdp_status &= 0xFFFE;
      mode_pal = 0;
      gwenesis_detected_region = 2;
      return;
    }
    printf("Oversea-NTSC USA 60Hz no detection>> default mode\n");
    gwenesis_io_set_reg(0, 0x81);
    gwenesis_vdp_status &= 0xFFFE;
    mode_pal = 0;
    gwenesis_detected_region = 0;

}

/* Apply a region directly, bypassing ROM header detection.
 * region_code: 0=USA (NTSC overseas), 1=Europe (PAL), 2=Japan (NTSC domestic).
 * Call after load_cartridge() and before gwenesis_system_init() / power_on(). */
void gwenesis_apply_region_override(int region_code)
{
    switch (region_code) {
    case 1: /* Europe PAL */
        printf("Region override: Europe PAL 50Hz\n");
        gwenesis_io_set_reg(0, 0xC1);
        gwenesis_vdp_status |= 0x1;
        mode_pal = 1;
        break;
    case 2: /* Japan NTSC domestic */
        printf("Region override: Japan NTSC 60Hz\n");
        gwenesis_io_set_reg(0, 0x01);
        gwenesis_vdp_status &= 0xFFFE;
        mode_pal = 0;
        break;
    default: /* 0: USA NTSC overseas */
        printf("Region override: USA NTSC 60Hz\n");
        gwenesis_io_set_reg(0, 0x81);
        gwenesis_vdp_status &= 0xFFFE;
        mode_pal = 0;
        break;
    }
    gwenesis_detected_region = region_code;
}

/* Forward declarations for 0xA1xxxx dispatcher handlers */
static unsigned int mmap_a1_read8(unsigned int address);
static unsigned int mmap_a1_read16(unsigned int address);
static void         mmap_a1_write8(unsigned int address, unsigned int value);
static void         mmap_a1_write16(unsigned int address, unsigned int value);

/* Forward declarations for memory_map update helpers */
void gwenesis_bus_ssf2_update_memory_map(void);
void gwenesis_bus_sram_update_memory_map(void);

/* Forward declarations for cart register (0xA130xx) time_w handlers */
static void cart_time_w_none(unsigned int address, unsigned int value);
static void cart_time_w_sram(unsigned int address, unsigned int value);
static void cart_time_w16_sram(unsigned int address, unsigned int value);
static void cart_time_w_ssf2(unsigned int address, unsigned int value);
static void cart_time_w16_ssf2(unsigned int address, unsigned int value);
static void cart_time_w_ssf2_sram(unsigned int address, unsigned int value);
static void cart_time_w16_ssf2_sram(unsigned int address, unsigned int value);

/* Active cart register write handlers — set once at init */
static void (*cart_time_w8)(unsigned int, unsigned int);
static void (*cart_time_w16)(unsigned int, unsigned int);

/* ============================================================================
 *  memory_map I/O handlers
 *  These are installed as read8/read16/write8/write16 callbacks in the
 *  cpu_memory_map table so that m68ki_read_* / m68ki_write_* (m68kcpu.h)
 *  can dispatch directly.
 * ========================================================================== */

/* ---- ROM -----------------------------------------------------------------
 * Three sets of ROM read handlers are provided — one per mapper situation.
 * The correct set is installed ONCE at init (and on SSF2 bank writes) so
 * that the hot path contains zero conditional branches.
 *
 * All handlers receive the full 24-bit M68K address.  They derive the
 * physical byte offset from memory_map[page].base which is already set to
 * the correct physical ROM page by gwenesis_bus_ssf2_update_memory_map /
 * gwenesis_bus_init_memory_map.
 *
 * ROM_SWAP is always defined: ROM_DATA is stored byte-swapped so that 16-bit
 * aligned reads are naturally big-endian.  Byte reads therefore need ^ 1.
 * On TARGET_GNW we must not do unaligned 16-bit pointer casts (hardfault),
 * so we always reconstruct 16-bit values byte-by-byte.
 * -------------------------------------------------------------------------*/

/* ROM writes are silently ignored (open bus) */
static void mmap_rom_write8(unsigned int address, unsigned int value)  { (void)address; (void)value; }
static void mmap_rom_write16(unsigned int address, unsigned int value) { (void)address; (void)value; }

/* --- Standard ROM (no mapper, full ROM or mirrored) ---------------------- */
/* base already points to the right physical 64 KB page for this slot.
 * For mirrored ROMs the mirror is baked into base at init time. */
static unsigned int mmap_rom_read8(unsigned int address)
{
  const unsigned char *base = m68k.memory_map[(address >> 16) & 0xFF].base;
  return base[(address & 0xFFFF) ^ 1u];
}
static unsigned int mmap_rom_read16(unsigned int address)
{
  const unsigned char *base = m68k.memory_map[(address >> 16) & 0xFF].base;
  unsigned int off = address & 0xFFFEu;   /* force even */
  /* ROM_SWAP: bytes are pair-swapped in ROM_DATA, so word = base[off] | (base[off+1] << 8)
   * Same order as FETCH16ROM on TARGET_GNW. */
  return (unsigned int)base[off] | ((unsigned int)base[off + 1u] << 8);
}

/* SSF2 and QuackShot use the same read handlers as standard ROM — .base
 * already encodes the correct physical page, no per-access tests needed. */

/* ---- ROM mirror / QuackShot page setup -----------------------------------
 * Sets .base for all 64 ROM pages at init. Handles:
 *   - Standard ROM: mirror mask (power-of-2 wrap for ROMs < 4 MB)
 *   - QuackShot Rev A: custom wiring baked into .base, no per-access test
 * Called once from gwenesis_bus_init_memory_map. */
static void gwenesis_bus_set_rom_pages(void)
{
  if (gwenesis_quackshot_map) {
    /* QuackShot Rev A custom ROM wiring:
     * 0x000000-0x0FFFFF: lower 256 KB mirrored every 256 KB (VA18/VA19 open)
     * 0x100000-0x1FFFFF: upper 256 KB mirrored every 256 KB (VA20 → A19)
     * 0x200000-0x3FFFFF: mirror of 0x000000-0x1FFFFF (VA21 open) */
    for (int i = 0; i < 0x40; i++) {
      unsigned int bank = (unsigned int)i;
      unsigned int phys;
      if (bank < 0x10 || (bank >= 0x20 && bank < 0x30))
        phys = (bank & 0x03u) << 16;
      else
        phys = 0x40000u + ((bank & 0x03u) << 16);
      m68k.memory_map[i].base = (unsigned char *)ROM_DATA + phys;
    }
    return;
  }

  /* Standard ROM: mirror mask (round ROM_DATA_LENGTH up to next power-of-two) */
  unsigned int sz = ROM_DATA_LENGTH;
  if (sz == 0) sz = 1;
  sz--;
  sz |= sz >> 1; sz |= sz >> 2; sz |= sz >> 4; sz |= sz >> 8; sz |= sz >> 16;
  unsigned int mask = (sz > 0x3FFFFFu) ? 0x3FFFFFu : sz;

  for (int i = 0; i < 0x40; i++) {
    unsigned int phys = ((unsigned int)i << 16) & mask;
    m68k.memory_map[i].base = (unsigned char *)ROM_DATA + phys;
  }
}


/* ---- RAM ----------------------------------------------------------------- */
static unsigned int mmap_ram_read8(unsigned int address)
{
  return FETCH8RAM(address);
}
static unsigned int mmap_ram_read16(unsigned int address)
{
  return FETCH16RAM(address);
}
static void mmap_ram_write8(unsigned int address, unsigned int value)
{
  WRITE8RAM(address, value);
}
static void mmap_ram_write16(unsigned int address, unsigned int value)
{
  WRITE16RAM(address, value);
}

/* ---- SRAM ----------------------------------------------------------------
 * GWENESIS_SRAM is always allocated as MAX_SRAM_SIZE (64 KB), matching the
 * memory_map page granularity.  We index with (address & 0xFFFF) exactly as
 * Genesis Plus GX does — no bounds check needed because no MD game mixes ROM
 * and SRAM accesses within the same 64 KB page in a way that matters. */
static unsigned int mmap_sram_read8(unsigned int address)
{
  if (gwenesis_sram_odd_only)
    return (address & 1) ? GWENESIS_SRAM[(address & 0xFFFF) >> 1] : 0xFF;
  return GWENESIS_SRAM[address & 0xFFFF];
}
static unsigned int mmap_sram_read16(unsigned int address)
{
  if (gwenesis_sram_odd_only)
    return 0xFF00 | GWENESIS_SRAM[(address & 0xFFFF) >> 1];
  unsigned int off = address & 0xFFFE;  /* force even */
  return (GWENESIS_SRAM[off] << 8) | GWENESIS_SRAM[off + 1];
}
static void mmap_sram_write8(unsigned int address, unsigned int value)
{
  if (gwenesis_sram_write_protect) return;
  if (gwenesis_sram_odd_only) {
    if (address & 1) {
      GWENESIS_SRAM[(address & 0xFFFF) >> 1] = value & 0xFF;
      gwenesis_sram_mark_dirty();
    }
  } else {
    GWENESIS_SRAM[address & 0xFFFF] = value & 0xFF;
    gwenesis_sram_mark_dirty();
  }
}
static void mmap_sram_write16(unsigned int address, unsigned int value)
{
  if (gwenesis_sram_write_protect) return;
  if (gwenesis_sram_odd_only) {
    GWENESIS_SRAM[(address & 0xFFFF) >> 1] = value & 0xFF;
  } else {
    unsigned int off = address & 0xFFFE;
    GWENESIS_SRAM[off]     = (value >> 8) & 0xFF;
    GWENESIS_SRAM[off + 1] = value & 0xFF;
  }
  gwenesis_sram_mark_dirty();
}

/* ---- VDP ----------------------------------------------------------------- */
static unsigned int mmap_vdp_read8(unsigned int address)
{
  return gwenesis_vdp_read_memory_8(address);
}
static unsigned int mmap_vdp_read16(unsigned int address)
{
  return gwenesis_vdp_read_memory_16(address);
}
static void mmap_vdp_write8(unsigned int address, unsigned int value)
{
  /* 8-bit VDP writes are promoted to 16-bit on the real hardware */
  gwenesis_vdp_write_memory_16(address & ~1u, (value << 8) | value);
}
static void mmap_vdp_write16(unsigned int address, unsigned int value)
{
  gwenesis_vdp_write_memory_16(address, value);
}

/* ---- IO ctrl (0xA10000-0xA10FFF) ---------------------------------------- */
static unsigned int mmap_io_read8(unsigned int address)
{
  return gwenesis_io_read_ctrl(address & 0x1F);
}
static unsigned int mmap_io_read16(unsigned int address)
{
  return gwenesis_io_read_ctrl(address & 0x1F);
}
static void mmap_io_write8(unsigned int address, unsigned int value)
{
  gwenesis_io_write_ctrl(address & 0x1F, value);
}
static void mmap_io_write16(unsigned int address, unsigned int value)
{
  gwenesis_io_write_ctrl(address & 0x1F, value);
}

/* ---- Z80 ctrl (0xA11000-0xA11FFF) --------------------------------------- */
static unsigned int mmap_z80ctrl_read8(unsigned int address)
{
  return z80_read_ctrl(address & 0xFFFF);
}
static unsigned int mmap_z80ctrl_read16(unsigned int address)
{
  unsigned int a = address & 0xFFFF;
  if (a == 0x1100)
    return z80_read_busack_word();
  return (z80_read_ctrl(a) << 8) | z80_read_ctrl(a | 1);
}
static void mmap_z80ctrl_write8(unsigned int address, unsigned int value)
{
  z80_write_ctrl(address & 0x1FFF, value);
}
static void mmap_z80ctrl_write16(unsigned int address, unsigned int value)
{
  z80_write_ctrl(address & 0xFFFF, value >> 8);
}

/* ---- 0xA0 page dispatcher (0xA00000-0xA0FFFF) ----------------------------
 * Covers Z80 RAM (0xA00000-0xA03FFF), YM2612 (0xA04000-0xA05FFF),
 * bank register (0xA06000-0xA06FFF), PSG write-only (0xA07000-0xA07FFF),
 * and Z80 bank window (0xA08000-0xA0FFFF, open bus from M68K side).
 * One handler for the whole 64 KB page because the memory_map granularity
 * is 64 KB and all these sub-regions live within 0xA0xxxx.
 * -------------------------------------------------------------------------*/
static unsigned int mmap_a0_read8(unsigned int address)
{
  unsigned int range = address & 0xF000;
  if (range < 0x4000)           return ZRAM[address & 0x1FFF];        /* 0xA00000-0xA03FFF Z80 RAM */
  if (range < 0x6000)           return YM2612Read(m68k_cycles_master());                  /* 0xA04000-0xA05FFF YM2612  */
  if (range < 0x8000)           return 0xFF;                           /* 0xA06000-0xA07FFF bank/PSG read-only */
  return 0xFF;                                                          /* 0xA08000-0xA0FFFF bank window */
}
static unsigned int mmap_a0_read16(unsigned int address)
{
  unsigned int v = mmap_a0_read8(address);
  return (v << 8) | v;
}
static void mmap_a0_write8(unsigned int address, unsigned int value)
{
  unsigned int range = address & 0xF000;
  if (range < 0x4000) { ZRAM[address & 0x1FFF] = value; return; }      /* Z80 RAM */
  if (range < 0x6000) { YM2612Write((address & 0x3), (value & 0xFF), m68k_cycles_master()); return; } /* YM2612 */
  if (range == 0x6000) { /* Z80 bank register */
    z80_bank_register_write(value);
    return;
  }
  if (range == 0x7000) { gwenesis_SN76489_Write(value & 0xFF, m68k_cycles_master()); return; } /* PSG */
  /* 0xA08000-0xA0FFFF: Z80 bank window — open bus, ignore */
}
static void mmap_a0_write16(unsigned int address, unsigned int value)
{
  unsigned int range = address & 0xF000;
  if (range < 0x4000) { ZRAM[address & 0x1FFF] = value >> 8; return; } /* Z80 RAM, high byte only */
  if (range < 0x6000) { YM2612Write(address & 0x3, value >> 8, m68k_cycles_master()); return; }
  if (range == 0x6000) { z80_bank_register_write(value >> 8); return; } /* Z80 bank register */
  if (range == 0x7000) { gwenesis_SN76489_Write(value >> 8, m68k_cycles_master()); return; }
}

/* ---- Z80 bank window (0xA08000-0xA0FFFF) — open bus -------------------- */
static unsigned int mmap_openbus_read8(unsigned int address)  { (void)address; return 0xFF; }
static unsigned int mmap_openbus_read16(unsigned int address) { (void)address; return 0xFFFF; }
static void mmap_openbus_write8(unsigned int address, unsigned int value)   { (void)address; (void)value; }
static void mmap_openbus_write16(unsigned int address, unsigned int value)  { (void)address; (void)value; }



/* ---- TMSS ctrl (0xA14000-0xA14003) -------------------------------------- */
static unsigned int mmap_tmss_read8(unsigned int address)
{
  if (tmss_state == 0) return TMSS[address & 0x3];
  return 0xFF;
}
static unsigned int mmap_tmss_read16(unsigned int address)
{
  if (tmss_state == 0) return (TMSS[address & 0x3] << 8) | TMSS[(address + 1) & 0x3];
  return 0xFFFF;
}
static void mmap_tmss_write8(unsigned int address, unsigned int value)
{
  if (tmss_state == 0) {
    TMSS[address & 0x3] = value;
    tmss_count++;
    if (tmss_count == 4) tmss_state = 1;
  }
}
static void mmap_tmss_write16(unsigned int address, unsigned int value)
{
  mmap_tmss_write8(address,     (value >> 8) & 0xFF);
  mmap_tmss_write8(address + 1,  value       & 0xFF);
}

/* ============================================================================
 *  gwenesis_bus_ssf2_update_memory_map
 *  Rebuild the ROM slots (0x00..0x3F) in the memory_map after an SSF2 bank
 *  register write.  Only .base is updated — the read handlers (mmap_rom_read8/16)
 *  derive the physical address from .base directly, so no handler swap needed.
 * ========================================================================== */
void gwenesis_bus_ssf2_update_memory_map(void)
{
  /* Each logical slot covers 8 x 64 KB pages = 512 KB (0x80000 bytes).
   * gwenesis_ssf2_banks[slot] gives the physical 512 KB page to map in. */
  for (int slot = 0; slot < 8; slot++) {
    unsigned int phys_base = (unsigned int)gwenesis_ssf2_banks[slot] << 19;
    for (int page = 0; page < 8; page++) {
      int idx = slot * 8 + page;  /* 0x00 .. 0x3F */
      m68k.memory_map[idx].base = (unsigned char *)ROM_DATA + phys_base + ((unsigned int)page << 16);
    }
  }
}

/* ============================================================================
 *  gwenesis_bus_sram_update_memory_map
 *  Called whenever gwenesis_sram_active or gwenesis_sram_write_protect changes.
 * ========================================================================== */
void gwenesis_bus_sram_update_memory_map(void)
{
  if (!gwenesis_sram_enabled) return;

  unsigned int start_page = (gwenesis_sram_start >> 16) & 0xFF;
  unsigned int end_page   = (gwenesis_sram_end   >> 16) & 0xFF;

  for (unsigned int p = start_page; p <= end_page && p < 0x80; p++) {
    if (gwenesis_sram_active) {
      m68k.memory_map[p].read8   = mmap_sram_read8;
      m68k.memory_map[p].read16  = mmap_sram_read16;
      m68k.memory_map[p].write8  = mmap_sram_write8;
      m68k.memory_map[p].write16 = mmap_sram_write16;
    } else {
      /* SRAM deactivated — fall back to ROM handlers */
      m68k.memory_map[p].read8   = mmap_rom_read8;
      m68k.memory_map[p].read16  = mmap_rom_read16;
      m68k.memory_map[p].write8  = mmap_rom_write8;
      m68k.memory_map[p].write16 = mmap_rom_write16;
    }
  }
}

/* ============================================================================
 *  gwenesis_bus_init_memory_map
 *  Build the full 256-entry memory_map for the M68K CPU.
 *  Must be called after load_cartridge() (SRAM/SSF2 flags are set) and before
 *  reset_emulation() / m68k_pulse_reset().
 * ========================================================================== */
void gwenesis_bus_init_memory_map(void)
{
  int i;

  /* ------------------------------------------------------------------ */
  /* Install cart register (0xA130xx) handler — one function pointer,   */
  /* chosen once here, zero tests in the hot path.                       */
  /* ------------------------------------------------------------------ */
  if (gwenesis_ssf2_enabled && gwenesis_sram_enabled) {
    cart_time_w8  = cart_time_w_ssf2_sram;
    cart_time_w16 = cart_time_w16_ssf2_sram;
  } else if (gwenesis_ssf2_enabled) {
    cart_time_w8  = cart_time_w_ssf2;
    cart_time_w16 = cart_time_w16_ssf2;
  } else if (gwenesis_sram_enabled) {
    cart_time_w8  = cart_time_w_sram;
    cart_time_w16 = cart_time_w16_sram;
  } else {
    cart_time_w8  = cart_time_w_none;
    cart_time_w16 = cart_time_w_none;
  }

  /* ------------------------------------------------------------------ */
  /* 0x00-0x3F : ROM space (4 MB logical, up to 32 MB physical via SSF2) */
  /* ------------------------------------------------------------------ */

  /* Set .base for all 64 ROM pages — handles mirror, SSF2, QuackShot   */
  if (gwenesis_ssf2_enabled)
    gwenesis_bus_ssf2_update_memory_map();   /* sets .base for all 64 pages */
  else
    gwenesis_bus_set_rom_pages();            /* sets .base with mirror mask  */

  /* Install the same read/write handlers for all ROM pages.
   * mmap_rom_read8/16 use .base directly — zero conditional branches. */
  for (i = 0; i < 0x40; i++) {
    m68k.memory_map[i].read8   = mmap_rom_read8;
    m68k.memory_map[i].read16  = mmap_rom_read16;
    m68k.memory_map[i].write8  = mmap_rom_write8;
    m68k.memory_map[i].write16 = mmap_rom_write16;
  }

  /* If SRAM is active, overlay the SRAM pages */
  if (gwenesis_sram_enabled && gwenesis_sram_active) {
    gwenesis_bus_sram_update_memory_map();
  }

  /* ------------------------------------------------------------------ */
  /* 0x40-0x9F : unmapped (open bus) */
  /* ------------------------------------------------------------------ */
  for (i = 0x40; i < 0xA0; i++) {
    m68k.memory_map[i].base    = NULL;
    m68k.memory_map[i].read8   = mmap_openbus_read8;
    m68k.memory_map[i].read16  = mmap_openbus_read16;
    m68k.memory_map[i].write8  = mmap_openbus_write8;
    m68k.memory_map[i].write16 = mmap_openbus_write16;
  }

  /* ------------------------------------------------------------------ */
  /* 0xA0 : Z80 address space (0xA00000-0xA0FFFF)                       */
  /* 0xA00000-0xA07FFF : Z80 RAM                                         */
  /* 0xA08000-0xA0FFFF : Z80 bank window (open bus from M68K side)       */
  /* ------------------------------------------------------------------ */
  m68k.memory_map[0xA0].base    = NULL;
  m68k.memory_map[0xA0].read8   = mmap_a0_read8;
  m68k.memory_map[0xA0].read16  = mmap_a0_read16;
  m68k.memory_map[0xA0].write8  = mmap_a0_write8;
  m68k.memory_map[0xA0].write16 = mmap_a0_write16;

  /* YM2612 sits at 0xA04000-0xA05FFF inside the Z80 window.
   * On the real hardware the M68K sees the full Z80 bus at 0xA00000-0xA0FFFF
   * through a single page — one handler for 0xA0 inspects the sub-address. */

  /* ------------------------------------------------------------------ */
  /* 0xA1 : I/O / control registers (0xA10000-0xA1FFFF)                  */
  /*   0xA10000-0xA10FFF : I/O ctrl                                      */
  /*   0xA11000-0xA11FFF : Z80 ctrl                                      */
  /*   0xA13000-0xA13FFF : cart registers (SRAM ctrl + SSF2 banks)       */
  /*   0xA14000-0xA14003 : TMSS                                          */
  /* ------------------------------------------------------------------ */
  m68k.memory_map[0xA1].base    = NULL;
  m68k.memory_map[0xA1].read8   = mmap_a1_read8;
  m68k.memory_map[0xA1].read16  = mmap_a1_read16;
  m68k.memory_map[0xA1].write8  = mmap_a1_write8;
  m68k.memory_map[0xA1].write16 = mmap_a1_write16;

  /* ------------------------------------------------------------------ */
  /* 0xA2-0xBF : unmapped (open bus)                                    */
  /* ------------------------------------------------------------------ */
  for (i = 0xA2; i < 0xC0; i++) {
    m68k.memory_map[i].base    = NULL;
    m68k.memory_map[i].read8   = mmap_openbus_read8;
    m68k.memory_map[i].read16  = mmap_openbus_read16;
    m68k.memory_map[i].write8  = mmap_openbus_write8;
    m68k.memory_map[i].write16 = mmap_openbus_write16;
  }

  /* ------------------------------------------------------------------ */
  /* 0xC0-0xDF : VDP (0xC00000-0xDFFFFF)                               */
  /* ------------------------------------------------------------------ */
  for (i = 0xC0; i < 0xE0; i++) {
    m68k.memory_map[i].base    = NULL;
    m68k.memory_map[i].read8   = mmap_vdp_read8;
    m68k.memory_map[i].read16  = mmap_vdp_read16;
    m68k.memory_map[i].write8  = mmap_vdp_write8;
    m68k.memory_map[i].write16 = mmap_vdp_write16;
  }

  /* ------------------------------------------------------------------ */
  /* 0xE0-0xFF : RAM (0xE00000-0xFFFFFF, mirrors every 64 KB)           */
  /* ------------------------------------------------------------------ */
  for (i = 0xE0; i < 0x100; i++) {
    m68k.memory_map[i].base    = M68K_RAM; /* used by immediate fallback on desktop */
    m68k.memory_map[i].read8   = mmap_ram_read8;
    m68k.memory_map[i].read16  = mmap_ram_read16;
    m68k.memory_map[i].write8  = mmap_ram_write8;
    m68k.memory_map[i].write16 = mmap_ram_write16;
  }
}

/* ============================================================================
 *  0xA13000 cart register handler — installed as function pointer at init.
 *  Exactly one of these is active per session, chosen in gwenesis_bus_init_memory_map.
 * ========================================================================== */

/* No cart register (open bus) */
static void cart_time_w_none(unsigned int address, unsigned int value)
{
  (void)address; (void)value;
}

/* SRAM ctrl only (0xA130F0-0xA130F1) */
static void cart_time_w_sram(unsigned int address, unsigned int value)
{
  if ((address & 0xFF) >= 0xF0 && (address & 0xFF) <= 0xF1) {
    gwenesis_sram_active        = value & 0x01;
    gwenesis_sram_write_protect = (value >> 1) & 0x01;
    gwenesis_bus_sram_update_memory_map();
  }
}
static void cart_time_w16_sram(unsigned int address, unsigned int value)
{
  cart_time_w_sram(address, value >> 8);
}

/* SSF2 bank ctrl (0xA130F2-0xA130FF), no SRAM */
static void cart_time_w_ssf2(unsigned int address, unsigned int value)
{
  unsigned int slot = (address - 0xA130F0) >> 1;
  if (slot >= 1 && slot <= 7) {
    gwenesis_ssf2_banks[slot] = value & 0x3F;
    gwenesis_bus_ssf2_update_memory_map();
  }
}
static void cart_time_w16_ssf2(unsigned int address, unsigned int value)
{
  /* GPGX passes raw data to time_w for both 8 and 16-bit writes.
   * SSF2 bank registers sit at odd addresses (0xA130F3, F5, ...) so
   * a write16 to 0xA130F2 puts the bank number in the low byte. */
  cart_time_w_ssf2(address, value & 0xFF);
}

/* SSF2 + SRAM */
static void cart_time_w_ssf2_sram(unsigned int address, unsigned int value)
{
  if (address >= 0xA130F2 && address <= 0xA130FF)
    cart_time_w_ssf2(address, value);
  else
    cart_time_w_sram(address, value);
}
static void cart_time_w16_ssf2_sram(unsigned int address, unsigned int value)
{
  /* Dispatch by address range — each sub-region has its own byte lane convention:
   * 0xA130F0-0xA130F1 (SRAM ctrl): high byte of word, matching cart_time_w16_sram
   * 0xA130F2-0xA130FF (SSF2 banks): low byte of word, matching cart_time_w16_ssf2 */
  if (address >= 0xA130F2 && address <= 0xA130FF)
    cart_time_w_ssf2(address, value & 0xFF);
  else
    cart_time_w_sram(address, value >> 8);
}

/* Active pointers — set once at init (see gwenesis_bus_init_memory_map) */

/* ============================================================================
 *  0xA1xxxx dispatcher handlers — zero conditional branches for mapper dispatch
 * ========================================================================== */
static unsigned int mmap_a1_read8(unsigned int address)
{
  /* 0xA130F0-0xA130F1: SRAM control register read */
  if (gwenesis_sram_enabled
      && (address & 0xFFFF00) == 0xA13000
      && (address & 0xFF) >= 0xF0 && (address & 0xFF) <= 0xF1)
    return gwenesis_sram_active | (gwenesis_sram_write_protect << 1);
  unsigned int range = address & 0xF000;
  if (range == 0)      return mmap_io_read8(address);
  if (range == 0x1000) return mmap_z80ctrl_read8(address);
  if (range == 0x4000) return mmap_tmss_read8(address);
  return 0xFF;
}
static unsigned int mmap_a1_read16(unsigned int address)
{
  if (gwenesis_sram_enabled
      && (address & 0xFFFF00) == 0xA13000
      && (address & 0xFF) >= 0xF0 && (address & 0xFF) <= 0xF1)
    return gwenesis_sram_active | (gwenesis_sram_write_protect << 1);
  unsigned int range = address & 0xF000;
  if (range == 0)      return mmap_io_read16(address);
  if (range == 0x1000) return mmap_z80ctrl_read16(address);
  if (range == 0x4000) return mmap_tmss_read16(address);
  return 0xFFFF;
}
static void mmap_a1_write8(unsigned int address, unsigned int value)
{
  if ((address & 0xFFFF00) == 0xA13000) {
    cart_time_w8(address, value);
    return;
  }
  unsigned int range = address & 0xF000;
  if (range == 0)      { mmap_io_write8(address, value);      return; }
  if (range == 0x1000) { mmap_z80ctrl_write8(address, value); return; }
  if (range == 0x4000) { mmap_tmss_write8(address, value);    return; }
}
static void mmap_a1_write16(unsigned int address, unsigned int value)
{
  if ((address & 0xFFFF00) == 0xA13000) {
    cart_time_w16(address, value);
    return;
  }
  unsigned int range = address & 0xF000;
  if (range == 0)      { mmap_io_write16(address, value);      return; }
  if (range == 0x1000) { mmap_z80ctrl_write16(address, value); return; }
  if (range == 0x4000) { mmap_tmss_write16(address, value);    return; }
}

/* ============================================================================
 *  Disassembler helpers — use the memory_map read16 path
 * ========================================================================== */
unsigned int m68k_read_disassembler_8(unsigned int address)
{
  cpu_memory_map *m = &m68k.memory_map[(address >> 16) & 0xFF];
  if (m->read8) return (*m->read8)(address & 0xFFFFFF);
  if (m->base)  return READ_BYTE(m->base, address & 0xFFFF);
  return 0xFF;
}
unsigned int m68k_read_disassembler_16(unsigned int address)
{
  cpu_memory_map *m = &m68k.memory_map[(address >> 16) & 0xFF];
  if (m->read16) return (*m->read16)(address & 0xFFFFFF);
  if (m->base)   return *(uint16 *)(m->base + (address & 0xFFFF));
  return 0xFFFF;
}
unsigned int m68k_read_disassembler_32(unsigned int address)
{
  return (m68k_read_disassembler_16(address) << 16) | m68k_read_disassembler_16(address + 2);
}

void gwenesis_bus_save_state(FILE *file) {
  fwrite((unsigned char *)M68K_RAM, MAX_RAM_SIZE, 1, file);
  fwrite((unsigned char *)ZRAM, MAX_Z80_RAM_SIZE, 1, file);
  fwrite((unsigned char *)TMSS, sizeof(TMSS), 1, file);
  fwrite((unsigned char *)&tmss_state, 4, 1, file);
  fwrite((unsigned char *)&tmss_count, 4, 1, file);
  /* SRAM */
  if (gwenesis_sram_enabled) {
    fwrite((unsigned char *)&gwenesis_sram_active,         4, 1, file);
    fwrite((unsigned char *)&gwenesis_sram_write_protect,  4, 1, file);
  }
  /* SSF2 mapper */
  if (gwenesis_ssf2_enabled) {
    fwrite((unsigned char *)gwenesis_ssf2_banks, sizeof(gwenesis_ssf2_banks), 1, file);
  }
}

void gwenesis_bus_load_state(FILE *file, int ss_version) {
  fread((unsigned char *)M68K_RAM, MAX_RAM_SIZE, 1, file);
  fread((unsigned char *)ZRAM, MAX_Z80_RAM_SIZE, 1, file);
  fread((unsigned char *)TMSS, sizeof(TMSS), 1, file);
  fread((unsigned char *)&tmss_state, 4, 1, file);
  fread((unsigned char *)&tmss_count, 4, 1, file);
  if (ss_version >= 1) {
    /* SRAM */
    if (gwenesis_sram_enabled) {
      fread((unsigned char *)&gwenesis_sram_active,         4, 1, file);
      fread((unsigned char *)&gwenesis_sram_write_protect,  4, 1, file);
    }
    /* SSF2 mapper */
    if (gwenesis_ssf2_enabled) {
      fread((unsigned char *)gwenesis_ssf2_banks, sizeof(gwenesis_ssf2_banks), 1, file);
    }
  }

  /* Resync the memory_map with restored mapper state */
  if (gwenesis_ssf2_enabled)
    gwenesis_bus_ssf2_update_memory_map();
  if (gwenesis_sram_enabled)
    gwenesis_bus_sram_update_memory_map();
}
