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

// TMSS
int tmss_state = 0;
int tmss_count = 0;

/******************************************************************************
 *
 *   Load a Sega Genesis Cartridge into CPU Memory
 *
 ******************************************************************************/


 #ifdef TARGET_GNW

void load_cartridge()
{
    // Clear all volatile memory
    M68K_RAM = itc_malloc(MAX_RAM_SIZE); // M68K RAM 
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // Set Z80 Memory as Z80_RAM
    z80_set_memory(ZRAM);

    z80_pulse_reset();

    set_region();

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
    memset(GWENESIS_SRAM, 0x00, MAX_SRAM_SIZE);

    unsigned char flag_hi  = FETCH8ROM(0x1B0);
    unsigned char flag_lo  = FETCH8ROM(0x1B1);
    unsigned char sram_type = FETCH8ROM(0x1B2);

    if (flag_hi == 0x52 && flag_lo == 0x41) { /* "RA" */

        /* Odd-only (byte-wide SRAM on D0-D7): type byte has bit 3 set, e.g. 0xF9 */
        gwenesis_sram_odd_only = (sram_type & 0x08) ? 1 : 0;

        gwenesis_sram_start = ((unsigned int)FETCH8ROM(0x1B4) << 24) |
                     ((unsigned int)FETCH8ROM(0x1B5) << 16) |
                     ((unsigned int)FETCH8ROM(0x1B6) <<  8) |
                      (unsigned int)FETCH8ROM(0x1B7);
        gwenesis_sram_end   = ((unsigned int)FETCH8ROM(0x1B8) << 24) |
                     ((unsigned int)FETCH8ROM(0x1B9) << 16) |
                     ((unsigned int)FETCH8ROM(0x1BA) <<  8) |
                      (unsigned int)FETCH8ROM(0x1BB);

        /* Force start to even boundary so address math is consistent */
        gwenesis_sram_start &= ~1u;

        /* Actual number of bytes in our SRAM[] array:
         * odd-only -> only odd addresses carry data, so effective entries = range/2 */
        unsigned int addr_range = gwenesis_sram_end - gwenesis_sram_start + 1;
        unsigned int sram_size  = gwenesis_sram_odd_only ? (addr_range / 2) : addr_range;
        if (sram_size > MAX_SRAM_SIZE) sram_size = MAX_SRAM_SIZE;

        gwenesis_sram_enabled = 1;
        /* SRAM is active by default — old games (pre-1993, e.g. Landstalker)
         * never write to 0xA130F1; they expect SRAM to be always mapped.
         * Games that use the register will explicitly set/clear gwenesis_sram_active. */
        gwenesis_sram_active  = 1;
        printf("SRAM detected: start=0x%06X end=0x%06X size=%d bytes mode=%s\n",
               gwenesis_sram_start, gwenesis_sram_end, sram_size,
               gwenesis_sram_odd_only ? "odd-only" : "full");
    } else {
        printf("No SRAM detected in ROM header\n");
    }

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
    // Clear all volatile memory
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);
    memset(ROM_DATA, 0, MAX_ROM_SIZE);

    // Set Z80 Memory as ZRAM
    z80_set_memory(ZRAM);
    z80_pulse_reset();

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


    set_region();

    /* ------ SRAM detection from ROM header ------ */
    gwenesis_sram_enabled  = 0;
    gwenesis_sram_odd_only = 0;
    gwenesis_sram_active   = 0;
    gwenesis_sram_write_protect = 0;
    memset(GWENESIS_SRAM, 0x00, MAX_SRAM_SIZE);

    if (ROM_DATA[0x1B0] == 0x52 && ROM_DATA[0x1B1] == 0x41) { /* "RA" */
        unsigned char sram_type = ROM_DATA[0x1B2];
        gwenesis_sram_odd_only = (sram_type & 0x08) ? 1 : 0;

        gwenesis_sram_start = ((unsigned int)ROM_DATA[0x1B4] << 24) |
                     ((unsigned int)ROM_DATA[0x1B5] << 16) |
                     ((unsigned int)ROM_DATA[0x1B6] <<  8) |
                      (unsigned int)ROM_DATA[0x1B7];
        gwenesis_sram_end   = ((unsigned int)ROM_DATA[0x1B8] << 24) |
                     ((unsigned int)ROM_DATA[0x1B9] << 16) |
                     ((unsigned int)ROM_DATA[0x1BA] <<  8) |
                      (unsigned int)ROM_DATA[0x1BB];

        gwenesis_sram_start &= ~1u;  /* force even boundary */

        unsigned int addr_range = gwenesis_sram_end - gwenesis_sram_start + 1;
        unsigned int sram_size  = gwenesis_sram_odd_only ? (addr_range / 2) : addr_range;
        if (sram_size > MAX_SRAM_SIZE) sram_size = MAX_SRAM_SIZE;

        gwenesis_sram_enabled = 1;
        /* SRAM is active by default — old games (pre-1993, e.g. Landstalker)
         * never write to 0xA130F1; they expect SRAM to be always mapped.
         * Games that use the register will explicitly set/clear gwenesis_sram_active. */
        gwenesis_sram_active  = 1;
        printf("SRAM detected: start=0x%06X end=0x%06X size=%d bytes mode=%s\n",
               gwenesis_sram_start, gwenesis_sram_end, sram_size,
               gwenesis_sram_odd_only ? "odd-only" : "full");
    } else {
        printf("No SRAM detected in ROM header\n");
    }

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
  // Initialize Z80 CPU
  z80_start();
  // Initialize YM2612 chip
  YM2612Init();
  YM2612Config(9);
  // Initialize PSG SN76489 chip
  //CLOCK_NTSC      = 3579545,
  //CLOCK_PAL       = 3546895,
 // CLOCK_NTSC_SMS1 = 3579527

//  if (mode_pal) {
//     gwenesis_SN76489_Init(3546895, GWENESIS_AUDIO_BUFFER_LENGTH_PAL*50,AUDIO_FREQ_DIVISOR);
//   } else{
//     gwenesis_SN76489_Init(3579545, GWENESIS_AUDIO_BUFFER_LENGTH_NTSC*60,AUDIO_FREQ_DIVISOR);
//   }
  
  gwenesis_SN76489_Init(3579545, 888*60,AUDIO_FREQ_DIVISOR);

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
void set_region()
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

   // extern int mode_pal;

    int country = 0;

    char rom_str[3];

    printf("ROM game  : ");
    for (int j=0; j < 48;j++) printf("%c",(char)FETCH8ROM(0x150+j));
    printf("\n");

    rom_str[0]=FETCH8ROM(0x1F0);
    rom_str[1]=FETCH8ROM(0x1F1);
    rom_str[2]=FETCH8ROM(0x1F2);

    printf("ROM region:%c%c%c (0x%02x 0x%02x 0x%02x)\n", rom_str[0],rom_str[1],rom_str[2],rom_str[0],rom_str[1],rom_str[2]);

    /* from Gens */
    if (!memcmp(rom_str, "eur", 3)) country |= 8;
    else if (!memcmp(rom_str, "EUR", 3)) country |= 8;
    else if (!memcmp(rom_str, "Europe", 3)) country |= 8;
    else if (!memcmp(rom_str, "jap", 3)) country |= 1;
    else if (!memcmp(rom_str, "JAP", 3)) country |= 1;
    else if (!memcmp(rom_str, "usa", 3)) country |= 4;
    else if (!memcmp(rom_str, "USA", 3)) country |= 4;
    else
    {
      int i;
      unsigned char c;

      /* look for each characters */
      for(i = 0; i < 3; i++)
      {
        c = rom_str[i];

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
   //   gwenesis_vdp_status &= 0xFFFE;
     // mode_pal = 0;
      return;
    }
    /* EUROPE 50Hz */
    if (country & 8){
      printf("Oversea-PAL Europe 50Hz\n");
      gwenesis_io_set_reg(0, 0xC1);
    //  gwenesis_vdp_status |= 0x1;
      //mode_pal = 1;
      return;
    }
    /* set Asia 60HZ */
    if (country & 1){
      printf("Domestic-NTSC Asia 60Hz\n");
      gwenesis_io_set_reg(0, 0x1);
    //  gwenesis_vdp_status &= 0xFFFE;
      //mode_pal = 0;
      return;
    }
      printf("Oversea-NTSC USA 60Hz no detection>> default mode\n");
      gwenesis_io_set_reg(0, 0x81);
     // gwenesis_vdp_status &= 0xFFFE;
     // mode_pal = 0;

}
/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *   68K Access to Z80 Memory
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_z80_address(unsigned int address) {

  unsigned int range = (address & 0xF000);
  switch (range) {
  case 0:
  case 0x1000:
    return Z80_RAM_ADDR;
  case 0x2000:
  case 0x3000:
    return Z80_RAM_ADDR1K;
  case 0x4000:
    return Z80_YM2612_ADDR;
  case 0x6000:
    return Z80_BANK_ADDR;
  case 0x7000:
    return Z80_SN76489_ADDR;
  default:
    bus_log(__FUNCTION__,"no map Z80 %x",address);
    assert(0);
    return NONE;
  }
}

/* SRAM-aware data read helpers used only in gwenesis_bus_read_memory_*.
 * These replace FETCH8ROM/16ROM in the ROM_ADDR: handlers so that reads to
 * the SRAM address window return SRAM data instead of ROM.
 * SSF2 translation is handled inside the FETCH*ROM macros (m68k.h),
 * so these helpers just add the SRAM overlay on top. */
static inline unsigned int fetch8rom_data(unsigned int a) {
  if (gwenesis_sram_active && a >= gwenesis_sram_start && a <= gwenesis_sram_end) {
      if (gwenesis_sram_odd_only)
          return (a & 1) ? GWENESIS_SRAM[((a - gwenesis_sram_start) >> 1) & GWENESIS_SRAM_MASK] : 0xFF;
      return GWENESIS_SRAM[(a - gwenesis_sram_start) & GWENESIS_SRAM_MASK];
  }
  return FETCH8ROM(a);
}
static inline unsigned int fetch16rom_data(unsigned int a) {
  if (gwenesis_sram_active && a >= gwenesis_sram_start && a <= gwenesis_sram_end) {
      if (gwenesis_sram_odd_only)
          return 0xFF00 | GWENESIS_SRAM[((a - gwenesis_sram_start) >> 1) & GWENESIS_SRAM_MASK];
      unsigned int off = (a - gwenesis_sram_start) & GWENESIS_SRAM_MASK;
      return (GWENESIS_SRAM[off] << 8) | GWENESIS_SRAM[off + 1];
  }
  return FETCH16ROM(a);
}

/******************************************************************************
 *
 *   IO memory address mapper
 *   Map all input/output region address for CPU program
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_io_address(unsigned int address)
{
  /* 0xA130xx range: cartridge registers (SRAM control + SSF2 bankswitching) */
  if ((address & 0xFFFF00) == 0xA13000) {
    /* 0xA130F2-0xA130FF: SSF2 bankswitching registers (slots 1-7)
     * Written as byte to odd address: 0xA130F3, F5, F7, F9, FB, FD, FF
     * We also accept even-address word writes (bus writes high byte to even). */
    if (gwenesis_ssf2_enabled && address >= 0xA130F2 && address <= 0xA130FF)
      return SSF2_BANK_CTRL;

    /* 0xA130F0/F1: SRAM control register */
    if (gwenesis_sram_enabled)
      return SRAM_CTRL;

    return NONE;
  }

  unsigned int range = (address & 0x1000) ;
  switch (range) {
  case 0:      return IO_CTRL;
  case 0x1000: return Z80_CTRL;
  default:
      // if (address >= 0xa14000 && address < 0xa11404)
      // return (tmss_state == 0) ? TMSS_CTRL : NONE;
      bus_log(__FUNCTION__,"no map io %x",address);

    return NONE;
  }
}

/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *
 ******************************************************************************/

static inline 
unsigned int gwenesis_bus_map_address(unsigned int address) {
  // Mask address page
  unsigned int range = (address & 0xFF0000) >> 16;

  /* SRAM must be checked BEFORE the ROM catch-all (range < 0x80),
   * because SRAM lives at 0x200000-0x3FFFFF which is inside that range. */
  if (gwenesis_sram_active &&
      address >= gwenesis_sram_start &&
      address <= gwenesis_sram_end)
    return SRAM_ADDR;

  // Check mask and select memory type
  if (range < 0x80) //        ROM ADDRESS 0x000000 - 0x3FFFFF
    return ROM_ADDR;

  else if (range == 0xA0) // Z80 ADDRESS 0xA00000 - 0xA0FFFF
    return gwenesis_bus_map_z80_address(address);


  else if (range == 0xA1) //                  IO ADDRESS  0xA10000 - 0xA1FFFF
    return gwenesis_bus_map_io_address(address);

  else if (range == 0xC0) // VDP ADDRESS 0xC00000 - 0xDFFFFFF
    return VDP_ADDR;
  else if (range == 0xFF) // RAM ADDRESS 0xE00000 - 0xFFFFFFF
    return RAM_ADDR;
  // If not a valid address return 0
  bus_log(__FUNCTION__,"M68K > ?? unnmap address %x", address);
  //assert(0);
  return NONE;
}
/******************************************************************************
 *
 *   Main read address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_read_memory_8(unsigned int address) {
 bus_log(__FUNCTION__,"read8  %x", address);

  switch (gwenesis_bus_map_address(address)) {
  
  case VDP_ADDR:
    return gwenesis_vdp_read_memory_8(address);

  case ROM_ADDR:
    return fetch8rom_data(address);

  case SRAM_ADDR: {
    unsigned int val;
    if (gwenesis_sram_odd_only) {
      if (address & 1) {
        val = GWENESIS_SRAM[((address - gwenesis_sram_start) >> 1) & GWENESIS_SRAM_MASK];
      } else {
        val = 0xFF;
      }
    } else {
      val = GWENESIS_SRAM[(address - gwenesis_sram_start) & GWENESIS_SRAM_MASK];
    }

    return val;
  }

  case RAM_ADDR:
    return FETCH8RAM(address);

  case SRAM_CTRL:
    /* 0xA130F1: return current SRAM control register state */
    return gwenesis_sram_active | (gwenesis_sram_write_protect << 1);

  case IO_CTRL:
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
    return z80_read_ctrl(address & 0xFFFF);

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    return ZRAM[address & 0x1FFF];

  case Z80_YM2612_ADDR:
    return YM2612Read(m68k_cycles_master());

  case Z80_SN76489_ADDR:
    return 0xff;

  case Z80_BANK_ADDR:
    return 0xff;

  case TMSS_CTRL:
    bus_log(__FUNCTION__,"TMS");
    if (tmss_state == 0)
      return TMSS[address & 0x4];
    return 0xFF;

  default:
     bus_log(__FUNCTION__," default read 8 %x", address);
    return 0x00;
  }
  return 0x00;
}

static inline unsigned int gwenesis_bus_read_memory_16(unsigned int address) {
   bus_log(__FUNCTION__,"read16 %x", address);
   unsigned int ret_value;

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    return gwenesis_vdp_read_memory_16(address);

  case RAM_ADDR:
    return FETCH16RAM(address);

  case ROM_ADDR:
    return fetch16rom_data(address);

  case SRAM_ADDR: {
    if (gwenesis_sram_odd_only) {
      /* Odd byte = low byte of word, even byte (high) = 0xFF (bus open) */
      unsigned int idx = ((address - gwenesis_sram_start) >> 1) & GWENESIS_SRAM_MASK;
      return 0xFF00 | GWENESIS_SRAM[idx];
    }
    unsigned int off = (address - gwenesis_sram_start) & GWENESIS_SRAM_MASK;
    return (GWENESIS_SRAM[off] << 8) | GWENESIS_SRAM[off + 1];
  }

  case SRAM_CTRL:
    return gwenesis_sram_active | (gwenesis_sram_write_protect << 1);

  case IO_CTRL:
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
  //  ret_value = z80_read_ctrl(address & 0xFFFF); 
   // return ret_value | ret_value << 8;
    address &=0xFFFF;
        return (z80_read_ctrl(address) << 8) | z80_read_ctrl(address | 1);


  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    return ZRAM[address & 0X1FFF] | (ZRAM[address & 0X1FFF] << 8);

  case Z80_YM2612_ADDR:
    ret_value = YM2612Read(m68k_cycles_master());
    return ret_value | ret_value << 8;


  case Z80_SN76489_ADDR:
    return 0xff;

  case Z80_BANK_ADDR:
    return 0xff;

  default:
    bus_log(__FUNCTION__,"read mem 16 default %x", address);
    return (gwenesis_bus_read_memory_8(address) << 8) |
           gwenesis_bus_read_memory_8(address + 1);
  }
  return 0x00;
}

/******************************************************************************
 *
 *   Main write address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline void gwenesis_bus_write_memory_8(unsigned int address,
                                              unsigned int value) {
  bus_log(__FUNCTION__,"write8  @%x:%x", address,value);

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    gwenesis_vdp_write_memory_16(address & ~1, (value << 8) | value);
    return;

  case RAM_ADDR:
    WRITE8RAM(address, value);
    return;

  case SRAM_ADDR:
    if (gwenesis_sram_write_protect) return;
    if (gwenesis_sram_odd_only) {
      /* Byte-wide SRAM: only odd addresses are writable */
      if (address & 1) {
        unsigned int idx = ((address - gwenesis_sram_start) >> 1) & GWENESIS_SRAM_MASK;
        GWENESIS_SRAM[idx] = value & 0xFF;
        gwenesis_sram_mark_dirty();
      }
    } else {
      unsigned int off = (address - gwenesis_sram_start) & GWENESIS_SRAM_MASK;
      GWENESIS_SRAM[off] = value & 0xFF;
      gwenesis_sram_mark_dirty();
    }
    return;

  case SRAM_CTRL:
    /* 0xA130F1 SRAM control register:
     * bit 0 = 1 -> SRAM active (ROM no longer responds to gwenesis_sram_start..gwenesis_sram_end)
     * bit 1 = 1 -> write protect */
    gwenesis_sram_active        = value & 0x01;
    gwenesis_sram_write_protect = (value >> 1) & 0x01;
    return;

  case SSF2_BANK_CTRL:
    /* SSF2 bankswitching: byte write to 0xA130F3, F5, F7, F9, FB, FD, FF
     * selects which physical 512 KB page maps into logical slots 1-7.
     * Slot 0 (0x000000-0x07FFFF) is always fixed to page 0. */
    if (gwenesis_ssf2_enabled) {
      unsigned int slot = (address - 0xA130F0) >> 1;  /* 1..7 */
      if (slot >= 1 && slot <= 7)
        gwenesis_ssf2_banks[slot] = value & 0x3F;     /* 6 bits per SSF2 spec = up to 32 MB */
    }
    return;

  case IO_CTRL:
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    z80_write_ctrl(address & 0x1FFF, value);
    return;

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    ZRAM[address & 0x1FFF] = value;
    return;

  case Z80_YM2612_ADDR:
    bus_log(__FUNCTION__,"CPUZ80PSG8 ,m68kclk= %d", m68k_cycles_master());
    YM2612Write(address & 0x3, value & 0Xff,m68k_cycles_master());
    return;

  case Z80_SN76489_ADDR:
    bus_log(__FUNCTION__,"CPUZ80FM8  ,m68kclk= %d", m68k_cycles_master());
    gwenesis_SN76489_Write( value & 0Xff, m68k_cycles_master());
    return;

  case Z80_BANK_ADDR:
  //TODO
    return;

  case TMSS_CTRL:

    if (tmss_state == 0) {
      TMSS[address & 0x4] = value;
      tmss_count++;
      if (tmss_count == 4)
        tmss_state = 1;
    }
    return;



  default:
    //printf("write(%x, %x)\n", address, value);
    return;
  }
  return;
}

static inline void gwenesis_bus_write_memory_16(unsigned int address,
                                               unsigned int value) {
  bus_log(__FUNCTION__,"write16  @%x:%x", address,value);

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    gwenesis_vdp_write_memory_16(address, value);
    return;

  case RAM_ADDR:
    WRITE16RAM(address, value);
    return;

  case SRAM_ADDR: {
    if (gwenesis_sram_write_protect) return;
    if (gwenesis_sram_odd_only) {
      /* M68K 16-bit write to even address: high byte→even (ignored), low byte→odd (data) */
      unsigned int idx = ((address - gwenesis_sram_start) >> 1) & GWENESIS_SRAM_MASK;
      GWENESIS_SRAM[idx] = value & 0xFF;
    } else {
      unsigned int off = (address - gwenesis_sram_start) & GWENESIS_SRAM_MASK;
      GWENESIS_SRAM[off]     = (value >> 8) & 0xFF;
      GWENESIS_SRAM[off + 1] = value & 0xFF;
    }
    gwenesis_sram_mark_dirty();
    return;
  }

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    ZRAM[address & 0X1FFF]= value >> 8;
    return;

  case SRAM_CTRL:
    gwenesis_sram_active        = (value >> 8) & 0x01;
    gwenesis_sram_write_protect = (value >> 9) & 0x01;
    return;

  case SSF2_BANK_CTRL:
    /* 16-bit write: the bank number is in the low byte (M68K big-endian).
     * The odd address register captures value & 0xFF. */
    if (gwenesis_ssf2_enabled) {
      unsigned int slot = (address - 0xA130F0) >> 1;
      if (slot >= 1 && slot <= 7)
        gwenesis_ssf2_banks[slot] = (value & 0xFF) & 0x3F;
    }
    return;

  case IO_CTRL:
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    z80_write_ctrl(address & 0xFFFF, value >> 8) ;
    return;

  case Z80_YM2612_ADDR:
    bus_log(__FUNCTION__,"CZYM16 ,mclk=%d",  m68k_cycles_master());
    YM2612Write(address & 0x3, value >> 8,m68k_cycles_master() );
    return;

  case Z80_SN76489_ADDR:
    bus_log(__FUNCTION__,"CZSN16 ,mclk=%d", m68k_cycles_master());
    gwenesis_SN76489_Write(value >> 8,m68k_cycles_master() );
    return;

  default:
    bus_log(__FUNCTION__,"write mem 16 default %x ", address);
    gwenesis_bus_write_memory_8(address, (value >> 8) & 0xff);
    gwenesis_bus_write_memory_8(address + 1, (value)&0xff);

    return;
  }
  return;
}

/******************************************************************************
 *
 *   68K CPU read address R8
 *   Read an address from memory mapped and return value as byte
 *
 ******************************************************************************/
unsigned int m68k_read_memory_8(unsigned int address)
{
    return gwenesis_bus_read_memory_8(address);
}

/******************************************************************************
 *
 *   68K CPU read address R16
 *   Read an address from memory mapped and return value as word
 *
 ******************************************************************************/
 unsigned int m68k_read_memory_16(unsigned int address)
{
    return gwenesis_bus_read_memory_16(address);
}

/******************************************************************************
 *
 *   68K CPU read address R32
 *   Read an address from memory mapped and return value as long
 *
 ******************************************************************************/
 unsigned int m68k_read_memory_32(unsigned int address)
{
  //  if ((address &  0xFF0000 ) == 0xFF0000) return FETCH32RAM(address);
    return (gwenesis_bus_read_memory_16(address) << 16) | gwenesis_bus_read_memory_16(address + 2);
}

/******************************************************************************
 *
 *   68K CPU write address W8
 *   Write an value as byte to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_8(unsigned int address, unsigned int value) {
  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE8RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_8(address, value);
  return;
}

/******************************************************************************
 *
 *   68K CPU write address W16
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_16(unsigned int address, unsigned int value) {
  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE16RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_16(address, value);
  return;
}
/******************************************************************************
 *
 *   68K CPU write address W32
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_32(unsigned int address, unsigned int value) {

  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE32RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_16(address, (value >> 16) & 0xffff);
  gwenesis_bus_write_memory_16(address + 2, (value)&0xffff);

  return;
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
    return m68k_read_memory_16(address);
}
unsigned int m68k_read_disassembler_32(unsigned int address)
{
    return m68k_read_memory_32(address);
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

void gwenesis_bus_load_state(FILE *file) {
  fread((unsigned char *)M68K_RAM, MAX_RAM_SIZE, 1, file);
  fread((unsigned char *)ZRAM, MAX_Z80_RAM_SIZE, 1, file);
  fread((unsigned char *)TMSS, sizeof(TMSS), 1, file);
  fread((unsigned char *)&tmss_state, 4, 1, file);
  fread((unsigned char *)&tmss_count, 4, 1, file);
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
