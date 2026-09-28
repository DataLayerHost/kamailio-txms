/* SPDX-License-Identifier: MIT */
#include "gateway.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	if ((argc != 2 && argc != 3) || curl_global_init(CURL_GLOBAL_DEFAULT)) return 1;
	gw_rpc_config c = {argv[1], "operator_broadcast", getenv("TEST_RPC_USERNAME"), getenv("TEST_RPC_PASSWORD"), getenv("TEST_RPC_TOKEN"), 200, argc == 3 ? atoi(argv[2]) : 1};
	txms_buffer hex = {(uint8_t *)"0xabcd",6};
	printf("%d\n",gw_rpc_submit(&c,42,&hex)); curl_global_cleanup(); return 0;
}
