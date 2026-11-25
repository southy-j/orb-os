#include "alarm_view.h"
#include "app_shell.h"
#include "app_theme.h"
#include "config.h"
#include "custom_sprite.h"
#include "orb_time.h"
#include "theme_font.h"
#include "theme_style.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(ARDUINO)
#include <Preferences.h>
#include "audio.h"
#endif

namespace {

enum Mode { Watch, SetHour, SetMin, ConfirmOff, Ringing };

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_status = nullptr;
lv_obj_t *s_hint = nullptr;
lv_obj_t *s_digital = nullptr;
lv_obj_t *s_hourHand = nullptr;
lv_obj_t *s_minHand = nullptr;
lv_obj_t *s_secHand = nullptr;
bool s_themed = false;
lv_obj_t *s_themeHand[3] = { nullptr, nullptr, nullptr };
lv_obj_t *s_themeShadow[3] = { nullptr, nullptr, nullptr };
lv_img_dsc_t s_themeDsc[3];
lv_img_dsc_t s_shadowDsc[3];
lv_img_dsc_t s_plateDsc;
lv_point_t s_hourPts[2];
lv_point_t s_minPts[2];
lv_point_t s_secPts[2];
lv_timer_t *s_timer = nullptr;

int  s_hour = 7;
int  s_min = 0;
bool s_armed = false;
Mode s_mode = Watch;
bool s_ringing = false;
time_t s_snoozeUntil = 0;
int  s_firedKey = -1;
uint32_t s_lastBeep = 0;

void save() {
#if defined(ARDUINO)
    Preferences p;
    if (!p.begin("capsuleradar", false)) return;
    p.putUChar("almH", (uint8_t)s_hour);
    p.putUChar("almM", (uint8_t)s_min);
    p.putBool("almOn", s_armed);
    p.end();
#endif
}

void load() {
#if defined(ARDUINO)
    Preferences p;
    if (!p.begin("capsuleradar", true)) return;
    s_hour = p.getUChar("almH", 7);
    s_min = p.getUChar("almM", 0);
    s_armed = p.getBool("almOn", false);
    p.end();
#endif
    if (s_hour > 23) s_hour = 7;
    if (s_min > 59) s_min = 0;
}

void hand(lv_point_t pts[2], float deg, float length) {
    const float rad = deg * (float)M_PI / 180.0f;
    pts[0].x = SCREEN_CX;
    pts[0].y = SCREEN_CY;
    pts[1].x = (lv_coord_t)(SCREEN_CX + sinf(rad) * length);
    pts[1].y = (lv_coord_t)(SCREEN_CY - cosf(rad) * length);
}

void setAngle(lv_obj_t *img, float deg) {
    if (!img) return;
    int a = (int)lroundf(deg * 10.0f) % 3600;
    if (a < 0) a += 3600;
    lv_img_set_angle(img, (int16_t)a);
}

void paintHands(int hour, int minute, int second, bool showSecond) {
    const float mins = (float)minute + (float)second / 60.0f;
    const float hourDeg = ((hour % 12) + mins / 60.0f) * 30.0f;
    const float minDeg = mins * 6.0f;
    const float secDeg = second * 6.0f;
    if (s_themed) {
        const float deg[3] = { hourDeg, minDeg, secDeg };
        for (int k = 0; k < 3; ++k) {
            const float d = (k == 2 && !showSecond) ? 0.0f : deg[k];
            setAngle(s_themeShadow[k], d);
            setAngle(s_themeHand[k], d);
            if (k == 2) {
                const lv_opa_t opa = showSecond ? LV_OPA_COVER : LV_OPA_TRANSP;
                if (s_themeHand[2]) lv_obj_set_style_img_opa(s_themeHand[2], opa, 0);
                if (s_themeShadow[2]) lv_obj_set_style_img_opa(s_themeShadow[2], opa, 0);
            }
        }
        return;
    }
    hand(s_hourPts, hourDeg, 78.0f);
    hand(s_minPts, minDeg, 118.0f);
    hand(s_secPts, secDeg, 140.0f);
    if (s_hourHand) lv_line_set_points(s_hourHand, s_hourPts, 2);
    if (s_minHand)  lv_line_set_points(s_minHand, s_minPts, 2);
    if (s_secHand)  lv_obj_set_style_line_opa(s_secHand, showSecond ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (s_secHand)  lv_line_set_points(s_secHand, s_secPts, 2);
}

lv_obj_t *placeSprite(lv_obj_t *parent, lv_img_dsc_t *dsc, const uint8_t *data,
                      int w, int h, int pivotX, int pivotY, int centerX, int centerY) {
    if (!data || w <= 0 || h <= 0) return nullptr;
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.w = (uint16_t)w;
    dsc->header.h = (uint16_t)h;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc->data_size = (uint32_t)w * h * 3;
    dsc->data = data;
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, dsc);
    lv_img_set_pivot(img, pivotX, pivotY);
    lv_img_set_antialias(img, true);
    lv_obj_set_pos(img, centerX - pivotX, centerY - pivotY);
    return img;
}

void paint() {
    const AppPalette &pal = app_theme::palette();
    struct tm ti;
    const bool have = orb_local_time(&ti);
    const bool setting = s_mode == SetHour || s_mode == SetMin;
    if (setting) paintHands(s_hour, s_min, 0, false);
    else if (have) paintHands(ti.tm_hour, ti.tm_min, ti.tm_sec, true);
    else paintHands(0, 0, 0, false);

    char digital[16];
    snprintf(digital, sizeof(digital), "%02d:%02d", s_hour, s_min);
    if (s_digital) lv_label_set_text(s_digital, digital);

    const char *status = "Wecker aus";
    const char *hint = "Tippen: stellen";
    lv_color_t statusColor = pal.soft;
    if (s_ringing || s_mode == Ringing) {
        status = "Schlummer";
        hint = "Tippen: Schlummer    Drehen: aus";
        statusColor = ((lv_tick_get() / 400) & 1) ? pal.accent : pal.ink;
    } else if (s_mode == SetHour) {
        status = "Stunde";
        hint = "Drehen: Stunde    Tippen: weiter";
        statusColor = pal.accent;
    } else if (s_mode == SetMin) {
        status = "Minute";
        hint = "Drehen: Minute    Tippen: an";
        statusColor = pal.accent;
    } else if (s_mode == ConfirmOff) {
        status = "Ausschalten?";
        hint = "Tippen: aus    Drehen: zurueck";
        statusColor = pal.accent;
    } else if (s_snoozeUntil && time(nullptr) < s_snoozeUntil) {
        struct tm st;
        localtime_r(&s_snoozeUntil, &st);
        static char buf[32];
        snprintf(buf, sizeof(buf), "Schlummer bis %02d:%02d", st.tm_hour, st.tm_min);
        status = buf;
        hint = "Tippen: stellen";
        statusColor = pal.accent;
    } else if (s_armed) {
        status = "Wecker an";
        hint = "Tippen: aus    Drehen: stellen";
        statusColor = pal.accent;
    }
    if (s_status) {
        lv_label_set_text(s_status, status);
        lv_obj_set_style_text_color(s_status, statusColor, 0);
    }
    if (s_hint) lv_label_set_text(s_hint, hint);
}

void startRing() {
    s_ringing = true;
    s_mode = Ringing;
    s_lastBeep = 0;
#if defined(ARDUINO)
    audio_play(AUDIO_ALERT);
    s_lastBeep = millis();
#endif
    if (app_shell::index() != app_shell::APP_ALARM) app_shell::selectApp(app_shell::APP_ALARM);
    paint();
}

void stopRing() {
    s_ringing = false;
    if (s_mode == Ringing) s_mode = Watch;
}

void deactivate() {
    s_armed = false;
    s_snoozeUntil = 0;
    stopRing();
    s_mode = Watch;
    save();
    paint();
}

void tick(lv_timer_t *) {
    struct tm ti;
    const bool have = orb_local_time(&ti);
    if (s_armed && have && !s_ringing) {
        const time_t now = time(nullptr);
        if (s_snoozeUntil && now >= s_snoozeUntil) {
            s_snoozeUntil = 0;
            startRing();
        } else if (!s_snoozeUntil) {
            const int key = ti.tm_yday * 1440 + ti.tm_hour * 60 + ti.tm_min;
            if (ti.tm_hour == s_hour && ti.tm_min == s_min && key != s_firedKey) {
                s_firedKey = key;
                startRing();
            }
        }
    }
    if (s_ringing) {
#if defined(ARDUINO)
        if (millis() - s_lastBeep >= 2000) {
            audio_play(AUDIO_ALERT);
            s_lastBeep = millis();
        }
#endif
        if (app_shell::count() > 0 && app_shell::index() != app_shell::APP_ALARM)
            app_shell::selectApp(app_shell::APP_ALARM);
    }
    if (s_scr && lv_scr_act() == s_scr) paint();
}

lv_obj_t *makeHand(lv_obj_t *parent, lv_point_t *pts, int width, lv_color_t color) {
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_set_pos(line, 0, 0);
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

}  // namespace

void alarmview::init() {
    load();
    const AppPalette &pal = app_theme::palette();
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, pal.bg, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    const uint16_t *plate = custom_plate();
    if (plate) {
        s_themed = true;
        memset(&s_plateDsc, 0, sizeof(s_plateDsc));
        s_plateDsc.header.w = SCREEN_W;
        s_plateDsc.header.h = SCREEN_H;
        s_plateDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_plateDsc.data_size = (uint32_t)SCREEN_W * SCREEN_H * 2;
        s_plateDsc.data = (const uint8_t *)plate;
        lv_obj_t *bg = lv_img_create(s_scr);
        lv_img_set_src(bg, &s_plateDsc);
        lv_obj_center(bg);
        const theme_style::Clock &ck = theme_style::clock();
        for (int k = 0; k < 3; ++k) {
            const theme_style::Hand &hd = ck.hand[k];
            if (!hd.show) continue;
            if (ck.shadowOn) {
                CustomSprite sh = custom_shadow(k);
                s_themeShadow[k] = placeSprite(s_scr, &s_shadowDsc[k], sh.data, sh.w, sh.h,
                                               hd.pivotX, hd.pivotY,
                                               hd.centerX + ck.shadowDX, hd.centerY + ck.shadowDY);
            }
            CustomSprite spr = custom_hand(k);
            s_themeHand[k] = placeSprite(s_scr, &s_themeDsc[k], spr.data, spr.w, spr.h,
                                         hd.pivotX, hd.pivotY, hd.centerX, hd.centerY);
        }
    }

    if (!s_themed) {
    lv_obj_t *ring = lv_obj_create(s_scr);
    lv_obj_set_size(ring, 400, 400);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, pal.hairline, 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 60; ++i) {
        const float deg = i * 6.0f * (float)M_PI / 180.0f;
        const float inner = (i % 5 == 0) ? 158.0f : 176.0f;
        const float outer = 190.0f;
        static lv_point_t ticks[60][2];
        ticks[i][0].x = (lv_coord_t)(SCREEN_CX + sinf(deg) * inner);
        ticks[i][0].y = (lv_coord_t)(SCREEN_CY - cosf(deg) * inner);
        ticks[i][1].x = (lv_coord_t)(SCREEN_CX + sinf(deg) * outer);
        ticks[i][1].y = (lv_coord_t)(SCREEN_CY - cosf(deg) * outer);
        lv_obj_t *mark = lv_line_create(s_scr);
        lv_line_set_points(mark, ticks[i], 2);
        lv_obj_set_style_line_width(mark, (i % 5 == 0) ? 4 : 2, 0);
        lv_obj_set_style_line_color(mark, (i % 5 == 0) ? pal.ink : pal.dim, 0);
        lv_obj_set_style_line_rounded(mark, true, 0);
    }

    s_secHand = makeHand(s_scr, s_secPts, 2, pal.dim);
    s_minHand = makeHand(s_scr, s_minPts, 5, pal.ink);
    s_hourHand = makeHand(s_scr, s_hourPts, 7, pal.accent);

    lv_obj_t *cap = lv_obj_create(s_scr);
    lv_obj_set_size(cap, 16, 16);
    lv_obj_set_pos(cap, SCREEN_CX - 8, SCREEN_CY - 8);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, pal.accent, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    }

    s_digital = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_digital, s_themed ? theme_font::clock_text1() : &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_digital, pal.ink, 0);
    lv_obj_align(s_digital, LV_ALIGN_CENTER, 0, -78);

    s_status = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_status, pal.soft, 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 78);

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint, pal.dim, 0);
    lv_obj_set_width(s_hint, 300);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 118);

    paint();
    s_timer = lv_timer_create(tick, 250, nullptr);
}

lv_obj_t *alarmview::screen() { return s_scr; }

void alarmview::onEnter() { paint(); }

void alarmview::onPress() {
    if (s_ringing) {
        s_snoozeUntil = time(nullptr) + 5 * 60;
        s_ringing = false;
        s_mode = Watch;
        paint();
        return;
    }
    switch (s_mode) {
    case Watch:
        s_mode = s_armed ? ConfirmOff : SetHour;
        break;
    case SetHour:
        s_mode = SetMin;
        break;
    case SetMin:
        s_armed = true;
        s_snoozeUntil = 0;
        s_firedKey = -1;
        s_mode = Watch;
        save();
        break;
    case ConfirmOff:
        deactivate();
        return;
    case Ringing:
        break;
    }
    paint();
}

void alarmview::onTurn(int delta) {
    if (delta == 0) return;
    if (s_ringing) { deactivate(); return; }
    if (s_mode == ConfirmOff) { s_mode = Watch; paint(); return; }
    if (s_mode == Watch) {
        if (s_armed) s_mode = SetHour;
        else return;
    }
    if (s_mode == SetHour) {
        s_hour = (s_hour + delta) % 24;
        if (s_hour < 0) s_hour += 24;
    } else if (s_mode == SetMin) {
        s_min = (s_min + delta) % 60;
        if (s_min < 0) s_min += 60;
    }
    paint();
}
