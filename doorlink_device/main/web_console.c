#include "web_console.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "enrollment_mgr.h"
#include "device_key.h"
#include "cJSON.h"
#include "nvs.h"

static const char *TAG = "web_console";
static httpd_handle_t s_server = NULL;
static uint32_t s_session_token = 0;
static uint32_t s_session_expires = 0;

size_t web_console_get_esp_public_key_hex(char *out, size_t out_size)
{
    return device_key_get_public_hex(out, out_size);
}

static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex2bin(const char *hex, uint8_t *bin, size_t bin_len) {
    size_t count = 0;
    while (*hex && count < bin_len * 2) {
        if (*hex == ' ' || *hex == '\r' || *hex == '\n' || *hex == '\t' || *hex == '"' || *hex == '-') {
            hex++;
            continue;
        }
        int val = hex_char_to_int(*hex);
        if (val < 0) return -1;
        
        if (count % 2 == 0) bin[count / 2] = val << 4;
        else bin[count / 2] |= val;
        
        count++;
        hex++;
    }
    return (count == bin_len * 2) ? 0 : -1;
}

static bool is_authenticated(httpd_req_t *req)
{
    char buf[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, sizeof(buf)) == ESP_OK) {
        char *token_str = strstr(buf, "session=");
        if (token_str) {
            uint32_t token = strtoul(token_str + 8, NULL, 16);
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            if (token == s_session_token && token != 0 && now < s_session_expires) {
                s_session_expires = now + 3600; // Extend session by 1 hour on activity
                return true;
            }
        }
    }
    return false;
}

static esp_err_t send_auth_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (is_authenticated(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/status");
        return httpd_resp_send(req, NULL, 0);
    }
    
    const char *html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>"
                       "<body>"
                       "<h2>Admin Login</h2>"
                       "<form action='/login' method='POST'>"
                       "Password: <input type='password' name='password'><br><br>"
                       "<input type='submit' value='Login'>"
                       "</form></body></html>";
                       
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char *pwd = strstr(buf, "password=");
    if (pwd) {
        pwd += 9;
        char *end = strpbrk(pwd, "&\r\n "); // Strip off any trailing HTTP form data
        if (end) *end = '\0';

        if (enrollment_mgr_check_admin_pass(pwd)) {
            s_session_token = esp_random();
            s_session_expires = (uint32_t)(esp_timer_get_time() / 1000000ULL) + 3600;

            char cookie[64];
            snprintf(cookie, sizeof(cookie), "session=%08x; Path=/; HttpOnly", (unsigned int)s_session_token);
            httpd_resp_set_hdr(req, "Set-Cookie", cookie);
            
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/status");
            return httpd_resp_send(req, NULL, 0);
        }
    }
    
    const char *html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>"
                       "<body><h2>Invalid Password</h2><br><a href='/'>Back</a></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_auth_redirect(req);
    }
    
    enrolled_device_t devs[ENROLLMENT_MAX_DEVICES];
    size_t count = enrollment_mgr_get_devices(devs, ENROLLMENT_MAX_DEVICES);
    char esp_pub_hex[DEVICE_KEY_PUB_LEN * 2 + 1] = {0};
    if (device_key_get_public_hex(esp_pub_hex, sizeof(esp_pub_hex)) == 0) {
        snprintf(esp_pub_hex, sizeof(esp_pub_hex), "(unavailable)");
    }

    char *html = malloc(4096);
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int offset = snprintf(html, 4096, 
                       "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>"
                       "<body style='font-family: Arial; padding: 20px;'>"
                       "<h2>Lighthouse Web Admin Trust Portal</h2>"
                       "<p><b>ESP Public Key (hex, 65 bytes / 130 chars):</b><br><code style='word-break:break-all;'>%s</code></p>"
                       "<form action='/save' method='POST'>"
                       "<b>Device Fingerprint (User_ID Hex):</b><br>"
                       "<input type='text' name='fingerprint' maxlength='16' style='width:300px;'><br><br>"
                       "<b>Phone Public Key (hex, 65 bytes / 130 chars):</b><br>"
                       "<input type='text' name='pubkey' style='width:420px;'><br><br>"
                       "<input type='submit' value='Provision Target Device' style='padding: 10px 20px;'>"
                       "</form><hr><h3>Registered Users</h3><ul>", esp_pub_hex);
                       
    if (count == 0) {
        offset += snprintf(html + offset, 4096 - offset, "<li>No users registered.</li>");
    } else {
        for (size_t i = 0; i < count; i++) {
            char uid_hex[LIGHTHOUSE_USER_ID_LEN * 2 + 1];
            for (int j = 0; j < LIGHTHOUSE_USER_ID_LEN; j++) {
                sprintf(&uid_hex[j * 2], "%02X", devs[i].user_id[j]);
            }
            offset += snprintf(html + offset, 4096 - offset, "<li style='margin-bottom: 10px;'>User ID: <code>%s</code> "
                               "<form action='/api/revoke' method='POST' style='display:inline;'>"
                               "<input type='hidden' name='userId' value='%s'>"
                               "<input type='submit' value='Delete' style='color:red; margin-left: 10px;'></form></li>", uid_hex, uid_hex);
        }
    }
    
    snprintf(html + offset, 4096 - offset, "</ul></body></html>");
                       
    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ret;
}

static esp_err_t api_enrollments_get_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "{\"error\": \"Unauthorized\"}", HTTPD_RESP_USE_STRLEN);
    }
    
    enrolled_device_t devs[ENROLLMENT_MAX_DEVICES];
    size_t count = enrollment_mgr_get_devices(devs, ENROLLMENT_MAX_DEVICES);
    
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "enrollments", arr);
    
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        char uid_hex[LIGHTHOUSE_USER_ID_LEN * 2 + 1];
        for (int j = 0; j < LIGHTHOUSE_USER_ID_LEN; j++) {
            sprintf(&uid_hex[j * 2], "%02X", devs[i].user_id[j]);
        }
        cJSON_AddStringToObject(item, "userId", uid_hex);
        cJSON_AddItemToArray(arr, item);
    }
    
    const char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    
    cJSON_free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_auth_redirect(req);
    }
    
    char buf[512];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char *fp_ptr = strstr(buf, "fingerprint=");
    char *pubkey_ptr = strstr(buf, "pubkey=");
    
    if (fp_ptr && pubkey_ptr) {
        fp_ptr += 12;
        pubkey_ptr += 7;
        
        char *fp_end = strchr(fp_ptr, '&');
        if (fp_end) *fp_end = '\0';
        
        char *pubkey_end = strpbrk(pubkey_ptr, "&\r\n ");
        if (pubkey_end) *pubkey_end = '\0';
        
        uint8_t user_id[LIGHTHOUSE_USER_ID_LEN] = {0};
        hex2bin(fp_ptr, user_id, sizeof(user_id));
        uint8_t pubkey[ENROLLMENT_PUBKEY_LEN] = {0};
        if (hex2bin(pubkey_ptr, pubkey, sizeof(pubkey)) == 0) {
            enrollment_mgr_add_device(user_id, pubkey);
        }
        
    }
    
    const char *html = "<html><body>Parameters Provisioned Securely. <a href='/status'>Go Back</a></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_revoke_post_handler(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_auth_redirect(req);
    }
    
    char buf[128];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char *uid_ptr = strstr(buf, "userId=");
    if (uid_ptr) {
        uid_ptr += 7;
        char *uid_end = strpbrk(uid_ptr, "&\r\n ");
        if (uid_end) *uid_end = '\0';
        
        uint8_t user_id[LIGHTHOUSE_USER_ID_LEN] = {0};
        if (hex2bin(uid_ptr, user_id, sizeof(user_id)) == 0) {
            enrollment_mgr_revoke_device(user_id);
        }
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/status");
    return httpd_resp_send(req, NULL, 0);
}

static httpd_uri_t root_get = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
static httpd_uri_t login_post = { .uri = "/login", .method = HTTP_POST, .handler = login_post_handler };
static httpd_uri_t status_get = { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler };
static httpd_uri_t api_enrollments_get = { .uri = "/api/enrollments", .method = HTTP_GET, .handler = api_enrollments_get_handler };
static httpd_uri_t api_save_post = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
static httpd_uri_t api_revoke_post = { .uri = "/api/revoke", .method = HTTP_POST, .handler = api_revoke_post_handler };

esp_err_t web_console_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10; 
    
    ESP_LOGI(TAG, "Starting admin web server on port: '%d'", config.server_port);
    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_register_uri_handler(s_server, &root_get);
        httpd_register_uri_handler(s_server, &login_post);
        httpd_register_uri_handler(s_server, &status_get);
        httpd_register_uri_handler(s_server, &api_enrollments_get);
        httpd_register_uri_handler(s_server, &api_save_post);
        httpd_register_uri_handler(s_server, &api_revoke_post);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Error starting server!");
    return ESP_FAIL;
}
