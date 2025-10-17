 // src/main.c — ESP32 (ESP-IDF via PlatformIO) com Provisionamento SoftAP + MQTT/TLS + LWT + JSON

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_http_server.h"
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"     // esp_read_mac(..., ESP_MAC_WIFI_STA)
#include "esp_sntp.h"    // esp_sntp_* APIs (IDF 5.x)

#include "mqtt_client.h"
#include "cJSON.h"

// Provisionamento (SOFTAP)
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

#include "certs.h" // BROKER_ROOT_CA_PEM

#include "ota.h" //OTA para atualizar o firmware de forma online

// ============================ CONFIGURAR AQUI ============================
// Identidade e Broker
#define ORG_ID         "app-user-org01"                      // TODO
#define DEVICE_ID      "device-dev01"                      // TODO
#define BROKER_HOST    "x82148af.ala.us-east-1.emqxsl.com"        // TODO (hostname, NUNCA IP)
#define BROKER_PORT    8883
#define MQTT_USER      "device-dev01"               // TODO (usuario do device)
#define MQTT_PASS      "1234"                // TODO

// Provisionamento (SoftAP)
#define PROV_DEVICE_NAME_PREFIX  "ICW-"            // SSID do AP de provisão: PROD-xxxxxx
#define PROV_POP                 "abcd1234"         // Proof-of-Possession (mostre no manual/etiqueta)
#define PROV_SERVICE_KEY         NULL               // senha do AP de provisão (NULL = aberto)
// ========================================================================

// ========================SIMULAÇÃO======================================
#define LED_GPIO 2
#define PIN_SWING  GPIO_NUM_25
#define PIN_dreno  GPIO_NUM_26
#define PIN_bomba   GPIO_NUM_27

// Botão de reset Wi-Fi (liga no GND; interno em pull-up)
#define BTN_WIFI_RST   GPIO_NUM_13


static bool s_ventilador = false, st_swing=false, st_dreno=false, st_bomba=false;
// ========================================================================

static const char *TAG = "APP";

// MQTT
static esp_mqtt_client_handle_t client = NULL;
static char topic_lwt[128];
static char topic_reported[160];
static char topic_desired[160];
static char topic_cmd[160];

// Wi-Fi sync
static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

typedef struct {
    char ssid[33];
    char pass[65];
} sta_cfg_t;

// ---------- Fallback tunables ----------
#define FB_STA_RETRY_MS                 10000  // 10s entre tentativas STA
#define FB_AP_ON_AFTER_OFFLINE_MS        5000  // 5s offline -> liga AP
#define FB_AP_OFF_AFTER_MQTT_OK_MS       3000  // 3s MQTT ok -> desliga AP

// Flags de conectividade e controle do AP de fallback
static volatile bool s_mqtt_connected = false;
static bool s_ap_control_on = false;
static bool s_control_uris_registered = false;

//=================================================================CABEÇALHO DE FUNÇÕES========================================
static void switch_to_sta_task(void *arg);
static void start_control_ap(void);
static void stop_control_ap(void);
static inline void set_active_low(gpio_num_t pin, bool on);
static void update_peripherals_from_state(void);
static void force_all_off(void) ;
static void led_init(void);
static void app_sntp_sync_time(void);
static char* dup_payload(const char *data, int len);
static bool app_is_provisioned(void);
static esp_err_t prov_post_handler(httpd_req_t *req);
static void start_ap_and_http(void);
static esp_err_t api_ping_handler(httpd_req_t *req);
static char* build_reported_json(void);
static esp_err_t api_state_handler(httpd_req_t *req);
static esp_err_t api_cmd_handler(httpd_req_t *req);
static void http_register_control_api(void);
static void httpd_start_if_needed(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wifi_init_driver(void);
static void wifi_start_sta(void);
static void wifi_wait_connected(void);
static char* build_reported_json(void);
static void publish_reported(void);
static void handle_cmd(const char *payload, int len);
static void handle_desired(const char *payload, int len);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void mqtt_start(void);
static void reset_wifi_and_reboot(void);
static void btn_wifi_reset_task(void *arg);
static void link_fallback_task(void *arg);
//============================================================CABEÇALHOS=====================================================

static void switch_to_sta_task(void *arg) {
    sta_cfg_t *cfg = (sta_cfg_t*)arg;

    // dá um respiro para o HTTP entregar o 200 ao cliente
    vTaskDelay(pdMS_TO_TICKS(300));

    wifi_config_t sta = {0};
    strncpy((char*)sta.sta.ssid, cfg->ssid, sizeof(sta.sta.ssid)-1);
    strncpy((char*)sta.sta.password, cfg->pass, sizeof(sta.sta.password)-1);

    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    free(cfg);
    // se preferir reiniciar depois de obter IP, remova o restart:
    ESP_LOGI(TAG, "Trocando para STA, tentando conectar com SSID='%s'", sta.sta.ssid);
    vTaskDelete(NULL);
}

// ------------------------- Utilidades -------------------------
static void start_control_ap(void) {
    wifi_config_t ap = {0};
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf((char*)ap.ap.ssid, sizeof(ap.ap.ssid),
             PROV_DEVICE_NAME_PREFIX"%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN; // sem senha, conforme solicitado

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Garante HTTP ativo e endpoints /api/*
    httpd_start_if_needed();

    s_ap_control_on = true;
    ESP_LOGW(TAG, "SoftAP de CONTROLE ligado (SSID: %s) em 192.168.4.1", ap.ap.ssid);
}

static void stop_control_ap(void) {
    // Mantém STA, desliga apenas o AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_control_on = false;
    ESP_LOGI(TAG, "SoftAP de CONTROLE desligado (mantendo STA)");
    // Mantemos o HTTP server ligado para permitir controle por LAN (STA)
}


static inline void set_active_low(gpio_num_t pin, bool on) {
    // ON  -> 0;  OFF -> 1
    gpio_set_level(pin, on ? 0 : 1);
}

// --- [ADD] Helpers de saída: aplicação de estados + dependência do LED ---
static void update_peripherals_from_state(void) {
    // LED é ativo alto
    gpio_set_level(LED_GPIO, s_ventilador ? 1 : 0);

    // Demais periféricos são ativos baixos e só podem ligar se o LED estiver ON
    bool allow = s_ventilador;
    set_active_low(PIN_SWING, allow && st_swing);
    set_active_low(PIN_dreno, allow && st_dreno);
    set_active_low(PIN_bomba,  allow && st_bomba);
}

// Desliga tudo (estado e hardware)
static void force_all_off(void) {
    s_ventilador = false;
    st_swing = false;
    st_dreno = false;
    st_bomba  = false;
    update_peripherals_from_state();
}

static void led_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_GPIO) | (1ULL<<PIN_SWING) | (1ULL<<PIN_dreno) | (1ULL<<PIN_bomba),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    force_all_off(); // garante tudo OFF e estados coerentes com a regra do LED

    gpio_config_t in = {
    .pin_bit_mask = (1ULL<<BTN_WIFI_RST),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&in);
}

/*static void get_service_name(char *name, size_t max) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, max, PROV_DEVICE_NAME_PREFIX"%02X%02X%02X", mac[3], mac[4], mac[5]);
}*/

static void app_sntp_sync_time(void) {
    ESP_LOGI(TAG, "Sincronizando SNTP...");

    // Defina fuso horário do Brasil (UTC-3, sem DST)
    setenv("TZ", "BRT3", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "a.st1.ntp.br");
    esp_sntp_setservername(1, "b.st1.ntp.br");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = {0};
    for (int i = 0; i < 30; i++) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2016 - 1900)) {
            ESP_LOGI(TAG, "Hora OK: %s", asctime(&timeinfo));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "SNTP: não conseguiu hora válida (cheque rede/DNS).");
}


// Dup segura de payload (evita depender de strndup)
static char* dup_payload(const char *data, int len) {
    char *buf = (char*)malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, data, len);
    buf[len] = '\0';
    return buf;
}

// ------------------------- Provisionamento -------------------------
static bool app_is_provisioned(void) {
    bool provisioned = false;

    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(cfg));
    wifi_prov_mgr_is_provisioned(&provisioned);
    wifi_prov_mgr_deinit();
    return provisioned;
}

// ---- INÍCIO: provisão simples por HTTP ----
static httpd_handle_t s_httpd = NULL;

static esp_err_t prov_post_handler(httpd_req_t *req) {
    char buf[256];
    int r = httpd_req_recv(req, buf, MIN(req->content_len, (sizeof(buf)-1)));
    if (r <= 0) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read"); return ESP_FAIL; }
    buf[r] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    const cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItem(root, "pass");
    if (!cJSON_IsString(j_ssid)) { cJSON_Delete(root); httpd_resp_send_err(req, 400, "ssid"); return ESP_FAIL; }

    // envia 200 OK imediatamente (antes de derrubar o AP)
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    httpd_resp_sendstr_chunk(req, NULL); // fecha chunk (se aplicável)

    // agenda a troca para STA
    sta_cfg_t *cfg = calloc(1, sizeof(sta_cfg_t));
    strncpy(cfg->ssid, j_ssid->valuestring, sizeof(cfg->ssid)-1);
    if (cJSON_IsString(j_pass)) strncpy(cfg->pass, j_pass->valuestring, sizeof(cfg->pass)-1);
    cJSON_Delete(root);

    xTaskCreate(switch_to_sta_task, "switch_to_sta", 4096, cfg, 5, NULL);
    return ESP_OK;
}


static void start_ap_and_http(void) {
    // AP aberto com SSID “ICW-xxxxxx”
    wifi_config_t ap = {0};
    char ssid[32];
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, sizeof(ssid), PROV_DEVICE_NAME_PREFIX"%02X%02X%02X", mac[3], mac[4], mac[5]);
    strncpy((char*)ap.ap.ssid, ssid, sizeof(ap.ap.ssid)-1);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP provisão '%s' ativo em 192.168.4.1", ssid);

    // servidor HTTP
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        httpd_uri_t prov_uri = {
            .uri = "/prov",
            .method = HTTP_POST,
            .handler = prov_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_httpd, &prov_uri);
        ESP_LOGI(TAG, "Endpoint POST /prov pronto");
    }
}
// ---- FIM: provisão simples por HTTP ----

// ======================== HTTP de CONTROLE (AP/STA) ========================
static esp_err_t api_ping_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t api_state_handler(httpd_req_t *req) {
    char *msg = build_reported_json();
    if (!msg) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, msg);
    free(msg);
    return ESP_OK;
}

static esp_err_t api_cmd_handler(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "len");
        return ESP_FAIL;
    }
    char *buf = (char*)malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int r = httpd_req_recv(req, buf, req->content_len);
    if (r <= 0) { free(buf); httpd_resp_send_err(req, 500, "read"); return ESP_FAIL; }
    buf[r] = 0;

    // Reaproveita a mesma rotina do MQTT
    handle_cmd(buf, r);
    free(buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static void http_register_control_api(void) {
    if (!s_httpd || s_control_uris_registered) return;

    httpd_uri_t ping_uri = { .uri="/api/ping", .method=HTTP_GET,  .handler=api_ping_handler };
    httpd_uri_t state_uri= { .uri="/api/state",.method=HTTP_GET,  .handler=api_state_handler };
    httpd_uri_t cmd_uri  = { .uri="/api/cmd",  .method=HTTP_POST, .handler=api_cmd_handler  };

    httpd_register_uri_handler(s_httpd, &ping_uri);
    httpd_register_uri_handler(s_httpd, &state_uri);
    httpd_register_uri_handler(s_httpd, &cmd_uri);
    s_control_uris_registered = true;
    ESP_LOGI(TAG, "Endpoints de controle /api/* registrados");
}

static void httpd_start_if_needed(void) {
    if (!s_httpd) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = 80;
        if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
            ESP_LOGI(TAG, "HTTP server ativo para controle local");
        }
    }
    http_register_control_api();
}
// ========================================================================


// ------------------------- Wi-Fi STA -------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode & WIFI_MODE_AP) {
        ESP_LOGW(TAG, "Ignorando tentativa de reconexão — modo AP ativo.");
        return;
    }
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "Wi-Fi desconectado — SSID fora de alcance ou senha incorreta.");
                esp_wifi_connect();
                if (s_wifi_event_group != NULL) {
                    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                } else {
                    ESP_LOGW(TAG, "WiFi event group ainda não inicializado, ignorando clear.");
                }
                break;
            default: break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_driver(void) {
    // Netifs padrão para STA e AP (AP será usado no modo de provisão)
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL, NULL));
}

static void wifi_start_sta(void) {
    // Reinicia o driver garantindo que estamos só em STA
    esp_wifi_stop();                                 // ignora erro se já estiver parado
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Wi-Fi STA iniciado (credenciais do NVS).");
}


static void wifi_wait_connected(void) {
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }
    // Espera até obter GOT_IP, com re-tentativa
    int retry = 0;
    const int max_retries = 5;

    while (retry < max_retries) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group, WIFI_CONNECTED_BIT,
            pdFALSE, pdTRUE, pdMS_TO_TICKS(10000)  // espera 10s por tentativa
        );
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi conectado com sucesso.");
            break;
        }
        retry++;
        ESP_LOGW(TAG, "Falha na conexão Wi-Fi (%d/%d).", retry, max_retries);
    }
    ESP_LOGE(TAG, "Limite de tentativas excedido — iniciando modo AP de controle.");
    if (!s_ap_control_on) start_control_ap();
}

// Constrói o JSON de estado para /api/state e para o MQTT reported
static char* build_reported_json(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    cJSON_AddStringToObject(root, "fw", CONFIG_APP_PROJECT_VER);

    cJSON *hw = cJSON_CreateObject();
    cJSON_AddBoolToObject(hw, "ventilador", s_ventilador);
    cJSON_AddBoolToObject(hw, "swing",  st_swing);
    cJSON_AddBoolToObject(hw, "dreno",  st_dreno);
    cJSON_AddBoolToObject(hw, "bomba",   st_bomba);
    cJSON_AddItemToObject(root, "hw", hw);

    char *msg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return msg; // caller libera
}


// ------------------------- MQTT -------------------------
static void publish_reported(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    cJSON_AddStringToObject(root, "fw", CONFIG_APP_PROJECT_VER);

    //cJSON *sensors = cJSON_CreateObject();
    //cJSON_AddNumberToObject(sensors, "temp", 23.4);
    //cJSON_AddNumberToObject(sensors, "flow", 12.1);
    //cJSON_AddItemToObject(root, "sensors", sensors);

    cJSON *hw = cJSON_CreateObject();
    cJSON_AddBoolToObject(hw, "ventilador", s_ventilador);
    cJSON_AddBoolToObject(hw, "swing",  st_swing);  // <- nome sem "_on"
    cJSON_AddBoolToObject(hw, "dreno",  st_dreno);
    cJSON_AddBoolToObject(hw, "bomba",   st_bomba);
    cJSON_AddItemToObject(root, "hw", hw);

    char *msg = cJSON_PrintUnformatted(root);
    if (msg) {
        int id = esp_mqtt_client_publish(client, topic_reported, msg, 0, 1, 1);
        ESP_LOGI(TAG, "PUB reported (id=%d): %s", id, msg);
        free(msg);
    }
    cJSON_Delete(root);
}


static void handle_cmd(const char *payload, int len) {
    char *buf = dup_payload(payload, len);
    if (!buf) return;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { free(buf); return; }

    const cJSON *msgId = cJSON_GetObjectItem(root, "msgId");
    const cJSON *op    = cJSON_GetObjectItem(root, "op");
    const cJSON *data  = cJSON_GetObjectItem(root, "data");

    if (cJSON_IsString(op)) {

        // OTA
        if (strcmp(op->valuestring, "ota") == 0) {
            const cJSON *url = cJSON_GetObjectItem(data, "url");
            if (cJSON_IsString(url)) do_ota(url->valuestring);
        }

        // LED de teste (ativo ALTO aqui)
        else if (strcmp(op->valuestring, "led") == 0) {
            const cJSON *on = cJSON_GetObjectItem(data, "on");
            if (cJSON_IsBool(on)) {
                s_ventilador = cJSON_IsTrue(on);

            // Se LED desligar, todos os periféricos também desligam (estado + hardware)
                if (!s_ventilador) {
                    st_swing = st_dreno = st_bomba = false;
                }
                update_peripherals_from_state();
                ESP_LOGI(TAG, "VENTILADOR %s", s_ventilador ? "ON" : "OFF");
            }
        }

        // Swing/Dreno/Bomba (ativo BAIXO)
        else if (strcmp(op->valuestring, "swing") == 0) {
            bool on = cJSON_IsTrue(cJSON_GetObjectItem(data, "on"));
            st_swing = on && s_ventilador;
            update_peripherals_from_state();
            ESP_LOGI(TAG, "SWING %s", st_swing ? "ON" : "OFF");
        }
        else if (strcmp(op->valuestring, "dreno") == 0) {
            bool on = cJSON_IsTrue(cJSON_GetObjectItem(data, "on"));
            st_dreno = on && s_ventilador;
            update_peripherals_from_state();
            ESP_LOGI(TAG, "DRENO %s", st_dreno ? "ON" : "OFF");
        }
        else if (strcmp(op->valuestring, "bomba") == 0) {
            bool on = cJSON_IsTrue(cJSON_GetObjectItem(data, "on"));
            st_bomba = on && s_ventilador;
            update_peripherals_from_state();
            ESP_LOGI(TAG, "BOMBA %s", st_bomba ? "ON" : "OFF");
        }
    }

    // ACK do comando (opcional)
    if (cJSON_IsString(msgId) && cJSON_IsString(op)) {
        char topic_ack[196];
        snprintf(topic_ack, sizeof(topic_ack),
                 "org/%s/devices/%s/ack/%s", ORG_ID, DEVICE_ID, msgId->valuestring);
        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "msgId", msgId->valuestring);
        cJSON_AddStringToObject(ack, "status", "ok");
        cJSON_AddStringToObject(ack, "op", op->valuestring);
        cJSON_AddNumberToObject(ack, "appliedAt", (double)time(NULL));
        char *ack_msg = cJSON_PrintUnformatted(ack);
        if (ack_msg) {
            esp_mqtt_client_publish(client, topic_ack, ack_msg, 0, 1, 0);
            free(ack_msg);
        }
        cJSON_Delete(ack);
    }

    // Reporta novo estado
    publish_reported();

    cJSON_Delete(root);
    free(buf);
}


static void handle_desired(const char *payload, int len) {
    char *buf = dup_payload(payload, len);
    if (!buf) return;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { free(buf); return; }

    const cJSON *outputs = cJSON_GetObjectItem(root, "outputs");
    if (cJSON_IsObject(outputs)) {
        // TODO: aplicar desired->outputs no hardware
        ESP_LOGI(TAG, "Aplicando desired->outputs...");
    }

    publish_reported();

    cJSON_Delete(root);
    free(buf);
}

// Handler de eventos do MQTT (assinatura IDF 5.x)
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado");
            if (s_ap_control_on) {
                ESP_LOGI(TAG, "Desligando modo AP em 3s (MQTT restabelecido)...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                stop_control_ap();
            }
            s_mqtt_connected = true;
            esp_mqtt_client_publish(client, topic_lwt, "online", 0, 1, 1);
            esp_mqtt_client_subscribe(client, topic_desired, 1);
            esp_mqtt_client_subscribe(client, topic_cmd, 1);
            publish_reported();
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT desconectado");
        break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Msg em [%.*s], %d bytes",
                     event->topic_len, event->topic, event->data_len);

            if ((int)strlen(topic_cmd) == event->topic_len &&
                strncmp(event->topic, topic_cmd, event->topic_len) == 0) {
                handle_cmd(event->data, event->data_len);
            } else if ((int)strlen(topic_desired) == event->topic_len &&
                       strncmp(event->topic, topic_desired, event->topic_len) == 0) {
                handle_desired(event->data, event->data_len);
            }
            break;

        default:
            break;
    }
}

static void mqtt_start(void) {
    snprintf(topic_lwt, sizeof(topic_lwt),
             "org/%s/devices/%s/lwt", ORG_ID, DEVICE_ID);
    snprintf(topic_reported, sizeof(topic_reported),
             "org/%s/devices/%s/state/reported", ORG_ID, DEVICE_ID);
    snprintf(topic_desired, sizeof(topic_desired),
             "org/%s/devices/%s/state/desired", ORG_ID, DEVICE_ID);
    snprintf(topic_cmd, sizeof(topic_cmd),
             "org/%s/devices/%s/cmd", ORG_ID, DEVICE_ID);

    char uri[256];
    snprintf(uri, sizeof(uri), "mqtts://%s:%d", BROKER_HOST, BROKER_PORT);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .broker.verification.certificate = BROKER_ROOT_CA_PEM,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .session.keepalive = 60,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.last_will.topic = topic_lwt,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .network.timeout_ms = 8000,
    };

    client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

static void reset_wifi_and_reboot(void) {
    ESP_LOGW(TAG, "Resetando credenciais Wi-Fi + flag de provisionamento...");    

    // [ADD] Desliga imediatamente todos os periféricos (estado + hardware)
        force_all_off();
        vTaskDelay(pdMS_TO_TICKS(100));

    // 1) Apaga conf. Wi-Fi (SSID/senha) do NVS
    esp_wifi_stop();
    esp_wifi_restore(); // limpa Wi-Fi no NVS (SSID/senha, etc.)

    // 2) Zera a flag do gerenciador de provisionamento (para forçar SoftAP no próximo boot)
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
    };
    if (wifi_prov_mgr_init(cfg) == ESP_OK){
        wifi_prov_mgr_reset_provisioning(); // <- API do IDF que limpa a flag "provisioned"
        wifi_prov_mgr_deinit();
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "Reiniciando...");
    esp_restart();
}

// Tarefa simples de detecção de long-press (>=5s)
static void btn_wifi_reset_task(void *arg) {
    const int hold_ms = 5000;
    int pressed_ms = 0;

    for(;;){
        int lvl = gpio_get_level(BTN_WIFI_RST); // 1 = solto (pull-up), 0 = pressionado (GND)
        if (lvl == 0) {
            pressed_ms += 20;
            if (pressed_ms >= hold_ms) {
                ESP_LOGW(TAG, "Botão Wi-Fi pressionado por %d ms — acionando reset", pressed_ms);
                reset_wifi_and_reboot();
            }
        } else {
            // solto
            if (pressed_ms > 0 && pressed_ms < hold_ms) {
                ESP_LOGI(TAG, "Press curto ignorado (%d ms). Segure %d ms para reset Wi-Fi.",
                         pressed_ms, hold_ms);
            }
            pressed_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void link_fallback_task(void *arg) {
    int64_t last_sta_try_ms = 0;
    int64_t offline_since_ms = 0;
    int64_t mqtt_ok_since_ms = 0;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode & WIFI_MODE_AP) {
        ESP_LOGW(TAG, "Ignorando tentativa de reconexão — modo AP ativo.");
        return;
}

    for (;;) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        // 1) Throttle de tentativas STA a cada 10s (caso desconecte do Wi-Fi)
        bool wifi_ok = (s_wifi_event_group &&
                        (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT));
        if (!wifi_ok) {
            if (now_ms - last_sta_try_ms >= FB_STA_RETRY_MS) {
                ESP_LOGW(TAG, "STA: tentando reconectar (throttle 10s)...");
                esp_wifi_connect();
                last_sta_try_ms = now_ms;
            }
        }

        // 2) Janela de decisão do AP de controle baseada no MQTT
        if (!s_mqtt_connected) {
            if (offline_since_ms == 0) offline_since_ms = now_ms;
            mqtt_ok_since_ms = 0;

            if (!s_ap_control_on && (now_ms - offline_since_ms) >= FB_AP_ON_AFTER_OFFLINE_MS) {
                start_control_ap(); // >=5s offline -> liga AP de controle
            }
        } else {
            if (mqtt_ok_since_ms == 0) mqtt_ok_since_ms = now_ms;
            offline_since_ms = 0;

            if (s_ap_control_on && (now_ms - mqtt_ok_since_ms) >= FB_AP_OFF_AFTER_MQTT_OK_MS) {
                stop_control_ap(); // >=3s MQTT ok -> desliga AP
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


// ------------------------- app_main -------------------------
void app_main(void) {
    // NVS + pilha de rede/eventos
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Inicializa driver Wi-Fi (STA+AP, sem conectar ainda)
    wifi_init_driver();

    // Provisionamento: se não houver SSID/senha no NVS, entra em SoftAP
    if (!app_is_provisioned()) {
        start_ap_and_http();   // <-- novo: AP + HTTP
    // fica parado aqui até o reboot (feito no handler após /prov)
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Grupo de eventos para sincronizar GOT_IP
    s_wifi_event_group = xEventGroupCreate();

    // Agora já temos credenciais salvas no NVS — iniciar STA e conectar
    wifi_start_sta();

    // Espera IP (até 15s)
    wifi_wait_connected();

    // Habilita HTTP de controle também via LAN (STA)
    httpd_start_if_needed();


    // Hora certa para TLS
    app_sntp_sync_time();

    // Função do LED para teste
    led_init();
    xTaskCreatePinnedToCore(btn_wifi_reset_task, "btn_wifi_rst", 3072, NULL, 5, NULL, tskNO_AFFINITY);


    // MQTT com TLS
    mqtt_start();

    // Guardião de conectividade: STA throttle + AP fallback
    xTaskCreatePinnedToCore(link_fallback_task, "link_fallback", 4096, NULL, 5, NULL, tskNO_AFFINITY);


    // Telemetria periódica (exemplo)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        publish_reported();
    }
}