#include "cloud_link.h"
#include "alerts.h"
#include "device_status.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "settings.h"
#include "vision_service.h"
#include "wifi_sync.h"
#include <string>
#include <time.h>
#include <vector>
#include <atomic>
#include "esp_log.h"
namespace cloud_link {
namespace {
std::atomic<int> lastStatus{0},lastError{0};
std::atomic<uint32_t> lastSuccess{0};
struct Reply {
    std::string body;
};
esp_err_t event(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        auto *r = (Reply *)e->user_data;
        if (r->body.size() + e->data_len > 32000)
            return ESP_FAIL;
        r->body.append((const char *)e->data, e->data_len);
    }
    return ESP_OK;
}
void task(void *) {
    std::vector<std::string> done;
    uint32_t photoAt = 0;
    std::string lastId, lastResult;
    for (;;) {
        auto c = settings::cloud_options();
        if (c.enabled && c.url[0] && c.token[0] && wifi_sync::connected() && time(nullptr) > 1700000000) {
            cJSON *j = cJSON_CreateObject();
            auto *status = device_status_json();
            cJSON_AddStringToObject(status, "lastCommandId", lastId.c_str());
            cJSON_AddStringToObject(status, "lastCommandResult", lastResult.c_str());
            cJSON_AddItemToObject(j, "status", status);
            cJSON_AddItemToObject(j, "settings", cJSON_Parse(settings::public_json(true).c_str()));
            auto *a = cJSON_AddArrayToObject(j, "acks");
            for (auto &id : done)
                cJSON_AddItemToArray(a, cJSON_CreateString(id.c_str()));
            auto ev = vision_service::evidence();
            std::vector<uint8_t> jpg;
            uint32_t captured = 0;
            if (ev.at && ev.at != photoAt && vision_service::photo(jpg,120000)) {
                size_t n = 0;
                mbedtls_base64_encode(nullptr, 0, &n, jpg.data(), jpg.size());
                std::string encoded(n, '\0');
                if (mbedtls_base64_encode((unsigned char *)encoded.data(), encoded.size(), &n, jpg.data(),
                                          jpg.size()) == 0) {
                    encoded.resize(n);
                    cJSON_AddStringToObject(j, "photo", encoded.c_str());
                    captured = ev.at;
                }
            }
            char *body = cJSON_PrintUnformatted(j);
            cJSON_Delete(j);
            if (body) {
                std::string url = c.url;
                while (!url.empty() && url.back() == '/')
                    url.pop_back();
                url += "/device/sync";
                Reply reply;
                esp_http_client_config_t cfg = {};
                cfg.url = url.c_str();
                cfg.crt_bundle_attach = esp_crt_bundle_attach;
                cfg.timeout_ms = 8000;
                cfg.event_handler = event;
                cfg.user_data = &reply;
                cfg.disable_auto_redirect = true;
                auto client = esp_http_client_init(&cfg);
                if (client) {
                    std::string auth = "Bearer " + std::string(c.token);
                    esp_http_client_set_method(client, HTTP_METHOD_POST);
                    esp_http_client_set_header(client, "Authorization", auth.c_str());
                    esp_http_client_set_header(client, "Content-Type", "application/json");
                    esp_http_client_set_post_field(client, body, strlen(body));
                    auto err = esp_http_client_perform(client);
                    int statusCode = esp_http_client_get_status_code(client);
                    lastStatus=statusCode;lastError=err;
                    esp_http_client_cleanup(client);
                    if (err == ESP_OK && statusCode == 200) {
                        lastSuccess=millis();
                        if (captured)
                            photoAt = captured;
                        cJSON *r = cJSON_Parse(reply.body.c_str());
                        cJSON *cmd;
                        cJSON_ArrayForEach(cmd, cJSON_GetObjectItemCaseSensitive(r, "commands")) {
                            auto *id = cJSON_GetObjectItemCaseSensitive(cmd, "id"),
                                 *kind = cJSON_GetObjectItemCaseSensitive(cmd, "kind");
                            auto *data = cJSON_GetObjectItemCaseSensitive(cmd, "data");
                            if (!cJSON_IsString(id) || !cJSON_IsString(kind))
                                continue;
                            bool seen = false;
                            for (auto &v : done)
                                if (v == id->valuestring)
                                    seen = true;
                            if (seen)
                                continue;
                            bool ok = false;
                            std::string message;
                            if (!strcmp(kind->valuestring, "capture"))
                                ok = alerts::snapshot();
                            else if (!strcmp(kind->valuestring, "readings"))
                                ok = alerts::readings();
                            else if (!strcmp(kind->valuestring, "test"))
                                ok = alerts::test();
                            else if (!strcmp(kind->valuestring, "emergency_test"))
                                ok = alerts::test_emergency();
                            else if (!strcmp(kind->valuestring, "ack")) {
                                auto *v = cJSON_GetObjectItemCaseSensitive(data, "event");
                                if (cJSON_IsNumber(v) && v->valuedouble >= 1 && v->valuedouble <= UINT32_MAX)
                                    ok = alerts::acknowledge(uint32_t(v->valuedouble));
                            } else if (!strcmp(kind->valuestring, "settings")) {
                                char *json = cJSON_PrintUnformatted(data);
                                if (json) {
                                    ok = settings::parse_save(json, message);
                                    cJSON_free(json);
                                }
                            }
                            lastId = id->valuestring;
                            lastResult = ok                ? "Applied"
                                         : message.empty() ? "Rejected, stale event or rate limit"
                                                           : message;
                            done.push_back(lastId);
                            if (done.size() > 32)
                                done.erase(done.begin());
                        }
                        cJSON_Delete(r);
                    }
                }
                cJSON_free(body);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
} // namespace
Health health() { return {lastStatus.load(),lastError.load(),lastSuccess.load()}; }
void start() { configASSERT(xTaskCreate(task, "cloud", 12288, nullptr, 2, nullptr) == pdPASS); }
} // namespace cloud_link
