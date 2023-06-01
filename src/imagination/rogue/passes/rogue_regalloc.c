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

#include "rogue.h"
#include "rogue_builder.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/register_allocate.h"
#include "util/sparse_array.h"
#include "util/u_qsort.h"

#include <stdbool.h>
#include <stdlib.h>

/**
 * \file rogue_regalloc.c
 *
 * \brief Contains the rogue_regalloc pass.
 */

#define ROGUE_RA_CLASS_INFO_NODE_SIZE 64

/* TODO: Internal register support for high register pressure regs. */
/* TODO: vtxin register support. */

typedef struct rogue_ra_class_info {
   unsigned stride;
   unsigned class_index;
   enum rogue_reg_class hw_class;
} rogue_ra_class_info;

typedef struct rogue_live_range {
   unsigned start;
   unsigned end;
} rogue_live_range;

static void rogue_regarray_liveness(rogue_regarray *regarray,
                                    rogue_live_range *live_range)
{
   assert(list_is_singular(&regarray->writes) ||
          list_is_empty(&regarray->writes));
   if (!list_is_empty(&regarray->writes)) {
      rogue_regarray_write *write =
         list_first_entry(&regarray->writes, rogue_regarray_write, link);
      live_range->start = MIN2(live_range->start, write->instr->index);
   }

   rogue_foreach_regarray_use (use, regarray) {
      live_range->end = MAX2(live_range->end, use->instr->index);
   }
}

static void rogue_reg_liveness(rogue_reg *reg, rogue_live_range *live_range)
{
   assert(list_is_singular(&reg->writes) || list_is_empty(&reg->writes));

   if (!list_is_empty(&reg->writes)) {
      rogue_reg_write *write =
         list_first_entry(&reg->writes, rogue_reg_write, link);
      live_range->start = MIN2(live_range->start, write->instr->index);
   }

   rogue_foreach_reg_use (use, reg) {
      live_range->end = MAX2(live_range->end, use->instr->index);
   }
}

static int regarray_cmp(const void *lhs, const void *rhs, UNUSED void *arg)
{
   const rogue_regarray *const *l = lhs;
   const rogue_regarray *const *r = rhs;

   /* Signs swapped for sorting largest->smallest. */
   if ((*l)->size > (*r)->size)
      return -1;
   else if ((*l)->size < (*r)->size)
      return 1;

   return 0;
}

static inline void rogue_ra_setup_class(struct ra_regs *ra_regs,
                                        struct util_sparse_array *ra_class_info,
                                        unsigned stride,
                                        enum rogue_reg_class hw_class,
                                        unsigned num_hw_prealloced,
                                        unsigned num_hw)
{
   struct ra_class *ra_class = ra_alloc_contig_reg_class(ra_regs, stride);
   unsigned class_index = ra_class_index(ra_class);

   rogue_ra_class_info *class_info =
      util_sparse_array_get(ra_class_info, stride);
   class_info->stride = stride;
   class_info->class_index = class_index;
   class_info->hw_class = hw_class;

   for (unsigned t = num_hw_prealloced; t < num_hw - (stride - 1); ++t)
      if (!(t % stride))
         ra_class_add_reg(ra_class, t);
}

/* TODO: Track successors/predecessors and do this properly when
 * implementing full regalloc.
 */
static inline void
rogue_extend_loop_reg_lifetimes(rogue_shader *shader,
                                rogue_live_range *ssa_live_range,
                                unsigned num_ssa_regs)
{
   rogue_live_range *loop_live_range =
      rzalloc_array_size(NULL, sizeof(*loop_live_range), shader->loops);

   unsigned l = 0;
   rogue_foreach_instr_in_shader (instr, shader) {
      if (instr->type != ROGUE_INSTR_TYPE_CTRL)
         continue;

      const rogue_ctrl_instr *ctrl = rogue_instr_as_ctrl(instr);
      if (!ctrl->loop_start)
         continue;

      loop_live_range[l].start = instr->index;
      loop_live_range[l].end = ctrl->loop_link->index;

      ++l;
   }

   /* If an ssa reg stops being used in the middle of a loop, mark it as used
    * until the end of the loop. */
   for (unsigned u = 0; u < num_ssa_regs; ++u) {
      for (l = 0; l < shader->loops; ++l) {
         if ((ssa_live_range[u].end >= loop_live_range[l].start) &&
             (ssa_live_range[u].end <= loop_live_range[l].end)) {
            ssa_live_range[u].end = loop_live_range[l].end;
         }
      }
   }

   ralloc_free(loop_live_range);
}

PUBLIC
bool rogue_regalloc(rogue_shader *shader)
{
   if (shader->is_grouped)
      return false;

   bool progress = false;

   unsigned num_ssa_regs = rogue_count_used_regs(shader, ROGUE_REG_CLASS_SSA);
   if (!num_ssa_regs)
      return false;

   unsigned num_temps_prealloced =
      rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);
   unsigned num_hw_temps =
      rogue_reg_class_infos[ROGUE_REG_CLASS_TEMP].num - num_temps_prealloced;

   struct ra_regs *ra_regs = ra_alloc_reg_set(shader, num_hw_temps, true);

   /* TODO: Consider tracking this in the shader itself, i.e. one list for child
    * regarrays, one for parents. Or, since children are already in a list in
    * the parent, only have parent regarrays in the shader.
    */

   /* Count the parent regarrays. */
   unsigned num_parent_regarrays = 0;
   rogue_foreach_regarray (regarray, shader) {
      if (regarray->parent || regarray->regs[0]->class != ROGUE_REG_CLASS_SSA)
         continue;

      ++num_parent_regarrays;
   }

   /* Construct list of sorted parent regarrays. */
   rogue_regarray **parent_regarrays =
      rzalloc_array_size(ra_regs,
                         sizeof(*parent_regarrays),
                         num_parent_regarrays);

   unsigned ra = 0;
   rogue_foreach_regarray (regarray, shader) {
      if (regarray->parent || regarray->regs[0]->class != ROGUE_REG_CLASS_SSA)
         continue;

      parent_regarrays[ra++] = regarray;
   }

   util_qsort_r(parent_regarrays,
                num_parent_regarrays,
                sizeof(*parent_regarrays),
                regarray_cmp,
                NULL);

   /* Setup regset and contiguous register classes. */
   struct util_sparse_array ra_class_info;
   util_sparse_array_init(&ra_class_info,
                          sizeof(rogue_ra_class_info),
                          ROGUE_RA_CLASS_INFO_NODE_SIZE);

   bool single_regs = false;
   for (unsigned u = 0; u < num_parent_regarrays; ++u) {
      /* List sorted by size! So easily skip sizes we've already handled. */
      if (u > 0 && (parent_regarrays[u]->size == parent_regarrays[u - 1]->size))
         continue;

      unsigned stride = parent_regarrays[u]->size;
      rogue_ra_setup_class(ra_regs,
                           &ra_class_info,
                           stride,
                           ROGUE_REG_CLASS_TEMP,
                           num_temps_prealloced,
                           num_hw_temps);
      if (stride == 1)
         single_regs = true;
   }

   /* Ensure we have support for single/standalone registers too. */
   if (!single_regs) {
      rogue_ra_setup_class(ra_regs,
                           &ra_class_info,
                           1,
                           ROGUE_REG_CLASS_TEMP,
                           num_temps_prealloced,
                           num_hw_temps);
   }

   rogue_ra_class_info *single_class_info =
      util_sparse_array_get(&ra_class_info, 1);
   assert(single_class_info->stride == 1);

   ra_set_finalize(ra_regs, NULL);

   /* Prepare live ranges. */
   rogue_live_range *ssa_live_range =
      rzalloc_array_size(ra_regs, sizeof(*ssa_live_range), num_ssa_regs);
   for (unsigned u = 0; u < num_ssa_regs; ++u)
      ssa_live_range[u].start = ~0U;

   /* Populate live ranges for register arrays. */
   for (unsigned u = 0; u < num_parent_regarrays; ++u) {
      rogue_regarray *regarray = parent_regarrays[u];
      unsigned base_index = regarray->regs[0]->index;
      rogue_live_range *live_range = &ssa_live_range[base_index];

      rogue_regarray_liveness(regarray, live_range);

      rogue_foreach_subarray (subarray, regarray) {
         rogue_regarray_liveness(subarray, live_range);
      }
   }

   /* Populate live ranges for registers. */
   rogue_foreach_reg (reg, shader, ROGUE_REG_CLASS_SSA) {
      if (reg->regarray)
         continue;

      rogue_live_range *live_range = &ssa_live_range[reg->index];
      rogue_reg_liveness(reg, live_range);
   }

   /* Extended lifetimes of SSA regs in loops. */
   rogue_extend_loop_reg_lifetimes(shader, ssa_live_range, num_ssa_regs);

   struct ra_graph *ra_graph =
      ra_alloc_interference_graph(ra_regs, num_ssa_regs);
   ralloc_steal(ra_regs, ra_graph);

   /* Set register class for regarrays/vectors. */
   for (unsigned u = 0; u < num_parent_regarrays; ++u) {
      rogue_regarray *regarray = parent_regarrays[u];
      unsigned base_index = regarray->regs[0]->index;
      unsigned stride = regarray->size;

      rogue_ra_class_info *class_info =
         util_sparse_array_get(&ra_class_info, stride);
      assert(class_info->stride == stride);

      ra_set_node_class(ra_graph,
                        base_index,
                        ra_get_class_from_index(ra_regs,
                                                class_info->class_index));
   }

   /* Set register class for "standalone" registers. */
   rogue_foreach_reg (reg, shader, ROGUE_REG_CLASS_SSA) {
      if (reg->regarray)
         continue;

      ra_set_node_class(
         ra_graph,
         reg->index,
         ra_get_class_from_index(ra_regs, single_class_info->class_index));
   }

   /* Build interference graph from overlapping live ranges. */
   for (unsigned index0 = 0; index0 < num_ssa_regs; ++index0) {
      rogue_live_range *live_range0 = &ssa_live_range[index0];

      for (unsigned index1 = 0; index1 < num_ssa_regs; ++index1) {
         if (index0 == index1)
            continue;

         rogue_live_range *live_range1 = &ssa_live_range[index1];

         /* If the live ranges overlap, those register nodes interfere. */
         if (!(live_range0->start >= live_range1->end ||
               live_range1->start >= live_range0->end))
            ra_add_node_interference(ra_graph, index0, index1);
      }
   }

   /* TODO: Spilling support. */
   if (!ra_allocate(ra_graph))
      unreachable("Register allocation failed.");

   /* Print allocations. */
   if (ROGUE_DEBUG(REGALLOC)) {
      FILE *fp = stdout;

      /* Regarray allocations. */
      fputs("Register allocations\n", fp);
      for (unsigned u = 0; u < num_parent_regarrays; ++u) {
         rogue_regarray *regarray = parent_regarrays[u];

         unsigned size = regarray->size;
         unsigned base_index = regarray->regs[0]->index;
         unsigned hw_base_index = ra_get_node_reg(ra_graph, base_index);

         rogue_ra_class_info *class_info =
            util_sparse_array_get(&ra_class_info, size);
         assert(class_info->stride == size);
         enum rogue_reg_class new_class = class_info->hw_class;

         rogue_print_regarray(fp, regarray);
         fputs(" -> ", fp);
         rogue_print_regarray_raw(fp, new_class, hw_base_index, size);
         fputs("\n", fp);
      }

      /* Standalone register allocations. */
      rogue_foreach_reg_safe (reg, shader, ROGUE_REG_CLASS_SSA) {
         if (reg->regarray)
            continue;

         unsigned hw_index = ra_get_node_reg(ra_graph, reg->index);
         enum rogue_reg_class new_class = single_class_info->hw_class;

         rogue_print_reg(fp, reg, ROGUE_IDX_NONE);
         fputs(" -> ", fp);
         rogue_print_reg_raw(fp, new_class, hw_index);
         fputs("\n", fp);
      }

      fputs("\n", fp);
   }

   /* Replace SSA regarray registers with allocated physical registers. */
   for (unsigned u = 0; u < num_parent_regarrays; ++u) {
      rogue_regarray *regarray = parent_regarrays[u];

      unsigned base_index = regarray->regs[0]->index;
      unsigned hw_base_index = ra_get_node_reg(ra_graph, base_index);

      unsigned stride = regarray->size;
      rogue_ra_class_info *class_info =
         util_sparse_array_get(&ra_class_info, stride);
      assert(class_info->stride == stride);
      enum rogue_reg_class new_class = class_info->hw_class;

      bool used = false;
      for (unsigned r = 0; r < regarray->size; ++r) {
         used |= rogue_reg_is_used(shader, new_class, hw_base_index + r);

         if (used)
            break;
      }

      /* First time using new regarray, modify in place. */
      if (!used) {
         progress |=
            rogue_regarray_rewrite(shader, regarray, new_class, hw_base_index);
      } else {
         /* Regarray has already been used, replace references and delete. */

         /* Replace parent regarray first. */
         rogue_regarray *new_regarray = rogue_regarray_cached(shader,
                                                              regarray->size,
                                                              new_class,
                                                              hw_base_index,
                                                              true);
         progress |=
            rogue_regarray_replace(shader, regarray, new_regarray, true);
      }
   }

   /* Replace remaining standalone SSA registers with allocated physical
    * registers. */
   rogue_foreach_reg_safe (reg, shader, ROGUE_REG_CLASS_SSA) {
      assert(!reg->regarray);
      unsigned hw_index = ra_get_node_reg(ra_graph, reg->index);
      enum rogue_reg_class new_class = single_class_info->hw_class;

      /* First time using new register, modify in place. */
      if (!rogue_reg_is_used(shader, new_class, hw_index)) {
         progress |= rogue_reg_rewrite(shader, reg, new_class, hw_index);
      } else {
         /* Register has already been used, replace references and delete. */
         assert(list_is_singular(&reg->writes)); /* SSA reg. */
         rogue_reg *new_reg = rogue_temp_reg(shader, hw_index);
         progress |= rogue_reg_replace(reg, new_reg);
      }
   }

   /* In debug builds this will check the temp regs are contiguous from zero. */
   unsigned num_temp_regs = rogue_count_used_regs(shader, ROGUE_REG_CLASS_TEMP);

   /* If we're doing control flow, allocate extra temp(s) for the execution mask
    * counter(s). */
   unsigned num_emc_regs = rogue_count_used_regs(shader, ROGUE_REG_CLASS_EMC);
   assert(num_emc_regs <= 1); /* Should only be one for now. */

   /* TODO: commonise with above. */
   unsigned next_temp = num_temp_regs;
   rogue_foreach_reg_safe (reg, shader, ROGUE_REG_CLASS_EMC) {
      assert(!reg->regarray);
      unsigned hw_index = next_temp++;
      enum rogue_reg_class new_class = single_class_info->hw_class;

      /* First time using new register, modify in place. */
      if (!rogue_reg_is_used(shader, new_class, hw_index)) {
         progress |= rogue_reg_rewrite(shader, reg, new_class, hw_index);
      } else {
         /* Register has already been used, replace references and delete. */
         assert(list_is_singular(&reg->writes)); /* SSA reg. */
         rogue_reg *new_reg = rogue_temp_reg(shader, hw_index);
         progress |= rogue_reg_replace(reg, new_reg);
      }
   }

   util_sparse_array_finish(&ra_class_info);
   ralloc_free(ra_regs);

   return progress;
}
