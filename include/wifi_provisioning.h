#pragma once

#include "esp_err.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "esp_http_server.h"
#include <esp_event.h>
#include "freertos/event_groups.h"

namespace WIFI_PROVISIONING
{
    /**
     * @brief Usage
     * create object, for instance wifi_1
     * IF wifi_1.connect_to_network //get creds from NVS; otherwise ask and connect to network default nbr of retries
     *      main program
     * ELSE
     *      no network connection
     */

    class wifi_provisioning
    {
    private:
        // define static functions and vars; necessary because the http callback function must be a C-like function,
        // and not a method of a class
        static esp_err_t index_get_handler(httpd_req_t *req);
        static bool _credentials_stored_in_NVS();
        static bool network_credentials_sta_set;
        static esp_err_t setWifiParams(httpd_req_t *req);

        void wifi_init_softap();
        void _start_soft_AP_mode_and_get_credentials();
        bool _connect_to_network();
        esp_err_t _save_credentials();
        bool wifi_init_sta_try_to_connect_to_wifi(void);
        void startHTTPServer();
        bool valid_wifi_credentials_in_NVS; // indicated whether valid wifi credentials are saved in NVS

    public:
        /**
         * @brief Construct a new wifi provisioning object
         *
         */
        wifi_provisioning(); // constructor

        bool connect_to_network();

    }; // Class
} // Namespace