/*
** Copyright (c) 2024 LunarG, Inc.
**
** Permission is hereby granted, free of charge, to any person obtaining a
** copy of this software and associated documentation files (the "Software"),
** to deal in the Software without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Software, and to permit persons to whom the
** Software is furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
*/

#include "decode/vulkan_object_info.h"
#include "decode/vulkan_replay_dump_resources_draw_calls.h"
#include "decode/vulkan_replay_dump_resources_delegate.h"
#include "format/format.h"
#include "generated/generated_vulkan_enum_to_string.h"
#include "graphics/vulkan_resources_util.h"
#include "nlohmann/json.hpp"
#include "util/image_writer.h"
#include "util/buffer_writer.h"
#include "Vulkan-Utility-Libraries/vk_format_utils.h"
#include "util/logging.h"
#include "util/platform.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <sys/types.h>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <sstream>
#include <vector>
#include <vulkan/vulkan_core.h>
#if !defined(WIN32)
#include <dirent.h>
#endif

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(decode)

DrawCallsDumpingContext::DrawCallsDumpingContext(const std::vector<uint64_t>&              dc_indices,
                                                 const std::vector<std::vector<uint64_t>>& rp_indices,
                                                 CommonObjectInfoTable&                    object_info_table,
                                                 const VulkanReplayOptions&                options,
                                                 VulkanDumpResourcesDelegate&              delegate) :
    original_command_buffer_info(nullptr), current_cb_index(0), dc_indices(dc_indices), RP_indices(rp_indices),
    active_renderpass(nullptr), active_framebuffer(nullptr), bound_gr_pipeline{ nullptr }, current_renderpass(0),
    current_subpass(0), dump_resources_before(options.dump_resources_before), aux_command_buffer(VK_NULL_HANDLE),
    aux_fence(VK_NULL_HANDLE), device_table(nullptr), instance_table(nullptr), object_info_table(object_info_table),
    replay_device_phys_mem_props(nullptr), delegate_(delegate), dump_depth(options.dump_resources_dump_depth),
    color_attachment_to_dump(options.dump_resources_color_attachment_index),
    dump_vertex_index_buffers(options.dump_resources_dump_vertex_index_buffer),
    dump_immutable_resources(options.dump_resources_dump_immutable_resources), current_render_pass_type(kNone)
{
    must_backup_resources = (dc_indices.size() > 1);

    const size_t n_cmd_buffs = dump_resources_before ? 2 * dc_indices.size() : dc_indices.size();
    command_buffers.resize(n_cmd_buffs, VK_NULL_HANDLE);

    const size_t n_render_passes = rp_indices.size();
    render_pass_dumped_descriptors.resize(n_render_passes);
}

DrawCallsDumpingContext::~DrawCallsDumpingContext()
{
    Release();
}

void DrawCallsDumpingContext::Release()
{
    if (original_command_buffer_info)
    {
        const VulkanDeviceInfo* device_info =
            object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);

        if (device_info == nullptr)
        {
            return;
        }

        VkDevice device = device_info->handle;
        assert(device_table);

        const VulkanCommandPoolInfo* pool_info =
            object_info_table.GetVkCommandPoolInfo(original_command_buffer_info->pool_id);
        assert(pool_info);

        if (command_buffers.size())
        {
            device_table->FreeCommandBuffers(device, pool_info->handle, command_buffers.size(), command_buffers.data());
        }
        command_buffers.clear();

        if (aux_command_buffer != VK_NULL_HANDLE)
        {
            device_table->FreeCommandBuffers(device, pool_info->handle, 1, &aux_command_buffer);
            aux_command_buffer = VK_NULL_HANDLE;
        }

        if (aux_fence != VK_NULL_HANDLE)
        {
            device_table->DestroyFence(device, aux_fence, nullptr);
        }

        DestroyMutableResourceBackups();
        ReleaseIndirectParams();

        original_command_buffer_info = nullptr;
    }

    draw_call_params.clear();
    dc_indices.clear();
    RP_indices.clear();
    render_pass_dumped_descriptors.clear();

    current_renderpass = 0;
    current_subpass    = 0;
    n_subpasses        = 0;
    current_cb_index   = 0;
}

void DrawCallsDumpingContext::InsertNewDrawParameters(
    uint64_t index, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
    auto new_entry = draw_call_params.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(index),
        std::forward_as_tuple(DrawCallTypes::kDraw, vertex_count, instance_count, first_vertex, first_instance));
    assert(new_entry.second);
}

void DrawCallsDumpingContext::InsertNewDrawIndexedParameters(uint64_t index,
                                                             uint32_t index_count,
                                                             uint32_t instance_count,
                                                             uint32_t first_index,
                                                             int32_t  vertexOffset,
                                                             uint32_t first_instance)
{
    auto new_entry = draw_call_params.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(index),
        std::forward_as_tuple(
            DrawCallTypes::kDrawIndexed, index_count, instance_count, first_index, vertexOffset, first_instance));
    assert(new_entry.second);
}

void DrawCallsDumpingContext::InsertNewDrawIndirectParameters(
    uint64_t index, const VulkanBufferInfo* buffer_info, VkDeviceSize offset, uint32_t draw_count, uint32_t stride)
{
    auto new_entry =
        draw_call_params.emplace(std::piecewise_construct,
                                 std::forward_as_tuple(index),
                                 std::forward_as_tuple(kDrawIndirect, buffer_info, offset, draw_count, stride));
    assert(new_entry.second);
}

void DrawCallsDumpingContext::InsertNewDrawIndexedIndirectParameters(
    uint64_t index, const VulkanBufferInfo* buffer_info, VkDeviceSize offset, uint32_t draw_count, uint32_t stride)
{
    auto new_entry = draw_call_params.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(index),
        std::forward_as_tuple(DrawCallTypes::kDrawIndexedIndirect, buffer_info, offset, draw_count, stride));
    assert(new_entry.second);
}

void DrawCallsDumpingContext::InsertNewIndirectCountParameters(uint64_t                index,
                                                               const VulkanBufferInfo* buffer_info,
                                                               VkDeviceSize            offset,
                                                               const VulkanBufferInfo* count_buffer_info,
                                                               VkDeviceSize            count_buffer_offset,
                                                               uint32_t                max_draw_count,
                                                               uint32_t                stride)
{
    auto new_entry = draw_call_params.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(index),
                                              std::forward_as_tuple(DrawCallTypes::kDrawIndirectCount,
                                                                    buffer_info,
                                                                    offset,
                                                                    count_buffer_info,
                                                                    count_buffer_offset,
                                                                    max_draw_count,
                                                                    stride));
    assert(new_entry.second);
}

void DrawCallsDumpingContext::InsertNewDrawIndexedIndirectCountParameters(uint64_t                index,
                                                                          const VulkanBufferInfo* buffer_info,
                                                                          VkDeviceSize            offset,
                                                                          const VulkanBufferInfo* count_buffer_info,
                                                                          VkDeviceSize            count_buffer_offset,
                                                                          uint32_t                max_draw_count,
                                                                          uint32_t                stride)
{
    auto new_entry = draw_call_params.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(index),
                                              std::forward_as_tuple(DrawCallTypes::kDrawIndexedIndirectCount,
                                                                    buffer_info,
                                                                    offset,
                                                                    count_buffer_info,
                                                                    count_buffer_offset,
                                                                    max_draw_count,
                                                                    stride));
    assert(new_entry.second);
}

void DrawCallsDumpingContext::InsertNewDrawIndirectCountKHRParameters(uint64_t                index,
                                                                      const VulkanBufferInfo* buffer_info,
                                                                      VkDeviceSize            offset,
                                                                      const VulkanBufferInfo* count_buffer_info,
                                                                      VkDeviceSize            count_buffer_offset,
                                                                      uint32_t                max_draw_count,
                                                                      uint32_t                stride)
{
    auto new_entry = draw_call_params.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(index),
                                              std::forward_as_tuple(DrawCallTypes::kDrawIndirectCountKHR,
                                                                    buffer_info,
                                                                    offset,
                                                                    count_buffer_info,
                                                                    count_buffer_offset,
                                                                    max_draw_count,
                                                                    stride));
    assert(new_entry.second);
}

void DrawCallsDumpingContext::InsertNewDrawIndexedIndirectCountKHRParameters(uint64_t                index,
                                                                             const VulkanBufferInfo* buffer_info,
                                                                             VkDeviceSize            offset,
                                                                             const VulkanBufferInfo* count_buffer_info,
                                                                             VkDeviceSize count_buffer_offset,
                                                                             uint32_t     max_draw_count,
                                                                             uint32_t     stride)
{
    auto new_entry = draw_call_params.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(index),
                                              std::forward_as_tuple(DrawCallTypes::kDrawIndexedIndirectCountKHR,
                                                                    buffer_info,
                                                                    offset,
                                                                    count_buffer_info,
                                                                    count_buffer_offset,
                                                                    max_draw_count,
                                                                    stride));
    assert(new_entry.second);
}

VkResult DrawCallsDumpingContext::CopyDrawIndirectParameters(uint64_t index)
{
    auto entry = draw_call_params.find(index);
    assert(entry != draw_call_params.end());

    DrawCallParameters& dc_params = entry->second;

    assert(IsDrawCallIndirect(dc_params.type));

    if (IsDrawCallIndirectCount(dc_params.type))
    {
        DrawCallParameters::DrawCallParamsUnion::DrawIndirectCountParams& ic_params =
            dc_params.dc_params_union.draw_indirect_count;

        const uint32_t max_draw_count = ic_params.max_draw_count;

        // Not sure from spec if maxDrawCount can be zero. Assume it can
        if (!max_draw_count)
        {
            return VK_SUCCESS;
        }

        const VkDeviceSize draw_call_params_size =
            IsDrawCallIndexed(dc_params.type) ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);

        // Create a buffer to copy the parameters buffer
        //
        // #VUID-vkCmdDrawIndexedIndirectCount-maxDrawCount-03143:
        // ---------------------------------------------------
        // If maxDrawCount is greater than or equal to 1,
        // (stride × (maxDrawCount - 1) + offset + sizeof(VkDrawIndexedIndirectCommand))
        //  must be less than or equal to the size of buffer
        // ---------------------------------------------------
        const uint32_t     param_buffer_stride = ic_params.stride;
        VkDeviceSize       param_buffer_offset = ic_params.params_buffer_offset;
        const VkDeviceSize copy_buffer_size    = param_buffer_stride * (max_draw_count - 1) + draw_call_params_size;
        assert(copy_buffer_size <= ic_params.params_buffer_info->size + param_buffer_offset);

        ic_params.new_params_buffer_size = copy_buffer_size;

        VkResult res = CloneBuffer(object_info_table,
                                   device_table,
                                   replay_device_phys_mem_props,
                                   ic_params.params_buffer_info,
                                   &ic_params.new_params_buffer,
                                   &ic_params.new_params_memory,
                                   copy_buffer_size);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Failed cloning vk buffer (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }

        // Inject a cmdCopyBuffer to copy the draw params into the new buffer
        {
            std::vector<VkBufferCopy> regions(max_draw_count);
            if (param_buffer_stride != draw_call_params_size)
            {
                VkDeviceSize dst_offset = 0;
                for (uint32_t i = 0; i < max_draw_count; ++i)
                {
                    regions[i].size = draw_call_params_size;

                    regions[i].srcOffset = param_buffer_offset;
                    param_buffer_offset += param_buffer_stride;

                    regions[i].dstOffset = dst_offset;
                    dst_offset += draw_call_params_size;
                }
                assert(param_buffer_offset == copy_buffer_size);
            }
            else
            {
                regions[0].size      = copy_buffer_size;
                regions[0].srcOffset = ic_params.params_buffer_offset;
                regions[0].dstOffset = 0;
            }

            VkCommandBuffer cmd_buf = command_buffers[current_cb_index];
            device_table->CmdCopyBuffer(cmd_buf,
                                        ic_params.params_buffer_info->handle,
                                        ic_params.new_params_buffer,
                                        regions.size(),
                                        regions.data());

            VkBufferMemoryBarrier buf_barrier;
            buf_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            buf_barrier.pNext               = nullptr;
            buf_barrier.buffer              = ic_params.new_params_buffer;
            buf_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            buf_barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
            buf_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf_barrier.size                = copy_buffer_size;
            buf_barrier.offset              = 0;

            device_table->CmdPipelineBarrier(cmd_buf,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VkDependencyFlags{ 0 },
                                             0,
                                             nullptr,
                                             1,
                                             &buf_barrier,
                                             0,
                                             nullptr);
        }

        // Create a buffer to copy the draw count parameter
        const VkDeviceSize count_buffer_size = sizeof(uint32_t);
        assert(count_buffer_size <= ic_params.count_buffer_info->size);
        res = CloneBuffer(object_info_table,
                          device_table,
                          replay_device_phys_mem_props,
                          ic_params.count_buffer_info,
                          &ic_params.new_count_buffer,
                          &ic_params.new_count_memory,
                          count_buffer_size);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Failed cloning vk buffer (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }

        // Inject a cmdCopyBuffer to copy the count into the new buffer
        {
            VkBufferCopy region;
            region.size      = count_buffer_size;
            region.srcOffset = ic_params.count_buffer_offset;
            region.dstOffset = 0;

            VkCommandBuffer cmd_buf = command_buffers[current_cb_index];
            device_table->CmdCopyBuffer(
                cmd_buf, ic_params.params_buffer_info->handle, ic_params.new_params_buffer, 1, &region);

            VkBufferMemoryBarrier buf_barrier;
            buf_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            buf_barrier.pNext               = nullptr;
            buf_barrier.buffer              = ic_params.new_count_buffer;
            buf_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            buf_barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
            buf_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf_barrier.size                = count_buffer_size;
            buf_barrier.offset              = 0;

            device_table->CmdPipelineBarrier(cmd_buf,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VkDependencyFlags{ 0 },
                                             0,
                                             nullptr,
                                             1,
                                             &buf_barrier,
                                             0,
                                             nullptr);
        }
    }
    else
    {
        DrawCallParameters::DrawCallParamsUnion::DrawIndirectParams& i_params = dc_params.dc_params_union.draw_indirect;

        const uint32_t draw_count = i_params.draw_count;

        // According to spec drawCount can be zero. Nothing to do in this case
        if (!draw_count)
        {
            return VK_SUCCESS;
        }

        const VkDeviceSize draw_call_params_size =
            IsDrawCallIndexed(dc_params.type) ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);

        // Create a buffer to copy the parameters buffer
        //
        // VUID-vkCmdDrawIndexedIndirect-drawCount-00540
        // If drawCount is greater than 1, (stride × (drawCount - 1) + offset + sizeof(VkDrawIndexedIndirectCommand))
        // must be less than or equal to the size of buffer
        const uint32_t     param_buffer_stride = i_params.stride;
        const uint32_t     param_buffer_offset = i_params.params_buffer_offset;
        const VkDeviceSize copy_buffer_size =
            (draw_count > 1) ? (param_buffer_stride * (draw_count - 1) + draw_call_params_size) : draw_call_params_size;
        assert(copy_buffer_size <= i_params.params_buffer_info->size + param_buffer_offset);

        i_params.new_params_buffer_size = copy_buffer_size;

        VkResult res = CloneBuffer(object_info_table,
                                   device_table,
                                   replay_device_phys_mem_props,
                                   i_params.params_buffer_info,
                                   &i_params.new_params_buffer,
                                   &i_params.new_params_memory,
                                   copy_buffer_size);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Failed cloning vk buffer (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }

        // Inject a cmdCopyBuffer to copy the draw params into the new buffer
        {
            std::vector<VkBufferCopy> regions(draw_count);
            if (param_buffer_stride != draw_call_params_size)
            {
                VkDeviceSize src_offset = param_buffer_offset;
                VkDeviceSize dst_offset = 0;
                for (uint32_t i = 0; i < draw_count; ++i)
                {
                    regions[i].size = draw_call_params_size;

                    regions[i].srcOffset = src_offset;
                    src_offset += param_buffer_stride;

                    regions[i].dstOffset = dst_offset;
                    dst_offset += draw_call_params_size;
                }
                assert(src_offset == copy_buffer_size);
            }
            else
            {
                regions[0].size      = copy_buffer_size;
                regions[0].srcOffset = i_params.params_buffer_offset;
                regions[0].dstOffset = 0;
            }

            VkCommandBuffer cmd_buf = command_buffers[current_cb_index];
            device_table->CmdCopyBuffer(cmd_buf,
                                        i_params.params_buffer_info->handle,
                                        i_params.new_params_buffer,
                                        regions.size(),
                                        regions.data());

            VkBufferMemoryBarrier buf_barrier;
            buf_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            buf_barrier.pNext               = nullptr;
            buf_barrier.buffer              = i_params.new_params_buffer;
            buf_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            buf_barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
            buf_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf_barrier.size                = copy_buffer_size;
            buf_barrier.offset              = 0;

            device_table->CmdPipelineBarrier(cmd_buf,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VkDependencyFlags{ 0 },
                                             0,
                                             nullptr,
                                             1,
                                             &buf_barrier,
                                             0,
                                             nullptr);
        }
    }

    return VK_SUCCESS;
}

void DrawCallsDumpingContext::SnapshotBoundDescriptors(uint64_t index)
{
    auto entry = draw_call_params.find(index);
    assert(entry != draw_call_params.end());

    DrawCallParameters& dc_params = entry->second;

    // Iterate all bound descriptors
    for (const auto& desc_set : bound_descriptor_sets_gr)
    {
        const uint32_t desc_set_index = desc_set.first;
        for (const auto& desc : desc_set.second.descriptors)
        {
            const uint32_t desc_binding_index                                    = desc.first;
            dc_params.referenced_descriptors[desc_set_index][desc_binding_index] = desc.second;
        }
    }
}

void DrawCallsDumpingContext::CopyVertexInputStateInfo(uint64_t dc_index)
{
    auto entry = draw_call_params.find(dc_index);
    assert(entry != draw_call_params.end());

    DrawCallParameters& dc_params = entry->second;

    assert(bound_gr_pipeline != nullptr);

    // Pipeline has no vertex binding and/or attribute information.
    // This can be a case of shader generated vertices, or vertex buffer is bound as a UBO
    if (bound_gr_pipeline != nullptr &&
        (!bound_gr_pipeline->vertex_input_attribute_map.size() &&
         !bound_gr_pipeline->vertex_input_attribute_map.size()) &&
        (!bound_gr_pipeline->dynamic_vertex_input && !bound_gr_pipeline->dynamic_vertex_binding_stride))
    {
        return;
    }

    // If VK_DYNAMIC_STATE_VERTEX_INPUT_EXT is enabled then get all vertex input state from
    // vkCmdSetVertexInputEXT
    if (bound_gr_pipeline == nullptr || bound_gr_pipeline->dynamic_vertex_input)
    {
        dc_params.vertex_input_state = dynamic_vertex_input_state;
    }
    else
    {
        if (bound_gr_pipeline)
        {
            // Copy vertex input binding state
            dc_params.vertex_input_state.vertex_input_binding_map = bound_gr_pipeline->vertex_input_binding_map;

            // If VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT is enabled, ignore strides from
            // pipeline and get them from vkCmdBindVertexBuffers2EXT instead
            if (bound_gr_pipeline->dynamic_vertex_binding_stride)
            {
                for (auto& vb_binding : dc_params.vertex_input_state.vertex_input_binding_map)
                {
                    const uint32_t binding = vb_binding.first;
                    if (dynamic_vertex_input_state.vertex_input_binding_map.find(binding) !=
                        dynamic_vertex_input_state.vertex_input_binding_map.end())
                    {
                        vb_binding.second.stride = dynamic_vertex_input_state.vertex_input_binding_map[binding].stride;
                    }
                }
            }

            // Copy vertex attributes info
            dc_params.vertex_input_state.vertex_input_attribute_map = bound_gr_pipeline->vertex_input_attribute_map;
        }
    }

    // Keep a copy of the bound vertex buffers information
    dc_params.referenced_vertex_buffers = bound_vertex_buffers;

    if (IsDrawCallIndexed(dc_params.type))
    {
        dc_params.referenced_index_buffer = bound_index_buffer;
    }
}

void DrawCallsDumpingContext::FinalizeCommandBuffer()
{
    assert(current_render_pass_type == kRenderPass || current_render_pass_type == kDynamicRendering);
    assert(current_cb_index < command_buffers.size());
    assert(device_table != nullptr);

    VkCommandBuffer current_command_buffer = command_buffers[current_cb_index];

    if (current_render_pass_type == kRenderPass)
    {
        device_table->CmdEndRenderPass(current_command_buffer);
    }
    else
    {
        device_table->CmdEndRenderingKHR(current_command_buffer);

        // Transition render targets into TRANSFER_SRC_OPTIMAL
        assert(current_renderpass == render_targets.size() - 1);
        assert(render_targets[current_renderpass].size() == 1);
        for (auto& rt : render_targets[current_renderpass])
        {
            for (auto& cat : rt.color_att_imgs)
            {
                if (cat->intermediate_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                {
                    VkImageMemoryBarrier barrier;
                    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barrier.pNext               = nullptr;
                    barrier.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.oldLayout           = cat->intermediate_layout;
                    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    barrier.image               = cat->handle;
                    barrier.subresourceRange    = {
                           VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS
                    };

                    device_table->CmdPipelineBarrier(current_command_buffer,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                     0,
                                                     0,
                                                     nullptr,
                                                     0,
                                                     nullptr,
                                                     1,
                                                     &barrier);

                    cat->intermediate_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                }
            }
        }
    }

    device_table->EndCommandBuffer(current_command_buffer);

    // Increment index of command buffer that is going to be finalized next
    ++current_cb_index;
}

bool DrawCallsDumpingContext::MustDumpDrawCall(uint64_t index) const
{
    // Indices should be sorted
    if (!IsInsideRange(dc_indices, index))
    {
        return false;
    }

    for (size_t i = dump_resources_before ? current_cb_index / 2 : current_cb_index; i < dc_indices.size(); ++i)
    {
        if (index == dc_indices[i])
        {
            return true;
        }
        else if (index > dc_indices[i])
        {
            // Indices should be sorted
            return false;
        }
    }

    return false;
}

bool DrawCallsDumpingContext::ShouldHandleRenderPass(uint64_t index) const
{
    for (const auto& rp : RP_indices)
    {
        if (IsInsideRange(rp, index))
        {
            return true;
        }
    }

    return false;
}

VkResult DrawCallsDumpingContext::DumpDrawCalls(
    VkQueue queue, uint64_t qs_index, uint64_t bcb_index, const VkSubmitInfo& submit_info, VkFence fence)
{
    const size_t n_drawcalls = command_buffers.size();

    // Dump render targets
    for (size_t cb = 0; cb < n_drawcalls; ++cb)
    {
        VkSubmitInfo si;
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext                = nullptr;
        si.waitSemaphoreCount   = !cb ? submit_info.waitSemaphoreCount : 0;
        si.pWaitSemaphores      = !cb ? submit_info.pWaitSemaphores : nullptr;
        si.pWaitDstStageMask    = !cb ? submit_info.pWaitDstStageMask : nullptr;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &command_buffers[cb];
        si.signalSemaphoreCount = (cb == (n_drawcalls - 1)) ? submit_info.signalSemaphoreCount : 0;
        si.pSignalSemaphores    = (cb == (n_drawcalls - 1)) ? submit_info.pSignalSemaphores : nullptr;

        const VulkanDeviceInfo* device_info =
            object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
        assert(device_info);

        const VkFenceCreateInfo ci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0 };
        VkFence                 submission_fence;
        if (fence == VK_NULL_HANDLE)
        {
            VkResult res = device_table->CreateFence(device_info->handle, &ci, nullptr, &submission_fence);
            if (res != VK_SUCCESS)
            {
                GFXRECON_LOG_ERROR("CreateFence failed with %s", util::ToString<VkResult>(res).c_str());
                return res;
            }
        }
        else
        {
            submission_fence = fence;
        }

        VkResult res = device_table->QueueSubmit(queue, 1, &si, submission_fence);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR(
                "(%s:%u) QueueSubmit failed with %s", __FILE__, __LINE__, util::ToString<VkResult>(res).c_str());
            return res;
        }

        // Wait
        res = device_table->WaitForFences(device_info->handle, 1, &submission_fence, VK_TRUE, ~0UL);
        if (res != VK_SUCCESS)
        {
            device_table->DestroyFence(device_info->handle, submission_fence, nullptr);
            GFXRECON_LOG_ERROR("WaitForFences failed with %s", util::ToString<VkResult>(res).c_str());
            return res;
        }

        if (fence == VK_NULL_HANDLE)
        {
            device_table->DestroyFence(device_info->handle, submission_fence, nullptr);
        }

        const size_t                dc_index = dc_indices[CmdBufToDCVectorIndex(cb)];
        const RenderPassSubpassPair RP_index = GetRenderPassIndex(dc_index);
        const uint64_t              sp       = RP_index.second;
        const uint64_t              rp       = RP_index.first;

        // Fetch draw params for all Indirect and IndirectCount draw calls from the buffers
        // into the DrawCallParameters
        res = FetchDrawIndirectParams(dc_index);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Fetching indirect draw parameters failed (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }

        // Dump vertex/index buffers
        if (dump_vertex_index_buffers && (!dump_resources_before || dump_resources_before && !(cb % 2)))
        {
            res = DumpVertexIndexBuffers(qs_index, bcb_index, dc_index);
            if (res != VK_SUCCESS)
            {
                GFXRECON_LOG_ERROR("Dumping vertex/index buffers failed (%s)", util::ToString<VkResult>(res).c_str())
                return res;
            }
        }

        // Dump render targets
        res = DumpRenderTargetAttachments(cb, rp, sp, qs_index, bcb_index);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Dumping render target attachments failed (%s)", util::ToString<VkResult>(res).c_str())
            return res;
        }

        // Dump immutable resources
        if (dump_immutable_resources && (!dump_resources_before || dump_resources_before && !(cb % 2)))
        {
            VkResult res = DumpImmutableDescriptors(qs_index, bcb_index, dc_index, rp);
            if (res != VK_SUCCESS)
            {
                GFXRECON_LOG_ERROR("Dumping immutable resources failed (%s)", util::ToString<VkResult>(res).c_str())
                return res;
            }
        }

        if (!dump_resources_before || dump_resources_before && !(cb % 2))
        {
            VulkanDumpDrawCallInfo draw_call_info{};
            draw_call_info.type                         = DumpResourceType::kDrawCallInfo;
            draw_call_info.instance_table               = instance_table;
            draw_call_info.device_table                 = device_table;
            draw_call_info.object_info_table            = &object_info_table;
            draw_call_info.device_info                  = device_info;
            draw_call_info.original_command_buffer_info = original_command_buffer_info;
            draw_call_info.bcb_index                    = bcb_index;
            draw_call_info.qs_index                     = qs_index;
            draw_call_info.rp                           = rp;
            draw_call_info.sp                           = sp;
            draw_call_info.cmd_index                    = dc_index;
            draw_call_info.render_targets               = &render_targets[rp][sp];

            const auto& dc_param_entry = draw_call_params.find(draw_call_info.cmd_index);
            GFXRECON_ASSERT(dc_param_entry != draw_call_params.end());
            draw_call_info.dc_param = &dc_param_entry->second;

            delegate_.DumpDrawCallInfo(draw_call_info);
        }

        res = RevertRenderTargetImageLayouts(queue, cb);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Reverting render target attachments layouts failed(%s)",
                               util::ToString<VkResult>(res).c_str())
            return res;
        }
    }

    // Clean up some state in case this command buffer is submitted again
    ResetFetchedIndirectParams();
    for (auto& rpc : render_pass_dumped_descriptors)
    {
        rpc.image_descriptors.clear();
        rpc.buffer_descriptors.clear();
        rpc.inline_uniform_blocks.clear();
    }

    GFXRECON_LOG_INFO("Done.")

    return VK_SUCCESS;
}

VkResult DrawCallsDumpingContext::RevertRenderTargetImageLayouts(VkQueue queue, uint64_t cmd_buf_index)
{
    const size_t                dc_index = dc_indices[CmdBufToDCVectorIndex(cmd_buf_index)];
    const RenderPassSubpassPair RP_index = GetRenderPassIndex(dc_index);
    const uint64_t              rp       = RP_index.first;
    const uint64_t              sp       = RP_index.second;

    if (!render_targets[rp][sp].color_att_imgs.size() && render_targets[rp][sp].depth_att_img == nullptr)
    {
        return VK_SUCCESS;
    }

    const auto entry = dynamic_rendering_attachment_layouts.find(rp);
    assert(entry != dynamic_rendering_attachment_layouts.end());

    if (!entry->second.is_dynamic)
    {
        return VK_SUCCESS;
    }

    const VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, 0, nullptr };
    VkResult                       res = device_table->BeginCommandBuffer(aux_command_buffer, &bi);
    if (res != VK_SUCCESS)
    {
        GFXRECON_LOG_ERROR(
            "(%s:%u) BeginCommandBuffer failed with %s", __FILE__, __LINE__, util::ToString<VkResult>(res).c_str());
        return res;
    }

    std::vector<VkImageMemoryBarrier> img_barriers;

    VkImageMemoryBarrier img_barrier;
    img_barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    img_barrier.pNext               = nullptr;
    img_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    img_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    img_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    img_barrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    img_barrier.subresourceRange    = {
           VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS
    };

    for (size_t i = 0; i < render_targets[rp][sp].color_att_imgs.size(); ++i)
    {
        if (color_attachment_to_dump != kUnspecifiedColorAttachment &&
            static_cast<size_t>(color_attachment_to_dump) != i)
        {
            continue;
        }

        VulkanImageInfo* image_info = render_targets[rp][sp].color_att_imgs[i];

        img_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        img_barrier.newLayout     = entry->second.color_attachment_layouts[i];
        img_barrier.image         = image_info->handle;
        img_barriers.push_back(img_barrier);

        image_info->intermediate_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    if (dump_depth && render_targets[rp][sp].depth_att_img != nullptr)
    {
        VulkanImageInfo* image_info = render_targets[rp][sp].depth_att_img;

        img_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        img_barrier.newLayout     = entry->second.depth_attachment_layout;
        img_barrier.image         = image_info->handle;
        img_barriers.push_back(img_barrier);

        image_info->intermediate_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    if (!img_barriers.empty())
    {
        device_table->CmdPipelineBarrier(aux_command_buffer,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         0,
                                         0,
                                         nullptr,
                                         0,
                                         nullptr,
                                         img_barriers.size(),
                                         img_barriers.data());

        res = device_table->EndCommandBuffer(aux_command_buffer);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR(
                "(%s:%u) EndCommandBuffer failed with %s", __FILE__, __LINE__, util::ToString<VkResult>(res).c_str());
            return res;
        }

        VkSubmitInfo si;
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext                = nullptr;
        si.waitSemaphoreCount   = 0;
        si.pWaitSemaphores      = nullptr;
        si.pWaitDstStageMask    = nullptr;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &aux_command_buffer;
        si.signalSemaphoreCount = 0;
        si.pSignalSemaphores    = nullptr;

        const VulkanDeviceInfo* device_info =
            object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
        assert(device_info);

        res = device_table->ResetFences(device_info->handle, 1, &aux_fence);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR(
                "(%s:%u) EndCommandBuffer failed with %s", __FILE__, __LINE__, util::ToString<VkResult>(res).c_str());
            return res;
        }

        res = device_table->QueueSubmit(queue, 1, &si, aux_fence);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR(
                "(%s:%u) QueueSubmit failed with %s", __FILE__, __LINE__, util::ToString<VkResult>(res).c_str());
            return res;
        }

        // Wait
        res = device_table->WaitForFences(device_info->handle, 1, &aux_fence, VK_TRUE, ~0UL);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR(
                "(%s:%u) WaitForFences failed with %s", __FILE__, __LINE__, util::ToString<VkResult>(res).c_str());
            return res;
        }
    }

    return VK_SUCCESS;
}

VkResult DrawCallsDumpingContext::DumpRenderTargetAttachments(
    uint64_t cmd_buf_index, uint64_t rp, uint64_t sp, uint64_t qs_index, uint64_t bcb_index)
{
    assert(device_table != nullptr);

    const size_t dc_index = dc_indices[CmdBufToDCVectorIndex(cmd_buf_index)];

    if (!render_targets[rp][sp].color_att_imgs.size() && render_targets[rp][sp].depth_att_img == nullptr)
    {
        return VK_SUCCESS;
    }

    assert(original_command_buffer_info);
    assert(original_command_buffer_info->parent_id != format::kNullHandleId);
    const VulkanDeviceInfo* device_info = object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info);

    VulkanDumpResourceInfo res_info_base{};
    res_info_base.device_info                  = device_info;
    res_info_base.device_table                 = device_table;
    res_info_base.instance_table               = instance_table;
    res_info_base.object_info_table            = &object_info_table;
    res_info_base.original_command_buffer_info = original_command_buffer_info;
    res_info_base.cmd_index                    = dc_index;
    res_info_base.qs_index                     = qs_index;
    res_info_base.bcb_index                    = bcb_index;
    res_info_base.before_cmd                   = dump_resources_before && !(cmd_buf_index % 2);
    res_info_base.rp                           = rp;
    res_info_base.sp                           = sp;

    // Dump color attachments
    for (size_t i = 0; i < render_targets[rp][sp].color_att_imgs.size(); ++i)
    {
        if (color_attachment_to_dump != kUnspecifiedColorAttachment &&
            static_cast<size_t>(color_attachment_to_dump) != i)
        {
            continue;
        }

        VulkanDumpResourceInfo res_info = res_info_base;
        res_info.type                   = DumpResourceType::kRtv;
        res_info.image_info             = render_targets[rp][sp].color_att_imgs[i];
        res_info.attachment_index       = i;
        auto res                        = delegate_.DumpResource(res_info);
        if (res != VK_SUCCESS)
        {
            return res;
        }
    }

    // Dump depth attachment
    if (dump_depth && render_targets[rp][sp].depth_att_img != nullptr)
    {
        VulkanDumpResourceInfo res_info = res_info_base;
        res_info.type                   = DumpResourceType::kDsv;
        res_info.image_info             = render_targets[rp][sp].depth_att_img;
        res_info.attachment_index       = DEPTH_ATTACHMENT;
        auto res                        = delegate_.DumpResource(res_info);
        if (res != VK_SUCCESS)
        {
            return res;
        }
    }

    return VK_SUCCESS;
}

VkResult
DrawCallsDumpingContext::DumpImmutableDescriptors(uint64_t qs_index, uint64_t bcb_index, uint64_t dc_index, uint64_t rp)
{
    assert(rp < render_pass_dumped_descriptors.size());
    assert(draw_call_params.find(dc_index) != draw_call_params.end());

    // Create a list of all descriptors referenced by all draw calls
    std::unordered_set<const VulkanImageInfo*> image_descriptors;

    struct buffer_descriptor_info
    {
        VkDeviceSize offset;
        VkDeviceSize range;
    };
    std::unordered_map<const VulkanBufferInfo*, buffer_descriptor_info> buffer_descriptors;

    struct inline_uniform_block_info
    {
        uint32_t                    set;
        uint32_t                    binding;
        const std::vector<uint8_t>* data;
    };
    std::unordered_map<const std::vector<uint8_t>*, inline_uniform_block_info> inline_uniform_blocks;

    auto dc_param_entry = draw_call_params.find(dc_index);
    GFXRECON_ASSERT(dc_param_entry != draw_call_params.end());

    const DrawCallParameters& dc_params = dc_param_entry->second;
    for (const auto& desc_set : dc_params.referenced_descriptors)
    {
        const uint32_t desc_set_index = desc_set.first;
        for (const auto& desc_binding : desc_set.second)
        {
            const uint32_t desc_binding_index = desc_binding.first;
            switch (desc_binding.second.desc_type)
            {
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                {
                    for (const auto& img_desc_info : desc_binding.second.image_info)
                    {
                        if (img_desc_info.second.image_view_info != nullptr)
                        {
                            const VulkanImageInfo* img_info =
                                object_info_table.GetVkImageInfo(img_desc_info.second.image_view_info->image_id);
                            if (img_info != nullptr &&
                                (render_pass_dumped_descriptors[rp].image_descriptors.find(img_info) ==
                                 render_pass_dumped_descriptors[rp].image_descriptors.end()))
                            {
                                image_descriptors.insert(img_info);
                                render_pass_dumped_descriptors[rp].image_descriptors.insert(img_info);
                            }
                        }
                    }
                }
                break;

                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                {
                    for (const auto& buf_desc_info : desc_binding.second.buffer_info)
                    {
                        const VulkanBufferInfo* buffer_info = buf_desc_info.second.buffer_info;
                        if (buffer_info != nullptr &&
                            (render_pass_dumped_descriptors[rp].buffer_descriptors.find(buffer_info) ==
                             render_pass_dumped_descriptors[rp].buffer_descriptors.end()))
                        {
                            buffer_descriptors.emplace(std::piecewise_construct,
                                                       std::forward_as_tuple(buffer_info),
                                                       std::forward_as_tuple(buffer_descriptor_info{
                                                           buf_desc_info.second.offset, buf_desc_info.second.range }));
                            render_pass_dumped_descriptors[rp].buffer_descriptors.insert(buffer_info);
                        }
                    }
                }
                break;

                case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                {
                    if (render_pass_dumped_descriptors[rp].inline_uniform_blocks.find(
                            &desc_binding.second.inline_uniform_block) ==
                        render_pass_dumped_descriptors[rp].inline_uniform_blocks.end())
                    {
                        inline_uniform_blocks[&(desc_binding.second.inline_uniform_block)] = {
                            desc_set_index, desc_binding_index, &(desc_binding.second.inline_uniform_block)
                        };
                        render_pass_dumped_descriptors[rp].inline_uniform_blocks.insert(
                            &desc_binding.second.inline_uniform_block);
                    }
                }
                break;

                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    break;

                default:
                    GFXRECON_LOG_WARNING_ONCE("%s(): Descriptor type (%s) not handled",
                                              __func__,
                                              util::ToString<VkDescriptorType>(desc_binding.second.desc_type).c_str());
                    break;
            }
        }
    }

    assert(original_command_buffer_info);
    assert(original_command_buffer_info->parent_id != format::kNullHandleId);
    const VulkanDeviceInfo* device_info = object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info);

    VulkanDumpResourceInfo res_info_base{};
    res_info_base.device_info                  = device_info;
    res_info_base.device_table                 = device_table;
    res_info_base.instance_table               = instance_table;
    res_info_base.object_info_table            = &object_info_table;
    res_info_base.original_command_buffer_info = original_command_buffer_info;
    res_info_base.cmd_index                    = dc_index;
    res_info_base.qs_index                     = qs_index;
    res_info_base.bcb_index                    = bcb_index;
    res_info_base.rp                           = rp;

    for (const auto& image_info : image_descriptors)
    {
        VulkanDumpResourceInfo res_info = res_info_base;
        res_info.type                   = DumpResourceType::kImageDescriptor;
        res_info.image_info             = image_info;
        auto res                        = delegate_.DumpResource(res_info);
        if (res != VK_SUCCESS)
        {
            return res;
        }
    }

    const VulkanPhysicalDeviceInfo* phys_dev_info = object_info_table.GetVkPhysicalDeviceInfo(device_info->parent_id);
    assert(phys_dev_info);

    graphics::VulkanResourcesUtil resource_util(device_info->handle,
                                                device_info->parent,
                                                *device_table,
                                                *instance_table,
                                                *phys_dev_info->replay_device_info->memory_properties);

    for (const auto& buf : buffer_descriptors)
    {
        VulkanDumpResourceInfo res_info = res_info_base;
        res_info.buffer_info            = buf.first;
        const VkDeviceSize offset       = buf.second.offset;
        const VkDeviceSize range        = buf.second.range;
        const VkDeviceSize size         = range == VK_WHOLE_SIZE ? res_info.buffer_info->size - offset : range;

        VkResult res = resource_util.ReadFromBufferResource(
            res_info.buffer_info->handle, size, offset, res_info.buffer_info->queue_family_index, res_info.data);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Reading from buffer resource %" PRIu64 " failed (%s).",
                               res_info.buffer_info->capture_id,
                               util::ToString<VkResult>(res).c_str())
            return res;
        }

        res_info.type = DumpResourceType::kBufferDescriptor;
        res           = delegate_.DumpResource(res_info);
        if (res != VK_SUCCESS)
        {
            return res;
        }
    }

    for (const auto& iub : inline_uniform_blocks)
    {
        VulkanDumpResourceInfo res_info = res_info_base;
        res_info.type                   = DumpResourceType::kInlineUniformBufferDescriptor;
        res_info.set                    = iub.second.set;
        res_info.binding                = iub.second.binding;
        res_info.data                   = *iub.second.data;
        auto res                        = delegate_.DumpResource(res_info);
        if (res != VK_SUCCESS)
        {
            return res;
        }
    }

    return VK_SUCCESS;
}

VkResult DrawCallsDumpingContext::FetchDrawIndirectParams(uint64_t dc_index)
{
    assert(original_command_buffer_info);
    assert(original_command_buffer_info->parent_id != format::kNullHandleId);
    const VulkanDeviceInfo* device_info = object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info);

    const VulkanPhysicalDeviceInfo* phys_dev_info = object_info_table.GetVkPhysicalDeviceInfo(device_info->parent_id);
    assert(phys_dev_info);

    graphics::VulkanResourcesUtil resource_util(device_info->handle,
                                                device_info->parent,
                                                *device_table,
                                                *instance_table,
                                                *phys_dev_info->replay_device_info->memory_properties);

    auto dc_param_entry = draw_call_params.find(dc_index);
    assert(dc_param_entry != draw_call_params.end());

    DrawCallParameters& dc_params = dc_param_entry->second;
    if (!IsDrawCallIndirect(dc_params.type))
    {
        return VK_SUCCESS;
    }

    if (IsDrawCallIndirectCount(dc_params.type))
    {
        DrawCallParameters::DrawCallParamsUnion::DrawIndirectCountParams& ic_params =
            dc_params.dc_params_union.draw_indirect_count;

        if (!ic_params.max_draw_count)
        {
            return VK_SUCCESS;
        }

        // Fetch draw count buffer
        std::vector<uint8_t> data;
        VkResult             res = resource_util.ReadFromBufferResource(
            ic_params.new_count_buffer, sizeof(uint32_t), 0, ic_params.count_buffer_info->queue_family_index, data);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Reading from buffer resource failed (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }

        assert(data.size() == sizeof(uint32_t));
        assert(ic_params.actual_draw_count == std::numeric_limits<uint32_t>::max());
        util::platform::MemoryCopy(&ic_params.actual_draw_count, sizeof(uint32_t), data.data(), data.size());
        assert(ic_params.actual_draw_count != std::numeric_limits<uint32_t>::max());

        if (!ic_params.actual_draw_count)
        {
            return VK_SUCCESS;
        }

        const uint32_t actual_draw_count = ic_params.actual_draw_count;

        VkDeviceSize params_actual_size;
        if (IsDrawCallIndexed(dc_params.type))
        {
            assert(ic_params.draw_indexed_params == nullptr);
            ic_params.draw_indexed_params = new VkDrawIndexedIndirectCommand[actual_draw_count];
            if (ic_params.draw_indexed_params == nullptr)
            {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            // Now we know the exact draw count. We can fetch the exact draw params instead of the whole buffer
            params_actual_size = sizeof(VkDrawIndexedIndirectCommand) * actual_draw_count;
        }
        else
        {
            assert(ic_params.draw_params == nullptr);
            ic_params.draw_params = new VkDrawIndirectCommand[actual_draw_count];
            if (ic_params.draw_params == nullptr)
            {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            // Now we know the exact draw count. We can fetch the exact draw params instead of the whole buffer
            params_actual_size = sizeof(VkDrawIndirectCommand) * actual_draw_count;
        }

        // Fetch param buffers
        res = resource_util.ReadFromBufferResource(
            ic_params.new_params_buffer, params_actual_size, 0, ic_params.params_buffer_info->queue_family_index, data);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Reading from buffer resource failed (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }

        assert(data.size() == params_actual_size);
        if (IsDrawCallIndexed(dc_params.type))
        {
            util::platform::MemoryCopy(
                ic_params.draw_indexed_params, params_actual_size, data.data(), params_actual_size);
        }
        else
        {
            util::platform::MemoryCopy(ic_params.draw_params, params_actual_size, data.data(), params_actual_size);
        }
    }
    else
    {
        DrawCallParameters::DrawCallParamsUnion::DrawIndirectParams& i_params = dc_params.dc_params_union.draw_indirect;

        if (!i_params.draw_count)
        {
            return VK_SUCCESS;
        }

        if (IsDrawCallIndexed(dc_params.type))
        {
            assert(i_params.draw_indexed_params == nullptr);
            i_params.draw_indexed_params = new VkDrawIndexedIndirectCommand[i_params.draw_count];
            if (i_params.draw_indexed_params == nullptr)
            {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        else
        {
            assert(i_params.draw_params == nullptr);
            i_params.draw_params = new VkDrawIndirectCommand[i_params.draw_count];
            if (i_params.draw_params == nullptr)
            {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }

        std::vector<uint8_t> params_data;
        VkResult             res = resource_util.ReadFromBufferResource(i_params.new_params_buffer,
                                                            i_params.new_params_buffer_size,
                                                            0,
                                                            i_params.params_buffer_info->queue_family_index,
                                                            params_data);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Reading from buffer resource failed (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }

        assert(params_data.size() == i_params.new_params_buffer_size);

        if (IsDrawCallIndexed(dc_params.type))
        {
            util::platform::MemoryCopy(
                i_params.draw_indexed_params, i_params.new_params_buffer_size, params_data.data(), params_data.size());
        }
        else if (dc_params.type == DrawCallTypes::kDrawIndexedIndirect)
        {
            util::platform::MemoryCopy(
                i_params.draw_params, i_params.new_params_buffer_size, params_data.data(), params_data.size());
        }
    }

    return VK_SUCCESS;
}

VkResult DrawCallsDumpingContext::DumpVertexIndexBuffers(uint64_t qs_index, uint64_t bcb_index, uint64_t dc_index)
{
    assert(original_command_buffer_info);
    assert(original_command_buffer_info->parent_id != format::kNullHandleId);
    const VulkanDeviceInfo* device_info = object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info);

    const VulkanPhysicalDeviceInfo* phys_dev_info = object_info_table.GetVkPhysicalDeviceInfo(device_info->parent_id);
    assert(phys_dev_info);

    graphics::VulkanResourcesUtil resource_util(device_info->handle,
                                                device_info->parent,
                                                *device_table,
                                                *instance_table,
                                                *phys_dev_info->replay_device_info->memory_properties);

    auto dc_params_entry = draw_call_params.find(dc_index);
    assert(dc_params_entry != draw_call_params.end());
    DrawCallParameters& dc_params = dc_params_entry->second;

    MinMaxVertexIndex min_max_vertex_indices = MinMaxVertexIndex{ 0, 0 };

    VulkanDumpResourceInfo res_info_base{};
    res_info_base.device_info                  = device_info;
    res_info_base.device_table                 = device_table;
    res_info_base.instance_table               = instance_table;
    res_info_base.object_info_table            = &object_info_table;
    res_info_base.original_command_buffer_info = original_command_buffer_info;
    res_info_base.cmd_index                    = dc_index;
    res_info_base.qs_index                     = qs_index;
    res_info_base.bcb_index                    = bcb_index;

    // Dump index buffer
    if (IsDrawCallIndexed(dc_params.type) && dc_params.referenced_index_buffer.buffer_info != nullptr)
    {
        min_max_vertex_indices.min = std::numeric_limits<uint32_t>::max();

        struct DrawIndexedParams
        {
            uint32_t index_count;
            uint32_t first_index;
            int32_t  vertex_offset;
        };

        // Store all indexCount, firstIndex and vertexOffset used by all draw calls (in case of indirect)
        // associated with this index buffer. Then we will parse the index buffer using all these pairs
        // in order to detect the greatest index which should help calculate the size of the vertex buffer
        // actually used by the draw calls.
        std::vector<DrawIndexedParams> indexed_params;
        uint32_t                       abs_index_count = 0;

        if (IsDrawCallIndirect(dc_params.type))
        {
            if (IsDrawCallIndirectCount(dc_params.type))
            {
                const DrawCallParameters::DrawCallParamsUnion::DrawIndirectCountParams& ic_params =
                    dc_params.dc_params_union.draw_indirect_count;

                if (ic_params.max_draw_count)
                {
                    assert(ic_params.draw_indexed_params != nullptr);
                    for (uint32_t d = 0; d < ic_params.max_draw_count; ++d)
                    {
                        const uint32_t indirect_index_count = ic_params.draw_indexed_params[d].indexCount;
                        const uint32_t indirect_first_index = ic_params.draw_indexed_params[d].firstIndex;

                        if (abs_index_count < indirect_index_count + indirect_first_index)
                        {
                            abs_index_count = indirect_index_count + indirect_first_index;
                        }

                        indexed_params.emplace_back(DrawIndexedParams{ indirect_index_count,
                                                                       indirect_first_index,
                                                                       ic_params.draw_indexed_params[d].vertexOffset });
                    }
                }
            }
            else
            {
                const DrawCallParameters::DrawCallParamsUnion::DrawIndirectParams& i_params =
                    dc_params.dc_params_union.draw_indirect;

                if (i_params.draw_count)
                {
                    assert(i_params.draw_indexed_params != nullptr);
                    for (uint32_t d = 0; d < i_params.draw_count; ++d)
                    {
                        const uint32_t indirect_index_count = i_params.draw_indexed_params[d].indexCount;
                        const uint32_t indirect_first_index = i_params.draw_indexed_params[d].firstIndex;

                        if (abs_index_count < indirect_index_count + indirect_first_index)
                        {
                            abs_index_count = indirect_index_count + indirect_first_index;
                        }

                        indexed_params.emplace_back(DrawIndexedParams{
                            indirect_index_count, indirect_first_index, i_params.draw_indexed_params[d].vertexOffset });
                    }
                }
            }
        }
        else
        {
            const uint32_t index_count = dc_params.dc_params_union.draw_indexed.indexCount;
            const uint32_t first_index = dc_params.dc_params_union.draw_indexed.firstIndex;
            abs_index_count            = index_count + first_index;

            indexed_params.emplace_back(
                DrawIndexedParams{ index_count, first_index, dc_params.dc_params_union.draw_indexed.vertexOffset });
        }

        if (abs_index_count)
        {
            const VkIndexType index_type = dc_params.referenced_index_buffer.index_type;
            const uint32_t    index_size = VkIndexTypeToBytes(index_type);
            const uint32_t    offset     = dc_params.referenced_index_buffer.offset;

            dc_params.index_buffer_dumped_at_offset = offset;

            // Check if the exact size has been provided by vkCmdBindIndexBuffer2
            uint32_t total_size = (dc_params.referenced_index_buffer.size != 0)
                                      ? (dc_params.referenced_index_buffer.size)
                                      : (abs_index_count * index_size);

            // There is something wrong with the calculations if this is true
            assert(total_size <= dc_params.referenced_index_buffer.buffer_info->size - offset);
            if (total_size > dc_params.referenced_index_buffer.buffer_info->size - offset)
            {
                total_size = dc_params.referenced_index_buffer.buffer_info->size - offset;
            }

            dc_params.referenced_index_buffer.actual_size = total_size;
            VulkanDumpResourceInfo res_info               = res_info_base;
            VkResult               res =
                resource_util.ReadFromBufferResource(dc_params.referenced_index_buffer.buffer_info->handle,
                                                     total_size,
                                                     offset,
                                                     dc_params.referenced_index_buffer.buffer_info->queue_family_index,
                                                     res_info.data);
            if (res != VK_SUCCESS)
            {
                GFXRECON_LOG_ERROR("Reading index buffer resource %" PRIu64 " failed (%s).",
                                   dc_params.referenced_index_buffer.buffer_info->capture_id,
                                   util::ToString<VkResult>(res).c_str())
                return res;
            }

            res_info.type       = DumpResourceType::kIndex;
            res_info.index_type = index_type;
            res                 = delegate_.DumpResource(res_info);
            if (res != VK_SUCCESS)
            {
                return res;
            }

            // Parse all indices in order to find the smallest and greatest index
            for (const auto& params : indexed_params)
            {
                MinMaxVertexIndex min_max_indices = FindMinMaxVertexIndices(
                    res_info.data, params.index_count, params.first_index, params.vertex_offset, index_type);
                if (min_max_indices.min < min_max_vertex_indices.min)
                {
                    min_max_vertex_indices.min = min_max_indices.min;
                }

                if (min_max_indices.max > min_max_vertex_indices.max)
                {
                    min_max_vertex_indices.max = min_max_indices.max;
                }
            }
        }
    }

    // Dump vertex buffer
    if (!dc_params.referenced_vertex_buffers.bound_vertex_buffer_per_binding.empty())
    {
        uint32_t vertex_count   = 0;
        uint32_t instance_count = 0;

        if (IsDrawCallIndexed(dc_params.type))
        {
            // For indexed draw calls the greatest vertex index will be used as the max vertex count
            GFXRECON_ASSERT(min_max_vertex_indices.max >= min_max_vertex_indices.min);
            vertex_count = (min_max_vertex_indices.max - min_max_vertex_indices.min) + 1;

            if (IsDrawCallIndirect(dc_params.type))
            {
                if (IsDrawCallIndirectCount(dc_params.type))
                {
                    const DrawCallParameters::DrawCallParamsUnion::DrawIndirectCountParams& ic_params =
                        dc_params.dc_params_union.draw_indirect_count;

                    if (ic_params.max_draw_count)
                    {
                        assert(ic_params.draw_indexed_params != nullptr);
                        assert(ic_params.draw_params == nullptr);
                        for (uint32_t d = 0; d < ic_params.max_draw_count; ++d)
                        {
                            if (instance_count < ic_params.draw_indexed_params[d].instanceCount)
                            {
                                instance_count = ic_params.draw_indexed_params[d].instanceCount;
                            }
                        }
                    }
                }
                else
                {
                    const DrawCallParameters::DrawCallParamsUnion::DrawIndirectParams& i_params =
                        dc_params.dc_params_union.draw_indirect;

                    if (i_params.draw_count)
                    {
                        assert(i_params.draw_indexed_params != nullptr);
                        assert(i_params.draw_params == nullptr);
                        for (uint32_t d = 0; d < i_params.draw_count; ++d)
                        {
                            if (instance_count < i_params.draw_indexed_params[d].instanceCount)
                            {
                                instance_count = i_params.draw_indexed_params[d].instanceCount;
                            }
                        }
                    }
                }
            }
            else
            {
                instance_count = dc_params.dc_params_union.draw_indexed.instanceCount;
            }
        }
        else
        {
            if (IsDrawCallIndirect(dc_params.type))
            {
                if (IsDrawCallIndirectCount(dc_params.type))
                {
                    const DrawCallParameters::DrawCallParamsUnion::DrawIndirectCountParams& ic_params =
                        dc_params.dc_params_union.draw_indirect_count;

                    if (ic_params.max_draw_count)
                    {
                        assert(ic_params.draw_params != nullptr);
                        assert(ic_params.draw_indexed_params == nullptr);
                        for (uint32_t d = 0; d < ic_params.max_draw_count; ++d)
                        {
                            if (vertex_count < ic_params.draw_params[d].vertexCount)
                            {
                                vertex_count = ic_params.draw_params[d].vertexCount;
                            }

                            if (instance_count < ic_params.draw_params[d].instanceCount)
                            {
                                instance_count = ic_params.draw_params[d].instanceCount;
                            }
                        }
                    }
                }
                else
                {
                    const DrawCallParameters::DrawCallParamsUnion::DrawIndirectParams& i_params =
                        dc_params.dc_params_union.draw_indirect;

                    if (i_params.draw_count)
                    {
                        assert(i_params.draw_params != nullptr);
                        assert(i_params.draw_indexed_params == nullptr);
                        for (uint32_t d = 0; d < i_params.draw_count; ++d)
                        {
                            if (vertex_count < i_params.draw_params[d].vertexCount)
                            {
                                vertex_count = i_params.draw_params[d].vertexCount;
                            }

                            if (instance_count < i_params.draw_params[d].instanceCount)
                            {
                                instance_count = i_params.draw_params[d].instanceCount;
                            }
                        }
                    }
                }
            }
            else
            {
                vertex_count   = dc_params.dc_params_union.draw.vertexCount;
                instance_count = dc_params.dc_params_union.draw.instanceCount;
            }
        }

        if (vertex_count)
        {
            for (auto& vis : dc_params.vertex_input_state.vertex_input_binding_map)
            {
                const uint32_t binding = vis.first;

                auto vb_entry = dc_params.referenced_vertex_buffers.bound_vertex_buffer_per_binding.find(binding);
                assert(vb_entry != dc_params.referenced_vertex_buffers.bound_vertex_buffer_per_binding.end());

                // For some reason there was no buffer bound for this binding
                if (vb_entry == dc_params.referenced_vertex_buffers.bound_vertex_buffer_per_binding.end())
                {
                    continue;
                }

                // Buffers can be NULL
                if (vb_entry->second.buffer_info == nullptr)
                {
                    continue;
                }

                const uint32_t count =
                    vis.second.inputRate == VK_VERTEX_INPUT_RATE_VERTEX ? vertex_count : instance_count;
                uint32_t total_size = 0;
                uint32_t binding_stride;

                if (vb_entry->second.size)
                {
                    // Exact size was provided by vkCmdBindVertexBuffers2
                    total_size     = vb_entry->second.size;
                    binding_stride = vb_entry->second.stride;
                }
                else
                {
                    binding_stride = vis.second.stride;
                    if (binding_stride)
                    {
                        total_size = count * binding_stride;
                    }
                    else
                    {
                        // According to the spec providing a VkVertexInputBindingDescription.stride equal to zero is
                        // valid. In these cases we will assume that information for only 1 vertex will be consumed
                        // (since we can't tell where the next one is located). So calculate the total size of all
                        // attributes that are using that binding and use that as the size of the vertex information
                        // for 1 vertex.
                        uint32_t min_offset = std::numeric_limits<uint32_t>::max();
                        for (const auto& ppl_attr : dc_params.vertex_input_state.vertex_input_attribute_map)
                        {
                            if (ppl_attr.second.binding != binding)
                            {
                                continue;
                            }

                            total_size += vkuFormatElementSize(ppl_attr.second.format);
                            if (min_offset > ppl_attr.second.offset)
                            {
                                min_offset = ppl_attr.second.offset;
                            }
                        }

                        if (!total_size)
                        {
                            continue;
                        }

                        total_size += min_offset;
                    }
                }

                // Calculate offset including vertexOffset
                const uint32_t offset = vb_entry->second.offset + (min_max_vertex_indices.min * binding_stride);

                assert(total_size <= vb_entry->second.buffer_info->size - offset);
                // There is something wrong with the calculations if this is true
                if (total_size > vb_entry->second.buffer_info->size - offset)
                {
                    total_size = vb_entry->second.buffer_info->size - offset;
                }

                dc_params.vertex_buffer_dumped_at_offset[binding] = offset;

                vb_entry->second.actual_size    = total_size;
                VulkanDumpResourceInfo res_info = res_info_base;

                VkResult res = resource_util.ReadFromBufferResource(vb_entry->second.buffer_info->handle,
                                                                    total_size,
                                                                    offset,
                                                                    vb_entry->second.buffer_info->queue_family_index,
                                                                    res_info.data);
                if (res != VK_SUCCESS)
                {
                    GFXRECON_LOG_ERROR("Reading from buffer resource failed (%s).",
                                       util::ToString<VkResult>(res).c_str())
                    return res;
                }

                res_info.type    = DumpResourceType::kVertex;
                res_info.binding = binding;
                res              = delegate_.DumpResource(res_info);
                if (res != VK_SUCCESS)
                {
                    GFXRECON_LOG_ERROR("Reading from buffer resource failed (%s).",
                                       util::ToString<VkResult>(res).c_str())
                    return res;
                }
            }
        }
    }

    return VK_SUCCESS;
}

VkResult DrawCallsDumpingContext::CloneCommandBuffer(VulkanCommandBufferInfo*           orig_cmd_buf_info,
                                                     const encode::VulkanDeviceTable*   dev_table,
                                                     const encode::VulkanInstanceTable* inst_table)
{
    assert(orig_cmd_buf_info);
    assert(dev_table);
    assert(inst_table);

    const VulkanCommandPoolInfo* cb_pool_info = object_info_table.GetVkCommandPoolInfo(orig_cmd_buf_info->pool_id);

    const VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          nullptr,
                                          cb_pool_info->handle,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          1 };

    const VulkanDeviceInfo* dev_info = object_info_table.GetVkDeviceInfo(orig_cmd_buf_info->parent_id);

    for (size_t i = 0; i < command_buffers.size(); ++i)
    {
        assert(command_buffers[i] == VK_NULL_HANDLE);
        VkResult res = dev_table->AllocateCommandBuffers(dev_info->handle, &ai, &command_buffers[i]);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("AllocateCommandBuffers failed with %s", util::ToString<VkResult>(res).c_str());
            return res;
        }

        const VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, 0, nullptr };
        dev_table->BeginCommandBuffer(command_buffers[i], &bi);
    }

    assert(original_command_buffer_info == nullptr);
    original_command_buffer_info = orig_cmd_buf_info;

    assert(device_table == nullptr);
    device_table = dev_table;
    assert(instance_table == nullptr);
    instance_table = inst_table;

    const VulkanDeviceInfo* device_info = object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info->parent_id != format::kNullHandleId);
    const VulkanPhysicalDeviceInfo* phys_dev_info = object_info_table.GetVkPhysicalDeviceInfo(device_info->parent_id);
    assert(phys_dev_info);

    assert(phys_dev_info->replay_device_info);
    assert(phys_dev_info->replay_device_info->memory_properties);
    replay_device_phys_mem_props = &phys_dev_info->replay_device_info->memory_properties.value();

    // Allocate auxiliary command buffer
    VkResult res = device_table->AllocateCommandBuffers(dev_info->handle, &ai, &aux_command_buffer);
    if (res != VK_SUCCESS)
    {
        GFXRECON_LOG_ERROR("AllocateCommandBuffers failed with %s", util::ToString<VkResult>(res).c_str());
        return res;
    }

    const VkFenceCreateInfo ci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0 };
    res                        = device_table->CreateFence(dev_info->handle, &ci, nullptr, &aux_fence);
    if (res != VK_SUCCESS)
    {
        GFXRECON_LOG_ERROR("CreateFence failed with %s", util::ToString<VkResult>(res).c_str());
        return res;
    }

    return VK_SUCCESS;
}

void DrawCallsDumpingContext::BindDescriptorSets(
    VkPipelineBindPoint                                pipeline_bind_point,
    uint32_t                                           first_set,
    const std::vector<const VulkanDescriptorSetInfo*>& descriptor_sets_infos,
    uint32_t                                           dynamicOffsetCount,
    const uint32_t*                                    pDynamicOffsets)
{
    if (pipeline_bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS)
    {
        return;
    }

    uint32_t dynamic_offset_index = 0;
    for (size_t i = 0; i < descriptor_sets_infos.size(); ++i)
    {
        uint32_t set_index = first_set + i;

        if (descriptor_sets_infos[i] != nullptr)
        {
            bound_descriptor_sets_gr[set_index] = *descriptor_sets_infos[i];

            if (dynamicOffsetCount && pDynamicOffsets != nullptr)
            {
                for (const auto& binding : descriptor_sets_infos[i]->descriptors)
                {
                    const uint32_t bindind_index = binding.first;

                    if (binding.second.desc_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                        binding.second.desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                    {
                        for (size_t ai = 0;
                             ai < bound_descriptor_sets_gr[set_index].descriptors[bindind_index].buffer_info.size();
                             ++ai)
                        {
                            bound_descriptor_sets_gr[set_index].descriptors[bindind_index].buffer_info[ai].offset +=
                                pDynamicOffsets[dynamic_offset_index];
                            ++dynamic_offset_index;
                        }
                    }
                }
            }
        }
    }

    assert((dynamic_offset_index == dynamicOffsetCount && pDynamicOffsets != nullptr) || (!dynamic_offset_index));
}

VkResult DrawCallsDumpingContext::CloneRenderPass(const VulkanRenderPassInfo*  original_render_pass,
                                                  const VulkanFramebufferInfo* fb_info)
{
    std::vector<VkAttachmentDescription> modified_attachemnts = original_render_pass->attachment_descs;

    // Fix storeOps and final layouts
    for (auto& att : modified_attachemnts)
    {
        att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        if (vkuFormatHasStencil(att.format))
        {
            att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
    }

    // Inform the original command buffer about the new image layouts
    for (const auto& att_ref : original_render_pass->subpass_refs[0].color_att_refs)
    {
        const VulkanImageViewInfo* att_img_view_info =
            object_info_table.GetVkImageViewInfo(fb_info->attachment_image_view_ids[att_ref.attachment]);
        assert(att_img_view_info != nullptr);

        original_command_buffer_info->image_layout_barriers[att_img_view_info->image_id] = att_ref.layout;

        VulkanImageInfo* img_info = object_info_table.GetVkImageInfo(att_img_view_info->image_id);
        assert(img_info != nullptr);
        img_info->intermediate_layout = att_ref.layout;
    }

    // Create new render passes
    render_pass_clones.emplace_back(std::vector<VkRenderPass>());
    std::vector<VkRenderPass>& new_render_pass = render_pass_clones.back();
    assert(original_render_pass->subpass_refs.size());
    new_render_pass.resize(original_render_pass->subpass_refs.size());

    // Do one quick pass over the subpass references in order to check if the render pass
    // uses color and/or depth attachments. This information might be necessary when
    // defining the dependencies of the custom render passes
    bool has_color = false, has_depth = false;
    for (size_t sub = 0; sub < original_render_pass->subpass_refs.size(); ++sub)
    {
        if (original_render_pass->subpass_refs[sub].color_att_refs.size())
        {
            has_color = true;
        }

        if (original_render_pass->subpass_refs[sub].has_depth)
        {
            has_depth = true;
        }
    }

    // Create new render passes. For each subpass in the original render pass a new render pass will be created.
    // Each new render pass will progressively contain an additional subpass until all subpasses of the original
    // renderpass are exhausted.
    // For example for a render pass with 3 subpasses, 3 new render passes will be created and will contain the
    // following subpasses:
    // Renderpass 0: Will contain 1 subpass.
    // Renderpass 1: Will contain 2 subpass.
    // Renderpass 2: Will contain 3 subpass.
    // Each draw call that is marked for dumping will be "assigned" the appropriate render pass depending on which
    // subpasses it was called from in the original render pass
    std::vector<VkSubpassDescription> subpass_descs;
    for (size_t sub = 0; sub < original_render_pass->subpass_refs.size(); ++sub)
    {
        bool                             has_external_dependencies_post = false;
        bool                             has_external_dependencies_pre  = false;
        std::vector<VkSubpassDependency> modified_dependencies;
        for (size_t d = 0; d < original_render_pass->dependencies.size(); ++d)
        {
            const VkSubpassDependency& original_dep = original_render_pass->dependencies[d];

            if ((original_dep.srcSubpass > sub || original_dep.dstSubpass > sub) &&
                (original_dep.srcSubpass != VK_SUBPASS_EXTERNAL && original_dep.dstSubpass != VK_SUBPASS_EXTERNAL))
            {
                // Skip this dependency as out of scope
                continue;
            }

            auto new_dep = modified_dependencies.insert(modified_dependencies.end(), original_dep);
            if (new_dep->srcSubpass != VK_SUBPASS_EXTERNAL && new_dep->srcSubpass > sub)
            {
                new_dep->srcSubpass = sub;
            }
            else if (new_dep->dstSubpass != VK_SUBPASS_EXTERNAL && new_dep->dstSubpass > sub)
            {
                new_dep->dstSubpass = sub;
            }

            if (new_dep->srcSubpass == VK_SUBPASS_EXTERNAL)
            {
                // new_dep->srcStageMask |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                // new_dep->srcAccessMask |= VK_ACCESS_TRANSFER_READ_BIT;

                has_external_dependencies_pre = true;
            }

            if (new_dep->dstSubpass == VK_SUBPASS_EXTERNAL)
            {
                new_dep->dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
                new_dep->dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

                has_external_dependencies_post = true;
            }
        }

        // No post renderpass dependecy was detected
        if (!has_external_dependencies_post)
        {
            VkSubpassDependency post_dependency;
            post_dependency.srcSubpass    = sub;
            post_dependency.dstSubpass    = VK_SUBPASS_EXTERNAL;
            post_dependency.dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
            post_dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

            // Injecting one for color
            if (has_color)
            {
                post_dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                post_dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

                modified_dependencies.push_back(post_dependency);
            }

            // Injecting one for depth
            if (has_depth)
            {
                post_dependency.srcStageMask =
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                post_dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                modified_dependencies.push_back(post_dependency);
            }
        }

        const VulkanRenderPassInfo::SubpassReferences& original_subp_ref = original_render_pass->subpass_refs[sub];
        auto new_subp_desc = subpass_descs.insert(subpass_descs.end(), VkSubpassDescription());

        new_subp_desc->flags                = original_subp_ref.flags;
        new_subp_desc->pipelineBindPoint    = original_subp_ref.pipeline_bind_point;
        new_subp_desc->inputAttachmentCount = original_subp_ref.input_att_refs.size();
        new_subp_desc->pInputAttachments =
            original_subp_ref.input_att_refs.size() ? original_subp_ref.input_att_refs.data() : nullptr;
        new_subp_desc->colorAttachmentCount = original_subp_ref.color_att_refs.size();
        new_subp_desc->pColorAttachments =
            original_subp_ref.color_att_refs.size() ? original_subp_ref.color_att_refs.data() : nullptr;
        new_subp_desc->pResolveAttachments =
            original_subp_ref.resolve_att_refs.size() ? original_subp_ref.resolve_att_refs.data() : nullptr;
        new_subp_desc->pDepthStencilAttachment =
            original_subp_ref.has_depth ? &original_subp_ref.depth_att_ref : nullptr;
        new_subp_desc->preserveAttachmentCount = original_subp_ref.preserve_att_refs.size();
        new_subp_desc->pPreserveAttachments =
            original_subp_ref.preserve_att_refs.size() ? original_subp_ref.preserve_att_refs.data() : nullptr;

        VkRenderPassCreateInfo ci;
        ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.flags           = VkRenderPassCreateFlags(0);
        ci.attachmentCount = modified_attachemnts.size();
        ci.pAttachments    = modified_attachemnts.size() ? modified_attachemnts.data() : nullptr;

        assert(subpass_descs.size() == sub + 1);
        ci.subpassCount = sub + 1;
        ci.pSubpasses   = subpass_descs.data();

        ci.dependencyCount = modified_dependencies.size();
        ci.pDependencies   = modified_dependencies.size() ? modified_dependencies.data() : nullptr;

        VkRenderPassMultiviewCreateInfo renderPassMultiviewCI;
        if (original_render_pass->has_multiview)
        {
            renderPassMultiviewCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
            renderPassMultiviewCI.pNext = nullptr;

            renderPassMultiviewCI.subpassCount         = original_render_pass->multiview.view_masks.size();
            renderPassMultiviewCI.pViewMasks           = original_render_pass->multiview.view_masks.empty()
                                                             ? nullptr
                                                             : original_render_pass->multiview.view_masks.data();
            renderPassMultiviewCI.dependencyCount      = original_render_pass->multiview.view_offsets.size();
            renderPassMultiviewCI.pViewOffsets         = original_render_pass->multiview.view_offsets.empty()
                                                             ? nullptr
                                                             : original_render_pass->multiview.view_offsets.data();
            renderPassMultiviewCI.correlationMaskCount = original_render_pass->multiview.correlation_masks.size();
            renderPassMultiviewCI.pCorrelationMasks    = original_render_pass->multiview.correlation_masks.empty()
                                                             ? nullptr
                                                             : original_render_pass->multiview.correlation_masks.data();

            ci.pNext = &renderPassMultiviewCI;
        }
        else
        {
            ci.pNext = nullptr;
        }

        const VulkanDeviceInfo* device_info =
            object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
        VkDevice device = device_info->handle;

        assert(sub < new_render_pass.size());
        VkResult res = device_table->CreateRenderPass(device, &ci, nullptr, &new_render_pass[sub]);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("CreateRenderPass failed with %s", util::ToString<VkResult>(res).c_str());
            return res;
        }
    }

    return VK_SUCCESS;
}

VkResult DrawCallsDumpingContext::BeginRenderPass(const VulkanRenderPassInfo*  render_pass_info,
                                                  uint32_t                     clear_value_count,
                                                  const VkClearValue*          p_clear_values,
                                                  const VulkanFramebufferInfo* framebuffer_info,
                                                  const VkRect2D&              render_area,
                                                  VkSubpassContents            contents)
{
    assert(render_pass_info);
    assert(framebuffer_info);

    std::vector<VulkanImageInfo*> color_att_imgs;

    current_render_pass_type = kRenderPass;
    current_subpass          = 0;
    active_renderpass        = render_pass_info;
    active_framebuffer       = framebuffer_info;

    // Parse color attachments
    uint32_t i = 0;
    for (const auto& att_ref : active_renderpass->subpass_refs[current_subpass].color_att_refs)
    {
        const uint32_t att_idx = att_ref.attachment;

        const VulkanImageViewInfo* img_view_info =
            object_info_table.GetVkImageViewInfo(framebuffer_info->attachment_image_view_ids[att_idx]);
        assert(img_view_info);

        VulkanImageInfo* img_info = object_info_table.GetVkImageInfo(img_view_info->image_id);
        assert(img_info);

        color_att_imgs.push_back(img_info);
    }

    VulkanImageInfo* depth_img_info;

    if (active_renderpass->subpass_refs[current_subpass].has_depth)
    {
        const uint32_t depth_att_idx = active_renderpass->subpass_refs[current_subpass].depth_att_ref.attachment;
        const VulkanImageViewInfo* depth_img_view_info =
            object_info_table.GetVkImageViewInfo(framebuffer_info->attachment_image_view_ids[depth_att_idx]);
        assert(depth_img_view_info);

        depth_img_info = object_info_table.GetVkImageInfo(depth_img_view_info->image_id);
        assert(depth_img_info);
    }
    else
    {
        depth_img_info = nullptr;
    }

    SetRenderTargets(std::move(color_att_imgs), depth_img_info, true);

    SetRenderArea(render_area);

    VkResult res = CloneRenderPass(render_pass_info, framebuffer_info);
    if (res != VK_SUCCESS)
    {
        GFXRECON_LOG_ERROR("Failed cloning render pass (%s).", util::ToString<VkResult>(res).c_str())
        return res;
    }

    // Add vkCmdBeginRenderPass into the cloned command buffers using the modified render pass
    CommandBufferIterator first, last;
    GetDrawCallActiveCommandBuffers(first, last);

    VkRenderPassBeginInfo bi;
    bi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.pNext           = nullptr;
    bi.clearValueCount = clear_value_count;
    bi.pClearValues    = p_clear_values;
    bi.framebuffer     = framebuffer_info->handle;
    bi.renderArea      = render_area;

    size_t cmd_buf_idx = current_cb_index;
    for (CommandBufferIterator it = first; it < last; ++it, ++cmd_buf_idx)
    {
        const uint64_t dc_index = dc_indices[CmdBufToDCVectorIndex(cmd_buf_idx)];

        // GetRenderPassIndex will tell us which render pass each cloned command buffer should use depending on the
        // assigned draw call index
        const RenderPassSubpassPair RP_index = GetRenderPassIndex(dc_index);
        const uint64_t              rp       = RP_index.first;
        const uint64_t              sp       = RP_index.second;

        if (dc_index < RP_indices[rp][0])
        {
            continue;
        }

        if (dc_index > RP_indices[rp][RP_indices[rp].size() - 1] || rp > current_renderpass)
        {
            // Command buffers / Draw calls outside this specific render pass should get
            // assigned the original render pass
            bi.renderPass = render_pass_info->handle;
        }
        else
        {
            // Command buffers / Draw calls inside this render pass should get the newly created / modified
            // render pass
            assert(rp < render_pass_clones.size());
            assert(sp < render_pass_clones[rp].size());
            bi.renderPass = render_pass_clones[rp][sp];
        }

        device_table->CmdBeginRenderPass(*it, &bi, contents);
    }

    auto new_entry = dynamic_rendering_attachment_layouts.emplace(
        std::piecewise_construct, std::forward_as_tuple(current_renderpass), std::forward_as_tuple());
    assert(new_entry.second);
    new_entry.first->second.is_dynamic = false;

    return VK_SUCCESS;
}

void DrawCallsDumpingContext::NextSubpass(VkSubpassContents contents)
{
    assert(active_renderpass);
    assert(active_framebuffer);

    std::vector<VulkanImageInfo*>    color_att_imgs;
    std::vector<VkAttachmentStoreOp> color_att_storeOps;
    std::vector<VkImageLayout>       color_att_final_layouts;

    ++current_subpass;

    CommandBufferIterator first, last;
    GetDrawCallActiveCommandBuffers(first, last);
    size_t cmd_buf_idx = current_cb_index;
    for (CommandBufferIterator it = first; it < last; ++it, ++cmd_buf_idx)
    {
        const uint64_t              dc_index = dc_indices[CmdBufToDCVectorIndex(cmd_buf_idx)];
        const RenderPassSubpassPair RP_index = GetRenderPassIndex(dc_index);
        const uint64_t              rp       = RP_index.first;

        device_table->CmdNextSubpass(*it, contents);
    }

    assert(current_subpass < active_renderpass->subpass_refs.size());

    // Parse color attachments
    for (const auto& att_ref : active_renderpass->subpass_refs[current_subpass].color_att_refs)
    {
        const uint32_t att_idx = att_ref.attachment;
        assert(att_idx < active_framebuffer->attachment_image_view_ids.size());

        const VulkanImageViewInfo* img_view_info =
            object_info_table.GetVkImageViewInfo(active_framebuffer->attachment_image_view_ids[att_idx]);
        assert(img_view_info);

        VulkanImageInfo* img_info = object_info_table.GetVkImageInfo(img_view_info->image_id);
        assert(img_info);

        color_att_imgs.push_back(img_info);
        color_att_storeOps.push_back(active_renderpass->attachment_descs[att_idx].storeOp);
        color_att_final_layouts.push_back(active_renderpass->attachment_descs[att_idx].finalLayout);
    }

    VulkanImageInfo*    depth_img_info;
    VkAttachmentStoreOp depth_att_storeOp;
    VkImageLayout       depth_final_layout;

    if (active_renderpass->subpass_refs[current_subpass].has_depth)
    {
        const uint32_t depth_att_idx = active_renderpass->subpass_refs[current_subpass].depth_att_ref.attachment;
        assert(depth_att_idx < active_framebuffer->attachment_image_view_ids.size());

        const VulkanImageViewInfo* depth_img_view_info =
            object_info_table.GetVkImageViewInfo(active_framebuffer->attachment_image_view_ids[depth_att_idx]);
        assert(depth_img_view_info);

        depth_img_info = object_info_table.GetVkImageInfo(depth_img_view_info->image_id);
        assert(depth_img_info);
        depth_att_storeOp  = active_renderpass->attachment_descs[depth_att_idx].storeOp;
        depth_final_layout = active_renderpass->attachment_descs[depth_att_idx].finalLayout;
    }
    else
    {
        depth_img_info    = nullptr;
        depth_att_storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    SetRenderTargets(std::move(color_att_imgs), depth_img_info, false);

    // Inform the original command buffer about the new image layouts
    for (const auto& att_ref : active_renderpass->subpass_refs[current_subpass].color_att_refs)
    {
        const VulkanImageViewInfo* att_img_view_info =
            object_info_table.GetVkImageViewInfo(active_framebuffer->attachment_image_view_ids[att_ref.attachment]);
        assert(att_img_view_info != nullptr);

        original_command_buffer_info->image_layout_barriers[att_img_view_info->image_id] = att_ref.layout;

        VulkanImageInfo* img_info = object_info_table.GetVkImageInfo(att_img_view_info->image_id);
        assert(img_info != nullptr);
        img_info->intermediate_layout = att_ref.layout;
    }
}

void DrawCallsDumpingContext::BindPipeline(VkPipelineBindPoint pipeline_bind_point, const VulkanPipelineInfo* pipeline)
{
    if (pipeline_bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS)
    {
        return;
    }

    bound_gr_pipeline = pipeline;
}

void DrawCallsDumpingContext::EndRenderPass()
{
    assert(current_render_pass_type == kRenderPass);

    CommandBufferIterator first, last;
    GetDrawCallActiveCommandBuffers(first, last);
    size_t cmd_buf_idx = current_cb_index;
    for (CommandBufferIterator it = first; it < last; ++it, ++cmd_buf_idx)
    {
        const uint64_t              dc_index = dc_indices[CmdBufToDCVectorIndex(cmd_buf_idx)];
        const RenderPassSubpassPair RP_index = GetRenderPassIndex(dc_index);
        const uint64_t              rp       = RP_index.first;
        const uint64_t              sp       = RP_index.second;

        if (dc_index < RP_indices[rp][0])
        {
            continue;
        }

        device_table->CmdEndRenderPass(*it);
    }

    ++current_renderpass;

    current_render_pass_type = kNone;
}

void DrawCallsDumpingContext::EndRendering()
{
    assert(current_render_pass_type == kDynamicRendering);

    CommandBufferIterator first, last;
    GetDrawCallActiveCommandBuffers(first, last);
    size_t cmd_buf_idx = current_cb_index;
    for (CommandBufferIterator it = first; it < last; ++it, ++cmd_buf_idx)
    {
        device_table->CmdEndRendering(*it);
    }

    ++current_renderpass;

    current_render_pass_type = kNone;
}

void DrawCallsDumpingContext::BindVertexBuffers(uint64_t                                    index,
                                                uint32_t                                    firstBinding,
                                                const std::vector<const VulkanBufferInfo*>& buffer_infos,
                                                const VkDeviceSize*                         pOffsets)
{
    if (!buffer_infos.size())
    {
        return;
    }

    for (size_t i = 0; i < buffer_infos.size(); ++i)
    {
        const uint32_t binding = static_cast<uint32_t>(firstBinding + i);
        bound_vertex_buffers.bound_vertex_buffer_per_binding[binding].buffer_info = buffer_infos[i];
        bound_vertex_buffers.bound_vertex_buffer_per_binding[binding].offset      = pOffsets[i];
        bound_vertex_buffers.bound_vertex_buffer_per_binding[binding].size        = 0;
        bound_vertex_buffers.bound_vertex_buffer_per_binding[binding].stride      = 0;
    }
}

void DrawCallsDumpingContext::BindVertexBuffers2(uint64_t                                    index,
                                                 uint32_t                                    first_binding,
                                                 const std::vector<const VulkanBufferInfo*>& buffer_infos,
                                                 const VkDeviceSize*                         pOffsets,
                                                 const VkDeviceSize*                         pSizes,
                                                 const VkDeviceSize*                         pStrides)
{
    if (!buffer_infos.size())
    {
        return;
    }

    for (size_t i = 0; i < buffer_infos.size(); ++i)
    {
        VkDeviceSize buffer_size = 0;
        if (pSizes != nullptr && buffer_infos[i] != nullptr)
        {
            if (pSizes[i] == VK_WHOLE_SIZE)
            {
                assert(buffer_infos[i]->size > pOffsets[i]);
                buffer_size = buffer_infos[i]->size - pOffsets[i];
            }
            else
            {
                buffer_size = pSizes[i];
            }
        }

        const uint32_t binding = static_cast<uint32_t>(first_binding + i);
        bound_vertex_buffers.bound_vertex_buffer_per_binding[binding].buffer_info = buffer_infos[i];
        bound_vertex_buffers.bound_vertex_buffer_per_binding[binding].offset      = pOffsets[i];
        bound_vertex_buffers.bound_vertex_buffer_per_binding[binding].stride      = pStrides[i];
        if (pSizes != nullptr)
        {
            bound_vertex_buffers.bound_vertex_buffer_per_binding[binding].size = buffer_size;
        }
    }
}

void DrawCallsDumpingContext::SetVertexInput(uint32_t                                   vertexBindingDescriptionCount,
                                             const VkVertexInputBindingDescription2EXT* pVertexBindingDescriptions,
                                             uint32_t                                   vertexAttributeDescriptionCount,
                                             const VkVertexInputAttributeDescription2EXT* pVertexAttributeDescriptions)
{
    for (uint32_t i = 0; i < vertexBindingDescriptionCount; ++i)
    {
        dynamic_vertex_input_state.vertex_input_binding_map[pVertexBindingDescriptions[i].binding].inputRate =
            pVertexBindingDescriptions[i].inputRate;
        dynamic_vertex_input_state.vertex_input_binding_map[pVertexBindingDescriptions[i].binding].stride =
            pVertexBindingDescriptions[i].stride;
    }

    for (uint32_t i = 0; i < vertexAttributeDescriptionCount; ++i)
    {
        dynamic_vertex_input_state.vertex_input_attribute_map[pVertexAttributeDescriptions[i].location].binding =
            pVertexAttributeDescriptions[i].binding;
        dynamic_vertex_input_state.vertex_input_attribute_map[pVertexAttributeDescriptions[i].location].format =
            pVertexAttributeDescriptions[i].format;
        dynamic_vertex_input_state.vertex_input_attribute_map[pVertexAttributeDescriptions[i].location].offset =
            pVertexAttributeDescriptions[i].offset;
    }
}

void DrawCallsDumpingContext::BindIndexBuffer(
    uint64_t index, const VulkanBufferInfo* buffer_info, VkDeviceSize offset, VkIndexType index_type, VkDeviceSize size)
{
    VkDeviceSize index_buffer_size = 0;
    if (size)
    {
        if (size == VK_WHOLE_SIZE && buffer_info != nullptr)
        {
            assert(buffer_info->size > offset);
            index_buffer_size = buffer_info->size - offset;
        }
        else
        {
            index_buffer_size = size;
        }
    }

    bound_index_buffer.buffer_info = buffer_info;
    bound_index_buffer.offset      = offset;
    bound_index_buffer.index_type  = index_type;
    bound_index_buffer.size        = index_buffer_size;
}

void DrawCallsDumpingContext::SetRenderTargets(const std::vector<VulkanImageInfo*>& color_att_imgs,
                                               VulkanImageInfo*                     depth_att_img,
                                               bool                                 new_render_pass)
{
    if (new_render_pass)
    {
        render_targets.emplace_back(std::vector<RenderTargets>());
    }

    auto new_render_targets = render_targets.end() - 1;

    new_render_targets->emplace_back(RenderTargets());
    auto new_rts = new_render_targets->end() - 1;

    new_rts->color_att_imgs = color_att_imgs;
    new_rts->depth_att_img  = depth_att_img;
}

void DrawCallsDumpingContext::SetRenderArea(const VkRect2D& new_render_area)
{
    render_area.push_back(new_render_area);
}

void DrawCallsDumpingContext::ResetFetchedIndirectParams()
{
    for (auto& dc_param_entry : draw_call_params)
    {
        DrawCallParameters& dc_params = dc_param_entry.second;
        if (IsDrawCallIndirect(dc_params.type))
        {
            if (IsDrawCallIndirectCount(dc_params.type))
            {
                DrawCallParameters::DrawCallParamsUnion::DrawIndirectCountParams& ic_params =
                    dc_params.dc_params_union.draw_indirect_count;

                if (ic_params.draw_params != nullptr)
                {
                    delete[] ic_params.draw_params;
                    ic_params.draw_params = nullptr;
                }

                if (ic_params.draw_indexed_params != nullptr)
                {
                    delete[] ic_params.draw_indexed_params;
                    ic_params.draw_indexed_params = nullptr;
                }

                ic_params.actual_draw_count = std::numeric_limits<uint32_t>::max();
            }
            else
            {
                DrawCallParameters::DrawCallParamsUnion::DrawIndirectParams& i_params =
                    dc_params.dc_params_union.draw_indirect;

                if (i_params.draw_params != nullptr)
                {
                    delete[] i_params.draw_params;
                    i_params.draw_params = nullptr;
                }

                if (i_params.draw_indexed_params != nullptr)
                {
                    delete[] i_params.draw_indexed_params;
                    i_params.draw_indexed_params = nullptr;
                }
            }
        }
    }
}

void DrawCallsDumpingContext::ReleaseIndirectParams()
{
    assert(original_command_buffer_info);
    assert(original_command_buffer_info->parent_id != format::kNullHandleId);
    const VulkanDeviceInfo* device_info = object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);

    if (device_info == nullptr)
    {
        return;
    }

    for (auto& dc_param_entry : draw_call_params)
    {
        DrawCallParameters& dc_params = dc_param_entry.second;
        if (IsDrawCallIndirect(dc_params.type))
        {
            if (IsDrawCallIndirectCount(dc_params.type))
            {
                DrawCallParameters::DrawCallParamsUnion::DrawIndirectCountParams& ic_params =
                    dc_params.dc_params_union.draw_indirect_count;

                if (ic_params.draw_params != nullptr)
                {
                    delete[] ic_params.draw_params;
                    ic_params.draw_params = nullptr;
                }

                if (ic_params.draw_indexed_params != nullptr)
                {
                    delete[] ic_params.draw_indexed_params;
                    ic_params.draw_indexed_params = nullptr;
                }

                if (ic_params.new_params_buffer != VK_NULL_HANDLE)
                {
                    device_table->DestroyBuffer(device_info->handle, ic_params.new_params_buffer, nullptr);
                    ic_params.new_params_buffer = VK_NULL_HANDLE;
                }

                if (ic_params.new_params_memory != VK_NULL_HANDLE)
                {
                    device_table->FreeMemory(device_info->handle, ic_params.new_params_memory, nullptr);
                    ic_params.new_params_memory = VK_NULL_HANDLE;
                }

                if (ic_params.new_count_buffer != VK_NULL_HANDLE)
                {
                    device_table->DestroyBuffer(device_info->handle, ic_params.new_count_buffer, nullptr);
                    ic_params.new_count_buffer = VK_NULL_HANDLE;
                }

                if (ic_params.new_count_memory != VK_NULL_HANDLE)
                {
                    device_table->FreeMemory(device_info->handle, ic_params.new_count_memory, nullptr);
                    ic_params.new_count_memory = VK_NULL_HANDLE;
                }
            }
            else
            {
                DrawCallParameters::DrawCallParamsUnion::DrawIndirectParams& i_params =
                    dc_params.dc_params_union.draw_indirect;

                if (i_params.draw_params != nullptr)
                {
                    delete[] i_params.draw_params;
                    i_params.draw_params = nullptr;
                }

                if (i_params.draw_indexed_params != nullptr)
                {
                    delete[] i_params.draw_indexed_params;
                    i_params.draw_indexed_params = nullptr;
                }

                if (i_params.new_params_buffer != VK_NULL_HANDLE)
                {
                    device_table->DestroyBuffer(device_info->handle, i_params.new_params_buffer, nullptr);
                    i_params.new_params_buffer = VK_NULL_HANDLE;
                }

                if (i_params.new_params_memory != VK_NULL_HANDLE)
                {
                    device_table->FreeMemory(device_info->handle, i_params.new_params_memory, nullptr);
                    i_params.new_params_memory = VK_NULL_HANDLE;
                }
            }
        }
    }
}

void DrawCallsDumpingContext::DestroyMutableResourceBackups()
{
    assert(original_command_buffer_info);

    const VulkanDeviceInfo* device_info = object_info_table.GetVkDeviceInfo(original_command_buffer_info->parent_id);
    if (device_info == nullptr)
    {
        return;
    }

    VkDevice device = device_info->handle;

    for (size_t i = 0; i < mutable_resource_backups.images.size(); ++i)
    {
        device_table->DestroyImage(device, mutable_resource_backups.images[i], nullptr);
    }

    for (size_t i = 0; i < mutable_resource_backups.image_memories.size(); ++i)
    {
        device_table->FreeMemory(device, mutable_resource_backups.image_memories[i], nullptr);
    }

    mutable_resource_backups.images.clear();
    mutable_resource_backups.image_memories.clear();
    mutable_resource_backups.original_images.clear();

    for (size_t i = 0; i < mutable_resource_backups.buffers.size(); ++i)
    {
        device_table->DestroyBuffer(device, mutable_resource_backups.buffers[i], nullptr);
    }

    for (size_t i = 0; i < mutable_resource_backups.buffer_memories.size(); ++i)
    {
        device_table->FreeMemory(device, mutable_resource_backups.buffer_memories[i], nullptr);
    }

    mutable_resource_backups.buffers.clear();
    mutable_resource_backups.buffer_memories.clear();
    mutable_resource_backups.original_buffers.clear();
}

DrawCallsDumpingContext::RenderPassSubpassPair DrawCallsDumpingContext::GetRenderPassIndex(uint64_t dc_index) const
{
    assert(RP_indices.size());

    for (size_t rp = 0; rp < RP_indices.size(); ++rp)
    {
        const std::vector<uint64_t>& render_pass = RP_indices[rp];
        assert(render_pass.size());

        if (dc_index > render_pass[render_pass.size() - 1])
        {
            continue;
        }

        for (uint64_t sp = 0; sp < render_pass.size() - 1; ++sp)
        {
            if (dc_index > render_pass[sp] && dc_index < render_pass[sp + 1])
            {
                return RenderPassSubpassPair(rp, sp);
            }
        }
    }

    // If this is hit then probably there's something wrong with the draw call and/or render pass indices
    GFXRECON_LOG_ERROR(
        "It appears that there is an error with the provided Draw indices in combination with the render pass indices.")
    assert(0);

    return RenderPassSubpassPair(0, 0);
}

size_t DrawCallsDumpingContext::CmdBufToDCVectorIndex(size_t cmd_buf_index) const
{
    assert(cmd_buf_index < command_buffers.size());

    if (dump_resources_before)
    {
        assert(cmd_buf_index / 2 < dc_indices.size());

        return cmd_buf_index / 2;
    }
    else
    {
        assert(cmd_buf_index < dc_indices.size());

        return cmd_buf_index;
    }
}

uint32_t DrawCallsDumpingContext::GetDrawCallActiveCommandBuffers(CommandBufferIterator& first,
                                                                  CommandBufferIterator& last) const
{
    assert(current_cb_index <= command_buffers.size());

    first = command_buffers.begin() + current_cb_index;
    last  = command_buffers.end();

    return current_cb_index;
}

void DrawCallsDumpingContext::BeginRendering(const std::vector<VulkanImageInfo*>& color_attachments,
                                             const std::vector<VkImageLayout>&    color_attachment_layouts,
                                             VulkanImageInfo*                     depth_attachment,
                                             VkImageLayout                        depth_attachment_layout,
                                             const VkRect2D&                      render_area)
{
    assert(color_attachments.size() == color_attachment_layouts.size());
    assert(current_render_pass_type == kNone);

    current_render_pass_type = kDynamicRendering;

    for (size_t i = 0; i < color_attachments.size(); ++i)
    {
        color_attachments[i]->intermediate_layout = color_attachment_layouts[i];
    }

    if (depth_attachment != nullptr)
    {
        depth_attachment->intermediate_layout = depth_attachment_layout;
    }

    SetRenderTargets(color_attachments, depth_attachment, true);
    SetRenderArea(render_area);

    auto new_entry = dynamic_rendering_attachment_layouts.emplace(
        std::piecewise_construct, std::forward_as_tuple(current_renderpass), std::forward_as_tuple());
    assert(new_entry.second);

    new_entry.first->second.is_dynamic               = true;
    new_entry.first->second.color_attachment_layouts = color_attachment_layouts;
    new_entry.first->second.depth_attachment_layout  = depth_attachment_layout;
}

GFXRECON_END_NAMESPACE(gfxrecon)
GFXRECON_END_NAMESPACE(decode)
