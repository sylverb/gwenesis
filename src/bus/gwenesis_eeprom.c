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

I2C serial EEPROM (24Cxx) support ported from Genesis Plus GX
(core/cart_hw/eeprom_i2c.c, Copyright (C) 2007-2026 Eke-Eke).  The I2C state
machine is reproduced as-is; only the host integration (memory map, EEPROM
storage, game detection inputs) is adapted to the Gwenesis bus.
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "m68k.h"
#include "gwenesis_bus.h"
#include "gwenesis_sram.h"
#include "gwenesis_eeprom.h"

int gwenesis_eeprom_enabled = 0;

/* EEPROM data array lives in the cartridge backup buffer so that the existing
 * gwenesis_sram_load()/save() battery persistence is reused unchanged. */
#define EEPROM_MEM(addr) (GWENESIS_SRAM[(addr) & GWENESIS_SRAM_MASK])

/* ROM read handlers exported by gwenesis_bus.c (used by the Acclaim 32M board
 * to switch the $200000-$2FFFFF window back to cartridge ROM). */
extern unsigned int gwenesis_bus_rom_read8(unsigned int address);
extern unsigned int gwenesis_bus_rom_read16(unsigned int address);

/* ------------------------------------------------------------------------- */
/* I2C EEPROM types & specifications (identical to GPGX)                      */
/* ------------------------------------------------------------------------- */

typedef enum
{
  STAND_BY = 0,
  WAIT_STOP,
  GET_DEVICE_ADR,
  GET_WORD_ADR_7BITS,
  GET_WORD_ADR_HIGH,
  GET_WORD_ADR_LOW,
  WRITE_DATA,
  READ_DATA
} T_I2C_STATE;

typedef enum
{
  NO_EEPROM = -1,
  EEPROM_X24C01,
  EEPROM_X24C02,
  EEPROM_24C01,
  EEPROM_24C02,
  EEPROM_24C04,
  EEPROM_24C08,
  EEPROM_24C16,
  EEPROM_24C32,
  EEPROM_24C64,
  EEPROM_24C65,
  EEPROM_24C128,
  EEPROM_24C256,
  EEPROM_24C512
} T_I2C_TYPE;

typedef struct
{
  uint8_t  address_bits;
  uint16_t size_mask;
  uint16_t pagewrite_mask;
} T_I2C_SPEC;

static const T_I2C_SPEC i2c_specs[] =
{
  { 7 , 0x7F   , 0x03},
  { 8 , 0xFF   , 0x03},
  { 8 , 0x7F   , 0x07},
  { 8 , 0xFF   , 0x07},
  { 8 , 0x1FF  , 0x0F},
  { 8 , 0x3FF  , 0x0F},
  { 8 , 0x7FF  , 0x0F},
  {16 , 0xFFF  , 0x1F},
  {16 , 0x1FFF , 0x1F},
  {16 , 0x1FFF , 0x3F},
  {16 , 0x3FFF , 0x3F},
  {16 , 0x7FFF , 0x3F},
  {16 , 0xFFFF , 0x7F}
};

/* Board mappers supported by Gwenesis. */
typedef enum
{
  MAPPER_NONE = 0,
  MAPPER_EA,
  MAPPER_SEGA,
  MAPPER_ACCLAIM_16M,
  MAPPER_ACCLAIM_32M,
  MAPPER_JCART
} T_I2C_MAPPER;

typedef struct
{
  char         id[16];
  uint32_t     sp;
  uint16_t     chk;
  T_I2C_MAPPER mapper;
  T_I2C_TYPE   eeprom_type;
} T_I2C_GAME;

static const T_I2C_GAME i2c_database[] =
{
  {"T-50176"  , 0          , 0      , MAPPER_EA          , EEPROM_X24C01 }, /* Rings of Power */
  {"T-50396"  , 0          , 0      , MAPPER_EA          , EEPROM_X24C01 }, /* NHLPA Hockey 93 */
  {"T-50446"  , 0          , 0      , MAPPER_EA          , EEPROM_X24C01 }, /* John Madden Football 93 */
  {"T-50516"  , 0          , 0      , MAPPER_EA          , EEPROM_X24C01 }, /* John Madden Football 93 (Championship Ed.) */
  {"T-50606"  , 0          , 0      , MAPPER_EA          , EEPROM_X24C01 }, /* Bill Walsh College Football (invalid SRAM header) */
  {" T-12046" , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Megaman - The Wily Wars (SRAM hack exists) */
  {" T-12053" , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Rockman Mega World (SRAM hack exists) */
  {"MK-1215"  , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Evander 'Real Deal' Holyfield's Boxing */
  {"MK-1228"  , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Greatest Heavyweights of the Ring (U)(E) */
  {"G-5538"   , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Greatest Heavyweights of the Ring (J) */
  {"PR-1993"  , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Greatest Heavyweights of the Ring (Prototype) */
  {" G-4060"  , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Wonderboy in Monster World (SRAM hack exists) */
  {"00001211" , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Sports Talk Baseball */
  {"00004076" , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Honoo no Toukyuuji Dodge Danpei */
  {"G-4524"   , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Ninja Burai Densetsu */
  {"00054503" , 0          , 0      , MAPPER_SEGA        , EEPROM_X24C01 }, /* Game Toshokan */
  {"T-81033"  , 0          , 0      , MAPPER_ACCLAIM_16M , EEPROM_X24C02 }, /* NBA Jam (J) */
  {"T-081326" , 0          , 0      , MAPPER_ACCLAIM_16M , EEPROM_X24C02 }, /* NBA Jam (UE) */
  {"T-081276" , 0          , 0      , MAPPER_ACCLAIM_32M , EEPROM_24C02  }, /* NFL Quarterback Club */
  {"T-81406"  , 0          , 0      , MAPPER_ACCLAIM_32M , EEPROM_24C04  }, /* NBA Jam TE */
  {"T-081586" , 0          , 0      , MAPPER_ACCLAIM_32M , EEPROM_24C16  }, /* NFL Quarterback Club '96 */
  {"T-81476"  , 0          , 0      , MAPPER_ACCLAIM_32M , EEPROM_24C65  }, /* Frank Thomas Big Hurt Baseball */
  {"T-81576"  , 0          , 0      , MAPPER_ACCLAIM_32M , EEPROM_24C65  }, /* College Slam */
  {"T-120106" , 0          , 0      , MAPPER_JCART       , EEPROM_24C08  }, /* Brian Lara Cricket */
  {"00000000" , 0x444e4c44 , 0x168B , MAPPER_JCART       , EEPROM_24C08  }, /* Micro Machines Military */
  {"00000000" , 0x444e4c44 , 0x165E , MAPPER_JCART       , EEPROM_24C16  }, /* Micro Machines Turbo Tournament 96 */
  {"T-120096" , 0          , 0      , MAPPER_JCART       , EEPROM_24C16  }, /* Micro Machines 2 - Turbo Tournament */
  {"T-120146" , 0          , 0      , MAPPER_JCART       , EEPROM_24C65  }, /* Brian Lara Cricket 96 / Shane Warne Cricket */
  {"00000000" , 0xfffffffc , 0x168B , MAPPER_JCART       , NO_EEPROM     }, /* Super Skidmarks */
  {"00000000" , 0xfffffffc , 0x165E , MAPPER_JCART       , NO_EEPROM     }, /* Pete Sampras Tennis (Prototype) */
  {"T-120066" , 0          , 0      , MAPPER_JCART       , NO_EEPROM     }, /* Pete Sampras Tennis */
  {"T-123456" , 0          , 0      , MAPPER_JCART       , NO_EEPROM     }, /* Pete Sampras Tennis 96 */
  {"XXXXXXXX" , 0          , 0xDF39 , MAPPER_JCART       , NO_EEPROM     }, /* Pete Sampras Tennis 96 (Prototype ?) */
};

static struct
{
  uint8_t  sda;             /* current SDA line state */
  uint8_t  scl;             /* current SCL line state */
  uint8_t  old_sda;         /* previous SDA line state */
  uint8_t  old_scl;         /* previous SCL line state */
  uint8_t  cycles;          /* operation internal cycle (0-9) */
  uint8_t  rw;              /* operation type (1:READ, 0:WRITE) */
  uint16_t device_address;  /* device address */
  uint16_t word_address;    /* memory address */
  uint8_t  buffer;          /* write buffer */
  T_I2C_STATE state;        /* current operation state */
  T_I2C_SPEC  spec;         /* EEPROM characteristics */
  uint8_t  scl_in_bit;      /* SCL (write) bit position */
  uint8_t  sda_in_bit;      /* SDA (write) bit position */
  uint8_t  sda_out_bit;     /* SDA (read) bit position */
} eeprom_i2c;

/* Board mapper selected at detection time, installed in install_memory_map(). */
static T_I2C_MAPPER eeprom_i2c_mapper = MAPPER_NONE;

/********************************************************************/
/* I2C EEPROM state machine (1:1 with Genesis Plus GX)              */
/********************************************************************/

static inline void Detect_START(void)
{
  if (eeprom_i2c.old_scl && eeprom_i2c.scl)
  {
    if (eeprom_i2c.old_sda && !eeprom_i2c.sda)
    {
      eeprom_i2c.cycles = 0;

      if (eeprom_i2c.spec.address_bits == 7)
      {
        eeprom_i2c.word_address = 0;
        eeprom_i2c.state = GET_WORD_ADR_7BITS;
      }
      else
      {
        eeprom_i2c.device_address = 0;
        eeprom_i2c.state = GET_DEVICE_ADR;
      }
    }
  }
}

static inline void Detect_STOP(void)
{
  if (eeprom_i2c.old_scl && eeprom_i2c.scl)
  {
    if (!eeprom_i2c.old_sda && eeprom_i2c.sda)
    {
      eeprom_i2c.state = STAND_BY;
    }
  }
}

static void eeprom_i2c_update(void)
{
  switch (eeprom_i2c.state)
  {
    case STAND_BY:
    {
      Detect_START();
      break;
    }

    case WAIT_STOP:
    {
      Detect_STOP();
      break;
    }

    case GET_WORD_ADR_7BITS:
    {
      Detect_START();
      Detect_STOP();

      if (eeprom_i2c.old_scl && !eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          eeprom_i2c.cycles++;
        }
        else
        {
          eeprom_i2c.cycles = 1;
          eeprom_i2c.state = eeprom_i2c.rw ? READ_DATA : WRITE_DATA;
          eeprom_i2c.buffer = 0x00;
        }
      }
      else if (!eeprom_i2c.old_scl && eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 8)
        {
          eeprom_i2c.word_address |= (eeprom_i2c.sda << (7 - eeprom_i2c.cycles));
        }
        else if (eeprom_i2c.cycles == 8)
        {
          eeprom_i2c.rw = eeprom_i2c.sda;
        }
      }

      break;
    }

    case GET_DEVICE_ADR:
    {
      Detect_START();
      Detect_STOP();

      if (eeprom_i2c.old_scl && !eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          eeprom_i2c.cycles++;
        }
        else
        {
          eeprom_i2c.device_address <<= eeprom_i2c.spec.address_bits;

          eeprom_i2c.cycles = 1;
          if (eeprom_i2c.rw)
          {
            eeprom_i2c.state = READ_DATA;
          }
          else
          {
            eeprom_i2c.word_address = 0;
            eeprom_i2c.state = (eeprom_i2c.spec.address_bits == 16) ? GET_WORD_ADR_HIGH : GET_WORD_ADR_LOW;
          }
        }
      }
      else if (!eeprom_i2c.old_scl && eeprom_i2c.scl)
      {
        if ((eeprom_i2c.cycles > 4) && (eeprom_i2c.cycles < 8))
        {
          eeprom_i2c.device_address |= (eeprom_i2c.sda << (7 - eeprom_i2c.cycles));
        }
        else if (eeprom_i2c.cycles == 8)
        {
          eeprom_i2c.rw = eeprom_i2c.sda;
        }
      }

      break;
    }

    case GET_WORD_ADR_HIGH:
    {
      Detect_START();
      Detect_STOP();

      if (eeprom_i2c.old_scl && !eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          eeprom_i2c.cycles++;
        }
        else
        {
          eeprom_i2c.cycles = 1;
          eeprom_i2c.state = GET_WORD_ADR_LOW;
        }
      }
      else if (!eeprom_i2c.old_scl && eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          if (eeprom_i2c.spec.size_mask < (1 << (16 - eeprom_i2c.cycles)))
          {
            eeprom_i2c.device_address >>= 1;
          }
          else
          {
            eeprom_i2c.word_address |= (eeprom_i2c.sda << (16 - eeprom_i2c.cycles));
          }
        }
      }

      break;
    }

    case GET_WORD_ADR_LOW:
    {
      Detect_START();
      Detect_STOP();

      if (eeprom_i2c.old_scl && !eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          eeprom_i2c.cycles++;
        }
        else
        {
          eeprom_i2c.cycles = 1;
          eeprom_i2c.state = WRITE_DATA;
          eeprom_i2c.buffer = 0x00;
        }
      }
      else if (!eeprom_i2c.old_scl && eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          if (eeprom_i2c.spec.size_mask < (1 << (8 - eeprom_i2c.cycles)))
          {
            eeprom_i2c.device_address >>= 1;
          }
          else
          {
            eeprom_i2c.word_address |= (eeprom_i2c.sda << (8 - eeprom_i2c.cycles));
          }
        }
      }

      break;
    }

    case READ_DATA:
    {
      Detect_START();
      Detect_STOP();

      if (eeprom_i2c.old_scl && !eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          eeprom_i2c.cycles++;
        }
        else
        {
          eeprom_i2c.cycles = 1;
        }
      }
      else if (!eeprom_i2c.old_scl && eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles == 9)
        {
          if (eeprom_i2c.sda)
          {
            eeprom_i2c.state = WAIT_STOP;
          }
          else
          {
            eeprom_i2c.word_address = (eeprom_i2c.word_address + 1) & eeprom_i2c.spec.size_mask;
          }
        }
      }

      break;
    }

    case WRITE_DATA:
    {
      Detect_START();
      Detect_STOP();

      if (eeprom_i2c.old_scl && !eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          eeprom_i2c.cycles++;
        }
        else
        {
          eeprom_i2c.cycles = 1;
        }
      }
      else if (!eeprom_i2c.old_scl && eeprom_i2c.scl)
      {
        if (eeprom_i2c.cycles < 9)
        {
          eeprom_i2c.buffer |= (eeprom_i2c.sda << (8 - eeprom_i2c.cycles));
        }
        else
        {
          EEPROM_MEM(eeprom_i2c.device_address | eeprom_i2c.word_address) = eeprom_i2c.buffer;
          gwenesis_sram_mark_dirty();

          eeprom_i2c.buffer = 0;

          eeprom_i2c.word_address = (eeprom_i2c.word_address & ~eeprom_i2c.spec.pagewrite_mask) |
                                    ((eeprom_i2c.word_address + 1) & eeprom_i2c.spec.pagewrite_mask);
        }
      }

      break;
    }
  }

  eeprom_i2c.old_scl = eeprom_i2c.scl;
  eeprom_i2c.old_sda = eeprom_i2c.sda;
}

static uint8_t eeprom_i2c_out(void)
{
  if (eeprom_i2c.state == READ_DATA)
  {
    if (eeprom_i2c.cycles < 9)
    {
      return ((EEPROM_MEM(eeprom_i2c.device_address | eeprom_i2c.word_address) >> (8 - eeprom_i2c.cycles)) & 1);
    }
  }
  else if (eeprom_i2c.cycles == 9)
  {
    return 0;
  }

  return eeprom_i2c.sda;
}

/********************************************************************/
/* Common I2C board memory mapping                                  */
/********************************************************************/

static unsigned int mapper_i2c_generic_read8(unsigned int address)
{
  if (address & 0x01)
  {
    return eeprom_i2c_out() << eeprom_i2c.sda_out_bit;
  }
  return 0xFF; /* even byte: open bus */
}

static unsigned int mapper_i2c_generic_read16(unsigned int address)
{
  (void)address;
  return eeprom_i2c_out() << eeprom_i2c.sda_out_bit;
}

static void mapper_i2c_generic_write8(unsigned int address, unsigned int data)
{
  if (address & 0x01)
  {
    eeprom_i2c.sda = (data >> eeprom_i2c.sda_in_bit) & 1;
    eeprom_i2c.scl = (data >> eeprom_i2c.scl_in_bit) & 1;
    eeprom_i2c_update();
  }
}

static void mapper_i2c_generic_write16(unsigned int address, unsigned int data)
{
  (void)address;
  eeprom_i2c.sda = (data >> eeprom_i2c.sda_in_bit) & 1;
  eeprom_i2c.scl = (data >> eeprom_i2c.scl_in_bit) & 1;
  eeprom_i2c_update();
}

/********************************************************************/
/* EA mapper (PWA P10003 & P10004 boards)                           */
/********************************************************************/

static void mapper_i2c_ea_init(void)
{
  int i;

  for (i = 0x20; i < 0x40; i++)
  {
    m68k.memory_map[i].read8   = mapper_i2c_generic_read8;
    m68k.memory_map[i].read16  = mapper_i2c_generic_read16;
    m68k.memory_map[i].write8  = mapper_i2c_generic_write8;
    m68k.memory_map[i].write16 = mapper_i2c_generic_write16;
  }

  /* SCL (in) -> D6, SDA (in/out) -> D7 */
  eeprom_i2c.scl_in_bit  = 6;
  eeprom_i2c.sda_in_bit  = 7;
  eeprom_i2c.sda_out_bit = 7;
}

/********************************************************************/
/* SEGA mapper (171-5878, 171-6111, 171-6304 & 171-6584 boards)     */
/********************************************************************/

static void mapper_i2c_sega_init(void)
{
  int i;

  for (i = 0x20; i < 0x40; i++)
  {
    m68k.memory_map[i].read8   = mapper_i2c_generic_read8;
    m68k.memory_map[i].read16  = mapper_i2c_generic_read16;
    m68k.memory_map[i].write8  = mapper_i2c_generic_write8;
    m68k.memory_map[i].write16 = mapper_i2c_generic_write16;
  }

  /* SCL (in) -> D1, SDA (in/out) -> D0 */
  eeprom_i2c.scl_in_bit  = 1;
  eeprom_i2c.sda_in_bit  = 0;
  eeprom_i2c.sda_out_bit = 0;
}

/********************************************************************/
/* ACCLAIM 16M mapper (P/N 670120 board)                            */
/********************************************************************/

static void mapper_i2c_acclaim_16M_init(void)
{
  int i;

  for (i = 0x20; i < 0x40; i++)
  {
    /* /LWR & /UWR are unused: byte writes behave like word writes */
    m68k.memory_map[i].read8   = mapper_i2c_generic_read8;
    m68k.memory_map[i].read16  = mapper_i2c_generic_read16;
    m68k.memory_map[i].write8  = mapper_i2c_generic_write16;
    m68k.memory_map[i].write16 = mapper_i2c_generic_write16;
  }

  /* SCL (in) & SDA (out) -> D1, SDA (in) -> D0 */
  eeprom_i2c.scl_in_bit  = 1;
  eeprom_i2c.sda_in_bit  = 0;
  eeprom_i2c.sda_out_bit = 1;
}

/********************************************************************/
/* ACCLAIM 32M mapper (P/N 670125 & 670127 boards with LZ95A53 PAL) */
/********************************************************************/

static void mapper_acclaim_32M_write8(unsigned int address, unsigned int data)
{
  if (address & 0x01)
  {
    /* D0 -> /SDA when only /LWR is asserted */
    eeprom_i2c.sda = data & 1;
  }
  else
  {
    /* D0 -> /SCL when only /UWR is asserted */
    eeprom_i2c.scl = data & 1;
  }

  eeprom_i2c_update();
}

static void mapper_acclaim_32M_write16(unsigned int address, unsigned int data)
{
  int i;
  (void)address;

  /* custom bankshifting when both /LWR and /UWR are asserted */
  if (data & 0x01)
  {
    /* cartridge ROM (read) mapped to $200000-$2fffff */
    for (i = 0x20; i < 0x30; i++)
    {
      m68k.memory_map[i].read8  = gwenesis_bus_rom_read8;
      m68k.memory_map[i].read16 = gwenesis_bus_rom_read16;
    }
  }
  else
  {
    /* serial EEPROM (read) mapped to $200000-$2fffff */
    for (i = 0x20; i < 0x30; i++)
    {
      m68k.memory_map[i].read8  = mapper_i2c_generic_read8;
      m68k.memory_map[i].read16 = mapper_i2c_generic_read16;
    }
  }
}

static void mapper_i2c_acclaim_32M_init(void)
{
  int i;

  /* custom LZ95A53 PAL (write) mapped to $200000-$2fffff */
  for (i = 0x20; i < 0x30; i++)
  {
    m68k.memory_map[i].write8  = mapper_acclaim_32M_write8;
    m68k.memory_map[i].write16 = mapper_acclaim_32M_write16;
  }

  /* power-on state: $200000-$2FFFFF reads cartridge ROM (bankshift = 1) */
  for (i = 0x20; i < 0x30; i++)
  {
    m68k.memory_map[i].read8  = gwenesis_bus_rom_read8;
    m68k.memory_map[i].read16 = gwenesis_bus_rom_read16;
  }

  /* SCL (in) & SDA (in/out) -> D0 */
  eeprom_i2c.scl_in_bit  = 0;
  eeprom_i2c.sda_in_bit  = 0;
  eeprom_i2c.sda_out_bit = 0;
}

/********************************************************************/
/* CODEMASTERS mapper (J-CART boards)                               */
/*                                                                  */
/* Gwenesis does not implement the J-CART third/fourth controller   */
/* ports, so only the serial EEPROM portion of the board is mapped. */
/* The extra controller reads return "no buttons pressed".          */
/********************************************************************/

static unsigned int jcart_read_stub(unsigned int address)
{
  (void)address;
  return 0x7F7F; /* J-CART pads not connected: all lines high */
}

static void jcart_write_stub(unsigned int address, unsigned int data)
{
  (void)address; (void)data;
}

static unsigned int mapper_i2c_jcart_read8(unsigned int address)
{
  if (address & 0x01)
  {
    return ((eeprom_i2c_out() << 7) | (jcart_read_stub(address) & 0x7f));
  }
  return (jcart_read_stub(address) >> 8);
}

static unsigned int mapper_i2c_jcart_read16(unsigned int address)
{
  return ((eeprom_i2c_out() << 7) | jcart_read_stub(address));
}

static void mapper_i2c_jcart_init(void)
{
  int i;

  /* serial EEPROM (write) mapped to $300000-$37ffff (when EEPROM present) */
  if (eeprom_i2c.spec.size_mask != 0)
  {
    for (i = 0x30; i < 0x38; i++)
    {
      m68k.memory_map[i].write8  = mapper_i2c_generic_write16;
      m68k.memory_map[i].write16 = mapper_i2c_generic_write16;
    }
  }

  /* serial EEPROM (read) & J-CART (read/write) mapped to $380000-$3fffff */
  for (i = 0x38; i < 0x40; i++)
  {
    m68k.memory_map[i].read8   = mapper_i2c_jcart_read8;
    m68k.memory_map[i].read16  = mapper_i2c_jcart_read16;
    m68k.memory_map[i].write8  = jcart_write_stub;
    m68k.memory_map[i].write16 = jcart_write_stub;
  }

  /* SCL (in) -> D1, SDA (in) -> D0, SDA (out) -> D7 */
  eeprom_i2c.scl_in_bit  = 1;
  eeprom_i2c.sda_in_bit  = 0;
  eeprom_i2c.sda_out_bit = 7;
}

/********************************************************************/
/* Detection & memory-map installation                             */
/********************************************************************/

int gwenesis_eeprom_detect(const char *product, uint16_t checksum,
                           uint32_t rom_first_long,
                           int sram_detected,
                           uint32_t sram_start, uint32_t sram_end,
                           uint8_t header_type)
{
  int i = (int)(sizeof(i2c_database) / sizeof(T_I2C_GAME)) - 1;

  gwenesis_eeprom_enabled = 0;
  eeprom_i2c_mapper = MAPPER_NONE;

  /* initialize I2C EEPROM state */
  memset(&eeprom_i2c, 0, sizeof(eeprom_i2c));
  eeprom_i2c.sda = eeprom_i2c.old_sda = 1;
  eeprom_i2c.scl = eeprom_i2c.old_scl = 1;
  eeprom_i2c.state = STAND_BY;

  /* auto-detect games listed in database */
  do
  {
    if (strstr(product, i2c_database[i].id))
    {
      /* known SRAM-patched hacks keep a parallel SRAM mapping instead */
      if ((i2c_database[i].id[0] == ' ') && ((sram_end - sram_start) > 2))
      {
        break;
      }

      /* additional check for Codemasters games (checksum / first ROM long) */
      if (((i2c_database[i].chk == 0) || (i2c_database[i].chk == checksum)) &&
          ((i2c_database[i].sp == 0) || (i2c_database[i].sp == rom_first_long)))
      {
        if (i2c_database[i].eeprom_type > NO_EEPROM)
        {
          memcpy(&eeprom_i2c.spec, &i2c_specs[i2c_database[i].eeprom_type], sizeof(T_I2C_SPEC));
          gwenesis_eeprom_enabled = 1;
        }

        eeprom_i2c_mapper = i2c_database[i].mapper;
        break;
      }
    }
  }
  while (i--);

  /* games not in the database: fall back to the ROM header hint */
  if (!gwenesis_eeprom_enabled && sram_detected && (eeprom_i2c_mapper == MAPPER_NONE))
  {
    if ((header_type == 0xE8) || ((sram_end - sram_start) < 2))
    {
      memcpy(&eeprom_i2c.spec, &i2c_specs[EEPROM_X24C01], sizeof(T_I2C_SPEC));
      gwenesis_eeprom_enabled = 1;
      eeprom_i2c_mapper = MAPPER_SEGA;
    }
  }

  return gwenesis_eeprom_enabled;
}

void gwenesis_eeprom_install_memory_map(void)
{
  switch (eeprom_i2c_mapper)
  {
    case MAPPER_EA:          mapper_i2c_ea_init();          break;
    case MAPPER_SEGA:        mapper_i2c_sega_init();        break;
    case MAPPER_ACCLAIM_16M: mapper_i2c_acclaim_16M_init(); break;
    case MAPPER_ACCLAIM_32M: mapper_i2c_acclaim_32M_init(); break;
    case MAPPER_JCART:       mapper_i2c_jcart_init();       break;
    case MAPPER_NONE:
    default:
      break;
  }
}

uint32_t gwenesis_eeprom_size(void)
{
  if (!gwenesis_eeprom_enabled)
    return 0;
  return (uint32_t)eeprom_i2c.spec.size_mask + 1u;
}

void gwenesis_eeprom_save_state(FILE *file)
{
  fwrite(&eeprom_i2c, sizeof(eeprom_i2c), 1, file);
}

void gwenesis_eeprom_load_state(FILE *file)
{
  fread(&eeprom_i2c, sizeof(eeprom_i2c), 1, file);
}
