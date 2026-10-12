// screen_automation.cpp
// Depth-ordered input automation (Windows, C++17).
//
// Build (MSVC, from a Developer Command Prompt):
//   cl /EHsc /std:c++17 /O2 screen_automation.cpp user32.lib
//
// Controls:  F8 = run the sequence once   |   ESC = abort / quit
//
// The detector is behind an interface. MockDetector returns fake characters so you
// can test the ordering + mouse logic now. Later, replace it with a class that
// captures the screen and runs your ONNX model.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- data types

struct Detection {
    float cx, cy;   // center of the character in screen pixels
    float depth;    // z value: SMALLER = CLOSER to the camera
    float conf;     // detection confidence 0..1
};

class IDetector {
public:
    virtual ~IDetector() = default;
    virtual std::vector<Detection> detect() = 0;   // called once per frame
};

// ------------------------------------------------------------- test detector

class MockDetector : public IDetector {
public:
    std::vector<Detection> detect() override {
        const float w = (float)GetSystemMetrics(SM_CXSCREEN);
        const float h = (float)GetSystemMetrics(SM_CYSCREEN);
        // Expected visiting order: depth 2 -> 5 -> 8
        return {
            {0.30f * w, 0.40f * h, 5.0f, 0.90f},
            {0.60f * w, 0.50f * h, 2.0f, 0.95f},
            {0.50f * w, 0.70f * h, 8.0f, 0.88f},
        };
    }
};

// --------------------------------------------------------------- mouse output

static bool abortRequested() { return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; }

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static void setCursorAbs(int x, int y) {
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(x * 65535.0 / (sw - 1));
    in.mi.dy = (LONG)(y * 65535.0 / (sh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(INPUT));
}

// Smooth glide from the current cursor position to (x, y). Returns false if aborted.
static bool moveMouseSmooth(int x, int y, int steps = 25, int stepDelayMs = 4) {
    POINT p;
    GetCursorPos(&p);
    for (int i = 1; i <= steps; ++i) {
        if (abortRequested()) return false;
        float t = (float)i / steps;
        t = t * t * (3.0f - 2.0f * t);                 // smoothstep easing
        setCursorAbs((int)(p.x + (x - p.x) * t), (int)(p.y + (y - p.y) * t));
        sleepMs(stepDelayMs);
    }
    return true;
}

static void leftClick() {
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

// ------------------------------------------------------------ target selection

struct Config {
    float minConfidence = 0.5f;    // ignore weak detections
    float revisitRadius = 60.0f;   // pixels; a target this close to a done one counts as done
    int   maxTargets    = 10;      // safety cap per run
    int   settleMs      = 150;     // wait after each action so the screen can update
};

static bool alreadyDone(const Detection& d, const std::vector<POINT>& done, float radius) {
    for (const POINT& p : done) {
        if (std::hypot(d.cx - p.x, d.cy - p.y) < radius) return true;
    }
    return false;
}

// Rule: always act on the CLOSEST remaining character (smallest z),
// then the next further one, and so on. Re-detects after every action.
static void runSequence(IDetector& detector, const Config& cfg) {
    std::vector<POINT> done;

    for (int n = 0; n < cfg.maxTargets; ++n) {
        if (abortRequested()) { std::puts("Aborted."); return; }

        std::vector<Detection> dets = detector.detect();
        dets.erase(std::remove_if(dets.begin(), dets.end(), [&](const Detection& d) {
            return d.conf < cfg.minConfidence || alreadyDone(d, done, cfg.revisitRadius);
        }), dets.end());

        if (dets.empty()) { std::puts("No remaining targets."); return; }

        auto best = std::min_element(dets.begin(), dets.end(),
            [](const Detection& a, const Detection& b) { return a.depth < b.depth; });

        const int tx = (int)best->cx, ty = (int)best->cy;
        std::printf("Target %d: (%d, %d) depth=%.2f\n", n + 1, tx, ty, best->depth);

        if (!moveMouseSmooth(tx, ty)) { std::puts("Aborted."); return; }
        leftClick();                       // the "throw" action; swap for whatever your app needs
        done.push_back(POINT{tx, ty});
        sleepMs(cfg.settleMs);
    }
    std::puts("Reached maxTargets.");
}

// ------------------------------------------------------------------------ main

int main() {
    SetProcessDPIAware();                  // so coordinates match real screen pixels

    std::unique_ptr<IDetector> detector = std::make_unique<MockDetector>();
    Config cfg;

    std::puts("Press F8 to run, ESC to quit.");
    while (!abortRequested()) {
        if (GetAsyncKeyState(VK_F8) & 0x8000) {
            runSequence(*detector, cfg);
            while (GetAsyncKeyState(VK_F8) & 0x8000) sleepMs(10);   // wait for key release
        }
        sleepMs(20);
    }
    return 0;
}
