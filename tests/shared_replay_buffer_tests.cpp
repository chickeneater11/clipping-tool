#include "clipping/shared_replay_buffer.hpp"
#include <iostream>
#include <thread>
using namespace clipping;
void check(bool value) { if (!value) throw std::runtime_error("shared replay invariant failed"); }
Packet packet(std::uint64_t pts, bool idr = true, std::size_t bytes = 4) {
    return {pts, idr, std::vector<std::byte>(bytes)};
}
int main() {
    try {
        SharedReplayBuffer b({16, 20, 100});
        b.push(packet(0)); b.push(packet(1, false)); b.push(packet(2)); b.push(packet(3, false));
        auto saved = b.snapshot(8);
        check(saved && saved->payload_bytes() == 8 && saved->packets().front()->pts_us == 2);
        check(b.shared_payload_bytes() == 16); // snapshot references do not double charges
        check(!b.snapshot(8)); // only one snapshot can exist
        check(b.push(packet(4)) == PushResult::stored);
        check(b.shared_payload_bytes() == 12 && saved->packets().front()->pts_us == 2);
        b.push(packet(5, false)); b.push(packet(6));
        check(b.shared_payload_bytes() <= 16);
        check(saved->packets().size() == 2 && saved->packets().back()->pts_us == 3);
        saved.reset();
        check(b.shared_payload_bytes() == b.payload_bytes());
        auto again = b.snapshot(8); check(again != nullptr); again.reset();

        SharedReplayBuffer pinned({8, 20, 100});
        pinned.push(packet(0)); pinned.push(packet(1, false));
        auto full = pinned.snapshot(8);
        check(pinned.push(packet(2)) == PushResult::waiting_for_keyframe);
        check(pinned.shared_payload_bytes() == 8 && pinned.packets().empty());
        check(pinned.pressure_drops() == 1);
        full.reset(); check(pinned.shared_payload_bytes() == 0);
        check(pinned.push(packet(3, false)) == PushResult::waiting_for_keyframe);
        check(pinned.push(packet(4)) == PushResult::stored);
        check(!pinned.snapshot(3)); // must not start halfway through a GOP

        SharedReplayBuffer duration({64, 3, 5});
        duration.push(packet(0)); duration.push(packet(3)); duration.push(packet(6));
        check(duration.packets().front()->pts_us == 3);
        check(duration.push(packet(6)) == PushResult::invalid);
        auto reserved = packet(7); reserved.data.reserve(65);
        check(duration.push(std::move(reserved)) == PushResult::oversized);
        check(duration.packets().empty());

        // A snapshot stays valid if the producer exits before its worker.
        std::unique_ptr<ReplaySnapshot> orphan;
        {
            SharedReplayBuffer temporary({16, 10, 100});
            temporary.push(packet(0));
            orphan = temporary.snapshot(8);
        }
        check(orphan->packets().front()->data.size() == 4);
        orphan.reset();
        // Simulate an export failure: RAII must reopen the single export slot.
        auto failed_export = pinned.snapshot(8);
        check(failed_export != nullptr);
        try {
            auto worker_owned = std::move(failed_export);
            throw std::runtime_error("simulated export failure");
        } catch (const std::runtime_error&) {}
        check(pinned.shared_payload_bytes() == pinned.payload_bytes());
        auto retry = pinned.snapshot(8); check(retry != nullptr); retry.reset();

        // Concurrent snapshot release while the sole producer allocates/evicts.
        SharedReplayBuffer stress({1024, 64, 100});
        std::uint64_t tick = 0;
        for (unsigned round = 0; round < 100; ++round) {
            for (unsigned i = 0; i < 32; ++i) { stress.push(packet(tick++, true, 16)); }
            auto snapshot = stress.snapshot(512); check(snapshot != nullptr);
            std::jthread worker([owned = std::move(snapshot)]() mutable {
                for (const auto& p : owned->packets()) {
                    if (p->data.size() != 16) std::terminate();
                    std::this_thread::yield();
                }
                owned.reset();
            });
            for (unsigned i = 0; i < 128; ++i) {
                stress.push(packet(tick++, true, 16));
                check(stress.shared_payload_bytes() <= 1024);
            }
            worker.join();
            check(stress.shared_payload_bytes() == stress.payload_bytes());
        }
        std::cout << "Shared replay accounting and concurrent-release tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
