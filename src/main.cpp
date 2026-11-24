#include "clipping/replay_buffer.hpp"
#include <iostream>

int main() {
    clipping::ReplayBuffer buffer;
    std::size_t dropped = 0;
    // Simulate 120 seconds at 60 fps / 12 Mbps with an IDR every second.
    // These bytes are synthetic, NOT a recording or a GPU benchmark.
    for (std::uint64_t frame = 0; frame < 7200; ++frame) {
        auto result = buffer.push({frame * 1'000'000 / 60, frame % 60 == 0,
                                  std::vector<std::byte>(25'000)});
        if (result != clipping::PushResult::stored) ++dropped;
    }
    std::cout << "Synthetic replay-buffer probe (no screen capture)\n"
              << "retained_packets=" << buffer.packet_count() << '\n'
              << "retained_payload_bytes=" << buffer.payload_bytes() << '\n'
              << "rejected_packets=" << dropped << '\n';
}
