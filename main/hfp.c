#include "hfp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_i2s.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hfp";
#if HFP_AUTO_DIAL
static const char *HFP_TEST_NUMBER = "08064853902";
#endif
// static const char *HFP_AUTO_CONNECT_BDA = "08:3a:f2:22:a3:aa";
// static const char *HFP_AUTO_CONNECT_BDA = "f4:65:a6:74:f3:58"; // 13 Pro
static const char *HFP_AUTO_CONNECT_BDA = "fc:2a:9c:2b:50:44"; // XS Max

#define HFP_AUTO_ANSWER 1
#define HFP_AUTO_DIAL 0
#define HFP_AUTO_AUDIO 1
#define HFP_AUTO_CONNECT_DELAY_MS 5000
#define HFP_SEND_NREC 1

static volatile esp_hf_client_connection_state_t g_conn_state =
    ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED;
static volatile esp_hf_client_audio_state_t g_audio_state = ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED;
static bool g_msbc_warned = false;

static void log_bda(const char *label, const esp_bd_addr_t bda) {
    ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x", label,
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool parse_bda(const char *str, esp_bd_addr_t out) {
    int v[6];
    if (!str || str[0] == '\0') {
        return false;
    }
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static const char *call_status_to_str(esp_hf_call_status_t status) {
    switch (status) {
    case ESP_HF_CALL_STATUS_NO_CALLS:
        return "no_calls";
    case ESP_HF_CALL_STATUS_CALL_IN_PROGRESS:
        return "in_progress";
    default:
        return "unknown";
    }
}

static const char *call_setup_to_str(esp_hf_call_setup_status_t status) {
    switch (status) {
    case ESP_HF_CALL_SETUP_STATUS_IDLE:
        return "idle";
    case ESP_HF_CALL_SETUP_STATUS_INCOMING:
        return "incoming";
    case ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING:
        return "outgoing_dialing";
    case ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING:
        return "outgoing_alerting";
    default:
        return "unknown";
    }
}

static const char *call_held_to_str(esp_hf_call_held_status_t status) {
    switch (status) {
    case ESP_HF_CALL_HELD_STATUS_NONE:
        return "none";
    case ESP_HF_CALL_HELD_STATUS_HELD_AND_ACTIVE:
        return "held_and_active";
    case ESP_HF_CALL_HELD_STATUS_HELD:
        return "held";
    default:
        return "unknown";
    }
}

static const char *audio_state_to_str(esp_hf_client_audio_state_t state) {
    switch (state) {
    case ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED:
        return "disconnected";
    case ESP_HF_CLIENT_AUDIO_STATE_CONNECTING:
        return "connecting";
    case ESP_HF_CLIENT_AUDIO_STATE_CONNECTED:
        return "connected";
    case ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC:
        return "connected_msbc";
    default:
        return "unknown";
    }
}

static const char *at_response_to_str(esp_hf_at_response_code_t code) {
    switch (code) {
    case ESP_HF_AT_RESPONSE_CODE_OK:
        return "OK";
    case ESP_HF_AT_RESPONSE_CODE_ERR:
        return "ERROR";
    case ESP_HF_AT_RESPONSE_CODE_NO_CARRIER:
        return "NO_CARRIER";
    case ESP_HF_AT_RESPONSE_CODE_BUSY:
        return "BUSY";
    case ESP_HF_AT_RESPONSE_CODE_NO_ANSWER:
        return "NO_ANSWER";
    case ESP_HF_AT_RESPONSE_CODE_DELAYED:
        return "DELAYED";
    case ESP_HF_AT_RESPONSE_CODE_BLACKLISTED:
        return "BLACKLISTED";
    case ESP_HF_AT_RESPONSE_CODE_CME:
        return "CME";
    default:
        return "UNKNOWN";
    }
}

static void log_peer_features(uint32_t feat) {
    ESP_LOGI(TAG, "Peer features: 0x%08x%s%s%s%s",
             feat,
             (feat & ESP_HF_CLIENT_PEER_FEAT_CODEC) ? " CODEC" : "",
             (feat & ESP_HF_CLIENT_PEER_FEAT_ESCO_S4) ? " ESCO_S4" : "",
             (feat & ESP_HF_CLIENT_PEER_FEAT_ECNR) ? " ECNR" : "",
             (feat & ESP_HF_CLIENT_PEER_FEAT_INBAND) ? " INBAND" : "");
}

static void hfp_audio_data_cb(esp_hf_sync_conn_hdl_t sync_conn_hdl,
                              esp_hf_audio_buff_t *audio_buf,
                              bool is_bad_frame) {
    static uint32_t rx_frames = 0;
    rx_frames++;
    if ((rx_frames % 50) == 0) {
        ESP_LOGI(TAG, "Audio RX frames: %u (hdl=%u, bad=%d)", rx_frames, sync_conn_hdl, is_bad_frame);
    }
    esp_hf_client_audio_buff_free(audio_buf);
}

static void hfp_data_in_cb(const uint8_t *buf, uint32_t len) {
    static uint32_t rx_bytes = 0;
    if (g_audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
        return;
    }
    rx_bytes += len;
    if ((rx_bytes % 1600) < len) {
        ESP_LOGI(TAG, "Audio RX bytes: %u", rx_bytes);
    }
    audio_i2s_push_hfp_audio(buf, len);
    (void)buf;
}

static uint32_t hfp_data_out_cb(uint8_t *buf, uint32_t len) {
    (void)buf;
    (void)len;
    return 0;
}

static void try_auto_connect(void) {
    if (g_conn_state != ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
        ESP_LOGI(TAG, "Skip auto-connect: state=%d", g_conn_state);
        return;
    }

    esp_bd_addr_t target_bda;
    if (parse_bda(HFP_AUTO_CONNECT_BDA, target_bda)) {
        log_bda("Auto-connect to fixed device:", target_bda);
        esp_err_t err = esp_hf_client_connect(target_bda);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Auto-connect failed: %s", esp_err_to_name(err));
        }
        return;
    }

    int dev_num = esp_bt_gap_get_bond_device_num();
    if (dev_num <= 0) {
        ESP_LOGI(TAG, "No bonded device for auto-connect");
        return;
    }
    esp_bd_addr_t *dev_list = (esp_bd_addr_t *)calloc(dev_num, sizeof(esp_bd_addr_t));
    if (!dev_list) {
        ESP_LOGW(TAG, "No memory for bonded device list");
        return;
    }
    if (esp_bt_gap_get_bond_device_list(&dev_num, dev_list) == ESP_OK && dev_num > 0) {
        log_bda("Auto-connect to bonded device:", dev_list[0]);
        esp_err_t err = esp_hf_client_connect(dev_list[0]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Auto-connect failed: %s", esp_err_to_name(err));
        }
    }
    free(dev_list);
}

static void auto_connect_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(HFP_AUTO_CONNECT_DELAY_MS));
    try_auto_connect();
    vTaskDelete(NULL);
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Pairing success with %s", param->auth_cmpl.device_name);
            log_bda("Paired device:", param->auth_cmpl.bda);
            esp_err_t err = esp_hf_client_connect(param->auth_cmpl.bda);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Connect after pairing failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Pairing failed: status=%d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirm requested, accepting");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code;
        memset(pin_code, '0', sizeof(pin_code));
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        ESP_LOGI(TAG, "PIN requested, replied with 0000");
        break;
    }
    default:
        break;
    }
}

static void hfp_callback(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param) {
    static bool auto_answer_sent = false;
#if HFP_AUTO_DIAL
    static bool auto_dial_sent = false;
#endif
    static bool audio_requested = false;
    static esp_bd_addr_t active_bda = {0};
    static bool have_active_bda = false;
    static esp_hf_client_audio_state_t audio_state = ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED;

    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "HFP connection state: %d", param->conn_stat.state);
        g_conn_state = param->conn_stat.state;
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED ||
            param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            memcpy(active_bda, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            have_active_bda = true;
        }
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            log_peer_features(param->conn_stat.peer_feat);
#if HFP_SEND_NREC
            esp_err_t nrec_err = esp_hf_client_send_nrec();
            if (nrec_err != ESP_OK) {
                ESP_LOGW(TAG, "Send NREC failed: %s", esp_err_to_name(nrec_err));
            }
#endif
#if HFP_AUTO_DIAL
            if (!auto_dial_sent) {
                esp_err_t err = esp_hf_client_dial(HFP_TEST_NUMBER);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Dialing test number: %s", HFP_TEST_NUMBER);
                    auto_dial_sent = true;
                } else {
                    ESP_LOGW(TAG, "Dial failed: %s", esp_err_to_name(err));
                }
            }
#endif
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
#if HFP_AUTO_DIAL
            auto_dial_sent = false;
#endif
            have_active_bda = false;
        }
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "HFP audio state: %s (hdl=%u, frame=%u)",
                 audio_state_to_str(param->audio_stat.state),
                 param->audio_stat.sync_conn_handle,
                 param->audio_stat.preferred_frame_size);
        audio_state = param->audio_stat.state;
        g_audio_state = audio_state;
        if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
            param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            esp_hf_client_outgoing_data_ready();
        }
        if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            audio_i2s_set_hfp_enabled(true);
        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            audio_i2s_set_hfp_enabled(false);
            if (!g_msbc_warned) {
                ESP_LOGW(TAG, "mSBC audio received but decode is not implemented; muting.");
                g_msbc_warned = true;
            }
        }
        if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            audio_requested = false;
            audio_i2s_set_hfp_enabled(true);
        }
        break;
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGI(TAG, "Call status: %s", call_status_to_str(param->call.status));
#if HFP_AUTO_AUDIO
        if (param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS && !audio_requested) {
            if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
                audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
                ESP_LOGI(TAG, "Audio already connected, skip request");
                break;
            }
            if (!have_active_bda) {
                ESP_LOGW(TAG, "Audio connect skipped: no active BDA");
                break;
            }
            esp_err_t err = esp_hf_client_connect_audio(active_bda);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Audio connect requested");
                audio_requested = true;
            } else {
                ESP_LOGW(TAG, "Audio connect failed: %s", esp_err_to_name(err));
            }
        }
        if (param->call.status == ESP_HF_CALL_STATUS_NO_CALLS && audio_requested) {
            if (!have_active_bda) {
                ESP_LOGW(TAG, "Audio disconnect skipped: no active BDA");
                audio_requested = false;
                break;
            }
            esp_err_t err = esp_hf_client_disconnect_audio(active_bda);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Audio disconnect requested");
            } else {
                ESP_LOGW(TAG, "Audio disconnect failed: %s", esp_err_to_name(err));
            }
            audio_requested = false;
        }
#endif
        break;
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGI(TAG, "Call setup: %s", call_setup_to_str(param->call_setup.status));
#if HFP_AUTO_ANSWER
        if (param->call_setup.status == ESP_HF_CALL_SETUP_STATUS_INCOMING && !auto_answer_sent) {
            esp_err_t err = esp_hf_client_answer_call();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Auto-answer sent");
                auto_answer_sent = true;
            } else {
                ESP_LOGW(TAG, "Auto-answer failed: %s", esp_err_to_name(err));
            }
        } else if (param->call_setup.status == ESP_HF_CALL_SETUP_STATUS_IDLE) {
            auto_answer_sent = false;
        }
#endif
        break;
    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
        ESP_LOGI(TAG, "Call held: %s", call_held_to_str(param->call_held.status));
        break;
    case ESP_HF_CLIENT_BSIR_EVT:
        ESP_LOGI(TAG, "In-band ring: %d", param->bsir.state);
        break;
    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        ESP_LOGI(TAG, "AT response: %s (cme=%d)",
                 at_response_to_str(param->at_response.code),
                 param->at_response.cme);
        break;
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        ESP_LOGI(TAG, "HFP profile state: %d", param->prof_stat.state);
        break;
    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "HFP ring indication");
        break;
    case ESP_HF_CLIENT_CLIP_EVT:
        ESP_LOGI(TAG, "Caller ID: %s", param->clip.number);
        break;
    case ESP_HF_CLIENT_CCWA_EVT:
        ESP_LOGI(TAG, "Call waiting: %s", param->ccwa.number);
        break;
    default:
        break;
    }
}

esp_err_t hfp_init(void) {
    esp_err_t err;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_register_callback(gap_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_hf_client_register_callback(hfp_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HFP callback register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_hf_client_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HFP client init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_hf_client_register_audio_data_callback(hfp_audio_data_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio data callback register failed: %s", esp_err_to_name(err));
    }
    err = esp_hf_client_register_data_callback(hfp_data_in_cb, hfp_data_out_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Legacy data callback register failed: %s", esp_err_to_name(err));
    }

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

    esp_bt_gap_set_device_name("Kurodenwa");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    xTaskCreate(auto_connect_task, "hfp_auto_connect", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "HFP initialized. Pair from phone (search \"Kurodenwa\").");
    return ESP_OK;
}
