/* SPDX-License-Identifier: MIT */
#include "../../core/sr_module.h"
#include "../../core/pt.h"
#include "../../core/mod_fix.h"
#include "../../core/parser/parse_from.h"
#include "../../core/parser/parse_to.h"
#include "../../core/parser/parse_content.h"
#include "../../core/cfg/cfg_struct.h"
#include "../../core/lvalue.h"
#include "../../core/mem/shm_mem.h"
#include "../../core/locking.h"
#include "../../core/timer.h"
#include "../../core/rpc.h"
#include "../../core/rpc_lookup.h"
#include "gateway.h"
#include <curl/curl.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

MODULE_VERSION

static char *rpc_url, *rpc_method, *rpc_username, *rpc_password, *rpc_token;
static int timeout_ms = -1, tls_verify = -1, framing = 2;
static int ttl = -1, max_messages = -1, max_parts = -1, max_bytes = -1, max_jobs = -1, max_queue_bytes = -1;
static int job_ttl = -1, max_state_bytes = -1, cleanup_interval = -1;
static gen_lock_t *store_lock;
static pid_t owner_pid;
static gw_store store;
static gw_limits limits;
static gw_rpc_config rpc;
static gw_result current;
static unsigned current_id;
static int current_valid;

static const char *setting(char *parameter, const char *name, const char *fallback)
{
	const char *v = parameter ? parameter : getenv(name);
	return v ? v : fallback;
}
static int number(int parameter, const char *name, unsigned fallback, unsigned *out)
{
	const char *value = getenv(name); char *end;
	if (parameter >= 0) { *out = (unsigned)parameter; return 0; }
	if (!value) { *out = fallback; return 0; }
	if (!*value || *value == '-') return -1;
	errno = 0; unsigned long n = strtoul(value, &end, 10);
	if (errno || *end || n > INT_MAX) return -1;
	*out = (unsigned)n; return 0;
}
static void *shared_allocate(void *ctx, size_t n) { (void)ctx; return shm_malloc(n); }
static void shared_release(void *ctx, void *p) { (void)ctx; shm_free(p); }
static int shared_try_lock(void *ctx) { return lock_try((gen_lock_t *)ctx) == 0; }
static void shared_unlock(void *ctx) { lock_release((gen_lock_t *)ctx); }
static void cleanup(unsigned ticks, void *arg)
{
	(void)ticks; (void)arg; gw_expired removed;
	if (gw_expire(&store, gw_now(), &removed) > 0 && removed.queued)
		LM_WARN("TxMS expired %u unsent jobs\n", removed.queued);
}
static void stats_rpc(rpc_t *api, void *ctx)
{
	gw_stats v; void *object;
	if (gw_store_stats(&store, &v) < 0) { api->fault(ctx, 503, "TxMS state busy"); return; }
	if (api->add(ctx, "{", &object) < 0) return;
	api->struct_add(object, "dddddddddd", "groups", (int)v.groups, "jobs", (int)v.jobs,
		"queued", (int)v.queued, "sending", (int)v.sending, "succeeded", (int)v.succeeded,
		"rejected", (int)v.rejected, "uncertain", (int)v.uncertain,
		"segment_bytes", (int)v.segment_bytes, "transaction_bytes", (int)v.transaction_bytes,
		"memory_bytes", (int)v.memory_bytes);
}
static const char *stats_doc[] = {"Current volatile TxMS state and memory usage", NULL};
static rpc_export_t rpc_commands[] = {{"txms.stats", stats_rpc, stats_doc, 0}, {0,0,0,0}};
static int mod_init(void)
{
	gw_limits_default(&limits);
	if (number(ttl,"TXMS_MULTIPART_TTL",limits.ttl,&limits.ttl) ||
		number(max_messages,"TXMS_MAX_MULTIPART_MESSAGES",limits.max_messages,&limits.max_messages) ||
		number(max_parts,"TXMS_MAX_PARTS",limits.max_parts,&limits.max_parts) ||
		number(max_bytes,"TXMS_MAX_MESSAGE_BYTES",limits.max_bytes,&limits.max_bytes) ||
		number(max_jobs,"TXMS_MAX_JOBS",limits.max_jobs,&limits.max_jobs) ||
		number(max_queue_bytes,"TXMS_MAX_QUEUE_BYTES",limits.max_queue_bytes,&limits.max_queue_bytes) ||
		number(job_ttl,"TXMS_JOB_TTL",limits.job_ttl,&limits.job_ttl) ||
		number(max_state_bytes,"TXMS_MAX_STATE_BYTES",limits.max_state_bytes,&limits.max_state_bytes)) return -1;
	unsigned timeout;
	if (number(timeout_ms,"CORE_RPC_TIMEOUT_MS",10000,&timeout)) return -1;
	if (tls_verify < 0) {
		const char *v = getenv("CORE_RPC_TLS_VERIFY");
		if (!v || !strcmp(v,"true")) tls_verify = 1;
		else if (!strcmp(v,"false")) tls_verify = 0;
		else return -1;
	}
	rpc = (gw_rpc_config){setting(rpc_url,"CORE_RPC_URL",NULL), setting(rpc_method,"CORE_RPC_METHOD",NULL),
		setting(rpc_username,"CORE_RPC_USERNAME",NULL), setting(rpc_password,"CORE_RPC_PASSWORD",NULL), setting(rpc_token,"CORE_RPC_TOKEN",NULL), timeout, tls_verify};
	if (framing < 0 || framing > 2 || gw_rpc_validate(&rpc) < 0) { LM_ERR("invalid TxMS configuration (RPC URL and method are required)\n"); return -1; }
	unsigned interval;
	if (number(cleanup_interval,"TXMS_CLEANUP_INTERVAL",10,&interval) || !interval || interval > 3600) return -1;
	owner_pid = getpid();
	store_lock = lock_alloc();
	if (!store_lock || !lock_init(store_lock)) return -1;
	gw_memory memory = {(void *)store_lock, shared_allocate, shared_release, shared_try_lock, shared_unlock};
	if (gw_store_init(&store,&limits,&memory) < 0) { LM_ERR("cannot initialize TxMS shared memory\n"); return -1; }
	if (rpc_register_array(rpc_commands) < 0 || register_timer(cleanup,NULL,interval) < 0) return -1;
	register_procs(1); return 0;
}
static int child_init(int rank)
{
	if (rank == PROC_INIT) return 0;
	if (rank == PROC_MAIN) {
		int pid = fork_process(PROC_NOCHLDINIT,"TxMS RPC worker",0);
		if (pid < 0) return -1;
		if (pid > 0) return 0;
		if (curl_global_init(CURL_GLOBAL_DEFAULT)) _exit(1);
		if (cfg_child_init()) _exit(1);
		for (;;) {
			gw_job_id id; txms_buffer hex = {0}; cfg_update();
			int rc = gw_claim(&store,&id,&hex,gw_now());
			if (rc == 1) {
				int status = gw_rpc_submit(&rpc,id,&hex); txms_buffer_free(&hex);
				int saved;
				do { saved = gw_finish(&store,id,status,gw_now()); if (saved == GW_BUSY) usleep(1000); } while (saved == GW_BUSY);
				if (saved < 0) LM_ERR("TxMS RPC outcome could not be saved; job remains uncertain\n");
				else LM_INFO("TxMS job %llu outcome %d\n",(unsigned long long)id,status);
			} else usleep(100000);
		}
	}
	return 0;
}
static int identity(str *s, char *out, size_t cap)
{
	if (!s || s->len <= 0 || (size_t)s->len >= cap || memchr(s->s,0,(size_t)s->len)) return -1;
	memcpy(out,s->s,(size_t)s->len); out[s->len] = 0; return 0;
}
static int process(struct sip_msg *msg, int submit)
{
	char from[GW_ID_MAX], to[GW_ID_MAX], ct[256];
	gw_result_free(&current); current_valid = 0;
	if (msg->first_line.type != SIP_REQUEST || msg->first_line.u.request.method_value != METHOD_MESSAGE ||
		parse_headers(msg,HDR_EOH_F,0) < 0 || parse_from_header(msg) < 0 || parse_to_header(msg) < 0 ||
		!msg->content_type || !msg->content_length) return -1;
	char *body = get_body(msg);
	if (!body || body < msg->buf || body > msg->buf + msg->len) return -1;
	int len = get_content_length(msg);
	if (len <= 0 || (size_t)len > limits.max_bytes || len != msg->len - (body - msg->buf)) return -1;
	if (identity(&get_from(msg)->uri,from,sizeof(from)) || identity(&get_to(msg)->uri,to,sizeof(to)) || identity(&msg->content_type->body,ct,sizeof(ct))) return -1;
	if (gw_begin(&store) < 0) return -1;
	int rc = gw_process(&store,from,to,ct,(uint8_t *)body,(size_t)len,framing,gw_now(),&current);
	if (rc == 1 && submit) rc = gw_enqueue(&store,&current.transactions,gw_now());
	if (rc < 0) { gw_rollback(&store); gw_result_free(&current); LM_WARN("TxMS input rejected: %d\n",rc); return -1; }
	/* Inspection of a complete multipart is rolled back. submit() reprocesses
	 * atomically so a full queue cannot consume the last segment. */
	if (rc == 1 && !submit) gw_rollback(&store);
	else if (gw_commit(&store) < 0) { gw_result_free(&current); return -1; }
	current_id = msg->id; current_valid = 1;
	return rc == 0 ? 2 : 1;
}
static int w_process(struct sip_msg *msg, char *a, char *b) { (void)a; (void)b; return process(msg,0); }
static int w_submit(struct sip_msg *msg, char *a, char *b) { (void)a; (void)b; return process(msg,1); }
static int w_complete(struct sip_msg *msg, char *a, char *b)
{
	(void)a; (void)b; return current_valid && current_id == msg->id && current.transactions.len ? 1 : -1;
}
static int w_get(struct sip_msg *msg, char *destination, char *unused)
{
	(void)unused; pv_spec_t *pv = (pv_spec_t *)destination; pv_value_t value = {0};
	if (w_complete(msg,NULL,NULL) < 0 || !pv || !pv->setf) return -1;
	value.flags = PV_VAL_STR; value.rs.s = (char *)current.transactions.data; value.rs.len = (int)current.transactions.len;
	return pv->setf(msg,&pv->pvp,EQ_T,&value) < 0 ? -1 : 1;
}
static void mod_destroy(void)
{
	gw_result_free(&current);
	if (getpid() != owner_pid) return;
	gw_store_destroy(&store);
	if (store_lock) { lock_destroy(store_lock); lock_dealloc(store_lock); store_lock = NULL; }
}
static cmd_export_t commands[] = {
	{"txms_process",w_process,0,0,0,REQUEST_ROUTE},
	{"txms_is_complete",w_complete,0,0,0,REQUEST_ROUTE},
	{"txms_get_transaction",w_get,1,fixup_pvar_null,fixup_free_pvar_null,REQUEST_ROUTE},
	{"txms_submit",w_submit,0,0,0,REQUEST_ROUTE},
	{"txms_process_and_submit",w_submit,0,0,0,REQUEST_ROUTE},
	{0,0,0,0,0,0}
};
static param_export_t parameters[] = {
	{"rpc_url",PARAM_STRING,&rpc_url}, {"rpc_method",PARAM_STRING,&rpc_method},
	{"rpc_username",PARAM_STRING,&rpc_username}, {"rpc_password",PARAM_STRING,&rpc_password}, {"rpc_token",PARAM_STRING,&rpc_token},
	{"rpc_timeout_ms",PARAM_INT,&timeout_ms}, {"rpc_tls_verify",PARAM_INT,&tls_verify}, {"sms_framing",PARAM_INT,&framing},
	{"multipart_ttl",PARAM_INT,&ttl}, {"max_multipart_messages",PARAM_INT,&max_messages}, {"max_parts",PARAM_INT,&max_parts},
	{"max_message_bytes",PARAM_INT,&max_bytes}, {"max_jobs",PARAM_INT,&max_jobs}, {"max_queue_bytes",PARAM_INT,&max_queue_bytes},
	{"job_ttl",PARAM_INT,&job_ttl}, {"max_state_bytes",PARAM_INT,&max_state_bytes},
	{"cleanup_interval",PARAM_INT,&cleanup_interval},
	{0,0,0}
};
struct module_exports exports = {"txms",DEFAULT_DLFLAGS,commands,parameters,0,0,0,mod_init,child_init,mod_destroy};
