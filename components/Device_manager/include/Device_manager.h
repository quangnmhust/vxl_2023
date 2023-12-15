#ifndef __DEVICEMANAGER_H__
#define __DEVICEMANAGER_H__

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "string.h"
#include "time.h"


typedef enum {
    DISCONNECTED = 0,
    CONNECTED,
    CONNECTING,
    NOT_FOUND,
} status_t;

struct statusDevice_st
{
    status_t wifi;
//    status_t sdcard;
    status_t SHT31Module;
    status_t MQ2Sensor;
    status_t httpClient;
};

struct moduleError_st
{
    uint64_t timestamp;
    // esp_err_t sdError;
    esp_err_t sht31Error;
    esp_err_t mq2Error;
};


#endif
