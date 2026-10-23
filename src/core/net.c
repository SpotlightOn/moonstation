#include "net.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "settings.h"

static const char *TAG = "net";

// True once netif, event loop and driver are set up. The daily resync and
// the setup server bring the radio up more than once per boot, and creating
// the default netif twice would register a second station interface.
static bool wifi_ready = false;

// Number of link holders, see net_wifi_hold().
static int hold_count = 0;

bool net_wifi_configured(void)
{
    return settings.wifi_ssid[0] != '\0';
}

void net_wifi_hold(void)
{
    hold_count++;
    // A holder serves a request and waits for an answer; modem sleep would
    // add up to a second of latency to every packet of that exchange.
    if (hold_count == 1 && wifi_ready) esp_wifi_set_ps(WIFI_PS_NONE);
}

void net_wifi_release(void)
{
    if (hold_count > 0) hold_count--;
    if (hold_count == 0 && wifi_ready) esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

// Brings up the station interface and waits for an address. Returns false
// when the AP does not answer, so the caller can skip the pointless SNTP
// wait instead of burning its timeout on a link that never came up.
bool net_wifi_up(void)
{
    if (!net_wifi_configured()) {
        ESP_LOGW(TAG, "no WiFi credentials configured");
        return false;
    }

    if (!wifi_ready) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        wifi_ready = true;
    }
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, settings.wifi_ssid, sizeof(wcfg.sta.ssid));
    strncpy((char *)wcfg.sta.password, settings.wifi_password, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.sta.listen_interval = settings.wifi_listen_interval;
    esp_wifi_set_mode(WIFI_MODE_STA);
    // A link that is held is in active use, a link that is not goes back to
    // power saving.
    esp_wifi_set_ps(hold_count > 0 ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();
    esp_wifi_connect();

    // Wait up to 10 s for an IP.
    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "WiFi connected");
            return true;
        }
    }
    ESP_LOGW(TAG, "WiFi connect timeout");
    return false;
}

void net_wifi_down(void)
{
    if (hold_count > 0) {
        ESP_LOGI(TAG, "radio is held, staying up");
        return;
    }
    if (!wifi_ready) return;
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi stopped");
}

bool net_wifi_is_up(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

// Starts SNTP and blocks until the first answer has set the system clock.
// The clock is compiled with and without credentials in a config.h, so the
// wait simply times out and the previous time stays in place when no server
// answers. Returns false on timeout.
bool net_ntp_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // The radio goes off right after this call, so there is nothing to gain
    // from stepping the clock smoothly over minutes.
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, settings.ntp_server);
    esp_sntp_init();

    bool ok = false;
    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        // Only the library itself can tell a real sync from a clock that
        // merely looks valid: set_compile_time() already puts the system
        // time far above any epoch threshold.
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ok = true;
            break;
        }
    }
    // Stop the poll timer either way: a leftover timer would keep waking the
    // radio after the caller has taken it down again.
    esp_sntp_stop();

    if (ok) {
        ESP_LOGI(TAG, "NTP synced from %s", settings.ntp_server);
    } else {
        ESP_LOGW(TAG, "NTP sync timeout, keeping the time from before");
    }
    return ok;
}
