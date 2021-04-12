/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

// #include "spdk/nvme.h"
// #include "spdk/nvme_zns.h"
// #include "spdk/vmd.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#include "spdk/dif.h"
#include "spdk/util.h"



#include "config-host.h"
#include "fio.h"
#include "optgroup.h"


#include <rte_mempool.h>
#include <rte_ring.h>

#include "bs_ipc_msg.h"

#ifdef for_each_rw_ddir
// #define FIO_HAS_ZBD (FIO_IOOPS_VERSION >= 26)
#else
#define FIO_HAS_ZBD (0)
#endif

/* FreeBSD is missing CLOCK_MONOTONIC_RAW,
 * so alternative is provided. */
#ifndef CLOCK_MONOTONIC_RAW /* Defined in glibc bits/time.h */
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#define NVME_IO_ALIGN		4096





struct  bs_target_session {
    uint64_t uniq_id;

    struct rte_ring *msg_q; //Recv Ctrl Response
    struct rte_ring *cq;   //SPSC 
    struct rte_ring *sq;   //SPSC
    struct rte_mempool *msg_cache; 

    uint64_t bs_uniq_id;
};

//Bs
static struct rte_mempool *rte_mempool_create_hepler(char *name , uint64_t n , uint64_t elem_size) {
    return rte_mempool_create(name, n , elem_size , 0 , 0 , NULL , NULL , NULL , NULL ,
            SOCKET_ID_ANY, 0);
}

static  void bs_target_session_init(struct  bs_target_session* bs, uint64_t uniq_id_)  {
    
    bs->uniq_id = uniq_id_;
    char name[32];
    sprintf(name , "cli_%lu_msgq", uniq_id_);
    bs->msg_q =  rte_ring_create(name, 64, SOCKET_ID_ANY, RING_F_SC_DEQ | RING_F_SP_ENQ);
    

    sprintf(name , "cli_%lu_cq", uniq_id_);
    bs->cq =  rte_ring_create(name, 1024, SOCKET_ID_ANY, RING_F_SC_DEQ | RING_F_SP_ENQ);
    
    sprintf(name , "cli_%lu_sq", uniq_id_);
    bs->sq =  rte_ring_create(name, 1024, SOCKET_ID_ANY, RING_F_SC_DEQ | RING_F_SP_ENQ);
    
    sprintf(name , "cli_%lu_msgpool", uniq_id_);
    bs->msg_cache = rte_mempool_create_hepler(name, 1024-1 , 128);
    
    assert(bs->msg_cache && bs->cq  && bs->sq);
	
	fprintf(stdout , "msg_q=%p,msg_cache=%p,sq=%p,cq=%p\n", 
            bs->msg_q, bs->msg_cache , bs->sq, bs->cq);
};

static void bs_target_session_fini(struct  bs_target_session* bs) {
    rte_mempool_free(bs->msg_cache);
    rte_ring_free(bs->cq);
    rte_ring_free(bs->sq);
    rte_ring_free(bs->msg_q);
}

static int bs_target_session_connect(struct  bs_target_session* bs , const char *bs_name) {
    char bs_id[32];
    uint64_t bs_uid = 0;
    strcpy(bs_id , bs_name + 3);
    sscanf(bs_id , "%lu" , &bs_uid);

    fprintf(stdout , "Connecting to bs_%lu ,bs_uid=%lu\n", bs_uid , bs_uid);

    struct rte_ring *msg_q = rte_ring_lookup(bs_name);
    if(!msg_q) {
        fprintf(stdout , "%s Not Found \n", bs_name);
        return -1;
    }

    struct connection_establish_request *cr = NULL;
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

    struct connection_establish_response *cp = NULL;
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

static int bs_target_session_close(struct  bs_target_session* bs) {

    char bs_name[32];
    sprintf(bs_name,"bs_%lu",bs->bs_uniq_id);
    struct rte_ring *msg_q = rte_ring_lookup(bs_name);


    struct connection_close_request *cr = NULL;
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

    struct connection_close_response *cp = NULL;
    if(1) {
        do {
            rte_ring_dequeue(bs->msg_q, (void**)&cp);
        } while(!cp);
        assert(cp->base.msg_type == CONNECTION_CLOSE_RESPONSE);
        assert(cp->base.src_id == bs->bs_uniq_id);
        fprintf(stdout , "Close to bs_%lu Successfully\n" , bs->bs_uniq_id);
    }

    return 0;

}
 
static bool g_spdk_env_initialized;


struct spdk_fio_options {
	// int	shm_id;
};

struct spdk_fio_request {
	union {
		struct write_chunk_request wr;
		struct read_chunk_request  rr;
	};
	struct io_u		*io;
	struct spdk_fio_thread	*fio_thread;
	struct spdk_fio_qpair	*fio_qpair;
};


static int g_td_count;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
// static bool g_error;

static uint64_t g_thread_id_seq = 1;
// static uint64_t g_bs_session_id_seq;

struct spdk_fio_qpair {
	struct fio_file			*f;
	struct bs_target_session *bs;
	TAILQ_ENTRY(spdk_fio_qpair)	link;
	// struct spdk_fio_ctrlr		*fio_ctrlr;
};

struct spdk_fio_thread {
	struct thread_data		*td;
	uint64_t tid;
	TAILQ_HEAD(, spdk_fio_qpair)	fio_qpair;
	struct spdk_fio_qpair		*fio_qpair_current;	/* the current fio_qpair to be handled. */
	
	struct spdk_mempool *fio_req_pool;

	struct io_u			**iocq;		/* io completion queue */
	unsigned int			iocq_count;	/* number of iocq entries filled by last getevents */
	unsigned int			iocq_size;	/* number of iocq entries allocated */
	struct fio_file			*current_f;	/* fio_file given by user */

	uint32_t open_qpairs;
};




/* Called once at initialization. 

	filname="bs_%cpu"
	
*/
static int spdk_fio_setup(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;
	// struct spdk_fio_options *fio_options = td->eo;
	struct spdk_env_opts opts;
	struct fio_file *f;
	// char *p;
	int rc = 0;
	// struct spdk_nvme_transport_id trid;
	// struct spdk_fio_ctrlr *fio_ctrlr;
	// char *trid_info;
	unsigned int i;

	/* we might be running in a daemonized FIO instance where standard
	 * input and output were closed and fds 0, 1, and 2 are reused
	 * for something important by FIO. We can't ensure we won't print
	 * anything (and so will our dependencies, e.g. DPDK), so abort early.
	 * (is_backend is an fio global variable)
	 */
	if (is_backend) {
		char buf[1024];
		snprintf(buf, sizeof(buf),
			 "SPDK FIO plugin won't work with daemonized FIO server.");
		fio_server_text_output(FIO_LOG_ERR, buf, sizeof(buf));
		return -1;
	}

	if (!td->o.use_thread) {
		log_err("spdk: must set thread=1 when using spdk plugin\n");
		return 1;
	}

	pthread_mutex_lock(&g_mutex);

	fio_thread = calloc(1, sizeof(*fio_thread));
	assert(fio_thread != NULL);

	td->io_ops_data = fio_thread;
	fio_thread->td = td;
	fio_thread->tid = g_thread_id_seq++;

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	assert(fio_thread->iocq != NULL);

	TAILQ_INIT(&fio_thread->fio_qpair);

	if (!g_spdk_env_initialized) {
		spdk_env_opts_init(&opts);
		opts.name = "fio";
		opts.shm_id = 1;

		if (spdk_env_init(&opts) < 0) {
			SPDK_ERRLOG("Unable to initialize SPDK env\n");
			free(fio_thread->iocq);
			free(fio_thread);
			fio_thread = NULL;
			pthread_mutex_unlock(&g_mutex);
			return 1;
		}
		g_spdk_env_initialized = true;
		spdk_unaffinitize_thread();

	}
	pthread_mutex_unlock(&g_mutex);
	
	char name[64];
	sprintf(name, "fio_rmp_%lu" , fio_thread->tid );
	fio_thread->fio_req_pool = spdk_mempool_create(name, 512 , 128 , 0 ,  SPDK_ENV_SOCKET_ID_ANY);
	assert(fio_thread->fio_req_pool);
	
	for_each_file(td, f, i) {
		// struct spdk_fio_qpair *qpair = calloc( 1 ,sizeof(struct spdk_fio_qpair));
		// assert(qpair);
		// qpair->f = f;
		// qpair->bs = calloc(1, sizeof(struct bs_target_session));
		// f->engine_data = qpair; 

		// bs_target_session_init(qpair->bs , fio_thread->tid *1000 + i );
		// rc = bs_target_session_connect(qpair->bs , f->file_name);
		// if(rc) {
		// 	return 1;
		// }

		// TAILQ_INSERT_TAIL(&fio_thread->fio_qpair, qpair , link );
	}

	pthread_mutex_lock(&g_mutex);
	g_td_count++;
	pthread_mutex_unlock(&g_mutex);


	return rc;
}


static void spdk_fio_cleanup(struct thread_data *td)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	// struct spdk_fio_qpair	*fio_qpair, *fio_qpair_tmp;
	struct spdk_fio_options *fio_options = td->eo;
	(void)fio_options;

	// TAILQ_FOREACH_SAFE(fio_qpair, &fio_thread->fio_qpair, link, fio_qpair_tmp) {
	// 	TAILQ_REMOVE(&fio_thread->fio_qpair, fio_qpair, link);
	// 	bs_target_session_close(fio_qpair->bs);
	// 	bs_target_session_fini(fio_qpair->bs);
	// 	free(fio_qpair);
	// }


	spdk_mempool_free(fio_thread->fio_req_pool);
	free(fio_thread->iocq);
	free(fio_thread);

	pthread_mutex_lock(&g_mutex);
	g_td_count--;
	if (g_td_count == 0) {
		spdk_env_fini();
	}
	pthread_mutex_unlock(&g_mutex);
}


static int spdk_fio_open(struct thread_data *td, struct fio_file *f)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_qpair *qpair = calloc( 1 ,sizeof(struct spdk_fio_qpair));
	assert(qpair);
	qpair->f = f;
	qpair->bs = calloc(1, sizeof(struct bs_target_session));
	f->engine_data = qpair; 
	bs_target_session_init(qpair->bs , fio_thread->tid *1000 + fio_thread->open_qpairs++);
	int rc = bs_target_session_connect(qpair->bs , f->file_name);
	if(rc) {
		fio_thread->open_qpairs--;
		bs_target_session_fini(qpair->bs);
		free(qpair->bs);
		free(qpair);
		return 1;
	}

	TAILQ_INSERT_TAIL(&fio_thread->fio_qpair, qpair , link );
	return 0;
}

static int spdk_fio_close(struct thread_data *td, struct fio_file *f)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_qpair *qpair = f->engine_data;

	TAILQ_REMOVE(&fio_thread->fio_qpair, qpair , link );

	fio_thread->open_qpairs--;
	bs_target_session_close(qpair->bs);
	bs_target_session_fini(qpair->bs);
	free(qpair->bs);
	free(qpair);

	return 0;
}

static int spdk_fio_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	td->orig_buffer = spdk_dma_zmalloc(total_mem, NVME_IO_ALIGN, NULL);
	return td->orig_buffer == NULL;
}

static void spdk_fio_iomem_free(struct thread_data *td)
{
	spdk_dma_free(td->orig_buffer);
}

static int spdk_fio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_request	*fio_req = spdk_mempool_get(fio_thread->fio_req_pool);
	printf("%s\n", __func__);
	// io_u->engine_data = NULL;
	if (fio_req == NULL) {
		return 1;
	}
	
	fio_req->io = io_u;	
	fio_req->fio_thread = fio_thread;

	io_u->engine_data = fio_req;

	return 0;
}

static void spdk_fio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_request *fio_req = io_u->engine_data;
	// struct spdk_fio_qpair *qpair = io_u->file->engine_data;
	printf("%s\n", __func__);

	if (fio_req) {
		assert(fio_req->io == io_u);
		// free(fio_req);
		spdk_mempool_put(fio_thread->fio_req_pool , fio_req);
		io_u->engine_data = NULL;
	}
}




#if FIO_IOOPS_VERSION >= 24
typedef enum fio_q_status fio_q_status_t;
#else
typedef int fio_q_status_t;
#endif

static void spdk_fio_completion_cb(void *ctx)
{
	struct spdk_fio_request		*fio_req = ctx;
	struct spdk_fio_thread		*fio_thread = fio_req->fio_thread;
	struct spdk_fio_qpair		*fio_qpair = fio_req->fio_qpair;
	// int				rc;
	(void)fio_qpair;

	assert(fio_thread->iocq_count < fio_thread->iocq_size);
	fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;
}

static fio_q_status_t spdk_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	int rc = 1;
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_request	*fio_req = io_u->engine_data;
	struct spdk_fio_qpair	*fio_qpair = NULL;
	(void)fio_thread;

	fio_qpair = io_u->file->engine_data;

	/* Find the namespace that corresponds to the file in the io_u */
	// TAILQ_FOREACH(fio_qpair, &fio_thread->fio_qpair, link) {
	// 	if (fio_qpair->f == io_u->file) {
	// 		break;
	// 	}
	// }

	if (fio_qpair == NULL) {
		return -ENXIO;
	}


	fio_req->fio_qpair = fio_qpair;


	if(1) {
		assert(sizeof(fio_req->rr) == sizeof(fio_req->wr));
		fio_req->rr.ptr_data = (uint64_t)io_u->buf;
		fio_req->rr.chunk_id = 0;
		fio_req->rr.offset = 0;
		fio_req->rr.length = io_u->xfer_buflen;
		if(io_u->ddir == DDIR_READ)
			fio_req->rr.base.msg_type = READ_REQUEST;
		else 
			fio_req->rr.base.msg_type = WRITE_REQUEST;
		fio_req->rr.base.seq = 0;

		fio_req->rr.base.sign = (uint64_t)(fio_req);

		fio_req->rr.base.header_len = sizeof(fio_req->rr);
		fio_req->rr.base.to_id = fio_req->fio_qpair->bs->bs_uniq_id;
		fio_req->rr.base.src_id = fio_req->fio_qpair->bs->uniq_id;
	}

	struct bs_target_session *bs = fio_req->fio_qpair->bs;
	// printf("Enqueue wr=%p , fio_req=%p\n ", &fio_req->rr , fio_req);
	switch (io_u->ddir) {
	case DDIR_READ:
		rc = rte_ring_enqueue(bs->sq , &fio_req->rr);
		break;

	case DDIR_WRITE:
		rc = rte_ring_enqueue(bs->sq , &fio_req->wr);
		break;
	default:
		assert(false);
		break;
	}

	/* NVMe read/write functions return -ENOMEM if there are no free requests. */
	if (rc == -ENOMEM) {
		return FIO_Q_BUSY;
	}

	if (rc != 0) {
		io_u->error = abs(rc);
		return FIO_Q_COMPLETED;
	}

	return FIO_Q_QUEUED;
}

static struct io_u *spdk_fio_event(struct thread_data *td, int event)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	assert(event >= 0);
	assert((unsigned)event < fio_thread->iocq_count);
	return fio_thread->iocq[event];
}

static void spdk_qpair_process_completions(struct spdk_fio_qpair *qpair, uint32_t max) {
	uint32_t n;
	void *rsp[max];
	n = rte_ring_dequeue_burst(qpair->bs->cq, rsp ,max , NULL);
	for(uint32_t i = 0 ; i < n ; ++i) {
		void *ctx = (void*)(((struct msg_base*)rsp[i])->sign);
		spdk_fio_completion_cb(ctx);
		// rte_mempool_put(qpair->bs->msg_cache, rsp[i]);
	}
	// if(n) {
	// 	// printf(" Completion done , n = %u\n" , n);
	// }
	if(n) {
		rte_mempool_put_bulk(qpair->bs->msg_cache , rsp , n);
	}
};

static int spdk_fio_getevents(struct thread_data *td, unsigned int min,
			      unsigned int max, const struct timespec *t)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct spdk_fio_qpair *fio_qpair = NULL;
	struct timespec t0, t1;
	uint64_t timeout = 0;

	if (t) {
		timeout = t->tv_sec * 1000000000L + t->tv_nsec;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	fio_thread->iocq_count = 0;

	/* fetch the next qpair */
	if (fio_thread->fio_qpair_current) {
		fio_qpair = TAILQ_NEXT(fio_thread->fio_qpair_current, link);
	}

	for (;;) {
		if (fio_qpair == NULL) {
			fio_qpair = TAILQ_FIRST(&fio_thread->fio_qpair);
		}

		while (fio_qpair != NULL) {

			spdk_qpair_process_completions(fio_qpair, max - fio_thread->iocq_count);
			// printf(" Completion done , n = %u\n" , fio_thread->iocq_count);

			if (fio_thread->iocq_count >= min) {
				/* reset the currrent handling qpair */
				fio_thread->fio_qpair_current = fio_qpair;
				return fio_thread->iocq_count;
			}

			fio_qpair = TAILQ_NEXT(fio_qpair, link);
		}

		if (t) {
			uint64_t elapse;

			clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			elapse = ((t1.tv_sec - t0.tv_sec) * 1000000000L)
				 + t1.tv_nsec - t0.tv_nsec;
			if (elapse > timeout) {
				break;
			}
		}
	}

	/* reset the currrent handling qpair */
	fio_thread->fio_qpair_current = fio_qpair;
	return fio_thread->iocq_count;
}

static int spdk_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* TODO: This should probably send a flush to the device, but for now just return successful. */
	return 0;
}




/* This function enables addition of SPDK parameters to the fio config
 * Adding new parameters by defining them here and defining a callback
 * function to read the parameter value. */
static struct fio_option options[] = {
	// {
	// 	.name		= "shm_id",
	// 	.lname		= "shared memory ID",
	// 	.type		= FIO_OPT_INT,
	// 	.off1		= offsetof(struct spdk_fio_options, shm_id),
	// 	.def		= "1",
	// 	.help		= "Shared Memory ID",
	// 	.category	= FIO_OPT_C_ENGINE,
	// 	.group		= FIO_OPT_G_INVALID,
	// },
	{
		.name		= NULL,
	},
};

/* FIO imports this structure using dlsym */
struct ioengine_ops ioengine = {
	.name			= "bs_sim",
	.version		= FIO_IOOPS_VERSION,
	.queue			= spdk_fio_queue,
	.getevents		= spdk_fio_getevents,
	.event			= spdk_fio_event,
	.cleanup		= spdk_fio_cleanup,
	.open_file		= spdk_fio_open,
	.close_file		= spdk_fio_close,
	.invalidate		= spdk_fio_invalidate,
	.iomem_alloc	= spdk_fio_iomem_alloc,
	.iomem_free		= spdk_fio_iomem_free,
	.setup			= spdk_fio_setup,
	.io_u_init		= spdk_fio_io_u_init,
	.io_u_free		= spdk_fio_io_u_free,
	.flags			= FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN,
	// .options		= options,
	// .option_struct_size	= sizeof(struct spdk_fio_options),
};

static void fio_init fio_spdk_register(void)
{
	(void)options;
	register_ioengine(&ioengine);
}

static void fio_exit fio_spdk_unregister(void)
{
	unregister_ioengine(&ioengine);
}
