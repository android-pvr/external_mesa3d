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

#include "rogue.h"
#include "rogue_builder.h"
#include "util/macros.h"

#include <stdbool.h>

/**
 * \file rogue_constreg.c
 *
 * \brief Contains the rogue_constreg pass.
 */

static inline rogue_reg *rogue_imm_alloc_reg(rogue_shader *shader,
                                             rogue_imm_t imm)
{
   rogue_reg **imm_alloc_reg =
      util_sparse_array_get(&shader->imm_allocs, imm.u32);

   if (!*imm_alloc_reg) {
      rogue_builder b;
      rogue_builder_init(&b, shader);

      rogue_block *first_block =
         list_first_entry(&shader->blocks, rogue_block, link);
      rogue_instr *first_instr =
         list_first_entry(&first_block->instrs, rogue_instr, link);
      b.cursor = rogue_cursor_before_instr(first_instr);

      unsigned imm_alloc_idx = shader->ctx->next_ssa_idx++;
      *imm_alloc_reg = rogue_ssa_reg(shader, imm_alloc_idx);
      rogue_MOVI(&b, rogue_ref_reg(*imm_alloc_reg), rogue_ref_imm(imm.u32));
   }

   return *imm_alloc_reg;
}

static inline bool rogue_instr_is_movi(rogue_instr *instr)
{
   if (instr->type == ROGUE_INSTR_TYPE_BITWISE)
      return rogue_instr_as_bitwise(instr)->op == ROGUE_BITWISE_OP_MOVI;

   /* ALU MOV can also be lowered to a movi. */
   if (instr->type == ROGUE_INSTR_TYPE_ALU)
      return rogue_instr_as_alu(instr)->op == ROGUE_ALU_OP_MOV;

   return false;
}

/* Converts immediate values to constant register values. */
PUBLIC
bool rogue_constreg(rogue_shader *shader)
{
   if (shader->is_grouped)
      return false;

   bool progress = false;

   rogue_foreach_imm_use_safe (imm_use, shader) {
      unsigned index = rogue_constreg_lookup(*imm_use->imm);

      rogue_reg *reg;

      /* Replace values that aren't in the special constant registers
       * with immediate movs.
       */
      if (index == ROGUE_NO_CONST_REG) {
         if (rogue_instr_is_movi(imm_use->instr))
            continue;

         reg = rogue_imm_alloc_reg(shader, *imm_use->imm);
      } else {
         reg = rogue_const_reg(shader, index);
      }

      struct list_head imm_use_link = imm_use->link;
      if (rogue_src_imm_replace(imm_use, reg)) {
         list_del(&imm_use_link);
         progress |= true;
      }
   }

   return progress;
}
