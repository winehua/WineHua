#include "graphics/renderer_repaint_state.h"
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
    winehua::RendererRepaintState state;
    auto first = state.CaptureRequest();
    assert(state.NeedsRedraw(first));
    // A failed swap does not acknowledge a first frame or a resume request.
    assert(state.NeedsRedraw(state.CaptureRequest()));
    state.DidPresent(first);
    for (int i = 0; i != 10000; ++i)
        assert(!state.NeedsRedraw(state.CaptureRequest()));

    // The surface can be shown again with exactly the same dimensions and
    // no new Wine commit. It must present its cached texture once.
    state.RequestRedraw();
    auto shown = state.CaptureRequest();
    assert(state.NeedsRedraw(shown));
    state.DidPresent(shown);
    assert(!state.NeedsRedraw(state.CaptureRequest()));

    // A second WINDOW_SHOWN during the swap cannot be consumed by the first.
    state.RequestRedraw();
    auto beforeSwap = state.CaptureRequest();
    state.RequestRedraw();
    state.DidPresent(beforeSwap);
    assert(state.NeedsRedraw(state.CaptureRequest()));
    state.DidPresent(state.CaptureRequest());
    assert(!state.NeedsRedraw(state.CaptureRequest()));

    // Multiple UI notifications coalesce. Only the render thread owns the
    // presentation acknowledgement, including when UI requests race it.
    auto stale = state.CaptureRequest();
    std::thread ui([&]() {
        for (int i = 0; i != 10000; ++i) state.RequestRedraw();
    });
    state.DidPresent(stale);
    ui.join();
    assert(state.NeedsRedraw(state.CaptureRequest()));
    state.DidPresent(state.CaptureRequest());
    assert(!state.NeedsRedraw(state.CaptureRequest()));
    std::puts("PASS: initial paint, same-size resume, failed swap, late notification and coalescing");
}
