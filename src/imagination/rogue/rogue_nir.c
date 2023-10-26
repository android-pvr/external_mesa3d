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

#include "compiler/spirv/nir_spirv.h"
#include "nir/nir.h"
#include "rogue.h"
#include "util/macros.h"

#include <stdbool.h>

/**
 * \file rogue_nir.c
 *
 * \brief Contains SPIR-V and NIR-specific functions.
 */

/**
 * \brief SPIR-V to NIR compilation options.
 */
static const struct spirv_to_nir_options spirv_options = {
   .environment = NIR_SPIRV_VULKAN,

   .ubo_addr_format = nir_address_format_64bit_global,
   .ssbo_addr_format = nir_address_format_64bit_global,
};

static const nir_shader_compiler_options nir_options = {
   .fuse_ffma32 = true,
   .lower_bitops = true, /* TODO: Re-enable once supported. */
};

static int rogue_glsl_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/**
 * \brief Applies optimizations and passes required to lower the NIR shader into
 * a form suitable for lowering to Rogue IR.
 *
 * \param[in] ctx Shared multi-stage build context.
 * \param[in] shader Rogue shader.
 * \param[in] stage Shader stage.
 */
static void rogue_nir_passes(struct rogue_build_ctx *ctx,
                             nir_shader *nir,
                             gl_shader_stage stage)
{
   bool progress;

#if !defined(NDEBUG)
   bool nir_debug_print_shader_prev = nir_debug_print_shader[nir->info.stage];
   nir_debug_print_shader[nir->info.stage] = ROGUE_DEBUG(NIR_PASSES);
#endif /* !defined(NDEBUG) */

   nir_validate_shader(nir, "after spirv_to_nir");

   /* TODO: Clamp outputs when necessary. */
#if 0
   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      NIR_PASS_V(nir, nir_lower_clamp_color_outputs);
#endif

#if 0
   const struct nir_lower_sysvals_to_varyings_options sysvals_to_varyings = {
      .point_coord = true,
   };
   NIR_PASS_V(nir, nir_lower_sysvals_to_varyings, &sysvals_to_varyings);
#endif

   NIR_PASS_V(nir, nir_lower_variable_initializers, nir_var_shader_out);
   NIR_PASS_V(nir, nir_lower_variable_initializers, ~0);

   if (stage == MESA_SHADER_VERTEX) {
      NIR_PASS_V(nir, nir_lower_clip_cull_distance_arrays);
      NIR_PASS_V(nir,
                 nir_lower_point_size,
                 PVR_POINT_SIZE_RANGE_MIN,
                 PVR_POINT_SIZE_RANGE_MAX);
   }

   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_split_per_member_structs);

   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      NIR_PASS_V(nir,
                 nir_lower_input_attachments,
                 &(nir_input_attachment_options){
                    .use_fragcoord_sysval = true,
                    .use_layer_id_sysval = false,
                 });

   NIR_PASS_V(nir,
              nir_remove_dead_variables,
              nir_var_shader_in | nir_var_shader_out | nir_var_system_value,
              NULL);

   NIR_PASS_V(nir, nir_lower_global_vars_to_local);
   NIR_PASS_V(nir, nir_lower_vars_to_ssa);

   NIR_PASS_V(nir,
              nir_lower_io_to_temporaries,
              nir_shader_get_entrypoint(nir),
              true,
              true);

   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_lower_global_vars_to_local);
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_function_temp, NULL);

   NIR_PASS_V(nir, nir_lower_var_copies);

   NIR_PASS_V(nir, nir_opt_constant_folding);
   NIR_PASS_V(nir, nir_lower_system_values);

   /* Replace references to I/O variables with intrinsics. */
   NIR_PASS_V(nir,
              nir_lower_io,
              nir_var_shader_in | nir_var_shader_out,
              rogue_glsl_type_size,
              (nir_lower_io_options)0);

   /* Load inputs to scalars (single registers later). */
   /* TODO: Fitrp can process multiple frag inputs at once, scalarise I/O. */
   NIR_PASS_V(nir, nir_lower_io_to_scalar, nir_var_shader_in, NULL, NULL);

   /* Optimize GL access qualifiers. */
   const nir_opt_access_options opt_access_options = {
      .is_vulkan = true,
   };
   NIR_PASS_V(nir, nir_opt_access, &opt_access_options);

   /* Apply PFO code to the fragment shader output. */
   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      NIR_PASS_V(nir, rogue_nir_pfo);

   /* Load outputs to scalars (single registers later). */
   NIR_PASS_V(nir, nir_lower_io_to_scalar, nir_var_shader_out, NULL, NULL);

   /* Lower load_consts to scalars. */
   NIR_PASS_V(nir, nir_lower_load_const_to_scalar);

   /* Lower ALU operations to scalars. */
   NIR_PASS_V(nir, nir_lower_alu_to_scalar, NULL, NULL);

   /* Additional I/O lowering. */
   NIR_PASS_V(nir,
              nir_lower_explicit_io,
              nir_var_mem_push_const,
              nir_address_format_32bit_offset);

   NIR_PASS_V(nir,
              nir_lower_explicit_io,
              nir_var_mem_ubo,
              spirv_options.ubo_addr_format);
   NIR_PASS_V(nir, nir_lower_io_to_scalar, nir_var_mem_ubo, NULL, NULL);

   NIR_PASS_V(nir,
              nir_lower_explicit_io,
              nir_var_mem_ssbo,
              spirv_options.ssbo_addr_format);
   NIR_PASS_V(nir, nir_lower_io_to_scalar, nir_var_mem_ssbo, NULL, NULL);

   NIR_PASS_V(nir, rogue_nir_lower_io);

   nir_lower_compute_system_values_options compute_sysval_options = {};
   if (nir->info.stage == MESA_SHADER_COMPUTE)
      NIR_PASS_V(nir, nir_lower_compute_system_values, &compute_sysval_options);

   /* TODO: should really only need to do this once, and also split up lowering
    * i/o and sysvals (and rewrite to use callback functions) need
    * nir_lower_compute_system_values to lower global invocation id to workgroup
    * id, but to not eliminate the local invocation id by making it a const 0
    * also need to check if that's actually what vtx0 is...
    */
   NIR_PASS_V(nir, rogue_nir_lower_io);

   NIR_PASS_V(nir, nir_lower_vars_to_ssa);

   /* Lower samplers. */
   NIR_PASS_V(nir, nir_opt_dce);
   NIR_PASS_V(nir, nir_opt_deref);
   NIR_PASS_V(nir, nir_lower_samplers);

   /* Algebraic opts. */
   do {
      progress = false;

      NIR_PASS(progress, nir, nir_opt_copy_prop_vars);
      NIR_PASS(progress, nir, nir_opt_dead_write_vars);
      NIR_PASS(progress, nir, nir_lower_var_copies);
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);

      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_lower_phis_to_scalar, true);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_peephole_select, 64, false, true);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);

      NIR_PASS(progress, nir, nir_opt_undef);
      NIR_PASS(progress, nir, nir_lower_undef_to_zero);

      NIR_PASS_V(nir, nir_opt_gcm, false);
   } while (progress);

   NIR_PASS_V(nir, nir_opt_shrink_vectors);
   /* NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_function_temp |
    * nir_var_shader_in | nir_var_shader_out, NULL); */

   /* Late algebraic opts. */
   do {
      progress = false;

      NIR_PASS(progress, nir, nir_opt_algebraic_late);
      /*
       * NIR_PASS_V(nir, nir_lower_to_source_mods, nir_lower_all_source_mods);
       */
      NIR_PASS_V(nir, nir_opt_constant_folding);
      NIR_PASS_V(nir, nir_copy_prop);
      NIR_PASS_V(nir, nir_opt_dce);
      NIR_PASS_V(nir, nir_opt_cse);
   } while (progress);

   /* Remove unused constant registers. */
   NIR_PASS_V(nir, nir_opt_dce);

   /*
   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       (nir->info.fs.uses_discard || nir->info.fs.uses_demote)) {
      NIR_PASS_V(nir, nir_opt_conditional_discard);
      NIR_PASS_V(nir, nir_opt_move_discards_to_top);
   }
   */

   /* NIR_PASS_V(nir, nir_opt_move, nir_move_load_ubo); */
   //

   /* Move loads to just before they're needed. */
   /* Disabled for now since we want to try and keep them vectorised and group
    * them. */
   /* TODO: Investigate this further. */
   /* NIR_PASS_V(nir, nir_opt_move, nir_move_load_ubo | nir_move_load_input); */

   /* TODO: Re-enable scheduling after register pressure tweaks. */
#if 0
	/* Instruction scheduling. */
	struct nir_schedule_options schedule_options = {
		.threshold = ROGUE_MAX_REG_TEMP / 2,
	};
	NIR_PASS_V(nir, nir_schedule, &schedule_options);
#endif

   /* Assign I/O locations. */
   nir_assign_io_var_locations(nir,
                               nir_var_shader_in,
                               &nir->num_inputs,
                               nir->info.stage);
   nir_assign_io_var_locations(nir,
                               nir_var_shader_out,
                               &nir->num_outputs,
                               nir->info.stage);

   /* Renumber SSA defs. */
   nir_index_ssa_defs(nir_shader_get_entrypoint(nir));

   /* Gather info into nir shader struct. */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   /* Clean-up after passes. */
   nir_sweep(nir);

   nir_validate_shader(nir, "after passes");
   if (ROGUE_DEBUG(NIR)) {
      fputs("after passes\n", stdout);
      nir_print_shader(nir, stdout);
   }

#if !defined(NDEBUG)
   nir_debug_print_shader[nir->info.stage] = nir_debug_print_shader_prev;
#endif /* !defined(NDEBUG) */
}

static void rogue_collect_early_cs_build_data(rogue_build_ctx *ctx,
                                              nir_shader *nir)
{
   const struct shader_info *info = &nir->info;
   struct rogue_cs_build_data *cs_data = &ctx->stage_data.cs;

   nir_foreach_function (func, nir) {
      nir_foreach_block (block, func->impl) {
         nir_foreach_instr (instr, block) {
            switch (instr->type) {
            case nir_instr_type_intrinsic:
               switch (nir_instr_as_intrinsic(instr)->intrinsic) {
               case nir_intrinsic_load_local_invocation_id_x_img:
                  cs_data->has.location_id_x = true;
                  break;

               case nir_intrinsic_load_local_invocation_id_yz_img:
                  cs_data->has.location_id_y_or_z = true;
                  break;

               case nir_intrinsic_load_workgroup_id_x_img:
                  cs_data->has.work_group_id_x = true;
                  break;

               case nir_intrinsic_load_workgroup_id_y_img:
                  cs_data->has.work_group_id_y = true;
                  break;

               case nir_intrinsic_load_workgroup_id_z_img:
                  cs_data->has.work_group_id_z = true;
                  break;

               default:
                  break;
               }
               break;

            default:
               break;
            }
         }
      }
   }

   cs_data->has.num_work_groups =
      BITSET_TEST(info->system_values_read, SYSTEM_VALUE_NUM_WORKGROUPS);

   /* TODO */
   assert(!info->uses_control_barrier);
   assert(!info->uses_memory_barrier);
   cs_data->has.barrier = false;

   /* TODO */
   cs_data->has.atomic_ops = false;

   cs_data->work_size = info->workgroup_size[0] * info->workgroup_size[1] *
                        info->workgroup_size[2];
}

static void rogue_collect_early_build_data(rogue_build_ctx *ctx,
                                           nir_shader *nir)
{
   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      break;

   case MESA_SHADER_FRAGMENT:
      break;

   case MESA_SHADER_COMPUTE:
      return rogue_collect_early_cs_build_data(ctx, nir);

   default:
      unreachable("Unsupported shader stage.");
   }
}

/**
 * \brief Converts a SPIR-V shader to NIR.
 *
 * \param[in] ctx Shared multi-stage build context.
 * \param[in] entry Shader entry-point function name.
 * \param[in] stage Shader stage.
 * \param[in] spirv_size SPIR-V data length in DWORDs.
 * \param[in] spirv_data SPIR-V data.
 * \param[in] num_spec Number of SPIR-V specializations.
 * \param[in] spec SPIR-V specializations.
 * \return A nir_shader* if successful, or NULL if unsuccessful.
 */
PUBLIC
nir_shader *rogue_spirv_to_nir(rogue_build_ctx *ctx,
                               gl_shader_stage stage,
                               const char *entry,
                               unsigned spirv_size,
                               const uint32_t *spirv_data,
                               unsigned num_spec,
                               struct nir_spirv_specialization *spec)
{
   nir_shader *nir;

   nir = spirv_to_nir(spirv_data,
                      spirv_size,
                      spec,
                      num_spec,
                      stage,
                      entry,
                      &spirv_options,
                      &nir_options);
   if (!nir)
      return NULL;

   ralloc_steal(ctx, nir);

   /* Apply passes. */
   rogue_nir_passes(ctx, nir, stage);

   /* Collect initial build data. */
   rogue_collect_early_build_data(ctx, nir);

   return nir;
}
