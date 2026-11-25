#include "touch_circle.h"
#include "touch_cst9217.h"
#include "knob.h"
#include "config.h"
#include "display.h"
#include <Arduino.h>
#include <math.h>

namespace {

constexpr float kDeadRadiusPx = 48.0f;          // angle is noise this close to the centre
constexpr float kDetentRad    = 15.0f * (float)M_PI / 180.0f;  // one encoder click
constexpr float kTapSlopRad   = 8.0f * (float)M_PI / 180.0f;   // less than this is still a tap
constexpr float kTapMovePx    = 40.0f;
constexpr uint32_t kTapMaxMs  = 700;
constexpr uint32_t kLongMs    = 8000;           // matches knob.cpp LONG_PRESS_MS

bool     s_down = false;
bool     s_longFired = false;
int      s_emitted = 0;
float    s_accum = 0;
float    s_abs = 0;
float    s_lastAng = 0;
int      s_x0 = 0, s_y0 = 0;
int      s_x = 0, s_y = 0;
uint32_t s_downAt = 0;

float wrapPi(float d) {
    while (d > (float)M_PI)  d -= 2.0f * (float)M_PI;
    while (d < -(float)M_PI) d += 2.0f * (float)M_PI;
    return d;
}

}  // namespace

void touch_circle::begin() {
    touch_begin();
    Serial.println("[touch] circle input: clockwise = knob right, counter-clockwise = knob left, tap = press");
}

bool touch_circle::poll() {
    uint16_t x = 0, y = 0;
    const bool pressed = touch_read(&x, &y);
    if (pressed) {
        display::noteActivity();
        const float dx = (float)x - (float)SCREEN_CX;
        const float dy = (float)y - (float)SCREEN_CY;
        const float r = sqrtf(dx * dx + dy * dy);
        const float ang = atan2f(dy, dx);   // y grows downward, so increasing angle is clockwise
        if (!s_down) {
            s_down = true;
            s_longFired = false;
            s_emitted = 0;
            s_accum = 0;
            s_abs = 0;
            s_lastAng = ang;
            s_x0 = s_x = (int)x;
            s_y0 = s_y = (int)y;
            s_downAt = millis();
        } else {
            s_x = (int)x;
            s_y = (int)y;
            if (!s_longFired && s_emitted == 0 && (millis() - s_downAt) >= kLongMs) {
                s_longFired = true;
                knob::injectLongPress();
            }
            if (r >= kDeadRadiusPx) {
                const float d = wrapPi(ang - s_lastAng);
                s_lastAng = ang;
                s_accum += d;
                s_abs += fabsf(d);
                while (s_accum >= kDetentRad) {
                    s_accum -= kDetentRad;
                    knob::injectDetent(+1);
                    s_emitted++;
                }
                while (s_accum <= -kDetentRad) {
                    s_accum += kDetentRad;
                    knob::injectDetent(-1);
                    s_emitted++;
                }
            }
        }
        return true;
    }

    if (s_down) {
        s_down = false;
        const float travel = sqrtf((float)((s_x - s_x0) * (s_x - s_x0) + (s_y - s_y0) * (s_y - s_y0)));
        const uint32_t held = millis() - s_downAt;
        if (!s_longFired && s_emitted == 0 && s_abs < kTapSlopRad && travel < kTapMovePx && held < kTapMaxMs) {
            knob::injectTap();
            Serial.println("[touch] tap -> press");
        } else if (s_emitted) {
            Serial.printf("[touch] circle %d detent%s\n", s_emitted, s_emitted == 1 ? "" : "s");
        }
    }
    return false;
}
