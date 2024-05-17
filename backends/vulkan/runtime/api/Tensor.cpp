/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <executorch/backends/vulkan/runtime/api/Tensor.h>
#include <executorch/backends/vulkan/runtime/api/Utils.h>

namespace vkcompute {

namespace {

/*
 * When stored on the GPU, one dimension will be aligned to the next multiple of
 * 4 in order to take advantage of vec4 data types. The dimension that is
 * packed is denoted by the GPUMemoryLayout. This function adjusts one of
 * the dimensions based on the desired memory format and storage type and
 * returns a sizes array describing the dimensions of the memory used to store
 * the tensor data on the GPU.
 */
std::vector<int64_t> calc_gpu_sizes(
    const std::vector<int64_t>& sizes,
    const api::GPUMemoryLayout memory_layout,
    const api::StorageType storage_type) {
  std::vector<int64_t> gpu_sizes;
  if (storage_type == api::kBuffer) {
    gpu_sizes.resize(sizes.size());
    for (size_t i = 0; i < sizes.size(); i++) {
      gpu_sizes.at(i) = sizes.at(i);
    }
  }
  // For texture storage, tensors are typically stored using 3D image textures.
  // Batches are stacked along the depth dimension. To represent the physical
  // 3 dimensionality of the image texture (with concatenated batches) GPU sizes
  // will be fixed to 4 dimensions when using texture storage.
  else {
    VK_CHECK_COND(
        sizes.size() >= 0 && sizes.size() <= 4,
        "Texture storage only valid for 0 <= ndim <= 4, received: ",
        sizes.size());

    gpu_sizes.resize(4);
    gpu_sizes.at(0) = api::utils::val_at(-4, sizes);
    gpu_sizes.at(1) = api::utils::val_at(-3, sizes);
    gpu_sizes.at(2) = api::utils::val_at(-2, sizes);
    gpu_sizes.at(3) = api::utils::val_at(-1, sizes);
  }

  size_t ndim = gpu_sizes.size();
  switch (memory_layout) {
    case api::kWidthPacked:
      if (ndim >= 1) {
        gpu_sizes.at(ndim - 1) =
            api::utils::align_up(api::utils::val_at(-1, sizes), INT64_C(4));
      }
      break;

    case api::kHeightPacked:
      if (ndim >= 2) {
        gpu_sizes.at(ndim - 2) =
            api::utils::align_up(api::utils::val_at(-2, sizes), INT64_C(4));
      }
      break;

    case api::kChannelsPacked:
      if (ndim >= 3) {
        gpu_sizes.at(ndim - 3) =
            api::utils::align_up(api::utils::val_at(-3, sizes), INT64_C(4));
      }
      break;
  }

  return gpu_sizes;
}

/*
 * Creates a uvec3 denoting the extents of the image texture that will be
 * created to store a tensor of a given size.
 */
api::utils::uvec3 create_image_extents(
    const std::vector<int64_t>& gpu_sizes,
    const api::StorageType storage_type,
    const api::GPUMemoryLayout memory_layout) {
  size_t ndim = gpu_sizes.size();

  if (storage_type == api::kBuffer) {
    // image extents do not apply to buffer storage
    return {0u, 0u, 0u};
  } else {
    VK_CHECK_COND(
        ndim >= 1 && ndim <= 4,
        "Texture storage only valid for 1 <= ndim <= 4!");

    using namespace api::utils;
    uint32_t width = safe_downcast<uint32_t>(val_at(-1, gpu_sizes));
    uint32_t height = safe_downcast<uint32_t>(val_at(-2, gpu_sizes));
    uint32_t channels = safe_downcast<uint32_t>(val_at(-3, gpu_sizes));
    uint32_t batch = safe_downcast<uint32_t>(val_at(-4, gpu_sizes));

    switch (memory_layout) {
      case api::kWidthPacked:
        VK_CHECK_COND(width % 4 == 0, "Width must be divisible by 4!");
        width /= 4;
        break;
      case api::kHeightPacked:
        VK_CHECK_COND(height % 4 == 0, "Height must be divisible by 4!");
        height /= 4;
        break;
      case api::kChannelsPacked:
        VK_CHECK_COND(channels % 4 == 0, "Channels must be divisible by 4!");
        channels /= 4;
        break;
      default:
        VK_THROW("Invalid memory format used!");
    }

    return {width, height, batch * channels};
  }
}

} // namespace

//
// vTensor
//

vTensor::vTensor(
    api::Context* const context,
    const std::vector<int64_t>& sizes,
    const api::ScalarType dtype,
    const api::StorageType storage_type,
    const api::GPUMemoryLayout memory_layout,
    const bool allocate_memory)
    : dtype_(dtype),
      memory_layout_(memory_layout),
      // Calculate sizes and strides
      sizes_(sizes.begin(), sizes.end()),
      gpu_sizes_{calc_gpu_sizes(sizes, memory_layout_, storage_type)},
      texture_limits_{{0, 0, 0}},
      // Utility Uniform Buffers that can be passed to shaders as arguments
      sizes_uniform_(),
      texture_limits_uniform_(),
      packed_dim_meta_(),
      // Construct Tensor storage
      storage_(
          context,
          storage_type,
          memory_layout_,
          gpu_sizes_,
          dtype_,
          allocate_memory) {
  if (storage_type != api::kBuffer) {
    texture_limits_.limits = api::utils::ivec3{
        api::utils::safe_downcast<int32_t>(storage_.extents_.data[0]),
        api::utils::safe_downcast<int32_t>(storage_.extents_.data[1]),
        api::utils::safe_downcast<int32_t>(storage_.extents_.data[2])};
  }

  if (dtype == api::kHalf) {
    VK_CHECK_COND(
        api::context()->adapter_ptr()->has_16bit_storage(),
        "Half dtype is only available if the physical device supports float16 "
        "storage buffers!");
  }
}

api::VulkanImage& vTensor::image(
    api::PipelineBarrier& pipeline_barrier,
    const api::PipelineStageFlags stage) & {
  storage_.transition(pipeline_barrier, stage, api::MemoryAccessType::READ);
  return storage_.image_;
}

api::VulkanImage& vTensor::image(
    api::PipelineBarrier& pipeline_barrier,
    const api::PipelineStageFlags stage,
    const api::MemoryAccessFlags access) & {
  storage_.transition(pipeline_barrier, stage, access);
  return storage_.image_;
}

api::VulkanBuffer& vTensor::buffer(
    api::PipelineBarrier& pipeline_barrier,
    const api::PipelineStageFlags stage) & {
  storage_.transition(pipeline_barrier, stage, api::MemoryAccessType::READ);
  return storage_.buffer_;
}

api::VulkanBuffer& vTensor::buffer(
    api::PipelineBarrier& pipeline_barrier,
    const api::PipelineStageFlags stage,
    const api::MemoryAccessFlags access) & {
  storage_.transition(pipeline_barrier, stage, access);
  return storage_.buffer_;
}

const api::BufferBindInfo vTensor::sizes_ubo() {
  if (!sizes_uniform_.buffer()) {
    sizes_uniform_ = api::UniformParamsBuffer(
        storage_.context_, api::utils::make_whcn_ivec4(sizes_));
  }
  return api::BufferBindInfo(sizes_uniform_.buffer());
}

const api::BufferBindInfo vTensor::texture_limits_ubo() {
  if (!texture_limits_uniform_.buffer()) {
    texture_limits_uniform_ =
        api::UniformParamsBuffer(storage_.context_, texture_limits_);
  }
  return api::BufferBindInfo(texture_limits_uniform_.buffer());
}

vTensor::PackedDimMeta vTensor::make_packed_dim_metadata() const {
  int64_t packed_dim = gpu_memory_layout_int();
  int32_t dim_size = api::utils::val_at(-(packed_dim + 1), sizes_);
  int32_t dim_size_padded = api::utils::val_at(-(packed_dim + 1), gpu_sizes_);
  int32_t dim_texel_len =
      api::utils::safe_downcast<int32_t>(extents().data[packed_dim]);
  int32_t padding = dim_size_padded - dim_size;

  return {
      dim_size,
      dim_size_padded,
      dim_texel_len,
      padding,
  };
}

const api::BufferBindInfo vTensor::packed_dim_meta_ubo() {
  if (!packed_dim_meta_.buffer()) {
    packed_dim_meta_ =
        api::UniformParamsBuffer(storage_.context_, make_packed_dim_metadata());
  }
  return api::BufferBindInfo(packed_dim_meta_.buffer());
}

VmaAllocationCreateInfo vTensor::get_allocation_create_info() const {
  switch (storage_type()) {
    case api::kBuffer:
      return storage_.buffer_.allocation_create_info();
    case api::kTexture2D:
    case api::kTexture3D:
      return storage_.image_.allocation_create_info();
  }
  return {};
}

VkMemoryRequirements vTensor::get_memory_requirements() const {
  switch (storage_type()) {
    case api::kBuffer:
      return storage_.buffer_.get_memory_requirements();
    case api::kTexture2D:
    case api::kTexture3D:
      return storage_.image_.get_memory_requirements();
  }
  return {};
}

void vTensor::bind_allocation(const api::Allocation& allocation) {
  switch (storage_type()) {
    case api::kBuffer:
      storage_.buffer_.bind_allocation(allocation);
      break;
    case api::kTexture2D:
    case api::kTexture3D:
      storage_.image_.bind_allocation(allocation);
      break;
  }
}

void vTensor::update_size_metadata(const std::vector<int64_t>& new_sizes) {
  sizes_ = new_sizes;
  gpu_sizes_ = calc_gpu_sizes(sizes_, memory_layout_, storage_type());

  if (storage_type() != api::kBuffer) {
    // Calculate the extents of the image texture that would have been required
    // for a tensor of the new sizes.
    api::utils::uvec3 virtual_extents =
        create_image_extents(gpu_sizes_, storage_type(), memory_layout_);
    // Update the texture limits to reflect the new virtual extents.
    texture_limits_.limits = api::utils::ivec3{
        api::utils::safe_downcast<int32_t>(virtual_extents.data[0]),
        api::utils::safe_downcast<int32_t>(virtual_extents.data[1]),
        api::utils::safe_downcast<int32_t>(virtual_extents.data[2])};
  }

  if (sizes_uniform_.buffer()) {
    sizes_uniform_.update(api::utils::make_whcn_ivec4(sizes_));
  }
  if (texture_limits_uniform_.buffer()) {
    texture_limits_uniform_.update(texture_limits_);
  }
  if (packed_dim_meta_.buffer()) {
    packed_dim_meta_.update(make_packed_dim_metadata());
  }
}

void vTensor::reallocate(const std::vector<int64_t>& new_sizes) {
  update_size_metadata(new_sizes);
  storage_.discard_and_reallocate(
      calc_gpu_sizes(new_sizes, memory_layout_, storage_type()),
      memory_layout_,
      dtype_);
}

void vTensor::virtual_resize(const std::vector<int64_t>& new_sizes) {
  // For texture storage check that the current texture is large enough for the
  // new sizes of the tensor.
  if (storage_type() != api::kBuffer) {
    api::utils::uvec3 virtual_extents =
        create_image_extents(gpu_sizes_, storage_type(), memory_layout_);

    bool valid_resize = virtual_extents.data[0] <= extents().data[0];
    valid_resize = valid_resize && virtual_extents.data[1] <= extents().data[1];
    valid_resize = valid_resize && virtual_extents.data[2] <= extents().data[2];

    VK_CHECK_COND(
        valid_resize,
        "Cannot use virtual resize if new sizes requires a larger texture.");
  }

  update_size_metadata(new_sizes);
}

//
// vTensorStorage
//

api::VulkanImage allocate_image(
    api::Context* const context_ptr,
    api::utils::uvec3& extents,
    const api::StorageType storage_type,
    const VkFormat image_format,
    const bool allocate_memory) {
  api::Adapter* adapter_ptr = context_ptr->adapter_ptr();

  api::ImageSampler::Properties sampler_props{
      VK_FILTER_NEAREST,
      VK_SAMPLER_MIPMAP_MODE_NEAREST,
      VK_SAMPLER_ADDRESS_MODE_REPEAT,
      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
  };

  VkImageType image_type = VK_IMAGE_TYPE_3D;
  VkImageViewType image_view_type;

  switch (storage_type) {
    case api::kTexture3D:
      image_type = VK_IMAGE_TYPE_3D;
      image_view_type = VK_IMAGE_VIEW_TYPE_3D;
      break;
    case api::kTexture2D:
      image_type = VK_IMAGE_TYPE_2D;
      image_view_type = VK_IMAGE_VIEW_TYPE_2D;
      break;
    default:
      // Return an empty VulkanImage by default
      return api::VulkanImage();
  }

  VkSampler sampler = adapter_ptr->sampler_cache().retrieve(sampler_props);

  return adapter_ptr->vma().create_image(
      api::create_extent3d(extents),
      image_format,
      image_type,
      image_view_type,
      sampler_props,
      sampler,
      /*allow_transfer = */ true,
      /*allocate_memory = */ allocate_memory);
}

api::VulkanBuffer allocate_buffer(
    api::Context* const context_ptr,
    const int64_t numel,
    const api::StorageType storage_type,
    const api::ScalarType dtype,
    const bool allocate_memory) {
  api::Adapter* adapter_ptr = context_ptr->adapter_ptr();

  switch (storage_type) {
    case api::kBuffer:
      break;
    default:
      // Return an empty VulkanBuffer if Buffer storage is not used
      return api::VulkanBuffer();
  }

  return adapter_ptr->vma().create_storage_buffer(
      api::element_size(dtype) * numel, /*gpu_only = */ true, allocate_memory);
}

vTensorStorage::vTensorStorage(
    api::Context* const context,
    const api::StorageType storage_type,
    const api::GPUMemoryLayout gpu_memory_layout,
    const std::vector<int64_t>& gpu_sizes,
    const api::ScalarType dtype,
    const bool allocate_memory)
    : context_(context),
      storage_type_{storage_type},
      extents_(
          create_image_extents(gpu_sizes, storage_type, gpu_memory_layout)),
      buffer_length_{api::utils::multiply_integers(gpu_sizes)},
      image_(allocate_image(
          context_,
          extents_,
          storage_type_,
          api::to_vkformat(dtype),
          allocate_memory)),
      buffer_(allocate_buffer(
          context_,
          buffer_length_,
          storage_type_,
          dtype,
          allocate_memory)),
      last_access_{} {}

vTensorStorage::~vTensorStorage() {
  flush();
}

void vTensorStorage::flush() {
  if (image_) {
    context_->register_image_cleanup(image_);
  } else if (buffer_) {
    context_->register_buffer_cleanup(buffer_);
  }
  last_access_ = {};
}

void vTensorStorage::transition(
    api::PipelineBarrier& pipeline_barrier,
    const api::PipelineStageFlags cur_stage,
    const api::MemoryAccessFlags cur_access) {
  // Get last stage access
  api::PipelineStageFlags prev_stage = last_access_.stage;
  api::MemoryAccessFlags prev_access = last_access_.access;

  const bool prev_written = (prev_access & api::MemoryAccessType::WRITE) != 0;

  VkImageLayout cur_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool layout_changed = false;
  if (image_) {
    cur_layout = image_.layout();
    new_layout = api::vk_layout(cur_stage, cur_access);

    layout_changed = cur_layout != new_layout;
  }

  if (prev_written || layout_changed) {
    VkPipelineStageFlags src_stage = api::vk_stage(prev_stage);
    if (0u == src_stage) {
      src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    VkPipelineStageFlags dst_stage = api::vk_stage(cur_stage);
    if (0u == dst_stage) {
      dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }

    pipeline_barrier.stage.src |= src_stage;
    pipeline_barrier.stage.dst |= dst_stage;

    if (image_) {
      pipeline_barrier.images.emplace_back(
          api::vk_access(prev_stage, prev_access),
          api::vk_access(cur_stage, cur_access),
          cur_layout,
          new_layout,
          image_);

      image_.set_layout(new_layout);
    } else if (buffer_) {
      pipeline_barrier.buffers.emplace_back(
          api::vk_access(prev_stage, prev_access),
          api::vk_access(cur_stage, cur_access),
          buffer_);
    }
  }

  last_access_.stage = cur_stage;
  last_access_.access = cur_access;
}

void vTensorStorage::discard_and_reallocate(
    const std::vector<int64_t>& gpu_sizes,
    const api::GPUMemoryLayout gpu_memory_layout,
    const api::ScalarType dtype) {
  const bool image_owns_memory = image_.owns_memory();
  const bool buffer_owns_memory = buffer_.owns_memory();

  flush();

  extents_ = create_image_extents(gpu_sizes, storage_type_, gpu_memory_layout);
  image_ = allocate_image(
      context_,
      extents_,
      storage_type_,
      api::to_vkformat(dtype),
      image_owns_memory);

  buffer_length_ = api::utils::multiply_integers(gpu_sizes);
  buffer_ = allocate_buffer(
      context_, buffer_length_, storage_type_, dtype, buffer_owns_memory);
}

} // namespace vkcompute
