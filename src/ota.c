// src/ota.c
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

static const char *TAG = "OTA";

// Handler de eventos do sistema de OTA (somente IDs garantidos no IDF 5.x)
static void ota_sys_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base != ESP_HTTPS_OTA_EVENT) return;
    switch (id) {
        case ESP_HTTPS_OTA_START:
            ESP_LOGI(TAG, "OTA: start");
            break;
        case ESP_HTTPS_OTA_CONNECTED:
            ESP_LOGI(TAG, "OTA: connected");
            break;
        case ESP_HTTPS_OTA_GET_IMG_DESC:
            ESP_LOGI(TAG, "OTA: reading image description");
            break;
        case ESP_HTTPS_OTA_VERIFY_CHIP_ID:
            ESP_LOGI(TAG, "OTA: verify chip id");
            break;
        case ESP_HTTPS_OTA_WRITE_FLASH:
            ESP_LOGD(TAG, "OTA: writing...");
            break;
        case ESP_HTTPS_OTA_FINISH:
            ESP_LOGI(TAG, "OTA: finish");
            break;
        case ESP_HTTPS_OTA_ABORT:
            ESP_LOGW(TAG, "OTA: abort");
            break;
        default:
            ESP_LOGI(TAG, "OTA: event id=%" PRId32, id);
            break;
    }
}

static int semver_cmp(const char* a, const char* b) {
    int a1=0,a2=0,a3=0,b1=0,b2=0,b3=0;
    sscanf(a? a:"0.0.0", "%d.%d.%d", &a1,&a2,&a3);
    sscanf(b? b:"0.0.0", "%d.%d.%d", &b1,&b2,&b3);
    if (a1!=b1) return (a1<b1)?-1:1;
    if (a2!=b2) return (a2<b2)?-1:1;
    if (a3!=b3) return (a3<b3)?-1:1;
    return 0;
}

void do_ota(const char *url) {
    if (!url || !strlen(url)) { ESP_LOGE(TAG, "URL vazia"); return; }

    // 1) Config HTTP + TLS usando o bundle de CAs (não precisa ota_certs.h)
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
        // Opcional:
        // .partial_http_download = true,
        // .max_http_request_size = 4096,
        // .ota_resumption = true,
    };

    // 2) Registrar eventos (pra log útil)
    ESP_ERROR_CHECK( esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID,
                                                ota_sys_event_handler, NULL) );

    // 3) Begin
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin: %s", esp_err_to_name(err));
        goto EXIT;
    }

    // 4) Ler descrição da imagem nova e comparar versão
    esp_app_desc_t new_desc = {0};
    err = esp_https_ota_get_img_desc(h, &new_desc);
    if (err == ESP_OK) {
        const esp_app_desc_t *running = esp_app_get_description();
        ESP_LOGI(TAG, "Atual: %s | Nova: %s", running->version, new_desc.version);
        if (semver_cmp(new_desc.version, running->version) <= 0) {
            ESP_LOGW(TAG, "Ignorando OTA (versão nova <= atual)");
            esp_https_ota_abort(h);
            goto EXIT;
        }
    } else {
        ESP_LOGW(TAG, "Não foi possível ler app desc (prosseguindo).");
    }

    // 5) Perform (loop até terminar)
    while (true) {
        err = esp_https_ota_perform(h);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            continue; // ainda baixando
        }
        break;
    }

    // 6) Finish + reboot
    if (err == ESP_OK) {
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA concluído. Reiniciando...");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            return;
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "esp_https_ota_perform: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
    }

EXIT:
    esp_event_handler_unregister(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID, ota_sys_event_handler);
}