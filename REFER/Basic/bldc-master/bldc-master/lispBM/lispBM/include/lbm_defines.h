/*
    Copyright 2022 Joel Svensson        svenssonjoel@yahoo.se

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef LBM_DEFINES_H_
#define LBM_DEFINES_H_


/* ------------------------------------------------------------
   Types
   ------------------------------------------------------------ */

#ifndef LBM64

#define LBM_POINTER_TYPE_FIRST           0x10000000u
#define LBM_TYPE_CONS                    0x10000000u
#define LBM_TYPE_CONS_CONST              0x14000000u
#define LBM_NON_CONS_POINTER_TYPE_FIRST  0x20000000u
#define LBM_TYPE_U32                     0x28000000u
#define LBM_TYPE_I32                     0x38000000u
#define LBM_TYPE_I64                     0x48000000u
#define LBM_TYPE_U64                     0x58000000u
#define LBM_TYPE_FLOAT                   0x68000000u
#define LBM_TYPE_DOUBLE                  0x78000000u
#define LBM_TYPE_ARRAY                   0x80000000u
#define LBM_TYPE_CHANNEL                 0x90000000u
#define LBM_TYPE_CUSTOM                  0xA0000000u
#define LBM_NON_CONS_POINTER_TYPE_LAST   0xAC000000u
#define LBM_POINTER_TYPE_LAST            0xAC000000u

#define LBM_CONTINUATION_INTERNAL        0xF8000001u // PTR bit set
#define LBM_CONTINUATION_INTERNAL_TYPE   0xF8000000u

#define LBM_GC_MASK                      0x00000002u
#define LBM_GC_MARKED                    0x00000002u

#define LBM_VAL_MASK                     0xFFFFFFF0u
#define LBM_VAL_TYPE_MASK                0x0000000Cu
                                                     //    gc ptr
#define LBM_TYPE_SYMBOL                  0x00000000u // 00  0   0
#define LBM_TYPE_CHAR                    0x00000004u // 01  0   0
#define LBM_TYPE_BYTE                    0x00000004u
#define LBM_TYPE_U                       0x00000008u // 10  0   0
#define LBM_TYPE_I                       0x0000000Cu // 11  0   0

#else /* 64 bit Version */

#define LBM_POINTER_TYPE_FIRST           (lbm_uint)0x1000000000000000
#define LBM_TYPE_CONS                    (lbm_uint)0x1000000000000000
#define LBM_NON_CONS_POINTER_TYPE_FIRST  (lbm_uint)0x2000000000000000
#define LBM_TYPE_U64                     (lbm_uint)0x2800000000000000
#define LBM_TYPE_I64                     (lbm_uint)0x3800000000000000
#define LBM_TYPE_DOUBLE                  (lbm_uint)0x4800000000000000
#define LBM_TYPE_ARRAY                   (lbm_uint)0x5000000000000000
#define LBM_TYPE_CHANNEL                 (lbm_uint)0x7000000000000000
#define LBM_TYPE_CUSTOM                  (lbm_uint)0x8000000000000000
#define LBM_NON_CONS_POINTER_TYPE_LAST   (lbm_uint)0x8000000000000000
#define LBM_POINTER_TYPE_LAST            (lbm_uint)0x8000000000000000

#define LBM_CONTINUATION_INTERNAL        (lbm_uint)0xF800000000000001
#define LBM_CONTINUATION_INTERNAL_TYPE   (lbm_uint)0xF800000000000000

#define LBM_GC_MASK                      (lbm_uint)0x2
#define LBM_GC_MARKED                    (lbm_uint)0x2

/* 8 - 2 free bits to encode type information into */
#define LBM_VAL_MASK                    (lbm_uint)0xFFFFFFFFFFFFFF00
#define LBM_VAL_TYPE_MASK               (lbm_uint)0xFC
//    gc ptr
#define LBM_TYPE_SYMBOL                 (lbm_uint)0x0 // 00 00 00  0   0
#define LBM_TYPE_CHAR                   (lbm_uint)0x4 // 00 00 01  0   0
#define LBM_TYPE_BYTE                   (lbm_uint)0x4
#define LBM_TYPE_U                      (lbm_uint)0x8 // 00 00 10  0   0
#define LBM_TYPE_I                      (lbm_uint)0xC // 00 00 11  0   0
#define LBM_TYPE_U32                    (lbm_uint)0x14// 00 01 01  0   0
#define LBM_TYPE_I32                    (lbm_uint)0x18// 00 01 10  0   0
#define LBM_TYPE_FLOAT                  (lbm_uint)0x1C// 00 01 11  0   0

#endif
/* ------------------------------------------------------------
   Built in symbols
   ------------------------------------------------------------ */

// Default and fixed symbol ids
#define SYM_NIL           0x0
#define SYM_TRUE          0x2
#define SYM_DONTCARE      0x9
#define SYM_TIMEOUT       0xA

// 0x20 - 0x2F are errors
#define SYM_RERROR                0x20  /* READ ERROR */
#define SYM_TERROR                0x21  /* TYPE ERROR */
#define SYM_EERROR                0x22  /* EVAL ERROR */
#define SYM_MERROR                0x23
#define SYM_NOT_FOUND             0x24
#define SYM_DIVZERO               0x25
#define SYM_FATAL_ERROR           0x26 /* Runtime system is corrupt */
#define SYM_STACK_ERROR           0x27
#define SYM_RECOVERED             0x28
#define SYM_ERROR_FLASH_HEAP_FULL 0x29


#define TYPE_CLASSIFIER_STARTS 0x30
#define SYM_ARRAY_TYPE     0x30
#define SYM_RAW_I_TYPE     0x31
#define SYM_RAW_U_TYPE     0x32
#define SYM_RAW_F_TYPE     0x33
#define SYM_IND_I_TYPE     0x34
#define SYM_IND_U_TYPE     0x35
#define SYM_IND_F_TYPE     0x36
#define SYM_CHANNEL_TYPE   0x37
#define SYM_CUSTOM_TYPE    0x38
#define TYPE_CLASSIFIER_ENDS 0x39
#define SYM_NONSENSE       0x3A

#define SYM_NO_MATCH       0x40
#define SYM_MATCH_ANY      0x41

// Type identifying symbols
#define SYM_TYPE_LIST      0x50
#define SYM_TYPE_I         0x51
#define SYM_TYPE_U         0x52
#define SYM_TYPE_FLOAT     0x53
#define SYM_TYPE_I32       0x54
#define SYM_TYPE_U32       0x55
#define SYM_TYPE_DOUBLE    0x56
#define SYM_TYPE_I64       0x57
#define SYM_TYPE_U64       0x58
#define SYM_TYPE_ARRAY     0x59
#define SYM_TYPE_SYMBOL    0x5A
#define SYM_TYPE_CHAR      0x5B
#define SYM_TYPE_BYTE      0x5C
#define SYM_TYPE_CHANNEL   0x5E

//Relevant for the tokenizer and reader
#define TOKENIZER_SYMBOLS_START 0x70
#define SYM_OPENPAR          0x70
#define SYM_CLOSEPAR         0x71
#define SYM_BACKQUOTE        0x72
#define SYM_COMMA            0x73
#define SYM_COMMAAT          0x74
#define SYM_TOKENIZER_DONE   0x75
#define SYM_DOT              0x76
#define SYM_QUOTE_IT         0x77
#define SYM_COLON            0x78
#define SYM_TOKENIZER_WAIT   0x79
#define SYM_OPENBRACK        0x80
#define SYM_CLOSEBRACK       0x81
#define SYM_TOKENIZER_RERROR 0x82
#define SYM_OPENCURL         0x84
#define SYM_CONST            0x85
#define TOKENIZER_SYMBOLS_END 0x85

// Built in special forms:
// Special forms get their arguments unevaluated
// and are free to choose to evaluate one or more
// of them as needed.
// Consecutive for lookup-application.
#define SPECIAL_FORMS_START     0x100
#define SYM_QUOTE               0x100
#define SYM_DEFINE              0x101
#define SYM_PROGN               0x102
#define SYM_LAMBDA              0x103
#define SYM_IF                  0x104
#define SYM_LET                 0x105
#define SYM_AND                 0x106
#define SYM_OR                  0x107
#define SYM_MATCH               0x108
#define SYM_RECEIVE             0x109
#define SYM_RECEIVE_TIMEOUT     0x10A
#define SYM_CALLCC              0x10B
#define SYM_ATOMIC              0x10C
#define SYM_MACRO               0x10D
#define SYM_CONT                0x10E
#define SYM_CLOSURE             0x10F
#define SYM_COND                0x110
#define SYM_APP_CONT            0x111
#define SYM_PROGN_VAR           0x112
#define SYM_SETQ                0x113
#define SYM_MOVE_TO_FLASH       0x114
#define SPECIAL_FORMS_END       0x114

// Apply funs:
// Get their arguments in evaluated form.
// Consecutive value symbols for lookup-application
#define APPLY_FUNS_START          0x150
#define SYM_SETVAR                0x150
#define SYM_READ                  0x151
#define SYM_READ_PROGRAM          0x152
#define SYM_READ_AND_EVAL_PROGRAM 0x153
#define SYM_SPAWN                 0x154
#define SYM_SPAWN_TRAP            0x155
#define SYM_YIELD                 0x156
#define SYM_WAIT                  0x157
#define SYM_EVAL                  0x158
#define SYM_EVAL_PROGRAM          0x159 
#define SYM_SEND                  0x15A
#define SYM_EXIT_OK               0x15B
#define SYM_EXIT_ERROR            0x15C
#define SYM_MAP                   0x15D
#define SYM_REVERSE               0x15E
#define SYM_WAIT_FOR              0x15F
#define SYM_FLATTEN               0x160
#define SYM_UNFLATTEN             0x161
#define SYM_KILL                  0x162
#define APPLY_FUNS_END            0x162

#define FUNDAMENTALS_START 0x20E
#define SYM_ADD           0x20E
#define SYM_SUB           0x20F
#define SYM_MUL           0x210
#define SYM_DIV           0x211
#define SYM_MOD           0x212
#define SYM_EQ            0x213
#define SYM_NOT_EQ        0x214
#define SYM_NUMEQ         0x215
#define SYM_NUM_NOT_EQ    0x216
#define SYM_LT            0x217
#define SYM_GT            0x218
#define SYM_LEQ           0x219
#define SYM_GEQ           0x21A
#define SYM_NOT           0x21B
#define SYM_PERFORM_GC          0x21C
#define SYM_SELF                0x21D
#define SYM_SET_MAILBOX_SIZE    0x21E
#define SYM_CONS                0x21F
#define SYM_CAR                 0x220
#define SYM_CDR                 0x221
#define SYM_LIST                0x222
#define SYM_APPEND              0x223
#define SYM_UNDEFINE            0x224
#define SYM_ARRAY_CREATE        0x225
#define SYM_SYMBOL_TO_STRING    0x226
#define SYM_STRING_TO_SYMBOL    0x227
#define SYM_SYMBOL_TO_UINT      0x228
#define SYM_UINT_TO_SYMBOL      0x229
#define SYM_SET_CAR             0x22A
#define SYM_SET_CDR             0x22B
#define SYM_SET_IX              0x22C
#define SYM_ASSOC               0x22D
#define SYM_ACONS               0x22E
#define SYM_SET_ASSOC           0x22F
#define SYM_COSSA               0x230
#define SYM_IX                  0x231
#define SYM_TO_I                0x232
#define SYM_TO_I32              0x233
#define SYM_TO_U                0x234
#define SYM_TO_U32              0x235
#define SYM_TO_FLOAT            0x236
#define SYM_TO_I64              0x237
#define SYM_TO_U64              0x238
#define SYM_TO_DOUBLE           0x239
#define SYM_TO_BYTE             0x23A
#define SYM_SHL                 0x23B
#define SYM_SHR                 0x23C
#define SYM_BITWISE_AND         0x23D
#define SYM_BITWISE_OR          0x23E
#define SYM_BITWISE_XOR         0x23F
#define SYM_BITWISE_NOT         0x240
#define SYM_CUSTOM_DESTRUCT     0x241 /* run the destructor of a custom type */
#define SYM_TYPE_OF             0x242
#define SYM_LIST_LENGTH         0x243
#define SYM_RANGE               0x244
#define SYM_REG_EVENT_HANDLER   0x245
#define SYM_TAKE                0x246
#define SYM_DROP                0x247
#define FUNDAMENTALS_END        0x249

#define SPECIAL_SYMBOLS_START    0
#define SPECIAL_SYMBOLS_END      0xFFFF
#define EXTENSION_SYMBOLS_START  0x10000
#define EXTENSION_SYMBOLS_END    0x1FFFF
#define VARIABLE_SYMBOLS_START   0x20000
#define VARIABLE_SYMBOLS_END     0x2FFFF
#define RUNTIME_SYMBOLS_START    0x30000
#define MAX_SYMBOL_VALUE 0x0FFFFFFF

/* ------------------------------------------------------------
   Encoded Symbols
   ------------------------------------------------------------ */

#define ENC_SYM(X) (((X) << LBM_VAL_SHIFT) | LBM_TYPE_SYMBOL)

#define ENC_SYM_NIL           ENC_SYM(SYM_NIL)
#define ENC_SYM_TRUE          ENC_SYM(SYM_TRUE)
#define ENC_SYM_DONTCARE      ENC_SYM(SYM_DONTCARE)
#define ENC_SYM_TIMEOUT       ENC_SYM(SYM_TIMEOUT)

#define ENC_SYM_RERROR                ENC_SYM(SYM_RERROR)
#define ENC_SYM_TERROR                ENC_SYM(SYM_TERROR)
#define ENC_SYM_EERROR                ENC_SYM(SYM_EERROR)
#define ENC_SYM_MERROR                ENC_SYM(SYM_MERROR)
#define ENC_SYM_NOT_FOUND             ENC_SYM(SYM_NOT_FOUND)
#define ENC_SYM_DIVZERO               ENC_SYM(SYM_DIVZERO)
#define ENC_SYM_FATAL_ERROR           ENC_SYM(SYM_FATAL_ERROR)
#define ENC_SYM_STACK_ERROR           ENC_SYM(SYM_STACK_ERROR)
#define ENC_SYM_RECOVERED             ENC_SYM(SYM_RECOVERED)
#define ENC_SYM_ERROR_FLASH_HEAP_FULL ENC_SYM(SYM_ERROR_FLASH_HEAP_FULL)

#define ENC_SYM_ARRAY_TYPE     ENC_SYM(SYM_ARRAY_TYPE)
#define ENC_SYM_RAW_I_TYPE     ENC_SYM(SYM_RAW_I_TYPE)
#define ENC_SYM_RAW_U_TYPE     ENC_SYM(SYM_RAW_U_TYPE)
#define ENC_SYM_RAW_F_TYPE     ENC_SYM(SYM_RAW_F_TYPE)
#define ENC_SYM_IND_I_TYPE     ENC_SYM(SYM_IND_I_TYPE)
#define ENC_SYM_IND_U_TYPE     ENC_SYM(SYM_IND_U_TYPE)
#define ENC_SYM_IND_F_TYPE     ENC_SYM(SYM_IND_F_TYPE)
#define ENC_SYM_CHANNEL_TYPE   ENC_SYM(SYM_CHANNEL_TYPE)
#define ENC_SYM_CUSTOM_TYPE    ENC_SYM(SYM_CUSTOM_TYPE)
#define ENC_SYM_NONSENSE       ENC_SYM(SYM_NONSENSE)

#define ENC_SYM_NO_MATCH       ENC_SYM(SYM_NO_MATCH)
#define ENC_SYM_MATCH_ANY      ENC_SYM(SYM_MATCH_ANY)

#define ENC_SYM_TYPE_LIST      ENC_SYM(SYM_TYPE_LIST)
#define ENC_SYM_TYPE_I         ENC_SYM(SYM_TYPE_I)
#define ENC_SYM_TYPE_U         ENC_SYM(SYM_TYPE_U)
#define ENC_SYM_TYPE_FLOAT     ENC_SYM(SYM_TYPE_FLOAT)
#define ENC_SYM_TYPE_I32       ENC_SYM(SYM_TYPE_I32)
#define ENC_SYM_TYPE_U32       ENC_SYM(SYM_TYPE_U32)
#define ENC_SYM_TYPE_DOUBLE    ENC_SYM(SYM_TYPE_DOUBLE)
#define ENC_SYM_TYPE_I64       ENC_SYM(SYM_TYPE_I64)
#define ENC_SYM_TYPE_U64       ENC_SYM(SYM_TYPE_U64)
#define ENC_SYM_TYPE_ARRAY     ENC_SYM(SYM_TYPE_ARRAY)
#define ENC_SYM_TYPE_SYMBOL    ENC_SYM(SYM_TYPE_SYMBOL)
#define ENC_SYM_TYPE_CHAR      ENC_SYM(SYM_TYPE_CHAR)
#define ENC_SYM_TYPE_BYTE      ENC_SYM(SYM_TYPE_BYTE)
#define ENC_SYM_TYPE_CHANNEL   ENC_SYM(SYM_TYPE_CHANNEL)

#define ENC_SYM_OPENPAR          ENC_SYM(SYM_OPENPAR)
#define ENC_SYM_CLOSEPAR         ENC_SYM(SYM_CLOSEPAR)
#define ENC_SYM_BACKQUOTE        ENC_SYM(SYM_BACKQUOTE)
#define ENC_SYM_COMMA            ENC_SYM(SYM_COMMA)
#define ENC_SYM_COMMAAT          ENC_SYM(SYM_COMMAAT)
#define ENC_SYM_TOKENIZER_DONE   ENC_SYM(SYM_TOKENIZER_DONE)
#define ENC_SYM_DOT              ENC_SYM(SYM_DOT)
#define ENC_SYM_QUOTE_IT         ENC_SYM(SYM_QUOTE_IT)
#define ENC_SYM_COLON            ENC_SYM(SYM_COLON)
#define ENC_SYM_TOKENIZER_WAIT   ENC_SYM(SYM_TOKENIZER_WAIT)
#define ENC_SYM_OPENBRACK        ENC_SYM(SYM_OPENBRACK)
#define ENC_SYM_CLOSEBRACK       ENC_SYM(SYM_CLOSEBRACK)
#define ENC_SYM_TOKENIZER_RERROR ENC_SYM(SYM_TOKENIZER_RERROR)
#define ENC_SYM_OPENCURL         ENC_SYM(SYM_OPENCURL)
#define ENC_SYM_CONST            ENC_SYM(SYM_CONST)

#define ENC_SYM_QUOTE               ENC_SYM(SYM_QUOTE)
#define ENC_SYM_DEFINE              ENC_SYM(SYM_DEFINE)
#define ENC_SYM_PROGN               ENC_SYM(SYM_PROGN)
#define ENC_SYM_LAMBDA              ENC_SYM(SYM_LAMBDA)
#define ENC_SYM_IF                  ENC_SYM(SYM_IF)
#define ENC_SYM_LET                 ENC_SYM(SYM_LET)
#define ENC_SYM_AND                 ENC_SYM(SYM_AND)
#define ENC_SYM_OR                  ENC_SYM(SYM_OR)
#define ENC_SYM_MATCH               ENC_SYM(SYM_MATCH)
#define ENC_SYM_RECEIVE             ENC_SYM(SYM_RECEIVE)
#define ENC_SYM_RECEIVE_TIMEOUT     ENC_SYM(SYM_RECEIVE_TIMEOUT)
#define ENC_SYM_CALLCC              ENC_SYM(SYM_CALLCC)
#define ENC_SYM_ATOMIC              ENC_SYM(SYM_ATOMIC)
#define ENC_SYM_MACRO               ENC_SYM(SYM_MACRO)
#define ENC_SYM_CONT                ENC_SYM(SYM_CONT)
#define ENC_SYM_CLOSURE             ENC_SYM(SYM_CLOSURE)
#define ENC_SYM_COND                ENC_SYM(SYM_COND)
#define ENC_SYM_APP_CONT            ENC_SYM(SYM_APP_CONT)
#define ENC_SYM_PROGN_VAR           ENC_SYM(SYM_PROGN_VAR)
#define ENC_SYM_SETQ                ENC_SYM(SYM_SETQ)
#define ENC_SYM_MOVE_TO_FLASH       ENC_SYM(SYM_MOVE_TO_FLASH)
#define ENC_SYM_IN_ENV              ENC_SYM(SYM_IN_ENV)

#define ENC_SYM_SETVAR                ENC_SYM(SYM_SETVAR)
#define ENC_SYM_READ                  ENC_SYM(SYM_READ)
#define ENC_SYM_READ_PROGRAM          ENC_SYM(SYM_READ_PROGRAM)
#define ENC_SYM_READ_AND_EVAL_PROGRAM ENC_SYM(SYM_READ_AND_EVAL_PROGRAM)
#define ENC_SYM_SPAWN                 ENC_SYM(SYM_SPAWN)
#define ENC_SYM_SPAWN_TRAP            ENC_SYM(SYM_SPAWN_TRAP)
#define ENC_SYM_YIELD                 ENC_SYM(SYM_YIELD)
#define ENC_SYM_WAIT                  ENC_SYM(SYM_WAIT)
#define ENC_SYM_EVAL                  ENC_SYM(SYM_EVAL)
#define ENC_SYM_EVAL_PROGRAM          ENC_SYM(SYM_EVAL_PROGRAM)
#define ENC_SYM_SEND                  ENC_SYM(SYM_SEND)
#define ENC_SYM_EXIT_OK               ENC_SYM(SYM_EXIT_OK)
#define ENC_SYM_EXIT_ERROR            ENC_SYM(SYM_EXIT_ERROR)
#define ENC_SYM_MAP                   ENC_SYM(SYM_MAP)
#define ENC_SYM_REVERSE               ENC_SYM(SYM_REVERSE)
#define ENC_SYM_WAIT_FOR              ENC_SYM(SYM_WAIT_FOR)
#define ENC_SYM_FLATTEN               ENC_SYM(SYM_FLATTEN)
#define ENC_SYM_UNFLATTEN             ENC_SYM(SYM_UNFLATTEN)
#define ENC_SYM_KILL                  ENC_SYM(SYM_KILL)

#define ENC_SYM_ADD           ENC_SYM(SYM_ADD)
#define ENC_SYM_SUB           ENC_SYM(SYM_SUB)
#define ENC_SYM_MUL           ENC_SYM(SYM_MUL)
#define ENC_SYM_DIV           ENC_SYM(SYM_DIV)
#define ENC_SYM_MOD           ENC_SYM(SYM_MOD)
#define ENC_SYM_EQ            ENC_SYM(SYM_EQ)
#define ENC_SYM_NOT_EQ        ENC_SYM(SYM_NOT_EQ)
#define ENC_SYM_NUMEQ         ENC_SYM(SYM_NUMEQ)
#define ENC_SYM_NUM_NOT_EQ    ENC_SYM(SYM_NUM_NOT_EQ)
#define ENC_SYM_LT            ENC_SYM(SYM_LT)
#define ENC_SYM_GT            ENC_SYM(SYM_GT)
#define ENC_SYM_LEQ           ENC_SYM(SYM_LEQ)
#define ENC_SYM_GEQ           ENC_SYM(SYM_GEQ)
#define ENC_SYM_NOT           ENC_SYM(SYM_NOT)
#define ENC_SYM_PERFORM_GC          ENC_SYM(SYM_PERFORM_GC)
#define ENC_SYM_SELF                ENC_SYM(SYM_SELF)
#define ENC_SYM_SET_MAILBOX_SIZE    ENC_SYM(SYM_SET_MAILBOX_SIZE)
#define ENC_SYM_CONS                ENC_SYM(SYM_CONS)
#define ENC_SYM_CAR                 ENC_SYM(SYM_CAR)
#define ENC_SYM_CDR                 ENC_SYM(SYM_CDR)
#define ENC_SYM_LIST                ENC_SYM(SYM_LIST)
#define ENC_SYM_APPEND              ENC_SYM(SYM_APPEND)
#define ENC_SYM_UNDEFINE            ENC_SYM(SYM_UNDEFINE)
#define ENC_SYM_ARRAY_CREATE        ENC_SYM(SYM_ARRAY_CREATE)
#define ENC_SYM_SYMBOL_TO_STRING    ENC_SYM(SYM_ENC_SYMBOL_TO_STRING)
#define ENC_SYM_STRING_TO_SYMBOL    ENC_SYM(SYM_STRING_TO_SYMBOL)
#define ENC_SYM_SYMBOL_TO_UINT      ENC_SYM(SYM_SYMBOL_TO_UINT)
#define ENC_SYM_UINT_TO_SYMBOL      ENC_SYM(SYM_UINT_TO_SYMBOL)
#define ENC_SYM_SET_CAR             ENC_SYM(SYM_SET_CAR)
#define ENC_SYM_SET_CDR             ENC_SYM(SYM_SET_CDR)
#define ENC_SYM_SET_IX              ENC_SYM(SYM_SET_IX)
#define ENC_SYM_ASSOC               ENC_SYM(SYM_ASSOC)
#define ENC_SYM_ACONS               ENC_SYM(SYM_ACONS)
#define ENC_SYM_SET_ASSOC           ENC_SYM(SYM_SET_ASSOC)
#define ENC_SYM_COSSA               ENC_SYM(SYM_COSSA)
#define ENC_SYM_IX                  ENC_SYM(SYM_IX)
#define ENC_SYM_TO_I                ENC_SYM(SYM_TO_I)
#define ENC_SYM_TO_I32              ENC_SYM(SYM_TO_I32)
#define ENC_SYM_TO_U                ENC_SYM(SYM_TO_U)
#define ENC_SYM_TO_U32              ENC_SYM(SYM_TO_U32)
#define ENC_SYM_TO_FLOAT            ENC_SYM(SYM_TO_FLOAT)
#define ENC_SYM_TO_I64              ENC_SYM(SYM_TO_I64)
#define ENC_SYM_TO_U64              ENC_SYM(SYM_TO_U64)
#define ENC_SYM_TO_DOUBLE           ENC_SYM(SYM_TO_DOUBLE)
#define ENC_SYM_TO_BYTE             ENC_SYM(SYM_TO_BYTE)
#define ENC_SYM_SHL                 ENC_SYM(SYM_SHL)
#define ENC_SYM_SHR                 ENC_SYM(SYM_SHR)
#define ENC_SYM_BITWISE_AND         ENC_SYM(SYM_BITWISE_AND)
#define ENC_SYM_BITWISE_OR          ENC_SYM(SYM_BITWISE_OR)
#define ENC_SYM_BITWISE_XOR         ENC_SYM(SYM_BITWISE_XOR)
#define ENC_SYM_BITWISE_NOT         ENC_SYM(SYM_BITWISE_NOT)
#define ENC_SYM_CUSTOM_DESTRUCT     ENC_SYM(SYM_CUSTOM_DESTRUCT)
#define ENC_SYM_TYPE_OF             ENC_SYM(SYM_TYPE_OF)
#define ENC_SYM_LIST_LENGTH         ENC_SYM(SYM_LIST_LENGTH)
#define ENC_SYM_RANGE               ENC_SYM(SYM_RANGE)
#define ENC_SYM_REG_EVENT_HANDLER   ENC_SYM(SYM_REG_EVENT_HANDLER)
#define ENC_SYM_TAKE                ENC_SYM(SYM_TAKE)
#define ENC_SYM_DROP                ENC_SYM(SYM_DROP)

#endif
