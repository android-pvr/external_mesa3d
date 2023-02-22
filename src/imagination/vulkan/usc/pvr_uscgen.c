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
   emitpix->instr.end = true;

   rogue_shader_passes(shader);
   rogue_encode_shader(NULL, shader, binary);

   *temps_used = rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   ralloc_free(shader);
}

void pvr_uscgen_load_op(struct util_dynarray *binary,
                        const struct pvr_load_op *load_op)
{
   rogue_builder b;
   rogue_reg *dst;
   rogue_reg *src;
   rogue_shader *shader = rogue_shader_create(NULL, MESA_SHADER_NONE);
   rogue_set_shader_name(shader, "load_op");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   /* Clears. */
   assert(load_op->clears_loads_state.rt_clear_mask == 1);
   u_foreach_bit (rt_clear, load_op->clears_loads_state.rt_clear_mask) {
      VkFormat fmt = load_op->clears_loads_state.dest_vk_format[rt_clear];

      /* TODO: Calculate/ingest the proper offsets when
       * supporting additional clears.
       */
      dst = rogue_pixout_reg(shader, rt_clear);
      src = rogue_shared_reg(shader, rt_clear);

      switch (vk_format_get_blocksizebits(fmt)) {
      case 32:
         rogue_MOV(&b, rogue_ref_reg(dst), rogue_ref_reg(src));
         break;

      default:
         unreachable("Unsupported format block size.");
      }
   }

   /* Loads. */
   /* TODO: Implement. */
   assert(load_op->clears_loads_state.rt_load_mask == 0);

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
   rogue_set_shader_name(shader, "NOP");
   rogue_builder_init(&b, shader);
   rogue_push_block(&b);

   unsigned coords_lo_idx = 0;
   unsigned coords_hi_idx = 1;
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

   rogue_regarray *coords_64 = rogue_temp_regarray(shader, 2, coords_lo_idx);
   rogue_regarray *coords_2x32[2] = {
      rogue_temp_regarray(shader, 1, coords_lo_idx),
      rogue_temp_regarray(shader, 1, coords_hi_idx),
   };

   rogue_reg *st_src = rogue_temp_reg(shader, st_src_idx);
   rogue_reg *smp_dst = rogue_temp_reg(shader, smp_dst_idx);

   /* Do a memory store and a texture read to fence any loads/texture writes
    * from previous kernels.
    */

   /* r0 = sh1 */
   rogue_MOV(&b,
             rogue_ref_regarray(coords_2x32[0]),
             rogue_ref_regarray(coords_hi));

   /* r1 = sh0 */
   rogue_MOV(&b,
             rogue_ref_regarray(coords_2x32[1]),
             rogue_ref_regarray(coords_lo));

   /* r2 = c0 */
   rogue_MOV(&b, rogue_ref_reg(st_src), rogue_ref_imm(0));

   /* MEM[r[0..1]] = r2 */
   be = rogue_ST(&b,
                 rogue_ref_reg(st_src),
                 rogue_ref_val(2),
                 rogue_ref_drc(0),
                 rogue_ref_val(1),
                 rogue_ref_regarray(coords_64),
                 rogue_ref_io(ROGUE_IO_NONE));
   rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK);
   rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_WRITETHROUGH);
   rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_NOWDF);

   /* idf drc0, r0 */
   /* wdf drc0 */
   rogue_IDF(&b, rogue_ref_drc(0), rogue_ref_regarray(coords_64));

   /* Fence any texture writes from the preceding kernel, and
    * do a block of reads to flush the MADD cache through. */
   for (unsigned y = 0; y < 2; ++y) {
      for (unsigned x = 0; x < 4; ++x) {
         rogue_MOV(&b, rogue_ref_regarray(coords_2x32[0]), rogue_ref_imm(x));
         rogue_MOV(&b, rogue_ref_regarray(coords_2x32[1]), rogue_ref_imm(y));
         be = rogue_SMP2D(&b,
                          rogue_ref_reg(smp_dst),
                          rogue_ref_drc(0),
                          rogue_ref_regarray(image_state),
                          rogue_ref_regarray(coords_64),
                          rogue_ref_regarray(sampler_state),
                          rogue_ref_regarray(shared_lod),
                          rogue_ref_val(1));

         rogue_set_backend_op_mod(be, ROGUE_BACKEND_OP_MOD_SLCWRITEBACK);
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
