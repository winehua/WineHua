// Replay physical relative-motion bursts through the production InputQueue.
// Platform event-loop plumbing is stubbed; the queue and deduplication are real.
#include "compositor/input/input_queue.h"
#include <wayland-server-core.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

wl_event_loop* wl_display_get_event_loop(wl_display*) { return nullptr; }
wl_event_source* wl_event_loop_add_fd(wl_event_loop*, int, uint32_t,
                                     int (*)(int, uint32_t, void*), void*) { return nullptr; }
int wl_event_source_remove(wl_event_source*) { return 0; }
void wl_display_flush_clients(wl_display*) {}

int main() {
    using Event = InputQueue::Event;
    // Five seconds of reports, consumed in batches at simulated frame rates.
    // No wall-clock pacing: this verifies displacement preservation, not FPS.
    for (int reportRate : {125, 500, 1000}) {
        for (int frameRate : {30, 60, 120}) {
            InputQueue queue;
            int reports = 0, delivered = 0;
            int64_t expectedX = 0, expectedY = 0, actualX = 0, actualY = 0;
            auto drain = [&] {
                for (const auto& event : queue.Poll()) {
                    assert(event.type == Event::REL_MOTION);
                    ++delivered;
                    actualX += event.x;
                    actualY += event.y;
                }
            };
            for (int frame = 0; frame < frameRate * 5; ++frame) {
                const int through = (frame + 1) * reportRate / frameRate;
                while (reports < through) {
                    const int32_t dx = reports % 17 < 12 ? 128 : -64;
                    const int32_t dy = reports % 11 < 7 ? -32 : 96;
                    queue.Enqueue(Event::REL_MOTION, 7, nullptr, dx, dy, 0, 0);
                    expectedX += dx;
                    expectedY += dy;
                    ++reports;
                }
                drain();
            }
            drain();
            assert(reports == reportRate * 5 && delivered == reports);
            assert(actualX == expectedX && actualY == expectedY);
            std::printf("mouse queue replay: %d reports/s, %d batches/s, %d reports PASS\n",
                        reportRate, frameRate, delivered);
        }
    }

    // Repeated equal relative deltas are cumulative. A button boundary must
    // retain both press and release while absolute positions can coalesce.
    InputQueue queue;
    queue.Enqueue(Event::PTR_MOTION, 7, nullptr, 100, 100, 0, 0);
    queue.Enqueue(Event::PTR_MOTION, 7, nullptr, 200, 200, 0, 0);
    queue.Enqueue(Event::PTR_BUTTON, 7, nullptr, 0, 0, 272, 1);
    for (int index = 0; index < 200; ++index)
        queue.Enqueue(Event::REL_MOTION, 7, nullptr, 32, -16, 0, 0);
    queue.Enqueue(Event::PTR_BUTTON, 7, nullptr, 0, 0, 272, 0);
    const auto drag = queue.Poll();
    assert(drag.size() == 203);
    assert(drag.front().type == Event::PTR_MOTION && drag.front().x == 200);
    assert(drag[1].type == Event::PTR_BUTTON && drag[1].state == 1);
    assert(drag.back().type == Event::PTR_BUTTON && drag.back().state == 0);

    // Concurrent producer/consumer catches lost reports at batch swaps.
    constexpr int count = 20000;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int index = 1; index <= count; ++index) {
            queue.Enqueue(Event::REL_MOTION, index, nullptr, 32, -16, 0, 0);
            if (index % 97 == 0) std::this_thread::yield();
        }
        done.store(true);
    });
    int delivered = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!done.load() || delivered < count) &&
           std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : queue.Poll()) {
            assert(event.type == Event::REL_MOTION);
            assert(event.tl == static_cast<uint32_t>(++delivered));
            assert(event.x == 32 && event.y == -16);
        }
        std::this_thread::yield();
    }
    producer.join();
    assert(delivered == count && queue.Poll().empty());
    std::puts("mouse queue: repeated deltas, drag ordering and concurrent batches PASS");
}
