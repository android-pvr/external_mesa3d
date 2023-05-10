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

#include "pvr_job_transfer.h"
#include "pvr_private.h"
#include "pvr_uscgen.h"
#include "rogue/rogue.h"
#include "rogue/rogue_builder.h"
#include "util/bitscan.h"
#include "util/u_dynarray.h"
#include "vulkan/util/vk_format.h"

#include <stdbool.h>

/* Expects emit_count ROGUE_NUM_PBESTATE_STATE_WORDS entries */
void pvr_uscgen_eot(const char *name,
                    uint32_t emit_count,
                    const uint32_t *emit_state,
                    unsigned *temps_used,
                    struct util_dynarray *binary)
{
   rogue_builder b;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_reg *state_word_0 = rogue_temp_reg(shader, 0);
   rogue_reg *state_word_1 = rogue_temp_reg(shader, 1);
   rogue_backend_instr *emitpix = NULL;

   rogue_set_shader_name(shader, name);
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   for (unsigned u = 0; u < emit_count; u++) {
      if (u > 0)
         rogue_WOP(&b);

      rogue_MOV(&b, rogue_ref_reg(state_word_0), rogue_ref_imm(emit_state[0]));
      rogue_MOV(&b, rogue_ref_reg(state_word_1), rogue_ref_imm(emit_state[1]));

      emitpix = rogue_EMITPIX(&b,
                              rogue_ref_reg(state_word_0),
                              rogue_ref_reg(state_word_1));

      emit_state += 2;
   }

   assert(emitpix);

   rogue_set_backend_op_mod(emitpix, ROGUE_BACKEND_OP_MOD_FREEP);
   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   *temps_used = rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   ralloc_free(shader);
}

void pvr_uscgen_passthrough_vtx(struct util_dynarray *binary, bool rta)
{
   rogue_builder b;
   rogue_reg *dst;
   rogue_reg *src;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_VERTEX);
   rogue_set_shader_name(shader,
                         rta ? "passthrough vertex (RTA)"
                             : "passthrough vertex");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   for (unsigned u = 0; u < 3; ++u) {
      dst = rogue_vtxout_reg(b.shader, u);
      src = rogue_vtxin_reg(b.shader, u);
      rogue_MOV(&b, rogue_ref_reg(dst), rogue_ref_reg(src));
   }

   if (rta) {
      dst = rogue_vtxout_reg(b.shader, 4);
      src = rogue_vtxin_reg(b.shader, 3);
      rogue_MOV(&b, rogue_ref_reg(dst), rogue_ref_reg(src));
   }

   dst = rogue_vtxout_reg(b.shader, 3);
   rogue_MOV(&b, rogue_ref_reg(dst), rogue_ref_imm_f(1.0f));

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   ralloc_free(shader);
}

void pvr_uscgen_load_op(struct util_dynarray *binary,
                        const struct pvr_load_op *load_op)
{
   const struct usc_mrt_setup *mrt_setup;
   uint32_t next_sh_reg = 0;

   rogue_builder b;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_set_shader_name(shader, "load_op");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   /* TODO: Handle depth and stencil ops.  */
   if (load_op->is_hw_object)
      mrt_setup = &load_op->hw_render->init_setup;
   else
      assert(!"Handle load op at the subpass level.");

   u_foreach_bit (attachment_idx, load_op->clears_loads_state.rt_clear_mask) {
      VkFormat fmt = load_op->clears_loads_state.dest_vk_format[attachment_idx];
      uint32_t accum_size_dwords =
         DIV_ROUND_UP(pvr_get_pbe_accum_format_size_in_bytes(fmt),
                      sizeof(uint32_t));

      struct usc_mrt_resource *mrt_resource =
         &mrt_setup->mrt_resources[attachment_idx];

      char comment[50];
      snprintf(comment, sizeof(comment), "clear_attachment_%d", attachment_idx);

      /* TODO: Handle spilling to tile buffers. */
      assert(mrt_resource->type == USC_MRT_RESOURCE_TYPE_OUTPUT_REG);

      /* pvr_hw_pass always allocates the output regs with 4-byte alignment */
      assert(mrt_resource->reg.offset == 0);

      assert(accum_size_dwords ==
             DIV_ROUND_UP(mrt_resource->intermediate_size, sizeof(uint32_t)));

      for (int i = 0; i < accum_size_dwords; i++) {
         rogue_reg *dst =
            rogue_pixout_reg(shader, mrt_resource->reg.output_reg + i);
         rogue_reg *src = rogue_shared_reg(shader, next_sh_reg++);
         rogue_instr *instr =
            &rogue_MOV(&b, rogue_ref_reg(dst), rogue_ref_reg(src))->instr;
         rogue_add_instr_comment(instr, comment);
      }
   }

   u_foreach_bit (attachment_idx, load_op->clears_loads_state.rt_load_mask) {
      struct usc_mrt_resource *mrt_resource =
         &mrt_setup->mrt_resources[attachment_idx];

      /* TODO: Handle spilling to tile buffers. */
      assert(mrt_resource->type == USC_MRT_RESOURCE_TYPE_OUTPUT_REG);

      /* TODO: Need to do a masked write to the output regs to handle this? */
      /* Is this ever non 0? Ignoring tile buffers. */
      assert(mrt_resource->reg.offset == 0);

      /* TODO: Emit instruction for the LOAD_OP_LOAD. */
      /* Final output should go to mrt_resource->reg.output_reg + byte offset.
       */
      /* The descriptor can be found at `next_sh_reg`. */
      assert(!"Unimplemented");

      next_sh_reg += sizeof(struct pvr_combined_image_sampler_descriptor) / 4;
   }

   /* TODO: Unsupported options. */
   assert(load_op->clears_loads_state.unresolved_msaa_mask == 0);
   assert(load_op->clears_loads_state.depth_clear_to_reg == -1);

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   ralloc_free(shader);
}

void pvr_uscgen_idfwdf(struct util_dynarray *binary,
                       struct pvr_smp_layout *layout,
                       unsigned *temps_used)
{
   rogue_builder b;
   rogue_backend_instr *be;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_set_shader_name(shader, "idfwdf");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   unsigned smp_coords_idx = 0; /* 64-bit, so 0 and 1. */
   unsigned st_src_idx = 2;
   unsigned smp_dst_idx = 3;

   rogue_regarray *coords_lo =
      rogue_shared_regarray(shader, 1, layout->coords_idx);
   rogue_regarray *coords_hi =
      rogue_shared_regarray(shader, 1, layout->coords_idx + 1);
   rogue_regarray *shared_lod =
      rogue_shared_regarray(shader, 2, layout->lod_idx);
   rogue_regarray *image_state =
      rogue_shared_regarray(shader, 4, layout->img_state_idx);
   rogue_regarray *sampler_state =
      rogue_shared_regarray(shader, 4, layout->smp_state_idx);

   rogue_ref64 smp_coords = rogue_temp_ref64(shader, smp_coords_idx);

   rogue_reg *st_src = rogue_temp_reg(shader, st_src_idx);
   rogue_reg *smp_dst = rogue_temp_reg(shader, smp_dst_idx);

   /* Do a memory store and a texture read to fence any loads/texture writes
    * from previous kernels.
    */

   /* r0 = sh1 */
   rogue_MOV(&b, smp_coords.lo32, rogue_ref_regarray(coords_hi));

   /* r1 = sh0 */
   rogue_MOV(&b, smp_coords.hi32, rogue_ref_regarray(coords_lo));

   /* r2 = c0 */
   rogue_MOV(&b, rogue_ref_reg(st_src), rogue_ref_imm(0));

   /* MEM[r[0..1]] = r2 */
   be = rogue_ST(&b,
                 rogue_ref_reg(st_src),
                 rogue_ref_val(2),
                 rogue_ref_drc(0),
                 rogue_ref_val(1),
                 smp_coords.ref64,
                 rogue_none());
   /* rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK); */
   rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_WRITETHROUGH);
   rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_NOWDF);

   /* idf drc0, r0 */
   /* wdf drc0 */
   rogue_IDF(&b, rogue_ref_drc(0), smp_coords.ref64);

   /* Fence any texture writes from the preceding kernel, and
    * do a block of reads to flush the MADD cache through. */
   for (unsigned y = 0; y < 2; ++y) {
      for (unsigned x = 0; x < 4; ++x) {
         rogue_MOV(&b, smp_coords.lo32, rogue_ref_imm(x));
         rogue_MOV(&b, smp_coords.hi32, rogue_ref_imm(y));
         be = rogue_SMP2D(&b,
                          rogue_ref_reg(smp_dst),
                          rogue_ref_drc(0),
                          rogue_ref_regarray(image_state),
                          smp_coords.ref64,
                          rogue_ref_regarray(sampler_state),
                          rogue_ref_regarray(shared_lod),
                          rogue_ref_val(1));

         /* rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK); */
         rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_FCNORM);
         rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_REPLACE);
         rogue_add_instr_commentf(&be->instr, "Sample %u,%u", x, y);
      }
   }

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   *temps_used = rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   ralloc_free(shader);
}

void pvr_uscgen_nop(struct util_dynarray *binary)
{
   rogue_builder b;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_set_shader_name(shader, "NOP");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   rogue_END(&b);

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   ralloc_free(shader);
}
