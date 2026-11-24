#include "clipping/replay_buffer.hpp"
#include <iostream>
#include <stdexcept>
using namespace clipping;
void check(bool ok) { if (!ok) throw std::runtime_error("check failed"); }
Packet packet(std::uint64_t pts, bool key, std::size_t size = 4) {
    return {pts, key, std::vector<std::byte>(size)};
}
int main() {
    try {
        bool threw = false;
        try { ReplayBuffer invalid({0, 10, 10}); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw);
        ReplayBuffer b({12, 10, 100});
        check(b.push(packet(0, false)) == PushResult::waiting_for_keyframe);
        check(b.push(packet(1, true)) == PushResult::stored);
        b.push(packet(2, false)); b.push(packet(3, true)); b.push(packet(4, false));
        check(b.packet_count() == 2 && b.packets().front().pts_us == 3);
        check(b.payload_bytes() == 8);
        check(b.push(packet(4, true)) == PushResult::invalid);
        check(b.push(packet(5, false, 13)) == PushResult::oversized);
        check(b.packet_count() == 0 && b.payload_bytes() == 0);
        check(b.push(packet(6, false)) == PushResult::waiting_for_keyframe);
        b.reset(); check(b.push(packet(0, true)) == PushResult::stored);
        ReplayBuffer duration({100, 10, 5});
        duration.push(packet(0, true)); duration.push(packet(3, true));
        duration.push(packet(6, false));
        check(duration.packets().front().pts_us == 3);
        ReplayBuffer count({100, 2, 100});
        count.push(packet(0, true)); count.push(packet(1, false));
        check(count.push(packet(2, false)) == PushResult::waiting_for_keyframe);
        check(count.packet_count() == 0);
        Packet reserved = packet(3, true, 1); reserved.data.reserve(101);
        check(count.push(std::move(reserved)) == PushResult::oversized);
        check(count.push(packet(4, true, 0)) == PushResult::invalid);
        ReplayBuffer stress({1024, 60, 100});
        for (std::uint64_t i = 0; i < 100'000; ++i) {
            stress.push(packet(i, i % 15 == 0, static_cast<std::size_t>(i % 120 + 1)));
            check(stress.payload_bytes() <= 1024 && stress.packet_count() <= 60);
            if (!stress.packets().empty()) {
                check(stress.packets().front().keyframe);
                check(i - stress.packets().front().pts_us <= 100);
            }
        }
        std::cout << "All replay-buffer tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
