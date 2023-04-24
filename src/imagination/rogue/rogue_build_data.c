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

   if (location == VARYING_SLOT_POS) {
      /* Always at location 0. */
      assert(outputs->base[0] == 0);
      i = 0;
   } else if ((location >= VARYING_SLOT_VAR0) &&
              (location <= VARYING_SLOT_VAR31)) {
      i = (location - VARYING_SLOT_VAR0) + 1;
   } else {
      unreachable("Unsupported vertex output type.");
   }

   assert(i < outputs->num_output_vars);
   assert(component < outputs->components[i]);
   assert(outputs->base[i] != ~0);

   return outputs->base[i] + component;
}
