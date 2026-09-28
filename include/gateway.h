/* SPDX-License-Identifier: MIT */
#ifndef TXMS_GATEWAY_H
#define TXMS_GATEWAY_H
#include "txms.h"
#include <stdint.h>
#include <time.h>
#define GW_ID_MAX 256
#define GW_BODY_MAX 2097152u
#define GW_PARTS_MAX 255u
#define GW_BATCH_MAX 128u
/* 0 pending/duplicate, 1 complete/accepted; negative values are errors. */
enum { GW_INVALID = -1, GW_LIMIT = -2, GW_STORAGE = -3, GW_UNSUPPORTED = -4, GW_CONFLICT = -5, GW_BUSY = -6 };
typedef struct {
	unsigned ttl, max_messages, max_parts, max_bytes, max_jobs, max_queue_bytes, job_ttl, max_state_bytes;
} gw_limits;
typedef struct {
	uint16_t reference, destination_port;
	uint8_t total, sequence, reference_bits, has_port;
} gw_udh;
typedef struct {
	char sender[GW_ID_MAX];
	uint8_t timestamp[7], dcs, udhi, udl, encoding;
	gw_udh concat;
	txms_buffer payload, original;
} gw_sms;
typedef struct {
	char from[GW_ID_MAX], to[GW_ID_MAX], transport[16], content_type[256];
	unsigned multipart, multipart_reference, parts, original_encoding;
	size_t original_length;
	txms_buffer transactions;
} gw_result;
typedef struct gw_shared gw_shared;
typedef struct gw_group gw_group;
typedef struct gw_part gw_part;
typedef struct gw_job gw_job;
typedef uint64_t gw_job_id;
/* Production callbacks use process-shared memory and locking. try_lock returns
 * 1 when acquired, 0 when busy. Allocate/init once before forking; destroy only
 * after all children have stopped. All staging fields remain process-private. */
typedef struct {
	void *context;
	void *(*allocate)(void *, size_t);
	void (*release)(void *, void *);
	int (*try_lock)(void *);
	void (*unlock)(void *);
} gw_memory;
typedef struct {
	gw_shared *shared;
	gw_limits limits;
	gw_memory memory;
	/* Process-private staging; initialize before fork with no active batch. */
	gw_group *new_group, *target;
	gw_part *part;
	gw_job *new_jobs, *new_jobs_tail;
	size_t new_job_bytes;
	unsigned sequence, complete, new_job_count, active, assembled, enqueued;
	int allocation_error;
} gw_store;
typedef struct {
	unsigned groups, jobs, queued, sending, succeeded, rejected, uncertain;
	size_t segment_bytes, transaction_bytes, memory_bytes;
} gw_stats;
typedef struct { unsigned groups, queued, finished; } gw_expired;
typedef struct {
	const char *url, *method, *username, *password, *token;
	long timeout_ms;
	int tls_verify;
} gw_rpc_config;
enum { GW_RPC_SUCCESS = 1, GW_RPC_REJECTED = 2, GW_RPC_UNCERTAIN = 3 };
void gw_limits_default(gw_limits *limits);
int gw_udh_parse(const uint8_t *p, size_t n, gw_udh *out);
/* framing: 0 TPDU, 1 SMSC-prefixed PDU, 2 network-to-MS RP-DATA. */
int gw_sms_parse(const uint8_t *p, size_t n, int framing, gw_sms *out);
int gw_sms_text(unsigned encoding, const uint8_t *p, size_t n, txms_buffer *out);
void gw_sms_free(gw_sms *sms);
void gw_result_free(gw_result *result);
int gw_mime(const char *content_type, const uint8_t *body, size_t n, txms_buffer *out);
int gw_store_init(gw_store *store, const gw_limits *limits, const gw_memory *memory);
void gw_store_destroy(gw_store *store);
time_t gw_now(void);
int gw_expire(gw_store *store, time_t now, gw_expired *out);
int gw_store_stats(gw_store *store, gw_stats *out);
int gw_begin(gw_store *store);
int gw_commit(gw_store *store);
void gw_rollback(gw_store *store);
/* Caller holds the shared lock through processing and enqueue. */
int gw_assemble(gw_store *store, const gw_sms *sms, const char *to, time_t now, txms_buffer *out);
int gw_enqueue(gw_store *store, const txms_buffer *transactions, time_t now);
int gw_claim(gw_store *store, gw_job_id *id, txms_buffer *hex, time_t now);
int gw_finish(gw_store *store, gw_job_id id, int status, time_t now);
int gw_process(gw_store *store, const char *from, const char *to, const char *content_type,
	const uint8_t *body, size_t n, int framing, time_t now, gw_result *out);
int gw_rpc_validate(const gw_rpc_config *config);
int gw_rpc_submit(const gw_rpc_config *config, gw_job_id id, const txms_buffer *hex);
#endif
