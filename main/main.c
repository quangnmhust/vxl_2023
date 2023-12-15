#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/param.h>
#include <sys/time.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_cpu.h"
#include "esp_mem.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event_loop.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_attr.h"
#include "esp_spi_flash.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_adc_cal.h"

#include "driver/adc.h"

#include "esp_random.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"


#include <sht3x.h>

#include "Data_manager.h"
#include "Device_manager.h"

__attribute__((unused)) static const char *TAG = "Main";
#define PERIOD_GET_DATA_FROM_SENSOR (TickType_t)(5000 / portTICK_RATE_MS)
//#define PERIOD_SAVE_DATA_SENSOR_TO_SDCARD (TickType_t)(2500 / portTICK_RATE_MS)
#define PERIOD_SAVE_DATA_AFTER_WIFI_RECONNECT (TickType_t)(1000 / portTICK_RATE_MS)

#define NO_WAIT (TickType_t)(0)
#define WAIT_10_TICK (TickType_t)(10 / portTICK_RATE_MS)
#define WAIT_100_TICK (TickType_t)(100 / portTICK_RATE_MS)

#define QUEUE_SIZE 10U
#define NAME_FILE_QUEUE_SIZE 5U

#define WEB_SERVER  CONFIG_CONFIG_WEB_SERVER
#define WEB_PORT  CONFIG_CONFIG_WEB_PORT
#define API_KEY CONFIG_API_KEY

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define DEFAULT_VREF    1100        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   64	

static struct statusDevice_st statusDevice = {0};

static esp_adc_cal_characteristics_t *adc_chars;

static const adc_channel_t MQ_channel = ADC_CHANNEL_6;
static const adc_channel_t FL_channel = ADC_CHANNEL_7;

static const adc_bits_width_t width = ADC_WIDTH_BIT_12;

static const adc_atten_t atten = ADC_ATTEN_DB_0;
static const adc_unit_t unit = ADC_UNIT_1;

static sht3x_t dev;
struct dataSensor_st dataRaw;

char REQUEST[512];
char recv_buf[512];

TaskHandle_t getDataFromSensorTask_handle = NULL;
TaskHandle_t httpPublishMessageTask_handle = NULL;
TaskHandle_t sht31GetDataFromSensorTask_handle = NULL;
TaskHandle_t adcGetDataFromSensorTask_handle = NULL;


SemaphoreHandle_t getDataSensor_semaphore = NULL;
SemaphoreHandle_t allocateData_semaphore = NULL;
SemaphoreHandle_t sentDataToHTTP_semaphore = NULL;

QueueHandle_t dataSensorIntermediate_queue;

static void http_app_start(void);
static void sht31_start(void);
static void adc_start(void);

static void initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(error);
}

static esp_err_t WiFi_eventHandler(void *argument, system_event_t *event)
{
    switch (event->event_id)
    {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        ESP_LOGI(__func__, "Trying to connect with Wi-Fi\n");
        break;

    case SYSTEM_EVENT_STA_CONNECTED:
        ESP_LOGI(__func__, "Wi-Fi connected AP SSID:%s password:%s\n", CONFIG_SSID, CONFIG_PASSWORD);
        break;

    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(__func__, "got ip: starting MQTT Client\n");
        /* When connect/reconnect wifi, esp32 take an IP address and this
         * event become active. If it's the first-time connection, create
         * task mqttPublishMessageTask, else resume that task. */
        if (httpPublishMessageTask_handle == NULL)
        {
        	http_app_start();
        }
        else
        {
            if (eTaskGetState(httpPublishMessageTask_handle) == eSuspended)
            {
                vTaskResume(httpPublishMessageTask_handle);
                ESP_LOGI(__func__, "Resume task mqttPublishMessageTask.");
            }
        }
        break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* When esp32 disconnect to wifi, this event become
         * active. We suspend the task mqttPublishMessageTask. */
        ESP_LOGI(__func__, "disconnected: Retrying Wi-Fi connect to AP SSID:%s password:%s", CONFIG_SSID, CONFIG_PASSWORD);
        esp_wifi_connect();
        break;

    default:
        break;
    }
    return ESP_OK;
}

void WIFI_initSTA (void) {
    ESP_LOGI(__func__, "WIFI initializing STA");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t WIFI_initConfig = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_init(&WIFI_initConfig));

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_init(WiFi_eventHandler, NULL));

    static wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_SSID,
            .password = CONFIG_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());

    ESP_LOGI(__func__, "WIFI initialize STA finished.");
}

void getDataFromSensor_task(void* parameters) {

	sht31_start();
	adc_start();

	struct dataSensor_st dataTemp;
	struct moduleError_st errorTemp;
	TickType_t task_lastWakeTime;
	task_lastWakeTime = xTaskGetTickCount();
	getDataSensor_semaphore = xSemaphoreCreateMutex();

	while(1){
		dataTemp.temperature = dataRaw.temperature;
		dataTemp.humidity = dataRaw.humidity;
		dataTemp.CO = (float)dataRaw.CO/1.0;
		dataTemp.CO2 = (float)dataRaw.CO2/1.0;

		ESP_LOGI(__func__, "Read data from sensors completed!");
		if (xSemaphoreTake(allocateData_semaphore, portMAX_DELAY) == pdPASS)
		{
			if (xQueueSendToBack(dataSensorIntermediate_queue, (void *)&dataTemp, WAIT_10_TICK * 5) != pdPASS)
			{
				ESP_LOGE(__func__, "Failed to post the data sensor to dataSensorIntermediate Queue.");
			}
			else
			{
				ESP_LOGI(__func__, "Data waiting to read %d, Available space %d \n", uxQueueMessagesWaiting(dataSensorIntermediate_queue), uxQueueSpacesAvailable(dataSensorIntermediate_queue));
				ESP_LOGI(__func__, "Success to post the data sensor to dataSensorIntermediate Queue.");
			}
		}
		xSemaphoreGive(allocateData_semaphore);

		memset(&dataTemp, 0, sizeof(struct dataSensor_st));
		memset(&errorTemp, 0, sizeof(struct moduleError_st));
		vTaskDelayUntil(&task_lastWakeTime, PERIOD_GET_DATA_FROM_SENSOR);
	}
}

void httpPublishMessage_task(void* parameters) {
    sentDataToHTTP_semaphore = xSemaphoreCreateMutex();

    const struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res;
	struct in_addr *addr;
	int s, r;

    for(;;) {
        struct dataSensor_st data;

        int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);
		if(err != 0 || res == NULL) {
			ESP_LOGE(__func__, "DNS lookup failed err=%d res=%p", err, res);
			vTaskDelay((TickType_t)(1000 / portTICK_RATE_MS));
			continue;
		}

		addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
		ESP_LOGI(__func__, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

		s = socket(res->ai_family, res->ai_socktype, 0);
		if(s < 0) {
			ESP_LOGE(__func__, "... Failed to allocate socket.");
			freeaddrinfo(res);
			vTaskDelay((TickType_t)(1000 / portTICK_RATE_MS));
			continue;
		}
		ESP_LOGI(__func__, "... allocated socket");

		if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
			ESP_LOGE(__func__, "... socket connect failed errno=%d", errno);
			close(s);
			freeaddrinfo(res);
			vTaskDelay((TickType_t)(2000 / portTICK_RATE_MS));
			continue;
		}

		ESP_LOGI(__func__, "... connected");
		statusDevice.httpClient = CONNECTED;
		freeaddrinfo(res);

		if(statusDevice.httpClient == CONNECTED) {
			if(uxQueueMessagesWaiting(dataSensorIntermediate_queue) != 0) {
				if(xQueueReceive(dataSensorIntermediate_queue, (void* )&data, portMAX_DELAY) == pdPASS) {
					ESP_LOGI(__func__, "Receiving data from queue successfully.");
					if(xSemaphoreTake(sentDataToHTTP_semaphore, portMAX_DELAY) == pdTRUE) {

						sprintf(REQUEST, "GET https://api.thingspeak.com/update?api_key=%s&field1=%.2f&field2=%.2f&field3=%.2f&field4=%.2f\n\n\n\n", API_KEY, data.temperature, data.humidity, data.CO, data.CO2);
						ESP_LOGI(__func__, "Data waiting to read %d, Available space %d \n", uxQueueMessagesWaiting(dataSensorIntermediate_queue), uxQueueSpacesAvailable(dataSensorIntermediate_queue));
						xSemaphoreGive(sentDataToHTTP_semaphore);
						int erro = 0;
						erro = write(s, REQUEST, strlen(REQUEST));
						if ( erro < 0)
						{
							ESP_LOGE(__func__, "HTTP client publish message failed ¯\\_(ツ)_/¯...");
						}
						else
						{
							ESP_LOGI(__func__, "HTTP client publish message success (^人^).\n");
						}
					}
					vTaskDelay((TickType_t)(1000 / portTICK_RATE_MS));
				}
			}
			else
			{
				vTaskDelay(PERIOD_GET_DATA_FROM_SENSOR);
			}
		}
        else
        {
            ESP_LOGE(__func__, "HTTP Client disconnected.");
            // Suspend ourselves.
            vTaskSuspend(NULL);
        }

		struct timeval receiving_timeout;
		receiving_timeout.tv_sec = 5;
		receiving_timeout.tv_usec = 0;
		if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
				sizeof(receiving_timeout)) < 0) {
			ESP_LOGE(__func__, "... failed to set socket receiving timeout");
			close(s);
			vTaskDelay((TickType_t)(4000 / portTICK_RATE_MS));
			continue;
		}
		ESP_LOGI(__func__, "... set socket receiving timeout success");

		/* Read HTTP response */
		do {
			bzero(recv_buf, sizeof(recv_buf));
			r = read(s, recv_buf, sizeof(recv_buf)-1);
			for(int i = 0; i < r; i++) {
				putchar(recv_buf[i]);
			}
		} while(r > 0);

		ESP_LOGI(__func__, "... done reading from socket. Last read return=%d errno=%d.", r, errno);
		close(s);
    }
//    vTaskDelay((TickType_t)(10000 / portTICK_RATE_MS));
}

static void http_app_start(void) {
    ESP_LOGI(__func__, "Free memory: %d bytes", esp_get_free_heap_size());
    xTaskCreate(httpPublishMessage_task, "HTTP Publish", (1024 * 16), NULL, (UBaseType_t)10, &httpPublishMessageTask_handle);
}

#if defined(CONFIG_EXAMPLE_SHT3X_DEMO_HL)

	void sht31GetData_task(void *pvParameters)
	{
		float SHT31_temperature;
		float SHT31_humidity;
		TickType_t last_wakeup = xTaskGetTickCount();

		while (1)
		{
			// perform one measurement and do something with the results
			ESP_ERROR_CHECK(sht3x_measure(&dev, &SHT31_temperature, &SHT31_humidity));
			dataRaw.temperature = SHT31_temperature;
			dataRaw.humidity = SHT31_humidity;
			ESP_LOGI(__func__,"SHT3x Sensor: %.2f °C, %.2f %%\n", SHT31_temperature, SHT31_humidity);

			// wait until 5 seconds are over
			vTaskDelayUntil(&last_wakeup, pdMS_TO_TICKS(5000));
		}
	}

#elif defined(CONFIG_EXAMPLE_SHT3X_DEMO_LL)

	void sht31GetData_task(void *pvParameters)
	{
		float SHT31_temperature;
		float SHT31_humidity;

		TickType_t last_wakeup = xTaskGetTickCount();

		// get the measurement duration for high repeatability;
		uint8_t duration = sht3x_get_measurement_duration(SHT3X_HIGH);

		while (1)
		{
			// Trigger one measurement in single shot mode with high repeatability.
			ESP_ERROR_CHECK(sht3x_start_measurement(&dev, SHT3X_SINGLE_SHOT, SHT3X_HIGH));

			// Wait until measurement is ready (constant time of at least 30 ms
			// or the duration returned from *sht3x_get_measurement_duration*).
			vTaskDelay(duration);

			// retrieve the values and do something with them
			ESP_ERROR_CHECK(sht3x_get_results(&dev, &SHT31_temperature, &SHT31_humidity));
			dataRaw.temperature = SHT31_temperature;
			dataRaw.humidity = SHT31_humidity;
			ESP_LOGI(__func__,"SHT3x Sensor: %.2f °C, %.2f %%\n", SHT31_temperature, SHT31_humidity);

			// wait until 5 seconds are over
			vTaskDelayUntil(&last_wakeup, pdMS_TO_TICKS(5000));
		}
	}

#else // CONFIG_SHT3X_DEMO_PERIODIC

	void sht31GetData_task(void *pvParameters)
	{
		float SHT31_temperature;
		float SHT31_humidity;
		esp_err_t res;

		// Start periodic measurements with 1 measurement per second.
		ESP_ERROR_CHECK(sht3x_start_measurement(&dev, SHT3X_PERIODIC_1MPS, SHT3X_HIGH));

		// Wait until first measurement is ready (constant time of at least 30 ms
		// or the duration returned from *sht3x_get_measurement_duration*).
		vTaskDelay(sht3x_get_measurement_duration(SHT3X_HIGH));

		TickType_t last_wakeup = xTaskGetTickCount();

		while (1)
		{
			// Get the values and do something with them.
			if ((res = sht3x_get_results(&dev, &SHT31_temperature, &SHT31_humidity)) == ESP_OK)
			{
				dataRaw.temperature = SHT31_temperature;
				dataRaw.humidity = SHT31_humidity;
				ESP_LOGI(__func__,"SHT3x Sensor: %.2f °C, %.2f %%\n\n", SHT31_temperature, SHT31_humidity);
			}
			else
				ESP_LOGE(__func__,"Could not get results: %d (%s)", res, esp_err_to_name(res));

			// Wait until 2 seconds (cycle time) are over.
			vTaskDelayUntil(&last_wakeup, pdMS_TO_TICKS(2000));
		}
	}

#endif

static void sht31_start(void){
	ESP_LOGI(__func__, "Start checking SHT31 ....");
	ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(i2cdev_init());
    memset(&dev, 0, sizeof(sht3x_t));
	ESP_ERROR_CHECK(sht3x_init_desc(&dev, CONFIG_SHT3X_ADDR, 0, CONFIG_I2C_MASTER_SDA, CONFIG_I2C_MASTER_SCL));
    ESP_ERROR_CHECK(sht3x_init(&dev));

	xTaskCreate(sht31GetData_task, "SHT31 Task", (1024 * 32), NULL, (UBaseType_t)25, &sht31GetDataFromSensorTask_handle);
}

static void check_efuse(void)
{
    //Check if TP is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
        printf("eFuse Two Point: Supported\n");
    } else {
        printf("eFuse Two Point: NOT supported\n");
    }
    //Check Vref is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) {
        printf("eFuse Vref: Supported\n");
    } else {
        printf("eFuse Vref: NOT supported\n");
    }
}

static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        printf("Characterized using eFuse Vref\n");
    } else {
        printf("Characterized using Default Vref\n");
    }
}

void adcReadData_Task(void* parameters){
	uint32_t adc_reading1 = 0;
	uint32_t adc_reading2 = 0;
    //Multisampling
	for (int i = 0; i < NO_OF_SAMPLES; i++) {
			adc_reading1 += adc1_get_raw((adc1_channel_t)MQ_channel);
			adc_reading2 += adc1_get_raw((adc1_channel_t)FL_channel);
		} 
	adc_reading1 /= NO_OF_SAMPLES;
	adc_reading2 /= NO_OF_SAMPLES;
	dataRaw.CO2 = adc_reading1;
	dataRaw.CO = adc_reading2;
	ESP_LOGI(__func__,"MQ_data: %d\tFL_data: %d\n", adc_reading1, adc_reading2);
	vTaskDelay(pdMS_TO_TICKS(5000));
}

static void adc_start(void){
	ESP_LOGI(__func__, "Start checking ADC ....");
	check_efuse();

    //Configure ADC
    adc1_config_width(width);
    adc1_config_channel_atten(MQ_channel, atten);
	adc1_config_channel_atten(FL_channel, atten);

    //Characterize ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);

	xTaskCreate(adcReadData_Task, "ADC Task", (1024 * 32), NULL, (UBaseType_t)25, &adcGetDataFromSensorTask_handle);

}

void app_main(void)
{
	// Allow other core to finish initialization
	vTaskDelay(pdMS_TO_TICKS(200));

	/* Print chip information */
	esp_chip_info_t chip_info;
	esp_chip_info(&chip_info);
	ESP_LOGI("ESP_info", "This is %s chip with %d CPU core(s), revision %d, WiFi%s%s, ", CONFIG_IDF_TARGET, chip_info.cores, chip_info.revision, (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "", (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
	ESP_LOGI("ESP_info", "silicon revision %d, ", chip_info.revision);
	ESP_LOGI("ESP_info", "%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024), (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
	// ESP_LOGI(__func__, "Free memory: %d bytes", esp_get_free_heap_size());
	ESP_LOGI(__func__, "IDF version: %s", esp_get_idf_version());
	ESP_LOGI("ESP_info", "Minimum free heap size: %d bytes\r\n", esp_get_minimum_free_heap_size());

	// Booting firmware
	ESP_LOGI(__func__, "Booting....");
	ESP_LOGI(__func__, "Name device: %s.", CONFIG_NAME_DEVICE);
	ESP_LOGI(__func__, "Firmware version %s.", CONFIG_FIRMWARE_VERSION);

	// Initialize nvs partition
	ESP_LOGI(__func__, "Initialize nvs partition.");
	initialize_nvs();
	// Wait a second for memory initialization
	vTaskDelay(1000 / portTICK_RATE_MS);

	// Creat dataSensorIntermediateQueue
	dataSensorIntermediate_queue = xQueueCreate(30, sizeof(struct dataSensor_st));
	while (dataSensorIntermediate_queue == NULL)
	{
		ESP_LOGE(__func__, "Create dataSensorIntermediate Queue failed.");
		ESP_LOGI(__func__, "Retry to create dataSensorIntermediate Queue...");
		vTaskDelay(500 / portTICK_RATE_MS);
		dataSensorIntermediate_queue = xQueueCreate(30, sizeof(struct dataSensor_st));
	};
	ESP_LOGI(__func__, "Create dataSensorIntermediate Queue success.");

	allocateData_semaphore = xSemaphoreCreateMutex();

	xTaskCreate(getDataFromSensor_task, "GetDataSensor", (1024 * 32), NULL, (UBaseType_t)25, &getDataFromSensorTask_handle);

	//Wifi station config
	ESP_LOGI(__func__, "Initialize wifi station.");
	WIFI_initSTA();
}
