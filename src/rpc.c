/* SPDX-License-Identifier: MIT */
#include "gateway.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define RESPONSE_MAX 65536u
static size_t receive(char *p, size_t size, size_t count, void *context)
{
	txms_buffer *b = context;
	if (size && count > SIZE_MAX / size) return 0;
	size_t n = size * count;
	if (n > RESPONSE_MAX - b->len) return 0;
	uint8_t *next = realloc(b->data, b->len + n + 1);
	if (!next) return 0;
	b->data = next; memcpy(b->data + b->len, p, n); b->len += n; b->data[b->len] = 0;
	return n;
}
int gw_rpc_validate(const gw_rpc_config *c)
{
	if (!c || !c->url || !*c->url || !c->method || !*c->method || strlen(c->method) > 128 || !strcmp(c->method, "<configure-me>") || c->timeout_ms < 1 || c->timeout_ms > 300000 || (c->tls_verify != 0 && c->tls_verify != 1)) return GW_INVALID;
	if (c->token && *c->token && ((c->username && *c->username) || strpbrk(c->token, "\r\n"))) return GW_INVALID;
	CURLU *u = curl_url(); char *scheme = NULL, *user = NULL, *fragment = NULL;
	if (!u) return GW_STORAGE;
	int rc = GW_INVALID;
	if (!curl_url_set(u, CURLUPART_URL, c->url, 0) && !curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) && (!strcmp(scheme, "http") || !strcmp(scheme, "https")) && curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_NO_USER && curl_url_get(u, CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_NO_FRAGMENT) rc = 1;
	curl_free(scheme); curl_free(user); curl_free(fragment); curl_url_cleanup(u); return rc;
}
int gw_rpc_submit(const gw_rpc_config *c, gw_job_id id, const txms_buffer *hex)
{
	if (gw_rpc_validate(c) < 0 || !hex || !txms_is_hex(hex->data, hex->len)) return GW_RPC_UNCERTAIN;
	CURL *curl = curl_easy_init();
	if (!curl) return GW_RPC_UNCERTAIN;
	int result = GW_RPC_UNCERTAIN;
	char request_id[32]; snprintf(request_id, sizeof(request_id), "%llu", (unsigned long long)id);
	struct json_object *req = json_object_new_object(), *params = json_object_new_array();
	if (!req || !params) { if (req) json_object_put(req); if (params) json_object_put(params); curl_easy_cleanup(curl); return result; }
	json_object_object_add(req, "jsonrpc", json_object_new_string("2.0"));
	json_object_object_add(req, "method", json_object_new_string(c->method));
	json_object_object_add(req, "id", json_object_new_string(request_id));
	json_object_array_add(params, json_object_new_string_len((const char *)hex->data, (int)hex->len));
	json_object_object_add(req, "params", params);
	txms_buffer response = {0};
	struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
	if (!headers) goto end;
#define SET(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) goto end; } while (0)
	SET(CURLOPT_URL, c->url); SET(CURLOPT_PROTOCOLS_STR, "http,https");
	SET(CURLOPT_FOLLOWLOCATION, 0L); SET(CURLOPT_NOSIGNAL, 1L);
	SET(CURLOPT_CONNECTTIMEOUT_MS, c->timeout_ms < 3000 ? c->timeout_ms : 3000L);
	SET(CURLOPT_TIMEOUT_MS, c->timeout_ms);
	SET(CURLOPT_SSL_VERIFYPEER, c->tls_verify ? 1L : 0L); SET(CURLOPT_SSL_VERIFYHOST, c->tls_verify ? 2L : 0L);
	SET(CURLOPT_HTTPHEADER, headers); SET(CURLOPT_POSTFIELDS, json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN));
	SET(CURLOPT_WRITEFUNCTION, receive); SET(CURLOPT_WRITEDATA, &response);
	if (c->token && *c->token) { SET(CURLOPT_HTTPAUTH, (long)CURLAUTH_BEARER); SET(CURLOPT_XOAUTH2_BEARER, c->token); }
	else if (c->username && *c->username) { SET(CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC); SET(CURLOPT_USERNAME, c->username); SET(CURLOPT_PASSWORD, c->password ? c->password : ""); }
	if (curl_easy_perform(curl) != CURLE_OK) goto end;
	long http = 0;
	if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http) != CURLE_OK || http < 200 || http >= 300 || !response.len) goto end;
	struct json_tokener *tok = json_tokener_new_ex(16);
	if (!tok) goto end;
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	struct json_object *root = json_tokener_parse_ex(tok, (char *)response.data, (int)response.len);
	size_t consumed = json_tokener_get_parse_end(tok);
	while (consumed < response.len && isspace(response.data[consumed])) consumed++;
	if (root && json_tokener_get_error(tok) == json_tokener_success && consumed == response.len && json_object_is_type(root, json_type_object)) {
		struct json_object *version = NULL, *rid = NULL, *value = NULL, *error = NULL;
		int has_result = json_object_object_get_ex(root, "result", &value), has_error = json_object_object_get_ex(root, "error", &error);
		if (json_object_object_get_ex(root, "jsonrpc", &version) && json_object_is_type(version, json_type_string) && json_object_get_string_len(version) == 3 && !strcmp(json_object_get_string(version), "2.0") && json_object_object_get_ex(root, "id", &rid) && json_object_is_type(rid, json_type_string) && (size_t)json_object_get_string_len(rid) == strlen(request_id) && !strcmp(json_object_get_string(rid), request_id) && has_result != has_error) {
			if (has_result && value && json_object_is_type(value, json_type_string) && json_object_get_string_len(value) > 0) result = GW_RPC_SUCCESS;
			else if (has_error && json_object_is_type(error, json_type_object)) {
				struct json_object *code = NULL, *message = NULL;
				if (json_object_object_get_ex(error, "code", &code) && json_object_is_type(code, json_type_int) && json_object_object_get_ex(error, "message", &message) && json_object_is_type(message, json_type_string)) result = GW_RPC_REJECTED;
			}
		}
	}
	if (root) json_object_put(root);
	json_tokener_free(tok);
end:
	curl_slist_free_all(headers); txms_buffer_free(&response); json_object_put(req); curl_easy_cleanup(curl); return result;
#undef SET
}
