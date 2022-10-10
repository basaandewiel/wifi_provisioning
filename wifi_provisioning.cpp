#include <esp_log.h>
#include "wifi_provisioning.h"
#include <nvs_flash.h>
#include <cstring>

#define ESP_WIFI_SOFTAP_SSID "ESP32"
#define ESP_WIFI_SOFTAP_PASS "" // no password means open network
#define ESP_WIFI_SOFTAP_CHANNEL 11
#define ESP_WIFI_SOFTAP_MAX_STA_CONN 4
#define ESP_MAXIMUM_RETRY 10

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Index.html that asks for wifi credentials
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

namespace WIFI_PROVISIONING
{
      // define local static variables; these are not part of the class
      static int s_retry_num = 0;                   // count nbr of retries
      static EventGroupHandle_t s_wifi_event_group; // FreeRTOS event group to signal when connected to Wifi
      static httpd_handle_t httpd_handle = NULL;    // handle of HTTP server
      static esp_netif_t *esp_netif_handler;
      static const char *TAG = "WIFI_PROVISIONING"; // used in ESP_LOGx

      // init static class variables (no instance of class required)
      bool wifi_provisioning::network_credentials_sta_set{false};
      wifi_config_t glob_wifi_config = {}; // used to store wifi_config to connect to network

      // Constructor
      wifi_provisioning::wifi_provisioning()
      {
            ESP_LOGI(TAG, "Constructor");
            valid_wifi_credentials_in_NVS = true;
            wifi_provisioning::network_credentials_sta_set = false; // static var
      }                                                             // Constructor

      bool wifi_provisioning::connect_to_network()
      {
            bool ret = false;
            ESP_LOGI(TAG, "METHOD Connect_to_network");
            if (!_credentials_stored_in_NVS())
            {
                  _start_soft_AP_mode_and_get_credentials();
            }
            if (_connect_to_network())
            {
                  ret = true;
                  _save_credentials();
            }
            return ret;
      }

      /**
       * @brief check whether wifi credentials are stored in NVS; when credentials are valid, they are copied in glob_wifi_config
       *
       * @return true if wifi credentials are stored in NVS
       * @return false otherwise
       */
      bool wifi_provisioning::_credentials_stored_in_NVS()
      {
            bool return_value = false;
            size_t nvs_str_size;
            nvs_handle_t nvs_handle;

            ESP_LOGD(TAG, "Opening Non-Volatile Storage (NVS) handle... ");
            esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
            if (err != ESP_OK)
            {
                  ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
                  return_value = false;
            }
            else
            {
                  ESP_LOGD(TAG, "Reading SSID from NVS ... ");
                  err = nvs_get_str(nvs_handle, "nvs_ssid", NULL, &nvs_str_size);
                  char *ssid = (char *)malloc(nvs_str_size);
                  err = nvs_get_str(nvs_handle, "nvs_ssid", ssid, &nvs_str_size);

                  switch (err)
                  {
                  case ESP_OK:
                        ESP_LOGD(TAG, "NVS_SSID = %s", ssid);
                        // save read ssid into global wifi_config var
                        strcpy((char *)glob_wifi_config.sta.ssid, ssid); // C++ does not allow conversion from char[32] to unint8_t[32]
                        return_value = true;
                        break;
                  case ESP_ERR_NVS_NOT_FOUND:
                        ESP_LOGD(TAG, "SSID is not initialized yet!");
                        break;
                  default:
                        ESP_LOGE(TAG, "Error (%s) reading!", esp_err_to_name(err));
                  }

                  err = nvs_get_str(nvs_handle, "nvs_password", NULL, &nvs_str_size);
                  char *password = (char *)malloc(nvs_str_size);
                  err = nvs_get_str(nvs_handle, "nvs_password", password, &nvs_str_size);

                  switch (err)
                  {
                  case ESP_OK:
                        ESP_LOGD(TAG, "nvs_password = %s", password);
                        // save read password into global wifi_config var
                        strcpy((char *)glob_wifi_config.sta.password, password); // C++ does not allow conversion from char[32] to unint8_t[32]
                        return_value = true;
                        break;
                  case ESP_ERR_NVS_NOT_FOUND:
                        ESP_LOGD(TAG, "PASSWORD is not initialized yet!\n");
                        return_value = false;
                        break;
                  default:
                        ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
                        return_value = false;
                  }
                  nvs_close(nvs_handle);
                  ESP_LOGI(TAG, "wifi_credentials_stored_in_NVS= %d", return_value);
            }
            return return_value;
      }

      /**
       * @brief Decode strings from the URL; for instance replace hex codes by ASCII characters (like @)
       *
       * @param dst destination with 'normal' characters
       * @param src sourcing URL string possible hex encoded special characters
       */
      void urldecode2(char *dst, const char *src)
      {
            char a, b;
            while (*src)
            {
                  if ((*src == '%') &&
                      ((a = src[1]) && (b = src[2])) &&
                      (isxdigit(a) && isxdigit(b)))
                  {
                        if (a >= 'a')
                              a -= 'a' - 'A';
                        if (a >= 'A')
                              a -= ('A' - 10);
                        else
                              a -= '0';
                        if (b >= 'a')
                              b -= 'a' - 'A';
                        if (b >= 'A')
                              b -= ('A' - 10);
                        else
                              b -= '0';
                        *dst++ = 16 * a + b;
                        src += 3;
                  }
                  else if (*src == '+')
                  {
                        *dst++ = ' ';
                        src++;
                  }
                  else
                  {
                        *dst++ = *src++;
                  }
            }
            *dst++ = '\0';
      }

      /**
       * @brief Ask for wifi credentials, and/or handle them and put them in global glob_wifi_config
       *
       * @param req HTML request
       * @return esp_err_t
       */
      esp_err_t wifi_provisioning::setWifiParams(httpd_req_t *req)
      {
            ESP_LOGI(TAG, "setWifiParams");
            // present page for requesting wifi credentials
            httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);

            char *buf;
            size_t buf_len;

            /* Read URL query string length and allocate memory for length + 1,
             * extra byte for null termination */
            buf_len = httpd_req_get_url_query_len(req) + 1;
            if (buf_len > 1)
            {
                  buf = (char *)malloc(buf_len);
                  if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
                  {
                        ESP_LOGI(TAG, "Found URL query => %s", buf);
                        char urlpart[32];
                        char ssid[32];
                        char passkey[32];
                        // Get value of expected key from query string
                        if (httpd_query_key_value(buf, "ssid", urlpart, sizeof(urlpart)) == ESP_OK)
                        {
                              urldecode2(ssid, urlpart);
                              ESP_LOGI(TAG, "Found network SSID =%s", ssid);
                              strcpy((char *)glob_wifi_config.sta.ssid, ssid); // C++ does not allow conversion from char[32] to unint8_t[32]
                        }
                        if (httpd_query_key_value(buf, "passkey", urlpart, sizeof(urlpart)) == ESP_OK)
                        {
                              urldecode2(passkey, urlpart);
                              ESP_LOGI(TAG, "Found network PASSKEY =%s", passkey);
                              strcpy((char *)glob_wifi_config.sta.password, passkey); // C++ does not allow conversion from char[32] to unint8_t[32]
                        }
                        wifi_provisioning::network_credentials_sta_set = true;
                  }
                  free(buf);
            }
            return ESP_OK;
      }

      /**
       * @brief event handler, handling both soft_AP and STAT mode
       *
       * @param arg not used
       * @param event_base type of event
       * @param event_id Id of event to be handled
       * @param event_data Data belonging to Id
       */
      static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
      {
            if (event_id == WIFI_EVENT_AP_STACONNECTED)
            { // softAP
                  wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                  ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                           MAC2STR(event->mac), event->aid);
            }
            else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
            { // softAP
                  wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                  ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                           MAC2STR(event->mac), event->aid);
            }
            else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
            { // STA mode
                  esp_wifi_connect();
            }
            else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
            { // STA mode
                  if (s_retry_num < ESP_MAXIMUM_RETRY)
                  {
                        esp_wifi_connect();
                        s_retry_num++;
                        ESP_LOGI(TAG, "retry to connect to the AP");
                  }
                  else
                  {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                  }
                  ESP_LOGI(TAG, "connect to the AP fail");
            }
            else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
            { // STA mode
                  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                  ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                  s_retry_num = 0;
                  xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
            else if (event_id == WIFI_EVENT_AP_STACONNECTED)
            { // STA mode
                  wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                  ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                           MAC2STR(event->mac), event->aid);
            }
            else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
            { // STA mode
                  wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                  ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                           MAC2STR(event->mac), event->aid);
            }
      }

      /**
       * @brief Init wifi in softAP mode, so that wifi credentials can be asked for
       *
       */
      void wifi_provisioning::wifi_init_softap(void)
      {
            ESP_LOGI(TAG, "Start Wifi in SoftAP mode");
            ESP_ERROR_CHECK(esp_netif_init()); // Initialize the underlying TCP/IP stack; only call once
            ESP_ERROR_CHECK(esp_event_loop_create_default());
            esp_netif_handler = esp_netif_create_default_wifi_ap(); // create esp_netif object with default WiFi access point config, attaches the netif to wifi and registers default wifi handlers

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg)); // Initialize WiFi Allocate resource for WiFi driver, such as WiFi control structure, RX/TX buffer, WiFi NVS structure etc. This WiFi also starts WiFi task.

            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                &wifi_event_handler,
                                                                NULL,
                                                                NULL));

            wifi_config_t wifi_config = {};
            strcpy((char *)wifi_config.ap.ssid, ESP_WIFI_SOFTAP_SSID); // C++ does not allow conversion from cons string to unin8[32]
            wifi_config.ap.ssid_len = strlen(ESP_WIFI_SOFTAP_SSID);
            wifi_config.ap.channel = ESP_WIFI_SOFTAP_CHANNEL;
            strcpy((char *)wifi_config.ap.password, ESP_WIFI_SOFTAP_PASS);
            wifi_config.ap.max_connection = ESP_WIFI_SOFTAP_MAX_STA_CONN;
            wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

            if (strlen(ESP_WIFI_SOFTAP_PASS) == 0)
            {
                  wifi_config.ap.authmode = WIFI_AUTH_OPEN;
            }

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));               // set wifi operating mode
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config)); // set wifi config
            ESP_ERROR_CHECK(esp_wifi_start());                              // Start WiFi according to current configuration

            ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
                     ESP_WIFI_SOFTAP_SSID, ESP_WIFI_SOFTAP_PASS, ESP_WIFI_SOFTAP_CHANNEL);
            ESP_LOGI(TAG, "wifi_init_softap - end");
      } // wifi_init_softap

      /**
       * @brief Process HTTP GET from root URL; if nov valid wifi credentials are stored in NVS, wifi credentials are asked for.
       *        Otherwise the GET is handled depending of the type of user logged in, or no user logged in at all
       *
       * @param req
       * @return esp_err_t
       */
      esp_err_t wifi_provisioning::index_get_handler(httpd_req_t *req)
      {
            ESP_LOGI(TAG, "index_get_handler");

            if (!_credentials_stored_in_NVS())
            {
                  setWifiParams(req); // in SoftAP mode, ask for wifi credentials
            }
            return ESP_OK;
      }

      /**
       * @brief Start HTTP server
       *
       */
      void wifi_provisioning::startHTTPServer()
      {
            httpd_config_t config = HTTPD_DEFAULT_CONFIG();
            config.stack_size = 8000; // to avoid stack overflow

            // httpd_uri_t logout_uri = {
            //     .uri = "/logout",
            //     .method = HTTP_GET,
            //     .handler = logout_get_handler,
            //     .user_ctx = NULL};

            httpd_uri_t index_uri = {
                .uri = "/",
                .method = HTTP_GET,
                //.handler = std::bind(&wifi_provisioning::bind_get_handler, this, std::placeholders::_1),
                //.handler = std::bind(&wifi_provisioning::bind_get_handler, this, std::placeholders::_1),
                .handler = index_get_handler,
                .user_ctx = NULL};

            httpd_uri_t setWifiParams_uri = {
                .uri = "/control", // used in index.html to send ssid and password
                .method = HTTP_GET,
                .handler = setWifiParams,
                .user_ctx = NULL};

            // httpd_uri_t update_post = {
            //     .uri = "/update_post",
            //     .method = HTTP_POST,
            //     .handler = update_post_handler,
            //     .user_ctx = NULL};

            // if httpd_handle <> NULL, then httpd server is already started
            // if (httpd_handle == NULL)
            // {
            if (httpd_start(&httpd_handle, &config) == ESP_OK)
            {
                  // httpd_register_uri_handler(httpd_handle, &logout_uri);
                  httpd_register_uri_handler(httpd_handle, &index_uri);
                  httpd_register_uri_handler(httpd_handle, &setWifiParams_uri);
                  // httpd_register_basic_auth();                            // initialize user credentials
                  // httpd_register_uri_handler(httpd_handle, &update_post); // for OTA
            }
            // }
            ESP_LOGI(TAG, "HTTP server started");
      }

      void wifi_provisioning::_start_soft_AP_mode_and_get_credentials()
      {
            ESP_LOGI(TAG, "start_soft_AP_mode_and_get_credentials");
            valid_wifi_credentials_in_NVS = false;
            // start softAP; user can connect to this SSID
            wifi_init_softap();

            // start httpd server, so that wifi creds can be input via web page
            startHTTPServer();

            ESP_LOGI(TAG, "waiting for wifi credentials");
            while (network_credentials_sta_set == false)
            {
                  vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            ESP_LOGI(TAG, "network credentials received via webpage");
            httpd_stop(httpd_handle); // stop http server to get credentials

            ESP_ERROR_CHECK(esp_wifi_stop());
            ESP_ERROR_CHECK(esp_wifi_clear_default_wifi_driver_and_handlers(esp_netif_handler)); // unregister default wifi handlers and detach the created object from the wifi
            ESP_ERROR_CHECK(esp_event_loop_delete_default());
            esp_netif_destroy(esp_netif_handler); // so state is always no netif present; important when credentials are saved and STA mode is directly started

            return;
      }

      /**
       * @brief Start wifi in STA mode and wait until either the connection is established or connection failed for the maximum number of re-tries
       *
       */
      bool wifi_provisioning::wifi_init_sta_try_to_connect_to_wifi(void)
      {
            ESP_LOGI(TAG, "wifi_init_sta_try_to_connect_to_wifi");
            bool ret = false;

            ESP_ERROR_CHECK(esp_netif_init());                // possible object esp_netif used in SofAP is there also_destroyed
            ESP_ERROR_CHECK(esp_event_loop_create_default()); // when executed afer wifi_init_softap create_default is called twice ->panci

            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                &wifi_event_handler,
                                                                NULL,
                                                                NULL));

            esp_event_handler_instance_t instance_got_ip;
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                                IP_EVENT_STA_GOT_IP, // only for GOT_IP event
                                                                &wifi_event_handler, // this handler is for both SoftAP as well as STA
                                                                NULL,
                                                                &instance_got_ip));

            esp_netif_create_default_wifi_sta();

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg));

            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            glob_wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            glob_wifi_config.sta.pmf_cfg.capable = true;
            glob_wifi_config.sta.pmf_cfg.required = false;
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &glob_wifi_config));

            ESP_ERROR_CHECK(esp_wifi_start());

            ESP_LOGI(TAG, "wait for ESP to connect to network with credentials supplied");

            /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
             * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
            s_wifi_event_group = xEventGroupCreate();
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   portMAX_DELAY);

            /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
             * happened. */
            const char *const_ssid = (char *)&glob_wifi_config.sta.ssid;         // Convert uint8* to char* to const char * (latest conversion cannot be casted)
            const char *const_password = (char *)&glob_wifi_config.sta.password; // Convert uint8* to char* to const char * (latest conversion cannot be casted)
            if (bits & WIFI_CONNECTED_BIT)                                       // connection to Wifi was established
            {
                  ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                           const_ssid, const_password);
                  ret = true;

                  // save wifi credentials to NVS, only when they are obtained via captive portal
                  // so not when credentials were retrieved from NVS storage; do not write unnecessary to NVS
                  if (!valid_wifi_credentials_in_NVS)
                  {
                        ESP_LOGI(TAG, "save wifi credentials to NVS");
                        nvs_handle_t nvs_handle;
                        nvs_open("storage", NVS_READWRITE, &nvs_handle);
                        nvs_set_str(nvs_handle, "nvs_ssid", const_ssid);
                        nvs_set_str(nvs_handle, "nvs_password", const_password);
                        nvs_close(nvs_handle);
                  }
            }
            else if (bits & WIFI_FAIL_BIT) // could not connect to Wifi network
            {
                  ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                           const_ssid, const_password);
                  ret = false;
            }
            else
            {
                  ESP_LOGE(TAG, "UNEXPECTED EVENT");
            }
            return ret;
            // Do not unregister and delete EventGroup; these are probably needed if wifi connection is for instance temporarely unavailable
            // ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
            // vEventGroupDelete(s_wifi_event_group);
      } // wifi_init_sta_and_save_creds

      bool wifi_provisioning::_connect_to_network()
      {
            bool ret = false;
            ESP_LOGI(TAG, "_connect_to_network");
            // COND: either wifi credentials in NVS were valid, or are supplied via softAP
            if (wifi_init_sta_try_to_connect_to_wifi()) // start wifi in STA mode, and try to connect to wifi network
            {
                  ret = true;
                  esp_netif_ip_info_t ip_info;
                  esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
                  esp_wifi_set_ps(WIFI_PS_NONE); // disable any WiFi power save mode, this allows best throughput
            }
            return ret;
      }

      esp_err_t wifi_provisioning::_save_credentials()
      {
            return ESP_OK;
      }

} // namespace