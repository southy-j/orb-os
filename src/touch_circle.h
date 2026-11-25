#pragma once
// A finger sliding around the round display, in place of the external encoder.
// Clockwise and counter-clockwise travel become the same detents knob::takeDelta()
// already reports. A tap with no travel is a press. Holding still for the encoder's
// long-press time is the recovery reboot.
namespace touch_circle {
    void begin();                 // after imu_begin(), which owns Wire.begin()
    bool poll();                  // true while a finger is down
}
