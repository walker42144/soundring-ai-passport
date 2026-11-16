#include "sr_pair.h"

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "sr_net.h"
#include "sr_store.h"
#include "sr_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "sr_pair";
#define SR_PAIR_TTL_SEC 600
#define SR_PAIR_MAX_FAILS 5
// 授权帧是单行 JSON，ct 为十六进制：行长 ≈ 118 + 2×明文长度。
// 明文含 client_id(32) + scope(165) + access(≈300) + refresh(≈200) ≈ 800 B → 行长约 1700。
// 1024 会让首次配置必然溢出，这里留到 4096，并给明文一个显式上限。
#define SR_PAIR_PLAIN_MAX 1536
#define SR_PAIR_LINE_MAX 4096

_Static_assert(2 * SR_PAIR_PLAIN_MAX + 128 <= SR_PAIR_LINE_MAX,
               "十六进制密文 + 帧头必须放得进 SR_PAIR_LINE_MAX");

typedef struct {
    bool active;
    char code[9];
    uint8_t nonce[16];
    char nonce_hex[33];
    uint8_t key[32];
    int64_t deadline_us;
    int attempts;
    int expected_seq;
    bool hello_done;
} pair_session_t;

static sr_pair_callbacks_t s_callback_storage;
static const sr_pair_callbacks_t *s_callbacks;
static pair_session_t s_session;
static TaskHandle_t s_task;
static char s_status[64] = "等待电脑";
static int64_t s_last_broadcast_us;

// 帧缓冲改为 static：串口写入全部发生在配对任务里，放 .bss 可以省下 4 KB 栈。
static bool write_line(const char *prefix, const char *json) {
    static char line[SR_PAIR_LINE_MAX];
    const int length = snprintf(line, sizeof(line), "%s %s\r\n", prefix, json ? json : "");
    if (length <= 0 || (size_t)length >= sizeof(line)) return false;
    return usb_serial_jtag_write_bytes(line, (size_t)length, pdMS_TO_TICKS(300)) == length;
}

// 只广播 nonce；配对码仅在设备屏幕上显示，由使用者输入电脑。
// 每 10 秒重播一次，避免电脑后开串口时错过起始帧。
static void broadcast_frame(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "nonce", s_session.nonce_hex);
    char *json = cJSON_PrintUnformatted(root);
    write_line("SRPAIR1", json);
    free(json);
    cJSON_Delete(root);
    s_last_broadcast_us = esp_timer_get_time();
}

static void publish_state(const char *text) {
    sr_utf8_ellipsis(text, s_status, sizeof(s_status));
    if (s_callbacks && s_callbacks->on_state) s_callbacks->on_state(s_status, s_callbacks->ctx);
}

static void derive_key(void) {
    (void)mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)s_session.code,
                                        strlen(s_session.code), s_session.nonce,
                                        sizeof(s_session.nonce), 20000, sizeof(s_session.key),
                                        s_session.key);
}

static void end_session_locked(void) {
    s_session.active = false;
    s_session.hello_done = false;
    memset(s_session.key, 0, sizeof(s_session.key));
    publish_state("配对已结束");
}

void sr_pair_begin(void) {
    // 会话仍有效时复用同一配对码/nonce：用户可能来回切页（设置 → 电脑配对），
    // 每次重新生成会让屏幕上已显示的码立刻作废，电脑端就永远配不上。
    if (s_session.active && s_session.deadline_us > esp_timer_get_time()) {
        broadcast_frame();
        publish_state("请输入配对码");
        return;
    }
    memset(&s_session, 0, sizeof(s_session));
    s_session.expected_seq = 0;
    uint32_t code = esp_random() % 90000000U + 10000000U;  // 8 位，首位非 0
    snprintf(s_session.code, sizeof(s_session.code), "%08u", (unsigned)code);
    esp_fill_random(s_session.nonce, sizeof(s_session.nonce));
    for (size_t i = 0; i < sizeof(s_session.nonce); ++i) {
        snprintf(&s_session.nonce_hex[i * 2], 3, "%02x", s_session.nonce[i]);
    }
    s_session.nonce_hex[32] = '\0';
    derive_key();
    s_session.deadline_us = esp_timer_get_time() + (int64_t)SR_PAIR_TTL_SEC * 1000000;
    // active 最后置位：配对任务可能并发检查，先有 deadline 再算有效。
    s_session.active = true;

    // 只广播 nonce；配对码仅在设备屏幕上显示，由使用者输入电脑。
    broadcast_frame();
    publish_state("请输入配对码");
    ESP_LOGI(TAG, "配对已开始（%d 秒有效）", SR_PAIR_TTL_SEC);
}

void sr_pair_end(void) {
    if (!s_session.active) return;
    end_session_locked();
}

bool sr_pair_active(void) { return s_session.active; }
// 会话过期后不再显示旧码，避免用户照着已失效的码输入。
const char *sr_pair_code(void) { return s_session.active ? s_session.code : ""; }

int sr_pair_seconds_left(void) {
    if (!s_session.active) return 0;
    const int64_t left = s_session.deadline_us - esp_timer_get_time();
    return left > 0 ? (int)(left / 1000000) : 0;
}

static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t length) {
    uint8_t diff = 0;
    for (size_t i = 0; i < length; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static bool hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len) {
    const size_t length = strlen(hex);
    if (length % 2 != 0 || length / 2 > cap) return false;
    for (size_t i = 0; i < length / 2; ++i) {
        unsigned int value = 0;
        if (sscanf(hex + i * 2, "%2x", &value) != 1) return false;
        out[i] = (uint8_t)value;
    }
    *out_len = length / 2;
    return true;
}

static bool decrypt_field(const cJSON *root, const char *aad, char *out, size_t cap) {
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(root, "iv");
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(root, "ct");
    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag");
    if (!cJSON_IsString(iv) || !cJSON_IsString(ct) || !cJSON_IsString(tag)) return false;

    uint8_t iv_raw[16], ct_raw[SR_PAIR_PLAIN_MAX], tag_raw[16];
    size_t iv_len = 0, ct_len = 0, tag_len = 0;
    if (!hex_decode(iv->valuestring, iv_raw, sizeof(iv_raw), &iv_len) || iv_len != 12) return false;
    if (!hex_decode(ct->valuestring, ct_raw, sizeof(ct_raw), &ct_len) || ct_len == 0) return false;
    if (!hex_decode(tag->valuestring, tag_raw, sizeof(tag_raw), &tag_len) || tag_len != 16) return false;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = false;
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_session.key, 256) == 0) {
        uint8_t plain[SR_PAIR_PLAIN_MAX];
        if (mbedtls_gcm_auth_decrypt(&gcm, ct_len, iv_raw, iv_len, (const unsigned char *)aad,
                                     strlen(aad), tag_raw, tag_len, ct_raw, plain) == 0) {
            const size_t copy = ct_len < cap - 1 ? ct_len : cap - 1;
            memcpy(out, plain, copy);
            out[copy] = '\0';
            ok = true;
        }
        memset(plain, 0, sizeof(plain));
    }
    mbedtls_gcm_free(&gcm);
    return ok;
}

static void reply_error(const char *cmd, const char *reason) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", cmd ? cmd : "");
    cJSON_AddStringToObject(root, "error", reason);
    char *json = cJSON_PrintUnformatted(root);
    write_line("SRERR", json);
    free(json);
    cJSON_Delete(root);
}

static void reply_ok(const char *cmd, const char *note) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", cmd ? cmd : "");
    cJSON_AddBoolToObject(root, "wifi", s_callbacks && s_callbacks->on_wifi != NULL);
    cJSON_AddStringToObject(root, "note", note ? note : "");
    char *json = cJSON_PrintUnformatted(root);
    write_line("SROK", json);
    free(json);
    cJSON_Delete(root);
}

static void handle_hello(const cJSON *root) {
    const cJSON *mac = cJSON_GetObjectItemCaseSensitive(root, "mac");
    if (!cJSON_IsString(mac) || !mac->valuestring) {
        reply_error("hello", "bad_request");
        return;
    }
    uint8_t provided[64];
    size_t provided_len = 0;
    if (!hex_decode(mac->valuestring, provided, sizeof(provided), &provided_len) || provided_len != 32) {
        reply_error("hello", "bad_request");
        return;
    }
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t expected[32];
    if (!md || mbedtls_md_hmac(md, s_session.key, sizeof(s_session.key),
                               (const unsigned char *)"hello", 5, expected) != 0) {
        reply_error("hello", "internal");
        return;
    }
    if (!constant_time_equal(provided, expected, 32)) {
        ++s_session.attempts;
        ESP_LOGW(TAG, "配对码校验失败 %d/%d", s_session.attempts, SR_PAIR_MAX_FAILS);
        reply_error("hello", "bad_code");
        if (s_session.attempts >= SR_PAIR_MAX_FAILS) {
            end_session_locked();
        } else {
            publish_state("配对码错误，请重试");
        }
        return;
    }
    s_session.hello_done = true;
    s_session.attempts = 0;
    publish_state("已连接，等待写入");
    reply_ok("hello", "ready");
}

static void handle_secure(sr_config_t *dummy, const cJSON *root, const char *cmd) {
    (void)dummy;
    if (!s_session.hello_done) {
        reply_error(cmd, "need_hello");
        return;
    }
    const cJSON *seq_item = cJSON_GetObjectItemCaseSensitive(root, "n");
    const int seq = cJSON_IsNumber(seq_item) ? seq_item->valueint : -1;
    if (seq != s_session.expected_seq + 1) {
        reply_error(cmd, "bad_sequence");
        return;
    }

    char aad[32];
    snprintf(aad, sizeof(aad), "sr1|%s|%d", cmd, seq);
    cJSON *payload_root = NULL;
    bool ok = false;
    char plain[SR_PAIR_PLAIN_MAX + 1];

    if (strcmp(cmd, "wifi") == 0) {
        if (!decrypt_field(root, aad, plain, sizeof(plain))) {
            reply_error(cmd, "bad_cipher");
            return;
        }
        payload_root = cJSON_Parse(plain);
        const cJSON *ssid = payload_root ? cJSON_GetObjectItemCaseSensitive(payload_root, "ssid") : NULL;
        const cJSON *pass = payload_root ? cJSON_GetObjectItemCaseSensitive(payload_root, "pass") : NULL;
        if (cJSON_IsString(ssid) && ssid->valuestring && ssid->valuestring[0] &&
            strlen(ssid->valuestring) < SR_SSID_CAP &&
            (!cJSON_IsString(pass) || (pass->valuestring && strlen(pass->valuestring) < SR_PASS_CAP))) {
            char pass_text[SR_PASS_CAP] = "";
            if (cJSON_IsString(pass) && pass->valuestring) {
                sr_utf8_ellipsis(pass->valuestring, pass_text, sizeof(pass_text));
            }
            ok = s_callbacks && s_callbacks->on_wifi &&
                 s_callbacks->on_wifi(ssid->valuestring, pass_text, s_callbacks->ctx);
        }
        cJSON_Delete(payload_root);
        if (!ok) {
            reply_error(cmd, "wifi_rejected");
            return;
        }
    } else if (strcmp(cmd, "auth") == 0) {
        if (!decrypt_field(root, aad, plain, sizeof(plain))) {
            reply_error(cmd, "bad_cipher");
            return;
        }
        payload_root = cJSON_Parse(plain);
        const cJSON *client = payload_root ? cJSON_GetObjectItemCaseSensitive(payload_root, "client_id") : NULL;
        const cJSON *access = payload_root ? cJSON_GetObjectItemCaseSensitive(payload_root, "access_token") : NULL;
        const cJSON *refresh = payload_root ? cJSON_GetObjectItemCaseSensitive(payload_root, "refresh_token") : NULL;
        const cJSON *scope = payload_root ? cJSON_GetObjectItemCaseSensitive(payload_root, "scope") : NULL;
        const cJSON *expires = payload_root ? cJSON_GetObjectItemCaseSensitive(payload_root, "expires_at_ms") : NULL;
        ok = cJSON_IsString(client) && client->valuestring && strlen(client->valuestring) < SR_CLIENT_ID_CAP &&
             cJSON_IsString(access) && access->valuestring && strlen(access->valuestring) < SR_TOKEN_CAP &&
             cJSON_IsString(refresh) && refresh->valuestring && strlen(refresh->valuestring) < SR_TOKEN_CAP &&
             (!cJSON_IsString(scope) || (scope->valuestring && strlen(scope->valuestring) < SR_SCOPE_CAP)) &&
             s_callbacks && s_callbacks->on_auth &&
             s_callbacks->on_auth(client->valuestring, access->valuestring, refresh->valuestring,
                                  cJSON_IsString(scope) ? scope->valuestring : "",
                                  cJSON_IsNumber(expires) ? (int64_t)expires->valuedouble : 0,
                                  s_callbacks->ctx);
        cJSON_Delete(payload_root);
        if (!ok) {
            reply_error(cmd, "auth_rejected");
            return;
        }
        publish_state("授权已保存");
    } else {
        reply_error(cmd, "unknown");
        return;
    }

    s_session.expected_seq = seq;
    reply_ok(cmd, "saved");
}

static void handle_line(char *line) {
    if (!s_session.active) return;
    if (s_session.deadline_us > 0 && esp_timer_get_time() > s_session.deadline_us) {
        end_session_locked();
        return;
    }
    cJSON *root = cJSON_Parse(line);
    if (!root) return;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsNumber(version) || version->valueint != 1 || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        return;
    }
    const char *cmd = cmd_item->valuestring;
    if (strcmp(cmd, "hello") == 0) {
        handle_hello(root);
    } else if (strcmp(cmd, "wifi") == 0 || strcmp(cmd, "auth") == 0) {
        handle_secure(NULL, root, cmd);
    } else if (strcmp(cmd, "status") == 0) {
        // 配置工具用它确认 Wi-Fi 是否真的连上，避免“写入成功但没联网”被当成成功。
        sr_net_status_t st;
        sr_net_get_status(&st);
        const bool auth = s_callbacks && s_callbacks->query_auth && s_callbacks->query_auth(s_callbacks->ctx);
        char note[112];
        snprintf(note, sizeof(note), "connected=%d,time=%d,auth=%d,ip=%s", st.connected ? 1 : 0,
                 st.time_valid ? 1 : 0, auth ? 1 : 0, st.ip);
        reply_ok(cmd, note);
    } else if (strcmp(cmd, "bye") == 0) {
        reply_ok("bye", "closed");
        end_session_locked();
    } else {
        reply_error(cmd, "unknown");
    }
    cJSON_Delete(root);
}

static void pair_task(void *arg) {
    (void)arg;
    static char line[SR_PAIR_LINE_MAX];
    size_t used = 0;
    bool overflow = false;
    uint8_t chunk[128];
    ESP_LOGI(TAG, "串口读取任务已启动");
    for (;;) {
        const int read = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        for (int i = 0; i < read; ++i) {
            const char ch = (char)chunk[i];
            if (ch == '\n') {
                if (overflow) {
                    // 不能静默丢弃：电脑端只会干等到 60 秒超时，用户看不到任何原因。
                    if (s_session.active) reply_error("", "line_too_long");
                    ESP_LOGW(TAG, "丢弃超长帧（上限 %u 字节）", (unsigned)(SR_PAIR_LINE_MAX - 1));
                } else if (used > 0) {
                    line[used] = '\0';
                    handle_line(line);
                }
                used = 0;
                overflow = false;
            } else if (ch != '\r') {
                if (used + 1 < sizeof(line)) {
                    line[used++] = ch;
                } else {
                    overflow = true;
                }
            }
        }
        if (s_session.active && s_session.deadline_us > 0 &&
            esp_timer_get_time() > s_session.deadline_us) {
            end_session_locked();
        }
        if (s_session.active && !s_session.hello_done &&
            esp_timer_get_time() - s_last_broadcast_us > 10 * 1000000LL) {
            broadcast_frame();
        }
    }
}

// 设备端加密自检：PBKDF2 标准向量 + AES-256-GCM 往返，失败只记录不影响遥控。
static bool crypto_selftest(void) {
    static const uint8_t expected[32] = {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c, 0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48, 0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b,
    };
    uint8_t derived[32];
    if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)"password", 8,
                                      (const unsigned char *)"salt", 4, 1, sizeof(derived),
                                      derived) != 0) {
        return false;
    }
    if (memcmp(derived, expected, sizeof(expected)) != 0) return false;

    uint8_t key[32];
    uint8_t iv[12];
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)i;
    for (int i = 0; i < 12; ++i) iv[i] = (uint8_t)i;
    const char *plain = "soundring";
    const char *aad = "sr1|test";
    uint8_t cipher[32];
    uint8_t tag[16];
    uint8_t roundtrip[32];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, strlen(plain), iv, sizeof(iv),
                                        (const unsigned char *)aad, strlen(aad),
                                        (const unsigned char *)plain, cipher, sizeof(tag), tag) == 0 &&
              mbedtls_gcm_auth_decrypt(&gcm, strlen(plain), iv, sizeof(iv),
                                       (const unsigned char *)aad, strlen(aad), tag, sizeof(tag),
                                       cipher, roundtrip) == 0 &&
              memcmp(roundtrip, plain, strlen(plain)) == 0;
    mbedtls_gcm_free(&gcm);
    return ok;
}

esp_err_t sr_pair_start(const sr_pair_callbacks_t *callbacks) {
    // 必须复制：调用方（sr_app_start）传的是栈上临时结构，app_main 返回后该栈帧失效，
    // 之后再解引用 s_callbacks->on_state 就是野函数指针（实测会 panic 后重启）。
    if (callbacks) {
        s_callback_storage = *callbacks;
        s_callbacks = &s_callback_storage;
    } else {
        s_callbacks = NULL;
    }
    if (!s_task) {
        ESP_LOGI(TAG, "加密自检: %s", crypto_selftest() ? "PASS" : "FAIL");
        // 驱动接口的形参不是 const，这里不能加 const，否则 -Wdiscarded-qualifiers。
        usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 2048, .rx_buffer_size = 4096};
        const esp_err_t err = usb_serial_jtag_driver_install(&config);
        ESP_LOGI(TAG, "驱动安装: %s", esp_err_to_name(err));
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "USB 串口不可用: %s", esp_err_to_name(err));
            return err;
        }
        usb_serial_jtag_vfs_use_driver();
        ESP_LOGI(TAG, "VFS 已切换到驱动模式");
        // 10240：handle_secure 的 plain(1536) + decrypt_field 的 ct_raw/plain(3072) + mbedTLS GCM 上下文，
        // 最深处约 6.5 KB，留足余量；帧缓冲已改为 static，不再额外占栈。
        if (xTaskCreate(pair_task, "sr_pair", 10240, NULL, 4, &s_task) != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}
