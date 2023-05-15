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

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "rogue.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * \file rogue_nir_expand_swizzles_to_vec.c
 *
 * \brief Contains the rogue_nir_expand_swizzles_to_vec pass.
 */

static bool is_alu_src_swizzled(const nir_instr *instr,
                                UNUSED const void *_data)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (!alu->dest.dest.is_ssa)
      return false;

   for (unsigned src = 0; src < nir_op_infos[alu->op].num_inputs; ++src) {
      unsigned src_components = nir_src_num_components(alu->src[src].src);
      unsigned components_required = nir_ssa_alu_instr_src_components(alu, src);

      if (components_required > 1 && src_components < components_required)
         return true;
   }

   return false;
}

static nir_ssa_def *
expand_swizzle_to_vec(nir_builder *b, nir_instr *instr, UNUSED void *_data)
{
   b->cursor = nir_before_instr(instr);
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   for (unsigned src = 0; src < nir_op_infos[alu->op].num_inputs; ++src) {
      unsigned components_required = nir_ssa_alu_instr_src_components(alu, src);

      if (components_required == 1)
         continue;

      nir_ssa_def *components[NIR_MAX_VEC_COMPONENTS];

      unsigned c = 0;
      for (unsigned chan = 0; chan < NIR_MAX_VEC_COMPONENTS; ++chan) {
         if (!nir_alu_instr_channel_used(alu, src, chan))
            continue;

         components[c++] =
            nir_channel(b, alu->src[src].src.ssa, alu->src[src].swizzle[chan]);
      }
      assert(c == components_required);

      nir_ssa_def *unswizzled_src = nir_vec(b, components, components_required);

      nir_instr_rewrite_src_ssa(instr, &alu->src[src].src, unswizzled_src);
   }

   return NULL;
}

PUBLIC
bool rogue_nir_expand_swizzles_to_vec(nir_shader *shader)
{
   return nir_shader_lower_instructions(shader,
                                        is_alu_src_swizzled,
                                        expand_swizzle_to_vec,
                                        NULL);
}
