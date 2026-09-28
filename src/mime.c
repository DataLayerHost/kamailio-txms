/* SPDX-License-Identifier: MIT */
#include "gateway.h"
#include <gmime/gmime.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static void warning(gint64 offset, GMimeParserWarning code, const gchar *item, gpointer data)
{
	(void)offset; (void)code; (void)item; *(int *)data = 1;
}
static int append(txms_buffer *out, const uint8_t *p, size_t n)
{
	if ((n >= 7 && !strncasecmp((const char *)p, "http://", 7)) ||
		(n >= 8 && !strncasecmp((const char *)p, "https://", 8))) return GW_UNSUPPORTED;
	txms_buffer tx = {0};
	if (txms_transactions(p, n, &tx)) return GW_INVALID;
	if (tx.len + !!out->len > GW_BODY_MAX - out->len) { txms_buffer_free(&tx); return GW_LIMIT; }
	uint8_t *next = realloc(out->data, out->len + tx.len + 2);
	if (!next) { txms_buffer_free(&tx); return GW_STORAGE; }
	out->data = next;
	if (out->len) out->data[out->len++] = '\n';
	memcpy(out->data + out->len, tx.data, tx.len); out->len += tx.len; out->data[out->len] = 0;
	txms_buffer_free(&tx); return 1;
}
static int sextet(uint8_t c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static int hex_digit(uint8_t c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int transfer_valid(GMimeDataWrapper *content, GMimeObject *obj)
{
	GMimeContentEncoding encoding = g_mime_data_wrapper_get_encoding(content);
	const char *header = g_mime_object_get_header(obj, "Content-Transfer-Encoding");
	if (header) {
		char *copy = g_strdup(header); g_strstrip(copy);
		int known = !g_ascii_strcasecmp(copy,"base64") || !g_ascii_strcasecmp(copy,"quoted-printable") || !g_ascii_strcasecmp(copy,"7bit") || !g_ascii_strcasecmp(copy,"8bit") || !g_ascii_strcasecmp(copy,"binary");
		g_free(copy); if (!known) return GW_UNSUPPORTED;
	}
	if (encoding != GMIME_CONTENT_ENCODING_BASE64 && encoding != GMIME_CONTENT_ENCODING_QUOTEDPRINTABLE) return 1;
	/* GMime's transfer decoders are intentionally tolerant. Validate their raw
	 * input first so invalid alphabet/padding cannot silently change a tx. */
	GMimeStream *raw = g_mime_data_wrapper_get_stream(content), *memory = g_mime_stream_mem_new();
	if (!raw || g_mime_stream_reset(raw) < 0) { g_object_unref(memory); return GW_INVALID; }
	ssize_t size = g_mime_stream_write_to_stream(raw,memory);
	GByteArray *bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(memory));
	int rc = GW_INVALID;
	if (size < 0 || bytes->len > GW_BODY_MAX) goto end;
	if (encoding == GMIME_CONTENT_ENCODING_BASE64) {
		int q[4], ended = 0; unsigned used = 0;
		for (size_t i = 0; i < bytes->len; i++) {
			uint8_t c = bytes->data[i];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
			if (ended) goto end;
			int v = sextet(c); if (v < 0 && c != '=') goto end;
			q[used++] = v;
			if (used == 4) {
				if (q[0] < 0 || q[1] < 0) goto end;
				if (q[2] < 0) { if (q[3] >= 0 || (q[1] & 15)) goto end; ended = 1; }
				else if (q[3] < 0) { if (q[2] & 3) goto end; ended = 1; }
				used = 0;
			}
		}
		if (used) goto end;
	} else {
		for (size_t i = 0; i < bytes->len; i++) if (bytes->data[i] == '=') {
			if (bytes->len - i < 3) goto end;
			if (!((bytes->data[i+1] == '\r' && bytes->data[i+2] == '\n') || (hex_digit(bytes->data[i+1]) && hex_digit(bytes->data[i+2])))) goto end;
			i += 2;
		}
	}
	rc = 1;
end:
	(void)g_mime_stream_reset(raw); g_object_unref(memory); return rc;
}
static int walk(GMimeObject *obj, unsigned depth, unsigned *parts, txms_buffer *out)
{
	if (depth > 8 || ++*parts > 128) return GW_LIMIT;
	if (GMIME_IS_MULTIPART(obj)) {
		GMimeMultipart *mp = GMIME_MULTIPART(obj);
		int count = g_mime_multipart_get_count(mp);
		if (count > 128) return GW_LIMIT;
		for (int i = 0; i < count; i++) {
			int rc = walk(g_mime_multipart_get_part(mp, i), depth + 1, parts, out);
			if (rc < 0) return rc;
		}
		return 1;
	}
	if (!GMIME_IS_PART(obj)) return GW_UNSUPPORTED;
	GMimePart *part = GMIME_PART(obj);
	GMimeContentType *ct = g_mime_object_get_content_type(obj);
	const char *filename = g_mime_part_get_filename(part);
	int attachment = filename && strlen(filename) >= 9 && !strcasecmp(filename + strlen(filename) - 9, ".txms.txt");
	if (!g_mime_content_type_is_type(ct, "text", "plain") && !attachment) return GW_UNSUPPORTED;
	const char *charset = g_mime_content_type_get_parameter(ct, "charset");
	if (charset && strcasecmp(charset, "utf-8") && strcasecmp(charset, "us-ascii") && strcasecmp(charset, "utf-16be")) return GW_UNSUPPORTED;
	GMimeDataWrapper *content = g_mime_part_get_content(part);
	if (!content) return GW_INVALID;
	int valid = transfer_valid(content, obj);
	if (valid < 0) return valid;
	GMimeStream *stream = g_mime_stream_mem_new();
	ssize_t written = g_mime_data_wrapper_write_to_stream(content, stream);
	GByteArray *bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(stream));
	int rc;
	if (written < 0 || bytes->len > GW_BODY_MAX) rc = GW_LIMIT;
	else if (charset && !strcasecmp(charset, "utf-16be")) {
		txms_buffer utf8 = {0};
		rc = txms_utf16be_to_utf8(bytes->data, bytes->len, &utf8) ? GW_INVALID : append(out, utf8.data, utf8.len);
		txms_buffer_free(&utf8);
	} else rc = append(out, bytes->data, bytes->len);
	g_object_unref(stream); return rc;
}
int gw_mime(const char *ct, const uint8_t *body, size_t n, txms_buffer *out)
{
	if (!ct || !body || !n || n > GW_BODY_MAX || strlen(ct) > 255 || strpbrk(ct, "\r\n") || out->data) return GW_INVALID;
	/* GMime applies its own parser nesting cap. Input has a hard 2 MiB cap;
	 * transfer decoding cannot expand beyond input size for accepted encodings. */
	g_mime_init();
	GMimeStream *stream = g_mime_stream_mem_new();
	g_mime_stream_write_string(stream, "Content-Type: "); g_mime_stream_write_string(stream, ct);
	g_mime_stream_write_string(stream, "\r\nMIME-Version: 1.0\r\n\r\n");
	g_mime_stream_write(stream, (const char *)body, n); g_mime_stream_reset(stream);
	GMimeParser *parser = g_mime_parser_new_with_stream(stream);
	GMimeParserOptions *options = g_mime_parser_options_new();
	int bad = 0; unsigned parts = 0;
	#ifdef GW_GMIME_WARNING_DESTROY
	g_mime_parser_options_set_warning_callback(options, warning, &bad, NULL);
#else
	g_mime_parser_options_set_warning_callback(options, warning, &bad);
#endif
	GMimeMessage *message = g_mime_parser_construct_message(parser, options);
	int rc = GW_INVALID;
	if (message && !bad) {
		GMimeObject *obj = g_mime_message_get_mime_part(message);
		if (obj) rc = walk(obj, 0, &parts, out);
	}
	if (message) g_object_unref(message);
	g_mime_parser_options_free(options); g_object_unref(parser); g_object_unref(stream);
	if (rc < 0 || !out->len) { txms_buffer_free(out); return rc < 0 ? rc : GW_INVALID; }
	return 1;
}
