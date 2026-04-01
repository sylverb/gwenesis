/*
**
** software implementation of Yamaha FM sound generator (YM2612/YM3438)
**
** Original code (MAME fm.c)
**
** Copyright (C) 2001, 2002, 2003 Jarek Burczynski (bujar at mame dot net)
** Copyright (C) 1998 Tatsuyuki Satoh , MultiArcadeMachineEmulator development
**
** Version 1.4 (final beta)
**
** Additional code & fixes by Eke-Eke for Genesis Plus GX
**
*/

#ifndef _H_YM2612_
#define _H_YM2612_

#ifdef TARGET_GNW
#include <stdint.h>
#include <stdio.h>
extern int16_t gwenesis_ym2612_buffer[];
extern int ym2612_index;
extern int ym2612_clock;
#endif

extern void YM2612Init(void);
extern void YM2612Config(unsigned char dac_bits);
extern void YM2612ResetChip(void);
#ifdef TARGET_GNW
/* YM2612Update is static inline in GNW build, not exported */
extern void YM2612Write(unsigned int a, unsigned int v, int target);
extern void ym2612_run(int target);
extern unsigned int YM2612Read(int target);
#else
extern void YM2612Update(int *buffer, int length);
extern void YM2612Write(unsigned int a, unsigned int v);
extern unsigned int YM2612Read(void);
#endif
#ifdef TARGET_GNW
void gwenesis_ym2612_save_state(FILE *file);
void gwenesis_ym2612_load_state(FILE *file);
#else
extern int YM2612LoadContext(unsigned char *state);
extern int YM2612SaveContext(unsigned char *state);
#endif

#endif /* _YM2612_ */
