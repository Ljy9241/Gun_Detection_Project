#include "normal_wifi_window.h"
#include "low_power_motion_wake.h"
#include <cassert>
#include <initializer_list>

int main() {
    NormalWifiWindow w;
    auto wake = [&](uint32_t t) { return w.request(MotionDirection::Vertical, false, false, false, t); };
    auto tick = [&](uint32_t t, bool connected) { return w.update(t, connected, 120000, 60000); };
    assert(!w.active());
    assert(!w.request(MotionDirection::Vertical, true, false, false, 0));
    for (bool alwaysOn : {false, true}) {
        LowPowerMotionWake gate;
        MotionEvent e; e.direction = MotionDirection::Vertical;
        assert(!gate.shouldExit(1000, true, false, nullptr));
        assert(!gate.shouldExit(901000, false, true, nullptr));
        assert(!gate.shouldExit(1601000, false, false, &e));
        assert(w.request(e.direction, alwaysOn, true, false, 0));
        assert(tick(1000, true) && tick(120999, false) && !tick(121000, false));
        assert(!gate.shouldExit(2000000, true, false, nullptr));
        assert(gate.shouldExit(2300000, false, false, nullptr)); // Other motion still wakes.
        assert(!w.request(MotionDirection::Horizontal, alwaysOn, true, false, 0));
        assert(!w.request(MotionDirection::Vertical, alwaysOn, true, true, 0));
    }
    assert(!w.request(MotionDirection::Vertical, false, false, true, 0));
    assert(!w.request(MotionDirection::Horizontal, false, false, false, 0));
    assert(!w.request(MotionDirection::Unknown, false, false, false, 0));
    assert(!tick(0, true));
    assert(wake(100));
    assert(tick(59000, false));
    assert(tick(60000, true));
    assert(tick(100000, false));
    assert(tick(179999, true));
    assert(!tick(180000, true));
    assert(!tick(190000, false));
    assert(wake(200000));
    assert(!tick(260000, false));
    assert(wake(300000) && tick(300000, true));
    assert(wake(400000) && tick(400000, true));
    assert(tick(519999, true) && !tick(520000, true));
    assert(wake(UINT32_MAX - 99) && w.update(UINT32_MAX - 99, true, 1000, 500));
    assert(w.update(899, true, 1000, 500) && !w.update(900, true, 1000, 500));
    assert(wake(1000));
    w.cancel();
    assert(!tick(1001, true));
}
