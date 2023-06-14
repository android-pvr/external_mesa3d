/*
 * Copyright © 2023 Imagination Technologies Ltd.
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

#ifndef ROGUE_OP_HELPERS_H
#define ROGUE_OP_HELPERS_H

/**
 * \file rogue_op_helpers.h
 *
 * \brief Contains helper functions for building non-trivial Rogue IR ops.
 */

#include "rogue.h"
#include "rogue_builder.h"

static inline enum rogue_alu_op_mod cmp_func_to_rogue(enum compare_func func,
                                                      bool cmp_zero)
{
   if (!cmp_zero) {
      switch (func) {
      case COMPARE_FUNC_LESS:
         return ROGUE_ALU_OP_MOD_L;

      case COMPARE_FUNC_EQUAL:
         return ROGUE_ALU_OP_MOD_E;

      case COMPARE_FUNC_LEQUAL:
         return ROGUE_ALU_OP_MOD_LE;

      case COMPARE_FUNC_GREATER:
         return ROGUE_ALU_OP_MOD_G;

      case COMPARE_FUNC_NOTEQUAL:
         return ROGUE_ALU_OP_MOD_NE;

      case COMPARE_FUNC_GEQUAL:
         return ROGUE_ALU_OP_MOD_GE;

      default:
         break;
      }
   } else {
      switch (func) {
      case COMPARE_FUNC_EQUAL:
         return ROGUE_ALU_OP_MOD_Z;

      case COMPARE_FUNC_GREATER:
         return ROGUE_ALU_OP_MOD_GZ;

      case COMPARE_FUNC_GEQUAL:
         return ROGUE_ALU_OP_MOD_GEZ;

      default:
         break;
      }
   }

   unreachable("Unsupported compare_func.");
}

static inline enum rogue_alu_op_mod nir_type_to_rogue_type(nir_alu_type type)
{
   switch (type) {
   case nir_type_int8:
      return ROGUE_ALU_OP_MOD_S8;

   case nir_type_int16:
      return ROGUE_ALU_OP_MOD_S16;

   case nir_type_int32:
      return ROGUE_ALU_OP_MOD_S32;

   case nir_type_uint8:
      return ROGUE_ALU_OP_MOD_U8;

   case nir_type_uint16:
      return ROGUE_ALU_OP_MOD_U16;

   case nir_type_uint32:
      return ROGUE_ALU_OP_MOD_U32;

   case nir_type_float32:
      return ROGUE_ALU_OP_MOD_F32;

   default:
      break;
   }

   unreachable("Unsupported nir_alu_type.");
}

static rogue_backend_instr *rogue_load_global(rogue_builder *b,
                                              rogue_ref *dst,
                                              rogue_ref *src_addr,
                                              unsigned bit_size,
                                              unsigned load_components,
                                              bool constant)
{
   /* Burst loads only for 32-bit values. */
   assert((bit_size == 32 && load_components <= 16) ||
          (bit_size != 32 && load_components == 1));

   unsigned burst_len;
   switch (bit_size) {
   case 8:
   case 16:
      burst_len = 1;
      break;

   case 32:
      burst_len = load_components;
      break;

   /* 64-bit loads are just 2x32-bit loads. */
   case 64:
      burst_len = load_components * 2;
      break;

   default:
      unreachable("Unsupported bit size.");
   }

   /* If we're doing 8/16 bit loads, we need to mask out the rest of the loaded
    * data that we don't want/need.
    */
   /* TODO: macro for getting next free ssa reg. */
   rogue_ref ld_dst = *dst;
   if (bit_size < 32)
      ld_dst = rogue_ref_reg(
         rogue_ssa_reg(b->shader, b->shader->ctx->next_ssa_idx++));

   /* TODO: cache flags */
   rogue_backend_instr *ld =
      rogue_LD(b, ld_dst, rogue_ref_drc(0), rogue_ref_val(burst_len), *src_addr);

   /* Mask out the data. */
   if (bit_size < 32) {
      rogue_bitwise_instr *iand =
         rogue_IAND(b, *dst, ld_dst, rogue_ref_imm(BITFIELD_MASK(bit_size)));
      rogue_add_instr_commentf(&iand->instr, "load_mask_%u", bit_size);
   }

   return ld;
}

static rogue_backend_instr *rogue_store_global(rogue_builder *b,
                                               rogue_ref *dst_addr,
                                               rogue_ref *src,
                                               unsigned bit_size,
                                               unsigned store_components)
{
   /* Burst stores only for 32-bit values. */
   assert((bit_size == 32 && store_components <= 16) ||
          (bit_size != 32 && store_components == 1));

   unsigned burst_len;
   unsigned data_size;
   switch (bit_size) {
   case 8:
      burst_len = 1;
      data_size = 0;
      break;

   case 16:
      burst_len = 1;
      data_size = 1;
      break;

   case 32:
      burst_len = store_components;
      data_size = 2;
      break;

   /* 64-bit stores are just 2x32-bit stores. */
   case 64:
      burst_len = store_components * 2;
      data_size = 2;
      break;

   default:
      unreachable("Unsupported bit size.");
   }

   /* TODO: cache flags */
   return rogue_ST(b,
                   *src,
                   rogue_ref_val(data_size),
                   rogue_ref_drc(0),
                   rogue_ref_val(burst_len),
                   *dst_addr,
                   rogue_none());
}

/* Conditionally sets the output to 1 or 0 depending on whether the comparison
 * is true or false. */
static rogue_alu_instr *rogue_cmp(rogue_builder *b,
                                  rogue_ref *dst,
                                  rogue_ref *src0,
                                  rogue_ref *src1,
                                  enum compare_func func,
                                  nir_alu_type type)
{
   rogue_alu_instr *cmp = rogue_CMP(b, *dst, *src0, *src1);
   rogue_set_alu_op_mod(cmp, cmp_func_to_rogue(func, false));
   rogue_set_alu_op_mod(cmp, nir_type_to_rogue_type(type));

   if (nir_alu_type_get_type_size(type) < 32) {
      rogue_set_alu_src_mod(cmp, 0, ROGUE_ALU_SRC_MOD_E0);
      rogue_set_alu_src_mod(cmp, 1, ROGUE_ALU_SRC_MOD_E0);
   }

   return cmp;
}

static rogue_alu_instr *rogue_csel(rogue_builder *b,
                                   rogue_ref *dst,
                                   rogue_ref *src_cmp,
                                   rogue_ref *src_true,
                                   rogue_ref *src_false,
                                   enum compare_func func,
                                   nir_alu_type type)
{
   rogue_alu_instr *csel = rogue_CSEL(b, *dst, *src_cmp, *src_true, *src_false);
   rogue_set_alu_op_mod(csel, cmp_func_to_rogue(func, true));
   rogue_set_alu_op_mod(csel, nir_type_to_rogue_type(type));

   if (nir_alu_type_get_type_size(type) < 32) {
      rogue_set_alu_src_mod(csel, 0, ROGUE_ALU_SRC_MOD_E0);
      rogue_set_alu_src_mod(csel, 1, ROGUE_ALU_SRC_MOD_E0);
   }

   return csel;
}

#endif /* ROGUE_OP_HELPERS_H */
