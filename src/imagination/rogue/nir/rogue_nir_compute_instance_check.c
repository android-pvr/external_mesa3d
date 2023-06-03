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
 * \file rogue_nir_compute_instance_check.c
 *
 * \brief Contains the rogue_nir_compute_instance_check pass.
 */

#define ROGUE_INST_CHK_FUNC_NAME "__rogue_inst_chk_func__"

/* Inserts an instance check for compute shaders. */
PUBLIC
bool rogue_nir_compute_instance_check(nir_shader *shader)
{
   /* Only apply to compute shaders. */
   if (shader->info.stage != MESA_SHADER_COMPUTE)
      return false;

   /* Check we haven't already done this. */
   nir_foreach_function (function, shader) {
      if (!function->name || !strcmp(function->name, ROGUE_INST_CHK_FUNC_NAME))
         return false;
   }

   /* Get original entrypoint. */
   nir_function_impl *orig_entrypoint_impl = nir_shader_get_entrypoint(shader);
   nir_function *orig_entrypoint = orig_entrypoint_impl->function;

   /* Create a function for the instance check which will serve as the new
    * entrypoint.
    */
   nir_function *inst_chk_func =
      nir_function_create(shader, ROGUE_INST_CHK_FUNC_NAME);
   nir_function_impl *inst_chk_func_impl =
      nir_function_impl_create(inst_chk_func);

   inst_chk_func->is_entrypoint = true;
   orig_entrypoint->is_entrypoint = false;

   nir_builder b = nir_builder_create(inst_chk_func_impl);
   b.cursor = nir_after_cf_list(&inst_chk_func_impl->body);

   /* If the current instance index is greater than the total workgroup size,
    * we don't execute.
    */

   nir_ssa_def *local_size = nir_load_workgroup_size(&b);
   nir_ssa_def *size_x = nir_channel(&b, local_size, 0);
   nir_ssa_def *size_y = nir_channel(&b, local_size, 1);
   nir_ssa_def *size_z = nir_channel(&b, local_size, 2);

   nir_ssa_def *flat_size = nir_imul(&b, nir_imul(&b, size_x, size_y), size_z);
   nir_ssa_def *flat_id = nir_load_local_invocation_index(&b);
   nir_ssa_def *cond_inst_valid = nir_ilt32(&b, flat_id, flat_size);

   nir_if *nif = nir_push_if(&b, cond_inst_valid);
   nif->control = nir_selection_control_dont_flatten;

   nir_block *if_then_block = nir_if_first_then_block(nif);

   /* Call the original entrypoint (will get inlined). */
   nir_call_instr *ncall = nir_call_instr_create(shader, orig_entrypoint);
   nir_instr_insert_after_block(if_then_block, &ncall->instr);

   nir_pop_if(&b, nif);
   nir_jump(&b, nir_jump_return);

   return true;
}
