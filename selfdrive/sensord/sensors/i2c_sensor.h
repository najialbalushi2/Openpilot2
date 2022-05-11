#pragma once

#include <cstdint>

#include "cereal/gen/cpp/log.capnp.h"
#include "common/i2c.h"
#include "selfdrive/sensord/sensors/constants.h"
#include "selfdrive/sensord/sensors/sensor.h"
#include "selfdrive/common/gpio.h"

int16_t read_12_bit(uint8_t lsb, uint8_t msb);
int16_t read_16_bit(uint8_t lsb, uint8_t msb);
int32_t read_20_bit(uint8_t b2, uint8_t b1, uint8_t b0);


class I2CSensor : public Sensor {
private:
  I2CBus *bus;
  int gpio_nr;
  virtual uint8_t get_device_address() = 0;

public:
  I2CSensor(I2CBus *bus, int gpio_nr = 0);
  int read_register(uint register_address, uint8_t *buffer, uint8_t len);
  int set_register(uint register_address, uint8_t data);
  int init_gpio();
  bool has_interrupt_enabled();
  virtual int init() = 0;
  virtual bool get_event(cereal::SensorEventData::Builder &event) = 0;
};
