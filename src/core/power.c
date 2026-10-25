#include "power.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "sdkconfig.h"

#include "features.h"

static const char *TAG = "power";

// The ceiling of each profile. The idle floor and the light sleep switch are
// build-time values, see features.h.
#define ACTIVE_MAX_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define BOOST_MAX_MHZ 240

static const char *const PROFILE_NAMES[POWER_PROFILE_COUNT] = {
    "active",
    "boost",
};

static int requests[POWER_PROFILE_COUNT];

// POWER_PROFILE_COUNT until the first apply(), so init always configures.
static power_profile_t current = POWER_PROFILE_COUNT;

static esp_pm_lock_handle_t boost_lock = NULL;
static bool boost_lock_ready = false;

// Picks the highest requested profile and applies it. Only the differences to
// the current profile cost a call into the power manager.
static void apply(void)
{
    power_profile_t want = POWER_PROFILE_ACTIVE;
    for (int i = POWER_PROFILE_COUNT - 1; i >= 0; i--) {
        if (requests[i] > 0) {
            want = (power_profile_t)i;
            break;
        }
    }
    if (want == current) return;

    const int max_mhz =
        want == POWER_PROFILE_BOOST ? BOOST_MAX_MHZ : ACTIVE_MAX_MHZ;
    esp_pm_config_t cfg = {
        .max_freq_mhz = max_mhz,
        .min_freq_mhz = PM_MIN_CPU_MHZ,
        .light_sleep_enable = PM_LIGHT_SLEEP,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "profile %s not applied: %s", PROFILE_NAMES[want],
                 esp_err_to_name(err));
        return;
    }

    // A CPU_FREQ_MAX lock keeps boost at 240 MHz while the page waits for the
    // next request; without it the clock would drop between two requests.
    if (want == POWER_PROFILE_BOOST && boost_lock_ready) {
        esp_pm_lock_acquire(boost_lock);
    } else if (current == POWER_PROFILE_BOOST && boost_lock_ready) {
        esp_pm_lock_release(boost_lock);
    }

    current = want;
    ESP_LOGI(TAG, "profile %s: %d..%d MHz", PROFILE_NAMES[want],
             cfg.min_freq_mhz, cfg.max_freq_mhz);
}

void power_init(void)
{
    boost_lock_ready =
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "power_boost", &boost_lock) ==
        ESP_OK;
    if (!boost_lock_ready) {
        ESP_LOGW(TAG, "boost lock unavailable, the setup page stays at 80 MHz");
    }

    requests[POWER_PROFILE_ACTIVE] = 1;
    current = POWER_PROFILE_COUNT;
    apply();
}

void power_request(power_profile_t profile)
{
    if (profile >= POWER_PROFILE_COUNT) return;
    requests[profile]++;
    apply();
}

void power_release(power_profile_t profile)
{
    if (profile >= POWER_PROFILE_COUNT) return;
    if (requests[profile] > 0) requests[profile]--;
    apply();
}
