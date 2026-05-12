#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "lv_conf.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

void setup()
{
    Serial.begin(115200);
    Serial.println("Initializing board");

#if 0
    Board *board = new Board();
    board->init();

#if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
#endif
#endif

    assert(board->begin());

    Serial.println("Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    Serial.println("Creating UI");
    lvgl_port_lock(-1);

    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "hello world");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_30, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lvgl_port_unlock();
#endif
}

void loop()
{
    Serial.println("IDLE loop");
    delay(1000);
}
