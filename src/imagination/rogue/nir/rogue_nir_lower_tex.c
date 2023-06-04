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
#include "nir/nir_format_convert.h"
#include "rogue.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * \file rogue_nir_lower_tex.c
 *
 * \brief Contains the rogue_nir_lower_tex pass.
 */

static void get_tex_deref_layout(nir_builder *b,
                                 nir_deref_instr *deref,
                                 rogue_build_ctx *ctx,
                                 bool sampler,
                                 bool primary,
                                 uint32_t *sh_base_index,
                                 nir_ssa_def **index_ssa)
{
   uint32_t imm_index = 0;
   uint32_t sh_imm_index = 0;
   *index_ssa = NULL;

   if (deref->deref_type == nir_deref_type_array) {
      assert(deref->arr.index.is_ssa);
      if (nir_src_is_const(deref->arr.index))
         imm_index = nir_src_as_uint(deref->arr.index);
      else
         *index_ssa = deref->arr.index.ssa;

      deref = nir_deref_instr_parent(deref);
   }

   assert(deref->deref_type == nir_deref_type_var);
   nir_variable *var = deref->var;

   unsigned desc_set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   enum pvr_stage_allocation pvr_stage =
      mesa_stage_to_pvr(b->shader->info.stage);
   const struct pvr_pipeline_layout *pipeline_layout = ctx->pipeline_layout;
   const struct pvr_descriptor_set_layout *set_layout =
      pipeline_layout->set_layout[desc_set];
   assert(binding < set_layout->binding_count);

   /* Calculate offset for the descriptor/binding in this set. */
   const struct pvr_descriptor_set_layout_binding *binding_layout =
      pvr_get_descriptor_binding(set_layout, binding);

   unsigned desc_size;
   switch (binding_layout->type) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
      assert(sampler);
      desc_size = PVR_SAMPLER_DESCRIPTOR_SIZE;
      break;

   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      desc_size = PVR_SAMPLER_DESCRIPTOR_SIZE + PVR_IMAGE_DESCRIPTOR_SIZE;
      break;

   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      assert(!sampler);
      desc_size = PVR_IMAGE_DESCRIPTOR_SIZE;
      break;

   default:
      unreachable("Descriptor is not located in the shareds");
   }

   if (primary) {
      sh_imm_index = pvr_get_required_descriptor_primary_sh_reg(pipeline_layout,
                                                                pvr_stage,
                                                                desc_set,
                                                                binding_layout);
   } else {
      sh_imm_index =
         pvr_get_sampler_descriptor_secondary_sh_reg(pipeline_layout,
                                                     pvr_stage,
                                                     desc_set,
                                                     binding_layout);
   }

   sh_imm_index += imm_index * desc_size;

   /* First comes image state and then the sampler in the driver layout */
   if (binding_layout->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
       sampler) {
      sh_imm_index += PVR_IMAGE_DESCRIPTOR_SIZE;
   }

   if (*index_ssa)
      *index_ssa = nir_imul_imm(b, *index_ssa, desc_size);

   if (!sh_base_index && *index_ssa)
      *index_ssa = nir_iadd_imm(b, *index_ssa, sh_imm_index);
   else if (!sh_base_index)
      *index_ssa = nir_imm_int(b, sh_imm_index);
   else
      *sh_base_index = sh_imm_index;
}

static bool rogue_nir_lower_image_store(nir_builder *b,
                                        nir_intrinsic_instr *intr)
{
   enum pipe_format format = nir_intrinsic_format(intr);
   assert(format != PIPE_FORMAT_NONE);
   const struct util_format_description *desc = util_format_description(format);
   const struct util_format_channel_description *r_chan = &desc->channel[0];
   unsigned num_components = util_format_get_nr_components(format);

   /* HW will the take the first n-bits, and out of band
    * value writes to images are UB in SPIR-V
    */
   if (r_chan->pure_integer || r_chan->size == 32)
      return false;

   b->cursor = nir_before_instr(&intr->instr);

   nir_ssa_def *color = nir_ssa_for_src(b, intr->src[3], num_components);
   nir_ssa_def *formatted = NULL;

   const unsigned bits_16[] = { 16, 16, 16, 16 };
   const unsigned bits_8[] = { 8, 8, 8, 8 };
   const unsigned bits_10[] = { 10, 10, 10, 2 };
   const unsigned *bits;
   switch (r_chan->size) {
   case 11:
      assert(format == PIPE_FORMAT_R11G11B10_FLOAT);
      /* 10 and 11-bit floats are unsigned.  Clamp to non-negative */
      color = nir_fmax(b, color, nir_imm_float(b, 0));
      bits = bits_16;
      break;

   case 16:
      bits = bits_16;
      break;
   case 10:
      bits = bits_10;
      break;
   case 8:
      bits = bits_8;
      break;
   default:
      unreachable("Unsupported image format");
   }

   if (r_chan->normalized) {
      assert(r_chan->type == UTIL_FORMAT_TYPE_SIGNED ||
             r_chan->type == UTIL_FORMAT_TYPE_UNSIGNED);
      if (r_chan->type == UTIL_FORMAT_TYPE_SIGNED)
         formatted = nir_format_float_to_snorm(b, color, bits);
      else
         formatted = nir_format_float_to_unorm(b, color, bits);
   } else {
      assert(r_chan->type == UTIL_FORMAT_TYPE_FLOAT);
      formatted = nir_format_float_to_half(b, color);
      if (format == PIPE_FORMAT_R11G11B10_FLOAT) {
         nir_ssa_def *comps[3] = {
            nir_mask_shift(b, nir_channel(b, formatted, 0), 0x00007ff0, -4),
            nir_mask_shift(b, nir_channel(b, formatted, 1), 0x00007ff0, -4),
            nir_mask_shift(b, nir_channel(b, formatted, 2), 0x00007fe0, -5),
         };
         formatted = nir_vec(b, comps, 3);
      }
   }

   nir_instr_rewrite_src(&intr->instr,
                         &intr->src[3],
                         nir_src_for_ssa(formatted));
   intr->num_components = formatted->num_components;

   return true;
}

static bool lower_tex_instr_img(nir_builder *b,
                                nir_intrinsic_instr *intr,
                                rogue_build_ctx *ctx)
{
#define OP_SWAP(OP)                                        \
   case nir_intrinsic_image_deref_##OP:                    \
      intr->intrinsic = nir_intrinsic_bindless_image_##OP; \
      break;

   switch (intr->intrinsic) {
      OP_SWAP(load)
      OP_SWAP(store)
      OP_SWAP(size)
      OP_SWAP(samples)
      OP_SWAP(texel_address)

   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
      unreachable("Should have been lowered!");

   default:
      return false;
   }
#undef OP_SWAP

   b->cursor = nir_before_instr(&intr->instr);

   nir_ssa_def *index_ssa;
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   bool size_query = intr->intrinsic == nir_intrinsic_bindless_image_size;
   get_tex_deref_layout(b, deref, ctx, false, !size_query, NULL, &index_ssa);

   if (!size_query && nir_intrinsic_image_array(intr)) {
      nir_ssa_def *indices[2];
      indices[0] = index_ssa;
      get_tex_deref_layout(b, deref, ctx, false, false, NULL, &indices[1]);
      index_ssa = nir_vec(b, indices, 2);
   }

   nir_instr_rewrite_src(&intr->instr,
                         &intr->src[0],
                         nir_src_for_ssa(index_ssa));

   nir_variable *var = nir_deref_instr_get_variable(deref);
   nir_intrinsic_set_format(intr, var->data.image.format);
   nir_intrinsic_set_access(intr, var->data.access);

   if (intr->intrinsic == nir_intrinsic_bindless_image_store)
      rogue_nir_lower_image_store(b, intr);

   return true;
}

static bool
lower_tex_instr_tex(nir_builder *b, nir_tex_instr *tex, rogue_build_ctx *ctx)
{
   bool progress = false;

   b->cursor = nir_before_instr(&tex->instr);

   int sampler_src_idx =
      nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (sampler_src_idx >= 0) {
      nir_deref_instr *deref = nir_src_as_deref(tex->src[sampler_src_idx].src);
      nir_tex_instr_remove_src(tex, sampler_src_idx);

      nir_ssa_def *index_ssa;
      get_tex_deref_layout(b,
                           deref,
                           ctx,
                           true,
                           true,
                           &tex->sampler_index,
                           &index_ssa);

      if (index_ssa) {
         nir_tex_instr_add_src(tex,
                               nir_tex_src_sampler_offset,
                               nir_src_for_ssa(index_ssa));
      }
      progress = true;
   }

   int tex_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   if (tex_src_idx >= 0) {
      nir_deref_instr *deref = nir_src_as_deref(tex->src[tex_src_idx].src);
      nir_tex_instr_remove_src(tex, tex_src_idx);

      nir_ssa_def *index_ssa;
      get_tex_deref_layout(b,
                           deref,
                           ctx,
                           false,
                           true,
                           &tex->texture_index,
                           &index_ssa);

      if (index_ssa) {
         nir_tex_instr_add_src(tex,
                               nir_tex_src_texture_offset,
                               nir_src_for_ssa(index_ssa));
      }

      if (tex->is_array || tex->op == nir_texop_txs) {
         nir_ssa_def *secondary_index;
         get_tex_deref_layout(b,
                              deref,
                              ctx,
                              false,
                              false,
                              NULL,
                              &secondary_index);
         nir_tex_instr_add_src(tex,
                               nir_tex_src_backend1,
                               nir_src_for_ssa(secondary_index));
      }

      progress = true;
   }

   return progress;
}

static bool lower_tex_instr(nir_builder *b, nir_instr *instr, void *ctx)
{
   switch (instr->type) {
   case nir_instr_type_intrinsic:
      return lower_tex_instr_img(b, nir_instr_as_intrinsic(instr), ctx);
      break;

   case nir_instr_type_tex:
      return lower_tex_instr_tex(b, nir_instr_as_tex(instr), ctx);
      break;

   default:
      break;
   }

   return false;
}

PUBLIC
bool rogue_nir_lower_tex(nir_shader *shader, rogue_build_ctx *ctx)
{
   bool progress = false;

   NIR_PASS(progress,
            shader,
            nir_lower_tex,
            &(nir_lower_tex_options){
               .lower_txs_lod = true,
            });

   NIR_PASS(progress, shader, nir_lower_image_atomics_to_global);

   progress = nir_shader_instructions_pass(shader,
                                           lower_tex_instr,
                                           nir_metadata_dominance,
                                           ctx);

   if (progress)
      nir_opt_dce(shader);

   return progress;
}
