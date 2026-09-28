/* SPDX-License-Identifier: MIT */
#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUCKETS 1024u
struct gw_part { size_t len; uint8_t data[]; };
struct gw_group {
	gw_group *next;
	time_t created;
	size_t bytes;
	unsigned total, received, done, bucket;
	char key[768];
	gw_part *parts[];
};
struct gw_job {
	gw_job *next, *hash_next;
	gw_job_id id;
	time_t retained_since;
	size_t len;
	unsigned status, bucket;
	uint8_t hex[];
};
struct gw_shared {
	gw_group *groups[BUCKETS];
	gw_job *jobs[BUCKETS], *head, *tail;
	gw_job_id next_id;
	gw_stats stats;
};
static unsigned bucket(const void *data, size_t n)
{
	const uint8_t *p = data;
	uint64_t h = UINT64_C(14695981039346656037);
	for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * UINT64_C(1099511628211);
	/* Hashing only accelerates lookup; complete keys are always compared. */
	return (unsigned)(h % BUCKETS);
}
static void *allocate(gw_store *s, size_t n)
{
	if (n > s->limits.max_state_bytes - s->shared->stats.memory_bytes) {
		s->allocation_error = GW_LIMIT; return NULL;
	}
	void *p = s->memory.allocate(s->memory.context, n);
	if (!p) { s->allocation_error = GW_STORAGE; return NULL; }
	memset(p, 0, n); s->shared->stats.memory_bytes += n; return p;
}
static void release(gw_store *s, void *p, size_t n)
{
	if (!p) return;
	s->memory.release(s->memory.context, p); s->shared->stats.memory_bytes -= n;
}
static size_t group_size(const gw_group *g) { return sizeof(*g) + g->total * sizeof(gw_part *); }
static void clear_parts(gw_store *s, gw_group *g)
{
	for (unsigned i = 0; i < g->total; i++) if (g->parts[i]) {
		release(s, g->parts[i], sizeof(gw_part) + g->parts[i]->len);
		g->parts[i] = NULL;
	}
	s->shared->stats.segment_bytes -= g->bytes; g->bytes = 0;
}
static int expired(time_t now, time_t since, unsigned ttl)
{
	return now >= since && (uintmax_t)now - (uintmax_t)since >= ttl;
}
static void collect(gw_store *s, time_t now, gw_expired *out)
{
	gw_shared *h = s->shared;
	for (unsigned b = 0; b < BUCKETS; b++) {
		gw_group **p = &h->groups[b];
		while (*p) {
			gw_group *g = *p;
			if (!expired(now, g->created, s->limits.ttl)) { p = &g->next; continue; }
			*p = g->next; clear_parts(s, g); release(s, g, group_size(g));
			h->stats.groups--; out->groups++;
		}
	}
	gw_job **p = &h->head, *previous = NULL;
	while (*p) {
		gw_job *j = *p;
		/* An active HTTP request must retain its duplicate marker even if TTL
		 * elapses. The RPC worker releases it to normal expiry on completion. */
		if (j->status == 4 || !expired(now, j->retained_since, s->limits.job_ttl)) {
			previous = j; p = &j->next; continue;
		}
		*p = j->next; if (h->tail == j) h->tail = previous;
		gw_job **hash = &h->jobs[j->bucket];
		while (*hash != j) hash = &(*hash)->hash_next;
		*hash = j->hash_next;
		if (j->status == 0) out->queued++; else out->finished++;
		h->stats.jobs--; h->stats.transaction_bytes -= j->len;
		release(s, j, sizeof(*j) + j->len + 1);
	}
}
void gw_limits_default(gw_limits *l)
{
	*l = (gw_limits){3600, 10000, 32, GW_BODY_MAX, 10000, 64u * 1024u * 1024u,
		3600, 32u * 1024u * 1024u};
}
time_t gw_now(void)
{
	struct timespec value;
	return clock_gettime(CLOCK_MONOTONIC, &value) == 0 ? value.tv_sec : (time_t)-1;
}
int gw_store_init(gw_store *s, const gw_limits *l, const gw_memory *m)
{
	if (!s || !l || !m || !m->allocate || !m->release || !m->try_lock || !m->unlock) return GW_INVALID;
	memset(s, 0, sizeof(*s));
	if (!l->ttl || l->ttl > 604800 || !l->job_ttl || l->job_ttl > 604800 || !l->max_messages || l->max_messages > 100000 || !l->max_parts || l->max_parts > GW_PARTS_MAX || !l->max_bytes || l->max_bytes > GW_BODY_MAX || !l->max_jobs || l->max_jobs > 100000 || !l->max_queue_bytes || l->max_queue_bytes > 128u * 1024u * 1024u || l->max_state_bytes < sizeof(gw_shared) || l->max_state_bytes > 512u * 1024u * 1024u) return GW_INVALID;
	s->limits = *l; s->memory = *m;
	s->shared = m->allocate(m->context, sizeof(gw_shared));
	if (!s->shared) return GW_STORAGE;
	memset(s->shared, 0, sizeof(*s->shared)); s->shared->stats.memory_bytes = sizeof(gw_shared);
	return 1;
}
static void reset_stage(gw_store *s)
{
	s->new_group = s->target = NULL; s->part = NULL;
	s->new_jobs = s->new_jobs_tail = NULL;
	s->new_job_bytes = 0; s->new_job_count = s->sequence = s->complete = 0;
	s->assembled = s->enqueued = s->active = 0;
}
int gw_begin(gw_store *s)
{
	if (!s || !s->shared || s->active) return GW_INVALID;
	if (!s->memory.try_lock(s->memory.context)) return GW_BUSY;
	reset_stage(s); s->active = 1; return 1;
}
int gw_commit(gw_store *s)
{
	if (!s || !s->active) return GW_INVALID;
	gw_shared *h = s->shared;
	if (s->new_group) {
		gw_group *g = s->new_group;
		g->next = h->groups[g->bucket]; h->groups[g->bucket] = g; h->stats.groups++;
	}
	if (s->part) {
		gw_group *g = s->target;
		g->parts[s->sequence - 1] = s->part; g->received++;
		g->bytes += s->part->len; h->stats.segment_bytes += s->part->len;
		if (s->complete) { g->done = 1; clear_parts(s, g); }
	}
	for (gw_job *j = s->new_jobs; j; j = j->next) {
		j->id = ++h->next_id;
		j->hash_next = h->jobs[j->bucket]; h->jobs[j->bucket] = j;
	}
	if (s->new_jobs) {
		if (h->tail) h->tail->next = s->new_jobs; else h->head = s->new_jobs;
		h->tail = s->new_jobs_tail;
		h->stats.jobs += s->new_job_count; h->stats.transaction_bytes += s->new_job_bytes;
	}
	reset_stage(s); s->memory.unlock(s->memory.context); return 1;
}
void gw_rollback(gw_store *s)
{
	if (!s || !s->active) return;
	if (s->part) release(s, s->part, sizeof(*s->part) + s->part->len);
	if (s->new_group) release(s, s->new_group, group_size(s->new_group));
	for (gw_job *j = s->new_jobs, *next; j; j = next) {
		next = j->next; release(s, j, sizeof(*j) + j->len + 1);
	}
	reset_stage(s); s->memory.unlock(s->memory.context);
}
void gw_store_destroy(gw_store *s)
{
	if (!s || !s->shared) return;
	if (s->active) gw_rollback(s);
	/* Only the parent after all children have stopped may destroy shared state. */
	for (unsigned b = 0; b < BUCKETS; b++) for (gw_group *g = s->shared->groups[b], *next; g; g = next) {
		next = g->next; clear_parts(s, g); release(s, g, group_size(g));
	}
	for (gw_job *j = s->shared->head, *next; j; j = next) {
		next = j->next; release(s, j, sizeof(*j) + j->len + 1);
	}
	s->memory.release(s->memory.context, s->shared); s->shared = NULL;
}
int gw_expire(gw_store *s, time_t now, gw_expired *out)
{
	if (!out || now < 0) return GW_INVALID;
	memset(out, 0, sizeof(*out));
	int rc = gw_begin(s); if (rc < 0) return rc;
	collect(s, now, out); return gw_commit(s);
}
int gw_store_stats(gw_store *s, gw_stats *out)
{
	if (!out) return GW_INVALID;
	int rc = gw_begin(s); if (rc < 0) return rc;
	*out = s->shared->stats;
	for (gw_job *j = s->shared->head; j; j = j->next) switch (j->status) {
	case 0: out->queued++; break;
	case 1: out->succeeded++; break;
	case 2: out->rejected++; break;
	case 3: out->uncertain++; break;
	case 4: out->sending++; break;
	}
	return gw_commit(s);
}
int gw_assemble(gw_store *s, const gw_sms *sms, const char *to, time_t now, txms_buffer *out)
{
	if (!s || !s->active || s->assembled || !sms || !to || !out || out->data || now < 0 || !sms->payload.data) return GW_INVALID;
	s->assembled = 1;
	const gw_udh *c = &sms->concat;
	if (!c->total) {
		if (sms->payload.len > s->limits.max_bytes) return GW_LIMIT;
		out->data = malloc(sms->payload.len + 1); if (!out->data) return GW_STORAGE;
		memcpy(out->data, sms->payload.data, sms->payload.len); out->len = sms->payload.len; out->data[out->len] = 0; return 1;
	}
	if (c->total > s->limits.max_parts || !c->sequence || c->sequence > c->total || sms->payload.len > s->limits.max_bytes || strlen(to) >= GW_ID_MAX || !memchr(sms->sender, 0, sizeof(sms->sender))) return GW_LIMIT;
	char key[768];
	/* Full, length-prefixed identities and SCTS day disambiguate reference reuse.
	 * As before, disjoint same-day reuse of every field requires gateway policy. */
	int n = snprintf(key, sizeof(key), "%zu:%s%zu:%s/%u/%u/%u/%u/%02x%02x%02x", strlen(sms->sender), sms->sender, strlen(to), to, c->reference_bits, c->reference, c->total, sms->dcs, sms->timestamp[0], sms->timestamp[1], sms->timestamp[2]);
	if (n < 0 || (size_t)n >= sizeof(key)) return GW_LIMIT;
	unsigned b = bucket(key, (size_t)n);
	gw_group *g = s->shared->groups[b];
	while (g && strcmp(g->key, key)) g = g->next;
	/* Capacity may be reclaimed by the periodic timer. Expire the matching group
	 * lazily as well, so stale parts can never be used between timer ticks. */
	if (g && expired(now, g->created, s->limits.ttl)) {
		gw_group **p = &s->shared->groups[b]; while (*p != g) p = &(*p)->next;
		*p = g->next; clear_parts(s, g); release(s, g, group_size(g));
		s->shared->stats.groups--; g = NULL;
	}
	if (g && g->done) return 0;
	if (g && g->parts[c->sequence - 1]) {
		gw_part *part = g->parts[c->sequence - 1];
		return part->len == sms->payload.len && !memcmp(part->data, sms->payload.data, part->len) ? 0 : GW_CONFLICT;
	}
	if (!g) {
		if (s->shared->stats.groups >= s->limits.max_messages) return GW_LIMIT;
		g = allocate(s, sizeof(*g) + c->total * sizeof(gw_part *));
		if (!g) return s->allocation_error;
		g->created = now; g->total = c->total; g->bucket = b; memcpy(g->key, key, (size_t)n + 1);
		s->new_group = g;
	}
	if (g->bytes > s->limits.max_bytes - sms->payload.len || s->shared->stats.segment_bytes + sms->payload.len > s->limits.max_queue_bytes) return GW_LIMIT;
	s->target = g; s->sequence = c->sequence;
	s->part = allocate(s, sizeof(gw_part) + sms->payload.len);
	if (!s->part) return s->allocation_error;
	s->part->len = sms->payload.len; memcpy(s->part->data, sms->payload.data, sms->payload.len);
	if (g->received + 1 != g->total) return 0;
	out->data = malloc(g->bytes + sms->payload.len + 1);
	if (!out->data) return GW_STORAGE;
	for (unsigned i = 0; i < g->total; i++) {
		gw_part *part = i + 1 == c->sequence ? s->part : g->parts[i];
		memcpy(out->data + out->len, part->data, part->len); out->len += part->len;
	}
	out->data[out->len] = 0; s->complete = 1; return 1;
}
int gw_enqueue(gw_store *s, const txms_buffer *tx, time_t now)
{
	if (!s || !s->active || s->enqueued || !tx || !tx->data || !tx->len || tx->len > GW_BODY_MAX || now < 0) return GW_INVALID;
	s->enqueued = 1;
	size_t start = 0; unsigned count = 0;
	while (start < tx->len) {
		size_t end = start; while (end < tx->len && tx->data[end] != '\n') end++;
		size_t len = end - start;
		if (++count > GW_BATCH_MAX || !txms_is_hex(tx->data + start, len)) return GW_INVALID;
		unsigned b = bucket(tx->data + start, len);
		gw_job *match = s->shared->jobs[b];
		while (match && (match->len != len || memcmp(match->hex, tx->data + start, len))) match = match->hash_next;
		if (!match) {
			match = s->new_jobs;
			while (match && (match->len != len || memcmp(match->hex, tx->data + start, len))) match = match->next;
		}
		if (!match) {
			if (s->shared->stats.jobs + s->new_job_count >= s->limits.max_jobs ||
				s->shared->stats.transaction_bytes + s->new_job_bytes + len > s->limits.max_queue_bytes ||
				s->shared->next_id >= UINT64_MAX - s->new_job_count) return GW_LIMIT;
			gw_job *j = allocate(s, sizeof(*j) + len + 1); if (!j) return s->allocation_error;
			j->len = len; j->bucket = b; j->retained_since = now; memcpy(j->hex, tx->data + start, len);
			if (s->new_jobs_tail) s->new_jobs_tail->next = j; else s->new_jobs = j;
			s->new_jobs_tail = j; s->new_job_count++; s->new_job_bytes += len;
		}
		start = end + 1;
	}
	return 1;
}
int gw_claim(gw_store *s, gw_job_id *id, txms_buffer *hex, time_t now)
{
	if (!id || !hex || hex->data || now < 0) return GW_INVALID;
	int rc = gw_begin(s); if (rc < 0) return rc;
	gw_job *j = s->shared->head;
	while (j && (j->status || expired(now, j->retained_since, s->limits.job_ttl))) j = j->next;
	if (!j) { gw_commit(s); return 0; }
	hex->data = malloc(j->len + 1);
	if (!hex->data) { gw_rollback(s); return GW_STORAGE; }
	memcpy(hex->data, j->hex, j->len + 1); hex->len = j->len;
	*id = j->id; j->status = 4;
	/* Visible to all workers before HTTP, but deliberately not persistent. */
	gw_commit(s); return 1;
}
int gw_finish(gw_store *s, gw_job_id id, int status, time_t now)
{
	if (status < 1 || status > 3 || now < 0) return GW_INVALID;
	int rc = gw_begin(s); if (rc < 0) return rc;
	gw_job *j = s->shared->head; while (j && j->id != id) j = j->next;
	if (!j || j->status != 4) { gw_rollback(s); return GW_INVALID; }
	j->status = (unsigned)status; j->retained_since = now;
	return gw_commit(s);
}
