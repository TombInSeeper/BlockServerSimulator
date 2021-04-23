extern "C" {
    // #include <sys/time.h>
    // #include <pthread.h>
    #include <spdk/crc32.h>
    // #include <isa-l.h>

    #include <spdk/env.h>
    // #include "crc32.h"

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
#include <map>
#include <queue>
#include <random>
#include <assert.h>
#include <stack>

DEFINE_string(cpumask,"0x10","bs reactors CPU mask");
DEFINE_uint32(shmid, 1, "DPDK Share Memory Id");
DEFINE_int32(max_inplace_size , 32768 , " -1 代表全部请求就地处理 / 0 代表全部请求后台处理/ 其他代表 size ");
DEFINE_bool(force_inplace, false, "强制先小后大就地处理，此标记将覆盖 max_inplace_size");
DEFINE_bool(dry_run, false , " 基准测试");
DEFINE_string(dry_run_type, "gmalloc" , "crc/gmalloc/user_malloc 基准测试");
DEFINE_uint32(dry_run_bs, 4 , "数据块长度(4~128)");
DEFINE_bool(fake_exe, true , "用固定延迟模拟请求处理");
DEFINE_bool(force_fifo, false , "强制FIFO处理，此标记将覆盖 force_inplace 和 max_inplace_size");

uint32_t g_fixed_latency[] = {
    4, //4K
    8, //8K
    12, //16K
    16, //32K
    32, //64K
    64, //128K
};


#define SMALL_REQUEST_SIZE 0x1000;
#define MAX_RPC_BURST (32)
// std::atomic<bool> g_run;
//DPDK/SPDK

//intel storage accel library
rte_ring *g_bg_task_queue;
int g_run;
thread_local int g_master;

struct ipc_session_t {

    rte_ring *msg_q; //CTRL message

    rte_ring *sq; //IO Request
    rte_ring *cq; //IO Response
    
    rte_mempool *msg_cache;
};

enum reactor_state {
    REACTOR_READY,
    REACTOR_RUNNING,
    REACTOR_DIED
};

struct reactor_metric {
    uint64_t nr_self_requests = 0;
    uint64_t nr_others_requests = 0;
    uint64_t nr_rpc_poll_succ = 0;
    uint64_t nr_rpc_msg_recv = 0;
    // double nr_rpc_msg_avg_qd;


    // uint64_t nr_bt = 0;
    uint64_t tsc_run = 0;

    uint64_t tsc_execute_writes = 0;

    // uint64_t avg_tsc_poll_bg
    uint64_t nr_bt_deq = 0;
    uint64_t tsc_bt_enq = 0; //入队动作消耗的时间
    uint64_t tsc_bt_deq_raw = 0; //（无论是否poll到东西）出队动作消耗的时间
    uint64_t tsc_bt_deq = 0; //（poll到东西）出队动作消耗的时间
    

    uint64_t nr_others_bt_deq = 0;
    uint64_t tsc_others_bt_deq = 0; //
    uint64_t tsc_others_bt_deq_raw = 0; //


    uint64_t nr_bt_done_enq = 0;
    uint64_t tsc_bt_done_enq = 0; //入队动作消耗的时间



    uint64_t nr_bt_done_deq = 0;
    uint64_t tsc_bt_done_deq_raw = 0; //（无论是否poll到东西）出队动作消耗的时间
    uint64_t tsc_bt_done_deq = 0; //（poll到东西时）出队动作消耗的时间

    
    void dump() {
        printf("\n[self_requests:%lu", nr_self_requests); 
        printf(",others_requests=%lu" , nr_others_requests);
        printf(",rpc_msg_avg=%lf]" , (double) nr_rpc_msg_recv / (double)nr_rpc_poll_succ);
        
        printf(" enq bg_task_queue tsc=%lu. \n" , tsc_bt_enq);
        printf(" deq bg_task_queue tsc=%lu. \n" , tsc_bt_deq);
        printf(" [raw] deq others_bg_task_queue tsc percent =%lf. \n" , (double) tsc_others_bt_deq_raw / (double) tsc_run);
        printf(" deq others_bg_task_queue tsc =%lu , avg=%lu. \n" ,tsc_others_bt_deq , nr_others_requests == 0 ? 0 :tsc_others_bt_deq / nr_others_requests);
        
        uint64_t tw = (nr_others_requests + nr_self_requests);
        printf(" write requests execute tsc=%lu,avg=%lu.\n" , tsc_execute_writes , tw == 0 ? 0u : tsc_execute_writes > 0 ? tsc_execute_writes / tw : 0 );

        // printf(",rpc_msg_avg=%lf" , (double) nr_rpc_msg_recv / (double)nr_rpc_poll_succ);
        // printf(",rpc_msg_avg=%lf" , (double) nr_rpc_msg_recv / (double)nr_rpc_poll_succ);
        
        printf("\n");
    }

    reactor_metric() = default;
    ~reactor_metric() = default;
};

struct reactor_t {
    pthread_t me;
    unsigned int cpu;

    rte_ring *msg_queue; //多生产者单消费者 MPSC，用于接收控制消息。
    //立刻就地执行
    // std::priority_queue<void*,std::vector<void*>, >     
    
    
    rte_ring *bg_task_queue; //Task Queue,单生产者多消费者 SPMC
    rte_ring *bg_task_done_queue; //Task Done Queue,多生产者单消费者
    // uint32_t  

    // rte_mempool *small_buffers; // 4K
    // rte_mempool *middle_buffers; //16K
    // rte_mempool *large_buffers; //64K

    // rte_mempool *buffers[6];

    rte_mempool *bg_task_cache;

    // std::stack<bg_task> bg_task_local_cache;

    std::atomic<int> active;
    std::map<uint64_t , ipc_session_t> clients;
    uint64_t poll_current_client;

    std::vector<unsigned int> neighbors;
    unsigned int ni = 0;
    struct reactor_metric metrics;

    // std::unordered_map<uint64_t , 

    int32_t max_inplace_size;

    reactor_t(int cpu_) noexcept :cpu(cpu_) {
        me = pthread_self();

        char rname[32];
        sprintf(rname,"bs_%d",cpu);
        msg_queue = rte_ring_create(rname,128,SOCKET_ID_ANY, RING_F_SC_DEQ);

        sprintf(rname,"bs_%d_tq",cpu);
        // rte_ring
        bg_task_queue = rte_ring_create(rname, 65536 ,SOCKET_ID_ANY, RING_F_SP_ENQ);
        
        sprintf(rname,"bs_%d_btq",cpu);
        bg_task_done_queue = rte_ring_create(rname, 65536 ,SOCKET_ID_ANY, RING_F_SC_DEQ);
        assert(msg_queue && bg_task_queue && bg_task_done_queue);
    
        // for(int i = 0 ; i < 6 ; ++i) {
        //     uint32_t unit = 1ul << 12 << i ;
        //     uint32_t n = (1ul << 13 >> i) - 1;
        //     sprintf(rname,"bs_%d_bufs_%uK",cpu , unit);
        //     buffers[i] = rte_mempool_create(rname, n , unit , 128  , 0 , NULL ,NULL, NULL, NULL, SOCKET_ID_ANY , 0);
        //     assert(buffers[i]);
        // }

        sprintf(rname,"bs_%d_bg_task",cpu);
        bg_task_cache = rte_mempool_create(rname, MAX_RPC_BURST * 4 -1 , 64u , MAX_RPC_BURST , 0 , NULL ,NULL, NULL, NULL, SOCKET_ID_ANY , 0);
        assert(bg_task_cache);
 

        poll_current_client = UINT64_MAX;
        active.store(REACTOR_READY);
    }

    ~reactor_t() noexcept {
        // for(int i = 0 ; i < 6 ; ++i)
        //     rte_mempool_free(buffers[i]);
        rte_ring_free(bg_task_done_queue);
        rte_ring_free(bg_task_queue);
        rte_ring_free(msg_queue);
    }




};

struct reactor_t *g_reactos[128];

static inline rte_ring *bg_task_queue_lookup(int cpu) {
    return g_reactos[cpu]->bg_task_queue;
}

static inline rte_ring *bg_task_done_queue_lookup(int cpu) {
    return g_reactos[cpu]->bg_task_done_queue;
}

static void reactor_ctrl_msg_handler(reactor_t *rctx, void *msg) {
    msg_base *base = (msg_base*)msg;
    
    //握手
    if(base->msg_type == CONNECTION_ESTABLISH_REQUEST) {
        connection_establish_request *cr = NULL;
        connection_establish_response *cps = NULL;
        
        cr = (connection_establish_request *)base;
        rte_ring* msg_q = (rte_ring*)(void*)(uintptr_t)(cr->msg_q);
        rte_ring* sq = (rte_ring*)(void*)(uintptr_t)(cr->sq);
        rte_ring* cq = (rte_ring*)(void*)(uintptr_t)(cr->cq);
        rte_mempool *msg_cache = (rte_mempool*)(void*)(uintptr_t)(cr->msg_cache);
        uint64_t cid = cr->base.src_id;
        // uint64_t cid = cr->base.src_id;
        if(rctx->clients.count(cid) > 0) {
            fprintf(stderr , "重复的 Client ID:%40ld , 忽略\n", cid);
            return;
        }

        ipc_session_t sess = {
            msg_q,
            sq,
            cq,
            msg_cache
        };  
        rctx->clients.insert({cid, sess} );
        fprintf(stdout , "Add new client session :%016lu\n", cid);
        
        fprintf(stdout , "Handshake: msg_q=%p,msg_cache=%p,sq=%p,cq=%p\n", 
            msg_q, msg_cache , sq, cq);
        if(1) {

            char name[32];
            sprintf(name , "cli_%lu_msgq", cid);
            assert(rte_ring_lookup(name) == msg_q);
        }

        do {
            rte_mempool_get(msg_cache, (void**)&cps);
        } while (!cps);
        memcpy(&cps->base , &cr->base , sizeof(msg_base)); 
        cps->base.msg_type = CONNECTION_ESTABLISH_RESPONSE;
        cps->base.header_len = sizeof(*cps);
        std::swap(cps->base.src_id,cps->base.to_id);

        rte_ring_enqueue(msg_q, cps);
    } 
    //挥手
    else if (base->msg_type == CONNECTION_CLOSE_REQUEST) {
        connection_close_request *cr = NULL;
        connection_close_response *cps = NULL;
        
        cr = (connection_close_request *)base;
        uint64_t cid = cr->base.src_id;
        if(rctx->clients.count(cid) == 0) {
            fprintf(stderr, "未知的 Client:%016lu \n" , cid);
            return;
        }
        rte_mempool *msg_cache = rctx->clients[cid].msg_cache;
        rte_ring *msg_q = rctx->clients[cid].msg_q;

        do {
            rte_mempool_get(msg_cache, (void**)&cps);
        } while (!cps);
        memcpy(&cps->base , &cr->base , sizeof(msg_base)); 
        cps->base.msg_type = CONNECTION_CLOSE_RESPONSE;
        cps->base.header_len = sizeof(*cps);
        std::swap(cps->base.src_id,cps->base.to_id);
        rte_ring_enqueue(msg_q, cps);

        //Remove
        fprintf(stdout , "Remove client session :%016lu\n", cid);
        rctx->clients.erase(cid);
    }

    else {
        fprintf(stderr, "暂时不支持操作类型：msg_typeid=%d, msg_type=%s\n" , 
            base->msg_type, msg_type_str(base->msg_type));
        return;
    }
}

struct bg_task {
    union {
        struct {
            uint64_t client_session_ptr;
            uint64_t request_msg_ptr;

            // uint64_t bt_enq_tsc;
            // uint64_t bt_deq_tsc;
            // uint64_t bt_excute_tsc;
            // uint64_t bt_done_tsc;
        };
        char pad[64];
    };
};

static void reactor_inplace_execute_write_request(reactor_t *rctx , struct write_chunk_request *wr ) 
{
    if(FLAGS_fake_exe) {
        uint64_t s ,e ;
        s = rte_rdtsc();

        uint64_t idx = rte_log2_u32(wr->length) -12;
        // uint64_t us_tsc = g_fixed_latency[wr->length >> 12] * ((rte_get_tsc_hz()) / 1e6)
        rte_delay_us( g_fixed_latency[idx] )  ; 
        
        e = rte_rdtsc();
        rctx->metrics.tsc_execute_writes += (e-s);
        return ;
    }


    uint64_t s ,e ;
    s = rte_rdtsc();
    
    char *orig_wbuf = (char*)(void*)(wr->ptr_data);
    // char *copy_wbuf = (char*)reactor_alloc_buffer(rctx,wr->length);
    char copy_wbuf[128u << 10];

    assert(copy_wbuf);
    uint32_t x = spdk_crc32c_update(orig_wbuf, wr->length , 0x0);
    (void)x;
    wr->chunk_id = x;
    rte_memcpy(copy_wbuf, orig_wbuf, wr->length);

    // spdk_delay_us(1000);

    // reactor_free_buffer(rctx, copy_wbuf, wr->length);
    e = rte_rdtsc();
    rctx->metrics.tsc_execute_writes += (e-s);
};
static void reactor_inplace_execute_read_request(reactor_t *rctx , struct read_chunk_request *rr ) 
{
   
   if(FLAGS_fake_exe) {
        uint64_t s ,e ;
        s = rte_rdtsc();

        uint64_t idx = rte_log2_u32(rr->length) -12;
        // uint64_t us_tsc = g_fixed_latency[wr->length >> 12] * ((rte_get_tsc_hz()) / 1e6)
        rte_delay_us( g_fixed_latency[idx] )  ; 
        
        e = rte_rdtsc();
        rctx->metrics.tsc_execute_writes += (e-s);
        return ;
    }
   
    uint64_t s ,e ;
    s = rte_rdtsc();

    char *orig_wbuf = (char*)(void*)(rr->ptr_data);
    // char *copy_wbuf = (char*)reactor_alloc_buffer(rctx, rr->length);
    char copy_wbuf[128u << 10];


    uint32_t x = spdk_crc32c_update(copy_wbuf, rr->length , 0x0);
    (void)x;
    rr->chunk_id = x;

    rte_memcpy(orig_wbuf, copy_wbuf, rr->length);

    // reactor_free_buffer(rctx, copy_wbuf, rr->length);

    e = rte_rdtsc();
    rctx->metrics.tsc_execute_writes += (e-s);
};

static void reactor_inplace_send_reponse(reactor_t *rctx , const ipc_session_t* client , void *request) {
    msg_base *base = (msg_base*)request;
    if(base->msg_type == WRITE_REQUEST) {
        write_chunk_request *wr = (write_chunk_request*)base;
        write_chunk_response*wc =  NULL ;
        if(rte_mempool_get(client->msg_cache, (void**)&wc)) {
            assert(false);
        }
        memcpy(&wc->base,&wr->base,sizeof(*base));
        wc->base.msg_type = WRITE_RESPONSE;
        wc->base.header_len = sizeof(*wc);
        std::swap(wc->base.src_id,wc->base.to_id);
        rte_ring_enqueue(client->cq,wc);
    } else if(base->msg_type == READ_REQUEST) {
        read_chunk_request *rr = (read_chunk_request*)base;
        read_chunk_response*rc =  NULL ;
        if(rte_mempool_get(client->msg_cache, (void**)&rc)) {
            assert(false);
        }
        memcpy(&rc->base,&rr->base,sizeof(*base));
        rc->base.msg_type = READ_RESPONSE;
        rc->base.header_len = sizeof(*rc);
        std::swap(rc->base.src_id,rc->base.to_id);
        rte_ring_enqueue(client->cq,rc);
    } else {

    }
    return ;

}
static void reactor_inplace_execute_request(reactor_t *rctx , void *msg) {
    msg_base *base = (msg_base*)msg;
    if(base->msg_type == WRITE_REQUEST) {
        write_chunk_request *wr = (write_chunk_request*)base;
        reactor_inplace_execute_write_request(rctx,wr);
    } else if(base->msg_type == READ_REQUEST) {
        read_chunk_request *rr = (read_chunk_request*)base;
        reactor_inplace_execute_read_request(rctx,rr);
    } else {

    }
    return ;
}

static void reactor_inplace_io_request_handler(reactor_t *rctx , const ipc_session_t* client , void *msg) {
    msg_base *base = (msg_base*)msg;
    if(base->msg_type == WRITE_REQUEST || base->msg_type == READ_REQUEST ) {
        reactor_inplace_execute_request(rctx, msg);
        reactor_inplace_send_reponse(rctx,client,msg);
    }
    else {
        fprintf(stderr, "暂时不支持操作类型：msg_typeid=%d, msg_type=%s\n" , 
            base->msg_type, msg_type_str(base->msg_type));
        return;
    }

}

static int reactor_poll_bg_task_done_queue(reactor_t *rctx) {
    //1024 is a very large 
    unsigned int n = 0;
    int total = 0; 
    do {
        void *bt[MAX_RPC_BURST];
        n = rte_ring_dequeue_burst(rctx->bg_task_done_queue, bt , MAX_RPC_BURST , NULL);
        for(unsigned int i = 0 ; i < n ; ++i) {
            bg_task* b = (bg_task*)bt[i];
            reactor_inplace_send_reponse(rctx, (const ipc_session_t*)(b->client_session_ptr) , (void*)(b->request_msg_ptr));
        }
        if(n) {
            rte_mempool_put_bulk(rctx->bg_task_cache, bt, n);
        }
        total += n;
    } while(n);

   return total;
}


static int reactor_poll_others_bg_task_queue(reactor_t *rctx, uint32_t neighbor) {

    rte_ring *tq = bg_task_queue_lookup(neighbor);    
    // rte_ring *tq = g_bg_task_queue;    
    void* task = NULL;

    uint64_t s, e ; 
    s = rte_rdtsc();
    int rc = rte_ring_dequeue(tq, &task);
    if(rc) {
        e = rte_rdtsc();
        rctx->metrics.tsc_others_bt_deq_raw += (e-s);
        return 0;
    }
    // fprintf(stdout, "Steal successfully\n");
    e = rte_rdtsc();
    rctx->metrics.tsc_others_bt_deq += (e-s);
    rctx->metrics.nr_others_requests++;
    
    reactor_inplace_execute_request(rctx , (void*)((bg_task*)task)->request_msg_ptr);
    
    
    msg_base *base = (msg_base*)(void*)((bg_task*)task)->request_msg_ptr;
    rte_ring *tdq = bg_task_done_queue_lookup(base->to_id);
    // assert()
    
    rc = rte_ring_enqueue(tdq, task);
    assert(rc == 0);
    return 1;
};

static int reactor_poll_bg_task_queue(reactor_t *rctx) {
    //Poll myself
    int my_tasks = 0;

    // void *task_dones

    while(1) {
        void *bt = NULL;
        uint32_t remaining = 0;
        uint32_t rc = 0;

        if(1) {
            uint64_t  s , e1, e2;
            s = rte_rdtsc();

            rc = rte_ring_dequeue_burst(rctx->bg_task_queue , &bt , 1 , &remaining);
            
            e2 = rte_rdtsc();
            if(rc) {
                e1 = rte_rdtsc();
                rctx->metrics.tsc_bt_deq += (e1 - s);
            }
            rctx->metrics.tsc_bt_deq_raw += (e2 - s);
        }
                
        if(rc) {
            my_tasks++;
            rctx->metrics.nr_self_requests++;
            bg_task *b = (bg_task*)bt;
            reactor_inplace_execute_request(rctx , (void*)(b->request_msg_ptr));
            // rte_ring_enqueue(rctx->bg_task_done_queue , b);
            reactor_inplace_send_reponse(rctx, (const ipc_session_t*)(void*)(b->client_session_ptr), (void*)(b->request_msg_ptr));
            rte_mempool_put(rctx->bg_task_cache, b);
        }
        if(!remaining)
            break;
    }

    return my_tasks;
    // //Poll others
    // for(unsigned i = spdk_env_get_first_core() ; i != UINT32_MAX ; i = spdk_env_get_next_core(i) ) {
    //     if( i != rctx->cpu ) {

    //     }
    // }
};

static uint32_t  _reactor_poll_clients_requests(reactor_t *rctx , const ipc_session_t**from, void**msgs, uint32_t burst) {
    const auto& cmap = rctx->clients;
    auto iter = cmap.find(rctx->poll_current_client);
    if(iter != cmap.end()) {
        iter++;
    }
    uint32_t pn = 0;
    uint32_t pc = 0;
    while( pc < cmap.size() ) {
        if(iter == cmap.end()) {
            iter = cmap.begin();
        }
        while(iter != cmap.end()) {
            rte_ring *sq = iter->second.sq;
            uint32_t n = rte_ring_dequeue_burst(sq, msgs + pn , burst - pn , NULL);
            if(n) {
                // fprintf(stdout, "Poll requests=%u\n" , n);
                const ipc_session_t *s = &iter->second;
                for(unsigned int i = pn ; i < pn + n ; ++i) {
                    from[i] = s;
                }
                pn += n;
                if( pn == burst) {
                    // fprintf(stdout, "burst=%u\n" , n);
                    rctx->poll_current_client = iter->first;
                    return pn;
                }
            }
            pc++;
            iter++;
        }
    }
    return pn;
}

static int  reactor_poll_clients_requests(reactor_t *rctx) {
    void* msgs[MAX_RPC_BURST];
    const ipc_session_t *from[MAX_RPC_BURST];
    uint32_t msg_nr = _reactor_poll_clients_requests(rctx , from, msgs , MAX_RPC_BURST);
    if(msg_nr) {

        if(FLAGS_force_fifo) {
            for(unsigned int i = 0 ; i < msg_nr ; ++i) {
                reactor_inplace_io_request_handler(rctx , from[i] , msgs[i]);
            }
            return msg_nr;
        }

        // fprintf(stdout, "Handle requests=%u\n" , msg_nr);
        //
        rctx->metrics.nr_self_requests += msg_nr;
        //<session*,msg*>
        std::pair<const ipc_session_t* , void*> bg_msgs[MAX_RPC_BURST];
        unsigned int nr_bg_task = 0;
        std::pair<const ipc_session_t* , void*> inplace_msgs[MAX_RPC_BURST];
        unsigned int nr_inplace_task = 0;
        uint32_t max_inplace_size = FLAGS_max_inplace_size;
        for(unsigned int i = 0 ; i < msg_nr ; ++i) {
            //TODO
            write_chunk_request* wr = (write_chunk_request*)msgs[i]; 
            if(wr->length <= max_inplace_size) {
                inplace_msgs[nr_inplace_task++] = {from[i] , msgs[i]};
            } else {
                bg_msgs[nr_bg_task++] = {from[i] , msgs[i]};
            }
        }

        //BG task enqueue
        if(!FLAGS_force_inplace && nr_bg_task) {
            void *bg_tasks[MAX_RPC_BURST];
            int rc = rte_mempool_get_bulk(rctx->bg_task_cache, bg_tasks, nr_bg_task);
            assert(rc == 0);
            
            for(unsigned int i = 0  ; i < nr_bg_task ; ++i) {
                bg_task *b = (bg_task *)bg_tasks[i];
                b->client_session_ptr = (uint64_t)bg_msgs[i].first;
                b->request_msg_ptr = (uint64_t)bg_msgs[i].second;
            }
            unsigned int en = rte_ring_enqueue_burst(rctx->bg_task_queue, bg_tasks, nr_bg_task , NULL);
            assert(en == nr_bg_task);
        }

        if(nr_inplace_task) {
            for(unsigned int i = 0  ; i < nr_inplace_task ; ++i) {
                reactor_inplace_io_request_handler(rctx, inplace_msgs[i].first , inplace_msgs[i].second);    
            }
        }
        if(FLAGS_force_inplace) {
            for(unsigned int i = 0  ; i < nr_bg_task ; ++i) {
                reactor_inplace_io_request_handler(rctx, bg_msgs[i].first , bg_msgs[i].second);    
            }
        }

    }
    return msg_nr;
}

static int reactor_poll_ctrl_msgs(reactor_t *rctx) {
    void *msgs[32];
    unsigned int msg_nr = 0;

    msg_nr = rte_ring_dequeue_burst(rctx->msg_queue , msgs , 32 ,NULL);
    
    if(msg_nr) {
        // fprintf(stdout, "Poll msgs=%u\n" , msg_nr);
        for(unsigned int i = 0 ; i < msg_nr ; ++i) {
            reactor_ctrl_msg_handler(rctx , msgs[i]);
        }
    }

    return msg_nr;
}

static void* reactor_main_loop(void* ctx) {


    std::default_random_engine e; 

    reactor_t *rctx= (reactor_t *)ctx;
    int rc = 0;

    const auto& it = std::find(rctx->neighbors.begin() , rctx->neighbors.end(), rctx->cpu);
    rctx->neighbors.erase(it);

    if(1) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(rctx->cpu , &cpus);
        // fprintf(stdout, "Bind bs_reactor_%d to cpu%d\n", rctx->cpu , rctx->cpu);
        assert(rctx->me == pthread_self());
        rc = pthread_setaffinity_np(rctx->me, sizeof(cpus),&cpus);
        if(rc) {
            fprintf(stderr, "pthread_setaffinity_np falied ,errno=%d \n" , errno);
            return NULL;
        }

        sched_param sp;
        sp.sched_priority = 20;
        rc = pthread_setschedparam(rctx->me , SCHED_RR , &sp);
        if(rc) {
            fprintf(stderr, "pthread_setschedparam falied ,errno=%d \n" , errno);
            return NULL;   
        }
    }
    
    while(rctx->active.load() == REACTOR_READY)
            ;
    fprintf(stdout , "block server reactor_%d running. \n" , rctx->cpu);
    

    rctx->metrics.tsc_run = rte_rdtsc_precise();

    while(rctx->active.load() == REACTOR_RUNNING) {

        reactor_poll_ctrl_msgs(rctx);

        int rc = reactor_poll_clients_requests(rctx);
        if(rc > 0) {
            rctx->metrics.nr_rpc_poll_succ++;
            rctx->metrics.nr_rpc_msg_recv += rc;
        }
        if(FLAGS_max_inplace_size != -1 && !FLAGS_force_inplace) {
            rc = reactor_poll_bg_task_queue(rctx);
            if(!rc) {
                if(1) {
                    // 选择一个邻居                    
                    unsigned int x = e() % rctx->neighbors.size();
                    (void)x;
                    unsigned int ni = rctx->neighbors[x];
                    reactor_poll_others_bg_task_queue(rctx, ni);
                }
            }
            reactor_poll_bg_task_done_queue(rctx);
        }
    }


    rctx->metrics.tsc_run = rte_rdtsc() - rctx->metrics.tsc_run;

    fprintf(stdout , "block server reactor_%d exit. \n" , rctx->cpu);

    return NULL;
}

static double get_us() {
    return 1e6 *((double) rte_rdtsc_precise() / (double)rte_get_tsc_hz());
} 

struct dry_run_context {
    char *input_mem;
    char* out_mem;
    char *out_crc_arr;
    dry_run_context(uint32_t pack_size) {
        input_mem = (char *)spdk_dma_zmalloc(pack_size, 0x1000 , NULL);
        out_mem = (char *)spdk_dma_zmalloc(pack_size, 0x1000 , NULL);
        out_crc_arr = (char *)spdk_dma_zmalloc( (pack_size / 0x1000) * 4 , 0 , NULL);

        assert((input_mem) && (out_mem) && (out_crc_arr));
    }
    ~dry_run_context() {
        spdk_free(out_crc_arr);
        spdk_free(out_mem);
        spdk_free(input_mem);
    }
};

class user_allocator {
    void *base_;   
    uint64_t alloc_size_;
    uint64_t n_;
    std::stack<void*> allocator_;
public:
    user_allocator(void*base , uint64_t alloc_size , uint64_t n): 
        base_(base), alloc_size_(alloc_size), n_(n) {
            for(uint64_t i = 0 ; i < n ; ++i) {
                allocator_.push( (char*)base_ + (n - i - 1) * alloc_size_);
            }
    }

    ~user_allocator() = default;

    void* alloc() {
        if(unlikely(allocator_.empty())) {
            return NULL;
        }
        void* x = allocator_.top();
        allocator_.pop();
        return x;
    }

    void free(void* m) {
        if(!m) {
            return;
        }
        allocator_.push(m);
        return;
    }

};

static std::atomic<int> g_dry_run_start = {0};

static void* dry_run(void* ctx)  {
    dry_run_context * drctx = (dry_run_context*)ctx;
    // rte_mempool
    std::default_random_engine rde; 

    uint64_t total_length = 128ull << 30;

    double s, e ;
    char*in = drctx->input_mem;
    for(uint64_t i = 0 ; i < (FLAGS_dry_run_bs<<10) ; ++i) {
        memset(&in[i], rde() % 255,1);
    }
    char *out = drctx->out_mem;
    char *out_crc = drctx->out_crc_arr;


    while(g_dry_run_start.load() == 0)
        ;

    if(!strcmp(FLAGS_dry_run_type.c_str(),"crc")) {
        uint64_t loop = total_length / (FLAGS_dry_run_bs<<10) ;
        uint64_t count = loop;
        s = get_us();
        while(loop--) {
            uint64_t nseg = FLAGS_dry_run_bs / 4; 
            for(uint64_t i = 0 ;  i < nseg ; ++i) {
                out_crc[i] = spdk_crc32c_update(in + i * 0x1000 , 0x1000 , 0xffffffff );
            }
            // memcpy(out , in , (FLAGS_dry_run_bs<<10));
        }
        e = get_us();
        double t = e - s ;
        double bw = ((double)(total_length>>30)) / (t/1e6); 
        double lat =(t) / (double)count ;

        printf("Run time=%lf s , Total length = %lu GiB , bw = %lf GiB/s , avg_lat = %.9lf us for %u K\n", 
            t/1e6,
            total_length >> 30, 
            bw , lat , 
            FLAGS_dry_run_bs);
    } else if ((!strcmp(FLAGS_dry_run_type.c_str(),"gmalloc"))) {
        if(1) {
            printf("Testing malloc performance(glibc malloc)\n");
            uint64_t total_length = 128ull << 20; // 32MiB
            uint64_t n = (total_length / (FLAGS_dry_run_bs<<10)) ;
            char *bufs[n];
            uint64_t loop = 1024;
            uint64_t count = loop * n;
            s = get_us();
            while(loop--) {
                for(uint64_t i = 0 ;  i < n ; ++i) {
                    bufs[i] = (char*)malloc( (FLAGS_dry_run_bs<<10));
                    // memset(bufs[i],0,(FLAGS_dry_run_bs<<10));
                }
                for(uint64_t i = 0 ; i < n ; ++i ) {
                    free(bufs[i]);
                }
            }
            e = get_us();
            double t = e - s ;
            double lat = (t) / (double)count ;
            printf("Run time=%lf s , (alloc+free)avg_lat = %.9lf us for %u KiB\n", 
                t/1e6,  lat , 
                FLAGS_dry_run_bs);
            }
    } else if (!strcmp(FLAGS_dry_run_type.c_str(),"user_malloc"))  {
        if(1) {
            printf("Testing malloc performance (use user allocator)\n");
            uint64_t total_length = 128ull << 20; // 32MiB
            uint64_t n = (total_length / (FLAGS_dry_run_bs<<10)) ;
            char *mem = (char*)spdk_dma_zmalloc(total_length , 0x1000 , NULL);
            user_allocator alloc((void*)mem,(FLAGS_dry_run_bs<<10),n);

            char *bufs[n];
            uint64_t loop = 1024;
            uint64_t count = loop * n;
            s = get_us();
            while(loop--) {
                for(uint64_t i = 0 ;  i < n ; ++i) {
                    bufs[i] = (char*)alloc.alloc();
                    // memset(bufs[i],0,(FLAGS_dry_run_bs<<10));
                }
                for(uint64_t i = 0 ; i < n ; ++i ) {
                    alloc.free(bufs[i]);
                }
            }
            e = get_us();
            double t = e - s ;
            double lat =(t) / (double)count ;
            printf("Run time=%lf s , (alloc+free)avg_lat = %.9lf us for %u KiB\n", 
                t/1e6,  lat , 
                FLAGS_dry_run_bs);
            spdk_free(mem);
        }

        
    }
    return NULL;
};

void sigint_handler(int sig) {
    fprintf(stdout , "\n运行终止\n");
    if(g_master) {    
        unsigned i = 0;
        SPDK_ENV_FOREACH_CORE(i) {
            g_reactos[i]->active.store(REACTOR_DIED);           
            int rc = pthread_join(g_reactos[i]->me, NULL);
            assert(rc == 0);
            printf("[bs_reactor_%u]:" , i);
            g_reactos[i]->metrics.dump();
        }
        g_run = 0;
    }
}

int main(int argc , char **argv) {

    signal(SIGINT,sigint_handler);
    signal(SIGTERM,sigint_handler);

    google::ParseCommandLineFlags(&argc , &argv , true);

    spdk_env_opts opts;
    spdk_env_opts_init(&opts);

    opts.shm_id = FLAGS_shmid;
    opts.core_mask = FLAGS_cpumask.c_str();

    int rc = spdk_env_init(&opts);
    if(FLAGS_dry_run) {

        unsigned i = 0;
        std::vector<pthread_t> pt;
        std::vector<dry_run_context*> drt;
        SPDK_ENV_FOREACH_CORE(i) {

        }


        SPDK_ENV_FOREACH_CORE(i) {
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            cpu_set_t c;
            CPU_ZERO(&c);
            CPU_SET(i,&c);
            pthread_attr_setaffinity_np(&attr,sizeof(c),&c);
            pthread_t p;
            dry_run_context* dr = new dry_run_context(FLAGS_dry_run_bs << 10);
            pthread_create(&p , &attr , dry_run , dr);
            pt.push_back(p);
            drt.push_back(dr);

        }
        sleep(1);
        g_dry_run_start.store(1);
        for(auto p : pt) {
            pthread_join(p , NULL);
        }

        for(auto dp : drt) {
            delete dp;
        }

        spdk_env_fini();

        return 0 ;

    };

    if(rc) {
        std::cout << "SPDK env init error!\n";
        return 1;
    }



    std::vector<unsigned int> lcores;
    unsigned int i ;
    SPDK_ENV_FOREACH_CORE(i) {
        lcores.push_back(i);
    }
    SPDK_ENV_FOREACH_CORE(i) {
        g_reactos[i] = new reactor_t(i);
        g_reactos[i]->neighbors = lcores;
        g_reactos[i]->max_inplace_size = FLAGS_max_inplace_size;
    }
    SPDK_ENV_FOREACH_CORE(i) {

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        // pthread
        // pthread_attr_setstacksize(&attr , ul << 20);
        // cpu_set_t cpus;
        // CPU_ZERO(&cpus);
        // CPU_SET(i , &cpus);
        // // pthread_attr_setschedpolicy(&attr , SCHED_RR);
        // pthread_attr_setaffinity_np(&attr , sizeof(cpu_set_t), &cpus);
        int rc = pthread_create(&g_reactos[i]->me , &attr , reactor_main_loop , g_reactos[i]);
        
        assert(rc == 0);
        // }
    }


    SPDK_ENV_FOREACH_CORE(i) {
        g_reactos[i]->active.store(RUNNING);
    }
    
    g_run = 1;
    g_master = 1;
    while(g_run)
        sleep(100);

    SPDK_ENV_FOREACH_CORE(i) {
        delete g_reactos[i];
    }


    rte_ring_free(g_bg_task_queue);



    fprintf(stdout , "spdk env fini\n");
    spdk_env_fini();
}