#pragma once
#include <lvgl.h>

// Kid alarm clock. Analog face, same palette as the other screens.
// Turn is the touch circle (clockwise / counter-clockwise). Tap is the knob press.
namespace alarmview {
    void      init();
    lv_obj_t* screen();
    void      onPress();
    void      onTurn(int delta);
    void      onEnter();
}
