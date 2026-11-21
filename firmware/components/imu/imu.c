#include "imu.h"
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "imu";

#if __has_include("bmi270.h")
#include "bmi270.h"

static i2c_master_dev_handle_t s_dev;
static struct bmi2_dev s_bmi;

static BMI2_INTF_RETURN_TYPE bmi_read(uint8_t reg, uint8_t* data, uint32_t len, void* intf) {
  (void)intf;
  return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100) == ESP_OK ? 0 : -1;
}
static BMI2_INTF_RETURN_TYPE bmi_write(uint8_t reg, const uint8_t* data, uint32_t len, void* intf) {
  (void)intf;
  uint8_t buf[40];
  if (len > sizeof buf - 1) return -1;
  buf[0] = reg;
  memcpy(buf + 1, data, len);
  return i2c_master_transmit(s_dev, buf, len + 1, 100) == ESP_OK ? 0 : -1;
}
static void bmi_delay(uint32_t us, void* intf) {
  (void)intf;
  if (us >= 1000) vTaskDelay(pdMS_TO_TICKS(us / 1000 + 1));
  else esp_rom_delay_us(us);
}
static int wreg(uint8_t reg, uint8_t v) { return bmi_write(reg, &v, 1, NULL); }

int imu_init(i2c_master_bus_handle_t bus) {
  if (!bus) return -1;
  uint8_t addr = 0x68;
  if (i2c_master_probe(bus, 0x68, 50) != ESP_OK) {
    if (i2c_master_probe(bus, 0x69, 50) == ESP_OK) addr = 0x69;
    else { ESP_LOGW(TAG, "no BMI270 at 0x68/0x69"); return -2; }
  }
  const i2c_device_config_t dc = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
  if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) return -3;

  memset(&s_bmi, 0, sizeof s_bmi);
  s_bmi.intf = BMI2_I2C_INTF;
  s_bmi.read = bmi_read;
  s_bmi.write = bmi_write;
  s_bmi.delay_us = bmi_delay;
  s_bmi.intf_ptr = NULL;
  s_bmi.read_write_len = 32;       // firmware upload chunk
  s_bmi.config_file_ptr = NULL;    // use the blob bundled in bmi270.c
  const int8_t rc = bmi270_init(&s_bmi);
  if (rc != 0) { ESP_LOGW(TAG, "bmi270_init failed: %d", rc); return -4; }

  // accelerometer only: normal mode, 100 Hz, +-2 g
  int e = 0;
  e |= wreg(0x7C, 0x02);   // PWR_CONF: advanced power save off
  bmi_delay(1000, NULL);
  e |= wreg(0x7D, 0x04);   // PWR_CTRL: accelerometer on
  e |= wreg(0x40, 0xA8);   // ACC_CONF: performance filter, normal bandwidth, 100 Hz
  e |= wreg(0x41, 0x00);   // ACC_RANGE: +-2 g
  if (e) { ESP_LOGW(TAG, "accelerometer setup failed"); return -5; }
  bmi_delay(20000, NULL);
  ESP_LOGI(TAG, "BMI270 ready at 0x%02X", addr);
  return 0;
}

int imu_read_mg(int16_t* x, int16_t* y, int16_t* z) {
  uint8_t b[6];
  if (bmi_read(0x0C, b, 6, NULL) != 0) return -1;     // ACC_X_LSB .. ACC_Z_MSB
  const int16_t rx = (int16_t)(b[0] | (b[1] << 8)), ry = (int16_t)(b[2] | (b[3] << 8)),
                rz = (int16_t)(b[4] | (b[5] << 8));
  *x = (int16_t)((rx * 125) >> 11);                   // 16384 LSB/g -> milli-g
  *y = (int16_t)((ry * 125) >> 11);
  *z = (int16_t)((rz * 125) >> 11);
  return 0;
}

#else  // Bosch driver files not downloaded

int imu_init(i2c_master_bus_handle_t bus) {
  (void)bus;
  ESP_LOGW(TAG, "Bosch BMI270 driver files missing - run tools\\get_bmi270.ps1; auto-rotate is off");
  return -10;
}
int imu_read_mg(int16_t* x, int16_t* y, int16_t* z) { (void)x; (void)y; (void)z; return -1; }

#endif
