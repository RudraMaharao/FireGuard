#pragma once
struct Detection {
    int cls;
    float score;
    int x1, y1, x2, y2;
};
struct Verdict {
    static constexpr int MAX = 8;
    Detection det[MAX];
    int count = 0;
    float best_fire = 0, best_smoke = 0;
};
