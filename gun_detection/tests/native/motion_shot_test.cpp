#include "motion_direction.h"
#include "shot_detector.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

static unsigned simulate(int axes, uint64_t step, bool spike = false) {
    MotionDirectionDetector detector;
    ShotDetector shots;
    unsigned horizontal = 0, vertical = 0, unknown = 0;
    GpsFix fix;
    fix.valid = true; fix.latitude = 31.2; fix.longitude = 121.5;
    for (uint64_t t = 0; t <= 9000000; t += step) {
        float v = t >= 3000000 && t < 4000000 ?
            500 * std::sin((t-3000000)/1000000.0 * 4 * 3.141592653589793) : 0;
        if (spike) v = t == 3000000 ? 1000 : 0;
        detector.processSample(static_cast<uint16_t>(1700 + ((axes & 1) ? v : 0)),
                               static_cast<uint16_t>(2050 + ((axes & 2) ? v : 0)),
                               static_cast<uint16_t>(1820 + ((axes & 4) ? v : 0)), t);
        MotionEvent event;
        if (detector.consumeEvent(event)) {
            if (event.direction == MotionDirection::Horizontal) ++horizontal;
            else if (event.direction == MotionDirection::Vertical) ++vertical;
            else ++unknown;
            shots.onMotionEvent(event);
            ShotEvent shot;
            const bool produced = shots.consumeShot(shot, fix);
            assert(produced == (event.direction == MotionDirection::Horizontal));
            if (produced) {
                assert(shot.gps.valid && shot.gps.latitude == fix.latitude);
                assert(shot.vibration.peakMagnitude == event.peak);
                assert(shot.voice.peakToPeak == 0);
                shots.onMotionEvent(event);  // Duplicate delivery cannot count twice.
                assert(!shots.consumeShot(shot, fix));
            }
        }
    }
    if (spike || axes == 0) assert(horizontal + vertical + unknown == 0);
    else if (axes == 4) assert(horizontal == 1 && vertical == 0 && unknown == 0);
    else if (axes == 1) assert(horizontal == 0 && vertical == 1 && unknown == 0);
    else assert(horizontal == 0 && vertical == 0 && unknown == 1);
    return shots.shotCount();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "replay") {
        MotionDirectionDetector detector;
        ShotDetector shots;
        uint64_t t;
        unsigned x, y, z, horizontal = 0, vertical = 0, unknown = 0, consumed = 0;
        while (std::cin >> t >> x >> y >> z) {
            detector.processSample(x, y, z, t);
            MotionEvent e;
            if (!detector.consumeEvent(e)) continue;
            if (e.direction == MotionDirection::Horizontal) ++horizontal;
            else if (e.direction == MotionDirection::Vertical) ++vertical;
            else ++unknown;
            shots.onMotionEvent(e);
            ShotEvent shot;
            if (shots.consumeShot(shot, GpsFix{})) ++consumed;
        }
        assert(consumed == shots.shotCount());
        std::cout << horizontal << ' ' << vertical << ' ' << unknown << ' ' << consumed << '\n';
        return 0;
    }
    for (uint64_t step : {1000ULL, 50000ULL}) {
        assert(simulate(4, step) == 1);
        assert(simulate(1, step) == 0);
        assert(simulate(2, step) == 0);
        assert(simulate(5, step) == 0);
        assert(simulate(0, step) == 0);
        assert(simulate(4, step, true) == 0);
    }
    MotionDirectionDetector detector;
    for (uint64_t t = 0; t <= 2500000; t += 50000) detector.processSample(1700,2050,1820,t);
    assert(detector.calibrated());
    detector.processSample(1700,2050,2400,2550000);
    detector.processSample(1700,2050,2400,2600000);
    detector.processSample(1700,2050,1820,4000000); // Gap discards partial motion.
    assert(!detector.calibrated());
    MotionEvent event;
    assert(!detector.consumeEvent(event));
    for (uint64_t t = 4050000; t <= 6500000; t += 50000) detector.processSample(1700,2050,1820,t);
    assert(detector.calibrated());
    detector.processSample(1700,2050,1820,1000); // Clock reversal.
    assert(!detector.calibrated());
    detector.reset();
    assert(!detector.consumeEvent(event));
    std::cout << "PASS: directions, 1kHz decimation, one-shot counting, rejection, GPS, gaps/reset\n";
}
