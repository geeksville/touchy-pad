#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "lv_conf.h"
#include "trackpad_widget.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static Board         *g_board    = nullptr;
static TrackpadWidget *g_trackpad = nullptr;

void setup()
{
    Serial.begin(115200);
    Serial.println("Initializing board");

    g_board = new Board();
    g_board->init();

#if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = g_board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(g_board->getLCD()->getFrameWidth() * 10);
    }
#endif
#endif

    assert(g_board->begin());

    Serial.println("Initializing LVGL");
    // Pass nullptr for touch — TrackpadWidget reads touch directly to support multitouch
    lvgl_port_init(g_board->getLCD(), nullptr);

    Serial.println("Creating UI");
    lvgl_port_lock(-1);
    g_trackpad = new TrackpadWidget(g_board->getTouch(), lv_scr_act());
    lvgl_port_unlock();

    g_trackpad->begin();  // starts USB HID
    Serial.println("Ready");
}

void loop()
{
    g_trackpad->poll();
    delay(10);
}

