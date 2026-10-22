// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include <algorithm>
#include <cstdio>
#include <span>
#include "bbport_background.h"
#include "bbport_settings.h"
#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "video_core/host_shaders/frame_generation_copy_comp.h"
#include "video_core/host_shaders/frame_generation_hud_capture_comp.h"
#include "video_core/host_shaders/frame_generation_hud_composite_comp.h"
#include "video_core/host_shaders/frame_generation_hud_lut_comp.h"
#include "video_core/host_shaders/frame_generation_hud_resolve_comp.h"
#include <vk_mem_alloc.h>

namespace Vulkan {
namespace {
constexpr auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                       vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
bool CreateImage(vk::Device device, VmaAllocator allocator, FrameGeneration::Image& image,
                 vk::UniqueImageView& view, vk::Format format, u32 width, u32 height,
                 const std::vector<u32>& queue_families) {
    if (!image.Create(allocator,format,width,height,queue_families)) return false;
    auto result=device.createImageViewUnique({
        .image=vk::Image(image), .viewType=vk::ImageViewType::e2D, .format=format,
        .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});
    if (result.result!=vk::Result::eSuccess) return false;
    view=std::move(result.value);
    return true;
}
FfxVkFsr3_3_1_6FrameGenerationImage Describe(vk::Image image, vk::Format format, u32 w, u32 h) {
    return {VkImage(image), VkFormat(format), w, h, VK_IMAGE_LAYOUT_GENERAL, VkImageUsageFlags(usage)};
}
void WriteBarrier(vk::CommandBuffer cmdbuf, vk::Image image) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask=vk::AccessFlagBits2::eNone,
        .dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask=vk::AccessFlagBits2::eShaderWrite,
        .oldLayout=vk::ImageLayout::eUndefined, .newLayout=vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image, .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
}
}

FrameGeneration::Image::~Image() {
    if (image) vmaDestroyImage(allocator,VkImage(image),allocation);
}
bool FrameGeneration::Image::Create(VmaAllocator allocator_, vk::Format format, u32 w, u32 h,
                                    const std::vector<u32>& queue_families) {
    allocator=allocator_;
    const bool shared=queue_families.size()>1;
    const vk::ImageCreateInfo info{.imageType=vk::ImageType::e2D,.format=format,.extent={w,h,1},
        .mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,.usage=usage,
        .sharingMode=shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount=shared ? u32(queue_families.size()) : 0u,
        .pQueueFamilyIndices=shared ? queue_families.data() : nullptr};
    const VkImageCreateInfo raw=info;
    const VmaAllocationCreateInfo allocation_info{
        .flags=VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage handle{};
    if (vmaCreateImage(allocator,&raw,&allocation_info,&handle,&allocation,nullptr)!=VK_SUCCESS)
        return false;
    image=handle;
    return true;
}
FrameGeneration::FrameGeneration(vk::PhysicalDevice physical_, vk::Device device_,
                                 VmaAllocator allocator_, Submissions submissions_,
                                 std::vector<u32> queue_families_)
    : physical{physical_}, device{device_}, allocator{allocator_},
      submissions{std::move(submissions_)}, queue_families{std::move(queue_families_)} {}
FrameGeneration::~FrameGeneration() {
    Reset();
    if (hud_lut) vmaDestroyBuffer(allocator,VkBuffer(hud_lut),hud_lut_allocation);
}

bool FrameGeneration::EnsureCopyPipeline() {
    if (copy_pipeline) return true;
    std::array<vk::DescriptorSetLayoutBinding,6> bindings{};
    for (u32 i=0;i<6;++i) bindings[i]={
        .binding=i, .descriptorType=i<2 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eStorageImage,
        .descriptorCount=1, .stageFlags=vk::ShaderStageFlagBits::eCompute};
    auto descriptors=device.createDescriptorSetLayoutUnique({
        .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=u32(bindings.size()),.pBindings=bindings.data()});
    if (descriptors.result!=vk::Result::eSuccess) return false;
    copy_descriptors=std::move(descriptors.value);
    const vk::PushConstantRange range{.stageFlags=vk::ShaderStageFlagBits::eCompute,.offset=0,.size=20};
    auto layout=device.createPipelineLayoutUnique({
        .setLayoutCount=1,.pSetLayouts=&*copy_descriptors,.pushConstantRangeCount=1,.pPushConstantRanges=&range});
    if (layout.result!=vk::Result::eSuccess) return false;
    copy_layout=std::move(layout.value);
    auto shader=device.createShaderModuleUnique({.codeSize=sizeof(FRAME_GENERATION_COPY_COMP),
                                                 .pCode=FRAME_GENERATION_COPY_COMP});
    if (shader.result!=vk::Result::eSuccess) return false;
    auto pipeline=device.createComputePipelineUnique({},vk::ComputePipelineCreateInfo{
        .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*shader.value,.pName="main"},.layout=*copy_layout});
    if (pipeline.result!=vk::Result::eSuccess) return false;
    copy_pipeline=std::move(pipeline.value);
    return true;
}

void FrameGeneration::Capture(vk::CommandBuffer cmdbuf, const FrameGenerationSource& source,
                              vk::ImageView color_view, u32 w, u32 h, vk::Format format,
                              std::shared_ptr<Snapshot>& snapshot) {
    auto& settings=BbSettings::Get();
    const bool nvidia=settings.frame_generation==BbSettings::FrameGenerationNvidia;
    const bool sdr_unorm=format==vk::Format::eR8G8B8A8Unorm || format==vk::Format::eB8G8R8A8Unorm;
    const bool hdr=format==vk::Format::eA2B10G10R10UnormPack32 || format==vk::Format::eA2R10G10B10UnormPack32;
    // A UNORM swapchain already holds sRGB-encoded values: they are copied as they are.
    const bool srgb8=compact_color && !nvidia && !hdr;
    const vk::Format color_format=srgb8 ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR16G16B16A16Sfloat;
    if (settings.frame_generation == BbSettings::FrameGenerationOff ||
        settings.upscaler==BbSettings::UpscalerOff ||
        (settings.frame_generation==BbSettings::FrameGenerationFsr && failed.load(std::memory_order_relaxed)) || !source.valid ||
        VramPaused() ||
        !source.depth_view || !source.motion_view || !source.render_width || !source.render_height || !w || !h ||
        (!settings.window_focused && settings.background_mode!=BbSettings::BackgroundRun) ||
        (!hdr && format != vk::Format::eB8G8R8A8Srgb && format != vk::Format::eR8G8B8A8Srgb &&
                 format != vk::Format::eB8G8R8A8Unorm && format != vk::Format::eR8G8B8A8Unorm)) {
        snapshot.reset(); return;
    }
    if (!EnsureCopyPipeline()) {
        settings.frame_generation_problem="FG input pipeline could not be created. The real frame is kept.";
        failed=true;
        snapshot.reset(); return;
    }
    // A 4K scene can be displayed in a smaller window. Keep FI render dimensions
    // within the final color extent, scaling motion vectors in pixels with them.
    const float scale=std::min({1.0f,float(w)/source.render_width,float(h)/source.render_height});
    const u32 rw=std::max(1u,u32(source.render_width*scale));
    const u32 rh=std::max(1u,u32(source.render_height*scale));
    if (!snapshot || snapshot->width!=w || snapshot->height!=h || snapshot->hdr!=hdr || snapshot->sdr_unorm!=sdr_unorm || snapshot->nvidia_input_only!=nvidia ||
        snapshot->srgb8!=srgb8 ||
        snapshot->source.render_width!=rw || snapshot->source.render_height!=rh) {
        snapshot=std::make_shared<Snapshot>(); snapshot->width=w; snapshot->height=h; snapshot->hdr=hdr;
        snapshot->nvidia_input_only=nvidia;
        snapshot->sdr_unorm=sdr_unorm;
        snapshot->srgb8=srgb8;
        // NVIDIA consumes WSI color directly. Keep only a 1x1 descriptor dummy,
        // rather than allocating/copying three unused full-resolution FP16 images.
        if (!CreateImage(device,allocator,snapshot->color,snapshot->color_view,color_format,nvidia ? 1u : w,nvidia ? 1u : h,queue_families) ||
            (!nvidia && !CreateImage(device,allocator,snapshot->generated,snapshot->generated_view,color_format,w,h,queue_families)) ||
            !CreateImage(device,allocator,snapshot->depth,snapshot->depth_view,vk::Format::eR32Sfloat,rw,rh,queue_families) ||
            !CreateImage(device,allocator,snapshot->motion,snapshot->motion_view,vk::Format::eR16G16Sfloat,rw,rh,queue_families) ||
            ((hdr || (sdr_unorm && !srgb8)) && !nvidia && !CreateImage(device,allocator,snapshot->encoded,snapshot->encoded_view,vk::Format::eR16G16B16A16Sfloat,w,h,queue_families)) ||
            (srgb8 && !CreateImage(device,allocator,snapshot->hud_mask,snapshot->hud_mask_view,vk::Format::eR8Unorm,w,h,queue_families))) {
            settings.frame_generation_problem="FG could not allocate its input images. The real frame is kept.";
            failed=true;
            snapshot.reset(); return;
        }
    }
    snapshot->source=source;
    snapshot->source.render_width=rw; snapshot->source.render_height=rh;
    snapshot->source.jitter[0]*=float(rw)/source.render_width;
    snapshot->source.jitter[1]*=float(rh)/source.render_height;
    WriteBarrier(cmdbuf,vk::Image(snapshot->color));
    WriteBarrier(cmdbuf,vk::Image(snapshot->depth));
    WriteBarrier(cmdbuf,vk::Image(snapshot->motion));
    const vk::MemoryBarrier2 source_ready{
        .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask=vk::AccessFlagBits2::eShaderRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&source_ready});
    const std::array<vk::DescriptorImageInfo,6> images{{
        {.imageView=color_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=source.depth_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=source.motion_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*snapshot->color_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*snapshot->depth_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*snapshot->motion_view,.imageLayout=vk::ImageLayout::eGeneral}}};
    std::array<vk::WriteDescriptorSet,6> writes{};
    for (u32 i=0;i<6;++i) writes[i]={.dstBinding=i,.descriptorCount=1,
        .descriptorType=i<2 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eStorageImage,.pImageInfo=&images[i]};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,*copy_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*copy_layout,0,writes);
    const std::array<u32,5> render_size{source.render_width,source.render_height,rw,rh,
        nvidia ? 3u : hdr ? 1u : srgb8 ? (sdr_unorm ? 0u : 6u) : sdr_unorm ? 4u : 0u};
    cmdbuf.pushConstants(*copy_layout,vk::ShaderStageFlagBits::eCompute,0,20,render_size.data());
    cmdbuf.dispatch(((nvidia ? rw : w)+7)/8,((nvidia ? rh : h)+7)/8,1);
    const vk::MemoryBarrier2 complete{
        .srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,.srcAccessMask=vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,.dstAccessMask=vk::AccessFlagBits2::eMemoryRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&complete});
    // bbport: the game's HUD stays out of the interpolation (8-bit path: the generated frame is
    // in the presented frame's values, so the real frame's HUD pixels can go into it as they are).
    snapshot->hud=false;
    if (srgb8 && source.ui_view && source.hudless_view && source.ui_width && source.ui_height &&
        EnsureHudPipelines()) {
        CaptureHud(cmdbuf,*snapshot,color_view,
                   format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb);
    }
}

bool FrameGeneration::EnsureHudPipelines() {
    if (hud_failed) return false;
    if (hud_lut) return true;
    using T=vk::DescriptorType;
    struct Spec {
        std::vector<T> bindings;
        const u32* code;
        size_t size;
    };
    const std::array<Spec,4> specs{{
        {{T::eStorageBuffer,T::eSampledImage,T::eSampledImage,T::eSampledImage},
         FRAME_GENERATION_HUD_LUT_COMP,sizeof(FRAME_GENERATION_HUD_LUT_COMP)},
        {{T::eStorageBuffer},FRAME_GENERATION_HUD_RESOLVE_COMP,sizeof(FRAME_GENERATION_HUD_RESOLVE_COMP)},
        {{T::eStorageBuffer,T::eSampledImage,T::eSampledImage,T::eStorageImage,T::eStorageImage},
         FRAME_GENERATION_HUD_CAPTURE_COMP,sizeof(FRAME_GENERATION_HUD_CAPTURE_COMP)},
        {{T::eStorageBuffer,T::eSampledImage,T::eSampledImage,T::eStorageImage,T::eSampledImage,
          T::eStorageImage,T::eStorageImage,T::eStorageImage},
         FRAME_GENERATION_HUD_COMPOSITE_COMP,sizeof(FRAME_GENERATION_HUD_COMPOSITE_COMP)},
    }};
    const auto fail=[&] {
        std::printf("FSR Frame Generation: HUD passes unavailable; the HUD is interpolated\n");
        hud_failed=true;
        return false;
    };
    for (size_t i=0;i<specs.size();++i) {
        std::vector<vk::DescriptorSetLayoutBinding> bindings;
        for (u32 b=0;b<specs[i].bindings.size();++b)
            bindings.push_back({.binding=b,.descriptorType=specs[i].bindings[b],.descriptorCount=1,
                                .stageFlags=vk::ShaderStageFlagBits::eCompute});
        auto descriptors=device.createDescriptorSetLayoutUnique({
            .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount=u32(bindings.size()),.pBindings=bindings.data()});
        if (descriptors.result!=vk::Result::eSuccess) return fail();
        const vk::PushConstantRange range{.stageFlags=vk::ShaderStageFlagBits::eCompute,.offset=0,.size=20};
        auto layout=device.createPipelineLayoutUnique({
            .setLayoutCount=1,.pSetLayouts=&*descriptors.value,.pushConstantRangeCount=1,.pPushConstantRanges=&range});
        if (layout.result!=vk::Result::eSuccess) return fail();
        auto shader=device.createShaderModuleUnique({.codeSize=specs[i].size,.pCode=specs[i].code});
        if (shader.result!=vk::Result::eSuccess) return fail();
        auto pipeline=device.createComputePipelineUnique({},vk::ComputePipelineCreateInfo{
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*shader.value,.pName="main"},.layout=*layout.value});
        if (pipeline.result!=vk::Result::eSuccess) return fail();
        hud_passes[i]={std::move(descriptors.value),std::move(layout.value),std::move(pipeline.value)};
    }
    // sums, counts and the resolved table: 3 channels x 256 values each.
    const VkBufferCreateInfo info{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=768*4*3,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo allocation_info{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buffer{};
    if (vmaCreateBuffer(allocator,&info,&allocation_info,&buffer,&hud_lut_allocation,nullptr)!=VK_SUCCESS)
        return fail();
    hud_lut=buffer;
    return true;
}

void FrameGeneration::CaptureHud(vk::CommandBuffer cmdbuf, Snapshot& snapshot,
                                 vk::ImageView color_view, bool decodes) {
    const auto& source=snapshot.source;
    const u32 w=snapshot.width, h=snapshot.height;
    const std::array<u32,5> parameters{w,h,source.ui_width,source.ui_height,decodes ? 1u : 0u};
    const auto compute_barrier=[&](vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access) {
        const vk::MemoryBarrier2 barrier{.srcStageMask=src,.srcAccessMask=src_access,
            .dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask=vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&barrier});
    };
    const vk::DescriptorBufferInfo lut{.buffer=hud_lut,.offset=0,.range=VK_WHOLE_SIZE};
    const auto run=[&](u32 pass, std::span<const vk::DescriptorImageInfo> images,
                       std::span<const vk::DescriptorType> types, u32 x, u32 y) {
        std::vector<vk::WriteDescriptorSet> writes{{.dstBinding=0,.descriptorCount=1,
            .descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&lut}};
        for (u32 i=0;i<images.size();++i)
            writes.push_back({.dstBinding=i+1,.descriptorCount=1,.descriptorType=types[i],.pImageInfo=&images[i]});
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,*hud_passes[pass].pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*hud_passes[pass].layout,0,writes);
        cmdbuf.pushConstants(*hud_passes[pass].layout,vk::ShaderStageFlagBits::eCompute,0,20,parameters.data());
        cmdbuf.dispatch(x,y,1);
    };
    using T=vk::DescriptorType;
    const auto general=vk::ImageLayout::eGeneral;
    // The previous frame's passes read the table: clear the bins after them.
    const vk::MemoryBarrier2 to_clear{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask=vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eClear,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&to_clear});
    cmdbuf.fillBuffer(hud_lut,0,768*4*2,0);
    compute_barrier(vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite);
    const std::array<vk::DescriptorImageInfo,3> lut_images{{
        {.imageView=source.ui_view,.imageLayout=general},{.imageView=source.hudless_view,.imageLayout=general},
        {.imageView=color_view,.imageLayout=general}}};
    const std::array lut_types{T::eSampledImage,T::eSampledImage,T::eSampledImage};
    run(0,lut_images,lut_types,(w+23)/24,(h+23)/24);
    compute_barrier(vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite);
    run(1,{},{},1,1);
    compute_barrier(vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite);
    WriteBarrier(cmdbuf,vk::Image(snapshot.hud_mask));
    const std::array<vk::DescriptorImageInfo,4> capture_images{{
        {.imageView=source.ui_view,.imageLayout=general},{.imageView=source.hudless_view,.imageLayout=general},
        {.imageView=*snapshot.color_view,.imageLayout=general},{.imageView=*snapshot.hud_mask_view,.imageLayout=general}}};
    const std::array capture_types{T::eSampledImage,T::eSampledImage,T::eStorageImage,T::eStorageImage};
    run(2,capture_images,capture_types,(w+7)/8,(h+7)/8);
    const vk::MemoryBarrier2 complete{
        .srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,.srcAccessMask=vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,.dstAccessMask=vk::AccessFlagBits2::eMemoryRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&complete});
    snapshot.hud=true;
    snapshot.frame_view=color_view;
    snapshot.frame_decodes=decodes;
}

void FrameGeneration::CompositeHud(vk::CommandBuffer cmdbuf, const Snapshot& snapshot) {
    const std::array<u32,5> parameters{snapshot.width,snapshot.height,0u,0u,snapshot.frame_decodes ? 1u : 0u};
    if (!hud_history || hud_history->width!=snapshot.width || hud_history->height!=snapshot.height) {
        // Earlier frames' passes are in the same queue, ahead of this one: wait for them.
        submissions.wait_submitted();
        hud_history.reset();
        auto history=std::make_unique<HudHistory>();
        const u32 w=snapshot.width, h=snapshot.height;
        if (!CreateImage(device,allocator,history->opacity,history->opacity_view,vk::Format::eR8Unorm,w,h,queue_families) ||
            !CreateImage(device,allocator,history->scene,history->scene_view,vk::Format::eR8G8B8A8Unorm,w,h,queue_families) ||
            !CreateImage(device,allocator,history->frame,history->frame_view,vk::Format::eR8G8B8A8Unorm,w,h,queue_families))
            return;
        history->width=w; history->height=h;
        hud_history=std::move(history);
        for (const vk::Image image : {vk::Image(hud_history->opacity),vk::Image(hud_history->scene),
                                      vk::Image(hud_history->frame)}) {
            const vk::ImageMemoryBarrier2 to_clear{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eNone,.dstStageMask=vk::PipelineStageFlagBits2::eClear,
                .dstAccessMask=vk::AccessFlagBits2::eTransferWrite,.oldLayout=vk::ImageLayout::eUndefined,
                .newLayout=vk::ImageLayout::eGeneral,.image=image,
                .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&to_clear});
            cmdbuf.clearColorImage(image,vk::ImageLayout::eGeneral,
                                   vk::ClearColorValue{std::array<float,4>{0,0,0,0}},
                                   vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor,0,1,0,1});
        }
        const vk::MemoryBarrier2 cleared{.srcStageMask=vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask=vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&cleared});
    }
    const vk::MemoryBarrier2 ready{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask=vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&ready});
    const vk::DescriptorBufferInfo lut{.buffer=hud_lut,.offset=0,.range=VK_WHOLE_SIZE};
    const std::array<vk::DescriptorImageInfo,7> images{{
        {.imageView=snapshot.frame_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*snapshot.hud_mask_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*snapshot.generated_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*snapshot.color_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*hud_history->opacity_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*hud_history->scene_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*hud_history->frame_view,.imageLayout=vk::ImageLayout::eGeneral}}};
    const std::array<vk::WriteDescriptorSet,8> writes{{
        {.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&lut},
        {.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&images[0]},
        {.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&images[1]},
        {.dstBinding=3,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&images[2]},
        {.dstBinding=4,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&images[3]},
        {.dstBinding=5,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&images[4]},
        {.dstBinding=6,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&images[5]},
        {.dstBinding=7,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&images[6]}}};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,*hud_passes[3].pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*hud_passes[3].layout,0,writes);
    cmdbuf.pushConstants(*hud_passes[3].layout,vk::ShaderStageFlagBits::eCompute,0,20,parameters.data());
    cmdbuf.dispatch((snapshot.width+7)/8,(snapshot.height+7)/8,1);
}

void FrameGeneration::RetireCompleted() {
    while (!pending.empty() && submissions.is_free(pending.front().tick)) {
        if (context) ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context,pending.front().id);
        pending.pop_front();
    }
}
void FrameGeneration::Invalidate() {
    history=false;
    if (BbSettings::Get().frame_generation!=BbSettings::FrameGenerationNvidia)
        BbSettings::Get().frame_generation_active=0;
}
void FrameGeneration::Reset() {
    if (context) {
        submissions.wait_submitted();
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context); context=nullptr;
    }
    // bbport: composites only run with a context, and its destruction waited for them.
    hud_history.reset();
    pending.clear(); recording={}; Invalidate();
}
bool FrameGeneration::Record(vk::CommandBuffer cmdbuf, const std::shared_ptr<Snapshot>& snapshot) {
    auto& settings=BbSettings::Get();
    RetireCompleted();
    if (settings.frame_generation==BbSettings::FrameGenerationFsr && VramPaused()) {
        // Releases the FI/OF context and the HUD history (Capture dropped the inputs).
        if (context) Reset();
        return false;
    }
    if (settings.frame_generation!=BbSettings::FrameGenerationFsr ||
        settings.upscaler==BbSettings::UpscalerOff || !snapshot ||
        (!settings.window_focused && settings.background_mode!=BbSettings::BackgroundRun)) {
        Invalidate();
        if (settings.frame_generation==BbSettings::FrameGenerationNvidia && context) Reset();
        if (settings.frame_generation==BbSettings::FrameGenerationOff) { Reset(); failed=false; settings.frame_generation_problem=nullptr; }
        return false;
    }
    if (failed) return false;
    // Keep the FI/OF context within its buffered-frame budget without growing
    // a presentation queue or treating ordinary GPU back-pressure as a failure.
    if (pending.size()>=2) { submissions.wait(pending.front().tick); RetireCompleted(); }
    const auto& source=snapshot->source;
    const vk::Format color_format=snapshot->srgb8 ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR16G16B16A16Sfloat;
    if (context && (width!=snapshot->width || height!=snapshot->height ||
        render_width!=source.render_width || render_height!=source.render_height || context_hdr!=snapshot->hdr ||
        context_format!=color_format)) Reset();
    if (!context) {
        const FfxVkFsr3_3_1_6FrameGenerationCreateInfo create{
            VkPhysicalDevice(physical),VkDevice(device),
            source.render_width,source.render_height,snapshot->width,snapshot->height,VkFormat(color_format)};
        const auto result=ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create,&context);
        if (result!=FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            std::printf("FSR Frame Generation: context creation failed (%d)\n",int(result));
            settings.frame_generation_problem="FSR FG could not create its GPU context. Turn it off and on to retry.";
            failed=true; Invalidate(); return false;
        }
        width=snapshot->width; height=snapshot->height;
        context_hdr=snapshot->hdr;
        context_format=color_format;
        render_width=source.render_width; render_height=source.render_height;
        std::printf("FSR Frame Generation: %ux%u render, %ux%u display, analytical 3.1.6, %s color\n",
                    render_width,render_height,width,height,snapshot->srgb8 ? "8-bit sRGB" : "FP16 linear");
    }
    const u64 current_pause=BbBackground::PausedNanoseconds();
    const bool reset=!history || source.reset || paused_ns!=current_pause;
    paused_ns=current_pause;
    WriteBarrier(cmdbuf,vk::Image(snapshot->generated));
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{};
    prepare.commandBuffer=cmdbuf;
    prepare.color=Describe(vk::Image(snapshot->color),color_format,width,height);
    prepare.depth=Describe(vk::Image(snapshot->depth),vk::Format::eR32Sfloat,render_width,render_height);
    prepare.motionVectors=Describe(vk::Image(snapshot->motion),vk::Format::eR16G16Sfloat,render_width,render_height);
    prepare.renderWidth=render_width; prepare.renderHeight=render_height;
    prepare.jitterOffsetX=source.jitter[0]; prepare.jitterOffsetY=source.jitter[1];
    prepare.motionVectorScaleX=prepare.motionVectorScaleY=1.0f;
    prepare.frameTimeMilliseconds=std::clamp(source.frame_ms,1.0f,100.0f);
    // The luminance range only converts HDR transfer functions (PQ, scRGB).
    prepare.minLuminance=0.0f; prepare.maxLuminance=snapshot->srgb8 ? 1.0f : snapshot->hdr ? 10000.0f : 80.0f;
    prepare.transferFunction=snapshot->srgb8 ? FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB
                                             : FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SCRGB;
    prepare.cameraNear=source.near_plane; prepare.cameraFar=3000.0f;
    prepare.viewSpaceToMeters=1.0f; prepare.cameraVerticalFovRadians=source.vertical_fov;
    for (u32 i=0;i<3;++i) {
        prepare.cameraPosition[i]=source.inverse_view[i*4+3];
        prepare.cameraRight[i]=source.inverse_view[i*4];
        prepare.cameraUp[i]=source.inverse_view[i*4+1];
        prepare.cameraForward[i]=source.inverse_view[i*4+2];
    }
    prepare.frameId=++frame_id; prepare.reset=reset;
    recording={frame_id,0,snapshot};
    auto result=ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context,&prepare);
    if (result==FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        if (between_passes) between_passes(cmdbuf);
        FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{};
        dispatch.commandBuffer=cmdbuf; dispatch.color=prepare.color;
        dispatch.output=Describe(vk::Image(snapshot->generated),color_format,width,height);
        dispatch.displayWidth=width; dispatch.displayHeight=height;
        dispatch.interpolationWidth=width; dispatch.interpolationHeight=height;
        dispatch.frameTimeMilliseconds=prepare.frameTimeMilliseconds;
        dispatch.cameraNear=prepare.cameraNear; dispatch.cameraFar=prepare.cameraFar;
        dispatch.viewSpaceToMeters=prepare.viewSpaceToMeters;
        dispatch.cameraVerticalFovRadians=prepare.cameraVerticalFovRadians;
        dispatch.minLuminance=prepare.minLuminance; dispatch.maxLuminance=prepare.maxLuminance;
        dispatch.transferFunction=prepare.transferFunction; dispatch.frameId=frame_id; dispatch.reset=reset;
        result=ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context,&dispatch);
    }
    if (result==FFX_VK_FSR3_3_1_6_FRAMEGEN_OK && snapshot->hud) CompositeHud(cmdbuf,*snapshot);
    if (result!=FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        std::printf("FSR Frame Generation: prepare/dispatch failed (%d)\n",int(result));
        settings.frame_generation_problem="FSR FG dispatch failed. The real frame is kept; turn FG off and on to retry.";
        failed=true; Invalidate(); return false;
    }
    if (snapshot->hdr || (snapshot->sdr_unorm && !snapshot->srgb8)) {
        // FI writes linear FP16; a plain blit into a PQ swapchain would clamp
        // the HDR highlights. Encode into stable FP16 PQ, then blit to UNORM10.
        const vk::MemoryBarrier2 ready{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask=vk::AccessFlagBits2::eShaderRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&ready});
        WriteBarrier(cmdbuf,vk::Image(snapshot->encoded));
        const std::array<vk::DescriptorImageInfo,6> images{{
            {.imageView=*snapshot->generated_view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=source.depth_view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=source.motion_view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*snapshot->encoded_view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*snapshot->depth_view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*snapshot->motion_view,.imageLayout=vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet,6> writes{};
        for (u32 i=0;i<6;++i) writes[i]={.dstBinding=i,.descriptorCount=1,
            .descriptorType=i<2 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eStorageImage,.pImageInfo=&images[i]};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,*copy_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*copy_layout,0,writes);
        const std::array<u32,5> parameters{source.render_width,source.render_height,render_width,render_height,snapshot->hdr ? 2u : 5u};
        cmdbuf.pushConstants(*copy_layout,vk::ShaderStageFlagBits::eCompute,0,20,parameters.data());
        cmdbuf.dispatch((width+7)/8,(height+7)/8,1);
    }
    history=true; settings.frame_generation_active=reset ? 0 : 2;
    settings.frame_generation_problem=nullptr;
    return !reset;
}
bool FrameGeneration::Wanted() const {
    const auto& settings=BbSettings::Get();
    return settings.frame_generation==BbSettings::FrameGenerationFsr &&
           settings.upscaler!=BbSettings::UpscalerOff && !failed.load(std::memory_order_relaxed) &&
           !VramPaused();
}
void FrameGeneration::Submitted(u64 tick) {
    if (recording.snapshot) { recording.tick=tick; pending.push_back(std::move(recording)); recording={}; }
}
} // namespace Vulkan
