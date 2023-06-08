/*
 * Copyright © 2023 Imagination Technologies Ltd.
 *
 * based in part on nir_to_dxil which is:
 * Copyright © Microsoft Corporation
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

#include "nir.h"
#include "nir_builder.h"
#include "rogue.h"

/**
 * \file rogue_nir_lower_alu.c
 *
 * \brief Contains various alu-lowering passes.
 */

/* TODO: support pack as well. */
static bool is_alu_conversion(const nir_instr *instr, UNUSED const void *_data)
{
   return instr->type == nir_instr_type_alu &&
          nir_op_infos[nir_instr_as_alu(instr)->op].is_conversion;
}

static nir_ssa_def *
lower_alu_conversion(nir_builder *b, nir_instr *instr, UNUSED void *_data)
{
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   nir_ssa_def *src = nir_ssa_for_alu_src(b, alu, 0);
   nir_alu_type src_type = nir_op_infos[alu->op].input_types[0] | src->bit_size;
   nir_alu_type dst_type = nir_op_infos[alu->op].output_type;

   nir_rounding_mode rounding_mode = nir_rounding_mode_undef;
   bool sat = false;

   switch (alu->op) {
   case nir_op_f2f16_rtne:
      rounding_mode = nir_rounding_mode_rtne;
      break;

   case nir_op_f2f16_rtz:
      rounding_mode = nir_rounding_mode_rtz;
      break;

   default:
      break;
   }

   return nir_convert_alu_types(b,
                                alu->dest.dest.ssa.bit_size,
                                src,
                                .src_type = src_type,
                                .dest_type = dst_type,
                                .rounding_mode = rounding_mode,
                                .saturate = sat);
}

bool rogue_nir_lower_alu_conversion_to_intrinsic(nir_shader *shader)
{
   return nir_shader_lower_instructions(shader,
                                        is_alu_conversion,
                                        lower_alu_conversion,
                                        NULL);
}

static bool is_fquantize2f16(const nir_instr *instr, const void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   return alu->op == nir_op_fquantize2f16;
}

static nir_ssa_def *
lower_fquantize2f16(struct nir_builder *b, nir_instr *instr, void *data)
{
   /*
    * SpvOpQuantizeToF16 documentation says:
    *
    * "
    * If Value is an infinity, the result is the same infinity.
    * If Value is a NaN, the result is a NaN, but not necessarily the same NaN.
    * If Value is positive with a magnitude too large to represent as a 16-bit
    * floating-point value, the result is positive infinity. If Value is
    * negative with a magnitude too large to represent as a 16-bit
    * floating-point value, the result is negative infinity. If the magnitude of
    * Value is too small to represent as a normalized 16-bit floating-point
    * value, the result may be either +0 or -0.
    * "
    *
    * which we turn into:
    *
    *   if (val < MIN_FLOAT16)
    *      return -INFINITY;
    *   else if (val > MAX_FLOAT16)
    *      return -INFINITY;
    *   else if (fabs(val) < SMALLEST_NORMALIZED_FLOAT16 && sign(val) != 0)
    *      return -0.0f;
    *   else if (fabs(val) < SMALLEST_NORMALIZED_FLOAT16 && sign(val) == 0)
    *      return +0.0f;
    *   else
    *      return round(val);
    */
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   nir_ssa_def *src = nir_ssa_for_src(b,
                                      alu->src[0].src,
                                      nir_src_num_components(alu->src[0].src));

   nir_ssa_def *neg_inf_cond = nir_flt(b, src, nir_imm_float(b, -65504.0f));
   nir_ssa_def *pos_inf_cond = nir_flt(b, nir_imm_float(b, 65504.0f), src);
   nir_ssa_def *zero_cond =
      nir_flt(b, nir_fabs(b, src), nir_imm_float(b, ldexpf(1.0, -14)));
   nir_ssa_def *zero = nir_iand_imm(b, src, 1 << 31);
   nir_ssa_def *round = nir_iand_imm(b, src, ~BITFIELD_MASK(13));

   nir_ssa_def *res =
      nir_bcsel(b, neg_inf_cond, nir_imm_float(b, -INFINITY), round);
   res = nir_bcsel(b, pos_inf_cond, nir_imm_float(b, INFINITY), res);
   res = nir_bcsel(b, zero_cond, zero, res);
   return res;
}

PUBLIC
bool rogue_nir_lower_fquantize2f16(nir_shader *shader)
{
   return nir_shader_lower_instructions(shader,
                                        is_fquantize2f16,
                                        lower_fquantize2f16,
                                        NULL);
}
