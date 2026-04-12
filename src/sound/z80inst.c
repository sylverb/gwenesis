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
#include <assert.h>
#include "Z80.h"
#include "z80inst.h"
#include "m68k.h"
#include "gwenesis_bus.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_savestate.h"

#ifdef TARGET_GNW
  #pragma GCC optimize("Ofast")
#endif

static int bus_ack = 0;
static int reset = 0;
int zclk = 0;
static int initialized = 0;

/* Z80 core FAST_RDOP expects 8 pages of 0x2000 bytes. */
unsigned char *Z80_RAM[8];
static uint8_t z80_dummy[0x2000];
static unsigned char *Z80_RAM_BASE;

static Z80 cpu;

void ResetZ80(register Z80 *R);

#define Z80_INST_DISABLE_LOGGING 1

#if !Z80_INST_DISABLE_LOGGING
#include <stdarg.h>
void z80_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s]", frame_counter, scan_line, subs);

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define z80_log(...)  do {} while(0)
#endif

// Bank register used by Z80 to access M68K Memory space 1 BANK=32KByte
int Z80_BANK;


void z80_start() {
    cpu.IPeriod = 1;
    cpu.ICount = 0;
    cpu.Trace = 0;
    cpu.Trap = 0x0009;
    ResetZ80(&cpu);
    reset=1;
    bus_ack=0;
    zclk=0;
    memset(z80_dummy, 0xFF, sizeof(z80_dummy));
}

void z80_pulse_reset() {
  Z80_BANK = 0;
  ResetZ80(&cpu);
}
static int current_timeslice = 0;

void z80_run(int target) {

  // we are in advance,nothind to do
current_timeslice = 0;
  if (zclk >= target) {
 // z80_log("z80_skip time","%1d%1d||zclk=%d,tgt=%d",bus_ack,reset, zclk, target);
    return;
  }

  current_timeslice = target - zclk;

  int rem = 0;
  if ((bus_ack == 0) && (reset == 0)) {

   // z80_log("z80_run", "%1d%1d||zclk=%d,tgt=%d", bus_ack, reset, zclk, target);
    rem = ExecZ80(&cpu, current_timeslice / Z80_FREQ_DIVISOR);

  }

  zclk = target - rem * Z80_FREQ_DIVISOR;
}

void z80_sync(void) {
  /*
  get M68K cycles 
  Execute cycles on z80 to sync with m68K
  */

  z80_run(m68k_cycles_master());
}

void z80_set_memory(unsigned char *buffer)
{
    Z80_RAM_BASE = buffer;

    // 0x0000–0x1FFF → RAM
    Z80_RAM[0] = buffer;

    // 0x2000–0x3FFF → mirror RAM
    Z80_RAM[1] = buffer;
    
    // 0x4000–0xFFFF → NOT RAM → dummy (handled by RdZ80/WrZ80)
    for (int i = 2; i < 8; i++) {
      Z80_RAM[i] = z80_dummy;
    }
    initialized = 1;
}

void z80_write_ctrl(unsigned int address, unsigned int value) {
  z80_sync();

  if (address == 0x1100) // BUSREQ
  {
    z80_log(__FUNCTION__,"BUSREQ = %d, current=%d", value,bus_ack);

    // Bus request. Z80 bus on hold.
    if (value) {
      bus_ack = 1;


    // Bus request cancel. Z80 runs.
    } else {
            bus_ack = 0;
    }

  } else if (address == 0x1200) // RESET
  {
    z80_log(__FUNCTION__,"RESET = %d, current=%d", value,reset);

    if (value == 0) {
      reset = 1;
    } else {
      /* Real hardware: reset pulse occurs on 0->1 transition only. */
      if (reset) {
        z80_pulse_reset();
      }
      reset = 0;
    }
  }
}

unsigned int z80_read_ctrl(unsigned int address) {

  z80_sync();

  if (address == 0x1100) {

    /* BUSACK is asserted only when bus is requested and Z80 is not held in reset. */
    unsigned int busack = (bus_ack == 1 && reset == 0) ? 0 : 1;
    z80_log(__FUNCTION__,"RUNNING = %d ", busack);
    return busack;

  } else if (address == 0x1101) {
    return 0x00;

  } else if (address == 0x1200) {

    z80_log(__FUNCTION__,"RESET = %d ", reset );
    return reset;

  } else if (address == 0x1201) {
    return 0x00;
  }
  return 0xFF;
}

void z80_irq_line(unsigned int value)
{
    if (reset) return;

    if (value)
        cpu.IRequest = INT_IRQ;
    else
        cpu.IRequest = INT_NONE;

    z80_log(__FUNCTION__,"Interrupt = %d ", value);

}

#if 0

word z80_get_reg(int reg_i) {
    switch(reg_i) {
        case 0: return cpu.AF.W; break;
        case 1: return cpu.BC.W; break;
        case 2: return cpu.DE.W; break;
        case 3: return cpu.HL.W; break;
        case 4: return cpu.IX.W; break;
        case 5: return cpu.IY.W; break;
        case 6: return cpu.PC.W; break;
        case 7: return cpu.SP.W; break;
    }
}
#endif

/********************************************
 * Z80 Bank
 ********************************************/

unsigned int zbankreg_mem_r8(unsigned int address)
{
      z80_log(__FUNCTION__,"Z80 bank read pointer : %06x", Z80_BANK);

    return Z80_BANK;
}

static inline void zbankreg_mem_w8(unsigned int value) {
  Z80_BANK >>= 1;
  Z80_BANK |= (value & 1) << 8;
  z80_log(__FUNCTION__,"Z80 bank points to: %06x", Z80_BANK << 15);
  return;
}

/* Exported wrapper — called from gwenesis_bus.c 0xA06000 write handler */
void z80_bank_register_write(unsigned int value)
{
  zbankreg_mem_w8(value);
}

static inline unsigned int zbank_mem_r8(unsigned int address)
{
    address &= 0x7FFF;
    address |= (Z80_BANK << 15);

    z80_log(__FUNCTION__,"Z80 bank read: %06x", address);
    cpu_memory_map *m = &m68k.memory_map[(address >> 16) & 0xFF];
    if (m->read8) return (*m->read8)(address & 0xFFFFFF);
    if (m->base)  return READ_BYTE(m->base, address & 0xFFFF);
    return 0xFF;
}

static inline void zbank_mem_w8(unsigned int address, unsigned int value) {
  address &= 0x7FFF;
  address |= (Z80_BANK << 15);

  z80_log(__FUNCTION__,"Z80 bank write %06x: %02x", address, value);
  cpu_memory_map *m = &m68k.memory_map[(address >> 16) & 0xFF];
  if (m->write8) (*m->write8)(address & 0xFFFFFF, value);
  else if (m->base) WRITE_BYTE(m->base, address & 0xFFFF, value);
}

// TODO ??
/*
unsigned int zvdp_mem_r8(unsigned int address)
{
    if (address >= 0x7F00 && address < 0x7F20)
        return vdp_mem_r8(address);
    return 0xFF;
}

void zvdp_mem_w8(unsigned int address, unsigned int value)
{
    if (address >= 0x7F00 && address < 0x7F20)
        vdp_mem_w8(address, value);
}

*/

word LoopZ80(register Z80 *R)
{
    return 0;
}

byte RdZ80(register word Addr) {
  switch((Addr >> 13) & 7)
  {
    case 0: /* $0000-$3FFF: Z80 RAM (8K mirrored) */
    case 1:
    {
      return Z80_RAM_BASE[Addr & 0x1FFF];
    }

    case 2: /* $4000-$5FFF: YM2612 */
    {
      return YM2612Read(zclk + current_timeslice - (cpu.ICount * Z80_FREQ_DIVISOR));
    }

    case 3: /* $6000-$7FFF: bank register / PSG (write-only), open bus on read */
    {
      // $6000-$60FF: Bank register which is write-only
      // $7F00-$7FFF: VDP which should be accessible but
      //              no game is accessing VDP from Z80 side
      return 0xFF;
    }

    default: /* $8000-$FFFF: 68k bank (32K) */
    {
      return zbank_mem_r8(Addr);
    }
  }
}

extern int system_clock;

void WrZ80(register word Addr, register byte Value) {
  switch((Addr >> 13) & 7)
  {
    case 0: /* $0000-$3FFF: Z80 RAM (8K mirrored) */
    case 1:
      Z80_RAM_BASE[Addr&0x1FFF] = Value;
      break;
    case 2: /* $4000-$5FFF: YM2612 */
      z80_log("Z80","ZZYM(%x,%x) zk=%d,tgt=%d",Addr&0x3,Value, zclk, zclk + current_timeslice -(cpu.ICount * Z80_FREQ_DIVISOR) );
      YM2612Write(Addr&0x3, Value, zclk + current_timeslice -(cpu.ICount * Z80_FREQ_DIVISOR) );
      break;
    case 3: /* Bank register and VDP */
      switch(Addr >> 8)
      {
        case 0x60: /* $6000-$60FF: Bank register */
        {
          zbankreg_mem_w8(Value);
          return;
        }

        case 0x7F: /* $7F00-$7FFF: VDP */
        {
          z80_log("Z80","ZZSN zk=%d,tgt=%d", zclk, zclk + current_timeslice -(cpu.ICount * Z80_FREQ_DIVISOR) );
          gwenesis_SN76489_Write(Value,zclk + current_timeslice -(cpu.ICount * Z80_FREQ_DIVISOR) );
          return;
        }

        default:
        {
          return;
        }
      }
      break;
    default: /* $8000-$FFFF: 68k bank (32K) */
    {
      zbank_mem_w8(Addr, Value);
      return;
    }
  }
}


byte InZ80(register word Port) {return 0;}
void OutZ80(register word Port, register byte Value) {;}
void PatchZ80(register Z80 *R) {;}
void DebugZ80(register Z80 *R) {;}

void gwenesis_z80inst_save_state(FILE *file) {
    fwrite((unsigned char *)&cpu, sizeof(Z80), 1, file);

    fwrite((unsigned char *)&bus_ack, 4, 1, file);
    fwrite((unsigned char *)&reset, 4, 1, file);
    fwrite((unsigned char *)&zclk, 4, 1, file);
    fwrite((unsigned char *)&initialized, 4, 1, file);
    fwrite((unsigned char *)&Z80_BANK, 4, 1, file);
    fwrite((unsigned char *)&current_timeslice, 4, 1, file);
}

void gwenesis_z80inst_load_state(FILE *file, int ss_version) {
    fread((unsigned char *)&cpu, sizeof(Z80), 1, file);

    fread((unsigned char *)&bus_ack, 4, 1, file);
    fread((unsigned char *)&reset, 4, 1, file);
    if (ss_version == 0) {
      uint32_t dummy = 0;
      fread((unsigned char *)&dummy, 4, 1, file); // For compatibility with old savestates
    }
    fread((unsigned char *)&zclk, 4, 1, file);
    fread((unsigned char *)&initialized, 4, 1, file);
    fread((unsigned char *)&Z80_BANK, 4, 1, file);
    Z80_BANK &= 0x1FF;
    fread((unsigned char *)&current_timeslice, 4, 1, file);
}
