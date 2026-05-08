/*
   Copyright 2021 Scott Bezek and the splitflap contributors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include "display_task.h"

#include <WiFi.h>
#include <time.h>

#include "../core/common.h"
#include "../core/semaphore_guard.h"

#include "display_layouts.h"

// ReedBoard mod: convert WiFi RSSI (dBm) to a 0–100% strength reading.
// -50 dBm or stronger → 100%; -100 dBm or weaker → 0%; linear in between.
static inline int rssiToPercent(int rssi) {
    int p = 2 * (rssi + 100);
    if (p < 0) return 0;
    if (p > 100) return 100;
    return p;
}

DisplayTask::DisplayTask(SplitflapTask& splitflap_task, const uint8_t task_core) : Task("Display", 6000, 1, task_core), splitflap_task_(splitflap_task), semaphore_(xSemaphoreCreateMutex()) {
    assert(semaphore_ != NULL);
    xSemaphoreGive(semaphore_);
}

DisplayTask::~DisplayTask() {
  if (semaphore_ != NULL) {
    vSemaphoreDelete(semaphore_);
  }
}


static const int32_t X_OFFSET = 10;
static const int32_t Y_OFFSET = 28;  // ReedBoard mod: shifted from 10 to make room for top status bar

void DisplayTask::run() {
    tft_.begin();
    tft_.invertDisplay(1);
    tft_.setRotation(1);

    tft_.setTextFont(0);
    tft_.setTextColor(0xFFFF, TFT_BLACK);

    tft_.fillScreen(TFT_BLACK);

    // Automatically scale display based on DISPLAY_COLUMNS (see display_layouts.h)
    int32_t module_width = 20;
    int32_t module_height = 26;
    uint8_t module_text_size = 3;

    uint8_t rows = ((NUM_MODULES + DISPLAY_COLUMNS - 1) / DISPLAY_COLUMNS);

    if (DISPLAY_COLUMNS > 16 || rows > 6) {
        module_width = 7;
        module_height = 10;
        module_text_size = 1;
    } else if (DISPLAY_COLUMNS > 10 || rows > 4) {
        module_width = 14;
        module_height = 18;
        module_text_size = 2;
    }

    tft_.fillRect(X_OFFSET, Y_OFFSET, DISPLAY_COLUMNS * (module_width + 1) + 1, rows * (module_height + 1) + 1, 0x2104);

    uint8_t module_row, module_col;
    int32_t module_x, module_y;
    SplitflapState last_state = {};
    String last_messages[countof(messages_)] = {};
    while(1) {
        SplitflapState state = splitflap_task_.getState();
        if (state != last_state) {
            tft_.setTextSize(module_text_size);
            for (uint8_t i = 0; i < NUM_MODULES; i++) {
                SplitflapModuleState& s = state.modules[i];
                if (s == last_state.modules[i]) {
                    continue;
                }

                uint16_t background = 0x0000;
                uint16_t foreground = 0xFFFF;

                bool blink = (millis() / 400) % 2;

                char c;
                switch (s.state) {
                    case NORMAL:
                        c = flaps[s.flap_index];
                        if (s.moving) {
                            // use a dimmer color when moving
                            foreground = 0x6b4d;
                        }

                        // Color flaps render as filled colored squares (no letter) on the LCD
                        // status grid, instead of the literal lowercase 'g'/'r'/'y'/'p'/'w' chars.
                        // RGB565 colors picked to roughly match the physical Bezek Labs flap colors.
                        if (c == 'g') {        // green
                            c = ' ';
                            background = 0x46a0;
                        } else if (c == 'r') { // red
                            c = ' ';
                            background = 0xC000;
                        } else if (c == 'y') { // yellow
                            c = ' ';
                            background = 0xffe0;
                        } else if (c == 'p') { // purple
                            c = ' ';
                            background = 0xd938;
                        } else if (c == 'w') { // white
                            c = ' ';
                            background = 0xFFFF;
                        }
                        break;
                    case PANIC:
                        c = '~';
                        background = blink ? 0xD000 : 0;
                        break;
                    case STATE_DISABLED:
                        c = '*';
                        break;
                    case LOOK_FOR_HOME:
                        c = '?';
                        background = blink ? 0x6018 : 0;
                        break;
                    case SENSOR_ERROR:
                        c = ' ';
                        background = blink ? 0xD461 : 0;
                        break;
                    default:
                        c = ' ';
                        break;
                }
                getLayoutPosition(i, &module_row, &module_col);

                // Add 1 to width/height as a separator line between modules
                module_x = X_OFFSET + 1 + module_col * (module_width + 1);
                module_y = Y_OFFSET + 1 + module_row * (module_height + 1);

                tft_.setTextColor(foreground, background);
                tft_.fillRect(module_x, module_y, module_width, module_height, background);
                tft_.setCursor(module_x + 1, module_y + 2);
                tft_.printf("%c", c);
            }
            last_state = state;
        }

        // ReedBoard top status bar: redraw at most once per second.
        unsigned long now_ms = millis();
        if (now_ms - last_status_redraw_ms_ >= 1000) {
            last_status_redraw_ms_ = now_ms;

            struct tm timeinfo;
            bool have_time = getLocalTime(&timeinfo, 0);

            // What changed this tick?
            int minute_now = have_time ? (timeinfo.tm_hour * 60 + timeinfo.tm_min) : -1;
            bool wifi_connected = (WiFi.status() == WL_CONNECTED);
            int rssi = wifi_connected ? WiFi.RSSI() : -127;
            int wifi_pct = wifi_connected ? rssiToPercent(rssi) : -1;
            int rssi_bracket;
            if      (!wifi_connected) rssi_bracket = 0; // not connected → grey
            else if (rssi >= -55)     rssi_bracket = 3; // strong         → green
            else if (rssi >= -70)     rssi_bracket = 2; // medium         → yellow
            else                      rssi_bracket = 1; // weak           → red

            bool minute_changed = (minute_now != last_drawn_minute_);
            bool rssi_changed   = (rssi_bracket != last_drawn_rssi_bracket_);
            bool pct_changed    = (wifi_pct != last_drawn_wifi_pct_);

            if (minute_changed || rssi_changed || pct_changed) {
                // Repaint the whole status bar to keep alignment simple.
                tft_.fillRect(0, 0, tft_.width(), 24, TFT_BLACK);
                tft_.setTextSize(2);

                // Time on left
                tft_.setTextColor(TFT_WHITE, TFT_BLACK);
                if (have_time) {
                    int h12 = timeinfo.tm_hour % 12; if (h12 == 0) h12 = 12;
                    const char* ampm = (timeinfo.tm_hour >= 12) ? "PM" : "AM";
                    char tbuf[16];
                    snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, timeinfo.tm_min, ampm);
                    tft_.drawString(tbuf, 2, 4);
                } else {
                    tft_.drawString("--:-- --", 2, 4);
                }

                // WiFi strength % on right, color-coded by RSSI bracket
                uint16_t wifi_color;
                switch (rssi_bracket) {
                    case 3: wifi_color = TFT_GREEN;  break;
                    case 2: wifi_color = TFT_YELLOW; break;
                    case 1: wifi_color = TFT_RED;    break;
                    default: wifi_color = 0x8410;    break; // dim grey when not connected
                }
                tft_.setTextColor(wifi_color, TFT_BLACK);
                char wbuf[16];
                if (wifi_pct < 0) {
                    snprintf(wbuf, sizeof(wbuf), "WIFI --");
                } else {
                    snprintf(wbuf, sizeof(wbuf), "WIFI %d%%", wifi_pct);
                }
                int ww = strlen(wbuf) * 12;
                tft_.drawString(wbuf, tft_.width() - ww - 2, 4);

                // Thin divider under the status bar
                tft_.drawFastHLine(0, 25, tft_.width(), 0x2104);

                last_drawn_minute_       = minute_now;
                last_drawn_rssi_bracket_ = rssi_bracket;
                last_drawn_wifi_pct_     = wifi_pct;
            }
        }

        const int message_height = 20;     // was 10 — bigger lines = readable from across the bench
        const int message_text_size = 2;   // was 1 — 12x16 px chars; two lines × 20 px = 40 px bottom band
        bool redraw_messages = false;
        {
            SemaphoreGuard lock(semaphore_);
            for (uint8_t i = 0; i < countof(messages_); i++) {
                if (messages_[i] != last_messages[i]) {
                    redraw_messages = true;
                    last_messages[i] = messages_[i];
                }
            }
        }
        if (redraw_messages) {
            tft_.setTextSize(message_text_size);
            tft_.setTextColor(TFT_WHITE, TFT_BLACK);
            tft_.fillRect(0, tft_.height() - message_height * countof(messages_), tft_.width(), message_height * countof(messages_), TFT_BLACK);
            for (uint8_t i = 0; i < countof(messages_); i++) {
                int y = tft_.height() - message_height * (countof(messages_) - i);
                tft_.drawString(last_messages[i], 2, y);
            }
        }

        delay(10);
    }
}

void DisplayTask::setMessage(uint8_t i, String message) {
    SemaphoreGuard lock(semaphore_);
    assert(i < countof(messages_));
    messages_[i] = message;
}
