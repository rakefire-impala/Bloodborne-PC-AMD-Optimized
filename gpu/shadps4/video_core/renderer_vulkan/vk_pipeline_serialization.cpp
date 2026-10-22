// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "common/hash.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 7u; // bbport: interpolated integer fix (Pascal)
static constexpr u32 ShaderMetaVersion = 7u; // bbport: ImageResource::needs_native
static constexpr u32 PipelineKeyVersion = 5u; // bbport: Info layout (ImageResource::needs_native)
/// bbport: marks the motion buffer addresses after a motion vertex shader's Info.
static constexpr u64 MotionAddressesTag = 0x5345525344444142ull;
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);
    // bbport: a motion vertex shader embeds the object motion buffer addresses: they follow the
    // Info (tagged; other shaders keep their layout). They are the same from start
    // to start on one GPU and driver; LoadShaderMeta takes the shader while they still are.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        meta.Write(Serialization::MotionAddressesTag);
        meta.Write(Shader::MotionVectors::params_address);
        meta.Write(Shader::MotionVectors::positions_address);
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);

    // Motion vertex shaders embed the object motion buffer addresses (RegisterShaderMeta
    // records them). Loaded only while this start has the same ones; an older entry without
    // them is compiled again.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        // The addresses follow the Info (an entry without them has only the archive's slack
        // byte left here).
        if (ar.Remaining() < 3 * sizeof(u64) || !Shader::MotionVectors::positions_address) {
            return false;
        }
        u64 tag{}, params_address{}, positions_address{};
        meta.Read(tag);
        meta.Read(params_address);
        meta.Read(positions_address);
        if (tag != Serialization::MotionAddressesTag ||
            params_address != Shader::MotionVectors::params_address ||
            positions_address != Shader::MotionVectors::positions_address) {
            return false;
        }
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    it.value() =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    // Motion keys build only with object motion on: their vertex shaders load only while the
    // motion buffers keep the addresses those embed (LoadShaderMeta); otherwise the key is
    // stale and compiles at runtime.
    if (sel.graphics_key.motion_vectors && !Shader::MotionVectors::positions_address) {
        return false;
    }

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            // bbport: the cache lost this stage, so the key never builds; a pre-build deletes
            // it (a new permutation could take the index and pair with the old key's stages).
            if (std::getenv("BB_PREBUILD_SHADERS")) {
                unbuildable_keys.push_back(fmt::format(
                    "g_{:#018x}", std::hash<GraphicsPipelineKey>{}(sel.graphics_key)));
            }
            sel.infos.fill(nullptr);
            sel.modules.fill(nullptr);
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    ASSERT(is_new);

    it.value() = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    if (!it.value()->Handle()) {
        // bbport: the driver refused this key's stages together (GraphicsPipeline logged it).
        // Its shaders stay loaded (each fits its own specialization); the key is deleted so
        // the next start does not build it again, and a draw that needs it builds it anew.
        auto& storage = Storage::DataBase::Instance();
        const auto name =
            fmt::format("g_{:#018x}", std::hash<GraphicsPipelineKey>{}(sel.graphics_key));
        const bool stored = storage.Exists(Storage::BlobType::PipelineKey, name);
        storage.Remove(Storage::BlobType::PipelineKey, name);
        LOG_WARNING(Render_Vulkan, "Pipeline key {} {}", name,
                    stored ? "removed" : "not found under that name");
        graphics_pipelines.erase(it);
        return false;
    }
    if (sel.graphics_key.motion_vectors == 1) {
        preloaded_motion.emplace_back(sel.graphics_key, std::move(sdata));
    }
    return true;
}

// bbport: the plain permutation of one stage of a motion pipeline: the program's module whose
// specialization is the motion one without the motion flags (GetStillTwin's runtime infos).
// Permutations no loaded key uses are read from the cache.
bool PipelineCache::PlainPermutation(Program& program, u32 stage, size_t motion_hash,
                                     size_t& plain_hash, vk::ShaderModule& module,
                                     Shader::RuntimeInfo& runtime_info) {
    const u64 pgm_hash = program.info.pgm_hash;
    std::optional<Shader::StageSpecialization> plain;
    for (size_t idx = 0; idx < program.modules.size(); ++idx) {
        if (program.modules[idx].module && HashCombine(pgm_hash, idx) == motion_hash) {
            plain = program.modules[idx].spec;
            break;
        }
    }
    if (!plain) {
        return false;
    }
    if (stage == u32(Shader::SwStage::Vertex)) {
        plain->runtime_info.hw.vs.motion_vectors = false;
    } else if (stage == u32(Shader::SwStage::Fragment)) {
        plain->runtime_info.hw.fs.motion_vectors = 0;
    }
    const auto find = [&]() -> bool {
        for (size_t idx = 0; idx < program.modules.size(); ++idx) {
            if (program.modules[idx].module && program.modules[idx].spec == *plain) {
                plain_hash = HashCombine(pgm_hash, idx);
                module = program.modules[idx].module;
                runtime_info = program.modules[idx].spec.runtime_info;
                return true;
            }
        }
        return false;
    };
    if (find()) {
        return true;
    }
    auto& storage = Storage::DataBase::Instance();
    for (size_t idx = 0, missing = 0; idx < 64 && missing < 8; ++idx) {
        if (idx < program.modules.size() && program.modules[idx].module) {
            continue;
        }
        std::vector<u8> meta_blob;
        storage.Load(Storage::BlobType::ShaderMeta,
                     fmt::format("{:#018x}", HashCombine(pgm_hash, idx)), meta_blob);
        if (meta_blob.empty()) {
            missing += idx >= program.modules.size();
            continue;
        }
        Serialization::Archive meta_ar{std::move(meta_blob)};
        LoadPipelineStage(meta_ar, stage);
    }
    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();
    return find();
}

// bbport: still twins (GetStillTwin) of the motion pipelines the warm-up loaded. A twin is the
// motion key with the plain permutations of its stages; twins in the cache are linked here, so
// a motion pipeline's first draw needs no full selection. With `build` (BB_PREBUILD_SHADERS,
// the launcher's "Pre-build shaders") the missing ones are compiled and stored too: the first
// visit of an area then compiles none of them while playing.
void PipelineCache::PrepareStillTwins(bool build) {
    u32 linked = 0, built = 0, failed = 0, missing = 0, no_plain = 0;
    const auto started = std::chrono::steady_clock::now();
    u32 done = 0;
    for (auto& [motion_key, sdata] : preloaded_motion) {
        ++done;
        const auto found = graphics_pipelines.find(motion_key);
        if (found == graphics_pipelines.end()) {
            continue;
        }
        const GraphicsPipeline* pipeline = found->second.get();
        GraphicsPipelineKey twin_key = motion_key;
        twin_key.motion_vectors = 2;
        twin_key.write_masks[Shader::MotionVectors::Output] = {};
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        bool plain = true;
        for (u32 stage = 0; stage < MaxShaderStages && plain; ++stage) {
            if (!motion_key.stage_hashes[stage]) {
                continue;
            }
            const auto program = program_cache.find(pipeline->GetStage(Shader::SwStage(stage)).pgm_hash);
            plain = program != program_cache.end() &&
                    PlainPermutation(*program.value(), stage, motion_key.stage_hashes[stage],
                                     twin_key.stage_hashes[stage], modules[stage],
                                     runtime_infos[stage]);
            if (plain) {
                infos[stage] = &program.value()->info;
            }
        }
        if (!plain) {
            ++no_plain;
            continue;
        }
        if (const auto twin = graphics_pipelines.find(twin_key); twin != graphics_pipelines.end()) {
            pipeline->SetStillTwin(twin->second.get());
            ++linked;
            continue;
        }
        if (!build) {
            ++missing;
            continue;
        }
        auto twin_sdata = sdata;
        auto twin = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, twin_key, *pipeline_cache, infos,
            runtime_infos, pipeline->GetFetchShader(), modules, twin_sdata, true);
        if (!twin->Handle()) {
            ++failed;
            continue;
        }
        RegisterPipelineData(twin_key, std::hash<GraphicsPipelineKey>{}(twin_key), twin_sdata);
        pipeline->SetStillTwin(twin.get());
        graphics_pipelines.emplace(twin_key, std::move(twin));
        ++num_new_pipelines;
        if (++built % 50 == 0) {
            std::printf("Shader pre-build: %u still twins built (%u of %zu motion pipelines)\n",
                        built, done, preloaded_motion.size());
            std::fflush(stdout);
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (build) {
        std::printf("Shader pre-build: %zu motion pipelines; still twins %u built now, %u already "
                    "cached, %u failed, %u without cached plain shaders (built in play); %.1f s\n",
                    preloaded_motion.size(), built, linked, failed, no_plain, seconds);
    } else if (!preloaded_motion.empty()) {
        std::printf("Still twins: %u of %zu motion pipelines linked from the cache, %u build in "
                    "play (Pre-build shaders builds them before)\n",
                    linked, preloaded_motion.size(), missing + no_plain);
    }
    preloaded_motion.clear();
    preloaded_motion.shrink_to_fit();
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;

    return true;
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    // bbport: an integrated GPU keeps its own cache. Its shaders differ (device features, the
    // motion shaders' buffer addresses), and the incompatible-profile check below would clear
    // the discrete GPU's cache on every switch between the two (laptops with both).
    Storage::DataBase::Instance().SetDeviceTag(instance.IsIntegrated() ? "-igpu" : "");
    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                    profile_data.size(), sizeof(Shader::Profile));
    }
    Shader::Profile cached_profile{};
    if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    }
    if (profile_data.size() != sizeof(Shader::Profile) || cached_profile != profile) {
        // bbport: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            bool result{};
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }

            if (result) {
                ++num_pipelines;
            }
        });

    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    // bbport: BB_PREBUILD_SHADERS=1 builds the pipelines the cache can derive before the game
    // starts (still twins) and deletes keys whose shaders are gone; =only ends the run then.
    const char* prebuild = std::getenv("BB_PREBUILD_SHADERS");
    const bool build = prebuild && prebuild[0] && prebuild[0] != '0';
    PrepareStillTwins(build);
    if (build) {
        auto& storage = Storage::DataBase::Instance();
        for (const auto& name : unbuildable_keys) {
            storage.Remove(Storage::BlobType::PipelineKey, name);
        }
        std::printf("Shader pre-build: %zu cached pipelines on %s; %u preloaded, %zu keys "
                    "removed (their shaders are no longer cached)\n",
                    size_t(num_total_pipelines), std::string{instance.GetModelName()}.c_str(),
                    num_pipelines, unbuildable_keys.size());
        unbuildable_keys.clear();
    }

    Storage::DataBase::Instance().FinishPreload();

    if (build && std::string_view{prebuild} == "only") {
        Storage::DataBase::Instance().Drain();
        std::printf("Shader pre-build: done\n");
        std::fflush(stdout);
        std::_Exit(0);
    }
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
