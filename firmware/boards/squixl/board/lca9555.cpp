// SPDX-License-Identifier: GPL-3.0-or-later

#include "lca9555.h"
#include "tc_tag.h"

#include "esp_log.h"

static const char *TAG = TOUCHY_TAG("lca9555");

static constexpr uint8_t REG_INPUT0  = 0x00;
static constexpr uint8_t REG_INPUT1  = 0x01;
static constexpr uint8_t REG_OUTPUT0 = 0x02;
static constexpr uint8_t REG_OUTPUT1 = 0x03;
static constexpr uint8_t REG_CONFIG0 = 0x06;
static constexpr uint8_t REG_CONFIG1 = 0x07;

Lca9555::Lca9555():
    m_dev(nullptr) {
    m_output[0] = 0xff;
    m_output[1] = 0xff;
    m_config[0] = 0xff;  // all inputs by default
    m_config[1] = 0xff;
}

bool Lca9555::init(i2c_master_bus_handle_t bus, uint8_t addr) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = addr;
    cfg.scl_speed_hz    = 400000;

    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &m_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) adding LCA9555 at 0x%02x", esp_err_to_name(err), addr);
        return false;
    }

    err = i2c_master_probe(bus, addr, 50);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCA9555 not found at 0x%02x", addr);
        return false;
    }

    ESP_LOGI(TAG, "LCA9555 found at 0x%02x", addr);
    return true;
}

void Lca9555::pinMode(uint8_t pin, bool input, bool initial_value) {
    if (pin > 15) return;
    int port   = pin >> 3;
    uint8_t bit = 1u << (pin & 0x07);

    if (!input) {
        if (initial_value) m_output[port] |= bit;
        else               m_output[port] &= ~bit;
        writeReg(REG_OUTPUT0 + port, m_output[port]);
        m_config[port] &= ~bit;
    } else {
        m_config[port] |= bit;
    }
    writeReg(REG_CONFIG0 + port, m_config[port]);
}

void Lca9555::write(uint8_t pin, bool value) {
    if (pin > 15) return;
    int port   = pin >> 3;
    uint8_t bit = 1u << (pin & 0x07);

    if (value) m_output[port] |= bit;
    else       m_output[port] &= ~bit;
    writeReg(REG_OUTPUT0 + port, m_output[port]);
}

bool Lca9555::read(uint8_t pin) {
    if (pin > 15) return false;
    int port   = pin >> 3;
    uint8_t bit = 1u << (pin & 0x07);
    return (readReg(REG_INPUT0 + port) & bit) != 0;
}

uint8_t Lca9555::readReg(uint8_t reg) {
    uint8_t val = 0;
    esp_err_t err = i2c_master_transmit_receive(m_dev, &reg, 1, &val, 1, 50);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) reading reg 0x%02x", esp_err_to_name(err), reg);
    }
    return val;
}

void Lca9555::writeReg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    esp_err_t err = i2c_master_transmit(m_dev, buf, 2, 50);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) writing reg 0x%02x", esp_err_to_name(err), reg);
    }
}
