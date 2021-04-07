extern "C" {
    // #include <sys/time.h>
    // #include <pthread.h>
    #include <spdk/crc32.h>
    // #include <isa-l.h>

    #include <spdk/env.h>
    // #include "crc32.h"
    #include <spdk/histogram_data.h>

    //DPDK
    #include <rte_ring.h>
    #include <rte_mempool.h>
    #include <rte_malloc.h>


    #include  "bs_ipc_msg.h"

}
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>
#include <cstdint>
#include <gflags/gflags.h>
#include <atomic>
#include <unordered_map>
#include <queue>
#include <assert.h>
#include <map>
#include <random>

DEFINE_string(cpumask,"0x1","CPU mask");
DEFINE_uint32(shmid, 1, "Share Memory Id");
DEFINE_bool(randombuffer, true, "用随机 buffer 模拟 cache miss / False, 使用同一块buffer");
DEFINE_uint32(rm, 1024, "Randomrized memeory region size(MiB)");
DEFINE_uint32(cs, 4, "Chunk Size(KiB)");
DEFINE_uint32(mix_large, 20 , "混入大请求(128K)的比例(0~100)");
DEFINE_uint32(burst_size, 1, "Client 一次发送的 burst size");
DEFINE_uint32(nj, 1, "numjobs");
DEFINE_bool(thread, false, "每个 job 一个pthread ");


static double g_cutoff[] = {
    0.05,
    0.10,
    0.20,
    0.30,
    0.40,
    0.50,
    0.70,
    0.80,
    0.90,
    0.95,
    0.99,
    0.999, 
    0.9999, 
    -1
};

#define BS_SESS_SQ_SZ 128
#define BS_SESS_CQ_SZ 128
#define BS_SESS_MQ_SZ 32
#define BS_SESS_MSG_CACHE_SZ 512
#define BS_SESS_MSG_SZ 64u


rte_mempool *rte_mempool_create_hepler(char *name , uint64_t n , uint64_t elem_size) {
    return rte_mempool_create(name, n , elem_size , 0 , 0 , NULL , NULL , NULL , NULL ,
            SOCKET_ID_ANY, 0);
}

struct  bs_target_session {
    uint64_t uniq_id;
    uint64_t bs_uniq_id;

    struct rte_ring *msg_q; //Recv Ctrl Response
    struct rte_ring *cq;   //SPSC 
    struct rte_ring *sq;   //SPSC
    struct rte_mempool *msg_cache; 

};


void bs_target_session_init(struct  bs_target_session* bs, uint64_t uniq_id_)  {
    
    bs->uniq_id = uniq_id_;
    char name[32];
    sprintf(name , "cli_%lu_msgq", uniq_id_);
    bs->msg_q =  rte_ring_create(name, BS_SESS_MQ_SZ, SOCKET_ID_ANY, RING_F_SC_DEQ | RING_F_SP_ENQ);
    
    sprintf(name , "cli_%lu_cq", uniq_id_);
    bs->cq =  rte_ring_create(name, BS_SESS_SQ_SZ, SOCKET_ID_ANY, RING_F_SC_DEQ | RING_F_SP_ENQ);
    
    sprintf(name , "cli_%lu_sq", uniq_id_);
    bs->sq =  rte_ring_create(name, BS_SESS_CQ_SZ, SOCKET_ID_ANY, RING_F_SC_DEQ | RING_F_SP_ENQ);
    
    sprintf(name , "cli_%lu_msgpool", uniq_id_);
    bs->msg_cache = rte_mempool_create_hepler(name, BS_SESS_MSG_CACHE_SZ, BS_SESS_MSG_SZ);
    
    assert(bs->msg_cache && bs->cq  && bs->sq);

};
void bs_target_session_fini(struct  bs_target_session* bs) {
    rte_mempool_free(bs->msg_cache);
    rte_ring_free(bs->cq);
    rte_ring_free(bs->sq);
    rte_ring_free(bs->msg_q);
}

int bs_target_session_connect(struct  bs_target_session* bs , const char *bs_name) {
    char bs_id[32];
    uint64_t bs_uid = 0;
    strcpy(bs_id , bs_name + 3);
    sscanf(bs_id , "%lu" , &bs_uid);

    // fprintf(stdout , "Connecting to bs_%lu ,bs_uid=%lu\n", bs_uid , bs_uid);

    struct rte_ring *msg_q = rte_ring_lookup(bs_name);
    if(!msg_q) {
        fprintf(stdout , "%s Not Found \n", bs_name);
        return -1;
    }

    connection_establish_request *cr = NULL;
    if(1) {
        rte_mempool_get(bs->msg_cache, (void**)&cr);
        assert(cr);
        cr->base.header_len = sizeof(*cr);
        cr->base.seq = 0;
        cr->base.sign = 0;
        cr->base.pad = 0;
        cr->base.src_id = bs->uniq_id;
        cr->base.to_id = bs_uid;
        cr->base.msg_type = CONNECTION_ESTABLISH_REQUEST;
        cr->msg_cache = (uint64_t)(uintptr_t)(bs->msg_cache);
        cr->msg_q = (uint64_t)(uintptr_t)(bs->msg_q);
        cr->sq = (uint64_t)(uintptr_t)(bs->sq);
        cr->cq = (uint64_t)(uintptr_t)(bs->cq);

        rte_ring_enqueue(msg_q, cr);
    }

    connection_establish_response *cp = NULL;
    if(1) {
        do {
            rte_ring_dequeue(bs->msg_q, (void**)&cp);
        } while(!cp);
        assert(cp->base.msg_type == CONNECTION_ESTABLISH_RESPONSE);
        assert(cp->base.src_id == bs_uid);
        bs->bs_uniq_id = bs_uid;
        fprintf(stdout , "Connect to bs_%lu Successfully\n" , bs_uid);

        //Free request and response
    }
    
    rte_mempool_put(bs->msg_cache,cr);
    rte_mempool_put(bs->msg_cache,cp);

    return 0;

}

int bs_target_session_close(struct  bs_target_session* bs) {

    char bs_name[32];
    sprintf(bs_name,"bs_%lu",bs->bs_uniq_id);
    struct rte_ring *msg_q = rte_ring_lookup(bs_name);


    connection_close_request *cr = NULL;
    if(1) {
        rte_mempool_get(bs->msg_cache, (void**)&cr);
        assert(cr);
        cr->base.header_len = sizeof(*cr);
        cr->base.seq = 0;
        cr->base.sign = 0;
        cr->base.pad = 0;
        cr->base.src_id = bs->uniq_id;
        cr->base.to_id = bs->bs_uniq_id;
        cr->base.msg_type = CONNECTION_CLOSE_REQUEST;

        rte_ring_enqueue(msg_q, cr);

    }

    connection_close_response *cp = NULL;
    if(1) {
        do {
            rte_ring_dequeue(bs->msg_q, (void**)&cp);
        } while(!cp);
        assert(cp->base.msg_type == CONNECTION_CLOSE_RESPONSE);
        assert(cp->base.src_id == bs->bs_uniq_id);
        // fprintf(stdout , "Close to bs_%lu Successfully\n" , bs->bs_uniq_id);
    }

    return 0;

}

double get_us() {
    return 1e6 *((double) rte_rdtsc() / (double)rte_get_tsc_hz());
} 





enum job_state {
    JOB_STATE_READY,
    JOB_STATE_RUN,
    JOB_STATE_IO_WAIT,
    JOB_STATE_EXIT,
    JOB_STATE_DIED,
};

struct job_io_u {
    union {
        write_chunk_request *wr;
        read_chunk_request *rr;
    };
    uint64_t pingpong_latency = 0;
};

struct job_context_t {

    // std::atomic<int> run = {0};

    int state_word = JOB_STATE_READY;
    struct bs_target_session *bs = NULL;
    struct job_io_u *io_u_array = NULL;
    std::default_random_engine re;
    uint64_t msg_seq = 0;

    // uint64_t wait_send = 0;
    std::vector<write_chunk_request *> wait_send;

    // uint64_t wr_submitting = 0;
    uint64_t wr_submitted = 0;
    uint64_t wr_completed = 0;

    uint64_t wr_completed_dist[6] = {0};

    uint64_t run_tsc = 0; //不是 end-start
    
    // uint64_t start_tsc = 0;
    // uint64_t end_tsc = 0;
    
    uint64_t request_tsc_total = 0;

    // uint64_t last_burst_size = 0;

    struct spdk_histogram_data *h;

    int signal = 0;

};

struct job_config {
    uint64_t job_id = 0 ;
    const char *target_name = "";
    bool randomrized = false;
    void *randombuffer = NULL;
    uint64_t randombuffer_size = 0;
    uint64_t count = 10000; //request count
    uint64_t cs = 4; //chunk size(K)
    uint64_t dp = 1;
};


static void 
spdk_histogram_data_iterate_fn(void *ctx, uint64_t start, uint64_t end, uint64_t count,
				       uint64_t total, uint64_t so_far)
{
	double so_far_pct;
	double **cutoff = (double **)ctx;
	uint64_t g_tsc_rate = spdk_get_ticks_hz();
	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far / total;
	while (so_far_pct >= **cutoff && **cutoff > 0) {
		printf("%9.5f%% : %9.3fus\n", **cutoff * 100, (double)end * 1000 * 1000 / g_tsc_rate);
		(*cutoff)++;
	}
} 

static inline double tsc_to_us(uint64_t tsc) {
    return ((1e6 * tsc) /rte_get_tsc_hz()) ;
}


// static pthread_mutex_t g_metux;
static inline void _job_metrics_dump(struct job_context_t *jt, const struct job_config *jc) {
 
    double eplase = (double) (jt->run_tsc) / rte_get_tsc_hz();
    eplase *= 1e6;
    fprintf(stdout, "[=====JOB%lu=====]\n" , jc->job_id);
    fprintf(stdout, "Session To [%s], Requests=%lu, requests avg_lat=%lf us \n" , 
        jc->target_name, jt->wr_completed, tsc_to_us(jt->request_tsc_total) / jt->wr_completed);
    for(uint32_t i = 0 ; i < 6 ; ++i) {
        if(jt->wr_completed_dist[i])
            fprintf(stdout , "%lu K size requests = %lu; " , (4ul << i) , jt->wr_completed_dist[i]);
    }
    const double *cutoff = g_cutoff;
    
    fprintf(stdout , "\n");
    
    spdk_histogram_data_iterate(jt->h , spdk_histogram_data_iterate_fn , &cutoff);

    fprintf(stdout, "\n[=====JOB%lu=====]\n" , jc->job_id);
    // fflush(stdout);

}

static int job_routine( struct job_context_t *jt, const struct job_config *jc) {
    int prev_state_word;
    if(jt->signal) {
        if(jt->state_word == JOB_STATE_READY)
            jt->state_word = JOB_STATE_DIED;
        else 
            jt->state_word = JOB_STATE_EXIT;
    }

    do {
        prev_state_word = jt->state_word;
        if(jt->state_word == JOB_STATE_READY) {
            jt->io_u_array = new job_io_u [jc->dp];
            jt->bs = new bs_target_session();
            bs_target_session_init(jt->bs, jc->job_id);
            if(bs_target_session_connect(jt->bs , jc->target_name)) {
                jt->state_word = JOB_STATE_DIED;
            } else {
                if(1) {
                    void *randomrized_buffers = jc->randombuffer;
                    uint64_t randomrized_buffer_size = jc->randombuffer_size;
                    
                    struct job_io_u *io_u_array =  jt->io_u_array;

                    for(uint64_t i = 0 ; i < jc->dp ; ++i) {
                        rte_mempool_get(jt->bs->msg_cache , (void**)&(io_u_array[i].wr));
                        assert(io_u_array[i].wr);
                        write_chunk_request *wr = io_u_array[i].wr;
                        wr->base.seq = jt->msg_seq++;
                        wr->base.header_len = sizeof(write_chunk_request);
                        wr->base.sign = i;
                        wr->base.src_id = jc->job_id;
                        wr->base.to_id = jt->bs->bs_uniq_id;
                        wr->base.msg_type = WRITE_REQUEST;
                        wr->chunk_id = 0;
                        wr->offset = 0;
                        // wr->length = jc->cs * 1024;  
                        wr->length = (jt->re() % 100 <  FLAGS_mix_large) ? 128 * 1024 : jc->cs * 1024; 
                        if(jc->randomrized) {
                            char *x = (char*)randomrized_buffers + (jt->re() %(randomrized_buffer_size / wr->length)) * wr->length;
                            wr->ptr_data = (uint64_t)(x);
                        } else {
                            wr->ptr_data = (uint64_t)randomrized_buffers;
                        }
                        jt->wait_send.push_back(wr);
                    }

                    jt->h = spdk_histogram_data_alloc_sized(10);
                }
                return jt->state_word = JOB_STATE_RUN;
            }        
        } 
        else if (jt->state_word == JOB_STATE_RUN) {
            assert(jt->wait_send.size());
            if(1) {

                uint64_t now = rte_rdtsc();
                for(auto wr : jt->wait_send) {
                    uint64_t id = wr->base.sign;
                    jt->io_u_array[id].pingpong_latency = now;
                }
                write_chunk_request** swrs = jt->wait_send.data();
                unsigned int n = rte_ring_enqueue_burst(jt->bs->sq , (void * const* )(swrs) , jt->wait_send.size() ,NULL);
                // assert( n == jt->last_burst_size);

                jt->wait_send.clear();
                jt->wr_submitted += n ;
                assert(jt->wr_submitted <= jc->count);
                return jt->state_word = JOB_STATE_IO_WAIT;
            }
        } 
        else if(jt->state_word == JOB_STATE_IO_WAIT) {
            void *randomrized_buffers = jc->randombuffer;
            uint64_t randomrized_buffer_size = jc->randombuffer_size;

            // printf("buffer size %lu MiB\n" ,randomrized_buffer_size >> 20 );

            // write_chunk_request **wrs = jt->wrs;
            uint64_t count = jc->count;

            void *msgs[BS_SESS_CQ_SZ];
            unsigned int n = rte_ring_dequeue_burst(jt->bs->cq , msgs , BS_SESS_CQ_SZ , NULL);
            
            if(!n) {
                return jt->state_word = JOB_STATE_IO_WAIT; 
            }

            {
                // printf("[JOB%lu]:%lu completed,%lu submitted \n", jc->job_id , jt->wr_completed , jt->wr_submitted);
                // write_chunk_request *resend[1024];
                unsigned int resend_n = 0;
                for(unsigned int i = 0 ; i < n ; ++i ) {
                    write_chunk_response *wc = (write_chunk_response*)msgs[i];
                    uint64_t wr_id = wc->base.sign;
                    struct job_io_u *io_u = &jt->io_u_array[wr_id];
                    io_u->pingpong_latency = rte_rdtsc() - io_u->pingpong_latency;
                    // printf(" io_u->pingpong_latency = %lu\n" ,  io_u->pingpong_latency );
                    
                    spdk_histogram_data_tally(jt->h , io_u->pingpong_latency);
                    jt->request_tsc_total += io_u->pingpong_latency ;
                    jt->wr_completed++;
                    uint32_t idx = rte_log2_u32(io_u->wr->length) - 12;
                    jt->wr_completed_dist[idx] ++;

                    if(jt->wr_submitted + resend_n < count) {
                        write_chunk_request *wr = jt->io_u_array[wr_id].wr;
                        wr->base.seq = jt->msg_seq++;
                        if(jc->randomrized) {
                            char *x = (char*)randomrized_buffers + ( jt->re() %(randomrized_buffer_size / wr->length)) * wr->length;
                            wr->ptr_data = (uint64_t)(x);
                        } 
                        if(FLAGS_mix_large) {
                            wr->length = (jt->re() % 100 < FLAGS_mix_large) ? 128 * 1024 : FLAGS_cs * 1024; 
                        }
                        jt->wait_send.push_back(wr);
                        resend_n++;
                        // resend[resend_n++] = wr;
                    }
                    rte_mempool_put(jt->bs->msg_cache, wc);
                }
                      
                if(resend_n) {
                    if(jt->wait_send.size() > 0  || jt->wr_submitted + jt->wait_send.size() == count ) {
                        return jt->state_word = JOB_STATE_RUN;
                    }
                }
                if(jt->wr_completed == count) {
                    return jt->state_word = JOB_STATE_EXIT;
                }
            } 
        } 
        else if(jt->state_word == JOB_STATE_EXIT) {
            
            for(uint64_t i = 0 ; i < jc->dp ; ++i) {
                rte_mempool_put(jt->bs->msg_cache , jt->io_u_array[i].wr);
            }
            // _job_metrics_dump(jt , jc);
            bs_target_session_close(jt->bs);
            bs_target_session_fini(jt->bs);
            delete jt->bs;

            delete[] jt->io_u_array;
            jt->state_word = JOB_STATE_DIED;

        } 
        else if(jt->state_word == JOB_STATE_DIED) {
            
        }
    } while (jt->state_word != prev_state_word);

    // jt->run_tsc += rte_get_tsc_cycles() - run_tsc;
    return jt->state_word;
}


struct pthread_args {
    pthread_t p;

    std::atomic<bool> start = {false};

    uint64_t start_tsc;
    std::vector<struct job_context_t *> jt;
    std::vector<const struct job_config *> jc;
};

void *pthread_routine( void *ctx ) {

    pthread_args *arg = (pthread_args*)ctx;
    uint64_t njobs = arg->jc.size();
    uint64_t n_exit = 0;

    void *randombuffer = spdk_zmalloc(FLAGS_rm << 20 , 0x1000, NULL ,SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);

    while(!arg->start.load())
            ;
    arg->start_tsc = rte_rdtsc();
    
    do {
        for(uint64_t i = 0 ; i < njobs ; ++i ) {
            if(arg->jt[i]->state_word != JOB_STATE_DIED) {              
                int rc = job_routine(arg->jt[i] , arg->jc[i]);
                if(rc == JOB_STATE_DIED)
                    n_exit ++;
            }
        }
    } while(n_exit < njobs);


    double eplase = tsc_to_us(rte_rdtsc()) - tsc_to_us(arg->start_tsc);
    double iops = 0.0;

    for(uint64_t i = 0 ; i < njobs ; ++i ) {
        iops += (1000.0 * arg->jt[i]->wr_completed) / eplase; 
        // printf("Total IOPS=%lf K \n " , iops);
        _job_metrics_dump(arg->jt[i] , arg->jc[i]);
    }
    printf("Use time =%lf us \n " , eplase);
    printf("Total IOPS=%lf K \n " , iops);

    spdk_free(randombuffer);

    return NULL;
};


// std::map<uint64_t , struct bs_target_session> targets;

static void* g_randombuffer;


int jconfig_init(struct job_config *confs , int n ) {
    
    assert(n <= 8);
    
    const char *bs_uid[] = {
        "bs_4",
        "bs_5",
        "bs_6",
        "bs_7",
        "bs_8",
        "bs_9",
        "bs_10",
        "bs_11",
    };

    int i = 0;
    //job0

    for(int i = 0 ; i < n ; ++i ) {
        job_config *conf = &confs[i];
        conf->target_name = bs_uid[i];
        conf->job_id = i+1;
        conf->randombuffer = g_randombuffer;
        conf->randombuffer_size = FLAGS_rm << 20;
        conf->cs = FLAGS_cs;
        conf->randomrized = FLAGS_randombuffer;
        conf->dp = FLAGS_burst_size;
        // conf->dp_low = FLAGS_burst_min;
        if(conf->dp > BS_SESS_SQ_SZ) {
            fprintf(stdout , "dp 太大，调整到%u\n" , BS_SESS_SQ_SZ);
            conf->dp = BS_SESS_SQ_SZ;
        };
        conf->count = 250000;
    }
    return i ;
} ;


int main( int argc , char **argv ) {
    google::ParseCommandLineFlags(&argc , &argv , true);

    spdk_env_opts opts;
    spdk_env_opts_init(&opts);

    opts.shm_id = FLAGS_shmid;
    opts.core_mask = FLAGS_cpumask.c_str();

    int rc = spdk_env_init(&opts);
    if(rc) {
        std::cout << "SPDK env init error!\n";
        return 1;
    }

    if(1) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(spdk_env_get_first_core() ,&cpus);
        fprintf(stdout, "Bind to cpu %u\n" , spdk_env_get_first_core());
        pthread_setaffinity_np(pthread_self(),sizeof(cpu_set_t), &cpus);
    }

    uint64_t njobs = FLAGS_nj;
    job_context_t *job = new job_context_t[njobs];
    job_config *job_conf = new job_config[njobs];
    pthread_args *pargs = new pthread_args[njobs];

    g_randombuffer = spdk_malloc(FLAGS_rm << 20 , 0x1000, NULL ,SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
    assert(g_randombuffer);

    jconfig_init(job_conf , njobs);
    
    std::vector<uint32_t> lcores;

    if(FLAGS_thread) {

        for(uint64_t i = 0 ; i < njobs ; ++i )  {
            pargs[i].jc.push_back(&job_conf[i]);
            pargs[i].jt.push_back(&job[i]);
        }
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        cpu_set_t cpus;
        unsigned i ;
        SPDK_ENV_FOREACH_CORE(i) {
            lcores.push_back(i);
        }
        for(uint64_t i = 0 ; i < njobs ; ++i )  {
            CPU_ZERO(&cpus);
            CPU_SET(lcores[i], &cpus);
            pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);

            // sched_param sp;
            // sp.sched_priority = 20;
            pthread_attr_setschedpolicy(&attr , SCHED_RR);

            pthread_create(&pargs[i].p , &attr, pthread_routine , &pargs[i]);
        }
        for(uint64_t i = 0 ; i < njobs ; ++i )  {
            pargs[i].start.store(true);
        }
        for(uint64_t i = 0 ; i < njobs ; ++i )  {
            pthread_join(pargs[i].p , NULL);
        }

    } else {
        for(uint64_t i = 0 ; i < njobs ; ++i )  {
            pargs[0].jc.push_back(&job_conf[i]);
            pargs[0].jt.push_back(&job[i]);
        }
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        unsigned i ;
        SPDK_ENV_FOREACH_CORE(i) {
            CPU_SET(i, &cpus);
        }
        pthread_create(&pargs[0].p , &attr, pthread_routine , &pargs[0]);
        pargs[0].start.store(true);
        pthread_join(pargs[0].p , NULL);
    }

    spdk_free(g_randombuffer);


    delete[] pargs;
    delete[] job_conf;
    delete[] job;

    spdk_env_fini();
}
