#include "net.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
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

// Setup access point, see net_ap_start().
static esp_netif_t *ap_netif = NULL;
static bool ap_started = false;
static char ap_ssid[33] = "";

// Number of link holders, see net_wifi_hold().
static int hold_count = 0;

// Creates the station and access point interfaces plus the driver exactly
// once. Both netifs are made here, so a later mode switch neither has to
// create an interface while the radio runs nor depends on the order of the
// first network operation.
static void wifi_stack_init(void)
{
    if (wifi_ready) return;
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_ready = true;
}

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
    if (net_ap_is_up()) {
        // The setup access point owns the radio; joining a network would take
        // the page away from the phone that is configuring the box.
        ESP_LOGW(TAG, "setup access point active, not joining a network");
        return false;
    }
    if (!net_wifi_configured()) {
        ESP_LOGW(TAG, "no WiFi credentials configured");
        return false;
    }

    wifi_stack_init();
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
    if (net_ap_is_up()) {
        ESP_LOGI(TAG, "setup access point active, radio stays up");
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

bool net_ap_is_up(void)
{
    if (!ap_started || !ap_netif) return false;
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

const char *net_ap_ssid(void)
{
    return ap_ssid;
}

// Open access point with the address 192.168.4.1. The name carries the last
// two bytes of the soft AP MAC, so two boxes in the same building do not
// fight over one SSID.
bool net_ap_start(void)
{
    if (net_ap_is_up()) return true;

    wifi_stack_init();
    if (!ap_netif) {
        ESP_LOGE(TAG, "no access point interface");
        return false;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_ssid, sizeof(ap_ssid), "Moonstation-%02X%02X", mac[4], mac[5]);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, ap_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup access point not started: %s", esp_err_to_name(err));
        return false;
    }

    // The DHCP server of the default AP interface starts with it; wait until
    // the interface reports its address, then the page can bind to it.
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            ap_started = true;
            ESP_LOGI(TAG, "setup access point \"%s\" at " IPSTR, ap_ssid,
                     IP2STR(&ip.ip));
            return true;
        }
    }
    ESP_LOGE(TAG, "setup access point got no address");
    esp_wifi_stop();
    return false;
}

void net_ap_stop(void)
{
    if (!ap_started) return;
    ap_started = false;
    esp_wifi_stop();
    ESP_LOGI(TAG, "setup access point stopped");
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
