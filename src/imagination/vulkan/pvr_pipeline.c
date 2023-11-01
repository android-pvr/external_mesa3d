/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * based in part on v3dv driver which is:
 * Copyright © 2019 Raspberry Pi
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
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "compiler/shader_enums.h"
#include "hwdef/rogue_hw_utils.h"
#include "nir/nir.h"
#include "pvr_bo.h"
#include "pvr_csb.h"
#include "pvr_csb_enum_helpers.h"
#include "pvr_pds.h"
#include "pvr_private.h"
#include "pvr_robustness.h"
#include "pvr_types.h"
#include "rogue/rogue.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"
#include "util/u_qsort.h"
#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_graphics_state.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_pipeline_cache.h"
#include "vk_render_pass.h"
#include "vk_shader_module.h"
#include "vk_util.h"

/*****************************************************************************
   PDS functions
*****************************************************************************/

/* If allocator == NULL, the internal one will be used. */
static VkResult pvr_pds_coeff_program_create_and_upload(
   struct pvr_device *device,
   const VkAllocationCallbacks *allocator,
   const uint32_t *fpu_iterators,
   uint32_t fpu_iterators_count,
   const uint32_t *destinations,
   struct pvr_pds_upload *const pds_upload_out,
   uint32_t *const pds_temps_count_out)
{
   struct pvr_pds_coeff_loading_program program = {
      .num_fpu_iterators = fpu_iterators_count,
   };
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   assert(fpu_iterators_count < PVR_MAXIMUM_ITERATIONS);

   /* Get the size of the program and then allocate that much memory. */
   pvr_pds_coefficient_loading(&program, NULL, PDS_GENERATE_SIZES);

   if (!program.code_size) {
      pds_upload_out->pvr_bo = NULL;
      pds_upload_out->code_size = 0;
      pds_upload_out->data_size = 0;
      *pds_temps_count_out = 0;

      return VK_SUCCESS;
   }

   staging_buffer_size = PVR_DW_TO_BYTES(program.code_size + program.data_size);

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* FIXME: Should we save pointers when we redesign the pds gen api ? */
   typed_memcpy(program.FPU_iterators,
                fpu_iterators,
                program.num_fpu_iterators);

   typed_memcpy(program.destination, destinations, program.num_fpu_iterators);

   /* Generate the program into is the staging_buffer. */
   pvr_pds_coefficient_loading(&program,
                               staging_buffer,
                               PDS_GENERATE_CODEDATA_SEGMENTS);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               &staging_buffer[0],
                               program.data_size,
                               16,
                               &staging_buffer[program.data_size],
                               program.code_size,
                               16,
                               16,
                               pds_upload_out);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, staging_buffer);
      return result;
   }

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   *pds_temps_count_out = program.temps_used;

   return VK_SUCCESS;
}

/* FIXME: move this elsewhere since it's also called in pvr_pass.c? */
/* If allocator == NULL, the internal one will be used. */
VkResult pvr_pds_fragment_program_create_and_upload(
   struct pvr_device *device,
   const VkAllocationCallbacks *allocator,
   const struct pvr_suballoc_bo *fragment_shader_bo,
   uint32_t fragment_temp_count,
   enum rogue_msaa_mode msaa_mode,
   bool has_phase_rate_change,
   struct pvr_pds_upload *const pds_upload_out)
{
   const enum PVRX(PDSINST_DOUTU_SAMPLE_RATE)
      sample_rate = pvr_pdsinst_doutu_sample_rate_from_rogue(msaa_mode);
   struct pvr_pds_kickusc_program program = { 0 };
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   /* FIXME: Should it be passing in the USC offset rather than address here?
    */
   /* Note this is not strictly required to be done before calculating the
    * staging_buffer_size in this particular case. It can also be done after
    * allocating the buffer. The size from pvr_pds_kick_usc() is constant.
    */
   pvr_pds_setup_doutu(&program.usc_task_control,
                       fragment_shader_bo->dev_addr.addr,
                       fragment_temp_count,
                       sample_rate,
                       has_phase_rate_change);

   pvr_pds_kick_usc(&program, NULL, 0, false, PDS_GENERATE_SIZES);

   staging_buffer_size = PVR_DW_TO_BYTES(program.code_size + program.data_size);

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pvr_pds_kick_usc(&program,
                    staging_buffer,
                    0,
                    false,
                    PDS_GENERATE_CODEDATA_SEGMENTS);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               &staging_buffer[0],
                               program.data_size,
                               16,
                               &staging_buffer[program.data_size],
                               program.code_size,
                               16,
                               16,
                               pds_upload_out);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, staging_buffer);
      return result;
   }

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;
}

static inline size_t pvr_pds_get_max_vertex_program_const_map_size_in_bytes(
   const struct pvr_device_info *dev_info,
   bool robust_buffer_access)
{
   /* FIXME: Use more local variable to improve formatting. */

   /* Maximum memory allocation needed for const map entries in
    * pvr_pds_generate_vertex_primary_program().
    * When robustBufferAccess is disabled, it must be >= 410.
    * When robustBufferAccess is enabled, it must be >= 570.
    *
    * 1. Size of entry for base instance
    *        (pvr_const_map_entry_base_instance)
    *
    * 2. Max. number of vertex inputs (PVR_MAX_VERTEX_INPUT_BINDINGS) * (
    *     if (!robustBufferAccess)
    *         size of vertex attribute entry
    *             (pvr_const_map_entry_vertex_attribute_address) +
    *     else
    *         size of robust vertex attribute entry
    *             (pvr_const_map_entry_robust_vertex_attribute_address) +
    *         size of entry for max attribute index
    *             (pvr_const_map_entry_vertex_attribute_max_index) +
    *     fi
    *     size of Unified Store burst entry
    *         (pvr_const_map_entry_literal32) +
    *     size of entry for vertex stride
    *         (pvr_const_map_entry_literal32) +
    *     size of entries for DDMAD control word
    *         (num_ddmad_literals * pvr_const_map_entry_literal32))
    *
    * 3. Size of entry for DOUTW vertex/instance control word
    *     (pvr_const_map_entry_literal32)
    *
    * 4. Size of DOUTU entry (pvr_const_map_entry_doutu_address)
    */

   const size_t attribute_size =
      (!robust_buffer_access)
         ? sizeof(struct pvr_const_map_entry_vertex_attribute_address)
         : sizeof(struct pvr_const_map_entry_robust_vertex_attribute_address) +
              sizeof(struct pvr_const_map_entry_vertex_attribute_max_index);

   /* If has_pds_ddmadt the DDMAD control word is now a DDMADT control word
    * and is increased by one DWORD to contain the data for the DDMADT's
    * out-of-bounds check.
    */
   const size_t pvr_pds_const_map_vertex_entry_num_ddmad_literals =
      1U + (size_t)PVR_HAS_FEATURE(dev_info, pds_ddmadt);

   return (sizeof(struct pvr_const_map_entry_base_instance) +
           PVR_MAX_VERTEX_INPUT_BINDINGS *
              (attribute_size +
               (2 + pvr_pds_const_map_vertex_entry_num_ddmad_literals) *
                  sizeof(struct pvr_const_map_entry_literal32)) +
           sizeof(struct pvr_const_map_entry_literal32) +
           sizeof(struct pvr_const_map_entry_doutu_address));
}

static VkResult pvr_pds_vertex_attrib_program_create_and_upload(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   struct pvr_pds_vertex_primary_program_input *const input,
   struct pvr_pds_attrib_program *const program_out)
{
   const size_t const_entries_size_in_bytes =
      pvr_pds_get_max_vertex_program_const_map_size_in_bytes(
         &device->pdevice->dev_info,
         device->vk.enabled_features.robustBufferAccess);
   struct pvr_pds_upload *const program = &program_out->program;
   struct pvr_pds_info *const info = &program_out->info;
   struct pvr_const_map_entry *new_entries;
   ASSERTED uint32_t code_size_in_dwords;
   size_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   memset(info, 0, sizeof(*info));

   info->entries = vk_alloc2(&device->vk.alloc,
                             allocator,
                             const_entries_size_in_bytes,
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!info->entries) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_out;
   }

   info->entries_size_in_bytes = const_entries_size_in_bytes;

   pvr_pds_generate_vertex_primary_program(
      input,
      NULL,
      info,
      device->vk.enabled_features.robustBufferAccess,
      &device->pdevice->dev_info);

   code_size_in_dwords = info->code_size_in_dwords;
   staging_buffer_size = PVR_DW_TO_BYTES(info->code_size_in_dwords);

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_free_entries;
   }

   /* This also fills in info->entries. */
   pvr_pds_generate_vertex_primary_program(
      input,
      staging_buffer,
      info,
      device->vk.enabled_features.robustBufferAccess,
      &device->pdevice->dev_info);

   assert(info->code_size_in_dwords <= code_size_in_dwords);

   /* FIXME: Add a vk_realloc2() ? */
   new_entries = vk_realloc((!allocator) ? &device->vk.alloc : allocator,
                            info->entries,
                            info->entries_written_size_in_bytes,
                            8,
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!new_entries) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_free_staging_buffer;
   }

   info->entries = new_entries;
   info->entries_size_in_bytes = info->entries_written_size_in_bytes;

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               NULL,
                               0,
                               0,
                               staging_buffer,
                               info->code_size_in_dwords,
                               16,
                               16,
                               program);
   if (result != VK_SUCCESS)
      goto err_free_staging_buffer;

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;

err_free_staging_buffer:
   vk_free2(&device->vk.alloc, allocator, staging_buffer);

err_free_entries:
   vk_free2(&device->vk.alloc, allocator, info->entries);

err_out:
   return result;
}

static inline void pvr_pds_vertex_attrib_program_destroy(
   struct pvr_device *const device,
   const struct VkAllocationCallbacks *const allocator,
   struct pvr_pds_attrib_program *const program)
{
   pvr_bo_suballoc_free(program->program.pvr_bo);
   vk_free2(&device->vk.alloc, allocator, program->info.entries);
}

/* This is a const pointer to an array of pvr_pds_attrib_program structs.
 * The array being pointed to is of PVR_PDS_VERTEX_ATTRIB_PROGRAM_COUNT size.
 */
typedef struct pvr_pds_attrib_program (*const pvr_pds_attrib_programs_array_ptr)
   [PVR_PDS_VERTEX_ATTRIB_PROGRAM_COUNT];

/* Generate and uploads a PDS program for DMAing vertex attribs into USC vertex
 * inputs. This will bake the code segment and create a template of the data
 * segment for the command buffer to fill in.
 */
/* If allocator == NULL, the internal one will be used.
 *
 * programs_out_ptr is a pointer to the array where the outputs will be placed.
 */
static VkResult pvr_pds_vertex_attrib_programs_create_and_upload(
   struct pvr_device *device,
   const VkAllocationCallbacks *const allocator,
   uint32_t usc_temp_count,
   struct rogue_vertex_special_vars *special_vars_layout,
   const struct pvr_pds_vertex_dma
      dma_descriptions[static const PVR_MAX_VERTEX_ATTRIB_DMAS],
   uint32_t dma_count,
   pvr_pds_attrib_programs_array_ptr programs_out_ptr)
{
   struct pvr_pds_vertex_primary_program_input input = {
      .dma_list = dma_descriptions,
      .dma_count = dma_count,
   };
   struct pvr_pds_attrib_program *const programs_out = *programs_out_ptr;
   VkResult result;

   if (special_vars_layout->has.vertex_id) {
      input.flags |= PVR_PDS_VERTEX_FLAGS_VERTEX_ID_REQUIRED;
      input.vertex_id_register = special_vars_layout->offset.vertex_id;
   }

   if (special_vars_layout->has.instance_id) {
      input.flags |= PVR_PDS_VERTEX_FLAGS_INSTANCE_ID_REQUIRED;
      input.instance_id_register = special_vars_layout->offset.instance_id;
   }

   if (special_vars_layout->has.base_instance) {
      input.flags |= PVR_PDS_VERTEX_FLAGS_BASE_INSTANCE_REQUIRED;
      input.base_instance_register = special_vars_layout->offset.base_instance;
   }

   if (special_vars_layout->has.base_vertex) {
      input.flags |= PVR_PDS_VERTEX_FLAGS_BASE_VERTEX_REQUIRED;
      input.base_vertex_register = special_vars_layout->offset.base_vertex;
   }

   if (special_vars_layout->has.draw_index) {
      input.flags |= PVR_PDS_VERTEX_FLAGS_DRAW_INDEX_REQUIRED;
      input.draw_index_register = special_vars_layout->offset.draw_index;
   }

   pvr_pds_setup_doutu(&input.usc_task_control,
                       0,
                       usc_temp_count,
                       PVRX(PDSINST_DOUTU_SAMPLE_RATE_INSTANCE),
                       false);

   /* Note: programs_out_ptr is a pointer to an array so this is fine. See the
    * typedef.
    */
   for (uint32_t i = 0; i < ARRAY_SIZE(*programs_out_ptr); i++) {
      uint32_t extra_flags;

      switch (i) {
      case PVR_PDS_VERTEX_ATTRIB_PROGRAM_BASIC:
         extra_flags = 0;
         break;

      case PVR_PDS_VERTEX_ATTRIB_PROGRAM_BASE_INSTANCE:
         extra_flags = PVR_PDS_VERTEX_FLAGS_BASE_INSTANCE_VARIANT;
         break;

      case PVR_PDS_VERTEX_ATTRIB_PROGRAM_DRAW_INDIRECT:
         extra_flags = PVR_PDS_VERTEX_FLAGS_DRAW_INDIRECT_VARIANT;
         break;

      default:
         unreachable("Invalid vertex attrib program type.");
      }

      input.flags |= extra_flags;

      result =
         pvr_pds_vertex_attrib_program_create_and_upload(device,
                                                         allocator,
                                                         &input,
                                                         &programs_out[i]);
      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            pvr_pds_vertex_attrib_program_destroy(device,
                                                  allocator,
                                                  &programs_out[j]);
         }

         return result;
      }

      input.flags &= ~extra_flags;
   }

   return VK_SUCCESS;
}

size_t pvr_pds_get_max_descriptor_upload_const_map_size_in_bytes(void)
{
   /* Maximum memory allocation needed for const map entries in
    * pvr_pds_generate_descriptor_upload_program().
    * It must be >= 688 bytes. This size is calculated as the sum of:
    *
    *  1. Max. number of descriptor sets (8) * (
    *         size of descriptor entry
    *             (pvr_const_map_entry_descriptor_set) +
    *         size of Common Store burst entry
    *             (pvr_const_map_entry_literal32))
    *
    *  2. Max. number of PDS program buffers (24) * (
    *         size of the largest buffer structure
    *             (pvr_const_map_entry_constant_buffer) +
    *         size of Common Store burst entry
    *             (pvr_const_map_entry_literal32)
    *
    *  3. Size of DOUTU entry (pvr_const_map_entry_doutu_address)
    *
    *  4. Max. number of PDS address literals (8) * (
    *         size of entry
    *             (pvr_const_map_entry_descriptor_set_addrs_table)
    *
    *  5. Max. number of address literals with single buffer entry to DOUTD
              size of entry
                  (pvr_pds_const_map_entry_addr_literal_buffer) +
              8 * size of entry (pvr_pds_const_map_entry_addr_literal)
    */

   /* FIXME: PVR_MAX_DESCRIPTOR_SETS is 4 and not 8. The comment above seems to
    * say that it should be 8.
    * Figure our a define for this or is the comment wrong?
    */
   return (8 * (sizeof(struct pvr_const_map_entry_descriptor_set) +
                sizeof(struct pvr_const_map_entry_literal32)) +
           PVR_PDS_MAX_BUFFERS *
              (sizeof(struct pvr_const_map_entry_constant_buffer) +
               sizeof(struct pvr_const_map_entry_literal32)) +
           sizeof(struct pvr_const_map_entry_doutu_address) +
           sizeof(struct pvr_pds_const_map_entry_addr_literal_buffer) +
           8 * sizeof(struct pvr_pds_const_map_entry_addr_literal));
}

static VkResult pvr_pds_descriptor_program_create_and_upload(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   const struct rogue_compile_time_consts_data *const compile_time_consts_data,
   const struct rogue_ubo_data *const ubo_data,
   const struct pvr_pipeline_layout *const layout,
   enum pvr_stage_allocation stage,
   const struct pvr_sh_reg_layout *sh_reg_layout,
   struct util_dynarray *preamble_shader,
   uint32_t sec_temp_reg_count,
   struct pvr_stage_allocation_descriptor_state *const descriptor_state)
{
   const size_t const_entries_size_in_bytes =
      pvr_pds_get_max_descriptor_upload_const_map_size_in_bytes();
   struct pvr_pds_info *const pds_info = &descriptor_state->pds_info;
   struct pvr_pds_descriptor_program_input program = { 0 };
   struct pvr_const_map_entry *new_entries;
   ASSERTED uint32_t code_size_in_dwords;
   uint32_t staging_buffer_size;
   uint32_t addr_literals = 0;
   uint32_t *staging_buffer;
   VkResult result;

   assert(stage != PVR_STAGE_ALLOCATION_COUNT);

   *pds_info = (struct pvr_pds_info){ 0 };

   u_foreach_bit (set_num, layout->per_stage_descriptor_masks[stage]) {
      const struct pvr_descriptor_set_layout_mem_layout *reg_layout =
         &layout->required_register_layout_in_dwords_per_stage[stage][set_num];

      if (layout->per_stage_required_register_usage[stage] == 0)
         continue;

      if (reg_layout->primary_size == 0)
         continue;

      program.descriptor_sets[program.descriptor_set_count] =
         (struct pvr_pds_descriptor_set){
            .descriptor_set = set_num,
            .size_in_dwords = reg_layout->primary_size,
            .destination = reg_layout->primary_offset,
            .primary = true,
            .offset_in_dwords = 0,
         };

      program.descriptor_set_count++;

      /* Point sampler (for now a buffer DOUTD) */
      if (layout->point_sampler_in_dwords_per_stage[stage] !=
          ROGUE_REG_UNUSED) {
         program.buffers[program.buffer_count] = (struct pvr_pds_buffer){
            .type = PVR_BUFFER_TYPE_POINT_SAMPLER,
            .size_in_dwords = PVR_SAMPLER_DESCRIPTOR_SIZE,
            .destination = layout->point_sampler_in_dwords_per_stage[stage],
         };
         program.buffer_count++;
      }

      if (reg_layout->secondary_size == 0)
         continue;

      program.descriptor_sets[program.descriptor_set_count] =
         (struct pvr_pds_descriptor_set){
            .descriptor_set = set_num,
            .size_in_dwords = reg_layout->secondary_size,
            .destination = reg_layout->secondary_offset,
            .primary = false,
            .offset_in_dwords = 0,
         };

      program.descriptor_set_count++;
   }

   if (sh_reg_layout->descriptor_set_addrs_table.present) {
      program.addr_literals[addr_literals] = (struct pvr_pds_addr_literal){
         .type = PVR_PDS_ADDR_LITERAL_DESC_SET_ADDRS_TABLE,
         .destination = sh_reg_layout->descriptor_set_addrs_table.offset,
      };
      addr_literals++;
   }

   if (sh_reg_layout->scratch_buffer.present) {
      program.addr_literals[addr_literals] = (struct pvr_pds_addr_literal){
         .type = PVR_PDS_ADDR_LITERAL_SCRATCH_BUFFER,
         .destination = sh_reg_layout->scratch_buffer.offset,
      };
      addr_literals++;
   }

   if (sh_reg_layout->temp_spill_buffer.present) {
      program.addr_literals[addr_literals] = (struct pvr_pds_addr_literal){
         .type = PVR_PDS_ADDR_LITERAL_TEMP_SPILL_BUFFER,
         .destination = sh_reg_layout->temp_spill_buffer.offset,
      };
      addr_literals++;

      program.addr_literals[addr_literals] = (struct pvr_pds_addr_literal){
         .type = PVR_PDS_ADDR_LITERAL_TEMP_SPILL_BUFFER_BSO,
         .destination = sh_reg_layout->temp_spill_buffer.block_size_offset,
      };
      addr_literals++;
   }

   if (sh_reg_layout->push_consts.present) {
      program.addr_literals[addr_literals] = (struct pvr_pds_addr_literal){
         .type = PVR_PDS_ADDR_LITERAL_PUSH_CONSTS,
         .destination = sh_reg_layout->push_consts.offset,
      };
      addr_literals++;
   }

   if (sh_reg_layout->blend_consts.present) {
      program.addr_literals[addr_literals] = (struct pvr_pds_addr_literal){
         .type = PVR_PDS_ADDR_LITERAL_BLEND_CONSTANTS,
         .destination = sh_reg_layout->blend_consts.offset,
      };
      addr_literals++;
   }

   if (sh_reg_layout->num_workgroups.present) {
      program.addr_literals[addr_literals] = (struct pvr_pds_addr_literal){
         .type = PVR_PDS_ADDR_LITERAL_NUM_WORKGROUPS,
         .destination = sh_reg_layout->num_workgroups.offset,
      };
      addr_literals++;
   }

   program.addr_literal_count = addr_literals;

   /* Setup preamble shader/secondary program. */
   if (preamble_shader && preamble_shader->size) {
      const uint32_t cache_line_size =
         rogue_get_slc_cache_line_size(&device->pdevice->dev_info);

      result = pvr_gpu_upload_usc(device,
                                  util_dynarray_begin(preamble_shader),
                                  preamble_shader->size,
                                  cache_line_size,
                                  &descriptor_state->usc_code);
      if (result != VK_SUCCESS)
         goto err_free_secondary_program;

      program.secondary_program_present = true;
   }

   if (program.secondary_program_present) {
      pvr_pds_setup_doutu(&program.secondary_task_control,
                          0,
                          sec_temp_reg_count,
                          PVRX(PDSINST_DOUTU_SAMPLE_RATE_INSTANCE),
                          false);
   }

   pds_info->entries = vk_alloc2(&device->vk.alloc,
                                 allocator,
                                 const_entries_size_in_bytes,
                                 8,
                                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pds_info->entries) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_free_static_consts;
   }

   pds_info->entries_size_in_bytes = const_entries_size_in_bytes;

   pvr_pds_generate_descriptor_upload_program(&program, NULL, pds_info);

   code_size_in_dwords = pds_info->code_size_in_dwords;
   staging_buffer_size = PVR_DW_TO_BYTES(pds_info->code_size_in_dwords);

   if (!staging_buffer_size) {
      vk_free2(&device->vk.alloc, allocator, pds_info->entries);

      *descriptor_state = (struct pvr_stage_allocation_descriptor_state){ 0 };

      return VK_SUCCESS;
   }

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_free_entries;
   }

   pvr_pds_generate_descriptor_upload_program(&program,
                                              staging_buffer,
                                              pds_info);

   assert(pds_info->code_size_in_dwords <= code_size_in_dwords);

   /* FIXME: use vk_realloc2() ? */
   new_entries = vk_realloc((!allocator) ? &device->vk.alloc : allocator,
                            pds_info->entries,
                            pds_info->entries_written_size_in_bytes,
                            8,
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!new_entries) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_free_staging_buffer;
   }

   pds_info->entries = new_entries;
   pds_info->entries_size_in_bytes = pds_info->entries_written_size_in_bytes;

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               NULL,
                               0,
                               0,
                               staging_buffer,
                               pds_info->code_size_in_dwords,
                               16,
                               16,
                               &descriptor_state->pds_code);
   if (result != VK_SUCCESS)
      goto err_free_staging_buffer;

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;

err_free_staging_buffer:
   vk_free2(&device->vk.alloc, allocator, staging_buffer);

err_free_entries:
   vk_free2(&device->vk.alloc, allocator, pds_info->entries);

err_free_static_consts:
   pvr_bo_suballoc_free(descriptor_state->static_consts);

err_free_secondary_program:
   pvr_bo_suballoc_free(descriptor_state->usc_code);

   return result;
}

static void pvr_pds_descriptor_program_destroy(
   struct pvr_device *const device,
   const struct VkAllocationCallbacks *const allocator,
   struct pvr_stage_allocation_descriptor_state *const descriptor_state)
{
   if (!descriptor_state)
      return;

   pvr_bo_suballoc_free(descriptor_state->pds_code.pvr_bo);
   vk_free2(&device->vk.alloc, allocator, descriptor_state->pds_info.entries);
   pvr_bo_suballoc_free(descriptor_state->static_consts);
   pvr_bo_suballoc_free(descriptor_state->usc_code);
}

static void pvr_pds_compute_program_setup(
   const struct pvr_device_info *dev_info,
   const uint32_t local_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   const uint32_t work_group_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   uint32_t barrier_coefficient,
   bool add_base_workgroup,
   uint32_t usc_temps,
   pvr_dev_addr_t usc_shader_dev_addr,
   struct pvr_pds_compute_shader_program *const program)
{
   pvr_pds_compute_shader_program_init(program);
   program->local_input_regs[0] = local_input_regs[0];
   program->local_input_regs[1] = local_input_regs[1];
   program->local_input_regs[2] = local_input_regs[2];
   program->work_group_input_regs[0] = work_group_input_regs[0];
   program->work_group_input_regs[1] = work_group_input_regs[1];
   program->work_group_input_regs[2] = work_group_input_regs[2];
   program->barrier_coefficient = barrier_coefficient;
   program->add_base_workgroup = add_base_workgroup;
   program->flattened_work_groups = true;
   program->kick_usc = true;

   STATIC_ASSERT(ARRAY_SIZE(program->local_input_regs) ==
                 PVR_WORKGROUP_DIMENSIONS);
   STATIC_ASSERT(ARRAY_SIZE(program->work_group_input_regs) ==
                 PVR_WORKGROUP_DIMENSIONS);
   STATIC_ASSERT(ARRAY_SIZE(program->global_input_regs) ==
                 PVR_WORKGROUP_DIMENSIONS);

   pvr_pds_setup_doutu(&program->usc_task_control,
                       usc_shader_dev_addr.addr,
                       usc_temps,
                       PVRX(PDSINST_DOUTU_SAMPLE_RATE_INSTANCE),
                       false);

   pvr_pds_compute_shader(program, NULL, PDS_GENERATE_SIZES, dev_info);
}

/* FIXME: See if pvr_device_init_compute_pds_program() and this could be merged.
 */
static VkResult pvr_pds_compute_program_create_and_upload(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   const uint32_t local_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   const uint32_t work_group_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   uint32_t barrier_coefficient,
   uint32_t usc_temps,
   pvr_dev_addr_t usc_shader_dev_addr,
   struct pvr_pds_upload *const pds_upload_out,
   struct pvr_pds_info *const pds_info_out)
{
   struct pvr_device_info *dev_info = &device->pdevice->dev_info;
   struct pvr_pds_compute_shader_program program;
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   pvr_pds_compute_program_setup(dev_info,
                                 local_input_regs,
                                 work_group_input_regs,
                                 barrier_coefficient,
                                 false,
                                 usc_temps,
                                 usc_shader_dev_addr,
                                 &program);

   /* FIXME: According to pvr_device_init_compute_pds_program() the code size
    * is in bytes. Investigate this.
    */
   staging_buffer_size = PVR_DW_TO_BYTES(program.code_size + program.data_size);

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* FIXME: pvr_pds_compute_shader doesn't implement
    * PDS_GENERATE_CODEDATA_SEGMENTS.
    */
   pvr_pds_compute_shader(&program,
                          &staging_buffer[0],
                          PDS_GENERATE_CODE_SEGMENT,
                          dev_info);

   pvr_pds_compute_shader(&program,
                          &staging_buffer[program.code_size],
                          PDS_GENERATE_DATA_SEGMENT,
                          dev_info);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               &staging_buffer[program.code_size],
                               program.data_size,
                               16,
                               &staging_buffer[0],
                               program.code_size,
                               16,
                               16,
                               pds_upload_out);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, staging_buffer);
      return result;
   }

   *pds_info_out = (struct pvr_pds_info){
      .temps_required = program.highest_temp,
      .code_size_in_dwords = program.code_size,
      .data_size_in_dwords = program.data_size,
   };

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;
};

static void pvr_pds_compute_program_destroy(
   struct pvr_device *const device,
   const struct VkAllocationCallbacks *const allocator,
   struct pvr_pds_upload *const pds_program,
   struct pvr_pds_info *const pds_info)
{
   /* We don't allocate an entries buffer so we don't need to free it */
   pvr_bo_suballoc_free(pds_program->pvr_bo);
}

/* This only uploads the code segment. The data segment will need to be patched
 * with the base workgroup before uploading.
 */
static VkResult pvr_pds_compute_base_workgroup_variant_program_init(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   const uint32_t local_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   const uint32_t work_group_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   uint32_t barrier_coefficient,
   uint32_t usc_temps,
   pvr_dev_addr_t usc_shader_dev_addr,
   struct pvr_pds_base_workgroup_program *program_out)
{
   struct pvr_device_info *dev_info = &device->pdevice->dev_info;
   struct pvr_pds_compute_shader_program program;
   uint32_t buffer_size;
   uint32_t *buffer;
   VkResult result;

   pvr_pds_compute_program_setup(dev_info,
                                 local_input_regs,
                                 work_group_input_regs,
                                 barrier_coefficient,
                                 true,
                                 usc_temps,
                                 usc_shader_dev_addr,
                                 &program);

   /* FIXME: According to pvr_device_init_compute_pds_program() the code size
    * is in bytes. Investigate this.
    */
   buffer_size = PVR_DW_TO_BYTES(MAX2(program.code_size, program.data_size));

   buffer = vk_alloc2(&device->vk.alloc,
                      allocator,
                      buffer_size,
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pvr_pds_compute_shader(&program,
                          &buffer[0],
                          PDS_GENERATE_CODE_SEGMENT,
                          dev_info);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               NULL,
                               0,
                               0,
                               buffer,
                               program.code_size,
                               16,
                               16,
                               &program_out->code_upload);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, buffer);
      return result;
   }

   pvr_pds_compute_shader(&program, buffer, PDS_GENERATE_DATA_SEGMENT, dev_info);

   program_out->data_section = buffer;

   /* We'll need to patch the base workgroup in the PDS data section before
    * dispatch so we save the offsets at which to patch. We only need to save
    * the offset for the first workgroup id since the workgroup ids are stored
    * contiguously in the data segment.
    */
   program_out->base_workgroup_data_patching_offset =
      program.base_workgroup_constant_offset_in_dwords[0];

   program_out->info = (struct pvr_pds_info){
      .temps_required = program.highest_temp,
      .code_size_in_dwords = program.code_size,
      .data_size_in_dwords = program.data_size,
   };

   return VK_SUCCESS;
}

static void pvr_pds_compute_base_workgroup_variant_program_finish(
   struct pvr_device *device,
   const VkAllocationCallbacks *const allocator,
   struct pvr_pds_base_workgroup_program *const state)
{
   pvr_bo_suballoc_free(state->code_upload.pvr_bo);
   vk_free2(&device->vk.alloc, allocator, state->data_section);
}

/******************************************************************************
   Generic pipeline functions
 ******************************************************************************/

static void pvr_pipeline_init(struct pvr_device *device,
                              enum pvr_pipeline_type type,
                              struct pvr_pipeline *const pipeline)
{
   assert(!pipeline->layout);

   vk_object_base_init(&device->vk, &pipeline->base, VK_OBJECT_TYPE_PIPELINE);

   pipeline->type = type;
}

static void pvr_pipeline_finish(struct pvr_pipeline *pipeline)
{
   vk_object_base_finish(&pipeline->base);
}

/* How many shared regs it takes to store a pvr_dev_addr_t.
 * Each shared reg is 32 bits.
 */
#define PVR_DEV_ADDR_SIZE_IN_SH_REGS \
   DIV_ROUND_UP(sizeof(pvr_dev_addr_t), sizeof(uint32_t))

/**
 * \brief Allocates shared registers.
 *
 * \return How many sh regs are required.
 */
static uint32_t
pvr_pipeline_alloc_shareds(const struct pvr_device *device,
                           const struct pvr_pipeline_layout *layout,
                           enum pvr_stage_allocation stage,
                           bool uses_scratch,
                           bool uses_spilling,
                           struct pvr_sh_reg_layout *const sh_reg_layout_out)
{
   ASSERTED const uint64_t reserved_shared_size =
      device->pdevice->dev_runtime_info.reserved_shared_size;
   ASSERTED const uint64_t max_coeff =
      device->pdevice->dev_runtime_info.max_coeffs;

   struct pvr_sh_reg_layout reg_layout = { 0 };
   uint32_t next_free_sh_reg = 0;

   /* Reserve space for the descriptors required to be uploaded at the beginning
    * of the shareds. This simplifies the code for locating the shareds utilized
    * by a specific binding.
    */
   next_free_sh_reg += layout->per_stage_required_register_usage[stage];

   /* Reserve space for the descriptor set device address table base. */
   reg_layout.descriptor_set_addrs_table.present =
      !!(layout->shader_stage_mask & BITFIELD_BIT(stage));
   reg_layout.descriptor_set_addrs_table.present = true;

   if (reg_layout.descriptor_set_addrs_table.present) {
      reg_layout.descriptor_set_addrs_table.offset = next_free_sh_reg;
      next_free_sh_reg += PVR_DEV_ADDR_SIZE_IN_SH_REGS;
   }

   /* Reserve space for the scratch buffer base address. */
   /* TODO: Make this conditional. */
   reg_layout.scratch_buffer.present = uses_scratch;

   if (reg_layout.scratch_buffer.present) {
      reg_layout.scratch_buffer.offset = next_free_sh_reg;
      next_free_sh_reg += PVR_DEV_ADDR_SIZE_IN_SH_REGS;
   }

   /* Reserve space for the temp spilling base address. */
   /* TODO: Make this conditional. */
   reg_layout.temp_spill_buffer.present = uses_spilling;

   if (reg_layout.temp_spill_buffer.present) {
      reg_layout.temp_spill_buffer.offset = next_free_sh_reg;
      next_free_sh_reg += PVR_DEV_ADDR_SIZE_IN_SH_REGS;

      reg_layout.temp_spill_buffer.block_size_offset = next_free_sh_reg;
      next_free_sh_reg += PVR_DEV_ADDR_SIZE_IN_SH_REGS;
   }

   /* Reserve space for the push constants base device address. */
   reg_layout.push_consts.present =
      !!(layout->push_constants_shader_stages & BITFIELD_BIT(stage));
   reg_layout.push_consts.present = true;

   if (reg_layout.push_consts.present) {
      reg_layout.push_consts.offset = next_free_sh_reg;
      next_free_sh_reg += PVR_DEV_ADDR_SIZE_IN_SH_REGS;
   }

   *sh_reg_layout_out = reg_layout;

   /* FIXME: We might need to take more things into consideration.
    * See pvr_calc_fscommon_size_and_tiles_in_flight().
    */
   assert(next_free_sh_reg <= reserved_shared_size - max_coeff);

   return next_free_sh_reg;
}

/******************************************************************************
   Compute pipeline functions
 ******************************************************************************/

/**
 * \brief Allocates the coefficient registers for a compute pipeline.
 *
 * \param[in,out] cs_data Compiler build data and shader info for compute.
 * \return Amount of coefficient registers allocated.
 */
static uint32_t
pvr_compute_pipeline_alloc_coeffs(struct rogue_cs_build_data *cs_data)
{
   uint32_t next_free_reg = 0;

   if (cs_data->has.barrier) {
      cs_data->barrier_reg = next_free_reg;
      next_free_reg += 8;
   } else {
      cs_data->barrier_reg = ROGUE_REG_UNUSED;
   }

   /* Workgroup ID regs need to be 128-bit (4-byte) aligned. */
   next_free_reg = ALIGN_POT(next_free_reg, 4);

   if (cs_data->has.work_group_id_x)
      cs_data->workgroup_regs[0] = next_free_reg++;
   else
      cs_data->workgroup_regs[0] = ROGUE_REG_UNUSED;

   if (cs_data->has.work_group_id_y)
      cs_data->workgroup_regs[1] = next_free_reg++;
   else
      cs_data->workgroup_regs[1] = ROGUE_REG_UNUSED;

   if (cs_data->has.work_group_id_z)
      cs_data->workgroup_regs[2] = next_free_reg++;
   else
      cs_data->workgroup_regs[2] = ROGUE_REG_UNUSED;

   /* Align shared/local memory allocation.
    * TODO: Check if this is actually needed.
    */
   next_free_reg = ALIGN_POT(next_free_reg, 4);

   /* Allocate shared/local memory. */
   if (cs_data->has.shmem_bytes) {
      cs_data->shmem_offset = next_free_reg;
      next_free_reg += cs_data->has.shmem_bytes;
   } else {
      cs_data->shmem_offset = ROGUE_REG_UNUSED;
   }

   /* Align shared/local memory size.
    * TODO: Check if this is actually needed.
    */
   next_free_reg = ALIGN_POT(next_free_reg, 4);

   return next_free_reg;
}

/**
 * \brief Allocates the vertex input registers for a compute pipeline.
 *
 * Since compute shaders can't have any user defined input variables, only
 * gl_LocalInvocationID needs to be accounted for.
 *
 * \param[in,out] cs_data Compiler build data and shader info for compute.
 * \return Amount of vertex input registers allocated.
 */
static uint32_t
pvr_compute_pipeline_alloc_vtx_ins(struct rogue_cs_build_data *cs_data)
{
   uint32_t next_free_reg = 0;

   if (cs_data->has.location_id_x)
      cs_data->local_id_regs[0] = next_free_reg++;
   else
      cs_data->local_id_regs[0] = ROGUE_REG_UNUSED;

   /* gl_LocalInvocationID.Y and gl_LocalInvocationID.Z come pre-packed in
    * a PDS temp so they get allocated a single register. They'll be
    * unpacked in the shader.
    */
   if (cs_data->has.location_id_y_or_z)
      cs_data->local_id_regs[1] = next_free_reg++;
   else
      cs_data->local_id_regs[1] = ROGUE_REG_UNUSED;

   static_assert(ARRAY_SIZE(cs_data->local_id_regs) == 2,
                 "Y and Z are packed and should be using the same reg.");

   return next_free_reg;
}

static uint32_t pvr_compute_pipeline_alloc_shareds(
   const struct pvr_device *device,
   const struct pvr_compute_pipeline *compute_pipeline,
   struct pvr_sh_reg_layout *const sh_reg_layout_out)
{
   ASSERTED const uint64_t reserved_shared_size =
      device->pdevice->dev_runtime_info.reserved_shared_size;
   ASSERTED const uint64_t max_coeff =
      device->pdevice->dev_runtime_info.max_coeffs;

   const struct pvr_pipeline_layout *layout = compute_pipeline->base.layout;
   struct pvr_sh_reg_layout reg_layout = { 0 };
   uint32_t next_free_sh_reg = 0;

   next_free_sh_reg = pvr_pipeline_alloc_shareds(device,
                                                 layout,
                                                 PVR_STAGE_ALLOCATION_COMPUTE,
                                                 compute_pipeline->shader_state.scratch_size > 0,
                                                 true, /* Always assume true for now since we don't know the spill size yet */
                                                 &reg_layout);

   reg_layout.num_workgroups.present =
      compute_pipeline->shader_state.uses_num_workgroups;
   reg_layout.num_workgroups.present = true;

   if (reg_layout.num_workgroups.present) {
      reg_layout.num_workgroups.offset = next_free_sh_reg;
      next_free_sh_reg += PVR_DEV_ADDR_SIZE_IN_SH_REGS;
   }

   *sh_reg_layout_out = reg_layout;

   /* FIXME: We might need to take more things into consideration.
    * See pvr_calc_fscommon_size_and_tiles_in_flight().
    */
   assert(next_free_sh_reg <= reserved_shared_size - max_coeff);

   return next_free_sh_reg;
}

static nir_shader *
pvr_pipeline_spirv_to_nir(rogue_build_ctx *ctx,
                          gl_shader_stage stage,
                          const VkPipelineShaderStageCreateInfo *create_info)
{
   VK_FROM_HANDLE(vk_shader_module, module, create_info->module);
   struct nir_spirv_specialization *spec;
   unsigned num_spec = 0;
   nir_shader *nir;

   spec =
      vk_spec_info_to_nir_spirv(create_info->pSpecializationInfo, &num_spec);

   nir = rogue_spirv_to_nir(ctx,
                            stage,
                            create_info->pName,
                            module->size / sizeof(uint32_t),
                            (uint32_t *)module->data,
                            num_spec,
                            spec);

   free(spec);

   return nir;
}

/* Compiles and uploads shaders and PDS programs. */
static VkResult pvr_compute_pipeline_compile(
   struct pvr_device *const device,
   struct vk_pipeline_cache *cache,
   const VkComputePipelineCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *const allocator,
   struct pvr_compute_pipeline *const compute_pipeline)
{
   const uint64_t max_coeffs = device->pdevice->dev_runtime_info.max_coeffs;
   const uint32_t cache_line_size =
      rogue_get_slc_cache_line_size(&device->pdevice->dev_info);
   struct pvr_pipeline_layout *layout = compute_pipeline->base.layout;
   struct pvr_sh_reg_layout *sh_reg_layout =
      &layout->sh_reg_layout_per_stage[PVR_STAGE_ALLOCATION_COMPUTE];
   struct rogue_compile_time_consts_data compile_time_consts_data;
   struct rogue_compiler *compiler = device->pdevice->compiler;
   uint32_t local_input_regs[PVR_WORKGROUP_DIMENSIONS];
   const gl_shader_stage stage = MESA_SHADER_COMPUTE;
   rogue_common_build_data *common_data;
   struct rogue_cs_build_data *cs_data;
   struct rogue_build_ctx *ctx;
   uint32_t reg_count;
   VkResult result;

   ctx = rogue_build_context_create(compiler, layout);
   if (!ctx)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* NIR middle-end translation. */
   ctx->nir[stage] = pvr_pipeline_spirv_to_nir(ctx, stage, &pCreateInfo->stage);
   if (!ctx->nir[stage]) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_free_build_context;
   }

   rogue_nir_preprocess(ctx, true);
   rogue_nir_link(ctx, true);
   rogue_nir_lower(ctx, true);
   rogue_nir_postprocess(ctx, true);

   cs_data = &ctx->stage_data.cs;
   common_data = &ctx->common_data[stage];

   compute_pipeline->shader_state.uses_atomic_ops = cs_data->has.atomic_ops;
   compute_pipeline->shader_state.uses_barrier = cs_data->has.barrier;
   compute_pipeline->shader_state.uses_num_workgroups =
      cs_data->has.num_work_groups;
   compute_pipeline->shader_state.work_size = cs_data->work_size;

   compute_pipeline->shader_state.scratch_size = common_data->scratch_size;

   reg_count = pvr_compute_pipeline_alloc_shareds(device,
                                                  compute_pipeline,
                                                  sh_reg_layout);
   compute_pipeline->shader_state.const_shared_reg_count = reg_count;

   reg_count = pvr_compute_pipeline_alloc_coeffs(cs_data);
   assert(reg_count < max_coeffs);
   compute_pipeline->shader_state.coefficient_register_count = reg_count;

   reg_count = pvr_compute_pipeline_alloc_vtx_ins(cs_data);
   /* Safe estimate is 12. Absolute minimum would be 8, slightly less safe estimate would be 24. */
   reg_count = 12;
   /* reg_count = 24; */
   compute_pipeline->shader_state.input_register_count = reg_count;
   cs_data->vtxin_regs = ALIGN_POT(reg_count, 4); /* Allocated in blocks of 4. */

   /* We make sure that the compiler's unused reg value is compatible with
    * the pds api.
    */
   STATIC_ASSERT(ROGUE_REG_UNUSED == PVR_PDS_COMPUTE_INPUT_REG_UNUSED);

   if (!cs_data->has.work_group_id_x)
      assert(cs_data->workgroup_regs[0] == PVR_PDS_COMPUTE_INPUT_REG_UNUSED);

   if (!cs_data->has.work_group_id_y)
      assert(cs_data->workgroup_regs[1] == PVR_PDS_COMPUTE_INPUT_REG_UNUSED);

   if (!cs_data->has.work_group_id_z)
      assert(cs_data->workgroup_regs[2] == PVR_PDS_COMPUTE_INPUT_REG_UNUSED);

   if (!cs_data->has.barrier)
      assert(cs_data->barrier_reg == PVR_PDS_COMPUTE_INPUT_REG_UNUSED);

   /* TODO: Get rid of this copy when removing the hard coding path. */
   local_input_regs[0] = cs_data->local_id_regs[0];
   local_input_regs[1] = cs_data->local_id_regs[1];
   /* Y and Z are packed. */
   local_input_regs[2] = cs_data->local_id_regs[1];

   rogue_nir_build(ctx, true);

   /* Force buffer to be > 0 even if unused as PDS doesn't like gaps. */
   compute_pipeline->shader_state.spill_size = common_data->spill_regs ? common_data->spill_regs * 4 : 4;
#if 0
   sh_reg_layout->temp_spill_buffer.present = common_data->spill_regs > 0;
#endif


   /* compute_pipeline->shader_state.spill_buffer_size = PVR_DW_TO_BYTES(ctx->common_data[stage].spill_state.dwords); */

   result = pvr_gpu_upload_usc(device,
                               util_dynarray_begin(&ctx->binary[stage]),
                               ctx->binary[stage].size,
                               cache_line_size,
                               &compute_pipeline->shader_state.bo);
   if (result != VK_SUCCESS)
      goto err_free_build_context;

   result = pvr_pds_descriptor_program_create_and_upload(
      device,
      allocator,
      &compile_time_consts_data,
      &common_data->ubo_data,
      layout,
      PVR_STAGE_ALLOCATION_COMPUTE,
      sh_reg_layout,
      NULL,
      0,
      &compute_pipeline->descriptor_state);
   if (result != VK_SUCCESS)
      goto err_free_shader;

   result = pvr_pds_compute_program_create_and_upload(
      device,
      allocator,
      local_input_regs,
      cs_data->workgroup_regs,
      cs_data->barrier_reg,
      common_data->temps,
      compute_pipeline->shader_state.bo->dev_addr,
      &compute_pipeline->primary_program,
      &compute_pipeline->primary_program_info);
   if (result != VK_SUCCESS)
      goto err_free_descriptor_program;

   /* If the workgroup ID is required, then we require the base workgroup
    * variant of the PDS compute program as well.
    */
   compute_pipeline->flags.base_workgroup = cs_data->has.work_group_id_x ||
                                            cs_data->has.work_group_id_y ||
                                            cs_data->has.work_group_id_z;

   if (compute_pipeline->flags.base_workgroup) {
      result = pvr_pds_compute_base_workgroup_variant_program_init(
         device,
         allocator,
         local_input_regs,
         cs_data->workgroup_regs,
         cs_data->barrier_reg,
         common_data->temps,
         compute_pipeline->shader_state.bo->dev_addr,
         &compute_pipeline->primary_base_workgroup_variant_program);
      if (result != VK_SUCCESS)
         goto err_destroy_compute_program;
   }

   ralloc_free(ctx);

   return VK_SUCCESS;

err_destroy_compute_program:
   pvr_pds_compute_program_destroy(device,
                                   allocator,
                                   &compute_pipeline->primary_program,
                                   &compute_pipeline->primary_program_info);

err_free_descriptor_program:
   pvr_pds_descriptor_program_destroy(device,
                                      allocator,
                                      &compute_pipeline->descriptor_state);

err_free_shader:
   pvr_bo_suballoc_free(compute_pipeline->shader_state.bo);

err_free_build_context:
   ralloc_free(ctx);

   return result;
}

static VkResult
pvr_compute_pipeline_init(struct pvr_device *device,
                          struct vk_pipeline_cache *cache,
                          const VkComputePipelineCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *allocator,
                          struct pvr_compute_pipeline *compute_pipeline)
{
   VkResult result;

   pvr_pipeline_init(device,
                     PVR_PIPELINE_TYPE_COMPUTE,
                     &compute_pipeline->base);

   compute_pipeline->base.layout =
      pvr_pipeline_layout_from_handle(pCreateInfo->layout);

   result = pvr_compute_pipeline_compile(device,
                                         cache,
                                         pCreateInfo,
                                         allocator,
                                         compute_pipeline);
   if (result != VK_SUCCESS) {
      pvr_pipeline_finish(&compute_pipeline->base);
      return result;
   }

   return VK_SUCCESS;
}

static VkResult
pvr_compute_pipeline_create(struct pvr_device *device,
                            struct vk_pipeline_cache *cache,
                            const VkComputePipelineCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *allocator,
                            VkPipeline *const pipeline_out)
{
   struct pvr_compute_pipeline *compute_pipeline;
   VkResult result;

   compute_pipeline = vk_zalloc2(&device->vk.alloc,
                                 allocator,
                                 sizeof(*compute_pipeline),
                                 8,
                                 VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!compute_pipeline)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Compiles and uploads shaders and PDS programs. */
   result = pvr_compute_pipeline_init(device,
                                      cache,
                                      pCreateInfo,
                                      allocator,
                                      compute_pipeline);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, compute_pipeline);
      return result;
   }

   *pipeline_out = pvr_pipeline_to_handle(&compute_pipeline->base);

   return VK_SUCCESS;
}

static void pvr_compute_pipeline_destroy(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   struct pvr_compute_pipeline *const compute_pipeline)
{
   if (compute_pipeline->flags.base_workgroup) {
      pvr_pds_compute_base_workgroup_variant_program_finish(
         device,
         allocator,
         &compute_pipeline->primary_base_workgroup_variant_program);
   }

   pvr_pds_compute_program_destroy(device,
                                   allocator,
                                   &compute_pipeline->primary_program,
                                   &compute_pipeline->primary_program_info);
   pvr_pds_descriptor_program_destroy(device,
                                      allocator,
                                      &compute_pipeline->descriptor_state);
   pvr_bo_suballoc_free(compute_pipeline->shader_state.bo);

   pvr_pipeline_finish(&compute_pipeline->base);

   vk_free2(&device->vk.alloc, allocator, compute_pipeline);
}

VkResult
pvr_CreateComputePipelines(VkDevice _device,
                           VkPipelineCache pipelineCache,
                           uint32_t createInfoCount,
                           const VkComputePipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);
   PVR_FROM_HANDLE(pvr_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      const VkResult local_result =
         pvr_compute_pipeline_create(device,
                                     cache,
                                     &pCreateInfos[i],
                                     pAllocator,
                                     &pPipelines[i]);
      if (local_result != VK_SUCCESS) {
         result = local_result;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   return result;
}

/******************************************************************************
   Graphics pipeline functions
 ******************************************************************************/

static void
pvr_graphics_pipeline_destroy(struct pvr_device *const device,
                              const VkAllocationCallbacks *const allocator,
                              struct pvr_graphics_pipeline *const gfx_pipeline)
{
   const uint32_t num_vertex_attrib_programs =
      ARRAY_SIZE(gfx_pipeline->shader_state.vertex.pds_attrib_programs);

   pvr_pds_descriptor_program_destroy(
      device,
      allocator,
      &gfx_pipeline->shader_state.fragment.descriptor_state);

   pvr_pds_descriptor_program_destroy(
      device,
      allocator,
      &gfx_pipeline->shader_state.vertex.descriptor_state);

   for (uint32_t i = 0; i < num_vertex_attrib_programs; i++) {
      struct pvr_pds_attrib_program *const attrib_program =
         &gfx_pipeline->shader_state.vertex.pds_attrib_programs[i];

      pvr_pds_vertex_attrib_program_destroy(device, allocator, attrib_program);
   }

   pvr_bo_suballoc_free(
      gfx_pipeline->shader_state.fragment.pds_fragment_program.pvr_bo);
   pvr_bo_suballoc_free(
      gfx_pipeline->shader_state.fragment.pds_coeff_program.pvr_bo);

   pvr_bo_suballoc_free(gfx_pipeline->shader_state.fragment.bo);
   pvr_bo_suballoc_free(gfx_pipeline->shader_state.vertex.bo);

   pvr_pipeline_finish(&gfx_pipeline->base);

   vk_free2(&device->vk.alloc, allocator, gfx_pipeline);
}

static void
pvr_vertex_state_init(struct pvr_graphics_pipeline *gfx_pipeline,
                      const struct rogue_common_build_data *common_data,
                      uint32_t vtxin_regs_used,
                      const struct rogue_vs_build_data *vs_data)
{
   struct pvr_vertex_shader_state *vertex_state =
      &gfx_pipeline->shader_state.vertex;

   /* TODO: Hard coding these for now. These should be populated based on the
    * information returned by the compiler.
    */
   vertex_state->stage_state.const_shared_reg_count = common_data->shareds;
   vertex_state->stage_state.const_shared_reg_offset = 0;
   vertex_state->stage_state.coefficient_size = common_data->coeffs;
   vertex_state->stage_state.uses_atomic_ops = vs_data->has.atomic_ops;
   vertex_state->stage_state.uses_texture_rw = false;
   vertex_state->stage_state.uses_barrier = vs_data->has.barrier;
   vertex_state->stage_state.has_side_effects = vs_data->side_effects;
   vertex_state->stage_state.empty_program = false;

   vertex_state->stage_state.scratch_size = common_data->scratch_size;
   /* Force buffer to be > 0 even if unused as PDS doesn't like gaps. */
   vertex_state->stage_state.spill_size = common_data->spill_regs ? common_data->spill_regs * 4 : 4;

   /* This ends up unused since we'll use the temp_usage for the PDS program we
    * end up selecting, and the descriptor PDS program doesn't use any temps.
    * Let's set it to ~0 in case it ever gets used.
    */
   vertex_state->stage_state.pds_temps_count = ~0;

   vertex_state->vertex_input_size = vs_data->num_vertex_input_regs;
   vertex_state->vertex_output_size =
      vs_data->num_vertex_outputs * ROGUE_REG_SIZE_BYTES;

   vertex_state->psprite_present = vs_data->outputs.point_size_index != ~0;
   vertex_state->layer_present = vs_data->outputs.layer_index != ~0;
   vertex_state->viewport_present = vs_data->outputs.viewport_index != ~0;
   for (int i = 0; i < 8; i++) {
      vertex_state->clip_present[i / 4][i % 4] =
         vs_data->outputs.clip_index[i / 4][i % 4] != ~0;
      vertex_state->cull_present[i / 4][i % 4] =
         vs_data->outputs.cull_index[i / 4][i % 4] != ~0;
   }

   vertex_state->user_clip_planes_mask = 0;
   vertex_state->entry_offset = 0;

   /* TODO: The number of varyings should be checked against the fragment
    * shader inputs and assigned in the place where that happens.
    * There will also be an opportunity to cull unused fs inputs/vs outputs.
    */
   pvr_csb_pack (&gfx_pipeline->shader_state.vertex.varying[0],
                 TA_STATE_VARYING0,
                 varying0) {
      varying0.f32_linear = vs_data->num_f32_linear_varyings;
      varying0.f32_flat = vs_data->num_f32_flat_varyings;
      varying0.f32_npc = vs_data->num_f32_npc_varyings;
   }

   pvr_csb_pack (&gfx_pipeline->shader_state.vertex.varying[1],
                 TA_STATE_VARYING1,
                 varying1) {
      varying1.f16_linear = vs_data->num_f16_linear_varyings;
      varying1.f16_flat = vs_data->num_f16_flat_varyings;
      varying1.f16_npc = vs_data->num_f16_npc_varyings;
   }

   /* vertex_state->spill_buffer_size = PVR_DW_TO_BYTES(common_data->spill_state.dwords); */
}

static void
pvr_fragment_state_init(struct pvr_graphics_pipeline *gfx_pipeline,
                        const struct rogue_common_build_data *common_data,
                        const struct rogue_fs_build_data *fs_data)
{
   struct pvr_fragment_shader_state *fragment_state =
      &gfx_pipeline->shader_state.fragment;

   /* TODO: Hard coding these for now. These should be populated based on the
    * information returned by the compiler.
    */
   fragment_state->stage_state.const_shared_reg_count = 0;
   fragment_state->stage_state.const_shared_reg_offset = 0;
   fragment_state->stage_state.coefficient_size = common_data->coeffs;
   fragment_state->stage_state.uses_atomic_ops = fs_data->has.atomic_ops;
   fragment_state->stage_state.uses_texture_rw = false;
   fragment_state->stage_state.uses_barrier = fs_data->has.barrier;
   fragment_state->stage_state.has_side_effects = fs_data->side_effects;
   fragment_state->stage_state.empty_program = false;

   fragment_state->stage_state.scratch_size = common_data->scratch_size;
   /* Force buffer to be > 0 even if unused as PDS doesn't like gaps. */
   fragment_state->stage_state.spill_size = common_data->spill_regs ? common_data->spill_regs * 4 : 4;

   fragment_state->needs_iterated_depth = fs_data->iterator_args.iterates_depth;

   /* TODO: handle other cases. */
   if (fs_data->depth_feedback && !fs_data->early_fragment_tests)
      fragment_state->pass_type = PVRX(TA_PASSTYPE_DEPTH_FEEDBACK);
   else if (fs_data->discard)
      fragment_state->pass_type = PVRX(TA_PASSTYPE_PUNCH_THROUGH);
   else if (fs_data->translucent)
      fragment_state->pass_type = PVRX(TA_PASSTYPE_TRANSLUCENT);
   else
      fragment_state->pass_type = PVRX(TA_PASSTYPE_OPAQUE);

   fragment_state->entry_offset = 0;

   /* We can't initialize it yet since we still need to generate the PDS
    * programs so set it to `~0` to make sure that we set this up later on.
    */
   fragment_state->stage_state.pds_temps_count = ~0;

   /* fragment_state->spill_buffer_size = PVR_DW_TO_BYTES(common_data->spill_state.dwords); */
}

static bool pvr_blend_factor_requires_consts(VkBlendFactor factor)
{
   switch (factor) {
   case VK_BLEND_FACTOR_CONSTANT_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
   case VK_BLEND_FACTOR_CONSTANT_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
      return true;

   default:
      return false;
   }
}

/** \defgroup Graphics pipeline register allocation.
 * Functions covering shared, coefficient, and vertex input register allocation
 * setting up appropriate info to be passed into the compiler.
 * @{
 */

/**
 * \brief Indicates whether dynamic blend constants are needed.
 *
 * If the user has specified the blend constants to be dynamic, they might not
 * necessarily be using them. This function makes sure that they are being used
 * in order to determine whether we need to upload them later on for the shader
 * to access them.
 */
static bool pvr_graphics_pipeline_requires_dynamic_blend_consts(
   const struct pvr_graphics_pipeline *gfx_pipeline)
{
   const struct vk_dynamic_graphics_state *const state =
      &gfx_pipeline->dynamic_state;

   if (BITSET_TEST(state->set, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS))
      return false;

   for (uint32_t i = 0; i < state->cb.attachment_count; i++) {
      const struct vk_color_blend_attachment_state *attachment =
         &state->cb.attachments[i];

      const bool has_color_write =
         attachment->write_mask &
         (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT);
      const bool has_alpha_write = attachment->write_mask &
                                   VK_COLOR_COMPONENT_A_BIT;

      if (!attachment->blend_enable || attachment->write_mask == 0)
         continue;

      if (has_color_write) {
         const uint8_t src_color_blend_factor =
            attachment->src_color_blend_factor;
         const uint8_t dst_color_blend_factor =
            attachment->dst_color_blend_factor;

         if (pvr_blend_factor_requires_consts(src_color_blend_factor) ||
             pvr_blend_factor_requires_consts(dst_color_blend_factor)) {
            return true;
         }
      }

      if (has_alpha_write) {
         const uint8_t src_alpha_blend_factor =
            attachment->src_alpha_blend_factor;
         const uint8_t dst_alpha_blend_factor =
            attachment->dst_alpha_blend_factor;

         if (pvr_blend_factor_requires_consts(src_alpha_blend_factor) ||
             pvr_blend_factor_requires_consts(dst_alpha_blend_factor)) {
            return true;
         }
      }
   }

   return false;
}

static uint32_t pvr_graphics_pipeline_alloc_shareds(
   const struct pvr_device *device,
   const struct pvr_graphics_pipeline *gfx_pipeline,
   enum pvr_stage_allocation stage,
   unsigned preamble_shareds,
   bool uses_scratch,
   bool uses_spilling,
   struct pvr_sh_reg_layout *const sh_reg_layout_out)
{
   ASSERTED const uint64_t reserved_shared_size =
      device->pdevice->dev_runtime_info.reserved_shared_size;
   ASSERTED const uint64_t max_coeff =
      device->pdevice->dev_runtime_info.max_coeffs;

   const struct pvr_pipeline_layout *layout = gfx_pipeline->base.layout;
   struct pvr_sh_reg_layout reg_layout = { 0 };
   uint32_t next_free_sh_reg = 0;

   next_free_sh_reg =
      pvr_pipeline_alloc_shareds(device, layout, stage, uses_scratch, uses_spilling, &reg_layout);

   reg_layout.blend_consts.present =
      (stage == PVR_STAGE_ALLOCATION_FRAGMENT &&
       pvr_graphics_pipeline_requires_dynamic_blend_consts(gfx_pipeline));
   reg_layout.blend_consts.present = true;

   if (reg_layout.blend_consts.present) {
      reg_layout.blend_consts.offset = next_free_sh_reg;
      next_free_sh_reg += PVR_DEV_ADDR_SIZE_IN_SH_REGS;
   }

   reg_layout.preamble.present = !!preamble_shareds;
   if (reg_layout.preamble.present) {
      reg_layout.preamble.offset = next_free_sh_reg;
      next_free_sh_reg += preamble_shareds;
   }

   *sh_reg_layout_out = reg_layout;

   /* FIXME: We might need to take more things into consideration.
    * See pvr_calc_fscommon_size_and_tiles_in_flight().
    */
   assert(next_free_sh_reg <= reserved_shared_size - max_coeff);

   return next_free_sh_reg;
}

#undef PVR_DEV_ADDR_SIZE_IN_SH_REGS

/* This is a const pointer to an array of pvr_pds_vertex_dma structs.
 * The array being pointed to is of PVR_MAX_VERTEX_ATTRIB_DMAS size.
 */
typedef struct pvr_pds_vertex_dma (
      *const
         pvr_pds_attrib_dma_descriptions_array_ptr)[PVR_MAX_VERTEX_ATTRIB_DMAS];

static void pvr_graphics_pipeline_alloc_vertex_inputs(
   const VkPipelineVertexInputStateCreateInfo *const vs_data,
   rogue_vertex_inputs *const vertex_input_layout_out,
   unsigned *num_vertex_input_regs_out,
   pvr_pds_attrib_dma_descriptions_array_ptr dma_descriptions_out_ptr,
   uint32_t *const dma_count_out)
{
   const VkVertexInputBindingDescription
      *sorted_bindings[PVR_MAX_VERTEX_INPUT_BINDINGS] = { 0 };
   const VkVertexInputAttributeDescription
      *sorted_attributes[PVR_MAX_VERTEX_INPUT_BINDINGS] = { 0 };

   rogue_vertex_inputs build_data = { 0 };
   uint32_t next_reg_offset = 0;

   struct pvr_pds_vertex_dma *const dma_descriptions =
      *dma_descriptions_out_ptr;
   uint32_t dma_count = 0;

   /* Vertex attributes map to the `layout(location = x)` annotation in the
    * shader where `x` is the attribute's location.
    * Vertex bindings have NO relation to the shader. They have nothing to do
    * with the `layout(set = x, binding = y)` notation. They instead indicate
    * where the data for a collection of vertex attributes comes from. The
    * application binds a VkBuffer with vkCmdBindVertexBuffers() to a specific
    * binding number and based on that we'll know which buffer to DMA the data
    * from, to fill in the collection of vertex attributes.
    */

   for (uint32_t i = 0; i < vs_data->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *binding_desc =
         &vs_data->pVertexBindingDescriptions[i];

      sorted_bindings[binding_desc->binding] = binding_desc;
   }

   for (uint32_t i = 0; i < vs_data->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *attribute_desc =
         &vs_data->pVertexAttributeDescriptions[i];

      sorted_attributes[attribute_desc->location] = attribute_desc;
   }

   for (uint32_t i = 0, j = 0; i < ARRAY_SIZE(sorted_attributes); i++) {
      if (sorted_attributes[i])
         sorted_attributes[j++] = sorted_attributes[i];
   }

   for (uint32_t i = 0; i < vs_data->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *attribute = sorted_attributes[i];
      const VkVertexInputBindingDescription *binding =
         sorted_bindings[attribute->binding];
      const struct util_format_description *fmt_description =
         vk_format_description(attribute->format);
      struct pvr_pds_vertex_dma *dma_desc = &dma_descriptions[dma_count];
      const unsigned l = attribute->location;
      unsigned vtxin_reg_offset;

      /* Reg allocation. */

      vtxin_reg_offset = next_reg_offset;

      assert(fmt_description->colorspace == UTIL_FORMAT_COLORSPACE_RGB);

      build_data.defined[l] = true;
      build_data.base_vtxin_reg[l] = vtxin_reg_offset;
      build_data.format[l] = fmt_description->format;
      memcpy(&build_data.format_descs[l],
             fmt_description,
             sizeof(*fmt_description));

      /* DMA setup. */

      /* The PDS program sets up DDMADs to DMA attributes into vtxin regs.
       *
       * DDMAD -> Multiply, add, and DOUTD (i.e. DMA from that address).
       *          DMA source addr = src0 * src1 + src2
       *          DMA params = src3
       *
       * In the PDS program we setup src0 with the binding's stride and src1
       * with either the instance id or vertex id (both of which get filled by
       * the hardware). We setup src2 later on once we know which VkBuffer to
       * DMA the data from so it's saved for later when we patch the data
       * section.
       */

      /* TODO: Right now we're setting up a DMA per attribute. In a case where
       * there are multiple attributes packed into a single binding with
       * adjacent locations we'd still be DMAing them separately. This is not
       * great so the DMA setup should be smarter and could do with some
       * optimization.
       */

      *dma_desc = (struct pvr_pds_vertex_dma){ 0 };

      /* In relation to the Vulkan spec. 22.4. Vertex Input Address Calculation
       * this corresponds to `attribDesc.offset`.
       * The PDS program doesn't do anything with it but just save it in the
       * PDS program entry.
       */
      dma_desc->offset = attribute->offset;

      /* In relation to the Vulkan spec. 22.4. Vertex Input Address Calculation
       * this corresponds to `bindingDesc.stride`.
       * The PDS program will calculate the `effectiveVertexOffset` with this
       * and add it to the address provided in the patched data segment.
       */
      dma_desc->stride = binding->stride;

      if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
         dma_desc->flags = PVR_PDS_VERTEX_DMA_FLAGS_INSTANCE_RATE;
      else
         dma_desc->flags = 0;

      /* Size to DMA per vertex attribute. Used to setup src3 in the DDMAD. */
      assert(fmt_description->block.bits != 0); /* Likely an unsupported fmt. */
      dma_desc->size_in_dwords = PVR_BITS_TO_DW(fmt_description->block.bits);
      next_reg_offset += dma_desc->size_in_dwords;

      /* Vtxin reg offset to start DMAing into. */
      dma_desc->destination = vtxin_reg_offset;

      /* Will be used by the driver to figure out buffer address to patch in the
       * data section. I.e. which binding we should DMA from.
       */
      dma_desc->binding_index = attribute->binding;

      /* We don't currently support VK_EXT_vertex_attribute_divisor so no
       * repeating of instance-rate vertex attributes needed. We should always
       * move on to the next vertex attribute.
       */
      dma_desc->divisor = 1;

      /* Will be used to generate PDS code that takes care of robust buffer
       * access, and later on by the driver to write the correct robustness
       * buffer address to DMA the fallback values from.
       */
      dma_desc->robustness_buffer_offset =
         pvr_get_robustness_buffer_format_offset(attribute->format);

      /* Used by later on by the driver to figure out if the buffer is being
       * accessed out of bounds, for robust buffer access.
       */
      dma_desc->component_size_in_bytes =
         fmt_description->block.bits / fmt_description->nr_channels / 8;

      dma_count++;
   };

   *vertex_input_layout_out = build_data;
   *num_vertex_input_regs_out = next_reg_offset;
   *dma_count_out = dma_count;
}

static void pvr_graphics_pipeline_alloc_vertex_special_vars(
   unsigned *num_vertex_input_regs,
   struct rogue_vertex_special_vars *layout)
{
   unsigned next_free_reg = *num_vertex_input_regs;

   if (layout->has.vertex_id)
      layout->offset.vertex_id = next_free_reg++;
   else
      layout->offset.vertex_id = ROGUE_REG_UNUSED;

   if (layout->has.instance_id)
      layout->offset.instance_id = next_free_reg++;
   else
      layout->offset.instance_id = ROGUE_REG_UNUSED;

   if (layout->has.base_instance)
      layout->offset.base_instance = next_free_reg++;
   else
      layout->offset.base_instance = ROGUE_REG_UNUSED;

   if (layout->has.base_vertex)
      layout->offset.base_vertex = next_free_reg++;
   else
      layout->offset.base_vertex = ROGUE_REG_UNUSED;

   if (layout->has.draw_index)
      layout->offset.draw_index = next_free_reg++;
   else
      layout->offset.draw_index = ROGUE_REG_UNUSED;

   *num_vertex_input_regs = next_free_reg;
}

/**
 * \brief Reserves an iterator for a fragment shader input varying,
 * and calculates its setup data.
 *
 * \param[in] args The iterator argument data.
 * \param[in] idx The iterator index.
 * \param[in] type The interpolation type of the varying.
 * \param[in] f16 Whether the data type is F16 or F32.
 * \param[in] components The number of components in the varying.
 */
static void pvr_reserve_iterator(struct rogue_iterator_args *args,
                                 unsigned rogue_location,
                                 unsigned base_component,
                                 enum glsl_interp_mode type,
                                 bool f16,
                                 unsigned components)
{
   assert(components >= 1 && components <= 4);

   /* W and Z *must* be INTERP_MODE_NOPERSPECTIVE. */
   assert(rogue_location > 0 || type == INTERP_MODE_NOPERSPECTIVE);

   for (int c = 0; c < components; c++) {
      args->coeff_to_location[args->num_coeff_varyings].location =
         rogue_location;
      args->coeff_to_location[args->num_coeff_varyings].component =
         base_component + c;

      args->coeff_indices[rogue_location][base_component + c] =
         args->num_coeff_varyings;
      args->interp_modes[rogue_location][base_component + c] = type;

      args->num_coeff_varyings++;
   }
}

static inline unsigned nir_count_variables_with_modes(const nir_shader *nir,
                                                      nir_variable_mode mode)
{
   unsigned count = 0;

   nir_foreach_variable_with_modes (var, nir, mode) {
      count++;
   }

   return count;
}

/**
 * \brief Collects the fragment shader I/O data.
 *
 * \sa #pvr_collect_io_data()
 *
 * \param[in] common_data Common build data.
 * \param[in] fs_data Fragment-specific build data.
 * \param[in] nir NIR fragment shader.
 */
void pvr_collect_io_data_fs(struct rogue_common_build_data *common_data,
                            struct rogue_fs_build_data *fs_data,
                            nir_shader *nir,
                            bool no_perspective)
{
   unsigned num_inputs = nir_count_variables_with_modes(nir, nir_var_shader_in);
   assert(num_inputs < (ARRAY_SIZE(fs_data->iterator_args.fpu_iterators) - 1));
   assert(num_inputs < (ARRAY_SIZE(fs_data->iterator_args.coeff_indices) - 1));

   /* Initialise the data */
   fs_data->iterator_args.iterates_depth = false;
   fs_data->iterator_args.num_coeff_varyings = 0;
   for (int i = 0; i < ROGUE_MAX_IO_VARYING_VARS; i++) {
      for (int c = 0; c < 4; c++) {
         fs_data->iterator_args.interp_modes[i][c] = INTERP_MODE_SMOOTH;
         fs_data->iterator_args.coeff_indices[i][c] = ~0;
      }
   }

   /* Process inputs (if present). */
   if (num_inputs) {
      /* If the fragment shader has inputs, the first iterator must be used for
       * the W component.
       */
      if (!no_perspective) {
         pvr_reserve_iterator(&fs_data->iterator_args,
                              rogue_from_gl_varying_loc(VARYING_SLOT_POS),
                              3,
                              INTERP_MODE_NOPERSPECTIVE,
                              false,
                              1);
      }

      nir_foreach_shader_in_variable (var, nir) {
         const unsigned components = glsl_get_components(var->type);
         const struct glsl_type *type = glsl_without_array_or_matrix(var->type);
         const bool f16 = glsl_type_is_16bit(type);
         enum glsl_interp_mode interp = var->data.interpolation;
         unsigned rogue_loc;

         if (var->data.location == VARYING_SLOT_POS) {
            unsigned base = var->data.location_frac;
            /* Includes .z */
            if (base <= 2 && (base + components) > 2) {
               pvr_reserve_iterator(&fs_data->iterator_args,
                                    rogue_from_gl_varying_loc(VARYING_SLOT_POS),
                                    2,
                                    INTERP_MODE_NOPERSPECTIVE,
                                    false,
                                    1);
               fs_data->iterator_args.iterates_depth = true;
            }
            continue;
         }

         if (var->data.location == VARYING_SLOT_PNTC) {
            interp = INTERP_MODE_NOPERSPECTIVE;
            assert(components == 2 && var->data.location_frac == 0);
         }

         if (no_perspective)
            interp = INTERP_MODE_NOPERSPECTIVE;

         rogue_loc = rogue_from_gl_varying_loc(var->data.location);
         assert(rogue_loc != ~0);

         pvr_reserve_iterator(&fs_data->iterator_args,
                              rogue_loc,
                              var->data.location_frac,
                              interp,
                              f16,
                              components);
      }

      common_data->coeffs =
         fs_data->iterator_args.num_coeff_varyings * ROGUE_COEFF_ALIGN;
      assert(common_data->coeffs);
      /* TODO: This causes linking errors since the regs are setup in the
       * compiler stuff. See if there is a better way of re-enabling this check.
       * assert(common_data->coeffs <=
       *        rogue_reg_infos[ROGUE_REG_CLASS_COEFF].num);
       */
   }

   /* TODO: Process outputs. */
   fs_data->translucent = nir->info.fs.uses_fbfetch_output;
   /* TODO: Selective rate for blending with the use of phase changes */
   if (fs_data->alpha_to_coverage_enable || nir->info.fs.uses_sample_shading || nir->info.fs.uses_fbfetch_output)
      fs_data->msaa_mode = ROGUE_MSAA_MODE_FULL;
   else
      fs_data->msaa_mode = ROGUE_MSAA_MODE_PIXEL;
}

struct pvr_output_reg {
   unsigned location : 8;
   unsigned component : 2;
   bool f16 : 1;
   enum glsl_interp_mode mode : 3;
   unsigned _pad : 2;
};

static int
pvr_compare_output_reg(const void *_a, const void *_b, UNUSED void *arg)
{
   const struct pvr_output_reg *a = (const struct pvr_output_reg *)_a;
   const struct pvr_output_reg *b = (const struct pvr_output_reg *)_b;

   /* First is F32 vs F16 */
   if (a->f16 > b->f16)
      return 1;
   if (a->f16 < b->f16)
      return -1;

   /* Next is interpolation mode in this order:
    * INTERP_MODE_SMOOTH,
    * INTERP_MODE_FLAT,
    * INTERP_MODE_NOPERSPECTIVE,
    */
   if (a->mode > b->mode)
      return 1;
   if (a->mode < b->mode)
      return -1;

   /* Finally determine through location/component number */
   unsigned a_index = a->location * 4 + a->component;
   unsigned b_index = b->location * 4 + b->component;
   if (a_index > b_index)
      return 1;
   if (a_index < b_index)
      return -1;

   return 0;
}

/**
 * \brief Collects the vertex shader I/O data to feed-back to the driver.
 *
 * \sa #pvr_collect_io_data()
 *
 * \param[in] common_data Common build data.
 * \param[in] vs_data Vertex-specific build data.
 * \param[in] fs_data Fragment-specific build data.
 * \param[in] nir NIR vertex shader.
 */
static void pvr_collect_io_data_vs(struct rogue_common_build_data *common_data,
                                   struct rogue_vs_build_data *vs_data,
                                   struct rogue_fs_build_data *fs_data,
                                   nir_shader *nir)
{
   bool out_pos_present = false;
   ASSERTED unsigned num_outputs =
      nir_count_variables_with_modes(nir, nir_var_shader_out);
   bool psprite_present = false;
   bool viewport_present = false;
   bool layer_present = false;
   bool clip_present[2][4] = { 0 };
   bool cull_present[2][4] = { 0 };
   struct pvr_output_reg varyings[MAX_VARYING * 4];
   unsigned varyings_count = 0;

   /* Process outputs. */

   /* Initialise all indices to ~0 */
   vs_data->outputs.point_size_index = ~0;
   vs_data->outputs.viewport_index = ~0;
   vs_data->outputs.layer_index = ~0;
   memset(&vs_data->outputs.clip_index,
          ~0,
          sizeof(vs_data->outputs.clip_index));
   memset(&vs_data->outputs.cull_index,
          ~0,
          sizeof(vs_data->outputs.cull_index));
   memset(&vs_data->outputs.indices, ~0, sizeof(vs_data->outputs.indices));
   memset(vs_data->outputs.is_f16, 0, sizeof(vs_data->outputs.is_f16));

   /* We should always have at least a position variable. */
   assert(num_outputs > 0 && "Unsupported number of vertex shader outputs.");

   nir_foreach_shader_out_variable (var, nir) {
      unsigned components = glsl_get_components(var->type);
      const struct glsl_type *type = glsl_without_array_or_matrix(var->type);

      /* Check that outputs are either F32 or F16. */
      assert(glsl_type_is_32bit(type) || glsl_type_is_16bit(type));

      if (var->data.location == VARYING_SLOT_POS) {
         out_pos_present = true;
      } else if (var->data.location == VARYING_SLOT_PSIZ) {
         psprite_present = true;
      } else if (var->data.location == VARYING_SLOT_VIEWPORT) {
         viewport_present = true;
      } else if (var->data.location == VARYING_SLOT_LAYER) {
         layer_present = true;
      } else if ((var->data.location >= VARYING_SLOT_CLIP_DIST0) &&
                 (var->data.location <= VARYING_SLOT_CLIP_DIST1)) {
         for (int c = 0; c < components; c++) {
            clip_present[var->data.location - VARYING_SLOT_CLIP_DIST0]
                        [var->data.location_frac + c] = true;
         }
      } else if ((var->data.location >= VARYING_SLOT_CULL_DIST0) &&
                 (var->data.location <= VARYING_SLOT_CULL_DIST1)) {
         for (int c = 0; c < components; c++) {
            cull_present[var->data.location - VARYING_SLOT_CULL_DIST0]
                        [var->data.location_frac + c] = true;
         }
      } else if ((var->data.location >= VARYING_SLOT_VAR0) &&
                 (var->data.location <= VARYING_SLOT_VAR31)) {
         for (int c = 0; c < components; c++) {
            unsigned rogue_loc = rogue_from_gl_varying_loc(var->data.location);
            assert(rogue_loc != ~0);

            struct pvr_output_reg reg = { .location = rogue_loc,
                                          .component =
                                             var->data.location_frac + c,
                                          .f16 = glsl_type_is_16bit(type),
                                          .mode = INTERP_MODE_SMOOTH };
            unsigned fs_reg = fs_data
                                 ? rogue_coeff_index_fs(&fs_data->iterator_args,
                                                        var->data.location,
                                                        reg.component)
                                 : ~0;

            /* FS interpolation mode takes precedence over VS */
            if (fs_reg != ~0) {
               reg.mode = rogue_interp_mode_fs(&fs_data->iterator_args,
                                               var->data.location,
                                               reg.component);
            } else {
               switch (var->data.interpolation) {
               case INTERP_MODE_NONE:
                  reg.mode = INTERP_MODE_SMOOTH;
                  break;
               case INTERP_MODE_SMOOTH:
               case INTERP_MODE_NOPERSPECTIVE:
               case INTERP_MODE_FLAT:
                  reg.mode = var->data.interpolation;
                  break;
               default:
                  unreachable("Unimplemented interpolation type.");
               }
            }

            switch (reg.mode) {
            case INTERP_MODE_SMOOTH:
               if (reg.f16)
                  vs_data->num_f16_linear_varyings++;
               else
                  vs_data->num_f32_linear_varyings++;
               break;
            case INTERP_MODE_NOPERSPECTIVE:
               if (reg.f16)
                  vs_data->num_f16_npc_varyings++;
               else
                  vs_data->num_f32_npc_varyings++;
               break;
            case INTERP_MODE_FLAT:
               if (reg.f16)
                  vs_data->num_f16_flat_varyings++;
               else
                  vs_data->num_f32_flat_varyings++;
               break;
            default:
               unreachable("Unimplemented interpolation type.");
            }

            varyings[varyings_count++] = reg;
         }
      } else {
         unreachable("Unsupported vertex output type.");
      }
   }

#if 0
   /* Sort the varyings based on their type */
   util_qsort_r(varyings,
                varyings_count,
                sizeof(*varyings),
                pvr_compare_output_reg,
                NULL);
#endif

   for (int i = 0; i < varyings_count; i++) {
      struct pvr_output_reg *reg = &varyings[i];
      vs_data->outputs.indices[reg->location][reg->component] =
         i + (out_pos_present ? 4 : 0);
      vs_data->outputs.is_f16[reg->location][reg->component] = reg->f16;
   }

   vs_data->num_vertex_outputs = varyings_count + (out_pos_present ? 4 : 0);
   vs_data->outputs.num_output_vars = vs_data->num_vertex_outputs;

   /* The order of these system values are important */
   if (psprite_present)
      vs_data->outputs.point_size_index = vs_data->num_vertex_outputs++;
   if (viewport_present)
      vs_data->outputs.viewport_index = vs_data->num_vertex_outputs++;
   if (layer_present)
      vs_data->outputs.layer_index = vs_data->num_vertex_outputs++;
   for (int i = 0; i < 8; i++) {
      if (clip_present[i / 4][i % 4]) {
         vs_data->outputs.clip_index[i / 4][i % 4] =
            vs_data->num_vertex_outputs++;
      }
   }
   for (int i = 0; i < 8; i++) {
      if (cull_present[i / 4][i % 4]) {
         vs_data->outputs.cull_index[i / 4][i % 4] =
            vs_data->num_vertex_outputs++;
      }
   }

   assert(vs_data->num_vertex_outputs);
   /* TODO: This causes linking errors since the regs are setup in the compiler
    * stuff. See if there is a better way of re-enabling this check.
    * assert(vs_data->num_vertex_outputs <
    *        rogue_reg_infos[ROGUE_REG_CLASS_VTXOUT].num);
    */
}

/**
 * \brief Collects I/O data.
 *
 * Collects the inputs/outputs/memory required. Done at this stage rather than
 * at the start of rogue_to_binary, so that all the I/O of all the shader stages
 * is known before backend compilation, which would let us do things like cull
 * unused inputs.
 *
 * \param[in] ctx Shared multi-stage build context.
 * \param[in] nir NIR shader.
 */
static void pvr_collect_io_data(struct rogue_build_ctx *ctx, nir_shader *nir)
{
   gl_shader_stage stage = nir->info.stage;
   struct rogue_common_build_data *common_data = &ctx->common_data[stage];

   /* Collect stage-specific data. */
   switch (stage) {
   case MESA_SHADER_FRAGMENT:
      return pvr_collect_io_data_fs(common_data,
                                    &ctx->stage_data.fs,
                                    nir,
                                    false);

   case MESA_SHADER_VERTEX:
      return pvr_collect_io_data_vs(
         common_data,
         &ctx->stage_data.vs,
         ctx->nir[MESA_SHADER_FRAGMENT] ? &ctx->stage_data.fs : NULL,
         nir);

   default:
      unreachable("Unsupported stage.");
   }
}

/**
 * \brief Setup PDS douti iterator commands.
 *
 * This pass links VS and FS IO together and
 * must be run after both VS output and FS coeff
 * registers have been allocated.
 *
 * \param[in] ctx Shared multi-stage build context.
 */
static void pvr_generate_iterator_commands(struct rogue_build_ctx *ctx)
{
   rogue_iterator_args *args = &ctx->stage_data.fs.iterator_args;
   rogue_vertex_outputs *outputs = &ctx->stage_data.vs.outputs;

   struct rogue_vs_build_data *vs_data = &ctx->stage_data.vs;
   unsigned num_f32_varyings = vs_data->num_f32_linear_varyings +
                               vs_data->num_f32_flat_varyings +
                               vs_data->num_f32_npc_varyings;
   if (args->iterates_depth)
      num_f32_varyings += 2;
   else
      num_f32_varyings += 1;

   args->num_fpu_iterators = 0;

   for (int coeff = 0; coeff < args->num_coeff_varyings; coeff++) {
      uint32_t fpu_idx = args->num_fpu_iterators++;

      pvr_csb_pack (&args->fpu_iterators[fpu_idx],
                    PDSINST_DOUT_FIELDS_DOUTI_SRC,
                    douti_src) {
         int start = coeff;

         unsigned rogue_loc = args->coeff_to_location[coeff].location;
         gl_varying_slot start_location = gl_from_rogue_varying_loc(rogue_loc);
         unsigned start_component = args->coeff_to_location[coeff].component;
         enum glsl_interp_mode start_mode =
            rogue_interp_mode_fs(args, start_location, start_component);
         unsigned start_vs_index = 0;
         bool start_f16 = false;

         /* Sysvals are special */
         if (rogue_loc < ROGUE_MAX_SYSVAL_VARYINGS) {
            /* W and Z are special, the rest come after these 2 */
            switch (start_location) {
            case VARYING_SLOT_POS:
               assert((start_component == 2 && args->iterates_depth) ||
                      start_component == 3);
               start_vs_index = (start_component == 3) ? 0 : 1;
               break;

            case VARYING_SLOT_PNTC:
               /* This must be a contiguous vec2 */
               assert(start_component == 0 || start_component == 1);
               if (start_component == 1)
                  continue;
               coeff++;
               assert(args->coeff_to_location[coeff].location == rogue_loc);
               assert(args->coeff_to_location[coeff].component == 1);
               douti_src.pointsprite = true;
               break;

            default:
               unreachable("Unsupported gl_varying_slot");
            }
         } else {
            start_vs_index =
               rogue_output_index_vs(outputs, start_location, start_component);
            start_f16 = outputs->is_f16[rogue_loc][start_component];

            /* Vectorise the iterators */
            for (; coeff < MIN2(start + 3, args->num_coeff_varyings - 1);
                 coeff++) {
               unsigned rogue_loc = args->coeff_to_location[coeff + 1].location;
               gl_varying_slot next_location =
                  gl_from_rogue_varying_loc(rogue_loc);
               unsigned next_component =
                  args->coeff_to_location[coeff + 1].component;
               enum glsl_interp_mode next_mode =
                  rogue_interp_mode_fs(args, next_location, next_component);
               unsigned next_vs_index =
                  rogue_output_index_vs(outputs, next_location, next_component);
               bool next_f16 = outputs->is_f16[rogue_loc][next_component];
               if (start_f16 != next_f16)
                  break;
               if (start_mode != next_mode)
                  break;
               if ((next_vs_index - start_vs_index) != (coeff - start + 1))
                  break;
            }

            if (args->iterates_depth)
               start_vs_index -= 2;
            else
               start_vs_index -= 3;
         }

         switch (start_mode) {
         case INTERP_MODE_NONE:
         case INTERP_MODE_SMOOTH:
            douti_src.shademodel = PVRX(PDSINST_DOUTI_SHADEMODEL_GOURUAD);
            douti_src.perspective = true;
            break;

         case INTERP_MODE_NOPERSPECTIVE:
            douti_src.shademodel = PVRX(PDSINST_DOUTI_SHADEMODEL_GOURUAD);
            douti_src.perspective = false;
            break;

         case INTERP_MODE_FLAT:
            if (args->provoking_vtx_last) {
               douti_src.shademodel =
                  PVRX(PDSINST_DOUTI_SHADEMODEL_FLAT_VERTEX2);
            } else if (args->triangle_fan) {
               douti_src.shademodel =
                  PVRX(PDSINST_DOUTI_SHADEMODEL_FLAT_VERTEX1);
            } else {
               douti_src.shademodel =
                  PVRX(PDSINST_DOUTI_SHADEMODEL_FLAT_VERTEX0);
            }
            douti_src.perspective = false;
            break;

         default:
            unreachable("Unimplemented interpolation type.");
         }

         /* Number of components in this varying
          * (corresponds to ROGUE_PDSINST_DOUTI_SIZE_1..4D).
          */
         douti_src.size = pvr_pdsinst_douti_size(coeff - start + 1);

         douti_src.f16 = !!start_f16;

         /* Offsets within the vertex. */
         douti_src.f32_offset = 2 * start_vs_index;
         /* F16 TSP input values always come after all the F32 varyings */
         if (start_f16) {
            douti_src.f16_offset =
               (start_vs_index - num_f32_varyings) + 2 * num_f32_varyings;
         } else {
            douti_src.f16_offset = douti_src.f32_offset;
         }

         args->destination[fpu_idx] = start;
      }
   }
}

/** @} */
/* End of \defgroup Graphics pipeline register allocation. */

/* Passes the I/O information to the fragment shader. */
static inline void pvr_graphics_pipeline_assign_fs_io(
   rogue_build_ctx *ctx,
   struct rogue_fs_build_data *fs_data,
   const struct pvr_render_pass *pass,
   const struct pvr_render_subpass *subpass,
   const struct pvr_renderpass_hwsetup_subpass *hw_subpass,
   const struct vk_graphics_pipeline_state *state)
{
   fs_data->num_outputs = hw_subpass->setup.num_render_targets;
   fs_data->outputs =
      rzalloc_array_size(ctx, sizeof(*fs_data->outputs), fs_data->num_outputs);

   for (unsigned u = 0; u < subpass->color_count; ++u) {
      unsigned idx = subpass->color_attachments[u];
      if (idx == VK_ATTACHMENT_UNUSED)
         continue;

      VkFormat vk_format = pass->attachments[idx].vk_format;
      const struct usc_mrt_resource *mrt_resource =
         &hw_subpass->setup.mrt_resources[u];

      const struct util_format_description *fmt_desc =
         vk_format_description(vk_format);
      memcpy(&fs_data->outputs[u].fmt_desc, fmt_desc, sizeof(*fmt_desc));
      fs_data->outputs[u].accum_format = pvr_get_pbe_accum_format(vk_format);
      fs_data->outputs[u].format = vk_format_to_pipe_format(vk_format);
      fs_data->outputs[u].mrt_resource = mrt_resource;
   }

   fs_data->z_replicate = hw_subpass->z_replicate;
   if (hw_subpass->z_replicate >= 0) {
      unsigned mrt_idx = hw_subpass->z_replicate;
      const struct usc_mrt_resource *mrt_resource =
         &hw_subpass->setup.mrt_resources[mrt_idx];
      fs_data->outputs[mrt_idx].accum_format = PVR_PBE_ACCUM_FORMAT_F32;
      fs_data->outputs[mrt_idx].format = PIPE_FORMAT_R32_FLOAT;
      fs_data->outputs[mrt_idx].mrt_resource = mrt_resource;
   }

   fs_data->num_inputs = subpass->input_count;
   fs_data->inputs =
      ralloc_array_size(ctx, sizeof(*fs_data->inputs), subpass->input_count);

   for (unsigned u = 0; u < subpass->input_count; ++u) {
      fs_data->inputs[u].type = hw_subpass->input_access[u].type;
      fs_data->inputs[u].on_chip_rt = hw_subpass->input_access[u].on_chip_rt;

      if (fs_data->inputs[u].type !=
          PVR_RENDERPASS_HWSETUP_INPUT_ACCESS_OFFCHIP) {
         unsigned idx = subpass->input_attachments[u];
         if (idx == VK_ATTACHMENT_UNUSED)
            continue;

         unsigned mrt_idx = fs_data->inputs[u].on_chip_rt;
         const struct usc_mrt_resource *mrt_resource =
            &hw_subpass->setup.mrt_resources[mrt_idx];
         VkFormat vk_format = pass->attachments[idx].vk_format;

         fs_data->inputs[u].format = vk_format_to_pipe_format(vk_format);

         if (fs_data->inputs[u].type ==
             PVR_RENDERPASS_HWSETUP_INPUT_ACCESS_ONCHIP_ZREPLICATE)
            vk_format = VK_FORMAT_R32_SFLOAT;

         const struct util_format_description *fmt_desc =
            vk_format_description(vk_format);
         memcpy(&fs_data->outputs[mrt_idx].fmt_desc,
                fmt_desc,
                sizeof(*fmt_desc));
         fs_data->outputs[mrt_idx].accum_format =
            pvr_get_pbe_accum_format(vk_format);
         fs_data->outputs[mrt_idx].format = vk_format_to_pipe_format(vk_format);
         fs_data->outputs[mrt_idx].mrt_resource = mrt_resource;
      }
   }

   /* Blend constants. */
   if (state->cb)
      fs_data->cb_state = state->cb;

   /* Multisample info. */
   if (state->ms) {
      fs_data->sample_mask = state->ms->sample_mask;
      fs_data->alpha_to_coverage_enable = state->ms->alpha_to_coverage_enable;
      fs_data->alpha_to_one_enable = state->ms->alpha_to_one_enable;
      fs_data->rasterization_samples = state->ms->rasterization_samples;
   } else {
      fs_data->sample_mask = ~0;
   }

   /* Depth write. */
   if (state->ds) {
      ASSERTED bool dynamic_depth_write_enable =
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE);
      assert(!dynamic_depth_write_enable);
      fs_data->depth_write = state->ds->depth.write_enable;
   }

   /* Swap front face. */
   if (state->rs) {
      ASSERTED bool dynamic_swap_front_face =
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_FRONT_FACE);
      assert(!dynamic_swap_front_face);

      if (state->rs->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE)
         fs_data->swap_front_face = SWAP_FRONT_FACE_TRUE;
      else
         fs_data->swap_front_face = SWAP_FRONT_FACE_FALSE;
   }

   /* Swap front face (force). */
   if (state->ia) {
      switch (state->ia->primitive_topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
         fs_data->swap_front_face = SWAP_FRONT_FACE_FORCE;
         break;

      default:
         break;
      }
   }
}

/* Compiles and uploads shaders and PDS programs. */
static VkResult
pvr_graphics_pipeline_compile(struct pvr_device *const device,
                              struct vk_pipeline_cache *cache,
                              const VkGraphicsPipelineCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *const allocator,
                              struct pvr_graphics_pipeline *const gfx_pipeline,
                              const struct vk_graphics_pipeline_state *state)
{
   const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *rs_ext =
      vk_find_struct_const(
         pCreateInfo->pRasterizationState,
         PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT);
   const uint32_t cache_line_size =
      rogue_get_slc_cache_line_size(&device->pdevice->dev_info);
   struct pvr_pipeline_layout *layout = gfx_pipeline->base.layout;
   VkResult result;

   struct rogue_compiler *compiler = device->pdevice->compiler;
   struct rogue_build_ctx *ctx;

   struct pvr_pds_vertex_dma vtx_dma_descriptions[PVR_MAX_VERTEX_ATTRIB_DMAS];
   uint32_t vtx_dma_count = 0;
   struct pvr_sh_reg_layout *sh_reg_layout_vert =
      &layout->sh_reg_layout_per_stage[PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY];
   struct pvr_sh_reg_layout *sh_reg_layout_frag =
      &layout->sh_reg_layout_per_stage[PVR_STAGE_ALLOCATION_FRAGMENT];
   uint32_t sh_count[PVR_STAGE_ALLOCATION_FRAGMENT + 1] = { 0 };

   PVR_FROM_HANDLE(pvr_render_pass, pass, pCreateInfo->renderPass);
   const struct pvr_render_subpass *const subpass =
      &pass->subpasses[pCreateInfo->subpass];
   const struct pvr_renderpass_hw_map *subpass_map =
      &pass->hw_setup->subpass_map[pCreateInfo->subpass];
   const struct pvr_renderpass_hwsetup_subpass *hw_subpass =
      &pass->hw_setup->renders[subpass_map->render]
          .subpasses[subpass_map->subpass];

   /* Setup shared build context. */
   ctx = rogue_build_context_create(compiler, layout);
   if (!ctx)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

#if 1
   {
      for (unsigned u = 0; u < device->tile_buffer_state.buffer_count; ++u) {
         uint64_t tile_buffer_addr =
            device->tile_buffer_state.buffers[u]->vma->dev_addr.addr;
         ctx->tile_buffer_base_addr[u] = tile_buffer_addr;
      }
   }
#endif

   pvr_graphics_pipeline_assign_fs_io(ctx,
                                      &ctx->stage_data.fs,
                                      pass,
                                      subpass,
                                      hw_subpass,
                                      state);

   /* TODO: move into pvr_graphics_pipeline_assign_fs_io */
   ctx->stage_data.fs.dynamic_blend_consts = pvr_graphics_pipeline_requires_dynamic_blend_consts(gfx_pipeline);

   /* TODO: Fix this by having multiple variants of iterator PDS programs for
    * VK_EXT_extended_dynamic_state */
   ctx->stage_data.fs.iterator_args.triangle_fan =
      pCreateInfo->pInputAssemblyState->topology ==
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
   if (rs_ext) {
      ctx->stage_data.fs.iterator_args.provoking_vtx_last =
         rs_ext->provokingVertexMode ==
         VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   }

   /* NIR middle-end translation. */
   rogue_foreach_graphics_stage (stage) {
      /* clang-format on */
      size_t stage_index = gfx_pipeline->stage_indices[stage];

      /* Skip unused/inactive stages. */
      if (stage_index == ~0)
         continue;

      /* SPIR-V to NIR. */
      ctx->nir[stage] =
         pvr_pipeline_spirv_to_nir(ctx,
                                   stage,
                                   &pCreateInfo->pStages[stage_index]);
      if (!ctx->nir[stage]) {
         ralloc_free(ctx);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   rogue_nir_preprocess(ctx, false);
   rogue_nir_link(ctx, false);

   pvr_graphics_pipeline_alloc_vertex_inputs(
      pCreateInfo->pVertexInputState,
      &ctx->stage_data.vs.inputs,
      &ctx->stage_data.vs.num_vertex_input_regs,
      &vtx_dma_descriptions,
      &vtx_dma_count);

   rogue_nir_lower(ctx, false);
   rogue_nir_postprocess(ctx, false);

   pvr_graphics_pipeline_alloc_vertex_special_vars(
      &ctx->stage_data.vs.num_vertex_input_regs,
      &ctx->stage_data.vs.special_vars);

   /* Collect I/O data to pass back to the driver. */
   rogue_foreach_graphics_stage (stage) {
      if (!ctx->nir[stage])
         continue;

      pvr_collect_io_data(ctx, ctx->nir[stage]);
   }

   for (enum pvr_stage_allocation pvr_stage =
           PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY;
        pvr_stage < PVR_STAGE_ALLOCATION_COMPUTE;
        pvr_stage++) {
      gl_shader_stage stage = pvr_stage_to_mesa(pvr_stage);
      sh_count[pvr_stage] = pvr_graphics_pipeline_alloc_shareds(
         device,
         gfx_pipeline,
         pvr_stage,
         ctx->common_data[stage].preamble.shareds,
         ctx->common_data[stage].scratch_size > 0,
         true, /* Always assume true for now since we don't know the spill size yet */
         &layout->sh_reg_layout_per_stage[pvr_stage]);
   }

   rogue_nir_build(ctx, false);

#if 0
   for (enum pvr_stage_allocation pvr_stage =
           PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY;
        pvr_stage < PVR_STAGE_ALLOCATION_COMPUTE;
        pvr_stage++) {
      gl_shader_stage stage = pvr_stage_to_mesa(pvr_stage);
      layout->sh_reg_layout_per_stage[pvr_stage].temp_spill_buffer.present = ctx->common_data[stage].spill_regs > 0;
   }
#endif

   pvr_vertex_state_init(gfx_pipeline,
                         &ctx->common_data[MESA_SHADER_VERTEX],
                         ctx->stage_data.vs.num_vertex_input_regs,
                         &ctx->stage_data.vs);

   /* FIXME: For now we just overwrite it but the compiler shouldn't be
    * returning the sh count since the driver is in charge of
    * allocating them.
    */
   gfx_pipeline->shader_state.vertex.stage_state.const_shared_reg_count =
      sh_count[PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY];

   gfx_pipeline->shader_state.vertex.vertex_input_size =
      ctx->stage_data.vs.num_vertex_input_regs;

   result =
      pvr_gpu_upload_usc(device,
                         util_dynarray_begin(&ctx->binary[MESA_SHADER_VERTEX]),
                         ctx->binary[MESA_SHADER_VERTEX].size,
                         cache_line_size,
                         &gfx_pipeline->shader_state.vertex.bo);
   if (result != VK_SUCCESS)
      goto err_free_build_context;

   if (ctx->nir[MESA_SHADER_FRAGMENT]) {
      struct pvr_fragment_shader_state *fragment_state =
         &gfx_pipeline->shader_state.fragment;

      pvr_fragment_state_init(gfx_pipeline,
                              &ctx->common_data[MESA_SHADER_FRAGMENT],
                              &ctx->stage_data.fs);

      /* FIXME: For now we just overwrite it but the compiler shouldn't be
       * returning the sh count since the driver is in charge of allocating
       * them.
       */
      fragment_state->stage_state.const_shared_reg_count =
         sh_count[PVR_STAGE_ALLOCATION_FRAGMENT];

      result = pvr_gpu_upload_usc(
         device,
         util_dynarray_begin(&ctx->binary[MESA_SHADER_FRAGMENT]),
         ctx->binary[MESA_SHADER_FRAGMENT].size,
         cache_line_size,
         &gfx_pipeline->shader_state.fragment.bo);
      if (result != VK_SUCCESS)
         goto err_free_vertex_bo;

      /* TODO: powervr has an optimization where it attempts to recompile
       * shaders. See PipelineCompileNoISPFeedbackFragmentStage. Unimplemented
       * since in our case the optimization doesn't happen.
       */

      pvr_generate_iterator_commands(ctx);

      result = pvr_pds_coeff_program_create_and_upload(
         device,
         allocator,
         ctx->stage_data.fs.iterator_args.fpu_iterators,
         ctx->stage_data.fs.iterator_args.num_fpu_iterators,
         ctx->stage_data.fs.iterator_args.destination,
         &fragment_state->pds_coeff_program,
         &fragment_state->stage_state.pds_temps_count);
      if (result != VK_SUCCESS)
         goto err_free_fragment_bo;

      result = pvr_pds_fragment_program_create_and_upload(
         device,
         allocator,
         gfx_pipeline->shader_state.fragment.bo,
         ctx->common_data[MESA_SHADER_FRAGMENT].temps,
         ctx->stage_data.fs.msaa_mode,
         ctx->stage_data.fs.phas,
         &fragment_state->pds_fragment_program);
      if (result != VK_SUCCESS)
         goto err_free_coeff_program;

      result = pvr_pds_descriptor_program_create_and_upload(
         device,
         allocator,
         &ctx->common_data[MESA_SHADER_FRAGMENT].compile_time_consts_data,
         &ctx->common_data[MESA_SHADER_FRAGMENT].ubo_data,
         layout,
         PVR_STAGE_ALLOCATION_FRAGMENT,
         sh_reg_layout_frag,
         &ctx->preamble.binary[MESA_SHADER_FRAGMENT],
         ctx->common_data[MESA_SHADER_FRAGMENT].preamble.temps,
         &fragment_state->descriptor_state);
      if (result != VK_SUCCESS)
         goto err_free_frag_program;

      /* If not, we need to MAX2() and set
       * `fragment_state->stage_state.pds_temps_count` appropriately.
       */
      assert(fragment_state->descriptor_state.pds_info.temps_required == 0);
   }

   result = pvr_pds_vertex_attrib_programs_create_and_upload(
      device,
      allocator,
      ctx->common_data[MESA_SHADER_VERTEX].temps,
      &ctx->stage_data.vs.special_vars,
      vtx_dma_descriptions,
      vtx_dma_count,
      &gfx_pipeline->shader_state.vertex.pds_attrib_programs);
   if (result != VK_SUCCESS)
      goto err_free_frag_descriptor_program;

   result = pvr_pds_descriptor_program_create_and_upload(
      device,
      allocator,
      &ctx->common_data[MESA_SHADER_VERTEX].compile_time_consts_data,
      &ctx->common_data[MESA_SHADER_VERTEX].ubo_data,
      layout,
      PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY,
      sh_reg_layout_vert,
      &ctx->preamble.binary[MESA_SHADER_VERTEX],
      ctx->common_data[MESA_SHADER_VERTEX].preamble.temps,
      &gfx_pipeline->shader_state.vertex.descriptor_state);
   if (result != VK_SUCCESS)
      goto err_free_vertex_attrib_program;

   /* FIXME: When the temp_buffer_total_size is non-zero we need to allocate
    * a scratch buffer for both vertex and fragment stage. Figure out the
    * best place to do this.
    */
   /* assert(pvr_pds_descriptor_program_variables.temp_buff_total_size == 0); */
   /* TODO: Implement spilling with the above. */

   ralloc_free(ctx);

   return VK_SUCCESS;

err_free_vertex_attrib_program:
   for (uint32_t i = 0;
        i < ARRAY_SIZE(gfx_pipeline->shader_state.vertex.pds_attrib_programs);
        i++) {
      struct pvr_pds_attrib_program *const attrib_program =
         &gfx_pipeline->shader_state.vertex.pds_attrib_programs[i];

      pvr_pds_vertex_attrib_program_destroy(device, allocator, attrib_program);
   }
err_free_frag_descriptor_program:
   pvr_pds_descriptor_program_destroy(
      device,
      allocator,
      &gfx_pipeline->shader_state.fragment.descriptor_state);
err_free_frag_program:
   pvr_bo_suballoc_free(
      gfx_pipeline->shader_state.fragment.pds_fragment_program.pvr_bo);
err_free_coeff_program:
   pvr_bo_suballoc_free(
      gfx_pipeline->shader_state.fragment.pds_coeff_program.pvr_bo);
err_free_fragment_bo:
   pvr_bo_suballoc_free(gfx_pipeline->shader_state.fragment.bo);
err_free_vertex_bo:
   pvr_bo_suballoc_free(gfx_pipeline->shader_state.vertex.bo);
err_free_build_context:
   ralloc_free(ctx);
   return result;
}

static struct vk_render_pass_state
pvr_create_renderpass_state(const VkGraphicsPipelineCreateInfo *const info)
{
   PVR_FROM_HANDLE(pvr_render_pass, pass, info->renderPass);
   const struct pvr_render_subpass *const subpass =
      &pass->subpasses[info->subpass];

   VkImageAspectFlags attachment_aspects = VK_IMAGE_ASPECT_NONE;

   assert(info->subpass < pass->subpass_count);

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      if (subpass->color_attachments[i] == VK_ATTACHMENT_UNUSED)
         continue;

      attachment_aspects |=
         pass->attachments[subpass->color_attachments[i]].aspects;
   }

   if (subpass->depth_stencil_attachment != VK_ATTACHMENT_UNUSED) {
      attachment_aspects |=
         pass->attachments[subpass->depth_stencil_attachment].aspects;
   }

   return (struct vk_render_pass_state){
      .attachment_aspects = attachment_aspects,
      .render_pass = info->renderPass,
      .subpass = info->subpass,

      /* TODO: This is only needed for VK_KHR_create_renderpass2 (or
       * core 1.2), which is not currently supported.
       */
      .view_mask = 0,
   };
}

static VkResult
pvr_graphics_pipeline_init(struct pvr_device *device,
                           struct vk_pipeline_cache *cache,
                           const VkGraphicsPipelineCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *allocator,
                           struct pvr_graphics_pipeline *gfx_pipeline)
{
   struct vk_dynamic_graphics_state *const dynamic_state =
      &gfx_pipeline->dynamic_state;
   const struct vk_render_pass_state rp_state =
      pvr_create_renderpass_state(pCreateInfo);

   struct vk_graphics_pipeline_all_state all_state;
   struct vk_graphics_pipeline_state state = { 0 };

   VkResult result;

   pvr_pipeline_init(device, PVR_PIPELINE_TYPE_GRAPHICS, &gfx_pipeline->base);

   result = vk_graphics_pipeline_state_fill(&device->vk,
                                            &state,
                                            pCreateInfo,
                                            &rp_state,
                                            &all_state,
                                            NULL,
                                            0,
                                            NULL);
   if (result != VK_SUCCESS)
      goto err_pipeline_finish;

   vk_dynamic_graphics_state_init(dynamic_state);

   /* Load static state into base dynamic state holder. */
   vk_dynamic_graphics_state_fill(dynamic_state, &state);

   /* The value of ms.rasterization_samples is undefined when
    * rasterizer_discard_enable is set, but we need a specific value.
    * Fill that in here.
    */
   if (state.rs->rasterizer_discard_enable)
      dynamic_state->ms.rasterization_samples = VK_SAMPLE_COUNT_1_BIT;

   memset(gfx_pipeline->stage_indices, ~0, sizeof(gfx_pipeline->stage_indices));

   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      VkShaderStageFlagBits vk_stage = pCreateInfo->pStages[i].stage;
      gl_shader_stage gl_stage = vk_to_mesa_shader_stage(vk_stage);
      /* From the Vulkan 1.2.192 spec for VkPipelineShaderStageCreateInfo:
       *
       *    "stage must not be VK_SHADER_STAGE_ALL_GRAPHICS,
       *    or VK_SHADER_STAGE_ALL."
       *
       * So we don't handle that.
       *
       * We also don't handle VK_SHADER_STAGE_TESSELLATION_* and
       * VK_SHADER_STAGE_GEOMETRY_BIT stages as 'tessellationShader' and
       * 'geometryShader' are set to false in the VkPhysicalDeviceFeatures
       * structure returned by the driver.
       */
      switch (pCreateInfo->pStages[i].stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:
      case VK_SHADER_STAGE_FRAGMENT_BIT:
         gfx_pipeline->stage_indices[gl_stage] = i;
         break;
      default:
         unreachable("Unsupported stage.");
      }
   }

   gfx_pipeline->base.layout =
      pvr_pipeline_layout_from_handle(pCreateInfo->layout);

   /* Compiles and uploads shaders and PDS programs. */
   result = pvr_graphics_pipeline_compile(device,
                                          cache,
                                          pCreateInfo,
                                          allocator,
                                          gfx_pipeline,
                                          &state);
   if (result != VK_SUCCESS)
      goto err_pipeline_finish;

   return VK_SUCCESS;

err_pipeline_finish:
   pvr_pipeline_finish(&gfx_pipeline->base);

   return result;
}

/* If allocator == NULL, the internal one will be used. */
static VkResult
pvr_graphics_pipeline_create(struct pvr_device *device,
                             struct vk_pipeline_cache *cache,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *allocator,
                             VkPipeline *const pipeline_out)
{
   struct pvr_graphics_pipeline *gfx_pipeline;
   VkResult result;

   gfx_pipeline = vk_zalloc2(&device->vk.alloc,
                             allocator,
                             sizeof(*gfx_pipeline),
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!gfx_pipeline)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Compiles and uploads shaders and PDS programs too. */
   result = pvr_graphics_pipeline_init(device,
                                       cache,
                                       pCreateInfo,
                                       allocator,
                                       gfx_pipeline);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, gfx_pipeline);
      return result;
   }

   *pipeline_out = pvr_pipeline_to_handle(&gfx_pipeline->base);

   return VK_SUCCESS;
}

VkResult
pvr_CreateGraphicsPipelines(VkDevice _device,
                            VkPipelineCache pipelineCache,
                            uint32_t createInfoCount,
                            const VkGraphicsPipelineCreateInfo *pCreateInfos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);
   PVR_FROM_HANDLE(pvr_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      const VkResult local_result =
         pvr_graphics_pipeline_create(device,
                                      cache,
                                      &pCreateInfos[i],
                                      pAllocator,
                                      &pPipelines[i]);
      if (local_result != VK_SUCCESS) {
         result = local_result;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   return result;
}

/*****************************************************************************
   Other functions
*****************************************************************************/

void pvr_DestroyPipeline(VkDevice _device,
                         VkPipeline _pipeline,
                         const VkAllocationCallbacks *pAllocator)
{
   PVR_FROM_HANDLE(pvr_pipeline, pipeline, _pipeline);
   PVR_FROM_HANDLE(pvr_device, device, _device);

   if (!pipeline)
      return;

   switch (pipeline->type) {
   case PVR_PIPELINE_TYPE_GRAPHICS: {
      struct pvr_graphics_pipeline *const gfx_pipeline =
         to_pvr_graphics_pipeline(pipeline);

      pvr_graphics_pipeline_destroy(device, pAllocator, gfx_pipeline);
      break;
   }

   case PVR_PIPELINE_TYPE_COMPUTE: {
      struct pvr_compute_pipeline *const compute_pipeline =
         to_pvr_compute_pipeline(pipeline);

      pvr_compute_pipeline_destroy(device, pAllocator, compute_pipeline);
      break;
   }

   default:
      unreachable("Unknown pipeline type.");
   }
}
