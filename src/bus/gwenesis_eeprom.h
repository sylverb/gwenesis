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

I2C serial EEPROM (24Cxx) support, ported from Genesis Plus GX
(core/cart_hw/eeprom_i2c.c).  Games that use a serial EEPROM (e.g. Mega Man -
The Wily Wars, EA sports titles, NBA Jam, ...) bit-bang an I2C protocol over a
data line mapped in the cartridge address space instead of using a parallel
SRAM.  Detection is done from the ROM product code / header, exactly like GPGX.

The EEPROM contents are stored in GWENESIS_SRAM so that the existing
gwenesis_sram_load()/save() battery persistence is reused unchanged.
*/

#ifndef _GWENESIS_EEPROM_H_
#define _GWENESIS_EEPROM_H_

#pragma once

#include <stdint.h>
#include <stdio.h>

/* 1 = a serial I2C EEPROM was detected for the loaded cartridge. */
extern int gwenesis_eeprom_enabled;

/*
 * Detect whether the loaded cartridge uses a serial I2C EEPROM and, if so,
 * configure the EEPROM type / board mapper.  Mirrors GPGX eeprom_i2c_init().
 *
 *   product        : 14-char product code from ROM header (offset 0x180)
 *   checksum       : ROM header checksum (offset 0x18E)
 *   rom_first_long : big-endian 32-bit value at logical ROM offset 0
 *   sram_detected  : 1 if a "RA" backup-RAM header was found
 *   sram_start     : detected backup-RAM start address (raw header value)
 *   sram_end       : detected backup-RAM end   address (raw header value)
 *   header_type    : backup-RAM type byte (offset 0x1B2)
 *
 * Returns 1 if a serial EEPROM game was detected (gwenesis_eeprom_enabled set),
 * 0 otherwise.  The actual memory_map handlers are installed later by
 * gwenesis_eeprom_install_memory_map().
 */
int gwenesis_eeprom_detect(const char *product, uint16_t checksum,
                           uint32_t rom_first_long,
                           int sram_detected,
                           uint32_t sram_start, uint32_t sram_end,
                           uint8_t header_type);

/*
 * Install the I2C board memory-map handlers for the detected mapper.
 * Must be called from gwenesis_bus_init_memory_map() after the ROM pages have
 * been set up, and only when gwenesis_eeprom_enabled != 0.
 */
void gwenesis_eeprom_install_memory_map(void);

/* Persisted EEPROM size in bytes (array size of the detected chip). */
uint32_t gwenesis_eeprom_size(void);

/* Savestate of the transient I2C line state (not the EEPROM data array). */
void gwenesis_eeprom_save_state(FILE *file);
void gwenesis_eeprom_load_state(FILE *file);

#endif /* _GWENESIS_EEPROM_H_ */
