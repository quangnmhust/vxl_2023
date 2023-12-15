#ifndef __DATAMANAGER_H__
#define __DATAMANAGER_H__

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>

struct dataSensor_st
{
	float humidity;
	float temperature;

	float CO2;
	float CO;
};

const char dataSensor_templateSaveToSDCard[] = "%s,%0.2f,%0.2f,%0.2f,%d,%d,%d";

#endif
