// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    /// bbport: before Open: a suffix for the cache folder of a GPU that keeps its own cache.
    void SetDeviceTag(std::string tag) {
        device_tag = std::move(tag);
    }
    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// bbport: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    /// bbport: whether a blob of that name is stored (written in an earlier session; blobs of
    /// this session may still be queued). Always false for the archived cache.
    [[nodiscard]] bool Exists(BlobType type, const std::string& name) const;
    /// bbport: deletes a stored blob (a pipeline key that no longer builds), during the warm-up
    /// once it ends (ForEachBlob holds the file open). Not for the archived cache.
    void Remove(BlobType type, const std::string& name);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::string device_tag{};
    std::vector<std::filesystem::path> removals{}; ///< bbport: deleted by FinishPreload
    bool preloaded{};
    bool opened{};
};

} // namespace Storage
