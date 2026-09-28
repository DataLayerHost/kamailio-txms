/* SPDX-License-Identifier: MIT */
#include "gateway.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stddef.h>
static int fail_after = -1, busy;
static void *local_alloc(void *ctx, size_t n)
{
	(void)ctx;
	if (fail_after == 0) return NULL;
	if (fail_after > 0) fail_after--;
	return malloc(n);
}
static void local_free(void *ctx, void *p) { (void)ctx; free(p); }
static int local_lock(void *ctx) { (void)ctx; return !busy; }
static void local_unlock(void *ctx) { (void)ctx; }
static int init_store(gw_store *s, gw_limits *l)
{
	gw_memory memory = {NULL, local_alloc, local_free, local_lock, local_unlock};
	return gw_store_init(s,l,&memory);
}
/* Test-only shared arena: allocation is serialized by the inherited semaphore.
 * Individual frees are unnecessary here; the whole arena is unmapped at end. */
typedef struct { size_t used; max_align_t align; unsigned char bytes[1048576]; } arena;
static arena *pool;
static sem_t *gate;
static void *arena_alloc(void *ctx, size_t n)
{
	(void)ctx; size_t align = _Alignof(max_align_t);
	n = (n + align - 1) / align * align;
	if (n > sizeof(pool->bytes) - pool->used) return NULL;
	void *p = pool->bytes + pool->used; pool->used += n; return p;
}
static void arena_free(void *ctx, void *p) { (void)ctx; (void)p; }
static int arena_lock(void *ctx) { (void)ctx; return sem_trywait(gate) == 0; }
static void arena_unlock(void *ctx) { (void)ctx; assert(sem_post(gate) == 0); }
static size_t pdu(uint8_t *p, int dcs, const uint8_t *data, size_t n, unsigned total, unsigned seq, int wide)
{
	const uint8_t prefix[] = {0,4,0x91,0x21,0x43,0,0,0x62,0x90,0x82,0x21,0,0,0};
	memcpy(p, prefix, sizeof(prefix)); p[0] = total ? 0x40 : 0; p[6] = (uint8_t)dcs;
	size_t i = sizeof(prefix), h = total ? (wide ? 7 : 6) : 0;
	p[i++] = (uint8_t)(h + n);
	if (total) {
		p[i++] = wide ? 6 : 5; p[i++] = wide ? 8 : 0; p[i++] = wide ? 4 : 3;
		if (wide) p[i++] = 0x12;
		p[i++] = 42; p[i++] = (uint8_t)total; p[i++] = (uint8_t)seq;
	}
	memcpy(p + i, data, n); return i + n;
}
static int segment(gw_store *s, unsigned total, unsigned seq, int wide, const char *to, time_t now, const char *text, txms_buffer *out)
{
	uint8_t raw[256]; gw_sms sms = {0};
	size_t n = pdu(raw, 4, (const uint8_t *)text, strlen(text), total, seq, wide);
	assert(gw_sms_parse(raw, n, 0, &sms) == 1);
	assert(gw_begin(s) == 1);
	int rc = gw_assemble(s, &sms, to, now, out);
	if (rc < 0) gw_rollback(s); else assert(gw_commit(s) == 1);
	gw_sms_free(&sms); return rc;
}
static void sms_tests(void)
{
	uint8_t raw[256]; gw_sms sms = {0}; txms_buffer text = {0}; gw_udh h;
	assert(gw_udh_parse((const uint8_t *)"\x05\x00\x03\x01\x02\x00", 6, &h) < 0);
	assert(gw_udh_parse((const uint8_t *)"\x05\x00\x04\x01\x02\x01", 6, &h) < 0);
	size_t n = pdu(raw, 8, (const uint8_t *)"\x12\x34\xab\xcd", 4, 0, 0, 0);
	assert(gw_sms_parse(raw, n, 0, &sms) == 1);
	assert(!strcmp(sms.sender, "+1234") && sms.original.len == n && !memcmp(sms.original.data, raw, n));
	assert(gw_sms_text(sms.encoding, sms.payload.data, sms.payload.len, &text) == 1);
	txms_buffer hex = {0}; assert(!txms_decode(text.data, text.len, &hex));
	assert(!strcmp((char *)hex.data, "0x1234abcd")); txms_buffer_free(&hex); txms_buffer_free(&text); gw_sms_free(&sms);
	for (size_t i = 0; i < n; i++) assert(gw_sms_parse(raw, i, 0, &sms) < 0);
	uint8_t prefixed[257]; prefixed[0] = 0; memcpy(prefixed + 1, raw, n);
	assert(gw_sms_parse(prefixed, n + 1, 1, &sms) == 1); gw_sms_free(&sms);
	uint8_t rp[300] = {1,0,0,0}; rp[4] = (uint8_t)n; memcpy(rp + 5, raw, n);
	assert(gw_sms_parse(rp, n + 5, 2, &sms) == 1); gw_sms_free(&sms);
	/* Seven header septets, then A ESC ^ B. First character starts at bit 49. */
	n = pdu(raw, 0, (const uint8_t *)"", 0, 2, 1, 0);
	uint8_t codes[] = {'A',27,20,'B'}; raw[14] = 11;
	memset(raw + n, 0, 4);
	for (size_t k = 0; k < sizeof(codes); k++) for (unsigned b = 0; b < 7; b++)
		if (codes[k] & (1u << b)) { size_t bit = 49 + k * 7 + b; raw[15 + bit / 8] |= (uint8_t)(1u << (bit % 8)); }
	n = 15 + (11 * 7 + 7) / 8;
	assert(gw_sms_parse(raw, n, 0, &sms) == 1);
	assert(gw_sms_text(0, sms.payload.data, sms.payload.len, &text) == 1);
	assert(!strcmp((char *)text.data, "A^B")); txms_buffer_free(&text); gw_sms_free(&sms);
	assert(gw_sms_text(0, (const uint8_t *)"\x1b", 1, &text) < 0);
	assert(gw_sms_text(0, (const uint8_t *)"\x1b\x01", 2, &text) < 0);
	assert(gw_sms_text(2, (const uint8_t *)"\xd8\x00", 2, &text) < 0);
}
static void multipart_tests(gw_store *s)
{
	txms_buffer out = {0};
	assert(segment(s,3,3,0,"a",100,"cc",&out)==0);
	assert(segment(s,3,1,0,"a",100,"aa",&out)==0);
	assert(segment(s,3,1,0,"a",100,"aa",&out)==0);
	assert(segment(s,3,1,0,"a",100,"xx",&out)==GW_CONFLICT);
	assert(segment(s,3,2,0,"a",100,"bb",&out)==1);
	assert(out.len == 6 && !memcmp(out.data,"aabbcc",6)); txms_buffer_free(&out);
	assert(segment(s,3,2,0,"a",100,"bb",&out)==0);
	assert(segment(s,2,1,1,"b",100,"aa",&out)==0);
	assert(segment(s,2,2,1,"c",100,"bb",&out)==0); /* destination collision */
	assert(segment(s,2,2,1,"b",100,"bb",&out)==1); txms_buffer_free(&out);
	assert(segment(s,2,1,0,"expiry",100,"aa",&out)==0);
	assert(segment(s,2,2,0,"expiry",4000,"bb",&out)==0);
	assert(segment(s,2,1,0,"expiry",4001,"aa",&out)==1); txms_buffer_free(&out);
	assert(segment(s,3,1,0,"ordered",4001,"aa",&out)==0);
	assert(segment(s,3,2,0,"ordered",4001,"bb",&out)==0);
	assert(segment(s,3,3,0,"ordered",4001,"cc",&out)==1); txms_buffer_free(&out);
	assert(segment(s,33,1,0,"huge",4001,"aa",&out)==GW_LIMIT);
}
static void mime_tests(void)
{
	txms_buffer out = {0};
	assert(gw_mime("text/plain", (const uint8_t *)"0XAB\n1234", 9, &out)==1);
	assert(!strcmp((char *)out.data,"0xab\n0x1234")); txms_buffer_free(&out);
	const char *body = "--test\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=one.txms.txt\r\nContent-Transfer-Encoding: base64\r\n\r\nMHhhYg==\r\n--test\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Disposition: attachment; filename=two.txms.txt\r\n\r\n0xcd\r\n--test--\r\n";
	assert(gw_mime("multipart/mixed; boundary=test",(const uint8_t *)body,strlen(body),&out)==1);
	assert(!strcmp((char *)out.data,"0xab\n0xcd")); txms_buffer_free(&out);
	const char *bad64 = "--x\r\nContent-Type: text/plain\r\nContent-Transfer-Encoding: base64\r\n\r\nMHhhYg=!\r\n--x--\r\n";
	assert(gw_mime("multipart/mixed; boundary=x",(const uint8_t *)bad64,strlen(bad64),&out)<0);
	const char *qp = "--x\r\nContent-Type: text/plain\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n0x=61b\r\n--x--\r\n";
	assert(gw_mime("multipart/mixed; boundary=x",(const uint8_t *)qp,strlen(qp),&out)==1);
	assert(!strcmp((char *)out.data,"0xab")); txms_buffer_free(&out);
	assert(gw_mime("multipart/mixed",(const uint8_t *)body,strlen(body),&out)<0);
	assert(gw_mime("text/plain\r\nX: injected",(const uint8_t *)"ab",2,&out)<0);
	assert(gw_mime("text/plain",(const uint8_t *)"ab",GW_BODY_MAX+1,&out)<0);
	assert(gw_mime("text/plain",(const uint8_t *)"https://provider.invalid/mms",28,&out)==GW_UNSUPPORTED);
	char bomb[16384]; size_t used = 0;
	for (unsigned i=0; i<129; i++) used += (size_t)snprintf(bomb+used,sizeof(bomb)-used,"--many\r\nContent-Type: text/plain\r\n\r\n0xab\r\n");
	used += (size_t)snprintf(bomb+used,sizeof(bomb)-used,"--many--\r\n");
	assert(gw_mime("multipart/mixed; boundary=many",(const uint8_t *)bomb,used,&out)==GW_LIMIT);
	assert(gw_mime("application/vnd.wap.mms-message",(const uint8_t *)"url",3,&out)<0);
}
static void limits_tests(void)
{
	gw_limits l; gw_limits_default(&l); l.max_messages = 1; l.max_jobs = 1; l.max_bytes = 4;
	gw_store s; txms_buffer out = {0};
	assert(init_store(&s,&l) == 1);
	assert(segment(&s,2,1,0,"a",100,"aa",&out)==0);
	assert(segment(&s,2,1,0,"b",100,"bb",&out)==GW_LIMIT);
	assert(segment(&s,2,2,0,"a",100,"bbb",&out)==GW_LIMIT);
	assert(segment(&s,2,2,0,"a",100,"bb",&out)==1); txms_buffer_free(&out);
	/* Expiration frees capacity, including completed tombstones. */
	gw_expired removed; assert(gw_expire(&s,4000,&removed)==1 && removed.groups==1);
	assert(segment(&s,2,1,0,"b",4000,"bb",&out)==0);
	txms_buffer batch = {(uint8_t *)"0xab\n0xcd",9};
	assert(gw_begin(&s)==1); assert(gw_enqueue(&s,&batch,4000)==GW_LIMIT); gw_rollback(&s);
	gw_job_id id; assert(gw_claim(&s,&id,&out,4000)==0);
	gw_store_destroy(&s);
	gw_limits_default(&l); l.max_queue_bytes = 3;
	assert(init_store(&s,&l) == 1);
	assert(segment(&s,2,1,0,"a",100,"aa",&out)==0);
	assert(segment(&s,2,1,0,"b",100,"bb",&out)==GW_LIMIT);
	gw_store_destroy(&s);
	gw_limits_default(&l); l.max_parts = 256;
	assert(init_store(&s,&l)==GW_INVALID);
}
static void collision_tests(void)
{
	gw_limits l; gw_limits_default(&l); gw_store s; txms_buffer out = {0};
	assert(init_store(&s,&l)==1);
	gw_sms a = {0}; strcpy(a.sender,"sender"); a.concat=(gw_udh){42,0,2,1,8,0}; a.payload=(txms_buffer){(uint8_t *)"aa",2};
	assert(gw_begin(&s)==1); assert(gw_assemble(&s,&a,"to",100,&out)==0); assert(gw_commit(&s)==1);
	/* Same numeric ref in 16-bit form, DCS, sender and day are distinct groups. */
	for (int kind=0; kind<4; kind++) {
		gw_sms b=a; b.concat.sequence=2;
		if (kind==0) b.concat.reference_bits=16;
		if (kind==1) b.dcs=8;
		if (kind==2) strcpy(b.sender,"different");
		if (kind==3) b.timestamp[2]=1;
		assert(gw_begin(&s)==1); assert(gw_assemble(&s,&b,"to",100,&out)==0); assert(gw_commit(&s)==1);
	}
	a.concat.sequence=2; assert(gw_begin(&s)==1); assert(gw_assemble(&s,&a,"to",100,&out)==1); assert(gw_commit(&s)==1);
	assert(out.len==4); txms_buffer_free(&out); gw_store_destroy(&s);
}
static void lifecycle_tests(void)
{
	gw_limits l; gw_limits_default(&l); l.ttl=10; l.job_ttl=10;
	gw_store s; assert(init_store(&s,&l)==1);
	gw_stats v; assert(gw_store_stats(&s,&v)==1); size_t baseline=v.memory_bytes;
	txms_buffer batch={(uint8_t *)"0xab\n0xcd",9}, out={0}; gw_job_id id;
	assert(segment(&s,2,1,0,"idle",100,"aa",&out)==0);
	assert(gw_begin(&s)==1 && gw_enqueue(&s,&batch,100)==1 && gw_commit(&s)==1);
	assert(gw_claim(&s,&id,&out,100)==1); txms_buffer_free(&out);
	gw_expired removed;
	assert(gw_expire(&s,110,&removed)==1 && removed.groups==1 && removed.queued==1);
	assert(gw_store_stats(&s,&v)==1 && v.sending==1 && v.jobs==1);
	assert(gw_finish(&s,id,GW_RPC_SUCCESS,110)==1);
	assert(gw_expire(&s,119,&removed)==1 && !removed.finished);
	assert(gw_expire(&s,120,&removed)==1 && removed.finished==1);
	assert(gw_store_stats(&s,&v)==1 && !v.jobs && v.memory_bytes==baseline);
	busy=1; assert(gw_begin(&s)==GW_BUSY); busy=0;
	/* Failure on the second job leaves no partial batch or leaked memory. */
	fail_after=1; assert(gw_begin(&s)==1 && gw_enqueue(&s,&batch,121)==GW_STORAGE); gw_rollback(&s); fail_after=-1;
	assert(gw_store_stats(&s,&v)==1 && !v.jobs && v.memory_bytes==baseline);
	s.limits.max_state_bytes=(unsigned)baseline+1;
	assert(gw_begin(&s)==1 && gw_enqueue(&s,&batch,121)==GW_LIMIT); gw_rollback(&s);
	s.limits.max_state_bytes=l.max_state_bytes;
	assert(gw_begin(&s)==1 && gw_enqueue(&s,&batch,121)==1 && gw_commit(&s)==1);
	assert(gw_claim(&s,&id,&out,121)==1); txms_buffer_free(&out);
	assert(gw_finish(&s,id,GW_RPC_UNCERTAIN,121)==1);
	assert(gw_begin(&s)==1 && gw_enqueue(&s,&batch,122)==1 && gw_commit(&s)==1);
	assert(gw_store_stats(&s,&v)==1 && v.jobs==2 && v.uncertain==1);
	/* A failed final-segment enqueue must preserve the earlier segment. */
	assert(segment(&s,2,1,0,"rollback",122,"aa",&out)==0);
	gw_sms sms={0}; uint8_t raw[256];
	size_t n=pdu(raw,4,(const uint8_t *)"bb",2,2,2,0); assert(gw_sms_parse(raw,n,0,&sms)==1);
	s.limits.max_jobs=2; txms_buffer extra={(uint8_t *)"0xef",4};
	assert(gw_begin(&s)==1 && gw_assemble(&s,&sms,"rollback",122,&out)==1);
	assert(gw_enqueue(&s,&extra,122)==GW_LIMIT); gw_rollback(&s); txms_buffer_free(&out); gw_sms_free(&sms);
	assert(segment(&s,2,2,0,"rollback",123,"bb",&out)==1);
	assert(out.len==4 && !memcmp(out.data,"aabb",4)); txms_buffer_free(&out);
	gw_store_destroy(&s); assert(init_store(&s,&l)==1);
	assert(gw_store_stats(&s,&v)==1 && !v.jobs && !v.groups); gw_store_destroy(&s);
}
int main(void)
{
	sms_tests(); mime_tests(); limits_tests(); collision_tests(); lifecycle_tests();
	gw_store s; gw_limits limits; gw_limits_default(&limits);
	assert(init_store(&s,&limits)==1); multipart_tests(&s);
	gw_result result = {0}; assert(gw_begin(&s)==1);
	assert(gw_process(&s,"from","to","text/plain",(const uint8_t *)"0XAb",4,0,5000,&result)==1);
	assert(gw_enqueue(&s,&result.transactions,5000)==1); assert(gw_commit(&s)==1); gw_result_free(&result);
	assert(gw_begin(&s)==1);
	txms_buffer b = {(uint8_t *)"0xab\n0xcd",9}; assert(gw_enqueue(&s,&b,5000)==1); assert(gw_commit(&s)==1);
	gw_job_id id; txms_buffer hex = {0}; assert(gw_claim(&s,&id,&hex,5000)==1);
	assert(!strcmp((char *)hex.data,"0xab")); txms_buffer_free(&hex);
	assert(gw_finish(&s,id,GW_RPC_SUCCESS,5000)==1);
	assert(gw_claim(&s,&id,&hex,5000)==1); txms_buffer_free(&hex);
	/* Simulated crash leaves sending job unclaimable. */
	assert(gw_claim(&s,&id,&hex,5000)==0);
	gw_store_destroy(&s);
	pool = mmap(NULL,sizeof(*pool),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANON,-1,0);
	assert(pool != MAP_FAILED);
	char name[64]; snprintf(name,sizeof(name),"/txms-%ld",(long)getpid());
	gate = sem_open(name,O_CREAT|O_EXCL,0600,1); assert(gate != SEM_FAILED); sem_unlink(name);
	gw_memory memory = {NULL,arena_alloc,arena_free,arena_lock,arena_unlock};
	assert(gw_store_init(&s,&limits,&memory)==1);
	pid_t child = fork(); assert(child >= 0);
	if (!child) {
		assert(segment(&s,2,1,1,"fork",6000,"aa",&hex)==0);
		_exit(0);
	}
	int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
	assert(segment(&s,2,2,1,"fork",6000,"bb",&hex)==1);
	assert(hex.len==4 && !memcmp(hex.data,"aabb",4)); txms_buffer_free(&hex);
	pid_t workers[4];
	for (unsigned i=0; i<4; i++) {
		workers[i]=fork(); assert(workers[i]>=0);
		if (!workers[i]) {
			int rc; do { rc=gw_begin(&s); } while (rc==GW_BUSY);
			assert(rc==1); assert(gw_enqueue(&s,&b,6000)==1); assert(gw_commit(&s)==1); _exit(0);
		}
	}
	for (unsigned i=0; i<4; i++) assert(waitpid(workers[i],&status,0)==workers[i] && WIFEXITED(status) && !WEXITSTATUS(status));
	gw_stats stats; assert(gw_store_stats(&s,&stats)==1 && stats.jobs==2 && stats.queued==2);
	gw_store_destroy(&s); sem_close(gate); munmap(pool,sizeof(*pool));
	puts("SMS, UDH, multipart, MIME, queue and cross-process tests passed"); return 0;
}
