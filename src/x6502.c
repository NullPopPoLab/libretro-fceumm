/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2002 Xodnizel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <string.h>

#include "fceu.h"
#include "fceu-types.h"
#include "x6502.h"
#include "fceu.h"
#include "sound.h"

X6502 X;
uint8 encryptOpcodes =0;

#ifdef FCEUDEF_DEBUGGER
void (*X6502_Run)(int32 cycles);
#endif

uint32 timestamp;
uint32 sound_timestamp;
void FP_FASTAPASS(1) (*MapIRQHook)(int a);

#define _PC        X.PC
#define _A         X.A
#define _X         X.X
#define _Y         X.Y
#define _S         X.S
#define _P         X.P
#define _PI        X.mooPI
#define _DB        X.DB
#define _count     X.count
#define _tcount    X.tcount
#define _IRQlow    X.IRQlow
#define _jammed    X.jammed

#define ADDCYC(x) {									\
	int __x = x;									\
	_tcount += __x;									\
	_count -= __x * 48;								\
	timestamp += __x;  \
	if (!overclocked) sound_timestamp +=  __x; \
}

static INLINE uint8 RdMemNorm(uint32 A) {
	return(_DB = ARead[A](A));
}

static INLINE void WrMemNorm(uint32 A, uint8 V) {
	BWrite[A](A, V);
}

#ifdef FCEUDEF_DEBUGGER
X6502 XSave;	/* This is getting ugly. */

static INLINE uint8 RdMemHook(uint32 A) {
	if (X.ReadHook)
		return(_DB = X.ReadHook(&X, A));
	else
		return(_DB = ARead[A](A));
}

static INLINE void WrMemHook(uint32 A, uint8 V) {
	if (X.WriteHook)
		X.WriteHook(&X, A, V);
	else
		BWrite[A](A, V);
}
#endif

static INLINE uint8 RdRAMFast(uint32 A) {
	return(_DB = RAM[A]);
}

static INLINE void WrRAMFast(uint32 A, uint8 V) {
	RAM[A] = V;
}

uint8 FASTAPASS(1) X6502_DMR(uint32 A) {
	ADDCYC(1);
	return(X.DB = ARead[A](A));
}

void FASTAPASS(2) X6502_DMW(uint32 A, uint8 V) {
	ADDCYC(1);
	BWrite[A](A, V);
}

#define PUSH(V) {									\
	uint8 VTMP = V;									\
	WrRAM(0x100 + _S, VTMP);						\
	_S--;											\
}

#define POP() RdRAM(0x100 + (++_S))

static uint8 ZNTable[256];
/* Some of these operations will only make sense if you know what the flag constants are. */

#define X_ZN(zort)  _P &= ~(Z_FLAG | N_FLAG); _P |= ZNTable[zort]
#define X_ZNT(zort) _P |= ZNTable[zort]

#define JR(cond) {									\
	if (cond)										\
	{												\
		uint32 tmp;									\
		int32 disp;									\
		disp = (int8)RdMem(_PC);					\
		_PC++;										\
		ADDCYC(1);									\
		tmp = _PC;									\
		_PC += disp;								\
		if ((tmp ^ _PC) & 0x100)					\
			ADDCYC(1);								\
	} else _PC++;									\
}

#define LDA     _A = x; X_ZN(_A)
#define LDX     _X = x; X_ZN(_X)
#define LDY     _Y = x; X_ZN(_Y)

/* All of the freaky arithmetic operations. */
#define AND     _A &= x; X_ZN(_A)
#define BIT     _P &= ~(Z_FLAG | V_FLAG | N_FLAG); _P |= ZNTable[x & _A] & Z_FLAG; _P |= x & (V_FLAG | N_FLAG)
#define EOR     _A ^= x; X_ZN(_A)
#define ORA     _A |= x; X_ZN(_A)

#define ADC {										\
	uint32 l = _A + x + (_P & 1);					\
	_P &= ~(Z_FLAG | C_FLAG | N_FLAG | V_FLAG);		\
	_P |= ((((_A ^ x) & 0x80) ^ 0x80) & ((_A ^ l) & 0x80)) >> 1;  \
	_P |= (l >> 8) & C_FLAG;						\
	_A = l;											\
	X_ZNT(_A);										\
}

#define SBC {										\
	uint32 l = _A - x - ((_P & 1) ^ 1);				\
	_P &= ~(Z_FLAG | C_FLAG | N_FLAG | V_FLAG);		\
	_P |= ((_A ^ l) & (_A ^ x) & 0x80) >> 1;		\
	_P |= ((l >> 8) & C_FLAG) ^ C_FLAG;				\
	_A = l;											\
	X_ZNT(_A);										\
}

#define CMPL(a1, a2) {								\
	uint32 t = a1 - a2;								\
	X_ZN(t & 0xFF);									\
	_P &= ~C_FLAG;									\
	_P |= ((t >> 8) & C_FLAG) ^ C_FLAG;				\
}

/* Special undocumented operation.  Very similar to CMP. */
#define AXS {										\
	uint32 t = (_A & _X) - x;						\
	X_ZN(t & 0xFF);									\
	_P &= ~C_FLAG;									\
	_P |= ((t >> 8) & C_FLAG) ^ C_FLAG;				\
	_X = t;											\
}

#define CMP     CMPL(_A, x)
#define CPX     CMPL(_X, x)
#define CPY     CMPL(_Y, x)

/* The following operations modify the byte being worked on. */
#define DEC     x--; X_ZN(x)
#define INC     x++; X_ZN(x)

#define ASL     _P &= ~C_FLAG; _P |= x >> 7; x <<= 1; X_ZN(x)
#define LSR     _P &= ~(C_FLAG | N_FLAG | Z_FLAG); _P |= x & 1; x >>= 1; X_ZNT(x)

/* For undocumented instructions, maybe for other things later... */
#define LSRA    _P &= ~(C_FLAG | N_FLAG | Z_FLAG); _P |= _A & 1; _A >>= 1; X_ZNT(_A)

#define ROL {										\
	uint8 l = x >> 7;								\
	x <<= 1;										\
	x |= _P & C_FLAG;								\
	_P &= ~(Z_FLAG | N_FLAG | C_FLAG);				\
	_P |= l;										\
	X_ZNT(x);										\
}

#define ROR {										\
	uint8 l = x & 1;								\
	x >>= 1;										\
	x |= (_P & C_FLAG) << 7;						\
	_P &= ~(Z_FLAG | N_FLAG | C_FLAG);				\
	_P |= l;										\
	X_ZNT(x);										\
}

/* Icky icky thing for some undocumented instructions.  Can easily be
 * broken if names of local variables are changed.
 */

/* Absolute */
#define GetAB(target) {								\
	target = RdMem(_PC);							\
	_PC++;											\
	target |= RdMem(_PC) << 8;						\
	_PC++;											\
}

/* Absolute Indexed(for reads) */
#define GetABIRD(target, i) {						\
	uint32 tmp;										\
	GetAB(tmp);										\
	target = tmp;									\
	target += i;									\
	if ((target ^ tmp) & 0x100) {					\
		target &= 0xFFFF;							\
		RdMem(target ^ 0x100);						\
		ADDCYC(1);									\
	}												\
}

/* Absolute Indexed(for writes and rmws) */
#define GetABIWR(target, i) {						\
	uint32 rt;										\
	GetAB(rt);										\
	target = rt;									\
	target += i;									\
	target &= 0xFFFF;								\
	RdMem((target & 0x00FF) | (rt & 0xFF00));		\
}

/* Zero Page */
#define GetZP(target) {								\
	target = RdMem(_PC);							\
	_PC++;											\
}

/* Zero Page Indexed */
#define GetZPI(target, i) {							\
	target = i + RdMem(_PC);						\
	_PC++;											\
}

/* Indexed Indirect */
#define GetIX(target) {								\
	uint8 tmp;										\
	tmp = RdMem(_PC);								\
	_PC++;											\
	tmp += _X;										\
	target = RdRAM(tmp);							\
	tmp++;											\
	target |= RdRAM(tmp) << 8;						\
}

/* Indirect Indexed(for reads) */
#define GetIYRD(target) {							\
	uint32 rt;										\
	uint8 tmp;										\
	tmp = RdMem(_PC);								\
	_PC++;											\
	rt = RdRAM(tmp);								\
	tmp++;											\
	rt |= RdRAM(tmp) << 8;							\
	target = rt;									\
	target += _Y;									\
	if ((target ^ rt) & 0x100) {					\
		target &= 0xFFFF;							\
		RdMem(target ^ 0x100);						\
		ADDCYC(1);									\
	}												\
}

/* Indirect Indexed(for writes and rmws) */
#define GetIYWR(target) {							\
	uint32 rt;										\
	uint8 tmp;										\
	tmp = RdMem(_PC);								\
	_PC++;											\
	rt = RdRAM(tmp);								\
	tmp++;											\
	rt |= RdRAM(tmp) << 8;							\
	target = rt;									\
	target += _Y;									\
	target &= 0xFFFF;								\
	RdMem((target & 0x00FF) | (rt & 0xFF00));		\
}

/* Now come the macros to wrap up all of the above stuff addressing mode functions
and operation macros.  Note that operation macros will always operate(redundant
redundant) on the variable "x".
*/

#define RMW_A(op)       { uint8 x = _A; op; _A = x; break; }	/* Meh... */
#define RMW_AB(op)      { uint32 A; uint8 x; GetAB(A); x = RdMem(A); WrMem(A, x); op; WrMem(A, x); break; }
#define RMW_ABI(reg, op) { uint32 A; uint8 x; GetABIWR(A, reg); x = RdMem(A); WrMem(A, x); op; WrMem(A, x); break; }
#define RMW_ABX(op)     RMW_ABI(_X, op)
#define RMW_ABY(op)     RMW_ABI(_Y, op)
#define RMW_IX(op)      { uint32 A; uint8 x; GetIX(A); x = RdMem(A); WrMem(A, x); op; WrMem(A, x); break; }
#define RMW_IY(op)      { uint32 A; uint8 x; GetIYWR(A); x = RdMem(A); WrMem(A, x); op; WrMem(A, x); break; }
#define RMW_ZP(op)      { uint8 A; uint8 x; GetZP(A); x = RdRAM(A); op; WrRAM(A, x); break; }
#define RMW_ZPX(op)     { uint8 A; uint8 x; GetZPI(A, _X); x = RdRAM(A); op; WrRAM(A, x); break; }

#define LD_IM(op)       { uint8 x; x = RdMem(_PC); _PC++; op; break; }
#define LD_ZP(op)       { uint8 A; uint8 x; GetZP(A); x = RdRAM(A); op; break; }
#define LD_ZPX(op)      { uint8 A; uint8 x; GetZPI(A, _X); x = RdRAM(A); op; break; }
#define LD_ZPY(op)      { uint8 A; uint8 x; GetZPI(A, _Y); x = RdRAM(A); op; break; }
#define LD_AB(op)       { uint32 A; uint8 x; GetAB(A); x = RdMem(A); op; break; }
#define LD_ABI(reg, op) { uint32 A; uint8 x; GetABIRD(A, reg); x = RdMem(A); op; break; }
#define LD_ABX(op)      LD_ABI(_X, op)
#define LD_ABY(op)      LD_ABI(_Y, op)
#define LD_IX(op)       { uint32 A; uint8 x; GetIX(A); x = RdMem(A); op; break; }
#define LD_IY(op)       { uint32 A; uint8 x; GetIYRD(A); x = RdMem(A); op; break; }

#define ST_ZP(r)        { uint8 A; GetZP(A); WrRAM(A, r); break; }
#define ST_ZPX(r)       { uint8 A; GetZPI(A, _X); WrRAM(A, r); break; }
#define ST_ZPY(r)       { uint8 A; GetZPI(A, _Y); WrRAM(A, r); break; }
#define ST_AB(r)        { uint32 A; GetAB(A); WrMem(A, r); break; }
#define ST_ABI(reg, r)  { uint32 A; GetABIWR(A, reg); WrMem(A, r); break; }
#define ST_ABX(r)       ST_ABI(_X, r)
#define ST_ABY(r)       ST_ABI(_Y, r)
#define ST_IX(r)        { uint32 A; GetIX(A); WrMem(A, r); break; }
#define ST_IY(r)        { uint32 A; GetIYWR(A); WrMem(A, r); break; }

static uint8 CycTable[256] =
{
/*0x00*/ 7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
/*0x10*/ 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*0x20*/ 6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
/*0x30*/ 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*0x40*/ 6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
/*0x50*/ 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*0x60*/ 6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
/*0x70*/ 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*0x80*/ 2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
/*0x90*/ 2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
/*0xA0*/ 2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
/*0xB0*/ 2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
/*0xC0*/ 2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
/*0xD0*/ 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*0xE0*/ 2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
/*0xF0*/ 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
};

void FASTAPASS(1) X6502_IRQBegin(int w) {
	_IRQlow |= w;
}

void FASTAPASS(1) X6502_IRQEnd(int w) {
	_IRQlow &= ~w;
}

void TriggerNMI(void) {
	_IRQlow |= FCEU_IQNMI;
}

void TriggerNMI2(void) {
	_IRQlow |= FCEU_IQNMI2;
}

#ifdef FCEUDEF_DEBUGGER
/* Called from debugger. */
void FCEUI_NMI(void) {
	_IRQlow |= FCEU_IQNMI;
}

void FCEUI_IRQ(void) {
	_IRQlow |= FCEU_IQTEMP;
}

void FCEUI_GetIVectors(uint16 *reset, uint16 *irq, uint16 *nmi) {
	fceuindbg = 1;

	*reset = RdMemNorm(0xFFFC);
	*reset |= RdMemNorm(0xFFFD) << 8;
	*nmi = RdMemNorm(0xFFFA);
	*nmi |= RdMemNorm(0xFFFB) << 8;
	*irq = RdMemNorm(0xFFFE);
	*irq |= RdMemNorm(0xFFFF) << 8;
	fceuindbg = 0;
}
static int debugmode;
#endif

void X6502_Reset(void) {
	_IRQlow = FCEU_IQRESET;
}

void X6502_Init(void) {
	int x;

	memset((void*)&X, 0, sizeof(X));
	for (x = 0; x < 256; x++)
		if (!x)
			ZNTable[x] = Z_FLAG;
		else if (x & 0x80)
			ZNTable[x] = N_FLAG;
		else
			ZNTable[x] = 0;
	#ifdef FCEUDEF_DEBUGGER
	X6502_Debug(0, 0, 0);
	#endif
}

void X6502_Power(void) {
	_count = _tcount = _IRQlow = _PC = _A = _X = _Y = _P = _PI = _DB = _jammed = 0;
	_S = 0xFD;
	timestamp = sound_timestamp = 0;
	X6502_Reset();
}

#ifdef FCEUDEF_DEBUGGER
static void X6502_RunDebug(int32 cycles) {
	#define RdRAM RdMemHook
	#define WrRAM WrMemHook
	#define RdMem RdMemHook
	#define WrMem WrMemHook

	if (PAL)
		cycles *= 15;	/* 15*4=60 */
	else
		cycles *= 16;	/* 16*4=64 */

	_count += cycles;

	while (_count > 0) {
		int32 temp;
		uint8 b1;

		if (_IRQlow) {
			if (_IRQlow & FCEU_IQRESET) {
				_PC = RdMem(0xFFFC);
				_PC |= RdMem(0xFFFD) << 8;
				_jammed = 0;
				_PI = _P = I_FLAG;
				_IRQlow &= ~FCEU_IQRESET;
			} else if (_IRQlow & FCEU_IQNMI2) {
				_IRQlow &= ~FCEU_IQNMI2;
				_IRQlow |= FCEU_IQNMI;
			} else if (_IRQlow & FCEU_IQNMI) {
				if (!_jammed) {
					ADDCYC(7);
					PUSH(_PC >> 8);
					PUSH(_PC);
					PUSH((_P & ~B_FLAG) | (U_FLAG));
					_P |= I_FLAG;
					_PC = RdMem(0xFFFA);
					_PC |= RdMem(0xFFFB) << 8;
					_IRQlow &= ~FCEU_IQNMI;
				}
			} else {
				if (!(_PI & I_FLAG) && !_jammed) {
					ADDCYC(7);
					PUSH(_PC >> 8);
					PUSH(_PC);
					PUSH((_P & ~B_FLAG) | (U_FLAG));
					_P |= I_FLAG;
					_PC = RdMem(0xFFFE);
					_PC |= RdMem(0xFFFF) << 8;
				}
			}
			_IRQlow &= ~(FCEU_IQTEMP);
			if (_count <= 0) {
				_PI = _P;
				return;
			}	/* Should increase accuracy without a
				 * major speed hit.
				 */
		}

		if (X.CPUHook) X.CPUHook(&X);
		/* Ok, now the real fun starts.
		 * Do the pre-exec voodoo.
		 */
		if (X.ReadHook || X.WriteHook) {
			uint32 tsave = timestamp;
			XSave = X;

			fceuindbg = 1;
			X.preexec = 1;
			b1 = RdMem(_PC);
			_PC++;
			if (encryptOpcodes ==12) b1 =b1 &0x39 | b1 >>1 &0x42 | b1 <<1 &0x84;
			if (encryptOpcodes ==14) b1 =b1 &0x3F | b1 >>1 &0x40 | b1 <<1 &0x80;
			switch (b1) {
				#include "ops.h"
			}

			timestamp = tsave;

			/* In case an NMI/IRQ/RESET was triggered by the debugger.
			 * Should we also copy over the other hook variables?
			 */
			XSave.IRQlow = X.IRQlow;
			XSave.ReadHook = X.ReadHook;
			XSave.WriteHook = X.WriteHook;
			XSave.CPUHook = X.CPUHook;
			X = XSave;
			fceuindbg = 0;
		}

		_PI = _P;
		b1 = RdMem(_PC);
		ADDCYC(CycTable[b1]);

		temp = _tcount;
		_tcount = 0;
		if (MapIRQHook) MapIRQHook(temp);

      if (!overclocked)
         FCEU_SoundCPUHook(temp);

		_PC++;
		if (encryptOpcodes ==12) b1 =b1 &0x39 | b1 >>1 &0x42 | b1 <<1 &0x84;
		if (encryptOpcodes ==14) b1 =b1 &0x3F | b1 >>1 &0x40 | b1 <<1 &0x80;
		switch (b1) {
			#include "ops.h"
		}
	}
	#undef RdRAM
	#undef WrRAM
	#undef RdMem
	#undef WrMem
}

static void X6502_RunNormal(int32 cycles)
#else
void X6502_Run(int32 cycles)
#endif
{
	#define RdRAM RdRAMFast
	#define WrRAM WrRAMFast
	#define RdMem RdMemNorm
	#define WrMem WrMemNorm

	#if (defined(C80x86) && defined(__GNUC__))
	/* Gives a nice little speed boost. */
	register uint16 pbackus asm ("edi");
	#else
	uint16 pbackus;
	#endif

	pbackus = _PC;

	#undef _PC
	#define _PC pbackus

	if (PAL)
		cycles *= 15;	/* 15*4=60 */
	else
		cycles *= 16;	/* 16*4=64 */

	_count += cycles;

	while (_count > 0) {
		int32 temp;
		uint8 b1;

		if (_IRQlow) {
			if (_IRQlow & FCEU_IQRESET) {
				_PC = RdMem(0xFFFC);
				_PC |= RdMem(0xFFFD) << 8;
				_jammed = 0;
				_PI = _P = I_FLAG;
				_IRQlow &= ~FCEU_IQRESET;
			} else if (_IRQlow & FCEU_IQNMI2) {
				_IRQlow &= ~FCEU_IQNMI2;
				_IRQlow |= FCEU_IQNMI;
			} else if (_IRQlow & FCEU_IQNMI) {
				if (!_jammed) {
					ADDCYC(7);
					PUSH(_PC >> 8);
					PUSH(_PC);
					PUSH((_P & ~B_FLAG) | (U_FLAG));
					_P |= I_FLAG;
					_PC = RdMem(0xFFFA);
					_PC |= RdMem(0xFFFB) << 8;
					_IRQlow &= ~FCEU_IQNMI;
				}
			} else {
				if (!(_PI & I_FLAG) && !_jammed) {
					ADDCYC(7);
					PUSH(_PC >> 8);
					PUSH(_PC);
					PUSH((_P & ~B_FLAG) | (U_FLAG));
					_P |= I_FLAG;
					_PC = RdMem(0xFFFE);
					_PC |= RdMem(0xFFFF) << 8;
				}
			}
			_IRQlow &= ~(FCEU_IQTEMP);
			if (_count <= 0) {
				_PI = _P;
				X.PC = pbackus;
				return;
			}	/* Should increase accuracy without a
				 * major speed hit.
				 */
		}

		_PI = _P;
		b1 = RdMem(_PC);

		ADDCYC(CycTable[b1]);

		temp = _tcount;
		_tcount = 0;
		if (MapIRQHook) MapIRQHook(temp);
		if (!overclocked)
			FCEU_SoundCPUHook(temp);
		X.PC = pbackus;
		_PC++;
		if (encryptOpcodes ==12) b1 =b1 &0x39 | b1 >>1 &0x42 | b1 <<1 &0x84;
		if (encryptOpcodes ==14) b1 =b1 &0x3F | b1 >>1 &0x40 | b1 <<1 &0x80;
		switch (b1) {
			#include "ops.h"
		}
	}

	#undef _PC
	#define _PC X.PC
	_PC = pbackus;
	#undef RdRAM
	#undef WrRAM
}

#ifdef FCEUDEF_DEBUGGER
void X6502_Debug(void (*CPUHook)(X6502 *), uint8 (*ReadHook)(X6502 *, uint32), void (*WriteHook)(X6502 *, uint32, uint8)) {
	debugmode = (ReadHook || WriteHook || CPUHook) ? 1 : 0;
	X.ReadHook = ReadHook;
	X.WriteHook = WriteHook;
	X.CPUHook = CPUHook;

	if (!debugmode)
		X6502_Run = X6502_RunNormal;
	else
		X6502_Run = X6502_RunDebug;
}

#endif
