/* SPDX-License-Identifier: MIT */
#include "gateway.h"
int LLVMFuzzerTestOneInput(const uint8_t *p, size_t n)
{
	gw_sms sms = {0}; for (int f = 0; f < 3; f++) { gw_sms_parse(p, n, f, &sms); gw_sms_free(&sms); }
	return 0;
}
