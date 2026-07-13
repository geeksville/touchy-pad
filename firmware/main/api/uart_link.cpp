// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware-UART host-API link — see uart_link.h.

#include "uart_link.h"

#if CONFIG_TOUCHY_PROTO_OVER_UART && CONFIG_TOUCHY_HAS_PROTO_UART

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tc_tag.h"

static const char *TAG = TOUCHY_TAG("uart_link");

#define PROTO_UART_NUM  ((uart_port_t)CONFIG_TOUCHY_PROTO_UART_NUM)
#define PROTO_UART_BAUD (CONFIG_TOUCHY_PROTO_UART_BAUD)

size_t UartLink::read_some(uint8_t *dst, size_t max)
{
    int n = uart_read_bytes(PROTO_UART_NUM, dst, max, 0);
    return n > 0 ? (size_t)n : 0;
}

bool UartLink::write_all(const uint8_t *p, size_t n)
{
    int w = uart_write_bytes(PROTO_UART_NUM, (const char *)p, n);
    return w == (int)n;
}

void UartLink::flush()
{
    uart_wait_tx_done(PROTO_UART_NUM, pdMS_TO_TICKS(100));
}

// Pumps UART rx events into the link's wake semaphore so the dispatcher
// task blocks (rather than polling) while waiting for the next frame.
static void uart_rx_pump_task(void *arg)
{
    UartLink *link = (UartLink *)arg;
    uart_event_t evt;
    for (;;) {
        if (xQueueReceive(link->evt_queue, &evt, portMAX_DELAY)) {
            if (evt.type == UART_DATA && link->rx_sem) {
                xSemaphoreGive(link->rx_sem);
            }
        }
    }
}

bool UartLink::init()
{
    uart_config_t cfg = {};
    cfg.baud_rate = PROTO_UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(PROTO_UART_NUM, HOST_API_RX_MAX * 2,
                                        HOST_API_TX_MAX * 2, 16,
                                        &evt_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(PROTO_UART_NUM, &cfg));
    // Apply board-specific pin mapping when the board sets non-default GPIOs.
    // UART_PIN_NO_CHANGE (-1) keeps the hardware default for that pin.
    if (CONFIG_TOUCHY_PROTO_UART_TXD != -1 || CONFIG_TOUCHY_PROTO_UART_RXD != -1) {
        ESP_ERROR_CHECK(uart_set_pin(PROTO_UART_NUM,
                                     CONFIG_TOUCHY_PROTO_UART_TXD,
                                     CONFIG_TOUCHY_PROTO_UART_RXD,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE));
    }
    xTaskCreatePinnedToCore(uart_rx_pump_task, "uart_rx", 2048, this,
                            tskIDLE_PRIORITY + 2, nullptr, tskNO_AFFINITY);
    ESP_LOGI(TAG, "protocol UART%d @ %d baud, TXD=%d RXD=%d",
             (int)CONFIG_TOUCHY_PROTO_UART_NUM,
             (int)PROTO_UART_BAUD,
             (int)CONFIG_TOUCHY_PROTO_UART_TXD,
             (int)CONFIG_TOUCHY_PROTO_UART_RXD);
    return true;
}

HostApiLink *uart_link_instance()
{
    static UartLink s_link;
    return &s_link;
}

#endif  // CONFIG_TOUCHY_PROTO_OVER_UART && CONFIG_TOUCHY_HAS_PROTO_UART
