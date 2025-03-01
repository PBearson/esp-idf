// Copyright 2015-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <esp_event.h>
#include <esp_wifi.h>
#include "esp_log.h"
#include "esp_private/wifi.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_private/pm_impl.h"
#include "soc/rtc.h"
#include "esp_wpa.h"
#include "esp_netif.h"
#include "tcpip_adapter_compatible/tcpip_adapter_compat.h"
#include "driver/adc.h"
#include "driver/adc2_wifi_private.h"
#include "esp_coexist_internal.h"
#include "esp_phy_init.h"

#if (CONFIG_ESP32_WIFI_RX_BA_WIN > CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM)
#error "WiFi configuration check: WARNING, WIFI_RX_BA_WIN should not be larger than WIFI_DYNAMIC_RX_BUFFER_NUM!"
#endif

#if (CONFIG_ESP32_WIFI_RX_BA_WIN > (CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM << 1))
#error "WiFi configuration check: WARNING, WIFI_RX_BA_WIN should not be larger than double of the WIFI_STATIC_RX_BUFFER_NUM!"
#endif

ESP_EVENT_DEFINE_BASE(WIFI_EVENT);

extern uint8_t esp_wifi_get_user_init_flag_internal(void);
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_wifi_modem_sleep_lock;
#endif

#if CONFIG_IDF_TARGET_ESP32
/* Callback function to update WiFi MAC time */
wifi_mac_time_update_cb_t s_wifi_mac_time_update_cb = NULL;
#endif

/* Set additional WiFi features and capabilities */
uint64_t g_wifi_feature_caps =
#if CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE
    CONFIG_FEATURE_WPA3_SAE_BIT |
#endif
#if (CONFIG_ESP32_SPIRAM_SUPPORT || CONFIG_ESP32S2_SPIRAM_SUPPORT || CONFIG_ESP32S3_SPIRAM_SUPPORT)
    CONFIG_FEATURE_CACHE_TX_BUF_BIT |
#endif
#if CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT
    CONFIG_FEATURE_FTM_INITIATOR_BIT |
#endif
#if CONFIG_ESP_WIFI_FTM_RESPONDER_SUPPORT
    CONFIG_FEATURE_FTM_RESPONDER_BIT |
#endif
0;

static bool s_wifi_adc_xpd_flag;

static const char* TAG = "wifi_init";

static void __attribute__((constructor)) s_set_default_wifi_log_level(void)
{
    /* WiFi libraries aren't compiled to know CONFIG_LOG_DEFAULT_LEVEL,
       so set it at runtime startup. Done here not in esp_wifi_init() to allow
       the user to set the level again before esp_wifi_init() is called.
    */
    esp_log_level_set("wifi", CONFIG_LOG_DEFAULT_LEVEL);
    esp_log_level_set("mesh", CONFIG_LOG_DEFAULT_LEVEL);
    esp_log_level_set("smartconfig", CONFIG_LOG_DEFAULT_LEVEL);
    esp_log_level_set("ESPNOW", CONFIG_LOG_DEFAULT_LEVEL);
}

static void esp_wifi_set_debug_log(void)
{
    /* set WiFi log level and module */
#if CONFIG_ESP32_WIFI_DEBUG_LOG_ENABLE
    uint32_t g_wifi_log_level = WIFI_LOG_INFO;
    uint32_t g_wifi_log_module = 0;
    uint32_t g_wifi_log_submodule = 0;
#if CONFIG_ESP32_WIFI_DEBUG_LOG_DEBUG
    g_wifi_log_level = WIFI_LOG_DEBUG;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_VERBOSE
    g_wifi_log_level = WIFI_LOG_VERBOSE;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_MODULE_ALL
    g_wifi_log_module = WIFI_LOG_MODULE_ALL;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_MODULE_WIFI
    g_wifi_log_module = WIFI_LOG_MODULE_WIFI;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_MODULE_COEX
    g_wifi_log_module = WIFI_LOG_MODULE_COEX;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_MODULE_MESH
    g_wifi_log_module = WIFI_LOG_MODULE_MESH;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_SUBMODULE_ALL
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_ALL;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_SUBMODULE_INIT
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_INIT;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_SUBMODULE_IOCTL
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_IOCTL;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_SUBMODULE_CONN
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_CONN;
#endif
#if CONFIG_ESP32_WIFI_DEBUG_LOG_SUBMODULE_SCAN
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_SCAN;
#endif
    esp_wifi_internal_set_log_level(g_wifi_log_level);
    esp_wifi_internal_set_log_mod(g_wifi_log_module, g_wifi_log_submodule, true);

#endif /* CONFIG_ESP32_WIFI_DEBUG_LOG_ENABLE*/

}

esp_err_t esp_wifi_deinit(void)
{
    esp_err_t err = ESP_OK;

    if (esp_wifi_get_user_init_flag_internal()) {
        ESP_LOGE(TAG, "Wi-Fi not stop");
        return ESP_ERR_WIFI_NOT_STOPPED;
    }

    esp_supplicant_deinit();
    err = esp_wifi_deinit_internal();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinit Wi-Fi driver (0x%x)", err);
        return err;
    }

#if CONFIG_ESP_NETIF_TCPIP_ADAPTER_COMPATIBLE_LAYER
    tcpip_adapter_clear_default_wifi_handlers();
#endif
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
#if SOC_WIFI_HW_TSF
    esp_pm_unregister_skip_light_sleep_callback(esp_wifi_internal_is_tsf_active);
    esp_pm_unregister_inform_out_light_sleep_overhead_callback(esp_wifi_internal_update_light_sleep_wake_ahead_time);
#endif
#if CONFIG_ESP_WIFI_SLP_IRAM_OPT
    esp_pm_unregister_light_sleep_default_params_config_callback();
#endif
#endif
#if CONFIG_MAC_BB_PD
    esp_unregister_mac_bb_pd_callback(pm_mac_sleep);
    esp_unregister_mac_bb_pu_callback(pm_mac_wakeup);
#endif
    return err;
}

static void esp_wifi_config_info(void)
{
#ifdef CONFIG_ESP32_WIFI_RX_BA_WIN
    ESP_LOGI(TAG, "rx ba win: %d", CONFIG_ESP32_WIFI_RX_BA_WIN);
#endif

#ifdef CONFIG_ESP_NETIF_TCPIP_LWIP
    ESP_LOGI(TAG, "tcpip mbox: %d", CONFIG_LWIP_TCPIP_RECVMBOX_SIZE);
    ESP_LOGI(TAG, "udp mbox: %d", CONFIG_LWIP_UDP_RECVMBOX_SIZE);
    ESP_LOGI(TAG, "tcp mbox: %d", CONFIG_LWIP_TCP_RECVMBOX_SIZE);
    ESP_LOGI(TAG, "tcp tx win: %d", CONFIG_LWIP_TCP_SND_BUF_DEFAULT);
    ESP_LOGI(TAG, "tcp rx win: %d", CONFIG_LWIP_TCP_WND_DEFAULT);
    ESP_LOGI(TAG, "tcp mss: %d", CONFIG_LWIP_TCP_MSS);

#ifdef CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    ESP_LOGI(TAG, "WiFi/LWIP prefer SPIRAM");
#endif

#ifdef CONFIG_ESP32_WIFI_IRAM_OPT
    ESP_LOGI(TAG, "WiFi IRAM OP enabled");
#endif

#ifdef CONFIG_ESP32_WIFI_RX_IRAM_OPT
    ESP_LOGI(TAG, "WiFi RX IRAM OP enabled");
#endif

#ifdef CONFIG_ESP_WIFI_SLP_IRAM_OPT
    ESP_LOGI(TAG, "WiFi SLP IRAM OP enabled");
#endif

#ifdef CONFIG_LWIP_IRAM_OPTIMIZATION
    ESP_LOGI(TAG, "LWIP IRAM OP enabled");
#endif

#else
    ESP_LOGI(TAG, "LWIP disabled");
#endif
}

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
#ifdef CONFIG_PM_ENABLE
    if (s_wifi_modem_sleep_lock == NULL) {
        esp_err_t err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "wifi",
                &s_wifi_modem_sleep_lock);
        if (err != ESP_OK) {
            return err;
        }
    }
#endif

#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
#if CONFIG_MAC_BB_PD
    if (esp_register_mac_bb_pd_callback(pm_mac_sleep) != ESP_OK
        || esp_register_mac_bb_pu_callback(pm_mac_wakeup) != ESP_OK) {

        esp_unregister_mac_bb_pd_callback(pm_mac_sleep);
        esp_unregister_mac_bb_pu_callback(pm_mac_wakeup);
        return ESP_ERR_INVALID_ARG;
    }
#endif

#if CONFIG_ESP_WIFI_SLP_IRAM_OPT
    esp_pm_register_light_sleep_default_params_config_callback(esp_wifi_internal_update_light_sleep_default_params);

    int min_freq_mhz = esp_pm_impl_get_cpu_freq(PM_MODE_LIGHT_SLEEP);
    int max_freq_mhz = esp_pm_impl_get_cpu_freq(PM_MODE_CPU_MAX);
    esp_wifi_internal_update_light_sleep_default_params(min_freq_mhz, max_freq_mhz);

    uint32_t sleep_delay_us = CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME * 1000;
    esp_wifi_set_sleep_delay_time(sleep_delay_us);

    uint32_t keep_alive_time_us = CONFIG_ESP_WIFI_SLP_DEFAULT_MAX_ACTIVE_TIME * 1000 * 1000;
    esp_wifi_set_keep_alive_time(keep_alive_time_us);
#endif

#if SOC_WIFI_HW_TSF
    esp_err_t ret = esp_pm_register_skip_light_sleep_callback(esp_wifi_internal_is_tsf_active);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register skip light sleep callback (0x%x)", ret);
#if CONFIG_MAC_BB_PD
        esp_unregister_mac_bb_pd_callback(pm_mac_sleep);
        esp_unregister_mac_bb_pu_callback(pm_mac_wakeup);
#endif
        return ret;
    }
    ret = esp_pm_register_inform_out_light_sleep_overhead_callback(esp_wifi_internal_update_light_sleep_wake_ahead_time);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register inform light sleep overhead callback (0x%x)", ret);
        return ret;
    }
    esp_sleep_enable_wifi_wakeup();
#endif
#endif

#if CONFIG_ESP_NETIF_TCPIP_ADAPTER_COMPATIBLE_LAYER
    esp_err_t err = tcpip_adapter_set_default_wifi_handlers();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set default Wi-Fi event handlers (0x%x)", err);
    }
#endif
#if CONFIG_SW_COEXIST_ENABLE
    coex_init();
#endif
    esp_err_t result = esp_wifi_init_internal(config);
    if (result == ESP_OK) {
#if CONFIG_MAC_BB_PD
        esp_mac_bb_pd_mem_init();
        esp_wifi_internal_set_mac_sleep(true);
#endif
        esp_wifi_set_debug_log();
#if CONFIG_IDF_TARGET_ESP32
        s_wifi_mac_time_update_cb = esp_wifi_internal_update_mac_time;
#endif

        result = esp_supplicant_init();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init supplicant (0x%x)", result);
            esp_err_t deinit_ret = esp_wifi_deinit();
            if (deinit_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to deinit Wi-Fi (0x%x)", deinit_ret);
            }

            return result;
        }
    }
    adc2_cal_include(); //This enables the ADC2 calibration constructor at start up.

    esp_wifi_config_info();
    return result;
}

#ifdef CONFIG_PM_ENABLE
void wifi_apb80m_request(void)
{
    assert(s_wifi_modem_sleep_lock);
    esp_pm_lock_acquire(s_wifi_modem_sleep_lock);
    if (rtc_clk_apb_freq_get() != APB_CLK_FREQ) {
        ESP_LOGE(__func__, "WiFi needs 80MHz APB frequency to work, but got %dHz", rtc_clk_apb_freq_get());
    }
}

void wifi_apb80m_release(void)
{
    assert(s_wifi_modem_sleep_lock);
    esp_pm_lock_release(s_wifi_modem_sleep_lock);
}
#endif //CONFIG_PM_ENABLE

/* Coordinate ADC power with other modules. This overrides the function from PHY lib. */
// It seems that it is only required on ESP32, but we still compile it for all chips, in case it is
// called by PHY unexpectedly.
void set_xpd_sar(bool en)
{
    if (s_wifi_adc_xpd_flag == en) {
        /* ignore repeated calls to set_xpd_sar when the state is already correct */
        return;
    }

    s_wifi_adc_xpd_flag = en;
    if (en) {
        adc_power_acquire();
    } else {
        adc_power_release();
    }
}

#ifndef CONFIG_ESP_WIFI_FTM_ENABLE
void ieee80211_ftm_attach(void)
{
    /* Do not remove, stub to overwrite weak link in Wi-Fi Lib */
}
#endif
