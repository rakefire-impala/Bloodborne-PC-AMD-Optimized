// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <deque>
#include <mutex>
#include <type_traits>
#include <vector>

#include "bbport_toggles.h"
#include "common/debug.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/readback_audit.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_) : tracker{&tracker_} {}
    ~MemoryTracker() = default;

    /// Returns true if a region has been modified from the CPU
    bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                return manager->template IsRegionModified<Type::CPU>(offset, size);
            });
    }

    /// Returns true if a region has been modified from the GPU
    bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<false>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                return manager->template IsRegionModified<Type::GPU>(offset, size);
            });
    }

    /// Mark region as CPU modified, notifying the device_tracker about this change
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::CPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// bbport: see RegionManager::ExtendWriteFault.
    void ExtendWriteFault(VAddr window_addr, u64 size) {
        IteratePages<false>(window_addr, size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->ExtendWriteFault(manager->GetCpuAddr() + offset, size);
                            });
    }

    /// bbport: see RegionManager::ForEachProtectedGpuRange.
    void ForEachProtectedGpuRange(VAddr cpu_addr, u64 size, auto&& func) {
        IteratePages<false>(cpu_addr, size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->ForEachProtectedGpuRange(offset, size, func);
                            });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, false>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// bbport: marks a region GPU-modified again (pages whose GPU-written bytes survived a
    /// merged upload, see BufferCache::TwinGpuPages).
    void MarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Removes all protection from a page and ensures GPU data has been flushed if requested.
    /// bbport: with readbacks Off, `on_gpu_pages(address, size)` sees the GPU-modified pages of
    /// a write fault while they are still protected, before the guest store lands.
    void InvalidateRegion(VAddr cpu_addr, u64 size, bool unmapping, auto&& on_flush,
                          auto&& on_gpu_pages) noexcept {
        const bool audit = ReadbackAudit::Enabled();
        ReadbackAudit::Ranges skipped;
        IteratePages<false>(
            cpu_addr, size, [&](RegionManager* manager, u64 offset, size_t size) {
                const bool should_flush = [&] {
                    // Perform both the GPU modification check and CPU state change with the lock
                    // in case we are racing with GPU thread trying to mark the page as GPU
                    // modified. If we need to flush the flush function is going to perform CPU
                    // state change.
                    std::scoped_lock lk{manager->lock};
                    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                        if (manager->template IsRegionModified<Type::GPU>(offset, size)) {
                            return true;
                        }
                    } else {
                        // bbport: on_gpu_pages twins GPU-written bytes the GPU still writes;
                        // false: they are downloaded first, as with readbacks Relaxed.
                        bool download = false;
                        manager->template ForEachGpuModifiedRange<false>(
                            offset, size, [&](VAddr address, u64 bytes) {
                                if (audit) {
                                    skipped.emplace_back(address, bytes);
                                }
                                if (!unmapping && !on_gpu_pages(address, bytes)) {
                                    download = true;
                                }
                            });
                        if (download) {
                            return true;
                        }
                    }
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + offset, size);
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
        ReadbackAudit::Record(unmapping ? ReadbackAudit::Kind::Unmap : ReadbackAudit::Kind::Write,
                              skipped);
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    void ForEachUploadRange(VAddr query_cpu_range, u64 query_size, bool is_written, auto&& func,
                            auto&& on_upload) {
        const bool audit = ReadbackAudit::Enabled();
        ReadbackAudit::Ranges stale;
        IteratePages<true>(query_cpu_range, query_size,
                           [&func, &stale, audit, is_written](RegionManager* manager, u64 offset,
                                                              size_t size) {
                               // bbport: read-only bindings skip the region lock when no page
                               // in range is CPU-modified. A guest write sets its bit in the
                               // fault handler before the store, and the draw that uses the
                               // data reaches this thread through the submission queue lock.
                               if (!is_written &&
                                   !BbToggle::Disabled(BbToggle::LockFreeUploadCheck) &&
                                   !manager->template IsRegionModified<Type::CPU>(offset, size)) {
                                   return;
                               }
                               manager->lock.lock();
                               if (audit) {
                                   manager->template ForEachGpuModifiedRange<true>(
                                       offset, size, [&stale](VAddr address, u64 bytes) {
                                           stale.emplace_back(address, bytes);
                                       });
                               }
                               manager->template ForEachModifiedRange<Type::CPU, true>(
                                   manager->GetCpuAddr() + offset, size, func);
                               if (!is_written) {
                                   manager->lock.unlock();
                               }
                           });
        on_upload();
        if (is_written) {
            IteratePages<false>(query_cpu_range, query_size,
                                [](RegionManager* manager, u64 offset, size_t size) {
                                    manager->template ChangeRegionState<Type::GPU, true>(
                                        manager->GetCpuAddr() + offset, size);
                                    manager->lock.unlock();
                                });
        }
        ReadbackAudit::Record(ReadbackAudit::Kind::StaleUpload, stale);
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <bool clear>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, auto&& func) {
        IteratePages<false>(query_cpu_range, query_size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ForEachModifiedRange<Type::GPU, clear>(
                                    manager->GetCpuAddr() + offset, size, func);
                            });
    }

private:
    /**
     * @brief IteratePages Iterates L2 word manager page table.
     * @param cpu_address Start byte cpu address
     * @param size Size in bytes of the region of iterate.
     * @param func Callback for each word manager.
     * @return
     */
    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        RENDERER_TRACE;
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> TRACKER_HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & TRACKER_HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager{top_tier[page_index]};
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = top_tier[page_index];
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    void CreateRegion(std::size_t page_index) {
        const VAddr base_cpu_addr = page_index << TRACKER_HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                std::construct_at(&last_pool[i], tracker, 0);
                free_managers.push_back(&last_pool[i]);
            }
        }
        // Each manager tracks a 4_MB virtual address space.
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        free_managers.pop_back();
        top_tier[page_index] = new_manager;
    }

    PageManager* tracker;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<RegionManager*, NUM_HIGH_PAGES> top_tier{};
};

} // namespace VideoCore
