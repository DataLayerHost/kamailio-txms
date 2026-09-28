/* SPDX-License-Identifier: MIT */
#include "gateway.h"
#include <string.h>
#include <strings.h>
static int media(const char *ct, const char *type)
{
	size_t n = strlen(type); return !strncasecmp(ct, type, n) && (!ct[n] || ct[n] == ';');
}
void gw_result_free(gw_result *r) { txms_buffer_free(&r->transactions); memset(r, 0, sizeof(*r)); }
int gw_process(gw_store *s, const char *from, const char *to, const char *ct,
	const uint8_t *body, size_t n, int framing, time_t now, gw_result *out)
{
	if (!s || !from || !to || !ct || !body || !n || n > s->limits.max_bytes || strlen(from) >= GW_ID_MAX || strlen(to) >= GW_ID_MAX || strlen(ct) >= sizeof(out->content_type)) return GW_LIMIT;
	memset(out, 0, sizeof(*out));
	strcpy(out->from, from); strcpy(out->to, to); strcpy(out->content_type, ct); strcpy(out->transport, "sip-message");
	out->original_length = n; out->parts = 1;
	if (media(ct, "application/vnd.3gpp.sms") || media(ct, "application/octet-stream")) {
		gw_sms sms = {0}; txms_buffer assembled = {0}, text = {0};
		int rc = gw_sms_parse(body, n, framing, &sms);
		if (rc < 0) return rc;
		strcpy(out->from, sms.sender); out->multipart = !!sms.concat.total;
		out->multipart_reference = sms.concat.reference; out->parts = sms.concat.total ? sms.concat.total : 1; out->original_encoding = sms.encoding;
		rc = gw_assemble(s, &sms, to, now, &assembled);
		if (rc == 1) {
			rc = gw_sms_text(sms.encoding, assembled.data, assembled.len, &text);
			if (rc > 0) rc = txms_transactions(text.data, text.len, &out->transactions) ? GW_INVALID : 1;
		}
		txms_buffer_free(&assembled); txms_buffer_free(&text); gw_sms_free(&sms); return rc;
	}
	if (media(ct, "text/plain") || !strncasecmp(ct, "multipart/", 10)) return gw_mime(ct, body, n, &out->transactions);
	/* WAP notifications and provider URLs require a future authenticated adapter. */
	return GW_UNSUPPORTED;
}
