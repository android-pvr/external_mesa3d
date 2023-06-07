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
#include "nir/nir_lower_blend.h"
#include "rogue.h"
#include "util/macros.h"
#include "vk_blend.h"
#include "vk_graphics_state.h"
#include "vulkan/vk_format.h"

/**
 * \file rogue_nir_pfo.c
 *
 * \brief Contains the rogue_nir_pfo pass.
 */

static nir_ssa_def *nir_fsat_signed(nir_builder *b, nir_ssa_def *x)
{
   return nir_fclamp(b,
                     x,
                     nir_imm_floatN_t(b, -1.0, x->bit_size),
                     nir_imm_floatN_t(b, +1.0, x->bit_size));
}

static nir_ssa_def *
nir_fsat_to_format(nir_builder *b, nir_ssa_def *x, enum pipe_format format)
{
   if (util_format_is_unorm(format))
      return nir_fsat(b, x);
   else if (util_format_is_snorm(format))
      return nir_fsat_signed(b, x);
   else
      return x;
}

static inline nir_ssa_def *do_pack(nir_builder *b,
                                   enum pvr_pbe_accum_format pbe,
                                   nir_ssa_def *chans,
                                   enum pipe_format format)
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
      return nir_pack_half_2x16(b, nir_fsat_to_format(b, chans, format));

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

static inline nir_ssa_def *do_unpack(nir_builder *b,
                                     enum pvr_pbe_accum_format pbe,
                                     nir_ssa_def *pck,
                                     unsigned num_components)
{
   const unsigned bits_2101010[] = { 10, 10, 10, 2 };
   const unsigned bits_8888[] = { 8, 8, 8, 8 };
   const unsigned bits_1616[] = { 16, 16 };

   switch (pbe) {
   case PVR_PBE_ACCUM_FORMAT_U8:
      return nir_unpack_unorm_4x8(b, pck);

   case PVR_PBE_ACCUM_FORMAT_S8:
      return nir_unpack_snorm_4x8(b, pck);

   case PVR_PBE_ACCUM_FORMAT_U16:
      return nir_unpack_unorm_2x16(b, pck);

   case PVR_PBE_ACCUM_FORMAT_S16:
      return nir_unpack_snorm_2x16(b, pck);

   case PVR_PBE_ACCUM_FORMAT_F16:
      return nir_unpack_half_2x16(b, pck);

   case PVR_PBE_ACCUM_FORMAT_F32:
   case PVR_PBE_ACCUM_FORMAT_SINT32:
   case PVR_PBE_ACCUM_FORMAT_UINT32:
      return pck;

   case PVR_PBE_ACCUM_FORMAT_U1010102:
      return nir_format_unpack_uint(b, pck, bits_2101010, num_components);

   case PVR_PBE_ACCUM_FORMAT_UINT8:
      return nir_format_unpack_uint(b, pck, bits_8888, num_components);

   case PVR_PBE_ACCUM_FORMAT_SINT8:
      return nir_format_unpack_int(b, pck, bits_8888, num_components, true);

   case PVR_PBE_ACCUM_FORMAT_UINT16:
      return nir_format_unpack_uint(b, pck, bits_1616, num_components);

   case PVR_PBE_ACCUM_FORMAT_SINT16:
      return nir_format_unpack_int(b, pck, bits_1616, num_components, true);

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
static nir_ssa_def *
lower_output_io(nir_builder *b, nir_instr *instr, void *cb_data)
{
   struct rogue_fs_build_data *fs_data = (struct rogue_fs_build_data *)cb_data;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   bool is_store = intr->intrinsic == nir_intrinsic_store_output;

   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   nir_variable *output_var =
      nir_find_variable_with_location(b->shader,
                                      nir_var_shader_out,
                                      sem.location);
   unsigned location = sem.location - FRAG_RESULT_DATA0;
   unsigned mrt_idx = location + nir_src_as_uint(intr->src[!!is_store]) +
                      nir_intrinsic_base(intr);
   const struct usc_mrt_resource *mrt_resource =
      fs_data->outputs[mrt_idx].mrt_resource;

   assert(mrt_resource->type == USC_MRT_RESOURCE_TYPE_OUTPUT_REG);
   assert(mrt_resource->reg.offset == 0);

   enum pvr_pbe_accum_format pbe = fs_data->outputs[mrt_idx].accum_format;
   enum pipe_format format = fs_data->outputs[mrt_idx].format;
   unsigned components = util_format_get_nr_components(format);

   unsigned pbe_dwords =
      DIV_ROUND_UP(mrt_resource->intermediate_size, sizeof(uint32_t));

   const struct glsl_type *glsl_type = pbe_accum_to_glsl_type(pbe);
   nir_alu_type nir_type = nir_get_nir_type_for_glsl_type(glsl_type);

   /* Rewrite the old store and perform the pack/conversion if needed. */

   b->cursor = nir_before_instr(instr);

   unsigned size = DIV_ROUND_UP(components, pbe_dwords);

   if (is_store) {
      nir_src *output_src = &intr->src[0];

      for (unsigned out = 0; out < pbe_dwords; ++out) {
         nir_component_mask_t chan_mask =
            nir_component_mask(MIN2(size, components - out * size));

         nir_ssa_def *chans =
            nir_channels(b, output_src->ssa, chan_mask << (out * size));
         chans = nir_resize_vector(b, chans, size);

         /* TODO: Make the pack optional. */
         nir_ssa_def *pack = do_pack(b, pbe, chans, format);

         nir_store_output(b,
                          pack,
                          nir_imm_int(b, 0),
                          .base = mrt_resource->reg.output_reg + out,
                          .src_type = nir_type,
                          .write_mask = 1,
                          .io_semantics = sem);
      }

      /* Update the variable and dereference types. */
      output_var->type = glsl_type;

      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   bool is_float = nir_alu_type_get_base_type(nir_type) == nir_type_float;

   int num_unpack_comp = 0;
   nir_ssa_def *unpacks[4];

   for (unsigned in = 0; in < pbe_dwords; ++in) {
      nir_ssa_def *load =
         nir_load_output(b,
                         1,
                         32,
                         nir_imm_int(b, 0),
                         .base = mrt_resource->reg.output_reg + in,
                         .dest_type = nir_type,
                         .io_semantics = sem);

      nir_ssa_def *upck = do_unpack(b, pbe, load, size);
      for (unsigned c = 0; c < MIN2(size, components - in * size); c++)
         unpacks[num_unpack_comp++] = nir_channel(b, upck, c);
   }

   /* Missing .gba comps */
   for (; num_unpack_comp < 3; num_unpack_comp++)
      unpacks[num_unpack_comp] = nir_imm_int(b, 0);
   if (num_unpack_comp == 3) {
      unpacks[num_unpack_comp++] = is_float ? nir_imm_float(b, 1.0)
                                            : nir_imm_int(b, 0);
   }

   assert(num_unpack_comp == 4);
   return nir_vec(b, unpacks, num_unpack_comp);
}

static bool is_output_io(const nir_instr *instr, UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_store_output &&
       intr->intrinsic != nir_intrinsic_load_output)
      return false;

   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   assert(sem.dual_source_blend_index == 0 && "Should have been lowered");
   return (sem.location >= FRAG_RESULT_DATA0);
}

static bool
lower_blend_consts(struct nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_blend_const_color_rgba)
      return false;

   assert(intr->dest.is_ssa);
   nir_ssa_def *def = &intr->dest.ssa;

   b->cursor = nir_before_instr(&intr->instr);

   rogue_build_ctx *ctx = data;
   struct rogue_fs_build_data *fs_data = &ctx->stage_data.fs;
   const float *v = fs_data->cb_state->blend_constants;

   const struct pvr_pipeline_layout *pipeline_layout = ctx->pipeline_layout;
   const struct pvr_sh_reg_layout *sh_reg_layout =
      &pipeline_layout->sh_reg_layout_per_stage[PVR_STAGE_ALLOCATION_FRAGMENT];

   if (!sh_reg_layout->blend_consts.present) {
      nir_ssa_def_rewrite_uses(def, nir_imm_vec4(b, v[0], v[1], v[2], v[3]));
      nir_instr_remove(&intr->instr);
      return true;
   }

   /* Yes, this is reused between FS/CS. */
   nir_ssa_def *consts_base = nir_load_num_workgroups_base_addr_img(b);

   nir_ssa_def *vals[4];
   for (unsigned c = 0; c < 4; ++c) {
      vals[c] =
         nir_f2f32(b,
                   nir_load_global_constant(b,
                                            nir_iadd_imm(b, consts_base, 4 * c),
                                            4,
                                            1,
                                            32));
   }

   nir_ssa_def_rewrite_uses(&intr->dest.ssa, nir_vec(b, vals, 4));
   nir_instr_remove(&intr->instr);
   return true;
}

static void rogue_lower_blend(nir_shader *shader, rogue_build_ctx *ctx)
{
   struct rogue_fs_build_data *fs_data = &ctx->stage_data.fs;
   const struct vk_color_blend_state *cb = fs_data->cb_state;

   nir_lower_blend_options opts = {
      .scalar_blend_const = false,
      .logicop_enable = cb->logic_op_enable,
      .logicop_func = vk_logic_op_to_pipe(cb->logic_op),
   };

   memset(opts.format, 0, sizeof(*opts.format));

   for (int i = 0; i < MIN2(fs_data->num_outputs, cb->attachment_count); i++) {
      const struct vk_color_blend_attachment_state *rt = &cb->attachments[i];
      opts.format[i] = fs_data->outputs[i].format;

      if (cb->logic_op_enable) {
         /* No blending, but we get the colour mask below */
      } else if (!rt->blend_enable) {
         static const nir_lower_blend_channel replace = {
            .func = PIPE_BLEND_ADD,
            .src_factor = PIPE_BLENDFACTOR_ONE,
            .dst_factor = PIPE_BLENDFACTOR_ZERO,
         };

         opts.rt[i].rgb = replace;
         opts.rt[i].alpha = replace;
      } else {
         opts.rt[i].rgb.func = vk_blend_op_to_pipe(rt->color_blend_op);
         opts.rt[i].rgb.src_factor =
            vk_blend_factor_to_pipe(rt->src_color_blend_factor);
         opts.rt[i].rgb.dst_factor =
            vk_blend_factor_to_pipe(rt->dst_color_blend_factor);

         opts.rt[i].alpha.func = vk_blend_op_to_pipe(rt->alpha_blend_op);
         opts.rt[i].alpha.src_factor =
            vk_blend_factor_to_pipe(rt->src_alpha_blend_factor);
         opts.rt[i].alpha.dst_factor =
            vk_blend_factor_to_pipe(rt->dst_alpha_blend_factor);
      }

      opts.rt[i].colormask =
         (cb->color_write_enables & BITFIELD_BIT(i)) ? rt->write_mask : 0;
   }

   NIR_PASS_V(shader, nir_lower_blend, &opts);

   NIR_PASS_V(shader, nir_opt_dce);

   nir_shader_instructions_pass(shader,
                                lower_blend_consts,
                                nir_metadata_block_index |
                                   nir_metadata_dominance,
                                ctx);
}

PUBLIC
bool rogue_nir_pfo(nir_shader *shader, rogue_build_ctx *ctx)
{
   /* Only apply to fragment shaders. */
   if (shader->info.stage != MESA_SHADER_FRAGMENT)
      return false;

   rogue_lower_blend(shader, ctx);

   nir_shader_lower_instructions(shader,
                                 is_output_io,
                                 lower_output_io,
                                 &ctx->stage_data.fs);
   return true;
}
