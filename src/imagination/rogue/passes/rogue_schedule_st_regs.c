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

#include "rogue.h"
#include "rogue_builder.h"
#include "util/macros.h"

#include <stdbool.h>

/**
 * \file rogue_schedule_st_regs.c
 *
 * \brief Contains the rogue_schedule_st_regs pass.
 */

static inline bool schedule_st_regs(rogue_builder *b, rogue_backend_instr *st)
{
   rogue_ref *data_ref = &st->src[0].ref;
   rogue_ref *addr_ref = &st->src[4].ref;

   enum rogue_reg_class data_class;
   unsigned data_index;
   unsigned data_size;
   if (!rogue_ref_reg_regarray_info(data_ref,
                                    &data_class,
                                    &data_index,
                                    &data_size)) {
      unreachable("Invalid type for ST data.");
      return false;
   }

   if (data_class != ROGUE_REG_CLASS_SSA &&
       data_class != ROGUE_REG_CLASS_TEMP &&
       data_class != ROGUE_REG_CLASS_VTXIN)
      return false;

   enum rogue_reg_class addr_class;
   unsigned addr_index;
   unsigned addr_size;
   if (!rogue_ref_reg_regarray_info(addr_ref,
                                    &addr_class,
                                    &addr_index,
                                    &addr_size)) {
      unreachable("Invalid type for ST addr.");
      return false;
   }

   assert(rogue_ref_is_regarray(addr_ref));
   assert(addr_size == 2);

   /* If data already follows the address, then we're already good to go. */
   if (data_class == addr_class && data_index == (addr_index + 2))
      return false;

   /* If the data/address are already subarrays, then we're in trouble. */
   bool data_ref_is_regarray = rogue_ref_is_regarray(data_ref);
   assert(!data_ref_is_regarray || !data_ref->regarray->parent);
   assert(!addr_ref->regarray->parent);

   /* Create new contiguous regarray containing the address followed by data. */
   assert(b->shader->ctx);
   unsigned addr_data_idx = b->shader->ctx->next_ssa_idx++;
   rogue_ssa_vec_regarray(b->shader, addr_size + data_size, addr_data_idx, 0);
   rogue_regarray *addr =
      rogue_ssa_vec_regarray(b->shader, addr_size, addr_data_idx, 0);
   rogue_regarray *data =
      rogue_ssa_vec_regarray(b->shader, data_size, addr_data_idx, addr_size);

   b->cursor = rogue_cursor_before_instr(&st->instr);

   /* TODO: bulk contiguous mov support. */
   /* Copy the address & data. */
   unsigned addr_base_index = rogue_index_from_regarray(addr_ref->regarray);
   for (unsigned u = 0; u < addr_size; ++u) {
      rogue_regarray *dst =
         rogue_ssa_vec_regarray(b->shader, 1, addr_data_idx, u);
      rogue_regarray *src =
         rogue_ssa_vec_regarray(b->shader, 1, addr_base_index, u);
      rogue_MOV(b, rogue_ref_regarray(dst), rogue_ref_regarray(src));
   }

   if (!data_ref_is_regarray) {
      rogue_regarray *dst =
         rogue_ssa_vec_regarray(b->shader, 1, addr_data_idx, addr_size);
      rogue_MOV(b, rogue_ref_regarray(dst), *data_ref);
   } else {
      unsigned data_base_index = rogue_index_from_regarray(data_ref->regarray);
      for (unsigned u = 0; u < data_size; ++u) {
         rogue_regarray *dst =
            rogue_ssa_vec_regarray(b->shader, 1, addr_data_idx, addr_size + u);
         rogue_regarray *src =
            rogue_ssa_vec_regarray(b->shader, 1, data_base_index, u);
         rogue_MOV(b, rogue_ref_regarray(dst), rogue_ref_regarray(src));
      }
   }

   /* Replace the addr/data sources in the ST. */
   ASSERTED bool replaced;
   replaced = rogue_src_regarray_replace(&st->src_use[4].regarray, addr);
   assert(replaced);

   if (!data_ref_is_regarray)
      replaced = rogue_src_reg_replace_regarray(&st->src_use[0].reg, data);
   else
      replaced = rogue_src_regarray_replace(&st->src_use[0].regarray, data);

   assert(replaced);

   return true;
}

/* Schedules store instruction data/address registers to be contiguous. */
PUBLIC
bool rogue_schedule_st_regs(rogue_shader *shader)
{
   if (shader->is_grouped)
      return false;

   rogue_builder b;
   rogue_builder_init(&b, shader);

   bool progress = false;
   rogue_foreach_instr_in_shader (instr, shader) {
      if (instr->type != ROGUE_INSTR_TYPE_BACKEND)
         continue;

      rogue_backend_instr *backend = rogue_instr_as_backend(instr);
      if (backend->op != ROGUE_BACKEND_OP_ST)
         continue;

      progress |= schedule_st_regs(&b, backend);
   }

   return progress;
}
