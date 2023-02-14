/*
 * Copyright © 2023 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"
#include "rogue.h"
#include "rogue_nir_fmt_utils.h"

/**
 * \file rogue_nir_lower_images_to_tex.c
 *
 * \brief Contains the rogue_nir_lower_images_to_tex pass.
 */

static bool
is_undef(nir_def *def)
{
   if (!def)
      return true;

   return def->parent_instr->type == nir_instr_type_undef;
}

#if 0
static const struct glsl_type *
get_texture_type_for_image(const struct glsl_type *type)
{
   if (glsl_type_is_array(type)) {
      const struct glsl_type *elem_type =
         get_texture_type_for_image(glsl_get_array_element(type));
      return glsl_array_type(elem_type, glsl_get_length(type), 0 /*explicit size*/);
   }

   assert((glsl_type_is_image(type)));
   return glsl_texture_type(glsl_get_sampler_dim(type),
                            glsl_sampler_type_is_array(type),
                            glsl_get_sampler_result_type(type));
}

static bool
replace_image_type_with_texture(nir_deref_instr *deref)
{
   const struct glsl_type *type = deref->type;

   /* If we've already chased up the deref chain this far from a different intrinsic, we're done */
   if (!glsl_type_is_image(glsl_without_array(type)))
      return false;

   deref->type = get_texture_type_for_image(type);
   deref->modes = nir_var_uniform;
   if (deref->deref_type == nir_deref_type_var) {
      type = deref->var->type;
      if (glsl_type_is_image(glsl_without_array(type))) {
         deref->var->type = get_texture_type_for_image(type);
         deref->var->data.mode = nir_var_uniform;
         memset(&deref->var->data.sampler, 0, sizeof(deref->var->data.sampler));
      }
   } else {
      nir_deref_instr *parent = nir_deref_instr_parent(deref);
      if (parent)
         replace_image_type_with_texture(parent);
   }

   return true;
}
#endif

static nir_texop image_instr_texop(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
      return nir_texop_txf;

   /* case nir_intrinsic_image_deref_samples: */
   /* case nir_intrinsic_image_deref_samples_identical: */

   case nir_intrinsic_image_deref_size:
      return nir_texop_txs;

   /* case nir_intrinsic_image_deref_sparse_load: */

   case nir_intrinsic_image_deref_store:
      return nir_texop_txw_img;

   case nir_intrinsic_image_deref_texel_address:
      return nir_texop_texel_address_img;

   default:
      break;
   }

   unreachable("Unsupported image intrinsic.");
}

struct image_instr_info {
   nir_def *deref;
   nir_def *coords;
   nir_def *write_data;
   nir_def *lod;
   unsigned num_srcs;

   unsigned sample_index;
   nir_alu_type type;
};

static void setup_tex_srcs(nir_tex_instr *tex,
                           const struct image_instr_info *info)
{
   unsigned src = 0;
   tex->src[src++] =
      nir_tex_src_for_ssa(nir_tex_src_texture_deref, info->deref);

   if (info->coords) {
      tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_coord, info->coords);
      tex->coord_components = info->coords->num_components;
   }

   if (info->write_data)
      tex->src[src++] =
         nir_tex_src_for_ssa(nir_tex_src_wrdata, info->write_data);

   if (info->lod)
      tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_lod, info->lod);
}

static enum util_format_type nir_type_to_util_type(nir_alu_type nir_type)
{
   switch (nir_alu_type_get_base_type(nir_type)) {
   case nir_type_int:
      return UTIL_FORMAT_TYPE_SIGNED;

   case nir_type_uint:
      return UTIL_FORMAT_TYPE_UNSIGNED;

   case nir_type_float:
      return UTIL_FORMAT_TYPE_FLOAT;

   default:
      break;
   }

   unreachable("Unsupported nir_alu_type.");
}

static enum pipe_format type_to_format(nir_alu_type nir_type)
{
   enum util_format_type format_type = nir_type_to_util_type(nir_type);
   unsigned bits = nir_alu_type_get_type_size(nir_type);
   bool pure_integer = format_type != UTIL_FORMAT_TYPE_FLOAT;

   /* imageLoad/imageStore always use 4 components. */
   return util_format_get_array(format_type, bits, 4, false, pure_integer);
}

static nir_def *lower_image_intrinsic(nir_builder *b,
                                      nir_intrinsic_instr *intr,
                                      struct image_instr_info *info)
{
   nir_deref_instr *deref = nir_src_as_deref(nir_src_for_ssa(info->deref));
   nir_variable *var = nir_deref_instr_get_variable(deref);
   assert(var);

   nir_tex_instr *tex = nir_tex_instr_create(b->shader, info->num_srcs);
   tex->op = image_instr_texop(intr);
   tex->sampler_dim = nir_intrinsic_image_dim(intr);
   tex->is_array = nir_intrinsic_image_array(intr);
   tex->is_shadow = false;
   tex->sampler_index = info->sample_index;
   tex->dest_type = info->type;
   unsigned data_format_bits = nir_alu_type_get_type_size(info->type);

   enum pipe_format image_format = var->data.image.format;
   enum pipe_format data_format = type_to_format(info->type);
   bool equal_types = image_format == data_format;

   bool store = intr->intrinsic == nir_intrinsic_image_deref_store;
   if (store && !equal_types) {
      /* TODO: support other types. */
      /* assert(data_format == PIPE_FORMAT_R32G32B32A32_FLOAT); */

      assert(data_format_bits == 32);

      /* Pack data to required format. */
      const struct util_format_description *fmt_desc = util_format_description(image_format);

      unsigned fmt_chans = fmt_desc->nr_channels;

      /* Trim unused components. */
      /* TODO NEXT: either don't trim the unused components, or don't hardcode write_data to be 4 (preferably the latter). */
      /* TODO: Would be nice to make sure that the trimmed components are all nir_undef. */
      info->write_data = nir_trim_vector(b, info->write_data, fmt_chans);

      nir_def *write_data_packed[4] = { nir_imm_int(b, 0), nir_imm_int(b, 0),
                                        nir_imm_int(b, 0), nir_imm_int(b, 0) };

      /* TODO: commonise this with all the similar code in pvfio/fmt_utils,
       * and probably want vector versions of the scalar pack/unpacks.
       */
      for (unsigned c = 0; c < fmt_chans; ++c) {
         enum pipe_swizzle chan = fmt_desc->swizzle[c];
         assert(chan < fmt_chans);
         assert(chan <= PIPE_SWIZZLE_W);

         const struct util_format_channel_description *chan_desc = &fmt_desc->channel[chan];

         unsigned chan_offset = chan_desc->shift;
         unsigned chan_bits = chan_desc->size;
         /* No support for 64-bit components (yet). */
         assert(chan_bits <= 32);

         /* Texture output register offset that contains this channel. */
         ASSERTED unsigned texout_offset = chan_offset / ROGUE_REG_SIZE_BITS;

         /* Make sure the channel doesn't span multiple registers. */
         assert(texout_offset == ((chan_offset + chan_bits - 1) / ROGUE_REG_SIZE_BITS));

         /* N.B. for samplers it's one register PER CHANNEL, i.e. don't pack multiple channels into a single register.
          * The SMP instruction will take care of the packing.
          */

         /* Prepare store arguments. */
         nir_def **write_data_out = &write_data_packed[chan]; /* TODO check this - maybe it needs to be c as well and we don't need to swizzle? */
         nir_def *write_data_in = nir_channel(b, info->write_data, c);

         unsigned num_texout_regs = PVR_BITS_TO_DW(fmt_desc->block.bits);
         /* Adjust the channel component.
          * E.g. for [R16G16][B16A16]:
          * var.r => [0].x
          * var.g => [0].y
          * var.b => [1].x
          * var.a => [1].y
          */
         assert(fmt_chans >= num_texout_regs);
         chan %= DIV_ROUND_UP(fmt_chans, num_texout_regs);

         /* Transform and pack the value to be stored. */
         /* value = fmt_colorspace_transform_scalar(b, value, bit_size, chan, fmt_desc, true); */
         *write_data_out = fmt_pack_scalar(b, *write_data_out, write_data_in, data_format_bits, nir_imm_int(b, chan), chan_desc, true);
      }

      info->write_data = nir_vec(b, write_data_packed, fmt_desc->nr_channels);
      info->write_data = nir_pad_vector(b, info->write_data, 4);
   }

   setup_tex_srcs(tex, info);

#if 0
   enum gl_access_qualifier access = nir_intrinsic_access(intr);

   if (access & ACCESS_CAN_REORDER || access & ACCESS_COHERENT) {
      tex->backend_flags = 1;
   }
#endif

#if 0
   replace_image_type_with_texture(nir_src_as_deref(nir_src_for_ssa(info->deref)));
#endif

   /* If we're getting a texel address, ensure that the format is a 32-bit integer, can't work with anything else. */
   if (intr->intrinsic == nir_intrinsic_image_deref_texel_address) {
      nir_binding image_binding = nir_chase_binding(nir_src_for_ssa(info->deref));
      assert(image_binding.success);

      nir_variable *image_var = nir_get_binding_variable(b->shader, image_binding);
      assert(image_var);
      assert(image_var->data.mode & nir_var_image);

      enum pipe_format image_fmt = image_var->data.image.format;
      assert(util_format_is_plain(image_fmt));
      assert(util_format_is_pure_integer(image_fmt));

      assert(util_format_get_nr_components(image_fmt) == 1);
      assert(util_format_get_blockwidth(image_fmt) == 1);
      assert(util_format_get_blockheight(image_fmt) == 1);
      assert(util_format_get_blockdepth(image_fmt) == 1);
      assert(util_format_get_blocksizebits(image_fmt) == 32);

#if 0
      /* TODO: above but better */
      const struct util_format_description *fmt_desc = util_format_description(image_fmt);
      assert(fmt_desc->block.width == 1);
      assert(fmt_desc->block.height == 1);
      assert(fmt_desc->block.depth == 1);
      assert(fmt_desc->block.bits == 32);
      assert(fmt_desc->layout == UTIL_FORMAT_LAYOUT_PLAIN);
#endif
   }

   nir_def_init(&tex->instr, &tex->def, nir_tex_instr_dest_size(tex), data_format_bits);
   nir_builder_instr_insert(b, &tex->instr);

   unsigned dst_comps = nir_intrinsic_dest_components(intr);
   if (store) {
      assert(!dst_comps);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   return nir_trim_vector(b, &tex->def, dst_comps);
}

static bool is_image_instr(const nir_instr *instr, UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_samples:
   /* case nir_intrinsic_image_deref_samples_identical: */
   case nir_intrinsic_image_deref_size:
   /* case nir_intrinsic_image_deref_sparse_load: */
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_texel_address:
      return true;

   default:
      break;
   }

   return false;
}

static bool image_instr_has_coords(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
   /* case nir_intrinsic_image_deref_samples_identical: */
   /* case nir_intrinsic_image_deref_sparse_load: */
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_texel_address:
      return true;

   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
      return false;

   default:
      break;
   }

   unreachable("Unsupported image intrinsic.");
}

static unsigned image_instr_coord_components(const nir_intrinsic_instr *intr)
{
   enum glsl_sampler_dim image_dim = nir_intrinsic_image_dim(intr);

   /* TODO NEXT: make sure it's one we can handle! */

   bool is_image_array = nir_intrinsic_image_array(intr);

   if (!image_instr_has_coords(intr))
      return 0;

   unsigned coord_components = glsl_get_sampler_dim_coordinate_components(image_dim);
   if (is_image_array)
      ++coord_components;

   return coord_components;
}

static nir_def *image_instr_coords(nir_builder *b, const nir_intrinsic_instr *intr)
{
   /* Always at [1] if present. */
   nir_def *coords = intr->src[1].ssa;

   if (is_undef(coords))
      return NULL;

   unsigned coord_components = image_instr_coord_components(intr);
   if (!coord_components)
      return NULL;

   return nir_trim_vector(b, coords, coord_components);
}

/**/

static int image_instr_has_sample_index(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
   /* case nir_intrinsic_image_deref_sparse_load: */
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_texel_address:
      return true;

   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
   /* case nir_intrinsic_image_deref_samples_identical: */
      return false;

   default:
      break;
   }

   unreachable("Unsupported image intrinsic.");
}

static unsigned image_instr_sample_index(const nir_intrinsic_instr *intr)
{
   /* Always at [2] if present. */
   nir_def *sample_index = intr->src[2].ssa;

   if (is_undef(sample_index))
      return 0;

   if (!image_instr_has_sample_index(intr))
      return 0;

   return nir_src_as_uint(nir_src_for_ssa(sample_index));
}

/**/

static int image_instr_write_data_src_idx(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_store:
      return 3;

   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_samples:
   /* case nir_intrinsic_image_deref_samples_identical: */
   case nir_intrinsic_image_deref_size:
   /* case nir_intrinsic_image_deref_sparse_load: */
   case nir_intrinsic_image_deref_texel_address:
      return -1;

   default:
      break;
   }

   unreachable("Unsupported image intrinsic.");
}

static nir_def *image_instr_write_data(const nir_intrinsic_instr *intr)
{
   int write_data_src_idx = image_instr_write_data_src_idx(intr);
   if (write_data_src_idx < 0)
      return NULL;

   nir_def *write_data = intr->src[write_data_src_idx].ssa;
   if (is_undef(write_data))
      return NULL;

   return write_data;
}

/**/

static int image_instr_lod_src_idx(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_size:
      return 1;

   case nir_intrinsic_image_deref_load:
   /* case nir_intrinsic_image_deref_sparse_load: */
      return 3;

   case nir_intrinsic_image_deref_store:
      return 4;

   case nir_intrinsic_image_deref_samples:
   /* case nir_intrinsic_image_deref_samples_identical: */
   case nir_intrinsic_image_deref_texel_address:
      return -1;

   default:
      break;
   }

   unreachable("Unsupported image intrinsic.");
}

static nir_def *image_instr_lod(const nir_intrinsic_instr *intr)
{
   int lod_src_idx = image_instr_lod_src_idx(intr);
   if (lod_src_idx < 0)
      return NULL;

   nir_def *lod = intr->src[lod_src_idx].ssa;
   if (is_undef(lod))
      return NULL;

   return lod;
}

static void
setup_image_instr_info(nir_builder *b, const nir_intrinsic_instr *intr, struct image_instr_info *info)
{
   info->num_srcs = 0;

   info->deref = intr->src[0].ssa;
   ++info->num_srcs;

   info->coords = image_instr_coords(b, intr);
   if (info->coords)
      ++info->num_srcs;

   info->write_data = image_instr_write_data(intr);
   if (info->write_data)
      ++info->num_srcs;

   info->lod = image_instr_lod(intr);
   if (info->lod)
      ++info->num_srcs;

   info->sample_index = image_instr_sample_index(intr);

   if (nir_intrinsic_has_src_type(intr)) {
      info->type = nir_intrinsic_src_type(intr);
   } else if (nir_intrinsic_has_dest_type(intr)) {
      info->type = nir_intrinsic_dest_type(intr);
   } else {
      switch (intr->intrinsic) {
      case nir_intrinsic_image_deref_size:
         info->type = nir_type_uint32;
         break;

      case nir_intrinsic_image_deref_texel_address:
         info->type = nir_type_uint64;
         break;

      default:
         unreachable("Unsupported image intrinsic.");
      }
   }
}

static nir_def *lower_image_instr(nir_builder *b, nir_instr *instr, UNUSED void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   /* assert(nir_intrinsic_format(intr) == PIPE_FORMAT_NONE); */

   struct image_instr_info info;
   setup_image_instr_info(b, intr, &info);

   return lower_image_intrinsic(b, intr, &info);
}

bool
rogue_nir_lower_images_to_tex(nir_shader *shader)
{
   return nir_shader_lower_instructions(shader, is_image_instr, lower_image_instr, NULL);
}

