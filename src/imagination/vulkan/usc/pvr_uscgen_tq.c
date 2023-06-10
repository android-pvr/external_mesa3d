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

#include <assert.h>
#include <stdint.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_conversion_builder.h"
#include "nir_format_convert.h"
#include "pvr_debug.h"
#include "pvr_uscgen.h"
#include "rogue/rogue.h"
#include "rogue/rogue_builder.h"
#include "util/u_dynarray.h"

static bool needs_packing(enum pvr_transfer_pbe_pixel_src format)
{
   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW64:
   case PVR_TRANSFER_PBE_PIXEL_SRC_F32X2:
   case PVR_TRANSFER_PBE_PIXEL_SRC_MOV_BY45:
   case PVR_TRANSFER_PBE_PIXEL_SRC_D32S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_CONV_D24_D32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_CONV_D32U_D32F:
   case PVR_TRANSFER_PBE_PIXEL_SRC_CONV_D32_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_F32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SWAP_LMSB:
   case PVR_TRANSFER_PBE_PIXEL_SRC_CONV_S8D24_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_S8D24:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW128:
   case PVR_TRANSFER_PBE_PIXEL_SRC_F32X4:
      return false;
   default:
      break;
   }
   return true;
}

static bool needs_conversion(enum pvr_transfer_pbe_pixel_src format)
{
   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_CONV_D24_D32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_CONV_D32U_D32F:
   case PVR_TRANSFER_PBE_PIXEL_SRC_CONV_D32_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D32_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D32U_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SMRG_D24S8_D32S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SWAP_LMSB:
   case PVR_TRANSFER_PBE_PIXEL_SRC_CONV_S8D24_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_MOV_BY45:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SMRG_D32S8_D32S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_Y_UV_INTERLEAVED:
   case PVR_TRANSFER_PBE_PIXEL_SRC_YVU_PACKED:
   case PVR_TRANSFER_PBE_PIXEL_SRC_Y_U_V:
   case PVR_TRANSFER_PBE_PIXEL_SRC_YUV_PACKED:
      return true;
   default:
      break;
   }
   return false;
}

static void
int_format_signs(enum pvr_transfer_pbe_pixel_src format, bool *src, bool *dst)
{
   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU16U16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US16S16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US32S32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_U4XS32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_UU1010102:
      *src = false;
      break;
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SS8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU16U16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SS16S16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU32U32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_S4XU32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_SU1010102:
      *src = true;
      break;
   default:
      unreachable("Invalid format");
   }

   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU16U16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU16U16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU32U32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_S4XU32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_UU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_SU1010102:
      *dst = false;
      break;
   case PVR_TRANSFER_PBE_PIXEL_SRC_US8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SS8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US16S16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SS16S16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US32S32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_U4XS32:
      *dst = true;
      break;
   default:
      unreachable("Invalid format");
   }
}

static nir_ssa_def *
picked_component(nir_builder *b, nir_ssa_def *src, unsigned base_sh)
{
   nir_variable *pos = nir_get_variable_with_location(b->shader,
                                                      nir_var_shader_in,
                                                      VARYING_SLOT_POS,
                                                      glsl_vec4_type());
   nir_ssa_def *coord_x = nir_f2i32(b, nir_channel(b, nir_load_var(b, pos), 0));
   nir_ssa_def *offset = nir_load_preamble(b, 1, 32, .base = base_sh + 1);
   nir_ssa_def *mask = nir_load_preamble(b, 1, 32, .base = base_sh);
   nir_ssa_def *comp_idx = nir_iand(b, nir_isub(b, coord_x, offset), mask);
   nir_ssa_def *shift_val = nir_imul_imm(b, comp_idx, 8);
   return nir_ushr(b, src, shift_val);
}

static nir_ssa_def *pack_int_value(nir_builder *b,
                                   unsigned base_sh,
                                   bool pick_component,
                                   nir_ssa_def *src,
                                   enum pvr_transfer_pbe_pixel_src format)
{
   unsigned src_num_components = 4;
   const unsigned bits_8[] = { 8, 8, 8, 8 };
   const unsigned bits_10[] = { 10, 10, 10, 2 };
   const unsigned bits_16[] = { 16, 16, 16, 16 };
   const unsigned bits_32[] = { 32, 32, 32, 32 };
   const unsigned *bits;
   bool src_signed, dst_signed;
   int_format_signs(format, &src_signed, &dst_signed);

   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SS8888:
      bits = bits_8;
      break;
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU16U16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US16S16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU16U16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SS16S16:
      bits = bits_16;
      break;
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU32U32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_S4XU32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US32S32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_U4XS32:
      bits = bits_32;
      break;
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_UU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_SU1010102:
      bits = bits_10;
      break;
   default:
      unreachable("Invalid format");
   }

   if (format == PVR_TRANSFER_PBE_PIXEL_SRC_SU32U32 ||
       format == PVR_TRANSFER_PBE_PIXEL_SRC_US32S32) {
      src_num_components = 2;
   }

   if (format == PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_UU1010102 ||
       format == PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_SU1010102) {
      unsigned swiz[] = { 2, 1, 0, 3 };
      src = nir_swizzle(b, src, swiz, 4);
   }

   if (src_signed != dst_signed) {
      src = nir_convert_with_rounding(b,
                                      src,
                                      src_signed ? nir_type_int : nir_type_uint,
                                      dst_signed ? nir_type_int32
                                                 : nir_type_uint32,
                                      nir_rounding_mode_undef,
                                      true);
   }

   if (dst_signed)
      src = nir_format_clamp_sint(b, src, bits);
   else
      src = nir_format_clamp_uint(b, src, bits);
   if ((bits[0] < 32) && dst_signed)
      src = nir_format_mask_uvec(b, src, bits);

   if (bits != bits_16) {
      src = nir_format_pack_uint(b, src, bits, src_num_components);
   } else {
      src =
         nir_vec2(b,
                  nir_format_pack_uint(b, nir_channels(b, src, 0x3), bits, 2),
                  nir_format_pack_uint(b, nir_channels(b, src, 0xc), bits, 2));
   }

   if (!pick_component)
      return src;

   return picked_component(b, src, base_sh);
}

static nir_ssa_def *
pvr_uscgen_tq_frag_pack(nir_builder *b,
                        unsigned base_sh,
                        bool pick_component,
                        nir_ssa_def *src,
                        enum pvr_transfer_pbe_pixel_src format)
{
   if (!needs_packing(format))
      return src;

   /* Integer packing */
   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU16U16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US16S16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SS8888:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU16U16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SS16S16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU32U32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_S4XU32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_US32S32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_U4XS32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_UU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_UU1010102:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RBSWAP_SU1010102:
      return pack_int_value(b, base_sh, pick_component, src, format);

   case PVR_TRANSFER_PBE_PIXEL_SRC_F16F16:
      return nir_vec2(b,
                      nir_pack_half_2x16(b, nir_channels(b, src, 0x3)),
                      nir_pack_half_2x16(b, nir_channels(b, src, 0xc)));
   case PVR_TRANSFER_PBE_PIXEL_SRC_U16NORM:
      return nir_vec2(b,
                      nir_pack_unorm_2x16(b, nir_channels(b, src, 0x3)),
                      nir_pack_unorm_2x16(b, nir_channels(b, src, 0xc)));
   case PVR_TRANSFER_PBE_PIXEL_SRC_S16NORM:
      return nir_vec2(b,
                      nir_pack_snorm_2x16(b, nir_channels(b, src, 0x3)),
                      nir_pack_snorm_2x16(b, nir_channels(b, src, 0xc)));
   case PVR_TRANSFER_PBE_PIXEL_SRC_F16_U8:
      return nir_pack_unorm_4x8(b, src);

   default:
      unreachable("Unimplemented pvr_transfer_pbe_pixel_src");
   }
}

static bool uses_int_resolve(enum pvr_transfer_pbe_pixel_src format)
{
   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_F32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_F16F16:
   case PVR_TRANSFER_PBE_PIXEL_SRC_F16_U8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D32S8_D32S8:
      return false;
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW64:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SMRG_D24S8_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D24S8_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D32U_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SWAP_LMSB:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SMRG_D32S8_D32S8:
      return true;
   default:
      unreachable("Unsupported pvr_transfer_pbe_pixel_src");
   }
   return false;
}

static void prepare_samples_for_resolve(nir_builder *b,
                                        nir_ssa_def **samples,
                                        unsigned num_samples,
                                        enum pvr_transfer_pbe_pixel_src format,
                                        enum pvr_resolve_op resolve_op)
{
   unsigned num_components;

   if (resolve_op == PVR_RESOLVE_MIN || resolve_op == PVR_RESOLVE_MAX) {
      if (format != PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D24S8_D24S8)
         return;

      /* Mask out the stencil component since it is in the significant bits */
      for (unsigned i = 0; i < num_samples; i++)
         samples[i] = nir_iand_imm(b, samples[i], BITFIELD_MASK(24));

      return;
   }

   assert(resolve_op == PVR_RESOLVE_BLEND);

   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_SMRG_D24S8_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SWAP_LMSB:
      /* Mask out depth and convert to f32 */
      for (unsigned i = 0; i < num_samples; i++) {
         samples[i] = nir_ushr_imm(b, samples[i], 24);
         samples[i] = nir_u2f32(b, nir_channel(b, samples[i], 0));
      }
      return;

   case PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D24S8_D24S8:
      /* Mask out stencil and convert to f32 */
      for (unsigned i = 0; i < num_samples; i++) {
         samples[i] = nir_iand_imm(b, samples[i], ~BITFIELD_RANGE(24, 8));
         samples[i] = nir_u2f32(b, nir_channel(b, samples[i], 0));
      }
      return;

   case PVR_TRANSFER_PBE_PIXEL_SRC_F32:
   case PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D32S8_D32S8:
      num_components = 1;
      break;
   case PVR_TRANSFER_PBE_PIXEL_SRC_F32X2:
      num_components = 2;
      break;
   default:
      assert(pvr_pbe_pixel_is_norm(format));
      num_components = 4;
      break;
   }

   for (unsigned i = 0; i < num_samples; i++)
      samples[i] = nir_trim_vector(b, samples[i], num_components);
}

static nir_ssa_def *post_process_resolve(nir_builder *b,
                                         nir_ssa_def *src,
                                         enum pvr_transfer_pbe_pixel_src format,
                                         enum pvr_resolve_op resolve_op)
{
   unsigned bits;

   if (resolve_op != PVR_RESOLVE_BLEND)
      return src;

   switch (format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_SMRG_D24S8_D24S8:
   case PVR_TRANSFER_PBE_PIXEL_SRC_SWAP_LMSB:
      /* Convert back to unorm and shift back to correct place */
      bits = 8;
      assert(src->num_components == 1);
      src = nir_format_float_to_unorm(b, src, &bits);
      return nir_ishl_imm(b, src, 24);

   case PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_D24S8_D24S8:
      /* Convert back to unorm */
      bits = 24;
      assert(src->num_components == 1);
      return nir_format_float_to_unorm(b, src, &bits);

   default:
      break;
   }

   return src;
}

static nir_ssa_def *resolve_samples(nir_builder *b,
                                    nir_ssa_def **samples,
                                    unsigned num_samples,
                                    enum pvr_transfer_pbe_pixel_src format,
                                    enum pvr_resolve_op resolve_op)
{
   nir_ssa_def *accum = NULL;
   nir_ssa_def *coeff = NULL;
   nir_op op;

   switch (resolve_op) {
   case PVR_RESOLVE_BLEND:
      op = nir_op_ffma;
      coeff = nir_imm_float(b, 1.0 / num_samples);
      break;

   case PVR_RESOLVE_MIN:
      op = uses_int_resolve(format) ? nir_op_imin : nir_op_fmin;
      break;

   case PVR_RESOLVE_MAX:
      op = uses_int_resolve(format) ? nir_op_imax : nir_op_fmax;
      break;

   default:
      unreachable("Unsupported pvr_transfer_pbe_pixel_src");
   }

   prepare_samples_for_resolve(b, samples, num_samples, format, resolve_op);

   accum = samples[0];
   for (unsigned i = 1; i < num_samples; i++) {
      if (resolve_op == PVR_RESOLVE_BLEND)
         accum = nir_ffma(b, samples[i], coeff, accum);
      else
         accum = nir_build_alu2(b, op, samples[i], accum);
   }

   return post_process_resolve(b, accum, format, resolve_op);
}

static nir_ssa_def *
pvr_uscgen_tq_frag_conv(nir_builder *b,
                        nir_ssa_def *src,
                        enum pvr_transfer_pbe_pixel_src format)
{
   if (!needs_conversion(format))
      return src;

   unreachable("Not implemented yet!");
}

static nir_ssa_def *pvr_uscgen_tq_frag_load_sw_filter(
   nir_builder *b,
   uint32_t load_idx,
   nir_ssa_def *coords,
   const struct pvr_tq_shader_properties *shader_props,
   struct pvr_tq_frag_sh_reg_layout *sh_reg_layout)
{
   unreachable("Not implemented yet!");
}

static nir_ssa_def *
pvr_uscgen_tq_frag_load(nir_builder *b,
                        uint32_t load_idx,
                        nir_ssa_def *coords,
                        const struct pvr_tq_shader_properties *shader_props,
                        struct pvr_tq_frag_sh_reg_layout *sh_reg_layout)
{
   const struct pvr_tq_layer_properties *layer_props =
      &shader_props->layer_props;

   const unsigned num_samples = (shader_props->full_rate || !layer_props->msaa)
                                   ? 1
                                   : layer_props->sample_count;

   nir_ssa_def *samples[PVR_MAX_SAMPLE_COUNT];

   for (unsigned sample_idx = 0; sample_idx < num_samples; sample_idx++) {
      nir_tex_instr *tex;

      tex = nir_tex_instr_create(b->shader, !!layer_props->msaa + 1);
      tex->src[0].src_type = nir_tex_src_coord;
      tex->src[0].src = nir_src_for_ssa(coords);

      if (layer_props->msaa) {
         unsigned resolved_idx = layer_props->resolve_op - PVR_RESOLVE_SAMPLE0;
         nir_ssa_def *ms_idx;

         if (shader_props->full_rate) {
            ms_idx = nir_load_sample_id(b);
            b->shader->info.fs.uses_sample_shading = true;
         } else if (layer_props->resolve_op >= PVR_RESOLVE_SAMPLE0) {
            ms_idx = nir_imm_int(b, resolved_idx);
         } else {
            ms_idx = nir_imm_int(b, sample_idx);
         }

         tex->src[1].src_type = nir_tex_src_ms_index;
         tex->src[1].src = nir_src_for_ssa(ms_idx);
      }

      tex->dest_type = pvr_pbe_pixel_is_norm(layer_props->pbe_format)
                          ? nir_type_float32
                          : nir_type_uint32;

      tex->coord_components = layer_props->sample ? 3 : 2;

      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      if (layer_props->msaa)
         tex->sampler_dim = GLSL_SAMPLER_DIM_MS;
      else if (layer_props->sample)
         tex->sampler_dim = GLSL_SAMPLER_DIM_3D;

      assert(load_idx < sh_reg_layout->combined_image_samplers.count);
      tex->texture_index =
         sh_reg_layout->combined_image_samplers.offsets[load_idx].image;
      tex->sampler_index =
         sh_reg_layout->combined_image_samplers.offsets[load_idx].sampler;

      nir_ssa_dest_init(&tex->instr, &tex->dest, 4, 32);
      nir_builder_instr_insert(b, &tex->instr);

      samples[sample_idx] = &tex->dest.ssa;
   }

   if (num_samples == 1)
      return samples[0];

   return resolve_samples(b,
                          samples,
                          num_samples,
                          layer_props->pbe_format,
                          layer_props->resolve_op);
}

static nir_ssa_def *
pvr_uscgen_tq_frag_coords(nir_builder *b,
                          unsigned *next_sh,
                          const struct pvr_tq_shader_properties *shader_props,
                          struct pvr_tq_frag_sh_reg_layout *sh_reg_layout)
{
   const struct pvr_tq_layer_properties *layer_props =
      &shader_props->layer_props;
   unsigned base_sh = sh_reg_layout->dynamic_consts.offset;
   bool varying = shader_props->iterated;
   unsigned location = varying ? VARYING_SLOT_VAR0 : VARYING_SLOT_POS;
   const struct glsl_type *var_type = glsl_vec_type(varying ? 2 : 4);
   nir_variable *pos = nir_get_variable_with_location(b->shader,
                                                      nir_var_shader_in,
                                                      location,
                                                      var_type);
   nir_ssa_def *coords =
      nir_channels(b, nir_load_var(b, pos), nir_component_mask(2));

   assert(layer_props->layer_floats != PVR_INT_COORD_SET_FLOATS_6);
   if (!varying && layer_props->layer_floats == PVR_INT_COORD_SET_FLOATS_4) {
      /* coords.xy = coords.xy * (sh[0], sh[2]) + (sh[1], s[3]) */
      nir_ssa_def *mult =
         nir_vec2(b,
                  nir_load_preamble(b, 1, 32, .base = *next_sh + base_sh),
                  nir_load_preamble(b, 1, 32, .base = *next_sh + base_sh + 2));
      nir_ssa_def *add =
         nir_vec2(b,
                  nir_load_preamble(b, 1, 32, .base = *next_sh + base_sh + 1),
                  nir_load_preamble(b, 1, 32, .base = *next_sh + base_sh + 3));
      coords = nir_fmad(b, coords, mult, add);
      *next_sh += 4;
   }

   /* 3D texture, the depth comes from shared regs */
   if (layer_props->sample) {
      nir_ssa_def *depth =
         nir_load_preamble(b, 1, 32, .base = *next_sh + base_sh);

      coords = nir_pad_vector(b, coords, 3);
      coords = nir_vector_insert_imm(b, coords, depth, 2);
      (*next_sh)++;
   }

   return coords;
}

static void
pvr_uscgen_tq_frag_nir(const struct pvr_device *device,
                       const struct pvr_tq_shader_properties *shader_props,
                       struct pvr_tq_frag_sh_reg_layout *sh_reg_layout,
                       unsigned *temps_used,
                       struct util_dynarray *binary)
{
   unsigned base_sh = sh_reg_layout->dynamic_consts.offset;
   const struct pvr_tq_layer_properties *layer_props =
      &shader_props->layer_props;
   unsigned next_sh = 0;
   uint32_t loads;

   struct pvr_pipeline_layout pipeline_layout = { 0 };
   struct rogue_build_ctx *rogue_ctx =
      rogue_build_context_create(device->pdevice->compiler, &pipeline_layout);

   struct rogue_fs_build_data *fs_data = &rogue_ctx->stage_data.fs;

   unsigned pixel_size = pvr_pbe_pixel_size(layer_props->pbe_format);
   struct usc_mrt_resource mrt_resource = {
      .intermediate_size = PVR_DW_TO_BYTES(pixel_size),
      .type = USC_MRT_RESOURCE_TYPE_OUTPUT_REG,
      .reg = { .output_reg = 0, .offset = 0 },
   };

   unsigned shareds_used;
   rogue_shader *shader;

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  rogue_nir_options(),
                                                  "TQ (fragment)");

   /* TODO: Unrestrict. */
   assert(layer_props->layer_floats != PVR_INT_COORD_SET_FLOATS_6);
   assert(layer_props->byte_unwind == 0);
   assert(layer_props->linear == false);

   loads = pvr_pbe_pixel_num_loads(layer_props->pbe_format);
   /* TODO: Unrestrict. */
   assert(loads == 1);

   fs_data->num_outputs = loads;
   fs_data->outputs =
      rzalloc_array_size(rogue_ctx, sizeof(*fs_data->outputs), loads);

   fs_data->outputs[0].format = pvr_uscgen_raw_pipe_format(pixel_size);
   fs_data->outputs[0].accum_format = PVR_PBE_ACCUM_FORMAT_UINT32;
   fs_data->outputs[0].mrt_resource = &mrt_resource;

   for (uint32_t load = 0; load < loads; load++) {
      nir_ssa_def *loaded_data;
      nir_ssa_def *coords =
         pvr_uscgen_tq_frag_coords(&b, &next_sh, shader_props, sh_reg_layout);

      assert(!layer_props->linear || !layer_props->msaa);

      if (layer_props->linear) {
         loaded_data = pvr_uscgen_tq_frag_load_sw_filter(&b,
                                                         load,
                                                         coords,
                                                         shader_props,
                                                         sh_reg_layout);
      } else {
         loaded_data = pvr_uscgen_tq_frag_load(&b,
                                               load,
                                               coords,
                                               shader_props,
                                               sh_reg_layout);
      }

      loaded_data =
         pvr_uscgen_tq_frag_conv(&b, loaded_data, layer_props->pbe_format);

      loaded_data = pvr_uscgen_tq_frag_pack(&b,
                                            next_sh + base_sh,
                                            shader_props->pick_component,
                                            loaded_data,
                                            layer_props->pbe_format);

      nir_store_output(&b,
                       nir_resize_vector(&b, loaded_data, pixel_size),
                       nir_imm_int(&b, 0),
                       .base = 0,
                       .src_type = nir_type_uint32,
                       .write_mask = BITFIELD_MASK(pixel_size),
                       .io_semantics.location = FRAG_RESULT_DATA0 + load,
                       .io_semantics.num_slots = 1);

      b.shader->info.outputs_written |=
         BITFIELD64_BIT(FRAG_RESULT_DATA0 + load);
   }

   pvr_collect_io_data_fs(&rogue_ctx->common_data[MESA_SHADER_FRAGMENT],
                          fs_data,
                          b.shader,
                          true);

   shader = rogue_nir_compile(rogue_ctx, b.shader);
   rogue_set_shader_name(shader, "NIR load_op");

   rogue_encode_shader(NULL, shader, binary);

   shareds_used = rogue_count_used_regs(shader, ROGUE_REG_CLASS_SHARED);
   assert(shareds_used >= sh_reg_layout->driver_total);
   sh_reg_layout->dynamic_consts.count =
      shareds_used - sh_reg_layout->driver_total;
   sh_reg_layout->compiler_out_total = 0;
   sh_reg_layout->compiler_out.usc_constants.count = 0;

   *temps_used = rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   ralloc_free(b.shader);
   ralloc_free(rogue_ctx);
}

void pvr_uscgen_tq_frag(const struct pvr_device *device,
                        const struct pvr_tq_shader_properties *shader_props,
                        struct pvr_tq_frag_sh_reg_layout *sh_reg_layout,
                        unsigned *temps_used,
                        struct util_dynarray *binary)
{
   if (PVR_IS_DEBUG_SET(TQ_NIR)) {
      pvr_uscgen_tq_frag_nir(device,
                             shader_props,
                             sh_reg_layout,
                             temps_used,
                             binary);
      return;
   }

   rogue_builder b;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);

   unsigned smp_coord_size = 2;
   unsigned smp_coord_idx = 0;
   rogue_ref64 smp_coords = rogue_ssa_ref64(shader, smp_coord_idx);

   unsigned channels = 0;
   unsigned output_idx = 1;
   rogue_regarray *outputs = NULL;

   unsigned image_state_size = 4;
   unsigned image_state_idx;
   rogue_regarray *image_state;

   unsigned smp_state_size = 4;
   unsigned smp_state_idx;
   rogue_regarray *smp_state;

   rogue_set_shader_name(shader, "TQ (fragment)");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   /* TODO: Unrestrict. */
   assert(shader_props->full_rate == false);
   assert(shader_props->pick_component == false);

   const struct pvr_tq_layer_properties *layer_props =
      &shader_props->layer_props;
   uint32_t loads;

   /* TODO: Unrestrict. */
   assert(layer_props->msaa == false);
   assert(layer_props->sample_count == 1U);
   assert(layer_props->resolve_op == PVR_RESOLVE_BLEND);
   assert(layer_props->pbe_format == PVR_TRANSFER_PBE_PIXEL_SRC_RAW64 ||
          layer_props->pbe_format == PVR_TRANSFER_PBE_PIXEL_SRC_RAW128);
   assert(layer_props->sample == false);
   assert(layer_props->layer_floats == PVR_INT_COORD_SET_FLOATS_0);
   assert(layer_props->byte_unwind == 0);
   assert(layer_props->linear == false);

   loads = pvr_pbe_pixel_num_loads(layer_props->pbe_format);
   for (uint32_t load = 0; load < loads; ++load) {
      if (shader_props->iterated) {
         /* TODO: feed{back,forward} the coeff index to/from shader_info. */
         unsigned coeff_index = 0;
         rogue_regarray *coeffs =
            rogue_coeff_regarray(b.shader, smp_coord_size * 4, coeff_index);

         rogue_instr *instr = &rogue_FITR_PIXEL(&b,
                                                smp_coords.ref64,
                                                rogue_ref_drc(0),
                                                rogue_ref_regarray(coeffs),
                                                rogue_ref_val(smp_coord_size))
                                  ->instr;
         rogue_add_instr_comment(instr, "load_iterated");
      } else {
         rogue_instr *instr;
         rogue_regarray *smp_coord_x =
            rogue_ssa_vec_regarray(b.shader, 1, smp_coord_idx, 0);
         rogue_regarray *smp_coord_y =
            rogue_ssa_vec_regarray(b.shader, 1, smp_coord_idx, 1);

         /* (X,Y).P, pixel (X,Y) coordinates, pixel mode. */
         rogue_reg *in_x = rogue_special_reg(b.shader, 97);
         rogue_reg *in_y = rogue_special_reg(b.shader, 100);

         instr =
            &rogue_MOV(&b, rogue_ref_regarray(smp_coord_x), rogue_ref_reg(in_x))
                ->instr;
         rogue_add_instr_comment(instr, "load_x");

         instr =
            &rogue_MOV(&b, rogue_ref_regarray(smp_coord_y), rogue_ref_reg(in_y))
                ->instr;
         rogue_add_instr_comment(instr, "load_y");
      }

      if (!layer_props->msaa) {
      } else {
         unreachable("Unsupported layer property (MSAA).");
      }
   }

   /* Source conversion. */
   switch (layer_props->pbe_format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW64:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW128:
      break;

   default:
      unreachable("Unsupported layer property (format).");
   }

   /* TODO: Select the texture_regs index appropriately. */
   assert(sh_reg_layout->combined_image_samplers.count == 1);
   image_state_idx = sh_reg_layout->combined_image_samplers.offsets[0].image;
   image_state =
      rogue_shared_regarray(b.shader, image_state_size, image_state_idx);

   smp_state_idx = sh_reg_layout->combined_image_samplers.offsets[0].sampler;
   smp_state = rogue_shared_regarray(b.shader, smp_state_size, smp_state_idx);

   /* Pack/blend phase. */
   rogue_backend_instr *smp2d;

   switch (layer_props->pbe_format) {
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW64:
   case PVR_TRANSFER_PBE_PIXEL_SRC_RAW128: {
      switch (layer_props->pbe_format) {
      case PVR_TRANSFER_PBE_PIXEL_SRC_RAW64:
         channels = 2;
         break;

      case PVR_TRANSFER_PBE_PIXEL_SRC_RAW128:
         channels = 4;
         break;

      default:
         unreachable("Unsupported layer property (format).");
      }

      outputs = rogue_ssa_vec_regarray(b.shader, channels, output_idx, 0);

      smp2d = rogue_SMP2D(&b,
                          rogue_ref_regarray(outputs),
                          rogue_ref_drc(0),
                          rogue_ref_regarray(image_state),
                          smp_coords.ref64,
                          rogue_ref_regarray(smp_state),
                          rogue_none(),
                          rogue_ref_val(channels));
      /* rogue_set_backend_op_mod(smp2d, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK); */
      rogue_add_instr_comment(&smp2d->instr, "pack/blend");

      if (!shader_props->iterated)
         rogue_set_backend_op_mod(smp2d, ROGUE_BACKEND_OP_MOD_NNCOORDS);
      break;
   }

   default:
      unreachable("Unsupported layer property (format).");
   }

   assert(channels && outputs);

   /* Copy outputs. */
   for (unsigned u = 0; u < channels; ++u) {
      rogue_regarray *output_elem =
         rogue_ssa_vec_regarray(b.shader, 1, output_idx, u);
      rogue_reg *pixout_elem = rogue_pixout_reg(b.shader, u);
      rogue_MOV(&b,
                rogue_ref_reg(pixout_elem),
                rogue_ref_regarray(output_elem));
   }

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   *temps_used = rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   sh_reg_layout->compiler_out.usc_constants.count = 0;
   sh_reg_layout->compiler_out_total = 0;

   ralloc_free(shader);
}

void pvr_uscgen_tq_eot(unsigned rt_count,
                       const uint64_t *pbe_regs,
                       struct util_dynarray *binary)
{
   rogue_builder b;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_set_shader_name(shader, "TQ (EOT)");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   rogue_backend_instr *emitpix = NULL;
   for (unsigned u = 0; u < rt_count; ++u) {
      if (u > 0)
         rogue_WOP(&b);

      rogue_reg *state_word_0 = rogue_shared_reg(shader, pbe_regs[u]);
      rogue_reg *state_word_1 = rogue_shared_reg(shader, pbe_regs[u] + 1);

      emitpix = rogue_EMITPIX(&b,
                              rogue_ref_reg(state_word_0),
                              rogue_ref_reg(state_word_1));
   }

   assert(emitpix);

   rogue_set_backend_op_mod(emitpix, ROGUE_BACKEND_OP_MOD_FREEP);
   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   ralloc_free(shader);
}
