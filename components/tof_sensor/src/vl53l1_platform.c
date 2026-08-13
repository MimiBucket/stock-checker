/**
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#include "vl53l1_platform.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VL53L1X_I2C_ADDR 0x29

static i2c_master_dev_handle_t s_i2c_dev = NULL;

void vl53l1_platform_set_i2c_device(i2c_master_dev_handle_t dev)
{
    s_i2c_dev = dev;
}

int8_t VL53L1_WriteMulti(
    uint16_t dev,
    uint16_t index,
    uint8_t *pdata,
    uint32_t count)
{
    uint8_t buffer[2 + count];

    buffer[0] = (uint8_t)(index >> 8);
    buffer[1] = (uint8_t)(index & 0xFF);

    for (uint32_t i = 0; i < count; i++) {
        buffer[2 + i] = pdata[i];
    }

    return (i2c_master_transmit(
        s_i2c_dev,
        buffer,
        count + 2,
        -1
    ) == ESP_OK) ? 0 : -1;
}

int8_t VL53L1_ReadMulti(
    uint16_t dev,
    uint16_t index,
    uint8_t *pdata,
    uint32_t count)
{
    uint8_t reg_addr[2] = {
        (uint8_t)(index >> 8),
        (uint8_t)(index & 0xFF)
    };

    return (i2c_master_transmit_receive(
        s_i2c_dev,
        reg_addr,
        2,
        pdata,
        count,
        -1
    ) == ESP_OK) ? 0 : -1;
}

int8_t VL53L1_WrByte(
    uint16_t dev,
    uint16_t index,
    uint8_t data)
{
    return VL53L1_WriteMulti(dev, index, &data, 1);
}

int8_t VL53L1_WrWord(
    uint16_t dev,
    uint16_t index,
    uint16_t data)
{
    uint8_t buffer[2] = {
        (uint8_t)(data >> 8),
        (uint8_t)(data & 0xFF)
    };

    return VL53L1_WriteMulti(dev, index, buffer, 2);
}

int8_t VL53L1_WrDWord(
    uint16_t dev,
    uint16_t index,
    uint32_t data)
{
    uint8_t buffer[4] = {
        (uint8_t)(data >> 24),
        (uint8_t)(data >> 16),
        (uint8_t)(data >> 8),
        (uint8_t)data
    };

    return VL53L1_WriteMulti(dev, index, buffer, 4);
}

int8_t VL53L1_RdByte(
    uint16_t dev,
    uint16_t index,
    uint8_t *pdata)
{
    return VL53L1_ReadMulti(dev, index, pdata, 1);
}

int8_t VL53L1_RdWord(
    uint16_t dev,
    uint16_t index,
    uint16_t *pdata)
{
    uint8_t buffer[2];

    if (VL53L1_ReadMulti(dev, index, buffer, 2) != 0) {
        return -1;
    }

    *pdata = ((uint16_t)buffer[0] << 8) | buffer[1];

    return 0;
}

int8_t VL53L1_RdDWord(
    uint16_t dev,
    uint16_t index,
    uint32_t *pdata)
{
    uint8_t buffer[4];

    if (VL53L1_ReadMulti(dev, index, buffer, 4) != 0) {
        return -1;
    }

    *pdata = ((uint32_t)buffer[0] << 24) |
             ((uint32_t)buffer[1] << 16) |
             ((uint32_t)buffer[2] << 8) |
             buffer[3];

    return 0;
}

int8_t VL53L1_WaitMs(
    uint16_t dev,
    int32_t wait_ms)
{
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    return 0;
}