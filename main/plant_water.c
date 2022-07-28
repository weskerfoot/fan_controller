#include "./plant_water.h"
#include "sht3x.h"
#include "cjson.h"

#define I2C_BUS       0
#define I2C_SCL_PIN   22
#define I2C_SDA_PIN   21

#define HTTPD_RESP_SIZE 100
#define MAX_CRON_SPECS 5

static const char *TAG = "pump";
static sht3x_sensor_t* sensor;

static void set_pump(int pump_num, int state) {
    // Set duty to 100%
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, pump_num, state == 1 ? LEDC_DUTY: 0));
    // Update duty to apply the new value
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, pump_num));
}

static void
pump_one_on() {
  set_pump(0, 1);
  set_pump(1, 0);
}

static void
pump_two_on() {
  set_pump(0, 0);
  set_pump(1, 1);
}

static void
pumps_off() {
  set_pump(0, 0);
  set_pump(1, 0);
}

typedef enum {
  PUMP_ON = 1,
  PUMP_OFF = 2,
} event_type;

struct event_t {
  event_type pump_1;
  event_type pump_2;
  int pump_delay;
};

struct cron_t {
  int pump_num;
  int state;
  int pump_on_time;
  int hour;
  int minute;
  int last_ran_day; // day of the month it last ran
  int last_ran_minute; // minute it last ran
  int last_ran_hour; // hour it last ran
};

static struct cron_t cron_specs[MAX_CRON_SPECS];

static cJSON*
serialize_cron_data(struct cron_t cron_spec) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "pump_num", cron_spec.pump_num);
    cJSON_AddNumberToObject(result, "state", cron_spec.state);
    cJSON_AddNumberToObject(result, "hour", cron_spec.hour);
    cJSON_AddNumberToObject(result, "minute", cron_spec.minute);
    cJSON_AddNumberToObject(result, "pump_on_time", cron_spec.pump_on_time);
    cJSON_AddNumberToObject(result, "last_ran_day", cron_spec.last_ran_day);
    cJSON_AddNumberToObject(result, "last_ran_hour", cron_spec.last_ran_hour);
    cJSON_AddNumberToObject(result, "last_ran_minute", cron_spec.last_ran_minute);

    return result;
}

static cJSON*
serialize_cron_specs(struct cron_t cron_specs[MAX_CRON_SPECS]) {
  cJSON *cron_spec_arr = cJSON_CreateArray();
  for(int i = 0; i < MAX_CRON_SPECS; i++) {
    cJSON_AddItemToArray(cron_spec_arr, serialize_cron_data(cron_specs[i]));
  }
  return cron_spec_arr;
}

static struct cron_t
parse_cron_data(cJSON *json) {

    cJSON *pump_num;
    cJSON *state;
    cJSON *pump_on_time; // time it will be on for
    cJSON *hour; // hour to trigger in
    cJSON *minute; // minute on the hour to trigger on
    
    struct cron_t cron_spec = {0};


    if (cJSON_IsObject(json)) {
      pump_num = cJSON_GetObjectItemCaseSensitive(json, "pump_num");
      state = cJSON_GetObjectItemCaseSensitive(json, "state");
      pump_on_time = cJSON_GetObjectItemCaseSensitive(json, "pump_on_time");
      hour = cJSON_GetObjectItemCaseSensitive(json, "hour");
      minute = cJSON_GetObjectItemCaseSensitive(json, "minute");

      if (cJSON_IsNumber(pump_num) &&
          cJSON_IsNumber(state) &&
          cJSON_IsNumber(pump_on_time) &&
          cJSON_IsNumber(hour) &&
          cJSON_IsNumber(minute)) {
        printf("Creating cron spec\n");
        cron_spec.pump_num = pump_num->valueint;
        cron_spec.state = state->valueint;
        cron_spec.pump_on_time = pump_on_time->valueint;
        cron_spec.hour = hour->valueint;
        cron_spec.minute = minute->valueint;
        cron_spec.last_ran_day = -1;
        cron_spec.last_ran_hour = -1;
        cron_spec.last_ran_minute = -1;
        printf("Parsed: hour = %d, minute = %d\n", cron_spec.hour, cron_spec.minute);
      }
    }
    return cron_spec;
}

static TickType_t
make_delay(int seconds) {
  return (1000*seconds) / portTICK_PERIOD_MS;
}

// Timer type definitions
const TickType_t PUMP_TIMER_DELAY = (1000*60) / portTICK_PERIOD_MS;
const TickType_t PUMP_CB_PERIOD = (1000*10) / portTICK_PERIOD_MS;

TimerHandle_t pumpTimer = 0; // Timer for toggling the pumps on/off
StaticTimer_t pumpTimerBuffer; // Memory backing for the timer, allocated statically

// Queue type definitions
uint8_t queueStorage[PUMP_EV_NUM*sizeof (struct event_t)]; // byte array for queue memory
static StaticQueue_t pumpEvents;
QueueHandle_t pumpEventsHandle;

uint8_t timerQueueStorage[PUMP_EV_NUM*sizeof (struct cron_t)]; // byte array for queue memory
static StaticQueue_t timerEvents;
QueueHandle_t timerEventsHandle;

// Task type definitions

StaticTask_t xTaskBuffer;
StackType_t xStack[TASK_STACK_SIZE];

void
pumpRunnerTaskFunction(void *params) {
  struct event_t pumpMessage;

  printf("Task started\n");

  configASSERT( ( uint32_t ) params == 1UL );

  while (1) {
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    if (pumpEventsHandle != NULL) {
      // The queue exists and is created
      if (xQueueReceive(pumpEventsHandle, &pumpMessage, (TickType_t)PUMP_TIMER_DELAY) == pdPASS) {
        printf("got a message, pump_1 = %u, pump_2 = %u, PUMP_ON = %u, PUMP_OFF = %u\n", pumpMessage.pump_1, pumpMessage.pump_2, PUMP_ON, PUMP_OFF);
        if (pumpMessage.pump_1 == PUMP_ON) {
          pump_one_on();
        }
        if (pumpMessage.pump_2 == PUMP_ON) {
          pump_two_on();
        }
        vTaskDelay(pumpMessage.pump_delay);
        pumps_off();
      }
    }
  }
}

static void
createPumpRunnerTask(void) {
  xTaskCreateStatic(pumpRunnerTaskFunction,
                    "pumpt",
                     TASK_STACK_SIZE,
                     (void*)1,
                     tskIDLE_PRIORITY + 2,
                     xStack,
                     &xTaskBuffer);
}


void
runPumps(int delay1, int delay2) {
  struct event_t message;
  message.pump_1 = PUMP_ON;
  message.pump_2 = PUMP_OFF;
  message.pump_delay = make_delay(delay1);

  xQueueSend(pumpEventsHandle, (void*)&message, (TickType_t)0);

  message.pump_2 = PUMP_ON;
  message.pump_1 = PUMP_OFF;
  message.pump_delay = make_delay(delay2);

  xQueueSend(pumpEventsHandle, (void*)&message, (TickType_t)0);
}

void
pumpTimerCb(TimerHandle_t pumpTimer) {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  struct cron_t pump_timer_config;
  static int next_cron_spec = 0;

  if (timerEventsHandle != NULL) {
    if (xQueueReceive(timerEventsHandle, &pump_timer_config, (TickType_t)0) == pdPASS) {
      printf("Got a new cron spec\n");
      cron_specs[next_cron_spec] = pump_timer_config;
      next_cron_spec = (next_cron_spec + 1) % MAX_CRON_SPECS;
      printf("new: hour = %d, minute = %d\n", pump_timer_config.hour, pump_timer_config.minute);
    }
  }

  // check current number of cron specs, remove oldest one and replace if none left
  // loop over them and check time and execute commands
  for (int i = 0; i < MAX_CRON_SPECS; i++) {
    //printf("to match: hour = %d, minute = %d\n", cron_specs[i].hour, cron_specs[i].minute);
    //printf("current: hour = %d, minute = %d\n", timeinfo.tm_hour, timeinfo.tm_min);
    if (timeinfo.tm_hour == cron_specs[i].hour && (timeinfo.tm_min == cron_specs[i].minute)) {
      if (cron_specs[i].last_ran_day == timeinfo.tm_mday) {
        // it already ran today, skip it
        continue;
      }
      printf("running, hour = %d, minute = %d\n", cron_specs[i].hour, cron_specs[i].minute);
      // TODO refactor this bit, what if there are more pumps in the future?
      struct event_t message = {0};
      switch (cron_specs[i].pump_num) {
        case 0:
          message.pump_1 = PUMP_ON;
          message.pump_2 = PUMP_OFF;
          break;
        case 1:
          message.pump_2 = PUMP_ON;
          message.pump_1 = PUMP_OFF;
          break;
      }
      message.pump_delay = make_delay(cron_specs[i].pump_on_time);
      xQueueSend(pumpEventsHandle, (void*)&message, (TickType_t)0);

      cron_specs[i].last_ran_day = timeinfo.tm_mday;
      cron_specs[i].last_ran_hour = timeinfo.tm_hour;
      cron_specs[i].last_ran_minute = timeinfo.tm_min;
    }
  }
}

esp_err_t
get_sensor_data_handler(httpd_req_t *req) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    float temperature;
    float humidity;

    char resp[HTTPD_RESP_SIZE] = {0};

    cJSON *resp_object_j = cJSON_CreateObject();

    if (sht3x_measure(sensor, &temperature, &humidity)) {
      cJSON_AddNumberToObject(resp_object_j, "temperature", (double)temperature);
      cJSON_AddNumberToObject(resp_object_j, "humidity", (double)humidity);
    }

    cJSON_AddNumberToObject(resp_object_j, "hour", (double)timeinfo.tm_hour);
    cJSON_AddNumberToObject(resp_object_j, "minute", (double)timeinfo.tm_min);

    cJSON_PrintPreallocated(resp_object_j, resp, HTTPD_RESP_SIZE, false);

    if (resp_object_j != NULL) { cJSON_Delete(resp_object_j); }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

esp_err_t
pumps_on_handler(httpd_req_t *req) {
  printf("pumps_on_handler executed\n");
  // TODO stream
  char req_body[HTTPD_RESP_SIZE+1] = {0};
  char resp[HTTPD_RESP_SIZE] = {0};

  size_t body_size = MIN(req->content_len, (sizeof(req_body)-1));

  // Receive body and do error handling
  int ret = httpd_req_recv(req, req_body, body_size);

  // if ret == 0 then no data
  if (ret < 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_408(req);
    }
    return ESP_FAIL;
  }

  cJSON *json = cJSON_ParseWithLength(req_body, HTTPD_RESP_SIZE);

  cJSON *pump_one_time_j = NULL;
  cJSON *pump_two_time_j = NULL;

  if (json != NULL) {
    //printf("%s\n", cJSON_Print(json));
    if (cJSON_IsObject(json)) {
      pump_one_time_j = cJSON_GetObjectItemCaseSensitive(json, "pump_1");
      pump_two_time_j = cJSON_GetObjectItemCaseSensitive(json, "pump_2");
      if (cJSON_IsNumber(pump_one_time_j) && cJSON_IsNumber(pump_two_time_j)) {
        if (pump_one_time_j->valueint > 0 && pump_two_time_j->valueint > 0) {
          printf("Running pumps: p1 = %d, p2 = %d\n", pump_one_time_j->valueint, pump_two_time_j->valueint);
          runPumps(pump_one_time_j->valueint, pump_two_time_j->valueint);
        }
      }
    }
  }

  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

  if (json != NULL) { cJSON_Delete(json); }
  return ESP_OK;
}

/* URI handler structure for GET /uri */
httpd_uri_t get_sensor_data = {
    .uri      = "/sensor",
    .method   = HTTP_GET,
    .handler  = get_sensor_data_handler,
    .user_ctx = NULL
};

/* URI handler structure for POST /pumps_on */
httpd_uri_t pumps_on = {
    .uri      = "/pumps_on",
    .method   = HTTP_POST,
    .handler  = pumps_on_handler,
    .user_ctx = NULL
};

esp_err_t
add_cron_handler(httpd_req_t *req) {
  printf("add_cron_handler executed\n");
  // TODO stream
  char req_body[HTTPD_RESP_SIZE+1] = {0};
  char resp[HTTPD_RESP_SIZE] = {0};

  size_t body_size = MIN(req->content_len, (sizeof(req_body)-1));

  // Receive body and do error handling
  int ret = httpd_req_recv(req, req_body, body_size);

  // if ret == 0 then no data
  if (ret < 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_408(req);
    }
    return ESP_FAIL;
  }

  cJSON *json = cJSON_ParseWithLength(req_body, HTTPD_RESP_SIZE);

  struct cron_t cron_spec = {0};

  if (json != NULL) {
    //printf("%s\n", cJSON_Print(json));
    cron_spec = parse_cron_data(json);
    xQueueSend(timerEventsHandle, (void*)&cron_spec, (TickType_t)0);
  }

  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

  if (json != NULL) { cJSON_Delete(json); }
  return ESP_OK;
}

/* URI handler structure for POST /add_cron */
httpd_uri_t add_cron = {
    .uri      = "/add_cron",
    .method   = HTTP_POST,
    .handler  = add_cron_handler,
    .user_ctx = NULL
};


esp_err_t
get_cron_data_handler(httpd_req_t *req) {
    // TODO, figure out how large it actually is
    char resp[HTTPD_RESP_SIZE*10] = {0};

    cJSON *resp_object_j = cJSON_CreateObject();

    if (resp_object_j != NULL) { cJSON_Delete(resp_object_j); }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, HTTPD_200);

    cJSON *cron_specs_j = serialize_cron_specs(cron_specs);

    // TODO, figure out how large it actually is
    cJSON_PrintPreallocated(cron_specs_j, resp, HTTPD_RESP_SIZE*10, false);

    if (cron_specs_j != NULL) { cJSON_Delete(cron_specs_j); }

    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

httpd_uri_t get_cron_data = {
    .uri      = "/cron_data",
    .method   = HTTP_GET,
    .handler  = get_cron_data_handler,
    .user_ctx = NULL
};

/* Function for starting the webserver */
httpd_handle_t
start_webserver(void) {
    /* Generate default configuration */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* Empty handle to esp_http_server */
    httpd_handle_t server = NULL;

    /* Start the httpd server */
    if (httpd_start(&server, &config) == ESP_OK) {
        /* Register URI handlers */
        httpd_register_uri_handler(server, &get_sensor_data);
        httpd_register_uri_handler(server, &pumps_on);
        httpd_register_uri_handler(server, &add_cron);
        httpd_register_uri_handler(server, &get_cron_data);
    }
    /* If server failed to start, handle will be NULL */
    ESP_LOGI(TAG, "webserver started");
    return server;
}

static SemaphoreHandle_t s_semph_get_ip_addrs;

/* tear down connection, release resources */
static void
stop(void) {
#if CONFIG_EXAMPLE_CONNECT_WIFI
    wifi_stop();
    s_active_interfaces--;
#endif
}

esp_err_t
wifi_disconnect(void) {
    if (s_semph_get_ip_addrs == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    vSemaphoreDelete(s_semph_get_ip_addrs);
    s_semph_get_ip_addrs = NULL;
    stop();
    ESP_ERROR_CHECK(esp_unregister_shutdown_handler(&stop));
    return ESP_OK;
}
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 48
#endif

/* Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
RTC_DATA_ATTR static int boot_count = 0;

static void obtain_time(void);
static void initialize_sntp(void);

void wifi_init_sta(void);

void
time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void
obtain_time(void) {
    /**
     * NTP server address could be aquired via DHCP,
     * see following menuconfig options:
     * 'LWIP_DHCP_GET_NTP_SRV' - enable STNP over DHCP
     * 'LWIP_SNTP_DEBUG' - enable debugging messages
     *
     */

    wifi_init_sta();
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);\
}

static void
initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);

/*
 * If 'NTP over DHCP' is enabled, we set dynamic pool address
 * as a 'secondary' server. It will act as a fallback server in case that address
 * provided via NTP over DHCP is not accessible
 */
#if LWIP_DHCP_GET_NTP_SRV && SNTP_MAX_SERVERS > 1
    sntp_setservername(1, "pool.ntp.org");

#if LWIP_IPV6 && SNTP_MAX_SERVERS > 2          // statically assigned IPv6 address is also possible
    ip_addr_t ip6;
    if (ipaddr_aton("2a01:3f7::1", &ip6)) {    // ipv6 ntp source "ntp.netnod.se"
        sntp_setserver(2, &ip6);
    }
#endif  /* LWIP_IPV6 */

#else   /* LWIP_DHCP_GET_NTP_SRV && (SNTP_MAX_SERVERS > 1) */
    // otherwise, use DNS address from a pool
    sntp_setservername(0, "pool.ntp.org");
#endif

    sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    sntp_init();

    ESP_LOGI(TAG, "List of configured NTP servers:");

    for (uint8_t i = 0; i < SNTP_MAX_SERVERS; ++i){
        if (sntp_getservername(i)){
            ESP_LOGI(TAG, "server %d: %s", i, sntp_getservername(i));
        } else {
            // we have either IPv4 or IPv6 address, let's print it
            char buff[INET6_ADDRSTRLEN];
            ip_addr_t const *ip = sntp_getserver(i);
            if (ipaddr_ntoa_r(ip, buff, INET6_ADDRSTRLEN) != NULL)
                ESP_LOGI(TAG, "server %d: %s", i, buff);
        }
    }
}

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static void
event_handler(void *arg,
              esp_event_base_t event_base,
              int32_t event_id,
              void *event_data) {

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
      if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
          esp_wifi_connect();
          s_retry_num++;
          ESP_LOGI(TAG, "retry to connect to the AP");
      }
      else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      }
      ESP_LOGI(TAG,"connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void
wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#ifdef LWIP_DHCP_GET_NTP_SRV
    sntp_servermode_dhcp(1);      // accept NTP offers from DHCP server, if any
#endif

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);

        httpd_handle_t server;
        printf("Trying to start webserver\n");
        server = start_webserver();

    }
    else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

static void
ledc_init(int gpio_num,
          int ledc_channel_num,
          int ledc_timer_num) {
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = ledc_timer_num,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 5 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = ledc_channel_num,
        .timer_sel      = ledc_timer_num,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = gpio_num,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void
app_main(void) {
    ++boot_count;
    ESP_LOGI(TAG, "Boot count: %d", boot_count);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }

    char strftime_buf[64];

    // Set timezone to Eastern Standard Time and print local time
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in New York is: %s", strftime_buf);

    if (sntp_get_sync_mode() == SNTP_SYNC_MODE_SMOOTH) {
        struct timeval outdelta;
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS) {
            adjtime(NULL, &outdelta);
            ESP_LOGI(TAG, "Waiting for adjusting time ... outdelta = %jd sec: %li ms: %li us",
                        (intmax_t)outdelta.tv_sec,
                        outdelta.tv_usec/1000,
                        outdelta.tv_usec%1000);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
    pumpEventsHandle = xQueueCreateStatic(PUMP_EV_NUM, sizeof (struct event_t), queueStorage, &pumpEvents);
    timerEventsHandle = xQueueCreateStatic(PUMP_EV_NUM, sizeof (struct cron_t), timerQueueStorage, &timerEvents);

    configASSERT(pumpEventsHandle);
    configASSERT(timerEventsHandle);

    pumpTimer = xTimerCreateStatic("pump timer", PUMP_CB_PERIOD, pdTRUE, (void*)0, pumpTimerCb, &pumpTimerBuffer);
    xTimerStart(pumpTimer, 0);

    // Pump stuff
    // Set the LEDC peripheral configuration
    ledc_init(18, 0, 0);
    ledc_init(19, 1, 1);

    i2c_init(I2C_BUS, I2C_SCL_PIN, I2C_SDA_PIN, I2C_FREQ_100K);

    // Create the sensors, multiple sensors are possible.
    sensor = sht3x_init_sensor(I2C_BUS, SHT3x_ADDR_1);

    createPumpRunnerTask();
}
