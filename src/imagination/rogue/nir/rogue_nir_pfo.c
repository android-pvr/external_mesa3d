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
#include "nir/nir_format_convert.h"
#include "rogue.h"
#include "util/macros.h"
#include "vulkan/vk_format.h"

/**
 * \file rogue_nir_pfo.c
 *
 * \brief Contains the rogue_nir_pfo pass.
 */

static inline nir_def *
do_pack(nir_builder *b, enum pvr_pbe_accum_format pbe, nir_def *chans)
{
   const unsigned bits_2101010[] = { 10, 10, 10, 2 };
   const unsigned bits_8888[] = { 8, 8, 8, 8 };
   const unsigned bits_1616[] = { 16, 16 };

   switch (pbe) {
   case PVR_PBE_ACCUM_FORMAT_U8:
      return nir_pack_unorm_4x8(b, chans);

   case PVR_PBE_ACCUM_FORMAT_S8:
      return nir_pack_snorm_4x8(b, chans);

   case PVR_PBE_ACCUM_FORMAT_U16:
      return nir_pack_unorm_2x16(b, chans);

   case PVR_PBE_ACCUM_FORMAT_S16:
      return nir_pack_snorm_2x16(b, chans);

   case PVR_PBE_ACCUM_FORMAT_F16:
      return nir_pack_half_2x16(b, chans);

   case PVR_PBE_ACCUM_FORMAT_F32:
   case PVR_PBE_ACCUM_FORMAT_SINT32:
   case PVR_PBE_ACCUM_FORMAT_UINT32:
      return chans;

   case PVR_PBE_ACCUM_FORMAT_U1010102:
      return nir_format_pack_uint(b, chans, bits_2101010, chans->num_components);

   case PVR_PBE_ACCUM_FORMAT_UINT8:
   case PVR_PBE_ACCUM_FORMAT_SINT8:
      return nir_format_pack_uint(b, chans, bits_8888, chans->num_components);

   case PVR_PBE_ACCUM_FORMAT_UINT16:
   case PVR_PBE_ACCUM_FORMAT_SINT16:
      return nir_format_pack_uint(b, chans, bits_1616, chans->num_components);

   default:
      break;
   }

   unreachable("Unsupported pbe_accum_format.");
   return NULL;
}

static inline const struct glsl_type *
pbe_accum_to_glsl_type(enum pvr_pbe_accum_format pbe)
{
   switch (pbe) {
   case PVR_PBE_ACCUM_FORMAT_U8:
   case PVR_PBE_ACCUM_FORMAT_UINT8:
   case PVR_PBE_ACCUM_FORMAT_U16:
   case PVR_PBE_ACCUM_FORMAT_UINT16:
   case PVR_PBE_ACCUM_FORMAT_U1010102:
   case PVR_PBE_ACCUM_FORMAT_UINT32:
      return glsl_uintN_t_type(32);

   case PVR_PBE_ACCUM_FORMAT_S8:
   case PVR_PBE_ACCUM_FORMAT_SINT8:
   case PVR_PBE_ACCUM_FORMAT_S16:
   case PVR_PBE_ACCUM_FORMAT_SINT16:
   case PVR_PBE_ACCUM_FORMAT_SINT32:
      return glsl_intN_t_type(32);

   case PVR_PBE_ACCUM_FORMAT_F16:
   case PVR_PBE_ACCUM_FORMAT_F32:
      return glsl_floatN_t_type(32);

   default:
      break;
   }

   unreachable("Unsupported pbe_accum_format.");
   return NULL;
}

/* TODO: Support complex PFO with blending. */
static nir_def *lower_store_output(nir_builder *b, nir_instr *instr, void *cb_data)
{
   struct rogue_fs_build_data *fs_data = (struct rogue_fs_build_data *)cb_data;

   nir_intrinsic_instr *old_store = nir_instr_as_intrinsic(instr);
   nir_src *output_src = &old_store->src[0];
   unsigned components = nir_src_num_components(*output_src);
   assert(components == 4);

   nir_io_semantics sem = nir_intrinsic_io_semantics(old_store);
   nir_variable *output_var =
      nir_find_variable_with_location(b->shader,
                                      nir_var_shader_out,
                                      sem.location);
   unsigned location = sem.location - FRAG_RESULT_DATA0;
   unsigned mrt_idx = location + nir_src_as_uint(old_store->src[1]) +
                      nir_intrinsic_base(old_store);
   const struct usc_mrt_resource *mrt_resource =
      fs_data->outputs[mrt_idx].mrt_resource;

   assert(mrt_resource->type == USC_MRT_RESOURCE_TYPE_OUTPUT_REG);
   assert(mrt_resource->reg.offset == 0);

   enum pvr_pbe_accum_format pbe = fs_data->outputs[mrt_idx].accum_format;

   unsigned pbe_dwords = mrt_resource->intermediate_size;

   const struct glsl_type *glsl_type = pbe_accum_to_glsl_type(pbe);
   nir_alu_type nir_type = nir_get_nir_type_for_glsl_type(glsl_type);

   /* Rewrite the old store and perform the pack/conversion if needed. */

   b->cursor = nir_before_instr(instr);

   unsigned input_size = components / pbe_dwords;
   assert(!(components % pbe_dwords));
   nir_component_mask_t chan_mask = nir_component_mask(input_size);

   nir_def *chans, *pack;
   for (unsigned out = 0; out < pbe_dwords; ++out) {
      chans = nir_channels(b, output_src->ssa, chan_mask << (out * input_size));
      /* TODO: Make the pack optional. */
      pack = do_pack(b, pbe, chans);
      nir_store_output(b,
                       pack,
                       nir_imm_zero(b, 1, 32),
                       .base = mrt_resource->reg.output_reg + out,
                       .src_type = nir_type,
                       .write_mask = 1,
                       .io_semantics = nir_intrinsic_io_semantics(old_store));
   }

   /* Update the variable and dereference types. */
   output_var->type = glsl_type;

   return NIR_LOWER_INSTR_PROGRESS_REPLACE;
}

static bool is_store_output(const nir_instr *instr, UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   assert(sem.dual_source_blend_index == 0 && "Should have been lowered");
   return (sem.location >= FRAG_RESULT_DATA0);
}

PUBLIC
bool rogue_nir_pfo(nir_shader *shader, struct rogue_fs_build_data *fs_data)
{
   /* Only apply to fragment shaders. */
   if (shader->info.stage != MESA_SHADER_FRAGMENT)
      return false;

   return nir_shader_lower_instructions(shader,
                                        is_store_output,
                                        lower_store_output,
                                        fs_data);
}
