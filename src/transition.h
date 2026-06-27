#pragma once

// A transition to play when switching wallpapers.
// type: <0 instant (no animation), 0 fade, 1 wipe, 2 grow, 3 slide/push, 4 shrink,
//       5 wave, 6 pixelate, 7 blinds, 8 radial.
struct Transition {
    int type = 0;
    int durationMs = 600;
    float angle = 0.0f; // wipe/slide direction (radians)
    float cx = 0.5f;    // grow/shrink centre (0..1)
    float cy = 0.5f;
};

// A transition choice parsed from config. For slide, slideSide selects the side the
// new image enters from: -1 = random cardinal, 0 left, 1 right, 2 top, 3 bottom.
// type == -2 marks an unrecognised name (skipped by the config loader).
struct TransitionSpec {
    int type = 0;
    int slideSide = -1;
};
