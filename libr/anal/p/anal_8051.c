/* radare - LGPL - Copyright 2013-2017 - pancake, dkreuter, astuder  */

#include <string.h>
#include <r_types.h>
#include <r_lib.h>
#include <r_asm.h>
#include <r_anal.h>

#include <8051_ops.h>

#define IRAM 0x10000

static bool i8051_is_init = false;
// doesnt needs to be global, but anyway :D
static RAnalEsilCallbacks ocbs = {0};

static ut8 bitindex[] = {
	// bit 'i' can be found in (ram[bitindex[i>>3]] >> (i&7)) & 1
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, // 0x00
	0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, // 0x40
	0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, // 0x80
	0x00, 0x00, 0xD0, 0x00, 0xE0, 0x00, 0xF0, 0x00  // 0xC0
};

typedef struct {
	const char *name;
	ut8 offset; // offset into memory, where the value is held
	ut8 resetvalue; // value the register takes in case of a reset
	ut8 num_bytes; // no more than sizeof(ut64)
	ut8 banked : 1;
	ut8 isdptr : 1;
} RI8051Reg;

static RI8051Reg registers[] = {
	// keep these sorted
	{"a",     0xE0, 0x00, 1, 0},
	{"b",     0xF0, 0x00, 1, 0},
	{"dph",   0x83, 0x00, 1, 0},
	{"dpl",   0x82, 0x00, 1, 0},
	{"dptr",  0x82, 0x00, 2, 0, 1},
	{"ie",    0xA8, 0x00, 1, 0},
	{"ip",    0xB8, 0x00, 1, 0},
	{"p0",    0x80, 0xFF, 1, 0},
	{"p1",    0x90, 0xFF, 1, 0},
	{"p2",    0xA0, 0xFF, 1, 0},
	{"p3",    0xB0, 0xFF, 1, 0},
	{"pcon",  0x87, 0x00, 1, 0},
	{"psw",   0xD0, 0x00, 1, 0},
	{"r0",    0x00, 0x00, 1, 1},
	{"r1",    0x01, 0x00, 1, 1},
	{"r2",    0x02, 0x00, 1, 1},
	{"r3",    0x03, 0x00, 1, 1},
	{"r4",    0x04, 0x00, 1, 1},
	{"r5",    0x05, 0x00, 1, 1},
	{"r6",    0x06, 0x00, 1, 1},
	{"r7",    0x07, 0x00, 1, 1},
	{"sbuf",  0x99, 0x00, 1, 0},
	{"scon",  0x98, 0x00, 1, 0},
	{"sp",    0x81, 0x07, 1, 0},
	{"tcon",  0x88, 0x00, 1, 0},
	{"th0",   0x8C, 0x00, 1, 0},
	{"th1",   0x8D, 0x00, 1, 0},
	{"tl0",   0x8A, 0x00, 1, 0},
	{"tl1",   0x8B, 0x00, 1, 0},
	{"tmod",  0x89, 0x00, 1, 0}
};

#define emit(frag) r_strbuf_appendf(&op->esil, frag)
#define emitf(...) r_strbuf_appendf(&op->esil, __VA_ARGS__)

#define j(frag) emitf(frag, 1 & buf[0], buf[1], buf[2], op->jump)
#define h(frag) emitf(frag, 7 & buf[0], buf[1], buf[2], op->jump)
#define k(frag) emitf(frag, bitindex[buf[1]>>3], buf[1] & 7, buf[2], op->jump)

// on 8051 the stack grows upward and lsb is pushed first meaning
// that on little-endian esil vms =[2] works as indended
#define PUSH1 "1,sp,+=,sp,=[1]"
#define POP1  "sp,[1],1,sp,-=,"
#define PUSH2 "1,sp,+=,sp,=[2],1,sp,+="
#define POP2  "1,sp,-=,sp,[2],1,sp,-=,"
#define CALL(skipbytes) skipbytes",pc,+," PUSH2
#define JMP "%4$d,pc,="
#define CJMP "?{," JMP ",}"
#define BIT_SET "%2$d,1,<<,"
#define BIT_MASK BIT_SET "!,"
#define BIT_R "%2$d,%1$d,[1],>>,1,&,"
#define F_BIT_R "%d,%d,[1],>>,1,&,"
#define A_BIT_R a2, a1

#define IRAM_BASE  "0x10000"
#define XRAM_BASE  "0x10100"

#define ES_IB1 IRAM_BASE ",%2$d,+,"
#define ES_IB2 IRAM_BASE ",%3$d,+,"
#define ES_BIT IRAM_BASE ",%1$d,+,"
#define ES_R0I IRAM_BASE ",r%1$d,+,"
#define ES_R0X XRAM_BASE ",r%1$d,+,"
#define ES_AI "a,"
#define ES_R0  "r%1$d,"
#define ES_R1 "r%2$d,"
#define ES_A "a,"
#define ES_L1 "%2$d,"
#define ES_L2 "%3$d,"
#define ES_C "C,"

// signed char variant
#define ESX_L1 "%2$hhd,"
#define ESX_L2 "%3$hhd,"

#define ACC_IB1 "[1],"
#define ACC_IB2 "[1],"
#define ACC_BIT "[1],"
#define ACC_R0I "[1],"
#define ACC_R0X "[1],"
#define ACC_AI  "[1],"
#define ACC_R0  ""
#define ACC_R1  ""
#define ACC_A   ""
#define ACC_L1  ""
#define ACC_L2  ""
#define ACC_C   ""

#define XR(subject)            ES_##subject               ACC_##subject
#define XW(subject)            ES_##subject "="           ACC_##subject
#define XI(subject, operation) ES_##subject operation "=" ACC_##subject

#define TEMPLATE_4(base, format, src4, arg1, arg2) \
	case base + 0x4: \
		h (format(0, src4, arg1, arg2)); break; \
		\
	case base + 0x5: \
		h (format(1, IB1,  arg1, arg2)); break; \
		\
	case base + 0x6: \
	case base + 0x7: \
		j (format(0, R0I,  arg1, arg2)); break; \
		\
	case base + 0x8: case base + 0x9: \
	case base + 0xA: case base + 0xB: \
	case base + 0xC: case base + 0xD: \
	case base + 0xE: case base + 0xF: \
		h (format(0, R0,   arg1, arg2)); break;

#define OP_GROUP_INPLACE_LHS_4(base, lhs, op) TEMPLATE_4(base, OP_GROUP_INPLACE_LHS_4_FMT, L1, lhs, op)
#define OP_GROUP_INPLACE_LHS_4_FMT(databyte, rhs, lhs, op) XR(rhs) XI(lhs, op)

#define OP_GROUP_UNARY_4(base, op) TEMPLATE_4(base, OP_GROUP_UNARY_4_FMT, A, op, XXX)
#define OP_GROUP_UNARY_4_FMT(databyte, lhs, op, xxx) XI(lhs, op)

static void analop_esil(RAnal *a, RAnalOp *op, ut64 addr, const ut8 *buf) {
	r_strbuf_init (&op->esil);
	r_strbuf_set (&op->esil, "");

	const ut32 a1 = bitindex[buf[1] >> 3];
	const ut32 a2 = buf[1] & 7;
	const ut32 a3 = buf[2];

	switch (buf[0]) {
	// Irregulars sorted by lower nibble
	case 0x00: /* nop */
		emit (",");
		break;

	case 0x10: /* jbc bit, offset */
		emitf (F_BIT_R "&,?{,%d,1,<<,255,^,%d,&=[1],%hhd,3,+,pc,+=,}", A_BIT_R, a2, a1, a3);
		break;
	case 0x20: /* jb bit, offset */
		emitf (F_BIT_R "&,?{,%hhd,3,+,pc,+=,}", A_BIT_R, a3);
		break;
	case 0x30: /* jnb bit, offset */
//		emitf (F_BIT_R "&,!,?{,%hhd,3,+,pc,+=,}", A_BIT_R, a3);
		k (XR(BIT) BIT_MASK CJMP);
		break;
	case 0x40: /* jc offset */
		emitf ("C,!," CJMP);
		break;
	case 0x50: /* jnc offset */
		j ("C," CJMP );
		break;
	case 0x60: /* jz offset */
		j ("a,!," CJMP);
		break;
	case 0x70: /* jnz offset */
		j ("a," CJMP);
		break;
	case 0x80: /* sjmp offset */
		j (JMP);
		break;

	case 0x11: case 0x31: case 0x51: case 0x71:
	case 0x91: case 0xB1: case 0xD1: case 0xF1: /* acall addr11 */
		emit (CALL ("2"));
		/* fall through */
	case 0x01: case 0x21: case 0x41: case 0x61:
	case 0x81: case 0xA1: case 0xC1: case 0xE1: /* ajmp addr11 */
		emitf ("0x%x,pc,=", (addr & 0xF800) | ((((ut16)buf[0])<<3) & 0x0700) | buf[1]);
		break;

	case 0x12: /* lcall addr16 */
		emitf (CALL ("3"));
		/* fall through */
	case 0x02: /* ljmp addr16 */
		emitf ("%d,pc,=", (ut32)((buf[1] << 8) + buf[2]));
		break;
	
	case 0x22: /* ret */
	case 0x32: /* reti */
		emitf (POP2 "pc,=");
		break;

	case 0x03: /* rr a */
		emit ("1,a,0x101,*,>>,a,=");
		break;
	OP_GROUP_UNARY_4 (0x00, "++") /* 0x04..0x0f: inc */
	case 0x13: /* rrc a */
		emit ("1,a,>>,$c7,C,=,a,=");
		break;
	OP_GROUP_UNARY_4 (0x10, "--") /* 0x14..0x1f dec */
	case 0x23: /* rl a */
		emit ("7,a,0x101,*,>>,a,=");
		break;
	OP_GROUP_INPLACE_LHS_4 (0x20, A, "+") /* 0x24..0x2f add a,.. */
	case 0x33: /* rlc a */
		emit ("1,a,>>,$c0,C,=,a,=");
		break;
	case 0x34: /* addc a, imm */
		h (XR(L1)  "C,+," XI(A, "+"));
		 break;
	case 0x35: /* addc a, direct */
		h (XR(IB1) "C,+," XI(A, "+"));
		break;
	case 0x36: case 0x37: /* addc a, @Ri */
		j (XR(R0I) "C,+," XI(A, "+"));
		break;
	case 0x38: case 0x39:
	case 0x3A: case 0x3B:
	case 0x3C: case 0x3D:
	case 0x3E: case 0x3F: /* addc a, Rn */
		h (XR(R0)  "C,+," XI(A, "+"));
		break;
	case 0x42: /* orl direct, a */
		emitf ("%d,[],a,|,%d,=[]", (ut8)buf[1], (ut8)buf[1]);
		break;
	case 0x43: /* orl direct, imm */
		emitf ("%d,[],%d,|,%d,=[]", (ut8)buf[1], (ut8)buf[2], (ut8)buf[1]);
		break;
	OP_GROUP_INPLACE_LHS_4 (0x40, A, "|") /* 0x44..0x4f orl a,.. */
	case 0x52: /* anl direct, a */
		emitf ("%d,[],a,&,%d,=[]", (ut8)buf[1], (ut8)buf[1]);
		break;
	case 0x53: /* anl direct, imm */
		emitf ("%d,[],%d,&,%d,=[]", (ut8)buf[1], (ut8)buf[2], (ut8)buf[1]);
		break;
	OP_GROUP_INPLACE_LHS_4 (0x50, A, "&") /* 0x54..0x5f anl a,.. */
	OP_GROUP_INPLACE_LHS_4 (0x60, A, "^") /* 0x64..0x6f xrl a,.. */
	case 0x72: /* orl C, bit */
		k(BIT_R "C,|=");
		break;
	case 0x73: /* jmp @a+dptr */
		emit ("dptr,a,+,pc,="); break;
	case 0x74: /* mov a, imm */
		h (XR(L1) XW(A));
		break;
	case 0x75: /* mov direct, imm */
		h (XR(L2) XW(IB1));
		break;
	case 0x76: case 0x77: /* mov @Ri, imm */
		j (XR(L1) XW(R0I));
		break;
	case 0x78: case 0x79:
	case 0x7A: case 0x7B:
	case 0x7C: case 0x7D:
	case 0x7E: case 0x7F: /* mov Rn, imm */
		h (XR(L1) XW(R0));
		break;
	case 0x82: /* anl C, bit */
		k (BIT_R "C,&=");
		break;
	case 0x83: /* movc a, @a+pc */
		emit ("a,pc,+,[1],a,=");
		break;
	case 0x84: /* div ab */
		emit ("b,!,OV,=,0,a,b,a,/=,a,b,*,-,-,b,=,0,C,=");
		break;
	case 0x85: /* mov direct, direct */
		h (XR(IB1) XW(IB2));
		break;
	case 0x86: case 0x87: /* mov direct, @Ri */
		j (XR(R0I) XW(IB1));
		break;
	case 0x88: case 0x89:
	case 0x8A: case 0x8B:
	case 0x8C: case 0x8D:
	case 0x8E: case 0x8F: /* mov direct, Rn */
		h (XR(R0) XW(IB1));
		break;
	case 0x90: /* mov dptr, imm */
		emitf ("%d,dptr,=", (buf[1]<<8) + buf[2]);
		break;
	case 0x92: /* mov bit, C */
		/* TODO */
		break;
	case 0x93: /* movc a, @a+dptr */
		emit ("a,dptr,+,[1],a,=");
		break;
	case 0x94: /* subb a, imm */
		h (XR(L1)  "C,-," XI(A, "-"));
		 break;
	case 0x95: /* subb a, direct */
		h (XR(IB1) "C,-," XI(A, "-"));
		break;
	case 0x96: case 0x97: /* subb a, @Ri */
		j (XR(R0I) "C,-," XI(A, "-"));
		break;
	case 0x98: case 0x99:
	case 0x9A: case 0x9B:
	case 0x9C: case 0x9D:
	case 0x9E: case 0x9F: /* subb a, Rn */
		h (XR(R0)  "C,-," XI(A, "-"));
		break;
	case 0xA0: /* orl C, /bit */
		k(BIT_R "!,C,|=");
	case 0xA2: /* mov C, bit */
		/* TODO */
		break;
	case 0xA3: /* inc dptr */
		emit ("dptr,++=");
		break;
	case 0xA4: /* mul ab */
		emit ("8,a,b,*,NUM,>>,NUM,!,!,OV,=,b,=,a,=,0,C,=");
		break;
	case 0xA5: /* "reserved" */
		emit ("0,TRAP");
		break;
	case 0xA6: case 0xA7: /* mov @Ri, direct */
		j (XR(IB1) XW(R0I));
		break;
	case 0xA8: case 0xA9:
	case 0xAA: case 0xAB:
	case 0xAC: case 0xAD:
	case 0xAE: case 0xAF: /* mov Rn, direct */
		h (XR(IB1) XW(R0));
		break;
	case 0xB0: /* anl C, /bit */
		k (BIT_R "!,C,&=");
		break;
	case 0xB2: /* cpl bit */
		/* TODO: translate to macros */
		emitf ("%d,1,<<,%d,^=[1]", a2, a1);
		break;
	case 0xB3: /* cpl C */
		emit ("1," XI(C, "^"));
		break;
	case 0xB4: /* cjne a, imm, offset */
		/* TODO: is != correct op for "not equal"? Replace branch with CJMP macro? */
		h (XR(L1)  XR(A)   "!=,?{,%3$hhd,2,+pc,+=,}");
		break;
	case 0xB5: /* cjne a, direct, offset */
		/* TODO: is != correct op for "not equal"? Replace branch with CJMP macro? */
		h (XR(IB1) XR(A)   "!=,?{,%3$hhd,2,+pc,+=,}");
		break;
	case 0xB6: case 0xB7: /* cjne @ri, imm, offset */
		/* TODO: is != correct op for "not equal"? Replace branch with CJMP macro? */
		j (XR(L1)  XR(R0I) "!=,?{,%3$hhd,2,+pc,+=,}");
		break;
	case 0xB8: case 0xB9:
	case 0xBA: case 0xBB:
	case 0xBC: case 0xBD:
	case 0xBE: case 0xBF: /* cjne Rn, imm, offset */
		/* TODO: is != correct op for "not equal"? Replace branch with CJMP macro? */
		h (XR(L1)  XR(R0)  "!=,?{,%3$hhd,2,+pc,+=,}");
		break;
	case 0xC0: /* push direct */
		h (XR(IB1) PUSH1);
		break;
	case 0xC2: /* clr bit */
		k (BIT_MASK XI(BIT, "&"));
		break;
	case 0xC3: /* clr C */
		emit("0,C,=");
		break;
	case 0xC4: /* swap a */
		emit("4,a,0x101,*,>>,a,=");
		break;
	case 0xC5: /* xch a, direct */
		j (XR(A) XR(IB1) XW(A) "," XW(IB1));
		break;
	case 0xC6: case 0xC7: /* xch a, @Ri */ 
		j (XR(A) XR(R0I) XW(A) "," XW(R0I));
		break;
	case 0xC8: case 0xC9:
	case 0xCA: case 0xCB:
	case 0xCC: case 0xCD:
	case 0xCE: case 0xCF: /* xch a, Rn */
		h (XR(A) XR(R0) XW(A) "," XW(R0));
		break;
	case 0xD0: /* pop direct */
		h (POP1 XW(IB1));
		break;
	case 0xD2: /* setb bit */
		k (BIT_SET XI(BIT, "|"));
		break;
	case 0xD3: /* setb C */
		emitf ("1,C,=");
		break;
	case 0xD4: /* da a (BCD adjust after add) */
		/* TODO */
		break;
	case 0xD5: /* djnz direct, offset */
		h (XI(IB1, "--") "," XR(IB1) "!," CJMP);
		break;
	case 0xD6:
	case 0xD7: /* xchd a, @Ri*/
		/* TODO */
		break;
	case 0xD8: case 0xD9:
	case 0xDA: case 0xDB:
	case 0xDC: case 0xDD:
	case 0xDE: case 0xDF: /* djnz Rn, offset */
		h (XI(R0, "--") "," XR(R0) CJMP);
		break;
	case 0xE0: /* movx a, @dptr */
		emit (XRAM_BASE ",dptr,+,[2],a,=");
		break;
	case 0xE2: case 0xE3: /* movx a, @Ri */
		j (XR(R0X) XW(A));
		break;
	case 0xE4: /* clr a */
		emit ("0,a,=");
		break;
	case 0xE5: /* mov a, direct */
		h (XR(IB1) XW(A));
		break;
	case 0xE6: case 0xE7: /* mov a, @Ri */
		j (XR(R0I) XW(A));
		break;
	case 0xE8: case 0xE9:
	case 0xEA: case 0xEB:
	case 0xEC: case 0xED:
	case 0xEE: case 0xEF: /* mov a, Rn */
		h (XR(R0) XW(A));
		break;
	case 0xF0: /* movx @dptr, a */
		emit ("a," XRAM_BASE ",dptr,+,=[2]");
		break;
	case 0xF2: case 0xF3: /* movx @Ri, a */
		j (XR(A) XW(R0X));
		break;
	case 0xF4: /* cpl a */
		h ("255" XI(A, "^"));
		break;
	case 0xF5: /* mov direct, a */
		h (XR(A) XW(IB1));
		break;
	case 0xF6: case 0xF7: /* mov  @Ri, a */
		j (XR(A) XW(R0I));
		break;
	case 0xF8: case 0xF9:
	case 0xFA: case 0xFB:
	case 0xFC: case 0xFD:
	case 0xFE: case 0xFF: /* mov Rn, a */
		h (XR(A) XW(R0));
		break;
	default:
		break;
	}
}

static int i8051_hook_reg_read(RAnalEsil *, const char *, ut64 *, int *);

static int i8051_reg_compare(const void *name, const void *reg) {
	return strcmp ((const char*)name, ((RI8051Reg*)reg)->name);
}

static RI8051Reg *i8051_reg_find(const char *name) {
	return (RI8051Reg *) bsearch (
		name, registers,
		sizeof (registers) / sizeof (registers[0]),
		sizeof (registers[0]),
		i8051_reg_compare);
}

static int i8051_reg_get_offset(RAnalEsil *esil, RI8051Reg *ri) {
	ut8 offset = ri->offset;
	if (ri->banked) {
		ut64 psw = 0LL;
		i8051_hook_reg_read (esil, "psw", &psw, NULL);
		offset += psw & 0x18;
	}
	return offset;
}

// dkreuter: It would be nice if we could attach hooks to RRegItems directly.
//           That way we could avoid doing a string lookup on register names
//           as r_reg_get already does this. Also, the anal esil callbacks
//           approach interferes with r_reg_arena_swap.

static int i8051_hook_reg_read(RAnalEsil *esil, const char *name, ut64 *res, int *size) {
	int ret = 0;
	ut64 val = 0LL;
	RI8051Reg *ri;
	RAnalEsilCallbacks cbs = esil->cb;

	if ((ri = i8051_reg_find (name))) {
		ut8 offset = i8051_reg_get_offset(esil, ri);
		ret = r_anal_esil_mem_read (esil, IRAM + offset, (ut8*)res, ri->num_bytes);
	}
	esil->cb = ocbs;
	if (!ret && ocbs.hook_reg_read) {
		ret = ocbs.hook_reg_read (esil, name, res, NULL);
	}
	if (!ret && ocbs.reg_read) {
		ret = ocbs.reg_read (esil, name, &val, NULL);
	}
	esil->cb = cbs;

	return ret;
}

static int i8051_hook_reg_write(RAnalEsil *esil, const char *name, ut64 *val) {
	int ret = 0;
	RI8051Reg *ri;
	RAnalEsilCallbacks cbs = esil->cb;
	if ((ri = i8051_reg_find (name))) {
		ut8 offset = i8051_reg_get_offset(esil, ri);
		ret = r_anal_esil_mem_write (esil, IRAM + offset, (ut8*)val, ri->num_bytes);
	}
	esil->cb = ocbs;
	if (!ret && ocbs.hook_reg_write) {
		ret = ocbs.hook_reg_write (esil, name, val);
	}
	esil->cb = cbs;
	return ret;
}

static int esil_i8051_init (RAnalEsil *esil) {
	if (esil->cb.user) {
		return true;
	}
	ocbs = esil->cb;
	/* these hooks break esil emulation */
	/* pc is not read properly, mem mapped registers are not shown in ar, ... */
	/* all 8051 regs are mem mapped, and reg access via mem is very common */
//  disabled to make esil work, before digging deeper
//	esil->cb.hook_reg_read = i8051_hook_reg_read;
//	esil->cb.hook_reg_write = i8051_hook_reg_write;
	i8051_is_init = true;
	return true;
}

static int esil_i8051_fini (RAnalEsil *esil) {
	if (!i8051_is_init) {
		return false;
	}
	R_FREE (ocbs.user);
	i8051_is_init = false;
	return true;
}

static char *get_reg_profile(RAnal *anal) {
	const char *p =
		"=PC	pc\n"
		"=SP	sp\n"
		"gpr	r0	.8	0	0\n"
		"gpr	r1	.8	1	0\n"
		"gpr	r2	.8	2	0\n"
		"gpr	r3	.8	3	0\n"
		"gpr	r4	.8	4	0\n"
		"gpr	r5	.8	5	0\n"
		"gpr	r6	.8	6	0\n"
		"gpr	r7	.8	7	0\n"
		"gpr	a	.8	8	0\n"
		"gpr	b	.8	9	0\n"
		"gpr	dptr	.16	10	0\n"
		"gpr	dpl	.8	10	0\n"
		"gpr	dph	.8	11	0\n"
		"gpr	psw	.8	12	0\n"
		"gpr	sp	.8	13	0\n"
		"gpr	pc	.16	15	0\n"
		"flg	P	.1	96	0\n"
		"flg	OV	.1	98	0\n"
		"flg	C	.1	103	0\n";
		return strdup (p);
}

static int i8051_op(RAnal *anal, RAnalOp *op, ut64 addr, const ut8 *buf, int len) {
	op->delay = 0;

	int i = 0;
	while (_8051_ops[i].string && _8051_ops[i].op != (buf[0] & ~_8051_ops[i].mask))	{
		i++;
	}

	op->size = _8051_ops[i].len;
	op->jump = op->fail = -1;
	op->ptr = op->val = -1;

	ut8 arg1 = _8051_ops[i].arg1;
	ut8 arg2 = _8051_ops[i].arg2;

	switch (arg1) {
	case A_DIRECT:
		op->ptr = buf[1];
		break;
	case A_BIT:
		op->ptr = arg_bit (buf[1]);
		break;
	case A_IMMEDIATE:
		op->val = buf[1];
		break;
	case A_IMM16:
		op->val = buf[1] * 256 + buf[2];
		break;
	default:
		break;
	}

	switch (arg2) {
	case A_DIRECT:
		op->ptr = (arg1 == A_RI || arg1 == A_RN) ? buf[1] : buf[2];
		break;
	case A_BIT:
		op->ptr = arg_bit ((arg1 == A_RI || arg1 == A_RN) ? buf[1] : buf[2]);
		break;
	case A_IMMEDIATE:
		op->val = (arg1 == A_RI || arg1 == A_RN) ? buf[1] : buf[2];
		break;
	default:
		break;
	}

	switch(_8051_ops[i].instr) {
	case OP_PUSH:
		op->type = R_ANAL_OP_TYPE_PUSH;
		op->ptr = 0;
		op->stackop = R_ANAL_STACK_INC;
		op->stackptr = 1;
		break;
	case OP_POP:
		op->type = R_ANAL_OP_TYPE_POP;
		op->ptr = 0;
		op->stackop = R_ANAL_STACK_INC;
		op->stackptr = -1;
		break;
	case OP_RET:
		op->type = R_ANAL_OP_TYPE_RET;
		op->stackop = R_ANAL_STACK_INC;
		op->stackptr = -2;
		break;
	case OP_NOP:
		op->type = R_ANAL_OP_TYPE_NOP;
		break;
	case OP_INC:
	case OP_ADD:
	case OP_ADDC:
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case OP_DEC:
	case OP_SUBB:
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case OP_ANL:
		op->type = R_ANAL_OP_TYPE_AND;
		break;
	case OP_ORL:
		op->type = R_ANAL_OP_TYPE_OR;
		break;
	case OP_XRL:
		op->type = R_ANAL_OP_TYPE_XOR;
		break;
	case OP_CPL:
		op->type = R_ANAL_OP_TYPE_CPL;
		break;
	case OP_XCH:
		op->type = R_ANAL_OP_TYPE_XCHG;
		break;
	case OP_MOV:
		op->type = R_ANAL_OP_TYPE_MOV;
		break;
	case OP_MUL:
		op->type = R_ANAL_OP_TYPE_MUL;
		break;
	case OP_DIV:
		op->type = R_ANAL_OP_TYPE_DIV;
		break;
	case OP_CALL:
		op->type = R_ANAL_OP_TYPE_CALL;
		if (arg1 == A_ADDR11) {
			op->jump = arg_addr11 (addr + op->size, buf);
			op->fail = addr + op->size;
		} else if (arg1 == A_ADDR16) {
			op->jump = 0x100 * buf[1] + buf[2];
			op->fail = addr + op->size;
		}
		break;
	case OP_JMP:
		op->type = R_ANAL_OP_TYPE_JMP;
		if (arg1 == A_ADDR11) {
			op->jump = arg_addr11 (addr + op->size, buf);
			op->fail = addr + op->size;
		} else if (arg1 == A_ADDR16) {
			op->jump = 0x100 * buf[1] + buf[2];
			op->fail = addr + op->size;
		} else if (arg1 == A_OFFSET) {
			op->jump = arg_offset (addr + op->size, buf[1]);
			op->fail = addr + op->size;
		}
		break;
	case OP_CJNE:
	case OP_DJNZ:
	case OP_JC:
	case OP_JNC:
	case OP_JZ:
	case OP_JNZ:
	case OP_JB:
	case OP_JBC:
	case OP_JNB:
		op->type = R_ANAL_OP_TYPE_CJMP;
		if (op->size == 2) {
			op->jump = arg_offset (addr + 2, buf[1]);
			op->fail = addr + 2;
		} else if (op->size == 3) {
			op->jump = arg_offset (addr + 3, buf[2]);
			op->fail = addr + 3;
		}
		break;
	case OP_INVALID:
		op->type = R_ANAL_OP_TYPE_ILL;
		break;
	default:
		op->type = R_ANAL_OP_TYPE_UNK;
		break;
	}

	if (op->ptr != -1 && op->refptr == 0) {
		op->refptr = 1;
	}

	if (anal->decode) {
		ut8 copy[3] = {0, 0, 0};
		memcpy (copy, buf, len >= 3 ? 3 : len);
		analop_esil (anal, op, addr, copy);
	}

	return op->size;
}

RAnalPlugin r_anal_plugin_8051 = {
	.name = "8051",
	.arch = "8051",
	.esil = true,
	.bits = 8|16,
	.desc = "8051 CPU code analysis plugin",
	.license = "LGPL3",
	.op = &i8051_op,
	.get_reg_profile = &get_reg_profile,
	.esil_init = esil_i8051_init,
	.esil_fini = esil_i8051_fini
};

#ifndef CORELIB
RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_ANAL,
	.data = &r_anal_plugin_8051,
	.version = R2_VERSION
};
#endif
