/* SPDX-License-Identifier: MIT */
#include "gateway.h"
#include <stdlib.h>
#include <string.h>
/* 3GPP TS 23.038 default alphabet. Escape 0x1b is handled separately. */
static const uint16_t gsm[128] = {
	0x40,0xa3,0x24,0xa5,0xe8,0xe9,0xf9,0xec,0xf2,0xc7,0xa,0xd8,0xf8,0xd,0xc5,0xe5,
	0x394,0x5f,0x3a6,0x393,0x39b,0x3a9,0x3a0,0x3a8,0x3a3,0x398,0x39e,0,0xc6,0xe6,0xdf,0xc9,
	0x20,0x21,0x22,0x23,0xa4,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
	0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
	0xa1,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
	0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0xc4,0xd6,0xd1,0xdc,0xa7,
	0xbf,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
	0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0xe4,0xf6,0xf1,0xfc,0xe0
};
static int copy(txms_buffer *out, const uint8_t *p, size_t n)
{
	out->data = malloc(n + 1);
	if (!out->data) return GW_STORAGE;
	memcpy(out->data, p, n); out->data[n] = 0; out->len = n; return 1;
}
static uint16_t extension(uint8_t c)
{
	switch (c) {
	case 10: return 12; case 20: return '^'; case 40: return '{'; case 41: return '}';
	case 47: return '\\'; case 60: return '['; case 61: return '~'; case 62: return ']';
	case 64: return '|'; case 101: return 0x20ac; default: return 0;
	}
}
int gw_sms_text(unsigned encoding, const uint8_t *p, size_t n, txms_buffer *out)
{
	if (!p || n > GW_BODY_MAX || out->data) return GW_INVALID;
	if (encoding == 2) return txms_utf16be_to_utf8(p, n, out) ? GW_INVALID : 1;
	/* 8-bit SMS is gateway-defined UTF-8/hex, not Latin-1 guessed from binary. */
	if (encoding == 1) return copy(out, p, n);
	if (encoding != 0) return GW_UNSUPPORTED;
	txms_buffer be = {0};
	be.data = malloc(n * 2 + 1);
	if (!be.data) return GW_STORAGE;
	for (size_t i = 0; i < n; i++) {
		if (p[i] > 127) goto invalid;
		uint16_t c = gsm[p[i]];
		if (p[i] == 27) {
			if (++i >= n || !(c = extension(p[i]))) goto invalid;
		}
		be.data[be.len++] = (uint8_t)(c >> 8); be.data[be.len++] = (uint8_t)c;
	}
	int rc = txms_utf16be_to_utf8(be.data, be.len, out);
	txms_buffer_free(&be); return rc ? GW_INVALID : 1;
invalid:
	txms_buffer_free(&be); return GW_INVALID;
}
int gw_udh_parse(const uint8_t *p, size_t n, gw_udh *out)
{
	if (!p || !out || !n || n != (size_t)p[0] + 1) return GW_INVALID;
	memset(out, 0, sizeof(*out));
	for (size_t i = 1; i < n;) {
		if (n - i < 2) return GW_INVALID;
		unsigned tag = p[i++], len = p[i++];
		if (n - i < len) return GW_INVALID;
		if (tag == 0 || tag == 8) {
			if (out->total || len != (tag ? 4u : 3u)) return GW_INVALID;
			out->reference_bits = tag ? 16 : 8;
			out->reference = tag ? (uint16_t)((p[i] << 8) | p[i + 1]) : p[i];
			out->total = p[i + len - 2]; out->sequence = p[i + len - 1];
			if (!out->total || !out->sequence || out->sequence > out->total) return GW_INVALID;
		} else if (tag == 4 || tag == 5) {
			if (out->has_port || len != (tag == 4 ? 2u : 4u)) return GW_INVALID;
			out->has_port = 1;
			out->destination_port = tag == 4 ? p[i] : (uint16_t)((p[i] << 8) | p[i + 1]);
		} else if (tag == 0x24 || tag == 0x25) return GW_UNSUPPORTED; /* national tables */
		i += len;
	}
	return 1;
}
static int unpack(const uint8_t *p, size_t n, size_t start, size_t count, txms_buffer *out)
{
	if (start > n * 8 || count > (n * 8 - start) / 7) return GW_INVALID;
	out->data = malloc(count + 1);
	if (!out->data) return GW_STORAGE;
	for (size_t i = 0; i < count; i++) {
		size_t bit = start + i * 7, byte = bit / 8;
		unsigned value = p[byte] >> (bit % 8);
		if (bit % 8 > 1 && byte + 1 < n) value |= (unsigned)p[byte + 1] << (8 - bit % 8);
		out->data[i] = (uint8_t)(value & 127);
	}
	out->len = count; out->data[count] = 0; return 1;
}
static unsigned decimal(uint8_t c) { return (unsigned)(c & 15) * 10 + (c >> 4); }
int gw_sms_parse(const uint8_t *raw, size_t n, int framing, gw_sms *out)
{
	const uint8_t *p = raw;
	size_t raw_n = n, i = 0, oa_n, header = 0;
	int rc = GW_INVALID;
	if (!p || !out || n < 1 || n > 512 || framing < 0 || framing > 2) return GW_INVALID;
	memset(out, 0, sizeof(*out));
	if (framing == 1) {
		size_t skip = (size_t)p[0] + 1;
		if (skip >= n || p[0] > 12) return GW_INVALID;
		p += skip; n -= skip;
	} else if (framing == 2) {
		/* TS 24.011 RP-DATA network-to-MS: type, ref, OA, DA, UD. */
		if (n < 5 || p[0] != 1) return GW_UNSUPPORTED;
		i = 2;
		for (unsigned a = 0; a < 2; a++) {
			if (i >= n || p[i] > 12 || (size_t)p[i] >= n - i) return GW_INVALID;
			i += (size_t)p[i] + 1;
		}
		if (i >= n || p[i] != n - i - 1) return GW_INVALID;
		p += i + 1; n -= i + 1; i = 0;
	}
	if (n < 13 || (p[0] & 3) != 0) return GW_INVALID;
	out->udhi = !!(p[i++] & 0x40);
	unsigned digits = p[i++], toa = p[i++];
	if (!digits || digits > 20 || !(toa & 0x80)) return GW_INVALID;
	oa_n = (digits + 1) / 2;
	if (n - i < oa_n + 10) return GW_INVALID;
	if ((toa & 0x70) == 0x50) {
		txms_buffer septets = {0}, text = {0};
		if (unpack(p + i, oa_n, 0, digits * 4 / 7, &septets) < 0) return GW_INVALID;
		rc = gw_sms_text(0, septets.data, septets.len, &text); txms_buffer_free(&septets);
		if (rc < 0 || text.len >= sizeof(out->sender)) { txms_buffer_free(&text); return GW_INVALID; }
		memcpy(out->sender, text.data, text.len); txms_buffer_free(&text);
	} else {
		size_t used = 0;
		if ((toa & 0x70) == 0x10) out->sender[used++] = '+';
		for (unsigned d = 0; d < digits; d++) {
			unsigned v = (p[i + d / 2] >> ((d % 2) * 4)) & 15;
			if (v > 9) return GW_INVALID;
			out->sender[used++] = (char)('0' + v);
		}
		if (digits % 2 && (p[i + oa_n - 1] >> 4) != 15) return GW_INVALID;
	}
	i += oa_n; i++; /* PID is not interpreted as transaction data. */
	out->dcs = p[i++];
	if ((out->dcs & 0xc0) == 0 && !(out->dcs & 0x20) && ((out->dcs >> 2) & 3) != 3)
		out->encoding = (out->dcs >> 2) & 3;
	else if ((out->dcs & 0xf8) == 0xf0) out->encoding = (out->dcs & 4) ? 1 : 0;
	else return GW_UNSUPPORTED;
	memcpy(out->timestamp, p + i, 7); i += 7;
	for (unsigned t = 0; t < 7; t++) {
		uint8_t v = out->timestamp[t]; if (t == 6) v &= 0xf7;
		if ((v & 15) > 9 || (v >> 4) > 9) return GW_INVALID;
	}
	if (!decimal(out->timestamp[1]) || decimal(out->timestamp[1]) > 12 || !decimal(out->timestamp[2]) || decimal(out->timestamp[2]) > 31 || decimal(out->timestamp[3]) > 23 || decimal(out->timestamp[4]) > 59 || decimal(out->timestamp[5]) > 59) return GW_INVALID;
	out->udl = p[i++];
	size_t ud_bytes = out->encoding == 0 ? ((size_t)out->udl * 7 + 7) / 8 : out->udl;
	if (ud_bytes > 140 || n - i != ud_bytes) return GW_INVALID;
	if (out->udhi) {
		if (!ud_bytes || (header = (size_t)p[i] + 1) > ud_bytes) return GW_INVALID;
		rc = gw_udh_parse(p + i, header, &out->concat);
		if (rc < 0) return rc;
		if (out->concat.has_port) return GW_UNSUPPORTED; /* includes WAP Push */
	}
	if (out->encoding == 0) {
		/* Round UDH to septets BEFORE extracting text; preserve ESC across parts. */
		size_t skip = (header * 8 + 6) / 7;
		if (skip > out->udl) return GW_INVALID;
		rc = unpack(p + i, ud_bytes, skip * 7, out->udl - skip, &out->payload);
	} else rc = copy(&out->payload, p + i + header, ud_bytes - header);
	if (rc < 0) return rc;
	if (copy(&out->original, raw, raw_n) < 0) { gw_sms_free(out); return GW_STORAGE; }
	return 1;
}
void gw_sms_free(gw_sms *sms)
{
	txms_buffer_free(&sms->payload); txms_buffer_free(&sms->original);
}
