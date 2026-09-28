/* SPDX-License-Identifier: MIT */
#include "gateway.h"
int LLVMFuzzerTestOneInput(const uint8_t *p, size_t n)
{
	txms_buffer b = {0}; gw_mime("multipart/mixed; boundary=fuzz", p, n, &b); txms_buffer_free(&b);
	return 0;
}
