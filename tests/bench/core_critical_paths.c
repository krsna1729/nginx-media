/* Native production-core microbenchmark, not an nginx capacity estimate. */
#define _GNU_SOURCE
#include "ngx_media_feed.h"
#include "ngx_media_ts_mux.h"
#include "ngx_media_rtmp_wire.h"
#include "ngx_media_ipc.h"
#include "ngx_media_registry.h"
#include "ngx_media_owner_dir.h"
#include "ngx_media_egress_manager.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"invariant failed: %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#ifdef CORE_RESOURCE_COUNTS
static _Atomic uint64_t allocations, frees, copies, copied_bytes;
void *__real_malloc(size_t); void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t); void __real_free(void *);
void *__real_memcpy(void *,const void *,size_t);
void *__real_memmove(void *,const void *,size_t);
void *__wrap_malloc(size_t n) { void *p=__real_malloc(n); if(p) allocations++; return p; }
void *__wrap_calloc(size_t n,size_t s) { void *p=__real_calloc(n,s); if(p) allocations++; return p; }
void *__wrap_realloc(void *p,size_t n) { void *q=__real_realloc(p,n); if(q) allocations++; return q; }
void __wrap_free(void *p) { if(p) frees++; __real_free(p); }
void *__wrap_memcpy(void *d,const void *s,size_t n) { copies++; copied_bytes+=n; return __real_memcpy(d,s,n); }
void *__wrap_memmove(void *d,const void *s,size_t n) { copies++; copied_bytes+=n; return __real_memmove(d,s,n); }
#endif
static uint64_t clock_ns(void) { struct timespec t; REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW,&t)==0); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static ngx_log_t log_sink;
static ngx_open_file_t log_file;
static ngx_media_buf_t *payload, *ipc_payload;
static ngx_media_frame_t frame;
static ngx_media_feed_t feed;
static ngx_media_cursor_t cursor;
static ngx_media_ts_mux_t mux;
static ngx_media_trackset_t tracks;
static ngx_media_ts_burst_t encoded;
static ngx_media_ipc_endpoint_t *sender,*receiver;
static ngx_media_registry_t registry;
static ngx_media_stream_t *stream;
static ngx_media_owner_dir_t *directory;
static uint64_t owner_hash, destinations[64];
static ngx_str_t app=ngx_string("critical"), name=ngx_string("fixture");
static _Atomic int stop_visit;
static _Atomic uint64_t visits, visited_records;
static int visitor_cpu = -1;
static uint64_t decoded_frames, decoded_bytes;
static ngx_media_frame_t captured;
static void capture(void *ctx,const ngx_media_frame_t *f) {
    (void)ctx;
    if (!f->config && captured.payload==NULL && f->media_type==NGX_MEDIA_TYPE_AUDIO) REQUIRE(ngx_media_frame_copy(&captured,f)==NGX_OK);
}
static void decoded(void *ctx,const ngx_media_frame_t *f) {
    (void)ctx;
    if(f->config) return;
    REQUIRE(f->media_type==frame.media_type && f->codec==frame.codec);
    REQUIRE(f->payload->len==payload->len && memcmp(f->payload->data,payload->data,payload->len)==0);
    decoded_frames++; decoded_bytes+=f->payload->len;
}
static ngx_int_t message(void *ctx,ngx_uint_t type,ngx_uint_t sid,uint32_t ts,ngx_media_buf_t *p) {
    size_t used; ngx_media_amf_value_t v;
    (void)ctx;
    REQUIRE(type==NGX_MEDIA_RTMP_MSG_DATA_AMF0 && sid==1 && ts==42);
    REQUIRE(p->len==payload->len && memcmp(p->data,payload->data,p->len)==0);
    REQUIRE(ngx_media_amf_read(p->data,p->len,&v,&used)==NGX_OK && used==p->len);
    decoded_frames++; decoded_bytes+=p->len;
    return NGX_OK;
}
static ngx_int_t visit(const ngx_media_egress_stats_t *s,void *ctx) {
    uint64_t *count=ctx; REQUIRE(s->active && s->destination_id); (*count)++; return NGX_OK;
}
static void *traverse(void *ctx) {
    (void)ctx;
    if (visitor_cpu >= 0) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(visitor_cpu, &mask);
        REQUIRE(pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask)==0);
    }
    while(!atomic_load(&stop_visit)) { uint64_t count=0; REQUIRE(ngx_media_egress_manager_visit(visit,&count)==NGX_OK); REQUIRE(count==64); visited_records+=count; visits++; }
    return NULL;
}
static size_t buffer_refs(uint64_t i) { (void)i; REQUIRE(ngx_media_buf_ref(payload)==payload); ngx_media_buf_unref(payload); REQUIRE(payload->refs>=1); return payload->len; }
static size_t feed_roundtrip(uint64_t i) {
    ngx_media_frame_t out; ngx_uint_t count=0;
    REQUIRE(ngx_media_feed_publish(&feed,&frame,(ngx_msec_t)i)==NGX_OK);
    REQUIRE(ngx_media_feed_read(&feed,&cursor,1,payload->len,(ngx_msec_t)i,&out,&count)==NGX_MEDIA_FEED_BATCH && count==1);
    REQUIRE(out.payload==payload && out.pts==frame.pts);
    ngx_media_feed_release(&out,1); return payload->len;
}
static size_t mux_frame(uint64_t i) {
    ngx_media_ts_burst_t burst; size_t n; (void)i;
    REQUIRE(ngx_media_ts_mux_burst_init(&mux,&burst,65536)==NGX_OK);
    REQUIRE(ngx_media_ts_mux_write_frame(&mux,&burst,&frame,1)==NGX_OK);
    REQUIRE(ngx_media_ts_mux_burst_end(&mux,&burst)==NGX_OK);
    n=ngx_media_ts_burst_size(&burst); REQUIRE(n && n%188==0);
    ngx_media_ts_mux_burst_destroy(&burst); return n;
}
static size_t demux_frame(uint64_t i) {
    ngx_media_ts_demux_t d; ngx_media_ts_demux_conf_t c={8,65536}; ngx_media_ts_sink_t sink={NULL,decoded}; ngx_media_ts_demux_stats_t stats; uint64_t before=decoded_frames; (void)i;
    REQUIRE(ngx_media_ts_demux_init(&d,&c,&sink,NULL,&log_sink)==NGX_OK);
    REQUIRE(ngx_media_ts_demux_feed(&d,encoded.backing->data,encoded.backing->len)==NGX_OK);
    ngx_media_ts_demux_flush(&d); ngx_media_ts_demux_stats(&d,&stats);
    REQUIRE(decoded_frames==before+1 && !stats.crc_errors && !stats.psi_errors && !stats.pes_errors && !stats.sync_errors && !stats.continuity_errors && !stats.transport_errors);
    ngx_media_ts_demux_destroy(&d); return encoded.backing->len;
}
static size_t rtmp_parse(uint64_t i) {
    ngx_media_rtmp_writer_t w; ngx_media_rtmp_packet_t p; ngx_media_rtmp_reader_t r; size_t n=0,used; uint64_t before=decoded_frames; (void)i;
    ngx_media_rtmp_writer_init(&w,128,65536); ngx_media_rtmp_packet_init(&p); ngx_media_rtmp_reader_init(&r);
    REQUIRE(ngx_media_rtmp_writer_message(&w,&p,3,NGX_MEDIA_RTMP_MSG_DATA_AMF0,1,42,payload,0,payload->len)==NGX_OK);
    for(ngx_uint_t j=0;j<p.nparts;j++) { REQUIRE(ngx_media_rtmp_reader_feed(&r,p.parts[j].data,p.parts[j].len,&used,message,NULL)==NGX_OK); REQUIRE(used==p.parts[j].len); n+=used; }
    REQUIRE(decoded_frames==before+1); ngx_media_rtmp_reader_reset(&r); ngx_media_rtmp_packet_destroy(&p); return n;
}
static size_t ipc_roundtrip(uint64_t i) {
    ngx_media_ipc_header_t h={0}; ngx_media_ipc_frame_t f={0}; ngx_media_ipc_message_t msg; ngx_int_t rc;
    h.version=NGX_MEDIA_IPC_VERSION; h.type=NGX_MEDIA_IPC_MSG_AUDIO; h.hash=owner_hash; h.incarnation=1; h.sequence=i+1;
    REQUIRE(ngx_media_ipc_send(sender,&h,ipc_payload,0,ipc_payload->len)==NGX_OK);
    do { REQUIRE(ngx_media_ipc_recv(receiver,&msg)==NGX_OK); rc=ngx_media_ipc_frame_feed(&f,&msg); ngx_media_ipc_message_release(&msg); REQUIRE(rc==NGX_AGAIN || rc==NGX_OK); } while(rc==NGX_AGAIN);
    REQUIRE(f.payload && f.payload->len==ipc_payload->len && f.header.sequence==i+1 && memcmp(f.payload->data,ipc_payload->data,ipc_payload->len)==0);
    ngx_media_ipc_frame_reset(&f); return ipc_payload->len;
}
static size_t registry_lookup(uint64_t i) { (void)i; REQUIRE(ngx_media_registry_stream(&registry,&app,&name)==stream); REQUIRE(ngx_media_registry_stream_is_live(&registry,stream)); return app.len+name.len; }
static size_t owner_roundtrip(uint64_t i) {
    ngx_media_owner_record_t r;
    REQUIRE(ngx_media_owner_dir_claim(directory,owner_hash,0)!=NULL);
    ngx_media_owner_dir_heartbeat(directory,owner_hash,0,1,i,1);
    REQUIRE(ngx_media_owner_dir_observe(directory,owner_hash,&r)==NGX_OK && r.frames==i && r.generation==1 && r.hash==owner_hash);
    return sizeof(r);
}
static size_t egress_report(uint64_t i) { ngx_media_egress_report_t r={0}; r.delivered_bytes=i+1; ngx_media_egress_manager_report(destinations[i%64],&r); return sizeof(r); }
static ngx_int_t delivered(const ngx_media_egress_stats_t *s,void *ctx) { *(uint64_t *)ctx+=s->delivered_bytes; return NGX_OK; }
static int compare(const void *a,const void *b) { uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b; return (x>y)-(x<y); }
static void measure(const char *name_,size_t (*op)(uint64_t),unsigned n,unsigned repeat) {
    uint64_t *samples=malloc(n*sizeof(*samples)),bytes=0,start,end,a0=0,f0=0,c0=0,b0=0; REQUIRE(samples);
    uint64_t visit_before, record_before, visit_count, record_count;
    for(unsigned i=0;i<32;i++) op(i);
#ifdef CORE_RESOURCE_COUNTS
    a0=allocations; f0=frees; c0=copies; b0=copied_bytes;
#endif
    visit_before=atomic_load(&visits); record_before=atomic_load(&visited_records);
    start=clock_ns(); for(unsigned i=0;i<n;i++) { uint64_t t=clock_ns(); bytes+=op(i); samples[i]=clock_ns()-t; } end=clock_ns();
    visit_count=atomic_load(&visits)-visit_before;
    record_count=atomic_load(&visited_records)-record_before;
#ifdef CORE_RESOURCE_COUNTS
    a0=allocations-a0; f0=frees-f0; c0=copies-c0; b0=copied_bytes-b0;
#else
    (void)a0; (void)f0; (void)c0; (void)b0;
#endif
    qsort(samples,n,sizeof(*samples),compare);
    printf("{\"operation\":\"%s\",\"repeat\":%u,\"operations\":%u,\"bytes\":%llu,\"elapsed_ns\":%llu,\"ops_per_second\":%.3f,\"bytes_per_second\":%.3f,\"p50_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,\"metrics_visits\":%llu,\"metrics_records\":%llu",name_,repeat,n,(unsigned long long)bytes,(unsigned long long)(end-start),n*1e9/(end-start),bytes*1e9/(end-start),(unsigned long long)samples[(n-1)*500/1000],(unsigned long long)samples[(n-1)*950/1000],(unsigned long long)samples[(n-1)*990/1000],(unsigned long long)samples[(n-1)*999/1000],(unsigned long long)visit_count,(unsigned long long)record_count);
#ifdef CORE_RESOURCE_COUNTS
    printf(",\"mode\":\"native_linker_wrapped_resources\",\"observable_allocator_calls\":%llu,\"observable_free_calls\":%llu,\"observable_copy_calls\":%llu,\"observable_copy_bytes\":%llu",(unsigned long long)a0,(unsigned long long)f0,(unsigned long long)c0,(unsigned long long)b0);
#else
    printf(",\"mode\":\"native_uninstrumented\",\"observable_allocator_calls\":null,\"observable_free_calls\":null,\"observable_copy_calls\":null,\"observable_copy_bytes\":null");
#endif
    printf(",\"total_allocations\":null,\"total_copy_bytes\":null,\"sorted_latency_samples_ns\":[");
    for(unsigned i=0;i<n;i++) printf("%s%llu",i?",":"",(unsigned long long)samples[i]);
    puts("]}"); free(samples);
}
int main(int argc,char **argv) {
    unsigned n=argc>2?(unsigned)strtoul(argv[2],NULL,10):2000; FILE *f; long len; u_char *data; ngx_pool_t *pool; ngx_cycle_t cycle_={0}; ngx_media_ts_demux_t d; ngx_media_ts_demux_conf_t dc={8,65536}; ngx_media_ts_sink_t sink={NULL,capture}; ngx_media_ts_mux_conf_t mc; ngx_media_feed_conf_t fc={64,4*1024*1024,0}; pthread_t thread;
    REQUIRE((argc==3 || argc==4) && n>=1000 && n<=100000);
    visitor_cpu=argc==4 ? atoi(argv[3]) : -1;
    REQUIRE(visitor_cpu>=-1 && visitor_cpu<CPU_SETSIZE);
    log_file.fd=STDERR_FILENO; log_sink.file=&log_file; log_sink.log_level=NGX_LOG_ERR;
    ngx_time_init(); ngx_pid=getpid(); ngx_pagesize=(ngx_uint_t)sysconf(_SC_PAGESIZE); ngx_cacheline_size=64;
    pool=ngx_create_pool(16384,&log_sink); REQUIRE(pool); cycle_.pool=pool; cycle_.log=&log_sink;
    ngx_core_conf_t core_conf={0}; void ***conf[1]={(void ***)&core_conf}; core_conf.worker_processes=1; ngx_core_module.index=0; cycle_.conf_ctx=conf; ngx_cycle=&cycle_;
    f=fopen(argv[1],"rb"); REQUIRE(f); REQUIRE(fseek(f,0,SEEK_END)==0); len=ftell(f); REQUIRE(len>0 && len<=4*1024*1024); rewind(f); data=malloc((size_t)len); REQUIRE(data && fread(data,1,(size_t)len,f)==(size_t)len); fclose(f);
    ngx_media_frame_init(&captured); REQUIRE(ngx_media_ts_demux_init(&d,&dc,&sink,NULL,&log_sink)==NGX_OK); REQUIRE(ngx_media_ts_demux_feed(&d,data,(size_t)len)==NGX_OK); ngx_media_ts_demux_flush(&d); ngx_media_ts_demux_destroy(&d); free(data); REQUIRE(captured.payload);
    frame=captured; payload=frame.payload;
    REQUIRE(ngx_media_feed_init(&feed,&fc,&log_sink)==NGX_OK); ngx_media_feed_cursor_init(&feed,&cursor);
    ngx_media_ts_mux_conf_default(&mc); REQUIRE(ngx_media_ts_mux_init(&mux,&mc,&log_sink)==NGX_OK); REQUIRE(ngx_media_trackset_init(&tracks,1,&log_sink)==NGX_OK);
    ngx_media_track_t audio={0}; audio.media_type=frame.media_type; audio.codec=frame.codec; audio.payload_format=frame.payload_format; REQUIRE(ngx_media_trackset_add(&tracks,&audio)==0); REQUIRE(ngx_media_ts_mux_set_tracks(&mux,&tracks)==NGX_OK);
    REQUIRE(ngx_media_ts_mux_burst_init(&mux,&encoded,65536)==NGX_OK); REQUIRE(ngx_media_ts_mux_write_frame(&mux,&encoded,&frame,1)==NGX_OK); REQUIRE(ngx_media_ts_mux_burst_end(&mux,&encoded)==NGX_OK);
    ipc_payload=ngx_media_buf_alloc(131072); REQUIRE(ipc_payload); for(size_t i=0;i<131072;i++) ipc_payload->data[i]=payload->data[i%payload->len]; REQUIRE(ngx_media_buf_freeze(ipc_payload,131072)==NGX_OK); REQUIRE(ngx_media_ipc_pair_create(&sender,&receiver,&log_sink)==NGX_OK);
    REQUIRE(ngx_media_registry_init(&registry,pool,&log_sink)==NGX_OK); stream=ngx_media_registry_stream_create(&registry,&app,&name,&fc,&log_sink); REQUIRE(stream);
    owner_hash=ngx_media_owner_hash(&app,&name); REQUIRE(ngx_media_owner_dir_shm_create(&cycle_,256,&log_sink)==NGX_OK); directory=ngx_media_owner_dir_attach(&cycle_,&log_sink); REQUIRE(directory);
    ngx_media_egress_descriptor_t desc={0}; desc.application=app; desc.stream=name; desc.destination=name; desc.protocol=NGX_MEDIA_DEST_RTMP; desc.engine=NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP;
    for(unsigned i=0;i<64;i++) REQUIRE(ngx_media_egress_manager_admit(&desc,&destinations[i],&log_sink)==NGX_OK);
    for(unsigned rep=0;rep<3;rep++) {
        measure("buffer_refs_control_before",buffer_refs,n,rep); measure("feed_publish_read",feed_roundtrip,n,rep); measure("ts_mux",mux_frame,n,rep); measure("ts_demux",demux_frame,n,rep); measure("ipc_frame_reassembly",ipc_roundtrip,n,rep); measure("registry_lookup_liveness",registry_lookup,n,rep); measure("owner_claim_heartbeat_observe",owner_roundtrip,n,rep);
        /* Valid AMF0 is encoded by the production writer, then chunked and parsed. */
        ngx_media_buf_t *saved=payload; ngx_media_amf_writer_t aw; payload=ngx_media_buf_alloc(4096); REQUIRE(payload); ngx_media_amf_writer_init(&aw,payload->data,4096); REQUIRE(ngx_media_amf_put_string(&aw,(const u_char *)"onMetaData",10)==NGX_OK); REQUIRE(ngx_media_buf_freeze(payload,13)==NGX_OK); measure("rtmp_chunk_amf_parse",rtmp_parse,n,rep); ngx_media_buf_unref(payload); payload=saved;
        measure("egress_report_alone",egress_report,n,rep); stop_visit=0; visits=0; visited_records=0; REQUIRE(pthread_create(&thread,NULL,traverse,NULL)==0);
        while(atomic_load(&visits)==0) sched_yield();
        measure("egress_report_during_metrics_visit",egress_report,n,rep); stop_visit=1; REQUIRE(pthread_join(thread,NULL)==0); REQUIRE(visits>0);
        uint64_t reported=0; REQUIRE(ngx_media_egress_manager_visit(delivered,&reported)==NGX_OK);
        REQUIRE(reported==(uint64_t)(rep+1)*2*((uint64_t)n*(n+1)/2+32*33/2));
        measure("buffer_refs_control_after",buffer_refs,n,rep);
    }
    for(unsigned i=0;i<64;i++) {
        ngx_media_egress_manager_release(destinations[i]);
    }
    REQUIRE(ngx_media_egress_manager_count()==0);
    ngx_media_owner_dir_release(directory,owner_hash,0); ngx_media_owner_record_t r; REQUIRE(ngx_media_owner_dir_observe(directory,owner_hash,&r)==NGX_ERROR);
    ngx_media_registry_destroy(&registry); ngx_media_ipc_close(sender); ngx_media_ipc_close(receiver); ngx_media_buf_unref(ipc_payload); ngx_media_ts_mux_burst_destroy(&encoded); ngx_media_ts_mux_destroy(&mux); ngx_media_trackset_destroy(&tracks); ngx_media_feed_destroy(&feed); ngx_media_frame_release(&frame); ngx_destroy_pool(pool); return 0;
}
