#pragma once
#include "clipping/replay_buffer.hpp"
#include <atomic>
#include <memory>

namespace clipping {
namespace detail {
struct ReplayBudget {
    std::atomic_size_t used{0};
    std::atomic_bool snapshot_active{false};
};
struct RetainedPacket final : Packet {
    std::shared_ptr<ReplayBudget> budget;
    std::size_t charge;
    RetainedPacket(Packet packet, std::shared_ptr<ReplayBudget> owner) noexcept
        : Packet(std::move(packet)), budget(std::move(owner)), charge(data.capacity()) {}
    ~RetainedPacket() {
        // Free bytes before advertising them as available to the capture thread.
        std::vector<std::byte>().swap(data);
        budget->used.fetch_sub(charge, std::memory_order_relaxed);
    }
};
}
using PacketHandle = std::shared_ptr<const detail::RetainedPacket>;

// Immutable, one-at-a-time snapshot. Destroying it on the export thread returns
// packet charges only when the live ring no longer owns those same packets.
class ReplaySnapshot {
public:
    ReplaySnapshot(const ReplaySnapshot&) = delete;
    ReplaySnapshot& operator=(const ReplaySnapshot&) = delete;
    ~ReplaySnapshot() {
        packets_.clear();
        budget_->snapshot_active.store(false, std::memory_order_release);
    }
    [[nodiscard]] const std::vector<PacketHandle>& packets() const { return packets_; }
    [[nodiscard]] std::size_t payload_bytes() const { return bytes_; }
private:
    friend class SharedReplayBuffer;
    explicit ReplaySnapshot(std::shared_ptr<detail::ReplayBudget> budget) : budget_(std::move(budget)) {}
    std::shared_ptr<detail::ReplayBudget> budget_;
    std::vector<PacketHandle> packets_;
    std::size_t bytes_ = 0;
};

// Only the capture thread mutates this ring. The export thread owns a snapshot
// and may release shared immutable packets concurrently. Budget includes unique
// vector capacities across both owners, not duplicate references or total RSS.
class SharedReplayBuffer {
public:
    explicit SharedReplayBuffer(Limits limits = {}) : limits_(limits) {
        if (!limits.payload_bytes || !limits.packets || !limits.duration_us)
            throw std::invalid_argument("Replay limits must be positive");
    }
    SharedReplayBuffer(const SharedReplayBuffer&) = delete;
    SharedReplayBuffer& operator=(const SharedReplayBuffer&) = delete;
    PushResult push(Packet packet) {
        if (packet.data.empty() || (last_pts_ && packet.pts_us <= *last_pts_)) return PushResult::invalid;
        last_pts_ = packet.pts_us;
        const auto bytes = packet.data.capacity();
        if (bytes > limits_.payload_bytes) { clear(); return PushResult::oversized; }
        while (!packets_.empty() &&
               (shared_payload_bytes() > limits_.payload_bytes - bytes ||
                packets_.size() >= limits_.packets ||
                packet.pts_us - packets_.front()->pts_us > limits_.duration_us))
            evict_gop();
        // A pinned export can keep memory alive after eviction. Never admit data
        // past the global cap, and discard dependent frames until the next IDR.
        if (shared_payload_bytes() > limits_.payload_bytes - bytes) {
            clear(); ++pressure_drops_; return PushResult::waiting_for_keyframe;
        }
        if (packets_.empty() && !packet.keyframe) return PushResult::waiting_for_keyframe;
        // Only this thread adds charges; the worker can only decrease them.
        budget_->used.fetch_add(bytes, std::memory_order_relaxed);
        PacketHandle retained;
        try { retained = std::make_shared<detail::RetainedPacket>(std::move(packet), budget_); }
        catch (...) { budget_->used.fetch_sub(bytes, std::memory_order_relaxed); throw; }
        packets_.push_back(std::move(retained));
        live_bytes_ += bytes;
        return PushResult::stored;
    }
    // max_bytes leaves explicit headroom for new recording while export owns old
    // packets. Trim only whole GOPs and refuse if even the last GOP cannot fit.
    [[nodiscard]] std::unique_ptr<ReplaySnapshot> snapshot(std::size_t max_bytes) const {
        if (packets_.empty()) return nullptr;
        std::size_t begin = 0, bytes = live_bytes_;
        while (begin < packets_.size() && bytes > max_bytes) {
            do { bytes -= packets_[begin++]->charge; }
            while (begin < packets_.size() && !packets_[begin]->keyframe);
        }
        if (begin == packets_.size()) return nullptr;
        bool expected = false;
        if (!budget_->snapshot_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return nullptr;
        std::unique_ptr<ReplaySnapshot> result;
        try { result.reset(new ReplaySnapshot(budget_)); }
        catch (...) { budget_->snapshot_active.store(false, std::memory_order_release); throw; }
        result->packets_.reserve(packets_.size() - begin);
        for (std::size_t i = begin; i < packets_.size(); ++i) result->packets_.push_back(packets_[i]);
        result->bytes_ = bytes;
        return result;
    }
    [[nodiscard]] const std::deque<PacketHandle>& packets() const { return packets_; }
    [[nodiscard]] std::size_t payload_bytes() const { return live_bytes_; }
    [[nodiscard]] std::size_t shared_payload_bytes() const { return budget_->used.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t pressure_drops() const { return pressure_drops_; }
private:
    void clear() { packets_.clear(); live_bytes_ = 0; }
    void evict_gop() {
        do { live_bytes_ -= packets_.front()->charge; packets_.pop_front(); }
        while (!packets_.empty() && !packets_.front()->keyframe);
    }
    Limits limits_;
    std::shared_ptr<detail::ReplayBudget> budget_ = std::make_shared<detail::ReplayBudget>();
    std::deque<PacketHandle> packets_;
    std::size_t live_bytes_ = 0, pressure_drops_ = 0;
    std::optional<std::uint64_t> last_pts_;
};
} // namespace clipping
