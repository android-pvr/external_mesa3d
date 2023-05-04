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

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "compiler/shader_enums.h"
#include "nir/nir.h"
#include "rogue.h"
#include "util/macros.h"

/**
 * \file rogue_build_data.c
 *
 * \brief Contains functions to collect build data for the driver.
 */

/* N.B. This will all be hoisted into the driver. */

/**
 * \brief Returns the allocated coefficient register index for a component of an
 * input varying location.
 *
 * \param[in] args The allocated iterator argument data.
 * \param[in] location The input varying location, or ~0 for the W coefficient.
 * \param[in] component The requested component.
 * \return The coefficient register index.
 */
PUBLIC
unsigned rogue_coeff_index_fs(struct rogue_iterator_args *args,
                              gl_varying_slot location,
                              unsigned component)
{
   unsigned i;

   /* Special case: W coefficient. */
   if (location == ~0) {
      /* The W component shouldn't be the only one. */
      assert(args->num_fpu_iterators > 1);
      assert(args->destination[0] == 0);
      return 0;
   }

   i = (location - VARYING_SLOT_VAR0) + 1;
   assert(location >= VARYING_SLOT_VAR0 && location <= VARYING_SLOT_VAR31);
   assert(i < args->num_fpu_iterators);
   assert(component < args->components[i]);
   assert(args->base[i] != ~0);

   return args->base[i] + (ROGUE_COEFF_ALIGN * component);
}

/**
 * \brief Returns the interpolation mode for a component of an
 * input varying location.
 *
 * \param[in] args The allocated iterator argument data.
 * \param[in] location The input varying location, or ~0 for the W coefficient.
 * \param[in] component The requested component.
 * \return The interpolation mode.
 */
PUBLIC
enum glsl_interp_mode rogue_interp_mode_fs(struct rogue_iterator_args *args,
                                           gl_varying_slot location,
                                           unsigned component)
{
   unsigned i;

   /* Special case: W coefficient. */
   if (location == ~0) {
      /* The W component shouldn't be the only one. */
      assert(args->num_fpu_iterators > 1);
      assert(args->destination[0] == 0);
      return INTERP_MODE_NOPERSPECTIVE;
   }

   i = (location - VARYING_SLOT_VAR0) + 1;
   assert(location >= VARYING_SLOT_VAR0 && location <= VARYING_SLOT_VAR31);
   assert(i < args->num_fpu_iterators);
   assert(component < args->components[i]);
   assert(args->base[i] != ~0);

   return args->interp_modes[i][component] == INTERP_MODE_NONE
             ? INTERP_MODE_SMOOTH
             : args->interp_modes[i][component];
}

/**
 * \brief Returns the allocated vertex output index for a component of an input
 * varying location.
 *
 * \param[in] outputs The vertex output data.
 * \param[in] location The output varying location.
 * \param[in] component The requested component.
 * \return The vertex output index.
 */
PUBLIC
unsigned rogue_output_index_vs(struct rogue_vertex_outputs *outputs,
                               gl_varying_slot location,
                               unsigned component)
{
   unsigned i;
   assert(component < 4);
   if (location == VARYING_SLOT_POS) {
      /* Always at location 0. */
      i = component;
   } else if (location == VARYING_SLOT_PSIZ) {
      assert(component == 0);
      i = outputs->point_size_index;
   } else if (location == VARYING_SLOT_VIEWPORT) {
      assert(component == 0);
      i = outputs->viewport_index;
   } else if (location == VARYING_SLOT_LAYER) {
      assert(component == 0);
      i = outputs->layer_index;
   } else if ((location >= VARYING_SLOT_CLIP_DIST0) &&
              (location <= VARYING_SLOT_CLIP_DIST1)) {
      i = outputs->clip_index[location - VARYING_SLOT_CLIP_DIST0][component];
   } else if ((location >= VARYING_SLOT_CULL_DIST0) &&
              (location <= VARYING_SLOT_CULL_DIST1)) {
      i = outputs->cull_index[location - VARYING_SLOT_CULL_DIST0][component];
   } else if ((location >= VARYING_SLOT_VAR0) &&
              (location <= VARYING_SLOT_VAR31)) {
      i = outputs->indices[location - VARYING_SLOT_VAR0][component];
      assert(i < outputs->num_output_vars);
   } else {
      unreachable("Unsupported vertex output type.");
   }
   assert(i != ~0);
   return i;
}
