/* SPDX-License-Identifier: MIT */
#include "gateway.h"
int LLVMFuzzerTestOneInput(const uint8_t *p, size_t n)
{
	gw_udh udh; gw_udh_parse(p, n, &udh);
	return 0;
}
