#pragma once

// A transition to play when switching wallpapers.
// type: <0 instant (no animation), 0 fade, 1 wipe, 2 grow, 3 slide/push.
struct Transition {
    int type = 0;
    int durationMs = 600;
    float angle = 0.0f; // wipe/slide direction (radians)
    float cx = 0.5f;    // grow centre (0..1)
    float cy = 0.5f;
};
