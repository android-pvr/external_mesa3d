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
 * \file rogue_nir_lower_syncops.c
 *
 * \brief Contains passes that lower synchronisation ops.
 */

struct options {
   unsigned atomic_op_mask;
   nir_variable_mode modes;
};

/*
for (unsigned u = 0; u < instances_in_slot; ++u)
   if (u == instance_num)
      <per_instance_body>
*/
static nir_ssa_def *rogue_per_instance_loop(nir_builder *b,
                                            bool locked,
                                            unsigned result_num_components,
                                            unsigned result_bit_size)
{
   nir_shader *shader = b->shader;

   /* for (unsigned loop_iter = loop_init_val;
    *      loop_iter < loop_cond_val;
    *      loop_iter += loop_incr_val)
    */

   /* Setup the loop. */
   nir_ssa_def *loop_init_val = nir_imm_int(b, 0);
   nir_ssa_def *loop_cond_val = nir_imm_int(b, ROGUE_MAX_INSTANCES_PER_TASK);
   nir_ssa_def *loop_incr_val = nir_imm_int(b, 1);
   nir_ssa_def *op_result = NULL;

   if (locked)
      nir_mutex_img(b,
                    .mutex_id_img = ROGUE_MUTEX_ID_ATOMIC_EMU,
                    .mutex_op_img = ROGUE_MUTEX_OP_LOCK);

   /* Create phi for loop iterator. */
   nir_phi_instr *phi_iter = nir_phi_instr_create(shader);
   nir_ssa_dest_init(&phi_iter->instr,
                     &phi_iter->dest,
                     loop_init_val->num_components,
                     loop_init_val->bit_size);
   nir_ssa_def *loop_iter = &phi_iter->dest.ssa;

   /**/

   /* Create loop, don't allow it to be unrolled. */
   nir_loop *per_inst_loop = nir_push_loop(b);
   per_inst_loop->control = nir_loop_control_dont_unroll;
   {
      nir_phi_instr_add_src(phi_iter,
                            loop_init_val->parent_instr->block,
                            nir_src_for_ssa(loop_init_val));

      /* Loop test. */
      nir_ssa_def *loop_cond = nir_uge(b, loop_iter, loop_cond_val);
      nir_if *loop_break_if = nir_push_if(b, loop_cond);
      loop_break_if->control = nir_selection_control_dont_flatten;
      {
         nir_jump_instr *loop_break =
            nir_jump_instr_create(shader, nir_jump_break);
         nir_builder_instr_insert(b, &loop_break->instr);
      }
      nir_pop_if(b, loop_break_if);

      /* Per-instance loop body. */
      nir_ssa_def *inst_num = nir_load_instance_num_img(b);
      nir_ssa_def *per_inst_cond = nir_ieq(b, loop_iter, inst_num);
      nir_if *inst_num_if = nir_push_if(b, per_inst_cond);
      inst_num_if->control = nir_selection_control_dont_flatten;
      {
         /* To be replaced by the caller of this function. */
         nir_ssa_undef_instr *undef =
            nir_ssa_undef_instr_create(shader,
                                       result_num_components,
                                       result_bit_size);
         nir_builder_instr_insert(b, &undef->instr);
         op_result = &undef->def;
      }
      nir_pop_if(b, inst_num_if);

      /* Loop increment src for loop_iter. */
      nir_ssa_def *loop_incr = nir_iadd(b, loop_iter, loop_incr_val);
      nir_phi_instr_add_src(phi_iter,
                            loop_incr->parent_instr->block,
                            nir_src_for_ssa(loop_incr));
   }
   nir_pop_loop(b, per_inst_loop);

   if (locked)
      nir_mutex_img(b,
                    .mutex_id_img = ROGUE_MUTEX_ID_ATOMIC_EMU,
                    .mutex_op_img = ROGUE_MUTEX_OP_RELEASE);

   b->cursor = nir_before_cf_list(&per_inst_loop->body);
   nir_builder_instr_insert(b, &phi_iter->instr);

   return op_result;
}

static nir_ssa_def *
lower_atomic(nir_builder *b, nir_instr *instr, UNUSED void *_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   unsigned num_components = nir_dest_num_components(intr->dest);
   assert(num_components == 1);

   unsigned bit_size = nir_dest_bit_size(intr->dest);
   assert(bit_size == 32); /* TODO: support other bit sizes. */

   nir_atomic_op op = nir_intrinsic_atomic_op(intr);
   nir_alu_type type = nir_atomic_op_type(op) | bit_size;

   bool is_swap;
   switch (intr->intrinsic) {
   case nir_intrinsic_global_atomic:
      is_swap = false;
      break;

   case nir_intrinsic_global_atomic_swap:
      is_swap = true;
      break;

   default:
      unreachable();
   }

   assert(intr->src[0].is_ssa);
   nir_ssa_def *offset = intr->src[0].ssa;

   assert(intr->src[1].is_ssa);
   nir_ssa_def *value = intr->src[1].ssa;

   nir_ssa_def *value_swap = NULL;
   if (is_swap) {
      assert(intr->src[2].is_ssa);
      value_swap = intr->src[2].ssa;
   }

   /**/

   nir_ssa_def *op_result =
      rogue_per_instance_loop(b, true, num_components, bit_size);
   b->cursor = nir_after_instr(op_result->parent_instr);

   nir_ssa_def *pre_val, *post_val;
   pre_val = nir_load_global(b, offset, bit_size / 8, num_components, bit_size);

   switch (op) {
   case nir_atomic_op_iadd:
      assert(type == nir_type_uint32);
      post_val = nir_iadd(b, pre_val, value);
      break;

   case nir_atomic_op_imin:
      assert(type == nir_type_int32);
      post_val = nir_imin(b, pre_val, value);
      break;

   case nir_atomic_op_umin:
      assert(type == nir_type_uint32);
      post_val = nir_umin(b, pre_val, value);
      break;

   case nir_atomic_op_imax:
      assert(type == nir_type_int32);
      post_val = nir_imax(b, pre_val, value);
      break;

   case nir_atomic_op_umax:
      assert(type == nir_type_uint32);
      post_val = nir_umax(b, pre_val, value);
      break;

   case nir_atomic_op_iand:
      assert(type == nir_type_uint32);
      post_val = nir_iand(b, pre_val, value);
      break;

   case nir_atomic_op_ior:
      assert(type == nir_type_uint32);
      post_val = nir_ior(b, pre_val, value);
      break;

   case nir_atomic_op_ixor:
      assert(type == nir_type_uint32);
      post_val = nir_ixor(b, pre_val, value);
      break;

   case nir_atomic_op_xchg:
      assert(type == nir_type_uint32);
      post_val = nir_mov(b, value);
      break;

   case nir_atomic_op_fadd:
      assert(type == nir_type_float32);
      post_val = nir_fadd(b, pre_val, value);
      break;

   case nir_atomic_op_fmin:
      assert(type == nir_type_float32);
      post_val = nir_fmin(b, pre_val, value);
      break;

   case nir_atomic_op_fmax:
      assert(type == nir_type_float32);
      post_val = nir_fmax(b, pre_val, value);
      break;

   case nir_atomic_op_cmpxchg:
      assert(type == nir_type_uint32);
      post_val = nir_bcsel(b, nir_ieq(b, pre_val, value), value_swap, pre_val);
      break;

   case nir_atomic_op_fcmpxchg:
      assert(type == nir_type_float32);
      post_val = nir_bcsel(b, nir_feq(b, pre_val, value), value_swap, pre_val);
      break;

   default:
      unreachable("Unsupported atomic op.");
   }

   nir_ssa_def_rewrite_uses(op_result, post_val);
   assert(nir_ssa_def_is_unused(op_result));

   nir_store_global(b,
                    offset,
                    bit_size / 8,
                    post_val,
                    BITFIELD_MASK(num_components));

   return pre_val;
}

static bool is_lowerable_atomic(const nir_instr *instr, const void *data)
{
   const struct options *options = data;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
      if (!(options->modes & nir_var_mem_global))
         return false;
      break;

   default:
      return false;
   }

   if (BITFIELD_BIT(nir_intrinsic_atomic_op(intr)) & options->atomic_op_mask)
      return true;

   return false;
}

PUBLIC
bool rogue_nir_lower_atomics(nir_shader *shader,
                             unsigned atomic_op_mask,
                             nir_variable_mode modes)
{
   struct options options = {
      .atomic_op_mask = atomic_op_mask,
      .modes = modes,
   };

   return nir_shader_lower_instructions(shader,
                                        is_lowerable_atomic,
                                        lower_atomic,
                                        &options);
}
