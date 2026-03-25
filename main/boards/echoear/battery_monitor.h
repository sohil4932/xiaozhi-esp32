#ifndef _BATTERY_MONITOR_H_
#define _BATTERY_MONITOR_H_

#include "bq27220.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#ifdef __cplusplus
#include <cstdint>
#include <functional>

class BatteryMonitor {
public:
    BatteryMonitor();
    ~BatteryMonitor();

    bool init();

    uint8_t getBatterySOC() const;
    uint16_t getVoltage() const;
    int16_t getCurrent() const;
    uint16_t getTemperature() const;
    uint16_t getCapacity() const;
    uint16_t getFCC() const;
    bool is_charging() const { return battery_status.DSG == 0; }
    bool getBatteryStatus(battery_status_t &status);
    void printInfo() const;

    void setBatteryStatusCallback(std::function<void(const battery_status_t &)> callback) {
        status_cb = callback;
    }
    void setBatteryShutdownCallback(std::function<void(void)> callback) {
        shutdown_cb = callback;
    }
    void setMonitorPeriodCallback(std::function<void(void)> callback) {
        period_cb = callback;
    }

    bq27220_handle_t getHandle() const { return bq27220Handle; }

private:
    bq27220_handle_t bq27220Handle;
    TimerHandle_t timer;
    battery_status_t battery_status;
    static void monitor_period(TimerHandle_t xTimer);
    void check_shutdown(void);
    std::function<void(const battery_status_t &)> status_cb;
    std::function<void(void)> shutdown_cb;
    std::function<void(void)> period_cb;
};

#endif  // __cplusplus
#endif  // _BATTERY_MONITOR_H_
