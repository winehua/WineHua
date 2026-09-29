#include "compositor/frame/direct_viewport.h"
#include <cstdio>
#include <limits>

static int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #value); } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }
static void point(const DirectImageSampling& sampling, double x, double y, double u, double v) {
    CHECK(near(sampling.u[0] + x * sampling.u[1] + y * sampling.u[2], u));
    CHECK(near(sampling.v[0] + x * sampling.v[1] + y * sampling.v[2], v));
}

int main() {
    int w = 0, h = 0;
    DirectImageSampling sampling;
    DirectViewportState state;
    CHECK(ComputeDirectViewport(state, 800, 600, w, h, sampling));
    CHECK(w == 800 && h == 600);
    point(sampling, 0, 0, 0, 0); point(sampling, 1, 1, 1, 1);
    state.scale = 2;
    state.x = 25.25; state.y = 10.5; state.width = 200.5; state.height = 100.25;
    state.destinationW = 640; state.destinationH = 480;
    CHECK(ComputeDirectViewport(state, 800, 600, w, h, sampling));
    CHECK(w == 640 && h == 480);
    point(sampling, 0, 0, 50.5 / 800, 21.0 / 600);
    point(sampling, 1, 1, 451.5 / 800, 221.5 / 600);
    const auto cropped = sampling;
    state.x += 1;
    CHECK(ComputeDirectViewport(state, 800, 600, w, h, sampling));
    CHECK(sampling != cropped); // geometry-only changes must invalidate scene idle

    // Golden TL/TR/BL mappings for all eight protocol transforms on a
    // non-square raw buffer, with scale=2 and a cropped source rectangle.
    const double golden[8][6] = {
        {.10, .10, .30, .10, .10, .50},
        {.10, .90, .10, .70, .50, .90},
        {.90, .90, .70, .90, .90, .50},
        {.90, .10, .90, .30, .50, .10},
        {.90, .10, .70, .10, .90, .50},
        {.10, .10, .10, .30, .50, .10},
        {.10, .90, .30, .90, .10, .50},
        {.90, .90, .90, .70, .50, .90}
    };
    for (int transform = 0; transform < 8; ++transform) {
        state = {}; state.transform = transform; state.scale = 2;
        const int width = (transform & 1) ? 300 : 400, height = (transform & 1) ? 400 : 300;
        state.x = width * .1; state.y = height * .1;
        state.width = width * .2; state.height = height * .4;
        CHECK(ComputeDirectViewport(state, 800, 600, w, h, sampling));
        CHECK(w == width / 5 && h == height * 2 / 5);
        point(sampling, 0, 0, golden[transform][0], golden[transform][1]);
        point(sampling, 1, 0, golden[transform][2], golden[transform][3]);
        point(sampling, 0, 1, golden[transform][4], golden[transform][5]);
    }
    state = {}; state.width = 100.5; state.height = 80;
    CHECK(!ComputeDirectViewport(state, 800, 600, w, h, sampling));
    state.destinationW = 100; state.destinationH = 80;
    CHECK(ComputeDirectViewport(state, 800, 600, w, h, sampling));
    state.x = 750;
    CHECK(!ComputeDirectViewport(state, 800, 600, w, h, sampling));
    state = {}; state.scale = 0;
    CHECK(!ComputeDirectViewport(state, 800, 600, w, h, sampling));
    state = {}; state.transform = 8;
    CHECK(!ComputeDirectViewport(state, 800, 600, w, h, sampling));
    state = {}; state.destinationW = 100;
    CHECK(!ComputeDirectViewport(state, 800, 600, w, h, sampling));
    state = {}; state.width = 100; state.height = -1;
    CHECK(!ComputeDirectViewport(state, 800, 600, w, h, sampling));
    state = {}; state.x = std::numeric_limits<double>::quiet_NaN(); state.width = 100; state.height = 80;
    CHECK(!ComputeDirectViewport(state, 800, 600, w, h, sampling));
    state = {};
    CHECK(!ComputeDirectViewport(state, 0, 600, w, h, sampling));
    std::printf("direct_viewport: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
