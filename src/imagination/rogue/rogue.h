/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef ROGUE_H
#define ROGUE_H

/**
 * \file rogue.h
 *
 * \brief Main header.
 */

#define __pvr_address_type pvr_dev_addr_t
#define __pvr_get_address(pvr_dev_addr) (pvr_dev_addr).addr
/* clang-format off */
#define __pvr_make_address(addr_u64) PVR_DEV_ADDR(addr_u64)
/* clang-format on */

#include "pvr_types.h"
#include "csbgen/rogue_hwdefs.h"
#include "vulkan/pvr_formats.h"
#include "vulkan/pvr_limits.h"
#include "vulkan/pvr_common.h"

#include "compiler/nir/nir.h"
#include "compiler/shader_enums.h"
#include "compiler/spirv/nir_spirv.h"
#include "rogue_isa.h"
#include "util/bitscan.h"
#include "util/bitset.h"
#include "util/compiler.h"
#include "util/list.h"
#include "util/sparse_array.h"
#include "util/ralloc.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* Coefficient registers are typically used in groups of 4. */
#define ROGUE_COEFF_ALIGN 4

#define ROGUE_REG_UNUSED ~0

/* All registers are 32-bit in size. */
#define ROGUE_REG_SIZE_BYTES 4

/** Rogue register classes. */
enum rogue_reg_class {
   ROGUE_REG_CLASS_INVALID = 0,

   ROGUE_REG_CLASS_SSA, /** SSA register. */
   ROGUE_REG_CLASS_EMC, /** Execution mask counter register. */

   ROGUE_REG_CLASS_TEMP, /** Temp register. */
   ROGUE_REG_CLASS_COEFF, /** Coefficient register. */
   ROGUE_REG_CLASS_SHARED, /** Shared register. */

   ROGUE_REG_CLASS_SPECIAL, /** Special register. */
   ROGUE_REG_CLASS_INTERNAL, /** Internal register. */
   ROGUE_REG_CLASS_CONST, /** Constant register. */
   ROGUE_REG_CLASS_PIXOUT, /** Pixel output register. */

   ROGUE_REG_CLASS_VTXIN, /** Vertex input register. */
   ROGUE_REG_CLASS_VTXOUT, /** Vertex output register. */

   ROGUE_REG_CLASS_IDX0, /** Index register 0. */
   ROGUE_REG_CLASS_IDX1, /** Index register 1. */

   ROGUE_REG_CLASS_COUNT,
} PACKED;

typedef struct rogue_reg_class_info {
   const char *name; /** Human-readable name. */
   const char *str; /** Register prefix. */
   unsigned num; /** Number of hardware registers available. */
   uint64_t supported_io_srcs;
} rogue_reg_class_info;

extern const rogue_reg_class_info rogue_reg_class_infos[ROGUE_REG_CLASS_COUNT];

enum rogue_special_reg {
   ROGUE_SPECIAL_REG_CONSTS_0_START = 0,
   ROGUE_SPECIAL_REG_CONSTS_0_END = 31,

   ROGUE_SPECIAL_REG_PIXOUT_0 = 32,
   ROGUE_SPECIAL_REG_PIXOUT_1 = 33,
   ROGUE_SPECIAL_REG_PIXOUT_2 = 34,
   ROGUE_SPECIAL_REG_PIXOUT_3 = 35,

   ROGUE_SPECIAL_REG_INTL_0 = 36,
   ROGUE_SPECIAL_REG_INTL_1 = 37,
   ROGUE_SPECIAL_REG_INTL_2 = 38,
   ROGUE_SPECIAL_REG_INTL_3 = 39,
   ROGUE_SPECIAL_REG_INTL_4 = 40,
   ROGUE_SPECIAL_REG_INTL_5 = 41,
   ROGUE_SPECIAL_REG_INTL_6 = 42,
   ROGUE_SPECIAL_REG_INTL_7 = 43,

   ROGUE_SPECIAL_REG_FACE_ORIENT = 44,
   ROGUE_SPECIAL_REG_CLUSTER_NUM = 45,
   ROGUE_SPECIAL_REG_OUTPUT_PART = 46,
   ROGUE_SPECIAL_REG_TASK_ID = 47,
   ROGUE_SPECIAL_REG_SLOT_NUM = 48,
   ROGUE_SPECIAL_REG_TILE_X_PIX = 49,
   ROGUE_SPECIAL_REG_TILE_Y_PIX = 50,
   ROGUE_SPECIAL_REG_INST_NUM = 51,
   ROGUE_SPECIAL_REG_DM_TASK_TYPE = 52,
   ROGUE_SPECIAL_REG_SAMP_NUM = 53,

   ROGUE_SPECIAL_REG_TILED_LD_COMP_0 = 54,
   ROGUE_SPECIAL_REG_TILED_LD_COMP_1 = 55,
   ROGUE_SPECIAL_REG_TILED_LD_COMP_2 = 56,
   ROGUE_SPECIAL_REG_TILED_LD_COMP_3 = 57,

   ROGUE_SPECIAL_REG_TILED_ST_COMP_0 = 58,
   ROGUE_SPECIAL_REG_TILED_ST_COMP_1 = 59,
   ROGUE_SPECIAL_REG_TILED_ST_COMP_2 = 60,
   ROGUE_SPECIAL_REG_TILED_ST_COMP_3 = 61,

   ROGUE_SPECIAL_REG_BATCH_NUM = 62,
   ROGUE_SPECIAL_REG_INST_VALID = 63,

   ROGUE_SPECIAL_REG_CONSTS_1_START = 64,
   ROGUE_SPECIAL_REG_CONSTS_1_END = 95,

   ROGUE_SPECIAL_REG_TILE_XY = 96,

   ROGUE_SPECIAL_REG_X_P = 97,
   ROGUE_SPECIAL_REG_X_S = 98,

   ROGUE_SPECIAL_REG_Y_P = 100,
   ROGUE_SPECIAL_REG_Y_S = 101,

   ROGUE_SPECIAL_REG_SH_ALLOC_SIZE = 103,

   ROGUE_SPECIAL_REG_G0 = 104,
   ROGUE_SPECIAL_REG_G1 = 105,
   ROGUE_SPECIAL_REG_G2 = 106,
   ROGUE_SPECIAL_REG_G3 = 107,
   ROGUE_SPECIAL_REG_G4 = 108,
   ROGUE_SPECIAL_REG_G5 = 109,
   ROGUE_SPECIAL_REG_G6 = 110,
   ROGUE_SPECIAL_REG_G7 = 111,

   ROGUE_SPECIAL_REG_LOCAL_ADDR_INST_NUM = 112,

   ROGUE_SPECIAL_REG_TILE_X_P = 114,
   ROGUE_SPECIAL_REG_TILE_X_S = 115,
   ROGUE_SPECIAL_REG_TILE_Y_P = 116,
   ROGUE_SPECIAL_REG_TILE_Y_S = 117,

   ROGUE_SPECIAL_REG_RENDER_TGT_ID = 118,

   ROGUE_SPECIAL_REG_TILED_LD_COMP_4 = 119,
   ROGUE_SPECIAL_REG_TILED_LD_COMP_5 = 120,
   ROGUE_SPECIAL_REG_TILED_LD_COMP_6 = 121,
   ROGUE_SPECIAL_REG_TILED_LD_COMP_7 = 122,

   ROGUE_SPECIAL_REG_TILED_ST_COMP_4 = 123,
   ROGUE_SPECIAL_REG_TILED_ST_COMP_5 = 124,
   ROGUE_SPECIAL_REG_TILED_ST_COMP_6 = 125,
   ROGUE_SPECIAL_REG_TILED_ST_COMP_7 = 126,

   ROGUE_SPECIAL_REG_CONSTS_2_START = 128,
   ROGUE_SPECIAL_REG_CONSTS_2_END = 159,

   ROGUE_SPECIAL_REG_TIMER_80NS = 160,

   ROGUE_SPECIAL_REG_PIXOUT_4 = 164,
   ROGUE_SPECIAL_REG_PIXOUT_5 = 165,
   ROGUE_SPECIAL_REG_PIXOUT_6 = 166,
   ROGUE_SPECIAL_REG_PIXOUT_7 = 167,

   ROGUE_SPECIAL_REG_COUNT = 256,
};

typedef struct rogue_special_reg_info {
   const char *str;
   bool valid;
   bool needs_olchk;
} rogue_special_reg_info;

extern const rogue_special_reg_info
   rogue_special_reg_infos[ROGUE_SPECIAL_REG_COUNT];

enum rogue_mutex_op {
   ROGUE_MUTEX_OP_LOCK,
   ROGUE_MUTEX_OP_RELEASE,
};

enum rogue_mutex_id {
   ROGUE_MUTEX_ID_ATOMIC_EMU,

   ROGUE_MUTEX_ID_COUNT,
};
static_assert(ROGUE_MUTEX_ID_COUNT <= 16, "Too many mutex IDs.");

#define ROGUE_ISA_DSTS 2
#define ROGUE_ISA_SRCS 6
#define ROGUE_ISA_ISSS 6

#define ROGUE_ISA_ICACHE_ALIGN 8

typedef struct rogue_reg_dst_info {
   unsigned num_dsts;
   unsigned bank_bits[ROGUE_ISA_DSTS];
   unsigned index_bits[ROGUE_ISA_DSTS];
   unsigned bytes;
} rogue_reg_dst_info;

#define ROGUE_REG_DST_VARIANTS 5
extern const rogue_reg_dst_info rogue_reg_dst_infos[ROGUE_REG_DST_VARIANTS];

typedef struct rogue_reg_src_info {
   unsigned num_srcs;
   unsigned mux_bits;
   unsigned bank_bits[ROGUE_ISA_SRCS / 2];
   unsigned index_bits[ROGUE_ISA_SRCS / 2];
   unsigned bytes;
} rogue_reg_src_info;

#define ROGUE_REG_SRC_VARIANTS 8
extern const rogue_reg_src_info
   rogue_reg_lower_src_infos[ROGUE_REG_SRC_VARIANTS];
extern const rogue_reg_src_info
   rogue_reg_upper_src_infos[ROGUE_REG_SRC_VARIANTS];

typedef struct rogue_shader rogue_shader;
typedef struct rogue_reg rogue_reg;
typedef struct rogue_regarray rogue_regarray;

/** Rogue register. */
typedef struct rogue_reg {
   rogue_shader *shader; /** Pointer back to shader. */
   enum rogue_reg_class class; /** Register class. */

   struct list_head link; /** Link in rogue_shader::regs. */
   struct list_head writes; /** List of all writes to this register. */
   struct list_head uses; /** List of all register uses. */

   rogue_regarray *regarray;

   bool dirty;
   uint32_t index; /** Register index. */

   rogue_reg **cached;
} rogue_reg;

#define rogue_foreach_reg(reg, shader, class) \
   list_for_each_entry (rogue_reg, reg, &(shader)->regs[class], link)

#define rogue_foreach_reg_safe(reg, shader, class) \
   list_for_each_entry_safe (rogue_reg, reg, &(shader)->regs[class], link)

#define REG_CACHE_KEY_COMPONENT_BITS 4
#define REG_CACHE_KEY_INDEX_BITS 27
#define REG_CACHE_KEY_VEC_BITS 1

struct rogue_reg_cache_key {
   union {
      struct {
         uint32_t component : REG_CACHE_KEY_COMPONENT_BITS;
         uint32_t index : REG_CACHE_KEY_INDEX_BITS;
         uint32_t vec : REG_CACHE_KEY_VEC_BITS;
      } PACKED;

      uint32_t val;
   } PACKED;
} PACKED;
static_assert(sizeof(struct rogue_reg_cache_key) == sizeof(uint32_t),
              "sizeof(struct rogue_reg_cache_key) != sizeof(uint32_t)");

static inline uint32_t
rogue_reg_cache_key(unsigned index, bool vec, unsigned component)
{
   assert(util_last_bit(component) <= REG_CACHE_KEY_COMPONENT_BITS);
   assert(!vec || util_last_bit(index) <= REG_CACHE_KEY_INDEX_BITS);
   assert(vec || util_last_bit(index) <= 32);
   assert(util_last_bit(vec) <= REG_CACHE_KEY_VEC_BITS);

   if (!vec)
      return index;

   return (struct rogue_reg_cache_key){ .component = component,
                                        .index = index,
                                        .vec = vec }
      .val;
}

static inline bool rogue_reg_is_unused(rogue_reg *reg)
{
   return list_is_empty(&reg->uses) && list_is_empty(&reg->writes);
}

struct rogue_regarray_cache_key {
   union {
      struct {
         uint32_t start_index;
         enum rogue_reg_class class;
         uint16_t size;
      } PACKED;

      uint64_t val;
   } PACKED;
} PACKED;
static_assert(sizeof(struct rogue_regarray_cache_key) == sizeof(uint64_t),
              "sizeof(struct rogue_regarray_cache_key) != sizeof(uint64_t)");

static inline uint64_t rogue_regarray_cache_key(unsigned size,
                                                enum rogue_reg_class class,
                                                uint32_t start_index,
                                                bool vec,
                                                uint8_t component)
{
   uint32_t reg_cache_key = rogue_reg_cache_key(start_index, vec, component);
   return (struct rogue_regarray_cache_key){ .start_index = reg_cache_key,
                                             .class = class,
                                             .size = size }
      .val;
}

typedef struct rogue_regarray {
   struct list_head link; /** Link in rogue_shader::regarrays. */
   unsigned size; /** Number of registers in the array. */
   rogue_regarray *parent;
   struct list_head children; /** List of subarrays with this regarray as their
                                 parent. */
   struct list_head child_link; /** Link in rogue_regarray::children. */
   rogue_reg **regs; /** Registers (allocated array if this is a parent, else
                        pointer to inside parent regarray->regs). */
   rogue_regarray **cached;

   struct list_head writes; /** List of all writes to this register array. */
   struct list_head uses; /** List of all register array uses. */
} rogue_regarray;

#define rogue_foreach_regarray(regarray, shader) \
   list_for_each_entry (rogue_regarray, regarray, &(shader)->regarrays, link)

#define rogue_foreach_regarray_safe(regarray, shader) \
   list_for_each_entry_safe (rogue_regarray,          \
                             regarray,                \
                             &(shader)->regarrays,    \
                             link)

#define rogue_foreach_subarray(subarray, regarray) \
   assert(!regarray->parent);                      \
   list_for_each_entry (rogue_regarray,            \
                        subarray,                  \
                        &(regarray)->children,     \
                        child_link)

#define rogue_foreach_subarray_safe(subarray, regarray) \
   assert(!regarray->parent);                           \
   list_for_each_entry_safe (rogue_regarray,            \
                             subarray,                  \
                             &(regarray)->children,     \
                             child_link)

typedef struct rogue_instr rogue_instr;

typedef struct rogue_regarray_write {
   rogue_instr *instr;
   unsigned dst_index;
   struct list_head link; /** Link in rogue_regarray::writes. */
} rogue_regarray_write;

#define rogue_foreach_regarray_write(write, regarray) \
   list_for_each_entry (rogue_regarray_write, write, &(regarray)->writes, link)

#define rogue_foreach_regarray_write_safe(write, regarray) \
   list_for_each_entry_safe (rogue_regarray_write,         \
                             write,                        \
                             &(regarray)->writes,          \
                             link)

typedef struct rogue_regarray_use {
   rogue_instr *instr;
   unsigned src_index;
   struct list_head link; /** Link in rogue_regarray::uses. */
} rogue_regarray_use;

#define rogue_foreach_regarray_use(use, regarray) \
   list_for_each_entry (rogue_regarray_use, use, &(regarray)->uses, link)

#define rogue_foreach_regarray_use_safe(use, regarray) \
   list_for_each_entry_safe (rogue_regarray_use, use, &(regarray)->uses, link)

/** Instruction phases, used in bitset. */
enum rogue_instr_phase {
   /** Main/ALU (and backend) instructions. */
   ROGUE_INSTR_PHASE_0,
   ROGUE_INSTR_PHASE_1,
   ROGUE_INSTR_PHASE_2_PCK,
   ROGUE_INSTR_PHASE_2_TST,
   ROGUE_INSTR_PHASE_2_MOV,
   ROGUE_INSTR_PHASE_BACKEND,

   ROGUE_INSTR_PHASE_COUNT,

   /** Control instructions (no co-issuing). */
   ROGUE_INSTR_PHASE_CTRL = ROGUE_INSTR_PHASE_0,

   /** Bitwise instructions. */
   ROGUE_INSTR_PHASE_0_BITMASK = ROGUE_INSTR_PHASE_0,
   ROGUE_INSTR_PHASE_0_SHIFT1 = ROGUE_INSTR_PHASE_1,
   ROGUE_INSTR_PHASE_0_COUNT = ROGUE_INSTR_PHASE_2_PCK,
   ROGUE_INSTR_PHASE_1_LOGICAL = ROGUE_INSTR_PHASE_2_TST,
   ROGUE_INSTR_PHASE_2_SHIFT2 = ROGUE_INSTR_PHASE_2_MOV,
   ROGUE_INSTR_PHASE_2_TEST = ROGUE_INSTR_PHASE_BACKEND,

   ROGUE_INSTR_PHASE_INVALID = ~0,
};

/* TODO: put into bitscan.h */
#define u_foreach_bit64_rev(b, dword)                  \
   for (uint64_t __dword = (dword), b;                 \
        ((b) = util_last_bit64(__dword) - 1, __dword); \
        __dword &= ~(1ull << (b)))

#define rogue_foreach_phase_in_set(p, phases) u_foreach_bit64(p, phases)
#define rogue_foreach_phase_in_set_rev(p, phases) u_foreach_bit64_rev(p, phases)

#define rogue_foreach_mod_in_set(m, mods) u_foreach_bit64(m, mods)

/** Rogue basic block. */
typedef struct rogue_block {
   rogue_shader *shader; /** Shader containing this block. */
   struct list_head instrs; /** Basic block instruction list. */
   struct list_head link; /** Link in rogue_shader::blocks. */

   struct list_head uses; /** List of all block uses. */

   unsigned nir_index;
   unsigned index; /** Block index. */
   const char *label; /** Block label. */
} rogue_block;

#define rogue_foreach_block(block, shader) \
   list_for_each_entry (rogue_block, block, &(shader)->blocks, link)

#define rogue_foreach_block_safe(block, shader) \
   list_for_each_entry_safe (rogue_block, block, &(shader)->blocks, link)

#define rogue_foreach_block_rev(block, shader) \
   list_for_each_entry_rev (rogue_block, block, &(shader)->blocks, link)

#define rogue_foreach_block_safe_rev(block, shader) \
   list_for_each_entry_safe_rev (rogue_block, block, &(shader)->blocks, link)

/** Rogue execution conditions. */
enum rogue_exec_cond {
   ROGUE_EXEC_COND_INVALID = 0,

   ROGUE_EXEC_COND_PE_TRUE,
   ROGUE_EXEC_COND_P0_TRUE,
   ROGUE_EXEC_COND_PE_ANY,
   ROGUE_EXEC_COND_P0_FALSE,

   ROGUE_EXEC_COND_COUNT,
};

extern const char *rogue_exec_cond_str[ROGUE_EXEC_COND_COUNT];

/** Rogue instruction type. */
enum rogue_instr_type {
   ROGUE_INSTR_TYPE_INVALID = 0,

   ROGUE_INSTR_TYPE_ALU, /** ALU instruction. */
   ROGUE_INSTR_TYPE_BACKEND, /** Backend instruction. */
   ROGUE_INSTR_TYPE_CTRL, /** Control instruction. */
   ROGUE_INSTR_TYPE_BITWISE, /** Bitwise instruction. */
   /* ROGUE_INSTR_TYPE_F16SOP, */ /** TODO: F16 sum-of-products instruction. */

   ROGUE_INSTR_TYPE_COUNT,
};

extern const char *rogue_instr_type_str[ROGUE_INSTR_TYPE_COUNT];

enum rogue_alu {
   ROGUE_ALU_INVALID = 0,

   ROGUE_ALU_MAIN,
   ROGUE_ALU_BITWISE,
   ROGUE_ALU_CONTROL,

   ROGUE_ALU_COUNT,
};

extern const char *const rogue_alu_str[ROGUE_ALU_COUNT];

extern const char
   *const rogue_instr_phase_str[ROGUE_ALU_COUNT][ROGUE_INSTR_PHASE_COUNT];

typedef struct rogue_instr_group rogue_instr_group;

/** Rogue instruction. */
typedef struct rogue_instr {
   enum rogue_instr_type type; /** Instruction type. */

   enum rogue_exec_cond exec_cond;
   unsigned repeat;
   bool end;
   bool atom;

   union {
      struct list_head link; /** Link in rogue_block::instrs. */
      rogue_instr_group *group; /** Instruction group containing this
                                   instruction. */
   };

   rogue_block *block; /** Basic block containing this instruction. */

   bool group_next; /** Group next instruction with this one. */
   unsigned index; /** Instruction index. */
   char *comment; /** Comment string. */
} rogue_instr;

#define rogue_foreach_instr_in_block(instr, block) \
   list_for_each_entry (rogue_instr, instr, &(block)->instrs, link)

#define rogue_foreach_instr_in_block_safe(instr, block) \
   list_for_each_entry_safe (rogue_instr, instr, &(block)->instrs, link)

#define rogue_foreach_instr_in_block_rev(instr, block) \
   list_for_each_entry_rev (rogue_instr, instr, &(block)->instrs, link)

#define rogue_foreach_instr_in_block_safe_rev(instr, block) \
   list_for_each_entry_safe_rev (rogue_instr, instr, &(block)->instrs, link)

#define rogue_foreach_instr_in_shader(instr, shader) \
   rogue_foreach_block (_block, (shader))            \
      rogue_foreach_instr_in_block ((instr), _block)

#define rogue_foreach_instr_in_shader_safe(instr, shader) \
   rogue_foreach_block_safe (_block, (shader))            \
      rogue_foreach_instr_in_block_safe ((instr), _block)

#define rogue_foreach_instr_in_shader_rev(instr, shader) \
   rogue_foreach_block_rev (_block, (shader))            \
      rogue_foreach_instr_in_block_rev ((instr), _block)

#define rogue_foreach_instr_in_shader_safe_rev(instr, shader) \
   rogue_foreach_block_safe_rev (_block, (shader))            \
      rogue_foreach_instr_in_block_safe_rev ((instr), _block)

static inline rogue_instr *rogue_instr_next(rogue_instr *instr)
{
   struct list_head *list = &instr->link;
   bool first = instr->link.next == &instr->block->instrs;

   if (first)
      return NULL;

   return list_entry(list->next, rogue_instr, link);
}

static inline rogue_instr *rogue_instr_prev(rogue_instr *instr)
{
   struct list_head *list = &instr->link;
   bool first = instr->link.prev == &instr->block->instrs;

   if (first)
      return NULL;

   return list_entry(list->prev, rogue_instr, link);
}

static inline void rogue_set_instr_exec_cond(rogue_instr *instr,
                                             enum rogue_exec_cond exec_cond)
{
   instr->exec_cond = exec_cond;
}

static inline void rogue_set_instr_repeat(rogue_instr *instr, unsigned repeat)
{
   instr->repeat = repeat;
}

static inline void rogue_set_instr_end(rogue_instr *instr, bool end)
{
   instr->end = end;
}

static inline void rogue_set_instr_atom(rogue_instr *instr, bool atom)
{
   instr->atom = atom;
}

static inline void rogue_set_instr_group_next(rogue_instr *instr,
                                              bool group_next)
{
   instr->group_next = group_next;
}

static inline void rogue_add_instr_comment(rogue_instr *instr,
                                           const char *comment)
{
   if (!instr->comment)
      instr->comment = ralloc_strdup(instr, comment);
   else
      ralloc_asprintf_append(&instr->comment, ", %s", comment);
}

static inline void
rogue_add_instr_vscommentf(rogue_instr *instr, const char *fmt, va_list args)
{
   if (!instr->comment) {
      instr->comment = ralloc_vasprintf(instr, fmt, args);
   } else {
      ralloc_strcat(&instr->comment, ", ");
      ralloc_vasprintf_append(&instr->comment, fmt, args);
   }
}

static inline void PRINTFLIKE(2, 3)
   rogue_add_instr_commentf(rogue_instr *instr, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);

   rogue_add_instr_vscommentf(instr, fmt, args);

   va_end(args);
}

static inline void rogue_copy_instr_comment(rogue_instr *to,
                                            const rogue_instr *from)
{
   if (!from->comment)
      return;

   rogue_add_instr_comment(to, from->comment);
}

static inline void rogue_merge_instr_comment(rogue_instr *to,
                                             const rogue_instr *from,
                                             const char *comment)
{
   rogue_copy_instr_comment(to, from);
   rogue_add_instr_comment(to, comment);
}

static inline void PRINTFLIKE(3, 4)
   rogue_merge_instr_commentf(rogue_instr *to,
                              const rogue_instr *from,
                              const char *fmt,
                              ...)
{
   va_list args;
   va_start(args, fmt);

   rogue_copy_instr_comment(to, from);
   rogue_add_instr_vscommentf(to, fmt, args);

   va_end(args);
}

typedef union rogue_imm_t {
   float f32;
   int32_t s32;
   uint32_t u32;
} rogue_imm_t;

enum rogue_io {
   /* Lower sources. */
   ROGUE_IO_S0,
   ROGUE_IO_S1,
   ROGUE_IO_S2,

   /* Upper sources. */
   ROGUE_IO_S3,
   ROGUE_IO_S4,
   ROGUE_IO_S5,

   /* Destinations. */
   ROGUE_IO_W0,
   ROGUE_IO_W1,

   /* Internal selectors. */
   ROGUE_IO_IS0,
   ROGUE_IO_IS1,
   ROGUE_IO_IS2,
   ROGUE_IO_IS3,
   ROGUE_IO_IS4,
   ROGUE_IO_IS5,

   /* Feedthroughs. */
   ROGUE_IO_FT0,
   ROGUE_IO_FT1,
   ROGUE_IO_FT2,
   ROGUE_IO_FTE,

   /* Only used for bitwise instructions. */
   ROGUE_IO_FT3,
   ROGUE_IO_FT4,
   ROGUE_IO_FT5,

   /* Test output feedthrough. */
   ROGUE_IO_FTT,

   ROGUE_IO_FT0H,

   /* Predicate registers. */
   ROGUE_IO_P0,
   ROGUE_IO_PE,

   /* Integer carry-out. */
   ROGUE_IO_COUT,

   /* For optional instruction arguments. */
   ROGUE_IO_NONE,

   ROGUE_IO_COUNT,

   ROGUE_IO_INVALID = ~0,
};
static_assert(ROGUE_IO_COUNT <= 64,
              "enum rogue_io does not fit in an uint64_t bitmask!");

static inline bool rogue_io_is_src(enum rogue_io io)
{
   return (io >= ROGUE_IO_S0 && io <= ROGUE_IO_S5);
}

static inline bool rogue_io_is_dst(enum rogue_io io)
{
   return (io >= ROGUE_IO_W0 && io <= ROGUE_IO_W1);
}

static inline bool rogue_io_is_iss(enum rogue_io io)
{
   return (io >= ROGUE_IO_IS0 && io <= ROGUE_IO_IS5);
}

static inline bool rogue_io_is_ft(enum rogue_io io)
{
   return (io >= ROGUE_IO_FT0 && io <= ROGUE_IO_FT0H);
}

static inline bool rogue_io_is_none(enum rogue_io io)
{
   return io == ROGUE_IO_NONE;
}

typedef struct rogue_io_info {
   const char *str;
} rogue_io_info;

extern const rogue_io_info rogue_io_infos[ROGUE_IO_COUNT];

static inline bool rogue_io_supported(enum rogue_io io,
                                      uint64_t supported_io_set)
{
   return !!(BITFIELD64_BIT(io) & supported_io_set);
}

#define ROGUE_DRCS 2

typedef struct rogue_drc_trxn {
   rogue_instr *acquire;
   rogue_instr *release;
   struct list_head link; /** Link in rogue_shader::drc_trxns[0/1]. */
} rogue_drc_trxn;

#define rogue_foreach_drc_trxn(drc_trxn, shader, index) \
   list_for_each_entry (rogue_drc_trxn,                 \
                        drc_trxn,                       \
                        &(shader)->drc_trxns[index],    \
                        link)

#define rogue_foreach_drc_trxn_safe(drc_trxn, shader, index) \
   list_for_each_entry_safe (rogue_drc_trxn,                 \
                             drc_trxn,                       \
                             &(shader)->drc_trxns[index],    \
                             link)

enum rogue_ref_type {
   ROGUE_REF_TYPE_INVALID = 0,

   ROGUE_REF_TYPE_VAL, /* Immediate that is not going to be replaced with a
                          register reference. */

   ROGUE_REF_TYPE_REG,
   ROGUE_REF_TYPE_REGARRAY,

   ROGUE_REF_TYPE_IMM, /* Immediate that is going to be replaced with a register
                          reference. */

   ROGUE_REF_TYPE_IO,

   ROGUE_REF_TYPE_DRC,

   ROGUE_REF_TYPE_COUNT,
};

typedef struct rogue_drc {
   unsigned index;
   union {
      rogue_drc_trxn trxn;
      rogue_drc_trxn *trxn_ptr;
   };
} rogue_drc;

typedef struct rogue_imm_use {
   rogue_instr *instr;
   unsigned src_index;
   rogue_imm_t *imm;
   struct list_head link; /** Link in rogue_shader::imm_uses. */
} rogue_imm_use;

#define rogue_foreach_imm_use(imm_use, shader) \
   list_for_each_entry (rogue_imm_use, imm_use, &(shader)->imm_uses, link)

#define rogue_foreach_imm_use_safe(imm_use, shader) \
   list_for_each_entry_safe (rogue_imm_use, imm_use, &(shader)->imm_uses, link)

typedef struct rogue_imm {
   rogue_imm_t imm;
   rogue_imm_use use;
} rogue_imm;

enum rogue_idx {
   ROGUE_IDX_NONE,
   ROGUE_IDX_0,
   ROGUE_IDX_1,
};

typedef struct rogue_ref {
   enum rogue_ref_type type;

   union {
      unsigned val;
      rogue_imm imm;
      rogue_reg *reg;
      rogue_regarray *regarray;
      enum rogue_io io;
      rogue_drc drc;
   };

   enum rogue_idx idx;
} rogue_ref;

static inline bool rogue_ref_type_supported(enum rogue_ref_type type,
                                            uint64_t supported_types)
{
   return !!(BITFIELD64_BIT(type - 1) & supported_types);
}

/**
 * \brief Returns a reference to a value.
 *
 * \param[in] val The value.
 * \return The reference.
 */
static inline rogue_ref rogue_ref_val(unsigned val)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_VAL,
      .val = val,
   };
}

/**
 * \brief Returns a reference to a register.
 *
 * \param[in] reg The register.
 * \return The reference.
 */
static inline rogue_ref rogue_ref_reg(rogue_reg *reg)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_REG,
      .reg = reg,
   };
}

/**
 * \brief Returns a reference to an indexed register.
 *
 * \param[in] reg The register.
 * \param[in] idx The index register.
 * \return The reference.
 */
static inline rogue_ref rogue_ref_reg_indexed(rogue_reg *reg, unsigned idx)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_REG,
      .reg = reg,
      .idx = ROGUE_IDX_0 + !!idx,
   };
}

/**
 * \brief Returns a reference to a register array.
 *
 * \param[in] regarray The register array.
 * \return The reference.
 */
static inline rogue_ref rogue_ref_regarray(rogue_regarray *regarray)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_REGARRAY,
      .regarray = regarray,
   };
}

static inline rogue_ref rogue_ref_imm(uint32_t imm)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_IMM,
      .imm.imm.u32 = imm,
   };
}

static inline rogue_ref rogue_ref_imm_f(float imm)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_IMM,
      .imm.imm.f32 = imm,
   };
}

static inline rogue_ref rogue_ref_io(enum rogue_io io)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_IO,
      .io = io,
   };
}

static inline rogue_ref rogue_none(void)
{
   return rogue_ref_io(ROGUE_IO_NONE);
}

static inline rogue_ref rogue_ref_drc(unsigned index)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_DRC,
      .drc.index = index,
   };
}

static inline rogue_ref rogue_ref_drc_trxn(unsigned index,
                                           rogue_drc_trxn *drc_trxn)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_DRC,
      .drc.index = index,
      .drc.trxn_ptr = drc_trxn,
   };
}

static inline rogue_ref rogue_ref_null(void)
{
   return (rogue_ref){};
}

static inline bool rogue_ref_is_imm(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_IMM;
}

static inline bool rogue_ref_is_val(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_VAL;
}

static inline bool rogue_ref_is_reg(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_REG && ref->idx == ROGUE_IDX_NONE;
}

static inline bool rogue_ref_is_reg_indexed(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_REG && ref->idx != ROGUE_IDX_NONE;
}

static inline bool rogue_ref_is_idx_reg(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_REG &&
          (ref->reg->class == ROGUE_REG_CLASS_IDX0 ||
           ref->reg->class == ROGUE_REG_CLASS_IDX1);
}

static inline bool rogue_ref_is_special_reg(const rogue_ref *ref)
{
   return rogue_ref_is_reg(ref) && ref->reg->class == ROGUE_REG_CLASS_SPECIAL;
}

static inline bool rogue_ref_is_const_reg(const rogue_ref *ref)
{
   return rogue_ref_is_reg(ref) && ref->reg->class == ROGUE_REG_CLASS_CONST;
}

static inline bool rogue_ref_is_ssa_reg(const rogue_ref *ref)
{
   return rogue_ref_is_reg(ref) && ref->reg->class == ROGUE_REG_CLASS_SSA;
}

static inline bool rogue_ref_is_vtxout_reg(const rogue_ref *ref)
{
   return rogue_ref_is_reg(ref) && ref->reg->class == ROGUE_REG_CLASS_VTXOUT;
}

static inline bool rogue_ref_is_regarray(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_REGARRAY;
}

static inline bool rogue_ref_is_reg_or_regarray(const rogue_ref *ref)
{
   return rogue_ref_is_reg(ref) || rogue_ref_is_regarray(ref) ||
          rogue_ref_is_reg_indexed(ref);
}

static inline bool rogue_ref_is_io(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_IO;
}

static inline bool rogue_ref_is_drc(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_DRC;
}

static inline bool rogue_ref_is_null(const rogue_ref *ref)
{
   return ref->type == ROGUE_REF_TYPE_INVALID;
}

static inline unsigned rogue_ref_get_regarray_size(const rogue_ref *ref)
{
   if (rogue_ref_is_regarray(ref))
      return ref->regarray->size;
   unreachable("Ref is not a regarray.");
}

static inline bool rogue_ref_reg_regarray_info(const rogue_ref *ref,
                                               enum rogue_reg_class *class,
                                               unsigned *index,
                                               unsigned *size)
{
   enum rogue_reg_class reg_class;
   unsigned reg_index;

   if (rogue_ref_is_regarray(ref)) {
      reg_class = ref->regarray->regs[0]->class;
      reg_index = ref->regarray->regs[0]->index;
   } else if (rogue_ref_is_reg(ref)) {
      reg_class = ref->reg->class;
      reg_index = ref->reg->index;
   } else {
      return false;
   }

   if (class)
      *class = reg_class;

   if (index)
      *index = reg_index;

   if (size)
      *size = rogue_ref_is_reg(ref) ? 1 : ref->regarray->size;

   return true;
}

static inline enum rogue_io rogue_ref_get_io(const rogue_ref *ref)
{
   assert(rogue_ref_is_io(ref));
   return ref->io;
}

static inline bool rogue_ref_is_io_p0(const rogue_ref *ref)
{
   return rogue_ref_get_io(ref) == ROGUE_IO_P0;
}

static inline bool rogue_ref_is_io_ftt(const rogue_ref *ref)
{
   return rogue_ref_get_io(ref) == ROGUE_IO_FTT;
}

static inline bool rogue_ref_is_io_none(const rogue_ref *ref)
{
   /* Special case - never assert. */
   if (!rogue_ref_is_io(ref))
      return false;

   return rogue_ref_get_io(ref) == ROGUE_IO_NONE;
}

static inline unsigned rogue_ref_get_io_src_index(const rogue_ref *ref)
{
   return rogue_ref_get_io(ref) - ROGUE_IO_S0;
}

static inline unsigned rogue_ref_get_drc_index(const rogue_ref *ref)
{
   assert(rogue_ref_is_drc(ref));
   return ref->drc.index;
}

static inline rogue_drc *rogue_ref_get_drc(rogue_ref *ref)
{
   assert(rogue_ref_is_drc(ref));
   return &ref->drc;
}

static inline unsigned rogue_ref_get_val(const rogue_ref *ref)
{
   assert(rogue_ref_is_val(ref));
   return ref->val;
}

static inline rogue_imm *rogue_ref_get_imm(rogue_ref *ref)
{
   assert(rogue_ref_is_imm(ref));
   return &ref->imm;
}

static inline bool rogue_refs_equal(rogue_ref *a, rogue_ref *b)
{
   if (a->type != b->type)
      return false;

   switch (a->type) {
   case ROGUE_REF_TYPE_VAL:
      return a->val == b->val;

   case ROGUE_REF_TYPE_REG:
      return a->reg == b->reg && a->idx == b->idx;

   case ROGUE_REF_TYPE_REGARRAY:
      return a->regarray == b->regarray;

   case ROGUE_REF_TYPE_IMM:
      return a->imm.imm.u32 == b->imm.imm.u32;

   case ROGUE_REF_TYPE_IO:
      return a->io == b->io;

   case ROGUE_REF_TYPE_DRC:
      return a->drc.index == b->drc.index;

   default:
      break;
   }

   return false;
}

static inline uint64_t rogue_reg_supported_io_srcs(rogue_ref *ref)
{
   enum rogue_reg_class reg_class;

   if (rogue_ref_is_regarray(ref))
      reg_class = ref->regarray->regs[0]->class;
   else if (rogue_ref_is_reg(ref))
      reg_class = ref->reg->class;
   else
      unreachable("Reference is not a reg or regarray.");

   /* Special case - special constants range. */
   if (rogue_ref_is_special_reg(ref)) {
      unsigned idx = ref->reg->index;

      if ((idx >= ROGUE_SPECIAL_REG_CONSTS_0_START &&
           idx <= ROGUE_SPECIAL_REG_CONSTS_0_END) ||
          (idx >= ROGUE_SPECIAL_REG_CONSTS_1_START &&
           idx <= ROGUE_SPECIAL_REG_CONSTS_1_END) ||
          (idx >= ROGUE_SPECIAL_REG_CONSTS_2_START &&
           idx <= ROGUE_SPECIAL_REG_CONSTS_2_END))
         reg_class = ROGUE_REG_CLASS_CONST;
   }

   /* Special case - indexed registers. */
   if (rogue_ref_is_reg_indexed(ref))
      reg_class = ROGUE_REG_CLASS_IDX0 + ref->idx;

   return rogue_reg_class_infos[reg_class].supported_io_srcs;
}

static inline enum reg_bank rogue_reg_bank_encoding(const rogue_ref *ref)
{
   enum rogue_reg_class class;

   if (rogue_ref_is_regarray(ref)) {
      class = ref->regarray->regs[0]->class;
   } else if (rogue_ref_is_reg(ref)) {
      class = ref->reg->class;
   } else if (rogue_ref_is_reg_indexed(ref)) {
      class = ref->idx == ROGUE_IDX_0 ? ROGUE_REG_CLASS_IDX0
                                      : ROGUE_REG_CLASS_IDX1;
   } else {
      unreachable("Ref is not a reg/regarray.");
   }

   switch (class) {
   case ROGUE_REG_CLASS_TEMP:
      return BANK_TEMP;
   case ROGUE_REG_CLASS_COEFF:
      return BANK_COEFF;
   case ROGUE_REG_CLASS_SHARED:
      return BANK_SHARED;
   case ROGUE_REG_CLASS_SPECIAL:
      return BANK_SPECIAL;
   case ROGUE_REG_CLASS_VTXIN:
      return BANK_VTXIN;
   case ROGUE_REG_CLASS_IDX0:
      return BANK_IDX0;
   case ROGUE_REG_CLASS_IDX1:
      return BANK_IDX1;

   default:
      break;
   }

   unreachable("Unsupported register class.");
}

static inline enum idx_bank rogue_idx_bank_encoding(const rogue_ref *ref)
{
   enum rogue_reg_class class;

   if (rogue_ref_is_idx_reg(ref) || rogue_ref_is_reg_indexed(ref))
      class = ref->reg->class;
   else
      unreachable("Ref is not an index(ed) register.");

   switch (class) {
   case ROGUE_REG_CLASS_TEMP:
      return IDX_BANK_TEMP;

   case ROGUE_REG_CLASS_COEFF:
      return IDX_BANK_COEFF;

   case ROGUE_REG_CLASS_SHARED:
      return IDX_BANK_SHARED;

   case ROGUE_REG_CLASS_VTXIN:
      return IDX_BANK_VTXIN;

   case ROGUE_REG_CLASS_PIXOUT:
      return IDX_BANK_PIXOUT;

   case ROGUE_REG_CLASS_IDX0:
   case ROGUE_REG_CLASS_IDX1:
      return IDX_BANK_IDX;

   default:
      break;
   }

   unreachable("Unsupported register class.");
}

static inline unsigned rogue_reg_index_encoding(const rogue_ref *ref)
{
   if (rogue_ref_is_reg_indexed(ref) || rogue_ref_is_idx_reg(ref)) {
      return (rogue_idx_offset){
         .bank = rogue_idx_bank_encoding(ref),
         .offset = ref->reg->index,
      }
         ._;
   } else if (rogue_ref_is_reg(ref)) {
      return ref->reg->index;
   } else if (rogue_ref_is_regarray(ref)) {
      return ref->regarray->regs[0]->index;
   }

   unreachable("Ref is not a reg/regarray.");
}

typedef struct rogue_instr_dst {
   rogue_ref ref;
   uint64_t mod;
   unsigned index;
} rogue_instr_dst;

typedef struct rogue_instr_src {
   rogue_ref ref;
   uint64_t mod;
   unsigned index;
} rogue_instr_src;

static inline bool rogue_instr_dst_src_equal(rogue_instr_dst *dst,
                                             rogue_instr_src *src)
{
   /* TODO: Take modifiers into account. */
   if (dst->mod || src->mod)
      return false;

   return rogue_refs_equal(&dst->ref, &src->ref);
}

typedef struct rogue_reg_write {
   rogue_instr *instr;
   unsigned dst_index;
   struct list_head link; /** Link in rogue_reg::writes. */
} rogue_reg_write;

#define rogue_foreach_reg_write(write, reg) \
   list_for_each_entry (rogue_reg_write, write, &(reg)->writes, link)

#define rogue_foreach_reg_write_safe(write, reg) \
   list_for_each_entry_safe (rogue_reg_write, write, &(reg)->writes, link)

typedef struct rogue_reg_use {
   rogue_instr *instr;
   unsigned src_index;
   struct list_head link; /** Link in rogue_reg::uses. */
} rogue_reg_use;

#define rogue_foreach_reg_use(use, reg) \
   list_for_each_entry (rogue_reg_use, use, &(reg)->uses, link)

#define rogue_foreach_reg_use_safe(use, reg) \
   list_for_each_entry_safe (rogue_reg_use, use, &(reg)->uses, link)

typedef union rogue_dst_write {
   rogue_reg_write reg;
   rogue_regarray_write regarray;
} rogue_dst_write;

typedef union rogue_src_use {
   rogue_reg_use reg;
   rogue_regarray_use regarray;
} rogue_src_use;

typedef struct rogue_block_use {
   rogue_instr *instr;
   struct list_head link; /** Link in rogue_block::uses. */
} rogue_block_use;

#define rogue_foreach_block_use(use, block) \
   list_for_each_entry (rogue_block_use, use, &(block)->uses, link)

#define rogue_foreach_block_use_safe(use, block) \
   list_for_each_entry_safe (rogue_block_use, use, &(block)->uses, link)

/** Rogue ALU instruction operations. */
enum rogue_alu_op {
   ROGUE_ALU_OP_INVALID = 0,

   /* Real instructions. */

   ROGUE_ALU_OP_MBYP0,
   ROGUE_ALU_OP_MBYP1,

   ROGUE_ALU_OP_FRCP,
   ROGUE_ALU_OP_FRSQ,
   ROGUE_ALU_OP_FLOG2,
   ROGUE_ALU_OP_FLOGCN,
   ROGUE_ALU_OP_FEXP2,

   ROGUE_ALU_OP_FDSX,
   ROGUE_ALU_OP_FDSY,

   ROGUE_ALU_OP_FDSXF,
   ROGUE_ALU_OP_FDSYF,

   ROGUE_ALU_OP_FSINC,
   ROGUE_ALU_OP_FARCTANC,
   ROGUE_ALU_OP_FRED,

   ROGUE_ALU_OP_FADD,
   ROGUE_ALU_OP_FMUL,
   ROGUE_ALU_OP_FMAD,

   ROGUE_ALU_OP_ADD8,
   ROGUE_ALU_OP_MUL8,

   ROGUE_ALU_OP_ADD16,
   ROGUE_ALU_OP_MUL16,

   ROGUE_ALU_OP_ADD64,

   ROGUE_ALU_OP_ADD64_32,
   ROGUE_ALU_OP_MADD32,
   ROGUE_ALU_OP_MADD64,

   ROGUE_ALU_OP_TST0,
   ROGUE_ALU_OP_TST1,
   ROGUE_ALU_OP_TST2,

   ROGUE_ALU_OP_MOVC,

   ROGUE_ALU_OP_PCK_CONST0,
   ROGUE_ALU_OP_PCK_U8888,
   ROGUE_ALU_OP_PCK_S8888,
   ROGUE_ALU_OP_PCK_U1616,
   ROGUE_ALU_OP_PCK_S1616,
   ROGUE_ALU_OP_PCK_F16F16,
   ROGUE_ALU_OP_PCK_U32,
   ROGUE_ALU_OP_PCK_S32,
   ROGUE_ALU_OP_PCK_F32,
   ROGUE_ALU_OP_PCK_2F10F10F10,

   ROGUE_ALU_OP_UPCK_U8888,
   ROGUE_ALU_OP_UPCK_S8888,
   ROGUE_ALU_OP_UPCK_U1616,
   ROGUE_ALU_OP_UPCK_S1616,
   ROGUE_ALU_OP_UPCK_F16F16,
   ROGUE_ALU_OP_UPCK_U32,
   ROGUE_ALU_OP_UPCK_S32,

   /* Pseudo-instructions. */
   ROGUE_ALU_OP_PSEUDO,
   ROGUE_ALU_OP_MOV = ROGUE_ALU_OP_PSEUDO,

   ROGUE_ALU_OP_MIN,
   ROGUE_ALU_OP_MAX,

   ROGUE_ALU_OP_CMP, /** Compare. */
   ROGUE_ALU_OP_CSEL, /** Conditional select. */

   ROGUE_ALU_OP_SETPRED, /** Set predicate register. */
   ROGUE_ALU_OP_GETPRED, /** Get predicate register. */

   ROGUE_ALU_OP_FABS,
   ROGUE_ALU_OP_FNEG,
   ROGUE_ALU_OP_FNABS,
   ROGUE_ALU_OP_FFLR,

   ROGUE_ALU_OP_IADD8,
   ROGUE_ALU_OP_IMUL8,

   ROGUE_ALU_OP_IADD16,
   ROGUE_ALU_OP_IMUL16,

   ROGUE_ALU_OP_IADD32,
   ROGUE_ALU_OP_IADD64,

   ROGUE_ALU_OP_IMUL32,
   ROGUE_ALU_OP_IMUL64,

   ROGUE_ALU_OP_UMUL_HIGH,
   ROGUE_ALU_OP_UMUL_LOW,

   ROGUE_ALU_OP_IMUL_HIGH,
   ROGUE_ALU_OP_IMUL_LOW,

   ROGUE_ALU_OP_INEG8,
   ROGUE_ALU_OP_INEG16,
   ROGUE_ALU_OP_INEG32,
   ROGUE_ALU_OP_INEG64,

   ROGUE_ALU_OP_IABS8,
   ROGUE_ALU_OP_IABS16,
   ROGUE_ALU_OP_IABS32,
   ROGUE_ALU_OP_IABS64,

   ROGUE_ALU_OP_COUNT,
};

enum rogue_alu_op_mod {
   /* In order of priority */
   ROGUE_ALU_OP_MOD_PARTA,
   ROGUE_ALU_OP_MOD_PARTB,

   ROGUE_ALU_OP_MOD_SIN,
   ROGUE_ALU_OP_MOD_COS,

   ROGUE_ALU_OP_MOD_LP, /* Low-precision modifier (force 13 lsbs of all sources
                           to zero before op, and of result after op). */
   ROGUE_ALU_OP_MOD_SAT, /* Saturate output. */

   ROGUE_ALU_OP_MOD_SCALE, /* Scale to [0, 1]. */
   ROGUE_ALU_OP_MOD_ROUNDZERO, /* Round to zero. */

   ROGUE_ALU_OP_MOD_Z, /** Test == 0. */
   ROGUE_ALU_OP_MOD_GZ, /** Test > 0. */
   ROGUE_ALU_OP_MOD_GEZ, /** Test >= 0. */
   ROGUE_ALU_OP_MOD_C, /** Test integer carry-out. */
   ROGUE_ALU_OP_MOD_E, /** Test a == b. */
   ROGUE_ALU_OP_MOD_G, /** Test a > b. */
   ROGUE_ALU_OP_MOD_GE, /** Test a >= b. */
   ROGUE_ALU_OP_MOD_NE, /** Test a != b. */
   ROGUE_ALU_OP_MOD_L, /** Test a < b. */
   ROGUE_ALU_OP_MOD_LE, /** Test a <= b. */

   ROGUE_ALU_OP_MOD_F32,
   ROGUE_ALU_OP_MOD_U16,
   ROGUE_ALU_OP_MOD_S16,
   ROGUE_ALU_OP_MOD_U8,
   ROGUE_ALU_OP_MOD_S8,
   ROGUE_ALU_OP_MOD_U32,
   ROGUE_ALU_OP_MOD_S32,

   ROGUE_ALU_OP_MOD_S, /** Signed. */

   ROGUE_ALU_OP_MOD_INVERT,

   ROGUE_ALU_OP_MOD_COUNT,
};

typedef struct rogue_alu_op_mod_info {
   const char *str;
   uint64_t exclude; /* Can't use this op mod with any of these. */
   uint64_t require; /* Required op mods for this to be used (OR). */
} rogue_alu_op_mod_info;

extern const rogue_alu_op_mod_info
   rogue_alu_op_mod_infos[ROGUE_ALU_OP_MOD_COUNT];

static inline bool rogue_mods_supported(uint64_t mods, uint64_t supported_mods)
{
   return !(mods & ~supported_mods);
}

enum rogue_alu_dst_mod {
   ROGUE_ALU_DST_MOD_E0,
   ROGUE_ALU_DST_MOD_E1,
   ROGUE_ALU_DST_MOD_E2,
   ROGUE_ALU_DST_MOD_E3,

   ROGUE_ALU_DST_MOD_COUNT,
};

typedef struct rogue_alu_dst_mod_info {
   const char *str;
} rogue_alu_dst_mod_info;

extern const rogue_alu_dst_mod_info
   rogue_alu_dst_mod_infos[ROGUE_ALU_DST_MOD_COUNT];

enum rogue_alu_src_mod {
   /* In order of priority, i.e. if all NEG, ABS, and FLR are all set, FLR will
      happen first, then ABS, then NEG. */
   ROGUE_ALU_SRC_MOD_FLR,
   ROGUE_ALU_SRC_MOD_ABS,
   ROGUE_ALU_SRC_MOD_NEG,

   ROGUE_ALU_SRC_MOD_E0,
   ROGUE_ALU_SRC_MOD_E1,
   ROGUE_ALU_SRC_MOD_E2,
   ROGUE_ALU_SRC_MOD_E3,

   ROGUE_ALU_SRC_MOD_COUNT,
};

typedef struct rogue_alu_src_mod_info {
   const char *str;
} rogue_alu_src_mod_info;

extern const rogue_alu_src_mod_info
   rogue_alu_src_mod_infos[ROGUE_ALU_SRC_MOD_COUNT];

enum rogue_ctrl_op {
   ROGUE_CTRL_OP_INVALID = 0,

   /* Real instructions. */
   ROGUE_CTRL_OP_NOP,
   ROGUE_CTRL_OP_WOP,

   ROGUE_CTRL_OP_BR, /* Branch: relative (to block). */
   ROGUE_CTRL_OP_BA, /* Branch: absolute (to address). */

   ROGUE_CTRL_OP_CNDST, /** Conditional start. */
   ROGUE_CTRL_OP_CNDEF, /** Conditional elif. */
   ROGUE_CTRL_OP_CNDEND, /** Conditional end. */
   ROGUE_CTRL_OP_CNDLT, /** Conditional loop-test. */
   ROGUE_CTRL_OP_CNDSM, /** Conditional set mask. */

   ROGUE_CTRL_OP_WDF,

   ROGUE_CTRL_OP_MUTEX,

   /* Pseudo-instructions. */
   ROGUE_CTRL_OP_PSEUDO,
   ROGUE_CTRL_OP_END = ROGUE_CTRL_OP_PSEUDO,

   ROGUE_CTRL_OP_COUNT,
};

enum rogue_ctrl_op_mod {
   /* In order of priority */
   ROGUE_CTRL_OP_MOD_LINK,

   ROGUE_CTRL_OP_MOD_ALLINST,
   ROGUE_CTRL_OP_MOD_ANYINST,

   ROGUE_CTRL_OP_MOD_END,

   ROGUE_CTRL_OP_MOD_NOWDF, /* Don't schedule WDF. */

   ROGUE_CTRL_OP_MOD_ALWAYS,
   ROGUE_CTRL_OP_MOD_P0_TRUE,
   ROGUE_CTRL_OP_MOD_NEVER,
   ROGUE_CTRL_OP_MOD_P0_FALSE,

   ROGUE_CTRL_OP_MOD_LOCK,
   ROGUE_CTRL_OP_MOD_RELEASE,

   ROGUE_CTRL_OP_MOD_COUNT,
};

typedef struct rogue_ctrl_op_mod_info {
   const char *str;
   uint64_t exclude; /* Can't use this op mod with any of these. */
   uint64_t require; /* Required op mods for this to be used (OR). */
} rogue_ctrl_op_mod_info;

extern const rogue_ctrl_op_mod_info
   rogue_ctrl_op_mod_infos[ROGUE_CTRL_OP_MOD_COUNT];

#define ROGUE_CTRL_OP_MAX_SRCS 7
#define ROGUE_CTRL_OP_MAX_DSTS 3

typedef struct rogue_ctrl_io_info {
   uint64_t dst_set[ROGUE_CTRL_OP_MAX_SRCS];
   uint64_t src_set[ROGUE_CTRL_OP_MAX_DSTS];
} rogue_ctrl_io_info;

typedef struct rogue_ctrl_op_info {
   const char *str;

   bool has_target; /* Has a block as a target. */
   bool ends_block; /* Can be the instruction at the end of a block. */
   bool has_srcs; /* Has encodable sources. */
   bool has_dsts; /* Has encodable destinations. */

   unsigned num_dsts;
   unsigned num_srcs;

   rogue_ctrl_io_info io;

   uint64_t supported_op_mods;
   uint64_t supported_dst_mods[ROGUE_CTRL_OP_MAX_DSTS];
   uint64_t supported_src_mods[ROGUE_CTRL_OP_MAX_SRCS];

   uint64_t supported_dst_types[ROGUE_CTRL_OP_MAX_DSTS];
   uint64_t supported_src_types[ROGUE_CTRL_OP_MAX_SRCS];

   unsigned dst_stride[ROGUE_CTRL_OP_MAX_DSTS];
   unsigned src_stride[ROGUE_CTRL_OP_MAX_SRCS];

   uint64_t dst_repeat_mask;
   uint64_t src_repeat_mask;

   unsigned valnum_src;
   uint64_t dst_valnum_mask;
   uint64_t src_valnum_mask;

   unsigned max_repeat;
} rogue_ctrl_op_info;

extern const rogue_ctrl_op_info rogue_ctrl_op_infos[ROGUE_CTRL_OP_COUNT];

static inline bool rogue_ctrl_op_has_srcs(enum rogue_ctrl_op op)
{
   const rogue_ctrl_op_info *info = &rogue_ctrl_op_infos[op];
   return info->has_srcs;
}

static inline bool rogue_ctrl_op_has_dsts(enum rogue_ctrl_op op)
{
   const rogue_ctrl_op_info *info = &rogue_ctrl_op_infos[op];
   return info->has_dsts;
}

/* ALU instructions have at most 5 sources. */
#define ROGUE_ALU_OP_MAX_SRCS 5
#define ROGUE_ALU_OP_MAX_DSTS 3

typedef struct rogue_alu_io_info {
   uint64_t dst_set[ROGUE_ALU_OP_MAX_DSTS];
   uint64_t src_set[ROGUE_ALU_OP_MAX_SRCS];
} rogue_alu_io_info;

/** Rogue ALU instruction operation info. */
typedef struct rogue_alu_op_info {
   const char *str;

   unsigned num_dsts;
   unsigned num_srcs;

   bool whole_pipeline;

   enum rogue_instr_phase phase;
   rogue_alu_io_info io;

   uint64_t supported_op_mods;
   uint64_t supported_dst_mods[ROGUE_ALU_OP_MAX_DSTS];
   uint64_t supported_src_mods[ROGUE_ALU_OP_MAX_SRCS];

   uint64_t supported_dst_types[ROGUE_ALU_OP_MAX_DSTS];
   uint64_t supported_src_types[ROGUE_ALU_OP_MAX_SRCS];

   unsigned dst_stride[ROGUE_CTRL_OP_MAX_DSTS];
   unsigned src_stride[ROGUE_CTRL_OP_MAX_SRCS];

   uint64_t dst_repeat_mask;
   uint64_t src_repeat_mask;

   unsigned valnum_src;
   uint64_t dst_valnum_mask;
   uint64_t src_valnum_mask;

   unsigned max_repeat;
} rogue_alu_op_info;

extern const rogue_alu_op_info rogue_alu_op_infos[ROGUE_ALU_OP_COUNT];

/** Rogue ALU instruction. */
typedef struct rogue_alu_instr {
   rogue_instr instr;

   enum rogue_alu_op op;

   uint64_t mod;

   rogue_instr_dst dst[ROGUE_ALU_OP_MAX_DSTS];
   rogue_dst_write dst_write[ROGUE_ALU_OP_MAX_DSTS];

   rogue_instr_src src[ROGUE_ALU_OP_MAX_SRCS];
   rogue_src_use src_use[ROGUE_ALU_OP_MAX_SRCS];
} rogue_alu_instr;

static inline void rogue_set_alu_op_mod(rogue_alu_instr *alu,
                                        enum rogue_alu_op_mod mod)
{
   alu->mod |= BITFIELD64_BIT(mod);
}

static inline bool rogue_alu_op_mod_is_set(const rogue_alu_instr *alu,
                                           enum rogue_alu_op_mod mod)
{
   return !!(alu->mod & BITFIELD64_BIT(mod));
}

static inline void rogue_set_alu_dst_mod(rogue_alu_instr *alu,
                                         unsigned dst_index,
                                         enum rogue_alu_dst_mod mod)
{
   alu->dst[dst_index].mod |= BITFIELD64_BIT(mod);
}

static inline bool rogue_alu_dst_mod_is_set(const rogue_alu_instr *alu,
                                            unsigned dst_index,
                                            enum rogue_alu_dst_mod mod)
{
   return !!(alu->dst[dst_index].mod & BITFIELD64_BIT(mod));
}

static inline void rogue_set_alu_src_mod(rogue_alu_instr *alu,
                                         unsigned src_index,
                                         enum rogue_alu_src_mod mod)
{
   alu->src[src_index].mod |= BITFIELD64_BIT(mod);
}

static inline bool rogue_alu_src_mod_is_set(const rogue_alu_instr *alu,
                                            unsigned src_index,
                                            enum rogue_alu_src_mod mod)
{
   return !!(alu->src[src_index].mod & BITFIELD64_BIT(mod));
}

/**
 * \brief Allocates and initializes a new ALU instruction.
 *
 * \param[in] block The block which will contain the instruction.
 * \param[in] op The ALU instruction operation.
 * \return The new instruction.
 */
rogue_alu_instr *rogue_alu_instr_create(rogue_block *block,
                                        enum rogue_alu_op op);

#define ROGUE_BACKEND_OP_MAX_SRCS 6
#define ROGUE_BACKEND_OP_MAX_DSTS 2

enum rogue_backend_op {
   ROGUE_BACKEND_OP_INVALID = 0,

   ROGUE_BACKEND_OP_UVSW_WRITE,
   ROGUE_BACKEND_OP_UVSW_EMIT,
   /* ROGUE_BACKEND_OP_UVSW_CUT, */
   /* ROGUE_BACKEND_OP_UVSW_EMITTHENCUT, */
   ROGUE_BACKEND_OP_UVSW_ENDTASK,
   ROGUE_BACKEND_OP_UVSW_EMITTHENENDTASK,
   ROGUE_BACKEND_OP_UVSW_WRITETHENEMITTHENENDTASK,

   ROGUE_BACKEND_OP_MOVMSK,
   ROGUE_BACKEND_OP_SAVMSK,

   ROGUE_BACKEND_OP_IDF,

   ROGUE_BACKEND_OP_EMITPIX,

   ROGUE_BACKEND_OP_ATOMIC,

   ROGUE_BACKEND_OP_LD,
   ROGUE_BACKEND_OP_ST,

   ROGUE_BACKEND_OP_ATST,

   ROGUE_BACKEND_OP_FITR_PIXEL,
   /* ROGUE_BACKEND_OP_SAMPLE, */
   /* ROGUE_BACKEND_OP_CENTROID, */
   ROGUE_BACKEND_OP_FITRP_PIXEL,
   /* ROGUE_BACKEND_OP_FITRP_SAMPLE, */
   /* ROGUE_BACKEND_OP_FITRP_CENTROID, */

   ROGUE_BACKEND_OP_SMP1D,
   ROGUE_BACKEND_OP_SMP2D,
   ROGUE_BACKEND_OP_SMP3D,

   ROGUE_BACKEND_OP_PSEUDO,
   ROGUE_BACKEND_OP_ATST_IF = ROGUE_BACKEND_OP_PSEUDO,

   ROGUE_BACKEND_OP_COUNT,
};

typedef struct rogue_backend_io_info {
   uint64_t dst_set[ROGUE_BACKEND_OP_MAX_DSTS];
   uint64_t src_set[ROGUE_BACKEND_OP_MAX_SRCS];
} rogue_backend_io_info;

typedef struct rogue_backend_op_info {
   const char *str;

   unsigned num_dsts;
   unsigned num_srcs;

   rogue_backend_io_info io;

   uint64_t supported_op_mods;
   uint64_t supported_dst_mods[ROGUE_BACKEND_OP_MAX_DSTS];
   uint64_t supported_src_mods[ROGUE_BACKEND_OP_MAX_SRCS];

   uint64_t supported_dst_types[ROGUE_BACKEND_OP_MAX_DSTS];
   uint64_t supported_src_types[ROGUE_BACKEND_OP_MAX_SRCS];

   unsigned dst_stride[ROGUE_CTRL_OP_MAX_DSTS];
   unsigned src_stride[ROGUE_CTRL_OP_MAX_SRCS];

   uint64_t dst_repeat_mask;
   uint64_t src_repeat_mask;

   unsigned valnum_src;
   uint64_t dst_valnum_mask;
   uint64_t src_valnum_mask;

   unsigned max_repeat;
} rogue_backend_op_info;

extern const rogue_backend_op_info
   rogue_backend_op_infos[ROGUE_BACKEND_OP_COUNT];

enum rogue_backend_op_mod {
   /* In order of priority */
   ROGUE_BACKEND_OP_MOD_PROJ, /* Projection (send T co-ordinate). */
   ROGUE_BACKEND_OP_MOD_FCNORM, /* Fixed-point texture data (convert to float).
                                 */
   ROGUE_BACKEND_OP_MOD_NNCOORDS, /* Non-normalised co-ordinates. */

   ROGUE_BACKEND_OP_MOD_BIAS, /* LOD mode: bias. */
   ROGUE_BACKEND_OP_MOD_REPLACE, /* LOD mode: replace. */
   ROGUE_BACKEND_OP_MOD_GRADIENT, /* LOD mode: gradient. */

   ROGUE_BACKEND_OP_MOD_PPLOD, /* Per-pixel LOD. */
   ROGUE_BACKEND_OP_MOD_TAO, /* Texture address override. */
   ROGUE_BACKEND_OP_MOD_SOO, /* Sample offset supplied. */
   ROGUE_BACKEND_OP_MOD_SNO, /* Sample number supplied. */
   ROGUE_BACKEND_OP_MOD_WRT, /* SMP write. */

   ROGUE_BACKEND_OP_MOD_DATA, /* Sample bypass mode: data. */
   ROGUE_BACKEND_OP_MOD_INFO, /* Sample bypass mode: info. */
   ROGUE_BACKEND_OP_MOD_BOTH, /* Sample bypass mode: both. */

   ROGUE_BACKEND_OP_MOD_IADD,
   ROGUE_BACKEND_OP_MOD_ISUB,
   ROGUE_BACKEND_OP_MOD_XCHG,
   ROGUE_BACKEND_OP_MOD_UMIN,
   ROGUE_BACKEND_OP_MOD_IMIN,
   ROGUE_BACKEND_OP_MOD_UMAX,
   ROGUE_BACKEND_OP_MOD_IMAX,
   ROGUE_BACKEND_OP_MOD_AND,
   ROGUE_BACKEND_OP_MOD_OR,
   ROGUE_BACKEND_OP_MOD_XOR,

   ROGUE_BACKEND_OP_MOD_SM, /* Sample mask. */

   ROGUE_BACKEND_OP_MOD_VM, /* Valid mask. */
   ROGUE_BACKEND_OP_MOD_ICM, /* Input coverage mask. */
   ROGUE_BACKEND_OP_MOD_ICMOC, /* ICM, ones count. */
   ROGUE_BACKEND_OP_MOD_ICMI, /* ICM, intensity. */
   ROGUE_BACKEND_OP_MOD_CAXY, /* Centroid-adjusted x/y co-ords. */

   ROGUE_BACKEND_OP_MOD_TILED, /* Tiled LD/ST. */

   ROGUE_BACKEND_OP_MOD_BYPASS, /* MCU cache mode (read): bypass. */
   ROGUE_BACKEND_OP_MOD_FORCELINEFILL, /* MCU cache mode (read): force line
                                        * fill.
                                        */

   ROGUE_BACKEND_OP_MOD_WRITETHROUGH, /* MCU cache mode (write): write through
                                       * L1 & SLC.
                                       */
   ROGUE_BACKEND_OP_MOD_WRITEBACK, /* MCU cache mode (write): write back. */
   ROGUE_BACKEND_OP_MOD_LAZYWRITEBACK, /* MCU cache mode (write): lazy write
                                        * back.
                                        */

   ROGUE_BACKEND_OP_MOD_SLCBYPASS, /* SLC cache mode: bypass.*/
   ROGUE_BACKEND_OP_MOD_SLCWRITEBACK, /* SLC cache mode: write back */
   ROGUE_BACKEND_OP_MOD_SLCWRITETHROUGH, /* SLC cache mode: write through. */
   ROGUE_BACKEND_OP_MOD_SLCNOALLOC, /* SLC cache mode: cached reads/no
                                     * allocation on miss.
                                     */

   ROGUE_BACKEND_OP_MOD_ARRAY, /* Sample data contains array index/texture
                                * arrays enabled.
                                */
   ROGUE_BACKEND_OP_MOD_INTEGER, /* Integer co-ordinates and sample data. */
   ROGUE_BACKEND_OP_MOD_SCHEDSWAP, /* Deschedule slot after instruction. */

   ROGUE_BACKEND_OP_MOD_F16, /* Return packed F16 data. */

   ROGUE_BACKEND_OP_MOD_SAT, /* Saturate output. */

   ROGUE_BACKEND_OP_MOD_FREEP, /* Free partition. */

   ROGUE_BACKEND_OP_MOD_NOWDF, /* Don't schedule WDF. */

   ROGUE_BACKEND_OP_MOD_IFB, /* Inhibit FeedBack. */

   ROGUE_BACKEND_OP_MOD_NEVER,
   ROGUE_BACKEND_OP_MOD_LESS,
   ROGUE_BACKEND_OP_MOD_EQUAL,
   ROGUE_BACKEND_OP_MOD_LESSEQUAL,
   ROGUE_BACKEND_OP_MOD_GREATER,
   ROGUE_BACKEND_OP_MOD_NOTEQUAL,
   ROGUE_BACKEND_OP_MOD_GREATEREQUAL,
   ROGUE_BACKEND_OP_MOD_ALWAYS,

   ROGUE_BACKEND_OP_MOD_COUNT,
};

typedef struct rogue_backend_op_mod_info {
   const char *str;
   uint64_t exclude; /* Can't use this op mod with any of these. */
   uint64_t require; /* Required op mods for this to be used (OR). */
} rogue_backend_op_mod_info;

extern const rogue_backend_op_mod_info
   rogue_backend_op_mod_infos[ROGUE_BACKEND_OP_MOD_COUNT];

typedef struct rogue_backend_instr {
   rogue_instr instr;

   enum rogue_backend_op op;

   uint64_t mod;

   /* Backend instructions don't have source/dest modifiers. */

   rogue_instr_dst dst[ROGUE_BACKEND_OP_MAX_DSTS];
   rogue_dst_write dst_write[ROGUE_BACKEND_OP_MAX_DSTS];

   rogue_instr_src src[ROGUE_BACKEND_OP_MAX_SRCS];
   rogue_src_use src_use[ROGUE_BACKEND_OP_MAX_SRCS];
} rogue_backend_instr;

static inline void rogue_set_backend_op_mod(rogue_backend_instr *backend,
                                            enum rogue_backend_op_mod mod)
{
   backend->mod |= BITFIELD64_BIT(mod);
}

static inline bool
rogue_backend_op_mod_is_set(const rogue_backend_instr *backend,
                            enum rogue_backend_op_mod mod)
{
   return !!(backend->mod & BITFIELD64_BIT(mod));
}

rogue_backend_instr *rogue_backend_instr_create(rogue_block *block,
                                                enum rogue_backend_op op);

typedef struct rogue_ctrl_instr {
   rogue_instr instr;

   enum rogue_ctrl_op op;

   uint64_t mod;

   /* Control instructions don't have source/dest modifiers. */

   rogue_instr_dst dst[ROGUE_CTRL_OP_MAX_DSTS];
   rogue_dst_write dst_write[ROGUE_CTRL_OP_MAX_DSTS];

   rogue_instr_src src[ROGUE_CTRL_OP_MAX_SRCS];
   rogue_src_use src_use[ROGUE_CTRL_OP_MAX_SRCS];

   /* Is this instruction the start of a loop? */
   bool loop_start;

   /* Link to the start/end loop instruction from the end/start. */
   rogue_instr *loop_link;

   rogue_block *target_block;
   rogue_block_use block_use;
} rogue_ctrl_instr;

static inline void rogue_set_ctrl_op_mod(rogue_ctrl_instr *ctrl,
                                         enum rogue_ctrl_op_mod mod)
{
   ctrl->mod |= BITFIELD64_BIT(mod);
}

static inline bool rogue_ctrl_op_mod_is_set(const rogue_ctrl_instr *ctrl,
                                            enum rogue_ctrl_op_mod mod)
{
   return !!(ctrl->mod & BITFIELD64_BIT(mod));
}

/**
 * \brief Allocates and initializes a new control instruction.
 *
 * \param[in] block The block which will contain the instruction.
 * \param[in] op The ALU instruction operation.
 * \return The new instruction.
 */
rogue_ctrl_instr *rogue_ctrl_instr_create(rogue_block *block,
                                          enum rogue_ctrl_op op);

enum rogue_bitwise_op {
   ROGUE_BITWISE_OP_INVALID = 0,

   /* Real instructions. */
   ROGUE_BITWISE_OP_LSL0,
   ROGUE_BITWISE_OP_LSL2,
   ROGUE_BITWISE_OP_SHR,
   ROGUE_BITWISE_OP_ASR,
   ROGUE_BITWISE_OP_AND,
   ROGUE_BITWISE_OP_OR,
   ROGUE_BITWISE_OP_XOR,

   ROGUE_BITWISE_OP_REV,
   ROGUE_BITWISE_OP_CBS,
   ROGUE_BITWISE_OP_FTB,

   ROGUE_BITWISE_OP_MSK,

   ROGUE_BITWISE_OP_BYP0B,
   ROGUE_BITWISE_OP_BYP0C,
   ROGUE_BITWISE_OP_BYP0S,
   ROGUE_BITWISE_OP_BYP1L,

   ROGUE_BITWISE_OP_TZ,
   ROGUE_BITWISE_OP_TNZ,

   /* Pseudo-instructions. */
   ROGUE_BITWISE_OP_PSEUDO,
   ROGUE_BITWISE_OP_MOVI = ROGUE_BITWISE_OP_PSEUDO,
   ROGUE_BITWISE_OP_ISHL,
   ROGUE_BITWISE_OP_ISHR,
   ROGUE_BITWISE_OP_USHR,

   ROGUE_BITWISE_OP_IAND,
   ROGUE_BITWISE_OP_IOR,
   ROGUE_BITWISE_OP_IXOR,

   ROGUE_BITWISE_OP_INOT,
   ROGUE_BITWISE_OP_IREV,
   ROGUE_BITWISE_OP_ICBS,
   ROGUE_BITWISE_OP_IFTB,

   /* Sign extension (ASR)
    * out = ISXT in, sb, shift
    * out = (signed)(val << (31 - sb)) >> (31 + shift - sb)
    */
   ROGUE_BITWISE_OP_ISXT,

   ROGUE_BITWISE_OP_COUNT,
};

enum rogue_bitwise_op_mod {
   /* In order of priority */
   ROGUE_BITWISE_OP_MOD_TWB, /* Top word break. */
   ROGUE_BITWISE_OP_MOD_PWB, /* Partial word break. */
   ROGUE_BITWISE_OP_MOD_MTB, /* Mask top break. */
   ROGUE_BITWISE_OP_MOD_FTB, /* Find top break. */

   ROGUE_BITWISE_OP_MOD_COUNT,
};

typedef struct rogue_bitwise_op_mod_info {
   const char *str;
   uint64_t exclude; /* Can't use this op mod with any of these. */
   uint64_t require; /* Required op mods for this to be used (OR). */
} rogue_bitwise_op_mod_info;

extern const rogue_bitwise_op_mod_info
   rogue_bitwise_op_mod_infos[ROGUE_BITWISE_OP_MOD_COUNT];

#define ROGUE_BITWISE_OP_MAX_SRCS 7
#define ROGUE_BITWISE_OP_MAX_DSTS 2

typedef struct rogue_bitwise_op_info {
   const char *str;

   unsigned num_dsts;
   unsigned num_srcs;

   enum rogue_instr_phase phase;
   rogue_alu_io_info io;

   uint64_t supported_op_mods;
   uint64_t supported_dst_mods[ROGUE_BITWISE_OP_MAX_DSTS];
   uint64_t supported_src_mods[ROGUE_BITWISE_OP_MAX_SRCS];

   uint64_t supported_dst_types[ROGUE_BITWISE_OP_MAX_DSTS];
   uint64_t supported_src_types[ROGUE_BITWISE_OP_MAX_SRCS];

   unsigned dst_stride[ROGUE_CTRL_OP_MAX_DSTS];
   unsigned src_stride[ROGUE_CTRL_OP_MAX_SRCS];

   uint64_t dst_repeat_mask;
   uint64_t src_repeat_mask;

   unsigned valnum_src;
   uint64_t dst_valnum_mask;
   uint64_t src_valnum_mask;

   unsigned max_repeat;
} rogue_bitwise_op_info;

extern const rogue_bitwise_op_info
   rogue_bitwise_op_infos[ROGUE_BITWISE_OP_COUNT];

typedef struct rogue_bitwise_dst {
   rogue_ref ref;
   unsigned index;
} rogue_bitwise_dst;

typedef struct rogue_bitwise_src {
   rogue_ref ref;
   unsigned index;
} rogue_bitwise_src;

typedef struct rogue_bitwise_instr {
   rogue_instr instr;

   enum rogue_bitwise_op op;

   uint64_t mod;

   /* TODO NEXT: source/dest modifiers */

   rogue_instr_dst dst[ROGUE_BITWISE_OP_MAX_DSTS];
   rogue_dst_write dst_write[ROGUE_BITWISE_OP_MAX_DSTS];

   rogue_instr_src src[ROGUE_BITWISE_OP_MAX_SRCS];
   rogue_src_use src_use[ROGUE_BITWISE_OP_MAX_SRCS];
} rogue_bitwise_instr;

static inline void rogue_set_bitwise_op_mod(rogue_bitwise_instr *bitwise,
                                            enum rogue_bitwise_op_mod mod)
{
   bitwise->mod |= BITFIELD64_BIT(mod);
}

static inline bool
rogue_bitwise_op_mod_is_set(const rogue_bitwise_instr *bitwise,
                            enum rogue_bitwise_op_mod mod)
{
   return !!(bitwise->mod & BITFIELD64_BIT(mod));
}

/**
 * \brief Allocates and initializes a new bitwise instruction.
 *
 * \param[in] op The ALU instruction operation.
 * \return The new instruction.
 */
rogue_bitwise_instr *rogue_bitwise_instr_create(rogue_block *block,
                                                enum rogue_bitwise_op op);

/** Defines a cast function
 *
 * This macro defines a cast function from in_type to out_type where
 * out_type is some structure type that contains a field of type out_type.
 *
 * Note that you have to be a bit careful as the generated cast function
 * destroys constness.
 */
#define ROGUE_DEFINE_CAST(name,                           \
                          in_type,                        \
                          out_type,                       \
                          field,                          \
                          type_field,                     \
                          type_value)                     \
   static inline out_type *name(const in_type *parent)    \
   {                                                      \
      assert(parent && parent->type_field == type_value); \
      return list_entry(parent, out_type, field);         \
   }

ROGUE_DEFINE_CAST(rogue_instr_as_alu,
                  rogue_instr,
                  rogue_alu_instr,
                  instr,
                  type,
                  ROGUE_INSTR_TYPE_ALU)
ROGUE_DEFINE_CAST(rogue_instr_as_backend,
                  rogue_instr,
                  rogue_backend_instr,
                  instr,
                  type,
                  ROGUE_INSTR_TYPE_BACKEND)
ROGUE_DEFINE_CAST(rogue_instr_as_ctrl,
                  rogue_instr,
                  rogue_ctrl_instr,
                  instr,
                  type,
                  ROGUE_INSTR_TYPE_CTRL)
ROGUE_DEFINE_CAST(rogue_instr_as_bitwise,
                  rogue_instr,
                  rogue_bitwise_instr,
                  instr,
                  type,
                  ROGUE_INSTR_TYPE_BITWISE)

static inline enum rogue_io rogue_phase_io(uint64_t phase_io_set)
{
   assert(util_is_power_of_two_or_zero64(phase_io_set));

   if (!phase_io_set)
      return ROGUE_IO_INVALID;

   return ffsll(phase_io_set) - 1;
}

static inline uint64_t rogue_instr_src_io_src(const rogue_instr *instr,
                                              unsigned src_index)
{
   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU: {
      const rogue_alu_instr *alu = rogue_instr_as_alu(instr);
      const rogue_alu_op_info *info = &rogue_alu_op_infos[alu->op];
      return info->io.src_set[src_index];
   }

   case ROGUE_INSTR_TYPE_BACKEND: {
      const rogue_backend_instr *backend = rogue_instr_as_backend(instr);
      const rogue_backend_op_info *info = &rogue_backend_op_infos[backend->op];
      return info->io.src_set[src_index];
   }

   case ROGUE_INSTR_TYPE_CTRL: {
      const rogue_ctrl_instr *ctrl = rogue_instr_as_ctrl(instr);
      const rogue_ctrl_op_info *info = &rogue_ctrl_op_infos[ctrl->op];
      return info->io.src_set[src_index];
   }

   case ROGUE_INSTR_TYPE_BITWISE: {
      const rogue_bitwise_instr *bitwise = rogue_instr_as_bitwise(instr);
      const rogue_bitwise_op_info *info = &rogue_bitwise_op_infos[bitwise->op];
      return info->io.src_set[src_index];
   }

   default:
      unreachable("Unsupported instruction type.");
      break;
   }

   return ROGUE_IO_INVALID;
}

/* Maps sources and destinations ("inputs"/"outputs") to registers. */
typedef struct rogue_instr_group_io_sel {
   rogue_ref srcs[ROGUE_ISA_SRCS]; /** Upper + lower sources. */
   rogue_ref dsts[ROGUE_ISA_DSTS]; /** Destinations. */
   rogue_ref iss[ROGUE_ISA_ISSS]; /** Internal source selector (includes
                                     IS0/MUX). */
   /* Virtual, actually set up by the hardware automatically, but tracking it
    * ensures we don't try to use it in an unintended state. */
   rogue_ref fte;
   rogue_ref ft4;
   rogue_ref ft5;
} rogue_instr_group_io_sel;

static inline rogue_ref *
rogue_instr_group_io_sel_ref(rogue_instr_group_io_sel *map, enum rogue_io io)
{
   if (rogue_io_is_src(io))
      return &map->srcs[io - ROGUE_IO_S0];
   else if (rogue_io_is_dst(io))
      return &map->dsts[io - ROGUE_IO_W0];
   else if (rogue_io_is_iss(io))
      return &map->iss[io - ROGUE_IO_IS0];
   else if (io == ROGUE_IO_FTE)
      return &map->fte;
   else if (io == ROGUE_IO_FT4)
      return &map->ft4;
   else if (io == ROGUE_IO_FT5)
      return &map->ft5;
   unreachable("Unsupported io.");
}

/** Rogue instruction group. */
typedef struct rogue_instr_group {
   rogue_block *block;
   struct list_head link; /** Link in rogue_block::instrs. */

   rogue_instr *instrs[ROGUE_INSTR_PHASE_COUNT]; /** Instructions in group. */
   rogue_instr_group_io_sel io_sel; /** Source, destination, internal source
                                       selector maps. */

   struct {
      uint64_t phases; /** Instructions phases present. */

      enum rogue_exec_cond exec_cond;
      enum rogue_alu alu;

      bool end; /** Shader end flag. */
      bool atom;
      unsigned repeat;
   } header;

   struct {
      unsigned header;
      unsigned instrs[ROGUE_INSTR_PHASE_COUNT];
      unsigned lower_srcs;
      unsigned upper_srcs;
      unsigned iss;
      unsigned dsts;
      unsigned word_padding; /* Padding to make total size a word (% 2 == 0) */
      unsigned align_padding; /* Padding to align instruction position in memory
                               */
      unsigned total;

      unsigned offset;
   } size;

   struct {
      unsigned lower_src_index;
      unsigned upper_src_index;
      unsigned dst_index;
   } encode_info;

   unsigned index; /** For debug purposes. */
} rogue_instr_group;

#define rogue_foreach_instr_group_in_block(group, block) \
   list_for_each_entry (rogue_instr_group, group, &(block)->instrs, link)

#define rogue_foreach_instr_group_in_block_safe(group, block) \
   list_for_each_entry_safe (rogue_instr_group, group, &(block)->instrs, link)

#define rogue_foreach_instr_group_in_shader(group, shader) \
   rogue_foreach_block (_block, (shader))                  \
      rogue_foreach_instr_group_in_block ((group), _block)

#define rogue_foreach_instr_group_in_shader_safe(group, shader) \
   rogue_foreach_block_safe (_block, (shader))                  \
      rogue_foreach_instr_group_in_block_safe ((group), _block)

static inline rogue_instr_group *rogue_instr_group_create(rogue_block *block,
                                                          enum rogue_alu alu)
{
   rogue_instr_group *group = rzalloc_size(block, sizeof(*group));
   group->header.alu = alu;
   group->block = block;
   return group;
}

typedef struct rogue_build_ctx rogue_build_ctx;

enum rogue_mutex_state {
   ROGUE_MUTEX_STATE_RELEASED = 0,

   ROGUE_MUTEX_STATE_LOCKED = BITFIELD_BIT(0),
};

/** Rogue shader object. */
typedef struct rogue_shader {
   gl_shader_stage stage; /** Shader stage. */

   rogue_build_ctx *ctx; /** Build context. */

   unsigned next_instr; /** Next instruction index. */
   unsigned next_block; /** Next block index. */

   struct list_head blocks; /** List of basic blocks. */
   struct util_sparse_array block_cache;

   struct list_head regs[ROGUE_REG_CLASS_COUNT]; /** List of registers used by
                                                    the shader. */
   BITSET_WORD *regs_used[ROGUE_REG_CLASS_COUNT]; /** Bitset of register numbers
                                                     used. */
   struct util_sparse_array reg_cache[ROGUE_REG_CLASS_COUNT];

   struct list_head regarrays; /** List of register arrays used by the shader.
                                */
   struct util_sparse_array regarray_cache;

   struct list_head drc_trxns[ROGUE_DRCS]; /** List of drc transactions. */

   struct list_head imm_uses; /** List of immediate value uses. */

   struct util_sparse_array imm_allocs;

   bool emc_initialised; /** Has the EMC been initialised? */
   /* Number of conditional nesting levels in the innermost loop. */
   unsigned loop_nestings;
   /* Number of NIR loops in this shader. */
   unsigned loops;

   enum rogue_mutex_state mutex_state;

   bool is_grouped; /** Whether the instructions are grouped. */

   const char *name; /** Shader name. */
} rogue_shader;

static inline void rogue_set_shader_name(rogue_shader *shader, const char *name)
{
   shader->name = ralloc_strdup(shader, name);
}

#define BLOCK_CACHE_KEY_INDEX_BITS 63
#define BLOCK_CACHE_KEY_IS_NIR_BLOCK_BITS 1
struct rogue_block_cache_key {
   union {
      struct {
         uint64_t index : BLOCK_CACHE_KEY_INDEX_BITS;
         bool is_nir_block : BLOCK_CACHE_KEY_IS_NIR_BLOCK_BITS;
      } PACKED;

      uint64_t val;
   } PACKED;
} PACKED;
static_assert(sizeof(struct rogue_block_cache_key) == sizeof(uint64_t),
              "sizeof(struct rogue_block_cache_key) != sizeof(uint64_t)");

static inline uint64_t rogue_block_cache_key(bool is_nir_block, unsigned index)
{
   assert(util_last_bit(index) <= BLOCK_CACHE_KEY_INDEX_BITS);
   assert(util_last_bit(is_nir_block) <= BLOCK_CACHE_KEY_IS_NIR_BLOCK_BITS);

   return (struct rogue_block_cache_key){
      .index = index,
      .is_nir_block = is_nir_block,
   }
      .val;
}

rogue_block *
rogue_block_cached(rogue_shader *shader, bool is_nir_block, unsigned index);

static inline bool rogue_reg_is_used(const rogue_shader *shader,
                                     enum rogue_reg_class class,
                                     unsigned index)
{
   return BITSET_TEST(shader->regs_used[class], index);
}

static inline void rogue_set_reg_use(rogue_shader *shader,
                                     enum rogue_reg_class class,
                                     unsigned index)
{
   BITSET_SET(shader->regs_used[class], index);
}

static inline void rogue_clear_reg_use(rogue_shader *shader,
                                       enum rogue_reg_class class,
                                       unsigned index)
{
   BITSET_CLEAR(shader->regs_used[class], index);
}

/**
 * \brief Allocates and initializes a new rogue_shader object.
 *
 * \param[in] mem_ctx The parent memory context.
 * \param[in] stage The shader stage.
 * \return The new shader.
 */
rogue_shader *rogue_shader_create(void *mem_ctx, gl_shader_stage stage);

rogue_reg *rogue_ssa_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_emc_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_temp_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_coeff_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_shared_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_const_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_pixout_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_special_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_vtxin_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_vtxout_reg(rogue_shader *shader, unsigned index);

rogue_reg *rogue_index_reg(rogue_shader *shader, unsigned index);

rogue_reg *
rogue_ssa_vec_reg(rogue_shader *shader, unsigned index, unsigned component);

void rogue_reg_delete(rogue_reg *reg);

/**
 * \brief Returns a reference to a virtual execution mask counter register.
 *
 * \param[in] reg The register.
 * \return The reference.
 */
static inline rogue_ref rogue_ref_emc(rogue_shader *shader)
{
   return (rogue_ref){
      .type = ROGUE_REF_TYPE_REG,
      .reg = rogue_emc_reg(shader, 0),
   };
}

rogue_regarray *
rogue_ssa_regarray(rogue_shader *shader, unsigned size, unsigned start_index);

rogue_regarray *
rogue_temp_regarray(rogue_shader *shader, unsigned size, unsigned start_index);

rogue_regarray *
rogue_coeff_regarray(rogue_shader *shader, unsigned size, unsigned start_index);

rogue_regarray *rogue_shared_regarray(rogue_shader *shader,
                                      unsigned size,
                                      unsigned start_index);

rogue_regarray *
rogue_vtxin_regarray(rogue_shader *shader, unsigned size, unsigned start_index);

rogue_regarray *rogue_ssa_vec_regarray(rogue_shader *shader,
                                       unsigned size,
                                       unsigned start_index,
                                       unsigned component);

rogue_regarray *rogue_regarray_cached(rogue_shader *shader,
                                      unsigned size,
                                      enum rogue_reg_class class,
                                      uint32_t start_index,
                                      bool allow_overlap);

rogue_regarray *rogue_vec_regarray_cached(rogue_shader *shader,
                                          unsigned size,
                                          enum rogue_reg_class class,
                                          uint32_t start_index,
                                          uint8_t component);

static inline bool rogue_regarray_is_unused(rogue_regarray *regarray)
{
   return list_is_empty(&regarray->uses) && list_is_empty(&regarray->writes);
}

static void rogue_regarray_delete(rogue_regarray *regarray)
{
   assert(rogue_regarray_is_unused(regarray));

   if (!regarray->parent) {
      for (unsigned u = 0; u < regarray->size; ++u)
         rogue_reg_delete(regarray->regs[u]);
   }

   if (regarray->cached && *regarray->cached == regarray)
      *regarray->cached = NULL;

   list_del(&regarray->link);
   if (regarray->parent)
      list_del(&regarray->child_link);
   ralloc_free(regarray);
}

static inline void rogue_reset_reg_usage(rogue_shader *shader,
                                         enum rogue_reg_class class)
{
   const rogue_reg_class_info *info = &rogue_reg_class_infos[class];

   if (info->num) {
      memset(shader->regs_used[class],
             0,
             sizeof(*shader->regs_used[class]) * BITSET_WORDS(info->num));
   }

   rogue_foreach_reg (reg, shader, class) {
      reg->dirty = false;
   }
}

bool rogue_reg_set(rogue_shader *shader,
                   rogue_reg *reg,
                   enum rogue_reg_class class,
                   unsigned index);

bool rogue_reg_rewrite(rogue_shader *shader,
                       rogue_reg *reg,
                       enum rogue_reg_class class,
                       unsigned index);

bool rogue_regarray_set(rogue_shader *shader,
                        rogue_regarray *regarray,
                        enum rogue_reg_class class,
                        unsigned base_index,
                        bool set_regs);

bool rogue_regarray_rewrite(rogue_shader *shader,
                            rogue_regarray *regarray,
                            enum rogue_reg_class class,
                            unsigned base_index);

/** Cursor for Rogue instructions/groups and basic blocks. */
typedef struct rogue_cursor {
   bool block;
   struct list_head *prev; /** Linked-list pointer to before the object. */
   bool first; /** Whether the cursor is pointing to the first element. */
} rogue_cursor;

/**
 * \brief Returns a cursor set to the beginning of the shader.
 *
 * \param[in] shader The shader.
 * \return The cursor.
 */
static inline rogue_cursor rogue_cursor_before_shader(rogue_shader *shader)
{
   return (rogue_cursor){
      .block = true,
      .prev = &shader->blocks,
      .first = true,
   };
}

/**
 * \brief Returns a cursor set to before a block.
 *
 * \param[in] block The block.
 * \return The cursor.
 */
static inline rogue_cursor rogue_cursor_before_block(rogue_block *block)
{
   return (rogue_cursor){
      .block = true,
      .prev = block->link.prev,
      .first = (block->link.prev == &block->shader->blocks),
   };
}

/**
 * \brief Returns a cursor set to after a block.
 *
 * \param[in] block The block.
 * \return The cursor.
 */
static inline rogue_cursor rogue_cursor_after_block(rogue_block *block)
{
   return (rogue_cursor){
      .block = true,
      .prev = &block->link,
   };
}

/**
 * \brief Returns a cursor set to before an instruction.
 *
 * \param[in] instr The instruction.
 * \return The cursor.
 */
static inline rogue_cursor rogue_cursor_before_instr(rogue_instr *instr)
{
   return (rogue_cursor){
      .block = false,
      .prev = instr->link.prev,
      .first = (instr->link.prev == &instr->block->instrs),
   };
}

/**
 * \brief Returns a cursor set to after an instruction.
 *
 * \param[in] instr The instruction.
 * \return The cursor.
 */
static inline rogue_cursor rogue_cursor_after_instr(rogue_instr *instr)
{
   return (rogue_cursor){
      .block = false,
      .prev = &instr->link,
   };
}

/**
 * \brief Allocates and initializes a new rogue_block object.
 *
 * \param[in] shader The shader which will contain the block.
 * \param[in] label The (optional) block label.
 * \param[in] nir_index The block's NIR index, or ~0U if not from NIR.
 * \return The new block.
 */
rogue_block *
rogue_block_create(rogue_shader *shader, const char *label, unsigned nir_index);

/**
 * \brief Returns the block currently being pointed to by the cursor.
 *
 * If the cursor is currently pointing to a block, this function will
 * directly return said block. If it is pointing to an instruction, it
 * will return the block that said instruction is a part of.
 *
 * \param[in] cursor A cursor.
 * \return The block being pointed to.
 */
static inline rogue_block *rogue_cursor_block(rogue_cursor cursor)
{
   rogue_block *block = NULL;

   if (cursor.block) {
      assert(!cursor.first && "Cursor is not pointing at a block.");
      block = list_entry(cursor.prev, rogue_block, link);
   } else {
      block = cursor.first ? list_entry(cursor.prev, rogue_block, instrs)
                           : list_entry(cursor.prev, rogue_instr, link)->block;
   }

   return block;
}

/**
 * \brief Inserts a basic block at the specified cursor position.
 *
 * \param[in] block The basic block to insert.
 * \param[in] cursor The cursor.
 */
static inline void rogue_block_insert(rogue_block *block, rogue_cursor cursor)
{
   struct list_head *list = cursor.prev;

   /* If the cursor is pointing at an instruction, the block
    * is always going to be inserted *after* the block
    * that the instruction is in.
    */
   if (!cursor.block)
      list = &rogue_cursor_block(cursor)->link;

   list_add(&block->link, list);
}

void rogue_link_instr_write(rogue_instr *instr);

void rogue_link_instr_use(rogue_instr *instr);

void rogue_unlink_instr_write(rogue_instr *instr);

void rogue_unlink_instr_use(rogue_instr *instr);

/**
 * \brief Inserts an instruction at the specified cursor position.
 *
 * \param[in] instr The instruction to insert.
 * \param[in] cursor The cursor.
 */
static inline void rogue_instr_insert(rogue_instr *instr, rogue_cursor cursor)
{
   struct list_head *list = cursor.prev;

   /* If the cursor is pointing at block, the instruction
    * is always going to be inserted at the end of any other
    * instructions in the block.
    */
   if (cursor.block)
      list = rogue_cursor_block(cursor)->instrs.prev;

   list_add(&instr->link, list);

   rogue_link_instr_write(instr);
   rogue_link_instr_use(instr);
}

static inline void rogue_instr_delete(rogue_instr *instr)
{
   rogue_unlink_instr_use(instr);
   rogue_unlink_instr_write(instr);

   list_del(&instr->link);

   ralloc_free(instr);
}

static inline void
rogue_link_drc_trxn(rogue_shader *shader, rogue_instr *instr, rogue_drc *drc)
{
   unsigned index = drc->index;
   assert(index < ROGUE_DRCS);

   drc->trxn.acquire = instr;
   list_addtail(&drc->trxn.link, &shader->drc_trxns[index]);
}

static inline void
rogue_unlink_drc_trxn(rogue_shader *shader, rogue_instr *instr, rogue_drc *drc)
{
   ASSERTED unsigned index = drc->index;
   assert(index < ROGUE_DRCS);
   assert(drc->trxn.acquire == instr);

   if (drc->trxn.release)
      rogue_instr_delete(drc->trxn.release);

   list_del(&drc->trxn.link);
}

static inline void rogue_link_imm_use(rogue_shader *shader,
                                      rogue_instr *instr,
                                      unsigned src_index,
                                      rogue_imm *imm)
{
   rogue_imm_use *imm_use = &imm->use;

   imm_use->instr = instr;
   imm_use->src_index = src_index;
   imm_use->imm = &imm->imm;

   list_addtail(&imm_use->link, &shader->imm_uses);
}

static inline void rogue_unlink_imm_use(rogue_instr *instr,
                                        rogue_imm_use *imm_use)
{
   assert(imm_use->instr == instr);
   list_del(&imm_use->link);
}

static inline void rogue_link_instr_write_reg(rogue_instr *instr,
                                              rogue_reg_write *write,
                                              rogue_reg *reg,
                                              unsigned dst_index)
{
   write->instr = instr;
   write->dst_index = dst_index;
   list_addtail(&write->link, &reg->writes);
}

static inline void rogue_unlink_instr_write_reg(rogue_instr *instr,
                                                rogue_reg_write *write)
{
   assert(write->instr == instr);
   write->instr = NULL;
   list_del(&write->link);
}

static inline void rogue_link_instr_write_regarray(rogue_instr *instr,
                                                   rogue_regarray_write *write,
                                                   rogue_regarray *regarray,
                                                   unsigned dst_index)
{
   write->instr = instr;
   write->dst_index = dst_index;
   list_addtail(&write->link, &regarray->writes);
}

static inline void
rogue_unlink_instr_write_regarray(rogue_instr *instr,
                                  rogue_regarray_write *write)
{
   assert(write->instr == instr);
   write->instr = NULL;
   list_del(&write->link);
}

static inline void rogue_link_instr_use_reg(rogue_instr *instr,
                                            rogue_reg_use *use,
                                            rogue_reg *reg,
                                            unsigned src_index)
{
   use->instr = instr;
   use->src_index = src_index;
   list_addtail(&use->link, &reg->uses);
}

static inline void rogue_unlink_instr_use_reg(rogue_instr *instr,
                                              rogue_reg_use *use)
{
   assert(use->instr == instr);
   use->instr = NULL;
   list_del(&use->link);
}

static inline void rogue_link_instr_use_regarray(rogue_instr *instr,
                                                 rogue_regarray_use *use,
                                                 rogue_regarray *regarray,
                                                 unsigned src_index)
{
   use->instr = instr;
   use->src_index = src_index;
   list_addtail(&use->link, &regarray->uses);
}

static inline void rogue_unlink_instr_use_regarray(rogue_instr *instr,
                                                   rogue_regarray_use *use)
{
   assert(use->instr == instr);
   use->instr = NULL;
   list_del(&use->link);
}

static inline void rogue_link_instr_use_block(rogue_instr *instr,
                                              rogue_block_use *block_use,
                                              rogue_block *target_block)
{
   assert(!block_use->instr);
   block_use->instr = instr;
   list_addtail(&block_use->link, &target_block->uses);
}

static inline void rogue_unlink_instr_use_block(rogue_instr *instr,
                                                rogue_block_use *block_use)
{
   assert(block_use->instr == instr);
   block_use->instr = NULL;
   list_del(&block_use->link);
}

static inline bool rogue_dst_reg_replace(rogue_reg_write *write,
                                         rogue_reg *new_reg)
{
   unsigned dst_index = write->dst_index;
   rogue_instr *instr = write->instr;
   rogue_ref *ref;

   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU:
      ref = &rogue_instr_as_alu(instr)->dst[dst_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BACKEND:
      ref = &rogue_instr_as_backend(instr)->dst[dst_index].ref;
      break;

   case ROGUE_INSTR_TYPE_CTRL:
      ref = &rogue_instr_as_ctrl(instr)->dst[dst_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BITWISE:
      ref = &rogue_instr_as_bitwise(instr)->dst[dst_index].ref;
      break;

   default:
      unreachable("Unsupported instruction type.");
      return false;
   }

   /* We don't want to be modifying regarrays. */
   assert(rogue_ref_is_reg(ref));

   if (ref->reg == new_reg)
      return false;

   rogue_unlink_instr_write_reg(instr, write);
   *ref = rogue_ref_reg(new_reg);
   rogue_link_instr_write_reg(instr, write, new_reg, dst_index);

   return true;
}

static inline bool rogue_src_reg_replace(rogue_reg_use *use, rogue_reg *new_reg)
{
   unsigned src_index = use->src_index;
   rogue_instr *instr = use->instr;
   rogue_ref *ref;

   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU:
      ref = &rogue_instr_as_alu(instr)->src[src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BACKEND:
      ref = &rogue_instr_as_backend(instr)->src[src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_CTRL:
      ref = &rogue_instr_as_ctrl(instr)->src[src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BITWISE:
      ref = &rogue_instr_as_bitwise(instr)->src[src_index].ref;
      break;

   default:
      unreachable("Unsupported instruction type.");
      return false;
   }

   /* We don't want to be modifying regarrays. */
   assert(rogue_ref_is_reg(ref));

   if (ref->reg == new_reg)
      return false;

   rogue_unlink_instr_use_reg(instr, use);
   *ref = rogue_ref_reg(new_reg);
   rogue_link_instr_use_reg(instr, use, new_reg, src_index);

   return true;
}

/* Special case: regarrays of size 1. */
static inline bool rogue_src_reg_replace_regarray(rogue_reg_use *reg_use,
                                                  rogue_regarray *new_regarray)
{
   unsigned src_index = reg_use->src_index;
   rogue_instr *instr = reg_use->instr;
   rogue_ref *ref;

   rogue_regarray_use *regarray_use;
   assert(new_regarray->size == 1);

   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU: {
      rogue_alu_instr *alu = rogue_instr_as_alu(instr);
      ref = &alu->src[src_index].ref;
      regarray_use = &alu->src_use[src_index].regarray;
      break;
   }

   case ROGUE_INSTR_TYPE_BACKEND: {
      rogue_backend_instr *backend = rogue_instr_as_backend(instr);
      ref = &backend->src[src_index].ref;
      regarray_use = &backend->src_use[src_index].regarray;
      break;
   }

   case ROGUE_INSTR_TYPE_CTRL: {
      rogue_ctrl_instr *ctrl = rogue_instr_as_ctrl(instr);
      ref = &ctrl->src[src_index].ref;
      regarray_use = &ctrl->src_use[src_index].regarray;
      break;
   }

   case ROGUE_INSTR_TYPE_BITWISE: {
      rogue_bitwise_instr *bitwise = rogue_instr_as_bitwise(instr);
      ref = &bitwise->src[src_index].ref;
      regarray_use = &bitwise->src_use[src_index].regarray;
      break;
   }

   default:
      unreachable("Unsupported instruction type.");
      return false;
   }

   /* We don't want to be modifying regarrays. */
   assert(rogue_ref_is_reg(ref));

   rogue_unlink_instr_use_reg(instr, reg_use);
   *ref = rogue_ref_regarray(new_regarray);
   rogue_link_instr_use_regarray(instr, regarray_use, new_regarray, src_index);

   return true;
}

static inline bool rogue_reg_replace(rogue_reg *old_reg, rogue_reg *new_reg)
{
   bool replaced = true;

   rogue_foreach_reg_write_safe (write, old_reg) {
      replaced &= rogue_dst_reg_replace(write, new_reg);
   }

   rogue_foreach_reg_use_safe (use, old_reg) {
      replaced &= rogue_src_reg_replace(use, new_reg);
   }

   if (replaced)
      rogue_reg_delete(old_reg);

   return replaced;
}

/* TODO: try and commonise this with the reg one! */
static inline bool rogue_dst_regarray_replace(rogue_regarray_write *write,
                                              rogue_regarray *new_regarray)
{
   unsigned dst_index = write->dst_index;
   rogue_instr *instr = write->instr;
   rogue_ref *ref;

   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU:
      ref = &rogue_instr_as_alu(instr)->dst[dst_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BACKEND:
      ref = &rogue_instr_as_backend(instr)->dst[dst_index].ref;
      break;

   case ROGUE_INSTR_TYPE_CTRL:
      ref = &rogue_instr_as_ctrl(instr)->dst[dst_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BITWISE:
      ref = &rogue_instr_as_bitwise(instr)->dst[dst_index].ref;
      break;

   default:
      unreachable("Unsupported instruction type.");
      return false;
   }

   /* We don't want to be modifying regs. */
   assert(rogue_ref_is_regarray(ref));

   if (ref->regarray == new_regarray)
      return false;

   rogue_unlink_instr_write_regarray(instr, write);
   *ref = rogue_ref_regarray(new_regarray);
   rogue_link_instr_write_regarray(instr, write, new_regarray, dst_index);

   return true;
}

static inline bool rogue_src_regarray_replace(rogue_regarray_use *use,
                                              rogue_regarray *new_regarray)
{
   unsigned src_index = use->src_index;
   rogue_instr *instr = use->instr;
   rogue_ref *ref;

   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU:
      ref = &rogue_instr_as_alu(instr)->src[src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BACKEND:
      ref = &rogue_instr_as_backend(instr)->src[src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_CTRL:
      ref = &rogue_instr_as_ctrl(instr)->src[src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BITWISE:
      ref = &rogue_instr_as_bitwise(instr)->src[src_index].ref;
      break;

   default:
      unreachable("Unsupported instruction type.");
      return false;
   }

   /* We don't want to be modifying reg. */
   assert(rogue_ref_is_regarray(ref));

   if (ref->regarray == new_regarray)
      return false;

   rogue_unlink_instr_use_regarray(instr, use);
   *ref = rogue_ref_regarray(new_regarray);
   rogue_link_instr_use_regarray(instr, use, new_regarray, src_index);

   return true;
}

static inline bool rogue_regarray_replace(rogue_shader *shader,
                                          rogue_regarray *old_regarray,
                                          rogue_regarray *new_regarray,
                                          bool allow_overlap)
{
   bool replaced = true;

   assert(!old_regarray->parent);

   rogue_foreach_regarray_write_safe (write, old_regarray) {
      replaced &= rogue_dst_regarray_replace(write, new_regarray);
   }

   rogue_foreach_regarray_use_safe (use, old_regarray) {
      replaced &= rogue_src_regarray_replace(use, new_regarray);
   }

   enum rogue_reg_class new_class = new_regarray->regs[0]->class;
   unsigned new_base_index = new_regarray->regs[0]->index;

   /* Replace subarrays. */
   rogue_foreach_subarray_safe (old_subarray, old_regarray) {
      unsigned idx_offset =
         old_subarray->regs[0]->index - old_regarray->regs[0]->index;
      rogue_regarray *new_subarray =
         rogue_regarray_cached(shader,
                               old_subarray->size,
                               new_class,
                               new_base_index + idx_offset,
                               allow_overlap);

      rogue_foreach_regarray_write_safe (write, old_subarray) {
         replaced &= rogue_dst_regarray_replace(write, new_subarray);
      }

      rogue_foreach_regarray_use_safe (use, old_subarray) {
         replaced &= rogue_src_regarray_replace(use, new_subarray);
      }

      rogue_regarray_delete(old_subarray);
   }

   rogue_regarray_delete(old_regarray);

   return replaced;
}

static inline bool rogue_src_imm_replace(rogue_imm_use *imm_use,
                                         rogue_reg *new_reg)
{
   unsigned src_index = imm_use->src_index;
   rogue_instr *instr = imm_use->instr;
   rogue_ref *ref;
   rogue_reg_use *reg_use;

   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU:
      ref = &rogue_instr_as_alu(instr)->src[src_index].ref;
      reg_use = &rogue_instr_as_alu(instr)->src_use[src_index].reg;
      break;

   case ROGUE_INSTR_TYPE_BACKEND:
      ref = &rogue_instr_as_backend(instr)->src[src_index].ref;
      reg_use = &rogue_instr_as_backend(instr)->src_use[src_index].reg;
      break;

   case ROGUE_INSTR_TYPE_CTRL:
      ref = &rogue_instr_as_ctrl(instr)->src[src_index].ref;
      reg_use = &rogue_instr_as_ctrl(instr)->src_use[src_index].reg;
      break;

   case ROGUE_INSTR_TYPE_BITWISE:
      ref = &rogue_instr_as_bitwise(instr)->src[src_index].ref;
      reg_use = &rogue_instr_as_bitwise(instr)->src_use[src_index].reg;
      break;

   default:
      unreachable("Unsupported instruction type.");
      return false;
   }

   assert(rogue_ref_is_imm(ref));

   rogue_unlink_imm_use(instr, imm_use);
   *ref = rogue_ref_reg(new_reg);
   rogue_link_instr_use_reg(instr, reg_use, new_reg, src_index);

   return true;
}

static inline bool rogue_instr_is_nop_end(const rogue_instr *instr)
{
   if (instr->type != ROGUE_INSTR_TYPE_CTRL)
      return false;

   const rogue_ctrl_instr *ctrl = rogue_instr_as_ctrl(instr);

   if (ctrl->op == ROGUE_CTRL_OP_END)
      return true;

   if (ctrl->op != ROGUE_CTRL_OP_NOP)
      return false;

   return rogue_ctrl_op_mod_is_set(ctrl, ROGUE_CTRL_OP_MOD_END);
}

static bool rogue_instr_is_pseudo(const rogue_instr *instr)
{
   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU:
      return rogue_instr_as_alu(instr)->op >= ROGUE_ALU_OP_PSEUDO;

   case ROGUE_INSTR_TYPE_BACKEND:
      return rogue_instr_as_backend(instr)->op >= ROGUE_BACKEND_OP_PSEUDO;

   case ROGUE_INSTR_TYPE_CTRL:
      return rogue_instr_as_ctrl(instr)->op >= ROGUE_CTRL_OP_PSEUDO;

   case ROGUE_INSTR_TYPE_BITWISE:
      return rogue_instr_as_bitwise(instr)->op >= ROGUE_BITWISE_OP_PSEUDO;

   default:
      break;
   }

   unreachable("Unsupported instruction type.");
}

static inline enum rogue_instr_phase rogue_instr_phase(const rogue_instr *instr)
{
   if (rogue_instr_is_pseudo(instr))
      return ROGUE_INSTR_PHASE_INVALID;

   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU: {
      enum rogue_alu_op op = rogue_instr_as_alu(instr)->op;
      return rogue_alu_op_infos[op].phase;
   }

   case ROGUE_INSTR_TYPE_BACKEND:
      /* Backend instructions are always in the backend phase. */
      return ROGUE_INSTR_PHASE_BACKEND;

   case ROGUE_INSTR_TYPE_CTRL:
      /* Control instructions can't be co-issued. */
      return ROGUE_INSTR_PHASE_CTRL;

   case ROGUE_INSTR_TYPE_BITWISE: {
      enum rogue_bitwise_op op = rogue_instr_as_bitwise(instr)->op;
      return rogue_bitwise_op_infos[op].phase;
   }

   default:
      break;
   }

   unreachable("Unsupported instruction type.");
}

static inline bool rogue_phase_occupied(enum rogue_instr_phase phase,
                                        uint64_t occupied_phases)
{
   return !!(BITFIELD_BIT(phase) & occupied_phases);
}

static inline bool rogue_can_replace_reg_use(rogue_reg_use *use,
                                             const rogue_reg *new_reg)
{
#if 0
   const rogue_reg_class_info *info = &rogue_reg_class_infos[new_reg->class];
   const rogue_instr *instr = use->instr;
   enum rogue_io io_src = rogue_phase_io(rogue_instr_src_io_src(instr, use->src_index)); /* TODO */
   return rogue_io_supported(io_src, info->supported_io_srcs);
#endif

   /* TODO: this was incorrectly implemented before; patching out for now. */
   return false;
}

typedef struct rogue_ref64 {
   rogue_ref ref64;
   rogue_ref lo32;
   rogue_ref hi32;
} rogue_ref64;

static struct rogue_ref64 rogue_ssa_ref64(rogue_shader *shader, unsigned index)
{
   return (rogue_ref64){
      .ref64 = rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 2, index, 0)),
      .lo32 = rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 1, index, 0)),
      .hi32 = rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 1, index, 1)),
   };
}

static struct rogue_ref64 rogue_temp_ref64(rogue_shader *shader, unsigned index)
{
   return (rogue_ref64){
      .ref64 = rogue_ref_regarray(rogue_temp_regarray(shader, 2, index)),
      .lo32 = rogue_ref_regarray(rogue_temp_regarray(shader, 1, index)),
      .hi32 = rogue_ref_regarray(rogue_temp_regarray(shader, 1, index + 1)),
   };
}

static struct rogue_ref64 rogue_shared_ref64(rogue_shader *shader,
                                             unsigned index)
{
   return (rogue_ref64){
      .ref64 = rogue_ref_regarray(rogue_shared_regarray(shader, 2, index)),
      .lo32 = rogue_ref_regarray(rogue_shared_regarray(shader, 1, index)),
      .hi32 = rogue_ref_regarray(rogue_shared_regarray(shader, 1, index + 1)),
   };
}

static inline unsigned rogue_index_from_regarray(rogue_regarray *regarray)
{
   const struct rogue_reg_cache_key cache_key = {
      .val = regarray->regs[0]->index,
   };
   assert(!cache_key.component);
   assert(cache_key.vec); /* TODO: maybe not needed? */

   return cache_key.index;
}

static struct rogue_ref64
rogue_ssa_ref64_from_regarray(rogue_shader *shader, rogue_regarray *regarray)
{
   assert(regarray->size == 2);
   assert(regarray->regs[0]->class == ROGUE_REG_CLASS_SSA);

   unsigned index = rogue_index_from_regarray(regarray);

   return (rogue_ref64){
      .ref64 = rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 2, index, 0)),
      .lo32 = rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 1, index, 0)),
      .hi32 = rogue_ref_regarray(rogue_ssa_vec_regarray(shader, 1, index, 1)),
   };
}

static struct rogue_ref64
rogue_ssa_ref64_from_alu_dst(rogue_shader *shader,
                             const rogue_alu_instr *alu,
                             unsigned dst_num)
{
   assert(rogue_ref_is_regarray(&alu->dst[dst_num].ref));
   return rogue_ssa_ref64_from_regarray(shader, alu->dst[dst_num].ref.regarray);
}

static struct rogue_ref64
rogue_ssa_ref64_from_alu_src(rogue_shader *shader,
                             const rogue_alu_instr *alu,
                             unsigned src_num)
{
   assert(rogue_ref_is_regarray(&alu->src[src_num].ref));
   return rogue_ssa_ref64_from_regarray(shader, alu->src[src_num].ref.regarray);
}

#define ROGUE_NO_CONST_REG ~0

unsigned rogue_constreg_lookup(rogue_imm_t imm);

/* Instruction iteration utils. */
typedef struct rogue_instr_filter {
   BITSET_DECLARE(alu_mask, ROGUE_ALU_OP_COUNT);
   BITSET_DECLARE(backend_mask, ROGUE_BACKEND_OP_COUNT);
   BITSET_DECLARE(ctrl_mask, ROGUE_CTRL_OP_COUNT);
   BITSET_DECLARE(bitwise_mask, ROGUE_BITWISE_OP_COUNT);
} rogue_instr_filter;

typedef bool (*rogue_instr_find_cb)(const rogue_instr *instr,
                                    const void *instr_as,
                                    unsigned op,
                                    void *user_data);

static inline bool rogue_find_instrs_impl(const rogue_instr *instr,
                                          const rogue_instr_filter *filter,
                                          rogue_instr_find_cb cb,
                                          void *user_data)
{
   switch (instr->type) {
   case ROGUE_INSTR_TYPE_ALU: {
      const rogue_alu_instr *alu = rogue_instr_as_alu(instr);
      if (BITSET_TEST(filter->alu_mask, alu->op))
         return cb(instr, alu, alu->op, user_data);
      return true;
   }

   case ROGUE_INSTR_TYPE_BACKEND: {
      const rogue_backend_instr *backend = rogue_instr_as_backend(instr);
      if (BITSET_TEST(filter->backend_mask, backend->op))
         return cb(instr, backend, backend->op, user_data);
      return true;
   }

   case ROGUE_INSTR_TYPE_CTRL: {
      const rogue_ctrl_instr *ctrl = rogue_instr_as_ctrl(instr);
      if (BITSET_TEST(filter->ctrl_mask, ctrl->op))
         return cb(instr, ctrl, ctrl->op, user_data);
      return true;
   }

   case ROGUE_INSTR_TYPE_BITWISE: {
      const rogue_bitwise_instr *bitwise = rogue_instr_as_bitwise(instr);
      if (BITSET_TEST(filter->bitwise_mask, bitwise->op))
         return cb(instr, bitwise, bitwise->op, user_data);
      return true;
   }

   default:
      break;
   }

   unreachable("Unsupported instruction type.");
   return false;
}

static inline void rogue_find_instrs(const rogue_shader *shader,
                                     const rogue_instr_filter *filter,
                                     rogue_instr_find_cb cb,
                                     void *user_data)
{
   if (!shader->is_grouped) {
      rogue_foreach_instr_in_shader (instr, shader) {
         if (!rogue_find_instrs_impl(instr, filter, cb, user_data))
            return;
      }
   } else {
      rogue_foreach_instr_group_in_shader (group, shader) {
         rogue_foreach_phase_in_set (p, group->header.phases) {
            const rogue_instr *instr = group->instrs[p];
            if (!rogue_find_instrs_impl(instr, filter, cb, user_data))
               return;
         }
      }
   }
}

static inline const rogue_reg *rogue_reg_from_use(const rogue_reg_use *use)
{
   assert(use->instr);

   rogue_ref *ref;
   switch (use->instr->type) {
   case ROGUE_INSTR_TYPE_ALU:
      ref = &rogue_instr_as_alu(use->instr)->src[use->src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BACKEND:
      ref = &rogue_instr_as_backend(use->instr)->src[use->src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_CTRL:
      ref = &rogue_instr_as_ctrl(use->instr)->src[use->src_index].ref;
      break;

   case ROGUE_INSTR_TYPE_BITWISE:
      ref = &rogue_instr_as_bitwise(use->instr)->src[use->src_index].ref;
      break;

   default:
      unreachable("Unsupported instruction type.");
   }

   assert(rogue_ref_is_reg(ref));
   return ref->reg;
}

/* Printing */

void rogue_print_color(bool print_color);

/**
 * \brief Prints a shader's Rogue IR/assembly.
 *
 * \param[in] fp The file pointer to use for printing.
 * \param[in] shader The shader to print.
 */
void rogue_print_shader(FILE *fp, const rogue_shader *shader);

/**
 * \brief Prints an instruction.
 *
 * \param[in] fp The file pointer to use for printing.
 * \param[in] instr The instruction to print.
 */
void rogue_print_instr(FILE *fp, const rogue_instr *instr);

void rogue_print_reg_raw(FILE *fp, enum rogue_reg_class class, unsigned index);

void rogue_print_reg(FILE *fp, const rogue_reg *reg, enum rogue_idx idx);

void rogue_print_regarray_raw(FILE *fp,
                              enum rogue_reg_class class,
                              unsigned base_index,
                              unsigned size);

void rogue_print_regarray(FILE *fp, const rogue_regarray *regarray);

void rogue_print_reg_writes(FILE *fp, const rogue_shader *shader);

void rogue_print_reg_uses(FILE *fp, const rogue_shader *shader);

void rogue_print_block_uses(FILE *fp, const rogue_shader *shader);

void rogue_print_drc_trxns(FILE *fp, const rogue_shader *shader);

/* Validation */
bool rogue_validate_shader(rogue_shader *shader, const char *when);

/* Debug. */
enum rogue_debug {
   ROGUE_DEBUG_NIR = BITFIELD_BIT(0),
   ROGUE_DEBUG_NIR_PASSES = BITFIELD_BIT(1),
   ROGUE_DEBUG_IR = BITFIELD_BIT(2),
   ROGUE_DEBUG_IR_PASSES = BITFIELD_BIT(3),
   ROGUE_DEBUG_IR_DETAILS = BITFIELD_BIT(4),
   ROGUE_DEBUG_VLD_SKIP = BITFIELD_BIT(5),
   ROGUE_DEBUG_VLD_NONFATAL = BITFIELD_BIT(6),
   ROGUE_DEBUG_REGALLOC = BITFIELD_BIT(7),
   ROGUE_DEBUG_BURST_LOADS = BITFIELD_BIT(8),
   ROGUE_DEBUG_SKIP_CF_OPTS = BITFIELD_BIT(9),
   ROGUE_DEBUG_DUMP_BINARY = BITFIELD_BIT(10),
   ROGUE_DEBUG_ATOMIC_EMU = BITFIELD_BIT(11),
};

extern unsigned long rogue_debug;

#define ROGUE_DEBUG(flag) unlikely(!!(rogue_debug & (ROGUE_DEBUG_##flag)))

extern bool rogue_color;

void rogue_debug_init(void);

static inline void
rogue_print_pass_debug(rogue_shader *shader, const char *pass, FILE *fp)
{
   fprintf(fp, "%s\n", pass);
   rogue_print_shader(fp, shader);
   if (ROGUE_DEBUG(IR_DETAILS)) {
      rogue_print_reg_writes(fp, shader);
      rogue_print_reg_uses(fp, shader);
      rogue_print_block_uses(fp, shader);
      rogue_print_drc_trxns(fp, shader);
   }
}

/* Passes */
#define ROGUE_PASS(progress, shader, pass, ...)            \
   do {                                                    \
      if (pass((shader), ##__VA_ARGS__)) {                 \
         if (ROGUE_DEBUG(IR_PASSES))                       \
            rogue_print_pass_debug(shader, #pass, stdout); \
         rogue_validate_shader(shader, #pass);             \
         progress = true;                                  \
      }                                                    \
   } while (0)

#define ROGUE_PASS_V(shader, pass, ...)                    \
   do {                                                    \
      if (pass((shader), ##__VA_ARGS__)) {                 \
         if (ROGUE_DEBUG(IR_PASSES))                       \
            rogue_print_pass_debug(shader, #pass, stdout); \
         rogue_validate_shader(shader, #pass);             \
      }                                                    \
   } while (0)

bool rogue_constreg(rogue_shader *shader);

bool rogue_copy_prop(rogue_shader *shader);

bool rogue_dce(rogue_shader *shader);

bool rogue_lower_late_ops(rogue_shader *shader);

bool rogue_lower_pseudo_ops(rogue_shader *shader);

bool rogue_regalloc(rogue_shader *shader);

bool rogue_schedule_instr_groups(rogue_shader *shader, bool multi_instr_groups);

bool rogue_schedule_uvsw(rogue_shader *shader, bool latency_hiding);

bool rogue_schedule_wdf(rogue_shader *shader, bool latency_hiding);

bool rogue_schedule_st_regs(rogue_shader *shader);

bool rogue_trim(rogue_shader *shader);

void rogue_shader_passes(rogue_shader *shader);

struct pvr_device_info;

/**
 * \brief Compiler context.
 */
typedef struct rogue_compiler {
   const struct pvr_device_info *dev_info;
} rogue_compiler;

rogue_compiler *rogue_compiler_create(const struct pvr_device_info *dev_info);

static inline gl_shader_stage
pvr_stage_to_mesa(enum pvr_stage_allocation pvr_stage)
{
   switch (pvr_stage) {
   case PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY:
      return MESA_SHADER_VERTEX;

   case PVR_STAGE_ALLOCATION_FRAGMENT:
      return MESA_SHADER_FRAGMENT;

   case PVR_STAGE_ALLOCATION_COMPUTE:
      return MESA_SHADER_COMPUTE;

   default:
      break;
   }

   unreachable("Unsupported pvr_stage_allocation.");
}

static inline enum pvr_stage_allocation
mesa_stage_to_pvr(gl_shader_stage mesa_stage)
{
   switch (mesa_stage) {
   case MESA_SHADER_VERTEX:
      return PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY;

   case MESA_SHADER_FRAGMENT:
      return PVR_STAGE_ALLOCATION_FRAGMENT;

   case MESA_SHADER_COMPUTE:
      return PVR_STAGE_ALLOCATION_COMPUTE;

   default:
      break;
   }

   unreachable("Unsupported gl_shader_stage.");
}

/* Max number of I/O varying variables.
 * Fragment shader: VARYING_SLOT_POS + VARYING_SLOT_PNTC.
 * Vertex shader: VARYING_SLOT_POS.
 */
#define ROGUE_MAX_SYSVAL_VARYINGS (2)

/* Max number of I/O varying variables.
 * Fragment shader: Sysval varyings + MAX_VARYING.
 * Vertex shader: Position + MAX_VARYING.
 */
#define ROGUE_MAX_IO_VARYING_VARS (MAX_VARYING + ROGUE_MAX_SYSVAL_VARYINGS)

/* VERT_ATTRIB_GENERIC0-15 */
#define ROGUE_MAX_IO_ATTRIB_VARS 16

/* Max buffers entries that can be used. */
/* TODO: Currently UBOs are the only supported buffers. */
#define ROGUE_MAX_BUFFERS 24

static inline unsigned rogue_from_gl_varying_loc(gl_varying_slot gl_loc)
{
   switch (gl_loc) {
   case VARYING_SLOT_POS:
      return 0;

   case VARYING_SLOT_PNTC:
      return 1;

   default:
      assert(gl_loc >= VARYING_SLOT_VAR0 && gl_loc <= VARYING_SLOT_VAR31);
      return (gl_loc - VARYING_SLOT_VAR0) + ROGUE_MAX_SYSVAL_VARYINGS;
   }

   unreachable("Unsupported gl_varying_slot.");
}

static inline gl_varying_slot gl_from_rogue_varying_loc(unsigned rogue_loc)
{
   switch (rogue_loc) {
   case 0:
      return VARYING_SLOT_POS;

   case 1:
      return VARYING_SLOT_PNTC;

   default:
      assert(rogue_loc >= ROGUE_MAX_SYSVAL_VARYINGS &&
             rogue_loc <= ROGUE_MAX_IO_VARYING_VARS);
      return (rogue_loc - ROGUE_MAX_SYSVAL_VARYINGS) + VARYING_SLOT_VAR0;
   }

   unreachable("Invalid rogue_loc.");
}

/**
 * \brief UBO data.
 */
typedef struct rogue_ubo_data {
   unsigned num_ubo_entries;
   unsigned desc_set[ROGUE_MAX_BUFFERS];
   unsigned binding[ROGUE_MAX_BUFFERS];
   unsigned dest[ROGUE_MAX_BUFFERS];
   unsigned size[ROGUE_MAX_BUFFERS];
} rogue_ubo_data;

/**
 * \brief Compile time constants that need uploading.
 */
typedef struct rogue_compile_time_consts_data {
   /* TODO: Output these from the compiler. */
   /* TODO: Add the other types. */
   struct {
      unsigned num;
      unsigned dest;
      /* TODO: This should probably be bigger. Big enough to account for all
       * available writable special constant regs.
       */
      uint32_t value[ROGUE_MAX_BUFFERS];
   } static_consts;
} rogue_compile_time_consts_data;

/**
 * \brief Per-stage common build data.
 */
typedef struct rogue_common_build_data {
   unsigned temps;
   unsigned internals;
   unsigned coeffs;
   unsigned shareds;

   rogue_ubo_data ubo_data;
   rogue_compile_time_consts_data compile_time_consts_data;
} rogue_common_build_data;

/**
 * \brief Arguments for the FPU iterator(s)
 * (produces varyings for the fragment shader).
 */
typedef struct rogue_iterator_args {
   bool iterates_depth;
   bool triangle_fan;
   uint32_t num_coeff_varyings;
   uint32_t num_fpu_iterators;
   uint32_t fpu_iterators[ROGUE_MAX_IO_VARYING_VARS * 4];
   uint32_t destination[ROGUE_MAX_IO_VARYING_VARS * 4];
   unsigned coeff_indices[ROGUE_MAX_IO_VARYING_VARS][4];
   enum glsl_interp_mode interp_modes[ROGUE_MAX_IO_VARYING_VARS][4];
   struct reverse_map {
      unsigned location : 6;
      unsigned component : 2;
   } coeff_to_location[ROGUE_MAX_IO_VARYING_VARS * 4];
} rogue_iterator_args;

/**
 * \brief Special vertex input variables.
 * Each special variable gets allocated its own vtxin reg if used.
 */
typedef struct rogue_vertex_special_vars {
   /* VertexIndex built-in. */
   unsigned vertex_id_offset;
   /* InstanceIndex built-in. */
   unsigned instance_id_offset;
} rogue_vertex_special_vars;

/**
 * \brief Vertex input register allocations.
 */
typedef struct rogue_vertex_inputs {
   unsigned num_input_vars;
   unsigned base[ROGUE_MAX_IO_ATTRIB_VARS];
   unsigned components[ROGUE_MAX_IO_ATTRIB_VARS];
} rogue_vertex_inputs;

/**
 * \brief Vertex output allocations.
 */
typedef struct rogue_vertex_outputs {
   unsigned num_output_vars;
   unsigned point_size_index;
   unsigned viewport_index;
   unsigned layer_index;
   unsigned clip_index[2][4];
   unsigned cull_index[2][4];
   unsigned indices[ROGUE_MAX_IO_VARYING_VARS][4];
   bool is_f16[ROGUE_MAX_IO_VARYING_VARS][4];
} rogue_vertex_outputs;

enum rogue_msaa_mode {
   ROGUE_MSAA_MODE_UNDEF = 0, /* explicitly treat 0 as undefined */
   /* One task for all samples. */
   ROGUE_MSAA_MODE_PIXEL,
   /* For on-edge pixels only: separate tasks for each sample. */
   ROGUE_MSAA_MODE_SELECTIVE,
   /* For all pixels: separate tasks for each sample. */
   ROGUE_MSAA_MODE_FULL,
};

/**
 * \brief Stage-specific build data.
 */
typedef struct rogue_build_data {
   struct rogue_fs_build_data {
      rogue_iterator_args iterator_args;
      enum rogue_msaa_mode msaa_mode;
      bool phas; /* Indicates the presence of PHAS instruction. */
      bool discard;
      bool side_effects;
      bool translucent;

      const struct vk_color_blend_state *cb_state;

      /* If >=0 then copy the depth into this pixel output for all fragment
       * programs in the subpass.
       */
      int32_t z_replicate;

      unsigned num_outputs;
      struct {
         enum pipe_format format;
         enum pvr_pbe_accum_format accum_format;
         const struct usc_mrt_resource *mrt_resource;
      } * outputs;

      unsigned num_inputs;
      struct {
         enum pvr_renderpass_hwsetup_input_access type;
         uint32_t on_chip_rt;
      } * inputs;

      struct {
         bool barrier;
         bool atomic_ops;
      } has;
   } fs;
   struct rogue_vs_build_data {
      /* TODO: Should these be removed since the driver allocates the vertex
       * inputs?
       */
      rogue_vertex_special_vars special_vars;
      rogue_vertex_inputs inputs;
      unsigned num_vertex_input_regs; /* Final number of inputs. */

      rogue_vertex_outputs outputs;
      unsigned num_vertex_outputs; /* Final number of outputs. */

      unsigned num_f32_npc_varyings; /* Number of f32 NPC varyings. */
      unsigned num_f32_flat_varyings; /* Number of f32 flat varyings. */
      unsigned num_f32_linear_varyings; /* Number of f32 linear varyings. */

      unsigned num_f16_npc_varyings; /* Number of f16 NPC varyings. */
      unsigned num_f16_flat_varyings; /* Number of f16 flat varyings. */
      unsigned num_f16_linear_varyings; /* Number of f16 linear varyings. */

      struct {
         bool barrier;
         bool atomic_ops;
      } has;
   } vs;
   struct rogue_cs_build_data {
      uint32_t local_id_regs[2];
      uint32_t workgroup_regs[3];
      uint32_t barrier_reg;

      struct {
         /* If the shader uses gl_LocalInvocationID. */
         bool location_id_x;
         bool location_id_y_or_z;

         /* If the shader uses gl_WorkGroupID. */
         bool work_group_id_x;
         bool work_group_id_y;
         bool work_group_id_z;

         /* If the shader uses gl_NumWorkGroups. */
         bool num_work_groups;
         bool barrier;
         bool atomic_ops;
      } has;

      /* local_size_x * local_size_y * local_size_z from the glsl
       * layout(local_size_x = X, local_size_y = Y, local_size_z = Z).
       */
      uint32_t work_size;
   } cs;
} rogue_build_data;

/**
 * \brief Shared multi-stage build context.
 */
typedef struct rogue_build_ctx {
   rogue_compiler *compiler;

   /* Shaders in various stages of compilations. */
   nir_shader *nir[MESA_SHADER_COMPUTE + 1];
   rogue_shader *rogue[MESA_SHADER_COMPUTE + 1];
   struct util_dynarray binary[MESA_SHADER_COMPUTE + 1];

   rogue_common_build_data common_data[MESA_SHADER_COMPUTE + 1];
   rogue_build_data stage_data;
   struct pvr_pipeline_layout *pipeline_layout;
   unsigned next_ssa_idx;
} rogue_build_ctx;

/**
 * \brief Components within vec4-aligned coefficient registers.
 * The interpolation will be A*x + B*y + C
 */
enum rogue_coeff_component {
   ROGUE_COEFF_COMPONENT_A = 0,
   ROGUE_COEFF_COMPONENT_B,
   ROGUE_COEFF_COMPONENT_C,
   ROGUE_COEFF_COMPONENT_INVALID,
};

rogue_build_ctx *
rogue_build_context_create(rogue_compiler *compiler,
                           struct pvr_pipeline_layout *pipeline_layout);

void rogue_collect_io_data(rogue_build_ctx *ctx, nir_shader *nir);

unsigned rogue_count_used_regs(const rogue_shader *shader,
                               enum rogue_reg_class class);

unsigned rogue_coeff_index_fs(rogue_iterator_args *args,
                              gl_varying_slot location,
                              unsigned component);

enum glsl_interp_mode rogue_interp_mode_fs(struct rogue_iterator_args *args,
                                           gl_varying_slot location,
                                           unsigned component);

unsigned rogue_output_index_vs(rogue_vertex_outputs *outputs,
                               gl_varying_slot location,
                               unsigned component);

unsigned rogue_ubo_reg(rogue_ubo_data *ubo_data,
                       unsigned desc_set,
                       unsigned binding,
                       unsigned offset_bytes);

const nir_shader_compiler_options *rogue_nir_options(void);

nir_shader *rogue_spirv_to_nir(rogue_build_ctx *ctx,
                               gl_shader_stage stage,
                               const char *entry,
                               unsigned spirv_size,
                               const uint32_t *spirv_data,
                               unsigned num_spec,
                               struct nir_spirv_specialization *spec);

rogue_shader *rogue_nir_compile(rogue_build_ctx *ctx, nir_shader *nir);

/* Custom NIR passes. */
bool rogue_nir_algebraic_late(nir_shader *shader);

bool rogue_nir_compute_instance_check(nir_shader *shader);

bool rogue_nir_expand_swizzles_to_vec(nir_shader *shader);

bool rogue_nir_pfo(nir_shader *shader, rogue_build_ctx *ctx);

bool rogue_nir_lower_alu_conversion_to_intrinsic(nir_shader *shader);

bool rogue_nir_lower_atomics(nir_shader *shader,
                             unsigned atomic_op_mask,
                             nir_variable_mode modes);

bool rogue_nir_lower_fquantize2f16(nir_shader *shader);

bool rogue_nir_lower_input_attachments(nir_shader *shader,
                                       rogue_build_ctx *ctx);

bool rogue_nir_lower_tex(nir_shader *shader, rogue_build_ctx *ctx);

bool rogue_nir_lower_io(nir_shader *shader, rogue_build_ctx *ctx, bool late);

rogue_shader *rogue_nir_to_rogue(rogue_build_ctx *ctx, const nir_shader *nir);

/* Encode/decode */

void rogue_encode_shader(rogue_build_ctx *ctx,
                         rogue_shader *shader,
                         struct util_dynarray *binary);

#endif /* ROGUE_H */
