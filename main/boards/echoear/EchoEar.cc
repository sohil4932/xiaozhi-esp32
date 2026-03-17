#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "esp_video.h"
#include "offline/sd_card_manager.h"
#include "mcp_server.h"
#include "power_save_timer.h"
#include "sleep_timer.h"
#include <atomic>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include "battery_monitor.h"
#include <driver/i2c_master.h>
// #include <driver/i2c.h>
#include "i2c_device.h"
#include <driver/rtc_io.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include "esp_lcd_touch_cst816s.h"
#include "touch.h"
#ifdef IMU_INT_GPIO
#include "bmi270_api.h"
#include "i2c_bus.h"
#endif  // IMU_INT_GPIO
#include "assets/lang_config.h"
#include "driver/temperature_sensor.h"
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "touch_button_sensor.h"

#define TAG "EchoEar"
#define TOUCH_SLIDER_ENABLED 1
static BatteryMonitor battery_monitor;

namespace {
constexpr float kOuterTouchThreshold = 0.015f;
constexpr uint32_t kOuterTouchDebounceTimes = 2;
constexpr int kTouchVolumeStep = 10;
constexpr int64_t kTouchSwipeWindowMs = 250;
}  // namespace

#ifdef IMU_INT_GPIO
namespace Bmi270Imu {

static bmi270_handle_t bmi_handle_ = nullptr;

esp_err_t Initialize(i2c_bus_handle_t i2c_bus, uint8_t addr = BMI270_I2C_ADDRESS) {
    if (bmi_handle_) {
        return ESP_OK;
    }

    if (!i2c_bus) {
        ESP_LOGE(TAG, "Invalid I2C bus for BMI270");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = bmi270_sensor_create(i2c_bus, &bmi_handle_, bmi270_toy_config_file, 0);
    if (ret != ESP_OK || !bmi_handle_) {
        ESP_LOGE(TAG, "BMI270 create failed: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_FAIL : ret;
    }
    ESP_LOGI(TAG, "BMI270 initialized (toy firmware)");
    return ESP_OK;
}

esp_err_t EnableImuIntForMotion() {
    if (!bmi_handle_) {
        return ESP_ERR_INVALID_STATE;
    }

    int8_t rslt;
    uint8_t sens_list[2] = {BMI2_ACCEL, BMI2_GYRO};

    rslt = bmi2_set_adv_power_save(BMI2_DISABLE, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to disable BMI270 power save: %d", rslt);
        return ESP_FAIL;
    }

    struct bmi2_sens_config config[2];
    config[BMI2_ACCEL].type = BMI2_ACCEL;
    config[BMI2_GYRO].type = BMI2_GYRO;

    rslt = bmi2_get_sensor_config(config, 2, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to get accel/gyro config: %d", rslt);
        return ESP_FAIL;
    }

    config[BMI2_ACCEL].cfg.acc.odr = BMI2_ACC_ODR_200HZ;
    config[BMI2_ACCEL].cfg.acc.range = BMI2_ACC_RANGE_16G;
    config[BMI2_ACCEL].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    config[BMI2_ACCEL].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

    config[BMI2_GYRO].cfg.gyr.odr = BMI2_GYR_ODR_200HZ;
    config[BMI2_GYRO].cfg.gyr.range = BMI2_GYR_RANGE_2000;
    config[BMI2_GYRO].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    config[BMI2_GYRO].cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
    config[BMI2_GYRO].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;

    rslt = bmi2_set_sensor_config(config, 2, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to set accel/gyro config: %d", rslt);
        return ESP_FAIL;
    }

    uint8_t data = BMI270_TOY_INT_SHAKE_MASK;
    rslt = bmi2_set_regs(BMI2_INT1_MAP_FEAT_ADDR, &data, 1, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to map shake interrupt: %d", rslt);
        return ESP_FAIL;
    }

    struct bmi2_int_pin_config pin_config = {};
    pin_config.pin_type = BMI2_INT1;
    pin_config.pin_cfg[0].input_en = BMI2_INT_INPUT_DISABLE;
    pin_config.pin_cfg[0].lvl = BMI2_INT_ACTIVE_HIGH;
    pin_config.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
    pin_config.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;
    pin_config.int_latch = BMI2_INT_NON_LATCH;
    rslt = bmi2_set_int_pin_config(&pin_config, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to set BMI270 INT pin: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi2_sensor_enable(sens_list, 2, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to enable accel/gyro: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi270_enable_toy_shake(bmi_handle_, BMI2_ENABLE);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to enable shake detection: %d", rslt);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BMI270 shake detection enabled");
    return ESP_OK;
}

}  // namespace Bmi270Imu
#endif

temperature_sensor_handle_t temp_sensor = NULL;
static const st77916_lcd_init_cmd_t vendor_specific_init_yysj[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 0},
    {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};
float tsens_value;
gpio_num_t AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_1;
gpio_num_t AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_1;
gpio_num_t QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_1;
gpio_num_t TOUCH_PAD2 = TOUCH_PAD2_1;
gpio_num_t UART1_TX = UART1_TX_1;
gpio_num_t UART1_RX = UART1_RX_1;
i2c_master_bus_handle_t i2c_bus_;

class Charge : public I2cDevice {
public:
    Charge(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        read_buffer_ = new uint8_t[8];
    }
    ~Charge()
    {
        delete[] read_buffer_;
    }
    void Printcharge()
    {
        ReadRegs(0x08, read_buffer_, 2);
        ReadRegs(0x0c, read_buffer_ + 2, 2);
        if (temp_sensor) {
            esp_err_t ts_ret = temperature_sensor_get_celsius(temp_sensor, &tsens_value);
            if (ts_ret != ESP_OK) {
                ESP_LOGW(TAG, "Temp read failed: %s", esp_err_to_name(ts_ret));
            }
        }

        int16_t voltage = static_cast<uint16_t>(read_buffer_[1] << 8 | read_buffer_[0]);
        int16_t current = static_cast<int16_t>(read_buffer_[3] << 8 | read_buffer_[2]);
        
        // Use the variables to avoid warnings (can be removed if actual implementation uses them)
        (void)voltage;
        (void)current;
    }
    static void TaskFunction(void *pvParameters)
    {
        Charge* charge = static_cast<Charge*>(pvParameters);
        while (true) {
            charge->Printcharge();
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

private:
    uint8_t* read_buffer_ = nullptr;
};

class Cst816s : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };

    enum TouchEvent {
        TOUCH_NONE,
        TOUCH_PRESS,
        TOUCH_RELEASE,
        TOUCH_HOLD
    };

    Cst816s(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        read_buffer_ = new uint8_t[6];
        was_touched_ = false;
        press_count_ = 0;

        // Create touch interrupt semaphore
        touch_isr_mux_ = xSemaphoreCreateBinary();
        if (touch_isr_mux_ == NULL) {
            ESP_LOGE("EchoEar", "Failed to create touch semaphore");
        }
    }

    ~Cst816s()
    {
        delete[] read_buffer_;

        // Delete semaphore if it exists
        if (touch_isr_mux_ != NULL) {
            vSemaphoreDelete(touch_isr_mux_);
            touch_isr_mux_ = NULL;
        }
    }

    void UpdateTouchPoint()
    {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    const TouchPoint_t &GetTouchPoint()
    {
        return tp_;
    }

    TouchEvent CheckTouchEvent()
    {
        bool is_touched = (tp_.num > 0);
        TouchEvent event = TOUCH_NONE;

        if (is_touched && !was_touched_) {
            // Press event (transition from not touched to touched)
            press_count_++;
            event = TOUCH_PRESS;
            ESP_LOGI("EchoEar", "TOUCH PRESS - count: %d, x: %d, y: %d", press_count_, tp_.x, tp_.y);
        } else if (!is_touched && was_touched_) {
            // Release event (transition from touched to not touched)
            event = TOUCH_RELEASE;
            ESP_LOGI("EchoEar", "TOUCH RELEASE - total presses: %d", press_count_);
        } else if (is_touched && was_touched_) {
            // Continuous touch (hold)
            event = TOUCH_HOLD;
            ESP_LOGD("EchoEar", "TOUCH HOLD - x: %d, y: %d", tp_.x, tp_.y);
        }

        // Update previous state
        was_touched_ = is_touched;
        return event;
    }

    int GetPressCount() const
    {
        return press_count_;
    }

    void ResetPressCount()
    {
        press_count_ = 0;
    }

    // Semaphore management methods
    SemaphoreHandle_t GetTouchSemaphore()
    {
        return touch_isr_mux_;
    }

    bool WaitForTouchEvent(TickType_t timeout = portMAX_DELAY)
    {
        if (touch_isr_mux_ != NULL) {
            return xSemaphoreTake(touch_isr_mux_, timeout) == pdTRUE;
        }
        return false;
    }

    void NotifyTouchEvent()
    {
        if (touch_isr_mux_ != NULL) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(touch_isr_mux_, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;

    // Touch state tracking
    bool was_touched_;
    int press_count_;

    // Touch interrupt semaphore
    SemaphoreHandle_t touch_isr_mux_;
};

class EchoEar : public WifiBoard {
private:
    //i2c_master_bus_handle_t i2c_bus_;
    static constexpr int kLowBatteryNotificationLevel = 20;
    Cst816s* cst816s_;
    Charge* charge_;
    Button boot_button_;
    Display* display_ = nullptr;
    PwmBacklight* backlight_ = nullptr;
    esp_timer_handle_t touchpad_timer_;
    esp_lcd_touch_handle_t tp;   // LCD touch handle
    EspVideo* camera_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    #ifdef IMU_INT_GPIO
        i2c_bus_handle_t shared_i2c_bus_handle_ = nullptr;
        bool imu_ready_ = false;
        SemaphoreHandle_t imu_isr_mux_ = nullptr;
    #endif

    uint32_t wake_touch_channels_[2] = {0};
    float wake_touch_thresholds_[2] = {kOuterTouchThreshold, kOuterTouchThreshold};
    uint32_t wake_touch_channel_count_ = 0;
    touch_button_handle_t wake_touch_handle_ = NULL;

    #if TOUCH_SLIDER_ENABLED
        bool touch_slider_enabled_ = false;
        uint32_t touch_active_bitmap_ = 0;
        bool is_sliding_detected_ = false;
        uint32_t last_touch_channel_ = UINT32_MAX;
        int64_t last_touch_time_ms_ = 0;
    #endif

    #ifdef IMU_INT_GPIO
    void InitializeI2c()
    {
    // Initialize I2C peripheral
    i2c_config_t i2c_bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .master =
            {
                .clk_speed = I2C_MASTER_FREQ_HZ,
            },
        .clk_flags = 0,
    };
    shared_i2c_bus_handle_ = i2c_bus_create(I2C_NUM_0, &i2c_bus_cfg);
        if (!shared_i2c_bus_handle_) {
            ESP_LOGE(TAG, "Failed to create shared I2C bus");
            ESP_ERROR_CHECK(ESP_FAIL);
        }
    
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && !CONFIG_I2C_BUS_BACKWARD_CONFIG
            i2c_bus_ = i2c_bus_get_internal_bus_handle(shared_i2c_bus_handle_);
    #else
    #error "ESP-Spot board requires i2c_bus_get_internal_bus_handle() support"
    #endif
            if (!i2c_bus_) {
                ESP_LOGE(TAG, "Failed to obtain master bus handle");
                ESP_ERROR_CHECK(ESP_FAIL);
            }

        esp_err_t imu_ret = Bmi270Imu::Initialize(shared_i2c_bus_handle_);
        if (imu_ret != ESP_OK) {
            ESP_LOGW(TAG, "BMI270 initialization failed (%s)", esp_err_to_name(imu_ret));
        } else {
            imu_ready_ = true;
        }

        if (!temp_sensor) {
            temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
            esp_err_t ts_ret = temperature_sensor_install(&temp_sensor_config, &temp_sensor);
            if (ts_ret == ESP_OK) {
                ts_ret = temperature_sensor_enable(temp_sensor);
            }
            if (ts_ret != ESP_OK) {
                ESP_LOGW(TAG, "Temperature sensor init failed: %s", esp_err_to_name(ts_ret));
                temp_sensor = NULL;
            }
        }
    }

    #else
        void InitializeI2c()
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
        ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));

    }
    #endif

    uint8_t DetectPcbVersion()
    {
        esp_err_t ret = i2c_master_probe(i2c_bus_, 0x18, 100);
        uint8_t pcb_verison = 0;
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "PCB verison V1.0");
            pcb_verison = 0;
        } else {
            gpio_config_t gpio_conf = {
                .pin_bit_mask = (1ULL << GPIO_NUM_48),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE
            };
            ESP_ERROR_CHECK(gpio_config(&gpio_conf));
            ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_48, 1));
            vTaskDelay(pdMS_TO_TICKS(100));
            ret = i2c_master_probe(i2c_bus_, 0x18, 100);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "PCB verison V1.2");
                pcb_verison = 1;
                AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_2;
                AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_2;
                QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_2;
                TOUCH_PAD2 = TOUCH_PAD2_2;
                UART1_TX = UART1_TX_2;
                UART1_RX = UART1_RX_2;
            } else {
                ESP_LOGE(TAG, "PCB version detection error");

            }
        }
        return pcb_verison;
    }

    static void touch_isr_callback(void* arg)
    {
        Cst816s* touchpad = static_cast<Cst816s*>(arg);
        if (touchpad != nullptr) {
            touchpad->NotifyTouchEvent();
        }
    }

    #if TOUCH_SLIDER_ENABLED
    int GetWakeChannelIndex(uint32_t channel) const {
        for (uint32_t i = 0; i < wake_touch_channel_count_; ++i) {
            if (wake_touch_channels_[i] == channel) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void ChangeVolume(int delta) {
        auto* codec = GetAudioCodec();
        if (!codec) {
            return;
        }

        int volume = codec->output_volume() + delta;
        if (volume > 100) volume = 100;
        else if (volume < 0) volume = 0;

        codec->SetOutputVolume(volume);
        auto* display = GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        }
    }
    
    bool HandleOuterTouchSlider(uint32_t channel, touch_state_t state) {
        if (!touch_slider_enabled_) {
            return false;
        }

        const int channel_index = GetWakeChannelIndex(channel);
        if (channel_index < 0) {
            return false;
        }

        const uint32_t channel_mask = 1u << channel_index;
        const int64_t now_ms = esp_timer_get_time() / 1000;

        if (state == TOUCH_STATE_ACTIVE) {
            if (touch_active_bitmap_ == 0) {
                is_sliding_detected_ = false;
            }

            touch_active_bitmap_ |= channel_mask;

            if (!is_sliding_detected_ && last_touch_channel_ != UINT32_MAX &&
                last_touch_channel_ != channel &&
                (now_ms - last_touch_time_ms_) <= kTouchSwipeWindowMs) {
                is_sliding_detected_ = true;

                if (last_touch_channel_ == static_cast<uint32_t>(TOUCH_PAD1) &&
                    channel == static_cast<uint32_t>(TOUCH_PAD2)) {
                    ChangeVolume(kTouchVolumeStep);
                } else if (last_touch_channel_ == static_cast<uint32_t>(TOUCH_PAD2) &&
                           channel == static_cast<uint32_t>(TOUCH_PAD1)) {
                    ChangeVolume(-kTouchVolumeStep);
                }
            }

            last_touch_channel_ = channel;
            last_touch_time_ms_ = now_ms;
            return true;
        }

        if (state == TOUCH_STATE_INACTIVE) {
            touch_active_bitmap_ &= ~channel_mask;
            if (touch_active_bitmap_ == 0) {
                is_sliding_detected_ = false;
                last_touch_channel_ = UINT32_MAX;
                last_touch_time_ms_ = 0;
            }
            return true;
        }

        return true;
        }
    #endif

    static void outer_touch_callback(touch_button_handle_t handle, uint32_t channel,
                                     touch_state_t state, void* cb_arg) {
        (void)handle;

        auto* board = static_cast<EchoEar*>(cb_arg);
        if (board != nullptr) {
        #if TOUCH_SLIDER_ENABLED
                    if (board->HandleOuterTouchSlider(channel, state)) {
                        return;
                    }
        #endif
                }
            }
    
       static void outer_touch_task(void* arg) {
        auto* board = static_cast<EchoEar*>(arg);
        if (board == nullptr || board->wake_touch_handle_ == NULL) {
            ESP_LOGE(TAG, "Invalid outer touch context");
            vTaskDelete(NULL);
            return;
        }

        while (true) {
            esp_err_t ret = touch_button_sensor_handle_events(board->wake_touch_handle_);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Outer touch event handling failed: %s", esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    static void touch_event_task(void* arg)
    {
        Cst816s* touchpad = static_cast<Cst816s*>(arg);
        if (touchpad == nullptr) {
            ESP_LOGE(TAG, "Invalid touchpad pointer in touch_event_task");
            vTaskDelete(NULL);
            return;
        }

        while (true) {
            if (touchpad->WaitForTouchEvent()) {
                auto &app = Application::GetInstance();

                ESP_LOGD(TAG, "Touch event, TP_PIN_NUM_INT: %d", gpio_get_level(TP_PIN_NUM_INT));
                touchpad->UpdateTouchPoint();
                auto touch_event = touchpad->CheckTouchEvent();

                if (touch_event == Cst816s::TOUCH_RELEASE) {
                    app.Schedule([]() {
                        auto& app = Application::GetInstance();
                        auto state = app.GetDeviceState();
                        if (state == kDeviceStateStarting) {
                            auto& board = static_cast<EchoEar&>(Board::GetInstance());
                            board.EnterWifiConfigMode();
                            return;
                        }

                        auto& audio = app.GetAudioService();
                        if (audio.IsOfflineModeEnabled()) {
                            // In offline mode, trigger command listening on touch
                            ESP_LOGI(TAG, "Screen touch: Triggering command listening mode");
                            audio.TriggerCommandListening();
                        } else {
                            // In online mode, toggle chat state
                            app.ToggleChatState();
                        }
                    });
                }
            }
        }
    }

#ifdef IMU_INT_GPIO
    static void imu_isr_callback(void* arg)
    {
        auto* self = static_cast<EchoEar*>(arg);
        if (self && self->imu_isr_mux_ != NULL) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(self->imu_isr_mux_, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }

    static void imu_event_task(void* arg)
    {
        auto* self = static_cast<EchoEar*>(arg);
        if (!self) {
            vTaskDelete(NULL);
            return;
        }

        static constexpr int64_t kImuDebounceUs = 120 * 1000;   // 120ms
        static constexpr int64_t kWiggleWindowUs = 600 * 1000;  // 600ms
        static constexpr int64_t kListeningCooldownUs = 5000 * 1000;  // 5s
        const TickType_t kPollTick = pdMS_TO_TICKS(50);
        int64_t last_motion_us = 0;
        int64_t pending_wiggle_us = 0;
        int64_t last_listen_us = 0;
        bool pending_single = false;

        while (true) {
            int64_t now = esp_timer_get_time();

            if (pending_single && (now - pending_wiggle_us >= kWiggleWindowUs)) {
                pending_single = false;

                if (now - last_listen_us >= kListeningCooldownUs) {
                    auto &app = Application::GetInstance();
                    auto &audio = app.GetAudioService();
                    auto state = app.GetDeviceState();
                    if (state != kDeviceStateListening && state != kDeviceStateConnecting) {
                        ESP_LOGI(TAG, "IMU single-wiggle: entering listening mode");
                        if (audio.IsOfflineModeEnabled()) {
                            audio.TriggerCommandListening();
                        } else {
                            app.StartListening();
                        }
                        last_listen_us = now;
                    }
                }
            }

            if (self->imu_isr_mux_ && xSemaphoreTake(self->imu_isr_mux_, kPollTick) == pdTRUE) {
                now = esp_timer_get_time();
                if (now - last_motion_us < kImuDebounceUs) {
                    continue;
                }
                last_motion_us = now;

                if (!pending_single) {
                    pending_single = true;
                    pending_wiggle_us = now;
                    continue;
                }

                if (now - pending_wiggle_us <= kWiggleWindowUs) {
                    pending_single = false;
                    auto &app = Application::GetInstance();
                    auto state = app.GetDeviceState();
                    if (state == kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "IMU double-wiggle: stop speaking");
                        app.ToggleChatState();
                    } else if (state == kDeviceStateListening || state == kDeviceStateConnecting) {
                        ESP_LOGI(TAG, "IMU double-wiggle: stop listening");
                        app.StopListening();
                    } else {
                        ESP_LOGI(TAG, "IMU double-wiggle: already idle");
                    }
                } else {
                    pending_wiggle_us = now;
                }
            }
        }
    }
#endif

    void InitializeCharge()
    {
        charge_ = new Charge(i2c_bus_, 0x55);
        xTaskCreatePinnedToCore(Charge::TaskFunction, "batterydecTask", 3 * 1024, charge_, 6, NULL, 0);
    }

    void InitializeCst816sTouchPad()
    {
        cst816s_ = new Cst816s(i2c_bus_, 0x15);

        xTaskCreatePinnedToCore(touch_event_task, "touch_task", 4 * 1024, cst816s_, 5, NULL, 1);

        const gpio_config_t int_gpio_config = {
            .pin_bit_mask = (1ULL << TP_PIN_NUM_INT),
            .mode = GPIO_MODE_INPUT,
            // .intr_type = GPIO_INTR_NEGEDGE
            .intr_type = GPIO_INTR_ANYEDGE
        };
        gpio_config(&int_gpio_config);
        gpio_install_isr_service(0);
        gpio_intr_enable(TP_PIN_NUM_INT);
        gpio_isr_handler_add(TP_PIN_NUM_INT, EchoEar::touch_isr_callback, cst816s_);
    }

    
    
    void InitializeSliderTouch() {
        wake_touch_channel_count_ = 0;

        if (TOUCH_PAD1 != GPIO_NUM_NC) {
            wake_touch_channels_[wake_touch_channel_count_++] = static_cast<uint32_t>(TOUCH_PAD1);
        }
        if (TOUCH_PAD2 != GPIO_NUM_NC) {
            wake_touch_channels_[wake_touch_channel_count_++] = static_cast<uint32_t>(TOUCH_PAD2);
        }

        if (wake_touch_channel_count_ == 0) {
            ESP_LOGW(TAG, "No outer touch pads configured, skip wake touch");
            return;
        }

        #if TOUCH_SLIDER_ENABLED
                touch_slider_enabled_ = (wake_touch_channel_count_ >= 2);
                if (!touch_slider_enabled_) {
                    ESP_LOGW(TAG, "Touch slider enabled but less than 2 touch pads, slider disabled");
                } else {
                    ESP_LOGI(TAG, "Touch slider enabled for volume control");
                }
        #endif

        touch_button_config_t config = {
            .channel_num = wake_touch_channel_count_,
            .channel_list = wake_touch_channels_,
            .channel_threshold = wake_touch_thresholds_,
            .channel_gold_value = NULL,
            .debounce_times = kOuterTouchDebounceTimes,
            .skip_lowlevel_init = false,
        };

        esp_err_t ret = touch_button_sensor_create(&config, &wake_touch_handle_, outer_touch_callback, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize outer touch wake: %s", esp_err_to_name(ret));
            wake_touch_handle_ = NULL;
            return;
        }

        BaseType_t task_ret = xTaskCreatePinnedToCore(
            outer_touch_task, "outer_touch_task", 4096, this, 5, NULL, 1);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create outer touch task");
            touch_button_sensor_delete(wake_touch_handle_);
            wake_touch_handle_ = NULL;
            return;
        }

        ESP_LOGI(TAG, "Outer touch wake initialized on %lu channel(s)",
            static_cast<unsigned long>(wake_touch_channel_count_));
    }

    void InitializeSpi()
    {
        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                                  QSPI_PIN_NUM_LCD_DATA0,
                                                                                  QSPI_PIN_NUM_LCD_DATA1,
                                                                                  QSPI_PIN_NUM_LCD_DATA2,
                                                                                  QSPI_PIN_NUM_LCD_DATA3,
                                                                                  QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void Initializest77916Display(uint8_t pcb_verison)
    {

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(QSPI_PIN_NUM_LCD_CS, NULL, NULL);
        io_config.trans_queue_depth = 20;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));
        st77916_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init_yysj,
            .init_cmds_size = sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = QSPI_PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
            .flags = {
                .reset_active_high = pcb_verison,
            },
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
        backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        backlight_->RestoreBrightness();
    }
    
    void InitializeGpio()
    {
        // 初始化GPIO引脚，包括LED_G和其他需要的引脚
        gpio_config_t gpio_conf = {
            .pin_bit_mask = (1ULL << LED_G | 1ULL << POWER_CTRL),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&gpio_conf));

        #ifdef IMU_INT_GPIO
            gpio_config_t io_conf_imu_int = {
                .pin_bit_mask = (1ULL << IMU_INT_GPIO),
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_ENABLE,
                .intr_type = GPIO_INTR_POSEDGE,
            };
            gpio_config(&io_conf_imu_int);
            gpio_install_isr_service(0);
        #endif  // IMU_INT_GPIO
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enter sleep mode");
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
            bsp_set_head_led(false);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Exit sleep mode");
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
            bsp_set_head_led(true);
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutdown request");
            bsp_set_peripheral_power(false);
        });
        power_save_timer_->SetEnabled(true);
    }

    
#ifdef IMU_INT_GPIO
    void InitializeImuMotion()
    {
        if (!imu_ready_) {
            ESP_LOGW(TAG, "IMU not ready, skip motion interrupt");
            return;
        }

        esp_err_t imu_ret = Bmi270Imu::EnableImuIntForMotion();
        if (imu_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to enable IMU motion interrupt (%s)", esp_err_to_name(imu_ret));
            return;
        }

        if (!imu_isr_mux_) {
            imu_isr_mux_ = xSemaphoreCreateBinary();
        }
        if (!imu_isr_mux_) {
            ESP_LOGE(TAG, "Failed to create IMU ISR semaphore");
            return;
        }

#if CONFIG_FREERTOS_UNICORE
        BaseType_t task_ret = xTaskCreate(imu_event_task, "imu_task", 3 * 1024, this, 5, NULL);
#else
        BaseType_t task_ret = xTaskCreatePinnedToCore(imu_event_task, "imu_task", 3 * 1024, this, 5, NULL, 1);
#endif
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IMU event task");
            return;
        }

        esp_err_t isr_ret = gpio_isr_handler_add(IMU_INT_GPIO, EchoEar::imu_isr_callback, this);
        if (isr_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add IMU ISR handler: %s", esp_err_to_name(isr_ret));
            return;
        }
        gpio_intr_enable(IMU_INT_GPIO);
    }
#endif

    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                ESP_LOGI(TAG, "Boot button pressed, enter WiFi configuration mode");
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        gpio_config_t power_gpio_config = {
            .pin_bit_mask = (BIT64(POWER_CTRL)),
            .mode = GPIO_MODE_OUTPUT,

        };
        ESP_ERROR_CHECK(gpio_config(&power_gpio_config));

        gpio_set_level(POWER_CTRL, 0);
    }

#ifdef CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    void InitializeCamera() {
        esp_video_init_usb_uvc_config_t usb_uvc_config = {
            .uvc = {
                .uvc_dev_num = 1,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
            .usb = {
                .init_usb_host_lib = true,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
        };

        esp_video_init_config_t video_config = {
            .usb_uvc = &usb_uvc_config,
        };

        camera_ = new EspVideo(video_config);
    }
#endif // CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE

    void InitializeTools() 
    {
        auto& mcp_server = McpServer::GetInstance();
        auto display = GetDisplay();
        if (display) 
        {
            mcp_server.AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
                PropertyList(),
                [display](const PropertyList& properties) -> ReturnValue {
                    cJSON *json = cJSON_CreateObject();
                    cJSON_AddNumberToObject(json, "width", display->width());
                    cJSON_AddNumberToObject(json, "height", display->height());
                    // if (dynamic_cast<OledDisplay*>(display)) {
                    //     cJSON_AddBoolToObject(json, "monochrome", true);
                    // } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                    // }
                    return json;
                });
        }
                mcp_server.AddTool("self.battery.get_info", "Information about the battery, including state of charge, voltage, current, temperature, capacity, state of health, and charge status.",
                PropertyList(),
                [this](const PropertyList& properties) -> ReturnValue {
                    int16_t soc = battery_monitor.getBatterySOC();
                    int16_t voltage = battery_monitor.getVoltage();
                    int16_t current = battery_monitor.getCurrent();
                    uint16_t temperature = battery_monitor.getTemperature();
                    uint16_t capacity = battery_monitor.getCapacity();
                    bool is_charging = battery_monitor.is_charging();
                    ESP_LOGD(TAG, "Battery info: SOC=%d%%, voltage=%dmV, current=%dmA, temperature=%d℃, capacity=%dmAh, charging=%d",
                        soc, voltage, current, temperature, capacity, is_charging);
                    cJSON *json = cJSON_CreateObject();
                    cJSON_AddStringToObject(json, "SOC", (std::to_string(soc)+"%").c_str());
                    cJSON_AddStringToObject(json, "voltage", (std::to_string(voltage)+"mV").c_str());
                    cJSON_AddStringToObject(json, "current", (std::to_string(current)+"mA").c_str());
                    cJSON_AddStringToObject(json, "temperature", (std::to_string(temperature)+"℃").c_str());
                    cJSON_AddStringToObject(json, "capacity", (std::to_string(capacity)+"mAh").c_str());  
                    cJSON_AddBoolToObject(json, "charging", is_charging);
                    return json;
                });
            
    }

    void InitializeBatteryMonitor()
    {
        // PowerSaveTimer* power_save_timer = getPowerSaveTimer();

        auto& app = Application::GetInstance();
        if (!battery_monitor.init()) {
            ESP_LOGE(TAG, "Battery monitor init failed");
            return;
        }

        battery_monitor.setBatteryStatusCallback(
            [this, &app](const battery_status_t &status) {
                static battery_status_t bat_last_status = {};
                static bool last_low_battery = false;
                static bool status_initialized = false;

                const int soc = battery_monitor.getBatterySOC();
                const bool is_low_battery = status.DSG && soc <= kLowBatteryNotificationLevel;
                // const int soc = 10; 
                // const bool is_low_battery = soc <= kLowBatteryNotificationLevel;

                if (!status_initialized) {
                    if (power_save_timer_) {
                        power_save_timer_->SetEnabled(status.DSG != 0);
                    } 
                    bat_last_status = status;
                    last_low_battery = false;
                    status_initialized = true;
                    return;
                }

                if (bat_last_status.FC != status.FC) {
                    if (status.DSG == 0) {
                        if (power_save_timer_) {
                            power_save_timer_->SetEnabled(false);
                        } 
                    } else {
                        power_save_timer_->SetEnabled(true);
                    }
                }

                const bool charging_started = status.DSG == 0 && bat_last_status.DSG != 0 && !status.FC;
                const bool full_detected = status.FC && !bat_last_status.FC;
                const bool low_battery_started = is_low_battery && !last_low_battery;

                bat_last_status = status;
                last_low_battery = is_low_battery;

                if (low_battery_started) {
                    app.Schedule([]() {
                        auto& app = Application::GetInstance();
                        auto display = Board::GetInstance().GetDisplay();
                        if (display != nullptr) {
                            display->ShowNotification(Lang::Strings::BATTERY_LOW, 1000);
                        }
                        app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                    });
                }

                if (charging_started) {
                    app.Schedule([]() {
                        auto display = Board::GetInstance().GetDisplay();
                        if (display != nullptr) {
                            display->ShowNotification(Lang::Strings::BATTERY_CHARGING, 1000);
                        }
                    });
                } else if (full_detected) {
                    app.Schedule([]() {
                        auto display = Board::GetInstance().GetDisplay();
                        if (display != nullptr) {
                            display->ShowNotification(Lang::Strings::BATTERY_FULL, 1000);
                        }
                    });
                }
            });
        battery_monitor.setBatteryShutdownCallback(
            [&app]() { app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY); });
    }

public:
    EchoEar() : boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        uint8_t pcb_verison = DetectPcbVersion();
        InitializeGpio();
        //InitializeCharge();
        InitializeCst816sTouchPad();

        InitializeSpi();
        Initializest77916Display(pcb_verison);
        InitializeButtons();
        InitializeSliderTouch();
        InitializePowerSaveTimer();

        #ifdef IMU_INT_GPIO
             InitializeImuMotion();
        #endif  // IMU_INT_GPIO

        InitializeBatteryMonitor();
        InitializeTools();

#ifdef CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
        InitializeCamera();
#endif // CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE

        // Initialize SD card for offline audio playback
        auto& sd = offline::SDCardManager::GetInstance();
        esp_err_t ret = sd.Initialize();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card initialized successfully");
        } else {
            ESP_LOGW(TAG, "SD card initialization failed: %s (offline playback disabled)", esp_err_to_name(ret));
        }
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }

    Cst816s* GetTouchpad()
    {
        return cst816s_;
    }

    virtual Backlight* GetBacklight() override
    {
        return backlight_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual offline::SDCardManager* GetSDCard() override {
        return &offline::SDCardManager::GetInstance();
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override
    {
        if (battery_monitor.getHandle()) {
            level = battery_monitor.getBatterySOC();
            charging = battery_monitor.is_charging();
            discharging = !charging;
            return true;
        } else {
            return false;
        }
    }

    virtual bool GetTemperature(float& temperature) override
    {
        if (battery_monitor.getHandle()) {
            temperature = battery_monitor.getTemperature();
            return true;
        } else {
            return false;
        }
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    esp_err_t bsp_set_head_led(bool on) {
        return gpio_set_level(LED_G, !on);  // GREEN LED
    }

    esp_err_t bsp_set_peripheral_power(bool on) {
        return gpio_set_level(POWER_CTRL, !on);
    }
};

DECLARE_BOARD(EchoEar);
