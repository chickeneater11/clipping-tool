#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace clipping {
// One encoded video access unit. For this first core: no B frames, monotonic PTS,
// and closed GOPs. keyframe means independently decodable IDR, not any I-frame.
struct Packet {
    std::uint64_t pts_us;
    bool keyframe;
    std::vector<std::byte> data;
};
struct Limits {
    std::size_t payload_bytes = 64 * 1024 * 1024;
    std::size_t packets = 3600;
    std::uint64_t duration_us = 30'000'000;
};
enum class PushResult { stored, waiting_for_keyframe, oversized, invalid };

// Single-owner component. The producer transfers packet ownership. No snapshot
// API: an unbounded collection of shared snapshots could defeat memory limits.
class ReplayBuffer {
public:
    explicit ReplayBuffer(Limits limits = {}) : limits_(limits) {
        if (!limits.payload_bytes || !limits.packets || !limits.duration_us)
            throw std::invalid_argument("Replay limits must be positive");
    }
    PushResult push(Packet packet) {
        if (packet.data.empty() || (last_pts_ && packet.pts_us <= *last_pts_))
            return PushResult::invalid;
        last_pts_ = packet.pts_us;
        // Charge allocated capacity, not just the used vector length.
        const auto bytes = packet.data.capacity();
        if (bytes > limits_.payload_bytes) {
            clear(); // Missing this frame invalidates dependent frames.
            return PushResult::oversized;
        }
        while (!packets_.empty() &&
               (bytes > limits_.payload_bytes - payload_bytes_ ||
                packets_.size() >= limits_.packets ||
                packet.pts_us - packets_.front().pts_us > limits_.duration_us))
            evict_gop();
        if (packets_.empty() && !packet.keyframe)
            return PushResult::waiting_for_keyframe;
        packets_.push_back(std::move(packet));
        payload_bytes_ += bytes;
        return PushResult::stored;
    }
    // Call on a stream discontinuity/device reset, then wait for a new IDR.
    void reset() { clear(); last_pts_.reset(); }
    [[nodiscard]] std::size_t payload_bytes() const { return payload_bytes_; }
    [[nodiscard]] std::size_t packet_count() const { return packets_.size(); }
    [[nodiscard]] const std::deque<Packet>& packets() const { return packets_; }
private:
    void clear() { packets_.clear(); payload_bytes_ = 0; }
    void evict_gop() {
        do {
            payload_bytes_ -= packets_.front().data.capacity();
            packets_.pop_front();
        } while (!packets_.empty() && !packets_.front().keyframe);
    }
    Limits limits_;
    std::deque<Packet> packets_;
    std::size_t payload_bytes_ = 0;
    std::optional<std::uint64_t> last_pts_;
};
} // namespace clipping
