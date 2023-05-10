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

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "nir/nir_search_helpers.h"
#include "rogue.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * \file rogue_nir_lower_io.c
 *
 * \brief Contains the rogue_nir_lower_io pass.
 */

static bool lower_load_vulkan_descriptor(nir_builder *b,
                                         nir_intrinsic_instr *intr)
{
   nir_intrinsic_instr *vk_res_idk = nir_src_as_intrinsic(intr->src[0]);
   assert(vk_res_idk->intrinsic == nir_intrinsic_vulkan_resource_index);

   /* Fetch the desc_set, binding, desc_type. */
   unsigned desc_set = nir_intrinsic_desc_set(vk_res_idk);
   unsigned binding = nir_intrinsic_binding(vk_res_idk);
   VkDescriptorType desc_type = nir_intrinsic_desc_type(vk_res_idk);

   /* TODO: Skip VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER and other types
    * that will be put into shareds. Rather than hardcoding types,
    * look at info passed by the driver.
    * For types that live in memory, we can probably skip the custom intrinsics
    * -> lowering to load_global and instead just load_global directly.
    */

   b->cursor = nir_before_instr(&intr->instr);

   nir_ssa_def *ld_tbl_addr =
      nir_load_vulkan_desc_set_table_addr_img(b,
                                              nir_imm_int(b, desc_set),
                                              .desc_set = desc_set);
   nir_ssa_def *ld_set_addr =
      nir_load_vulkan_desc_set_addr_img(b,
                                        ld_tbl_addr,
                                        nir_imm_int(b, binding),
                                        .desc_set = desc_set,
                                        .binding = binding,
                                        .desc_type = desc_type);

   nir_ssa_def_rewrite_uses(&intr->dest.ssa, ld_set_addr);
   nir_instr_remove(&intr->instr);

   return true;
}

static bool lower_load_global_constant_to_scalar(nir_builder *b,
                                                 nir_intrinsic_instr *intr)
{
   if (ROGUE_DEBUG(BURST_LOADS))
      return false;

   /* Scalarize the load_global_constant. */
   b->cursor = nir_before_instr(&intr->instr);

   assert(intr->dest.is_ssa);
   if (intr->num_components == 1)
      return false;

   nir_ssa_def *loads[NIR_MAX_VEC_COMPONENTS];

   for (uint8_t i = 0; i < intr->num_components; i++) {
      nir_intrinsic_instr *chan_intr =
         nir_intrinsic_instr_create(b->shader, intr->intrinsic);
      nir_ssa_dest_init(&chan_intr->instr,
                        &chan_intr->dest,
                        1,
                        intr->dest.ssa.bit_size);
      chan_intr->num_components = 1;

      nir_intrinsic_set_access(chan_intr, nir_intrinsic_access(intr));
      nir_intrinsic_set_align_mul(chan_intr, nir_intrinsic_align_mul(intr));
      nir_intrinsic_set_align_offset(chan_intr,
                                     nir_intrinsic_align_offset(intr));

      /* Address. */
      chan_intr->src[0] =
         nir_src_for_ssa(nir_iadd_imm(b, intr->src[0].ssa, i * 4));

      nir_builder_instr_insert(b, &chan_intr->instr);

      loads[i] = &chan_intr->dest.ssa;
   }

   nir_ssa_def_rewrite_uses(&intr->dest.ssa,
                            nir_vec(b, loads, intr->num_components));
   nir_instr_remove(&intr->instr);

   return true;
}

static bool lower_load_push_constant_to_scalar(nir_builder *b,
                                               nir_intrinsic_instr *intr)
{
   if (ROGUE_DEBUG(BURST_LOADS))
      return false;

   /* Scalarize the load_push_constant. */
   b->cursor = nir_before_instr(&intr->instr);

   assert(intr->dest.is_ssa);
   if (intr->num_components == 1)
      return false;

   nir_ssa_def *loads[NIR_MAX_VEC_COMPONENTS];

   for (uint8_t i = 0; i < intr->num_components; i++) {
      nir_intrinsic_instr *chan_intr =
         nir_intrinsic_instr_create(b->shader, intr->intrinsic);
      nir_ssa_dest_init(&chan_intr->instr,
                        &chan_intr->dest,
                        1,
                        intr->dest.ssa.bit_size);
      chan_intr->num_components = 1;

      nir_intrinsic_set_base(chan_intr, nir_intrinsic_base(intr));
      nir_intrinsic_set_range(chan_intr, nir_intrinsic_range(intr));
      nir_intrinsic_set_align_mul(chan_intr, nir_intrinsic_align_mul(intr));
      nir_intrinsic_set_align_offset(chan_intr,
                                     nir_intrinsic_align_offset(intr) + i * 4);

      /* Offset. */
      chan_intr->src[0] =
         nir_src_for_ssa(nir_iadd_imm(b, intr->src[0].ssa, i * 4));

      nir_builder_instr_insert(b, &chan_intr->instr);

      loads[i] = &chan_intr->dest.ssa;
   }

   nir_ssa_def_rewrite_uses(&intr->dest.ssa,
                            nir_vec(b, loads, intr->num_components));
   nir_instr_remove(&intr->instr);

   return true;
}

/* Scalarize/convert load_local_invocation_id to our custom intrinsics.
 * Unused components will get DCEd later.
 */
static bool lower_load_local_invocation_id(nir_builder *b,
                                           nir_intrinsic_instr *intr)
{
   assert(intr->dest.is_ssa);
   assert(intr->dest.ssa.num_components == 3);
   assert(intr->dest.ssa.bit_size == 32);

   b->cursor = nir_before_instr(&intr->instr);

   /* x component is 32-bit OOTB. */
   nir_ssa_def *lid_x = nir_load_local_invocation_id_x_img(b);

   /* yz components are 16-bit and supplied packed into a 32-bit reg. */
   nir_ssa_def *lid_yz_packed32 = nir_load_local_invocation_id_yz_img(b);
   nir_ssa_def *lid_yz_2x16 = nir_unpack_32_2x16(b, lid_yz_packed32);
   nir_ssa_def *lid_yz = nir_i2i32(b, lid_yz_2x16);

   nir_ssa_def_rewrite_uses(
      &intr->dest.ssa,
      nir_vec3(b, lid_x, nir_channel(b, lid_yz, 0), nir_channel(b, lid_yz, 1)));
   nir_instr_remove(&intr->instr);

   return true;
}

/*
 * Scalarize/convert the load_workgroup_id to our custom intrinsics.
 * Unused components will get DCEd later.
 */
static bool lower_load_workgroup_id(nir_builder *b, nir_intrinsic_instr *intr)
{
   assert(intr->dest.is_ssa);
   assert(intr->dest.ssa.num_components == 3);
   assert(intr->dest.ssa.bit_size == 32);

   b->cursor = nir_before_instr(&intr->instr);

   nir_ssa_def *wgid_x = nir_load_workgroup_id_x_img(b);
   nir_ssa_def *wgid_y = nir_load_workgroup_id_y_img(b);
   nir_ssa_def *wgid_z = nir_load_workgroup_id_z_img(b);

   nir_ssa_def_rewrite_uses(&intr->dest.ssa,
                            nir_vec3(b, wgid_x, wgid_y, wgid_z));
   nir_instr_remove(&intr->instr);

   return true;
}

static bool lower_load_vulkan_desc_set_table_addr_img(nir_builder *b,
                                                      nir_intrinsic_instr *intr,
                                                      rogue_build_ctx *ctx)
{
   b->cursor = nir_before_instr(&intr->instr);

   unsigned desc_set = nir_intrinsic_desc_set(intr);
   unsigned load_align = intr->dest.ssa.bit_size / 8;

   /* This will be handled in rogue_nir_to_rogue, load the base address from
    * shareds. */
   nir_ssa_def *tbl_base_addr = nir_load_vulkan_desc_set_table_base_addr_img(b);

   ASSERTED const struct pvr_pipeline_layout *pipeline_layout =
      ctx->pipeline_layout;
   assert(desc_set < pipeline_layout->set_count);

   /* Calculate offset for the descriptor set. */
   unsigned desc_set_offset = desc_set * sizeof(pvr_dev_addr_t);

   /* Load the descriptor set table address for this set from memory. */
   nir_ssa_def *tbl_addr =
      nir_load_global_constant(b,
                               nir_iadd_imm(b, tbl_base_addr, desc_set_offset),
                               load_align,
                               intr->dest.ssa.num_components,
                               intr->dest.ssa.bit_size);

   nir_ssa_def_rewrite_uses(&intr->dest.ssa, tbl_addr);
   nir_instr_remove(&intr->instr);

   return true;
}

static inline bool descriptor_is_dynamic(VkDescriptorType type)
{
   return (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
}

static bool lower_load_vulkan_desc_set_addr_img(nir_builder *b,
                                                nir_intrinsic_instr *intr,
                                                rogue_build_ctx *ctx)
{
   b->cursor = nir_before_instr(&intr->instr);

   enum pvr_stage_allocation pvr_stage =
      mesa_stage_to_pvr(b->shader->info.stage);
   unsigned desc_set = nir_intrinsic_desc_set(intr);
   unsigned binding = nir_intrinsic_binding(intr);
   UNUSED VkDescriptorType desc_type = nir_intrinsic_desc_type(intr);
   unsigned load_align = intr->dest.ssa.bit_size / 8;

   const struct pvr_pipeline_layout *pipeline_layout = ctx->pipeline_layout;
   const struct pvr_descriptor_set_layout *set_layout =
      pipeline_layout->set_layout[desc_set];
   const struct pvr_descriptor_set_layout_mem_layout *mem_layout =
      &set_layout->memory_layout_in_dwords_per_stage[pvr_stage];
   assert(binding < set_layout->binding_count);

   /* Calculate offset for the descriptor/binding in this set. */
   const struct pvr_descriptor_set_layout_binding *binding_layout =
      pvr_get_descriptor_binding(set_layout, binding);

   /* TODO: Handle secondaries. */
   /* TODO: Handle bindings having multiple descriptors
    * (VkDescriptorSetLayoutBinding->descriptorCount).
    */

   unsigned desc_offset = descriptor_is_dynamic(binding_layout->type)
                             ? set_layout->total_size_in_dwords
                             : mem_layout->primary_offset;
   desc_offset += binding_layout->per_stage_offset_in_dwords[pvr_stage].primary;
   desc_offset = PVR_DW_TO_BYTES(desc_offset);

   /* Load the descriptor set table address for this set from memory. */
   assert(intr->src[0].is_ssa);
   nir_ssa_def *tbl_addr = intr->src[0].ssa;
   nir_ssa_def *desc_addr =
      nir_load_global_constant(b,
                               nir_iadd_imm(b, tbl_addr, desc_offset),
                               load_align,
                               intr->dest.ssa.num_components,
                               intr->dest.ssa.bit_size);

   nir_ssa_def_rewrite_uses(&intr->dest.ssa, desc_addr);
   nir_instr_remove(&intr->instr);

   return true;
}

static bool lower_load_push_constant(nir_builder *b, nir_intrinsic_instr *intr)
{
   b->cursor = nir_before_instr(&intr->instr);

   /* This will be handled in rogue_nir_to_rogue, load the base address from
    * shareds. */
   nir_ssa_def *push_consts_base_addr = nir_load_push_consts_base_addr_img(b);

   /* Calculate offset for the push constant. */
   assert(intr->src[0].is_ssa);
   nir_ssa_def *offset = intr->src[0].ssa;
   unsigned load_align = intr->dest.ssa.bit_size / 8;

   /* Load the push constant. */
   nir_ssa_def *push_const = nir_load_global_constant(
      b,
      nir_iadd(b, push_consts_base_addr, nir_u2u64(b, offset)),
      load_align,
      intr->dest.ssa.num_components,
      intr->dest.ssa.bit_size);

   nir_ssa_def_rewrite_uses(&intr->dest.ssa, push_const);
   nir_instr_remove(&intr->instr);

   return true;
}

static bool lower_intrinsic(nir_builder *b,
                            nir_intrinsic_instr *instr,
                            rogue_build_ctx *ctx,
                            bool late)
{
   if (!late) {
      switch (instr->intrinsic) {
      case nir_intrinsic_load_vulkan_descriptor:
         return lower_load_vulkan_descriptor(b, instr);

      case nir_intrinsic_load_global_constant:
         return lower_load_global_constant_to_scalar(b, instr);

      case nir_intrinsic_load_push_constant:
         return lower_load_push_constant_to_scalar(b, instr);

      case nir_intrinsic_load_local_invocation_id:
         return lower_load_local_invocation_id(b, instr);

      default:
         break;
      }
   } else {
      switch (instr->intrinsic) {
      case nir_intrinsic_load_workgroup_id:
         return lower_load_workgroup_id(b, instr);

      case nir_intrinsic_load_vulkan_desc_set_table_addr_img:
         return lower_load_vulkan_desc_set_table_addr_img(b, instr, ctx);

      case nir_intrinsic_load_vulkan_desc_set_addr_img:
         return lower_load_vulkan_desc_set_addr_img(b, instr, ctx);

      case nir_intrinsic_load_push_constant:
         return lower_load_push_constant(b, instr);

      default:
         break;
      }
   }

   return false;
}

static bool lower_impl(nir_function_impl *impl, rogue_build_ctx *ctx, bool late)
{
   bool progress = false;
   nir_builder b = nir_builder_create(impl);

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         b.cursor = nir_before_instr(instr);
         switch (instr->type) {
         case nir_instr_type_intrinsic:
            progress |=
               lower_intrinsic(&b, nir_instr_as_intrinsic(instr), ctx, late);
            break;

         default:
            break;
         }
      }
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_none);
   else
      nir_metadata_preserve(impl, nir_metadata_all);

   return progress;
}

PUBLIC
bool rogue_nir_lower_io(nir_shader *shader, rogue_build_ctx *ctx, bool late)
{
   bool progress = false;

   nir_foreach_function (function, shader) {
      if (function->impl)
         progress |= lower_impl(function->impl, ctx, late);
   }

   if (progress)
      nir_opt_dce(shader);

   return progress;
}
