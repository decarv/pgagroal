/*
 * Copyright (C) 2024 The pgagroal community
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgagroal */
#include <ev.h>

/* system */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#if USE_URING
#include <liburing.h>
#include <netdb.h>
#include <sys/eventfd.h>
#else /* USE_URING */
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#endif

#if USE_URING

/*
 * The following code was inspired by: https://git.kernel.dk/cgit/liburing/tree/examples/proxy.c
 * (C) 2024 Jens Axboe <axboe@kernel.dk>
 */

int
pgagroal_ev_setup(struct ev_context* ctx, struct ev_setup_opts opts)
{
   /* configuration invariant asserts */
   struct user_data ud;
   size_t ud_ind_max_value = sizeof(ud.ind) * 8;
   if (MAX_SIGNALS > ud_ind_max_value || EV_MAX_FDS > ud_ind_max_value || MAX_PERIODIC > ud_ind_max_value)
   {
      fprintf(stderr, "ev_setup: Bad configuration for MAX_SIGNALS, MAX_FDS or MAX_PERIODIC\n");
      exit(EXIT_FAILURE);
   }

   /* set opts */

   ctx->napi = opts.napi;
   ctx->sqpoll = opts.sqpoll;
   ctx->use_huge = opts.use_huge;
   ctx->defer_tw = opts.defer_tw;
   ctx->snd_ring = opts.snd_ring;
   ctx->snd_bundle = opts.snd_bundle;
   ctx->fixed_files = opts.fixed_files;

   ctx->buf_count = opts.buf_count;

   if (ctx->defer_tw && ctx->sqpoll)
   {
      fprintf(stderr, "Cannot use DEFER_TW and SQPOLL at the same time\n");
      exit(1);
   }

   /* setup params TODO: pull from opts */

   ctx->entries = (1 << 10);
   ctx->params.cq_entries = (1 << 10);
   ctx->params.flags = 0;
   ctx->params.flags |= IORING_SETUP_SINGLE_ISSUER; /* TODO: makes sense for pgagroal? */
   ctx->params.flags |= IORING_SETUP_CLAMP;
   ctx->params.flags |= IORING_SETUP_CQSIZE;
   ctx->params.flags |= IORING_SETUP_DEFER_TASKRUN;

   /* default optsuration */

   if (ctx->defer_tw)
   {
      ctx->params.flags |= IORING_SETUP_DEFER_TASKRUN; /* overwritten by SQPOLL */
   }
   if (ctx->sqpoll)
   {
      ctx->params.flags |= IORING_SETUP_SQPOLL;
      ctx->params.sq_thread_idle = opts.sq_thread_idle;
   }
   if (!ctx->sqpoll && !ctx->defer_tw)
   {
      ctx->params.flags |= IORING_SETUP_COOP_TASKRUN;
   }
   if (!ctx->buf_count)
   {
      ctx->buf_count = BUFFER_COUNT;
   }
   if (!ctx->buf_size)
   {
      ctx->buf_size = BUFFER_SIZE;
   }
   ctx->br_mask = (ctx->buf_count - 1);

   if (ctx->fixed_files)
   {
      fprintf(stderr, "io_context_setup: no support for fixed files\n"); /* TODO */
      exit(1);
   }

   return EV_OK;
}

int
pgagroal_ev_init(struct ev_watcher** ev_out, struct ev_setup_opts opts)
{
   int ret = EV_OK;
   struct ev_watcher* ev;
   struct ev_context ctx;

   *ev_out = calloc(1, sizeof(struct ev_watcher));
   if (!*ev_out)
   {
      fprintf(stderr, "ev_init: calloc\n");
      return EV_ERROR;
   }

   ev = *ev_out;

   ret = pgagroal_ev_setup(&ev->ctx, opts);
   if (ret)
   {
      fprintf(stderr, "ev_init: ev_setup\n");
      return EV_ERROR;
   }

   ctx = ev->ctx;

   ret = io_uring_queue_init_params(ctx.entries, &ev->ring, &ctx.params);
   if (ret)
   {
      fprintf(stderr, "ev_init: io_uring_queue_init_params: %s\n", strerror(-ret));
      fprintf(stderr, "make sure to setup context with io_context_setup\n");
      return EV_ERROR;
   }

   ret = pgagroal_ev_setup_buffers(ev);
   if (ret)
   {
      fprintf(stderr, "ev_init: ev_setup_buffers");
      return EV_ERROR;
   }

   for (int i = 0; i < EV_MAX_FDS; i++)
   {
      ev->io_table[i].fd = EMPTY;
   }

   sigemptyset(&ev->sigset);

   return EV_OK;
}

int
pgagroal_ev_free(struct ev_watcher** ev_out)
{
   struct ev_watcher* ev = *ev_out;

   /* free buffer rings */
   io_uring_free_buf_ring(&ev->ring, ev->in_br.br, ev->ctx.buf_count, ev->in_br.bgid);
   ev->in_br.br = NULL;
   io_uring_free_buf_ring(&ev->ring, ev->out_br.br, ev->ctx.buf_count, ev->out_br.bgid);
   ev->out_br.br = NULL;
   if (ev->ctx.use_huge)
   {
      /* TODO munmap(cbr->buf, buf_size * nr_bufs); */
   }
   else
   {
      free(ev->in_br.buf);
      free(ev->out_br.buf);
   }

   free(ev);

   *ev_out = NULL;

   return EV_OK;
}

int
pgagroal_ev_start(struct ev_watcher* ev)
{
   int ret;
   int sig;
   int events;
   int to_wait = 1; /* wait for any 1 */
   unsigned int head;
   struct io_uring_cqe* cqe;
   struct __kernel_timespec* ts;
   struct __kernel_timespec idle_ts = {
      .tv_sec = 0,
      .tv_nsec = 100000000LL
   };
   struct timespec timeout = {
      .tv_sec = 0,
      .tv_nsec = 0
   };

   ev->running = true; /* safe to initialize */
   while (atomic_load(&ev->running))
   {
      ts = &idle_ts;
      io_uring_submit_and_wait_timeout(&ev->ring, &cqe, to_wait, ts, NULL);

      /* Good idea to leave here to see what happens */
      if (*ev->ring.cq.koverflow)
      {
         printf("overflow %u\n", *ev->ring.cq.koverflow);
         exit(1);
      }

      if (*ev->ring.sq.kflags & IORING_SQ_CQ_OVERFLOW)
      {
         printf("saw overflow\n");
         exit(1);
      }


      /* Check for signals before iterating over cqes */
      sig = sigtimedwait(&ev->sigset, NULL, &timeout);
      if (sig > 0)
      {
         ret = signal_handler(ev, sig, sig);
         if (ret)
         {
            fprintf(stderr, "Signal handler not found\n");
            return EV_ERROR;
         }
      }
      if (!atomic_load(&ev->running))
      {
              break;
      }

      events = 0;
      io_uring_for_each_cqe(&(ev->ring), head, cqe)
      {
         if (ev_handler(ev, cqe))
         {
            fprintf(stderr, "ev_loop: io_handle_event\n");
            return 1;
         }
         events++;
      }
      if (events)
      {
         io_uring_cq_advance(&ev->ring, events);  /* batch marking as seen */
      }

      /* TODO: housekeeping ? */

      
   }

   return EV_OK;
}

int
pgagroal_ev_break(struct ev_watcher* ev)
{
   atomic_store(&ev->running, false);
   return EV_OK;
}

int
pgagroal_ev_setup_buffers(struct ev_watcher* ev)
{
   int ret = EV_OK;
   void* ptr;
   struct ev_context ctx = ev->ctx;

   struct io_buf_ring* in_br = &ev->in_br;
   struct io_buf_ring* out_br = &ev->out_br;

   if (ctx.use_huge)
   {
      fprintf(stderr, "ev_setup_buffers: use_huge not implemented yet\n"); /* TODO */
   }
   if (posix_memalign(&in_br->buf, ALIGNMENT, ctx.buf_count * ctx.buf_size))
   {
      perror("ev_setup_buffers: posix_memalign");
      return 1;
   }
   if (posix_memalign(&out_br->buf, ALIGNMENT, ctx.buf_count * ctx.buf_size))
   {
      perror("ev_setup_buffers: posix_memalign");
      return 1;
   }

   in_br->br = io_uring_setup_buf_ring(&ev->ring, ctx.buf_count, 0, 0, &ret);
   out_br->br = io_uring_setup_buf_ring(&ev->ring, ctx.buf_count, 1, 0, &ret);
   if (!in_br->br || !out_br->br)
   {
      fprintf(stderr, "Buffer ring register failed %d\n", ret);
      return 1;
   }

   ptr = in_br->buf;
   for (int i = 0; i < ctx.buf_count; i++)
   {
      io_uring_buf_ring_add(in_br->br, ptr, ctx.buf_size, i, ctx.br_mask, i);
      ptr += ctx.buf_size;
   }
   io_uring_buf_ring_advance(in_br->br, ctx.buf_count);

   ptr = out_br->buf;
   for (int i = 0; i < ctx.buf_count; i++)
   {
      io_uring_buf_ring_add(out_br->br, ptr, ctx.buf_size, i, ctx.br_mask, i);
      ptr += ctx.buf_size;
   }
   io_uring_buf_ring_advance(out_br->br, ctx.buf_count);

   ev->next_out_bid = 0;
   return ret;
}

int
ev_cleanup(struct ev_watcher* ev)
{
   if (ev)
   {
      io_uring_queue_exit(&ev->ring);
      if (ev->in_br.buf)
      {
         free(ev->in_br.buf);
      }
      free(ev);
   }
   return EV_OK;
}

int
ev_handler(struct ev_watcher* ev, struct io_uring_cqe* cqe)
{
   struct user_data ud;

   ud = decode_user_data(cqe);

   if (ud.event < 0 || ud.event >= EVENTS_NR)
   {
      fprintf(stderr, "handle_event: event \n");
      return 1;
   }

   if (ud.event == PERIODIC)
   {
      return pgagroal_periodic_handler(ev, ud.ind);
   }
   else if (ud.event == SIGNAL)
   {
      return signal_handler(ev,ud.ind,-1); /* unused, currently not handled here */
   }

   /* I/O event */
   return io_handler(ev,cqe);
}

/**
 * I/O Events
 */

/**
 * @param buf_len: either the length of the buffer or the bid.
 */
int
pgagroal_io_init(struct ev_watcher* io,int fd,int event,io_cb callback,void* buf,size_t buf_len,int bid)
{
   int t_index = -1;
   int domain;
   struct ev_context ctx = io->ctx;
   struct io_uring_sqe* sqe = get_sqe(io);
   /* struct io_entry* entry = NULL; */
   union sockaddr_u* addr;

   if (event >= IO_EVENTS_NR)
   {
      fprintf(stderr,"io_init: invalid event flag number: %d\n",event);
      return 1;
   }

   t_index = pgagroal_io_table_insert(io,fd,callback,event);
   if (t_index < 0)
   {
      fprintf(stderr,"io_init: io_table_insert\n");
      return 1;
   }

   switch (event)
   {

      case ACCEPT:
         encode_user_data(sqe,ACCEPT,io->id,io->bid,fd,t_index);
         io_uring_prep_multishot_accept(sqe,fd,NULL,NULL,0);
         break;

      case RECEIVE:
         io_uring_prep_recv_multishot(sqe,fd,NULL,0,0);
         encode_user_data(sqe,RECEIVE,io->id,0,fd,t_index);
         sqe->flags |= IOSQE_BUFFER_SELECT;
         sqe->buf_group = 0;
         break;

      case SEND:
         io_uring_prep_send(sqe,fd,buf,buf_len,MSG_WAITALL | MSG_NOSIGNAL); /* TODO: why these flags? */
         encode_user_data(sqe,SEND,io->id,bid,fd,t_index);
         break;

      case CONNECT:
         addr = (union sockaddr_u*)buf;
         if (ctx.ipv6)
         {
            io_uring_prep_connect(sqe,fd,(struct sockaddr*) &addr->addr6,sizeof(struct sockaddr_in6));
         }
         else
         {
            io_uring_prep_connect(sqe,fd,(struct sockaddr*) &addr->addr4,sizeof(struct sockaddr_in));
         }
         encode_user_data(sqe,CONNECT,io->id,0,fd,t_index);
         break;

      case SOCKET:
         if (ctx.ipv6)
         {
            domain = AF_INET6;
         }
         else
         {
            domain = AF_INET;
         }
         io_uring_prep_socket(sqe,domain,SOCK_STREAM,0,0); /* TODO: WHAT CAN BE USED HERE ? */
         encode_user_data(sqe,SOCKET,io->id,bid,0,t_index);
         break;

      case READ: /* unused */
         io_uring_prep_read(sqe,fd,buf,buf_len,0);
         encode_user_data(sqe,SIGNAL,io->id,bid,fd,t_index);
         break;

      default:
         fprintf(stderr,"io_init: unknown event type: %d\n",event);
         return 1;
   }

   return EV_OK;
}

int
pgagroal_io_stop(struct ev_watcher* ev,int fd)
{
   for (int i = 0; i < MAX_IO; i++)
   {
      if (ev->io_table[i].fd == fd)
      {
         ev->io_table[i].fd = EMPTY;
         break;
      }
   }
   struct io_uring_sqe* sqe = io_uring_get_sqe(&ev->ring);
   io_uring_prep_cancel_fd(sqe,fd,0);
   return 0;
}

int
pgagroal_io_accept_init(struct ev_watcher* ev,int fd,io_cb cb)
{
   return pgagroal_io_init(ev,fd,ACCEPT,cb,NULL,0,-1);
}

int
pgagroal_io_read_init(struct ev_watcher* ev,int fd,io_cb cb)
{
   return pgagroal_io_init(ev,fd,READ,cb,NULL,0,-1);
}

int
pgagroal_io_send_init(struct ev_watcher* ev,int fd,io_cb cb,void* buf,int buf_len,int bid)
{
   return pgagroal_io_init(ev,fd,SEND,cb,buf,buf_len,bid);
}

int
pgagroal_io_receive_init(struct ev_watcher* ev,int fd,io_cb cb)
{
   return pgagroal_io_init(ev,fd,RECEIVE,cb,NULL,0,-1);
}

int
pgagroal_io_connect_init(struct ev_watcher* ev,int fd,io_cb cb,union sockaddr_u* addr)
{
   return pgagroal_io_init(ev,fd,CONNECT,cb,(void*)addr,0,-1);
}

int
pgagroal_io_table_insert(struct ev_watcher* ev,int fd,io_cb cb,int event)
{
   int i;
   const int io_table_size = sizeof(ev->io_table) / sizeof(struct io_entry);

   /* if fd is already registered, add cb to fd entry */
   for (i = 0; i < io_table_size; i++)
   {
      if (ev->io_table[i].fd == EMPTY)
      {
         break; /* stop looking once reach unregistered entries */
      }
      if (ev->io_table[i].fd == fd)
      {
         ev->io_table[i].fd = fd;
         ev->io_table[i].cbs[event] = cb;

         return i;
      }
   }

   if (ev->io_count >= io_table_size)
   {
      fprintf(stderr,"periodic_table_insert: ev->periodic_count >= periodic_table_size\n");
      return -1;
   }

   i = ev->io_count++;

   ev->io_table[i].fd = fd;
   ev->io_table[i].cbs[event] = cb;

   return i;
}

// DO NOT USE -> use pgagroal_io_stop
// int
// pgagroal_io_table_remove(struct ev* ev,int fd)
// {
//    for (int i = 0; i < EV_MAX_FDS; i++)
//    {
//            if (i < MAX_IO && ev->io_table[i].fd == fd)
//            {
//                 ev->io_table[i].fd = EMPTY;
//                 break;
//            }
//            
//    }
// 
//    return EV_OK;
// }

int
pgagroal_signal_cancel(struct ev_watcher* ev, int signum)
{
        return EV_OK;
}

int pgagroal_periodic_cancel(struct ev_watcher* ev, int periodic)
{
        return EV_OK;
}

int
io_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe)
{
   int ret = EV_OK;
   struct user_data ud = decode_user_data(cqe);
   int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
   int bid_end = bid;
   void* buf;
   int ti = ud.ind;

   switch (ud.event)
   {
      case ACCEPT:
         return accept_handler(ev,cqe);
      case SEND:
         return send_handler(ev,cqe);
      case CONNECT:
         return connect_handler(ev,cqe);
      case RECEIVE:
      {
         ret = receive_handler(ev,cqe,&buf,&bid_end,false);
         switch (ret)
         {
            case _CLOSE_FD: /* connection closed */
               close(ud.fd);
               ev->io_table[ti].fd = -1; /* remove io_table entry */
               ret = EV_OK;
               break;
            case _REPLENISH_BUFFERS:
               printf("DEBUG - ev_handler - need to replenish buffers, requeue request\n");
               /* TODO: Stress test this... */
               pgagroal_io_receive_init(ev,ud.fd,(io_cb) ev->io_table[ti].cbs[RECEIVE]);
               ret = EV_OK;
               break;
         }
      }
   }

   return ret;
}

int
replenish_buffers(struct ev_watcher* ev,struct io_buf_ring* br,int bid_start,int bid_end)
{
   int count;
   struct ev_context ctx = ev->ctx;

   if (bid_end >= bid_start)
   {
      count = (bid_end - bid_start);
   }
   else
   {
      count = (bid_end + ctx.buf_count - bid_start);
   }

   for (int i = bid_start; i != bid_end; i = (i + 1) & (ctx.buf_count - 1))
   {
      io_uring_buf_ring_add(br->br,(void*)br->br->bufs[i].addr,ctx.buf_size,i,ctx.br_mask,0);
   }

   io_uring_buf_ring_advance(br->br,count);

   return 0;
}

/*
 * Signal Events
 */

int
pgagroal_signal_init(struct ev_watcher* io,int signum,signal_cb cb)
{
   int ret;
   int t_ind;

   /* register signal */
   t_ind = pgagroal_signal_table_insert(io,signum,cb);
   if (t_ind < 0)  /* TODO: t_ind serves no purpose in current implementation */
   {
      fprintf(stderr,"signal_init: signal_table_insert\n");
      return 1;
   }

   /* prepare signal */
   sigaddset(&io->sigset,signum);

   ret = sigprocmask(SIG_BLOCK,&io->sigset,NULL);
   if (ret == -1)
   {
      fprintf(stdout,"sigprocmask\n");
      return 1;
   }

   return EV_OK;
}

int
pgagroal_signal_table_insert(struct ev_watcher* ev,int signum,signal_cb cb)
{
   int i;

   switch (signum)
   {
      case SIGTERM:
         ev->sig_table[_SIGTERM].cb = cb;
         ev->sig_table[_SIGTERM].signum = signum;
         break;
      case SIGHUP:
         ev->sig_table[_SIGHUP].cb = cb;
         ev->sig_table[_SIGHUP].signum = signum;
         break;
      case SIGINT:
         ev->sig_table[_SIGINT].cb = cb;
         ev->sig_table[_SIGINT].signum = signum;
         break;
      case SIGTRAP:
         ev->sig_table[_SIGTRAP].cb = cb;
         ev->sig_table[_SIGTRAP].signum = signum;
         break;
      case SIGABRT:
         ev->sig_table[_SIGABRT].cb = cb;
         ev->sig_table[_SIGABRT].signum = signum;
         break;
      case SIGALRM:
         ev->sig_table[_SIGALRM].cb = cb;
         ev->sig_table[_SIGALRM].signum = signum;
         break;
      default:
         fprintf(stderr,"signal not supported\n");
         return 1;
   }

   return EV_OK;

   /* TODO: this code is kept unreachable because it is currently not possible to have the following
    *  implementation, based on signalfd
    */

   const int signal_table_size = sizeof(ev->sig_table) / sizeof(struct signal_entry);
   if (ev->signal_count >= signal_table_size)
   {
      fprintf(stderr,"signal_table_insert: ev->signal_count >= signal_table_size\n");
      return -1;
   }

   i = ev->signal_count++;
   ev->sig_table[i].signum = signum;
   ev->sig_table[i].cb = cb;

   return i;
}

int
signal_handler(struct ev_watcher* ev,int t_index,int signum)
{
   if (signum >= 0) /* currently signum is used here as a workaround, ideally there should be no signum */
   {
      switch (signum)
      {
         case SIGTERM:
            ev->sig_table[_SIGTERM].cb(ev,0);
            break;
         case SIGHUP:
            ev->sig_table[_SIGHUP].cb(ev,0);
            break;
         case SIGINT:
            ev->sig_table[_SIGINT].cb(ev,0);
            break;
         case SIGTRAP:
            ev->sig_table[_SIGTRAP].cb(ev,0);
            break;
         case SIGABRT:
            ev->sig_table[_SIGABRT].cb(ev,0);
            break;
         case SIGALRM:
            ev->sig_table[_SIGALRM].cb(ev,0);
            break;
         default:
            fprintf(stderr,"signal not supported\n");
            return 1;
      }

      return 0;
   }

   /** TODO: currently this has no solution
    *
    */
   fprintf(stderr,"shouldn't execute");
   exit(EXIT_FAILURE);

   if (t_index < 0 || t_index >= ev->signal_count)
   {
      fprintf(stderr,"signal_handler: (t_index < 0 || t_index >= ev->signal_count). t_index: %d\n",t_index);
      return 1;
   }

   ev->sig_table[t_index].cb(ev->data,0);

   return EV_OK;
}

/**
 * Periodic Events
 */

int
pgagroal_periodic_init(struct ev_watcher* ev,int msec,periodic_cb cb)
{
   /* register */
   struct __kernel_timespec ts = {
      .tv_sec = msec / 1000,
      .tv_nsec = (msec % 1000) * 1000000
   };
   int t_ind = periodic_table_insert(ev,ts,cb);

   /* prepare periodic */
   struct io_uring_sqe* sqe = io_uring_get_sqe(&ev->ring);
   encode_user_data(sqe,PERIODIC,0,0,t_ind,t_ind);
   io_uring_prep_timeout(sqe,&ev->per_table[t_ind].ts,0,IORING_TIMEOUT_MULTISHOT);

   return EV_OK;
}

int
periodic_table_insert(struct ev_watcher* ev,struct __kernel_timespec ts,periodic_cb cb)
{
   int i;
   const int periodic_table_size = sizeof(ev->per_table) / sizeof(struct periodic_entry);

   if (ev->periodic_count >= periodic_table_size)
   {
      fprintf(stderr,"periodic_table_insert: ev->periodic_count >= periodic_table_size\n");
      return 1;
   }

   i = ev->periodic_count++;

   ev->per_table[i].ts.tv_sec = ts.tv_sec;
   ev->per_table[i].ts.tv_nsec = ts.tv_nsec;
   ev->per_table[i].cb = cb;

   return i;
}

int
pgagroal_periodic_handler(struct ev_watcher* ev,int t_index)
{
   if (t_index < 0 || t_index >= ev->periodic_count)
   {
      fprintf(stderr,"periodic_handler: (t_index < 0 || t_index >= ev->periodic_count). t_index: %d\n",t_index);
      return 1;
   }

   return ev->per_table[t_index].cb(ev,0);
}

int
rearm_receive(struct ev_watcher* io,int fd,int t_index)
{
   struct io_uring_sqe* sqe = get_sqe(io);
   io_uring_prep_recv_multishot(sqe,fd,NULL,0,0);
   encode_user_data(sqe,RECEIVE,io->id,0,fd,t_index);
   sqe->flags |= IOSQE_BUFFER_SELECT;
   sqe->buf_group = 0;
   return EV_OK;
}

int
prepare_send(struct ev_watcher* ev,int fd,void* buf,size_t data_len,int t_index)
{
   struct io_uring_sqe* sqe = get_sqe(ev);
   io_uring_prep_send(sqe,fd,buf,data_len,MSG_WAITALL | MSG_NOSIGNAL);
   encode_user_data(sqe,SEND,ev->id,0,fd,t_index);
   return EV_OK;
}


/***********
 * HANDLERS
 ***********/

void
pgagroal_ev_set_out(struct ev_watcher* ev, void *data, int data_len, int fd, int client_fd)
{

        ev->out.data = data;
        ev->out.data_len = data_len;
        ev->out.fd = fd;
        ev->out.client_fd = client_fd;
}

int
accept_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe)
{
   int ret = EV_OK;
   struct user_data ud = decode_user_data(cqe);
   int client_fd = cqe->res;
   int t_index = ud.ind;

   pgagroal_ev_set_out(ev, NULL, 0, ud.fd, client_fd);
   ev->io_table[t_index].cbs[ACCEPT](ev->data,0);

   return ret;
}

int
connect_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe)
{
   int ret = EV_OK;
   struct user_data ud = decode_user_data(cqe);
   io_cb cb = ev->io_table[ud.ind].cbs[CONNECT];

   pgagroal_ev_set_out(ev, NULL, 0, ud.fd, 0);
   cb(ev->data, 0);

   return ret;
}

int
receive_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe,void** send_buf_base,int* bid,bool is_proxy)
{
   int ret = EV_OK;
   struct ev_context ctx = ev->ctx;
   struct user_data ud = decode_user_data(cqe);
   int t_index = ud.ind;
   struct io_buf_ring* in_br = &ev->in_br;
   struct io_buf_ring* out_br = &ev->out_br;
   *send_buf_base = (void*) (in_br->buf + *bid * ctx.buf_size);
   struct io_uring_buf* buf;
   void* data;
   int this_bytes;
   // int pending_recv = 0;
   // int nr_packets = 0;
   int in_bytes;
   int bid_start = *bid;
   int total_in_bytes;

   if (cqe->res == -ENOBUFS)
   {
      fprintf(stderr,"io_receive_handler: Not enough buffers\n");
      return _REPLENISH_BUFFERS;
   }

   if (!(cqe->flags & IORING_CQE_F_BUFFER))
   {
      if (!(cqe->res)) /* Closed connection */
      {
         return _CLOSE_FD;
      }
   }

   total_in_bytes = cqe->res;

   /* If the size of the buffer (this_bytes) is greater than the size of the received bytes, then continue.
    * Otherwise, we iterate over another buffer. */
   in_bytes = cqe->res;
   while (in_bytes)
   {
      buf = &(in_br->br->bufs[*bid]);
      data = (char*) buf->addr;
      this_bytes = buf->len;

      /* Break if the received bytes is smaller than buffer length. 
       * Otherwise, continue iterating over the buffers. */
      if (this_bytes > in_bytes)
      {
         this_bytes = in_bytes;
      }

      io_uring_buf_ring_add(out_br->br,data,this_bytes,*bid,ctx.br_mask,0);
      io_uring_buf_ring_advance(out_br->br,1);

      in_bytes -= this_bytes;

      *bid = (*bid + 1) & (ctx.buf_count - 1);
      // nr_packets++;
   }

   /* From the docs: https://man7.org/linux/man-pages/man3/io_uring_prep_recv_multishot.3.html
    * "If a posted CQE does not have the IORING_CQE_F_MORE flag set then the multishot receive will be
    * done and the application should issue a new request."
    */
   if (!(cqe->flags & IORING_CQE_F_MORE))
   {
      ret = rearm_receive(ev,ud.fd,ud.ind);
      if (ret)
      {
         return 1;
      }
   }

   pgagroal_ev_set_out(ev, (char*)*send_buf_base, ev->io_table[t_index].fd, total_in_bytes);
   ev->io_table[t_index].cbs[RECEIVE](ev->data,ret);

   ret = replenish_buffers(ev,in_br,bid_start,*bid);
   if (ret)
   {
      perror("replenish_buffers");
      return EV_ERROR;
   }

   ev->bid = *bid;

   return 0;
}

int
send_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe)
{
   int ret;
   int buf_len = cqe->res;
   struct ev_context ctx = ev->ctx;
   struct user_data ud = decode_user_data(cqe);
   if (ud.bid < 0)
   {
      return EV_OK;
   }
   int bid_end = (ud.bid + buf_len / ctx.buf_size + (int)(buf_len % ctx.buf_size > 0)) % ctx.buf_count;
   ret = replenish_buffers(ev,&ev->out_br,ud.bid,bid_end);
   if (ret)
   {
      return EV_ERROR;
   }
   return EV_OK;
}

int
socket_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe,void** buf,int* bid)
{
   return EV_OK;
}

/**
 * io_uring utils
 */

void
encode_user_data(struct io_uring_sqe* sqe,uint8_t event,uint16_t id,uint8_t bid,uint16_t fd,uint16_t ind)
{
   struct user_data ud = {
      .event = event,
      .id = id,
      .bid = bid,
      .fd = fd,
      .ind = ind,
   };
   io_uring_sqe_set_data64(sqe,ud.as_u64);
}

struct user_data
decode_user_data(struct io_uring_cqe* cqe)
{
   struct user_data ud = { .as_u64 = cqe->user_data };
   return ud;
}

struct io_uring_sqe*
get_sqe(struct ev_watcher* ev)
{
   struct io_uring* ring = &ev->ring;
   struct io_uring_sqe* sqe;
   do /* necessary if SQPOLL, but I don't think there is an advantage of using SQPOLL */
   {
      sqe = io_uring_get_sqe(ring);
      if (sqe)
      {
         return sqe;
      }
      else
      {
         io_uring_sqring_wait(ring);
      }
   }
   while (1);
}

void
next_bid(struct ev_watcher* ev,int* bid)
{
   struct ev_context ctx = ev->ctx;
   *bid = (*bid + 1) % ctx.buf_count;
}

#else /* use epoll */

int
pgagroal_ev_setup(struct ev_context* ctx,struct ev_setup_opts opts)
{
   printf("epoll ev_setup");
   ctx->flags = 0;
   return 0;
}

int
pgagroal_ev_init(struct ev** ev_out,void* data,struct ev_setup_opts opts)
{
   int ret;
   int fd;
   struct ev* ev = calloc(1,sizeof(struct ev));
   if (!ev)
   {
      fprintf(stderr,"calloc\n");
      return -1;
   }

   pgagroal_ev_setup(&ev->ctx,opts);

   ev->epoll_fd = epoll_create1(ev->ctx.flags);
   if (ev->epoll_fd == -1)
   {
      free(ev);
      fprintf(stderr,"epoll_create1\n");
      return -1;
   }

   /* clean ev_table */
   for (int i = 0; i < MAX_EVENTS; i++)
   {
      ev->ev_table_imap[i] = EMPTY;
      ev->ev_table[i].epoll_ev.data.fd = EMPTY;
   }

   /* signals use sig_table */
   sigemptyset(&ev->sigset);
   fd = signalfd(-1,&ev->sigset,SFD_NONBLOCK);
   if (fd == -1)
   {
      perror("signal_init_epoll: signalfd");
      return ERROR;
   }
   ev->signalfd = fd;
   ev_table_insert(ev,fd,SIGNAL,(event_cb){0},NULL,0);

   ev->data = data;

   *ev_out = ev;

   return 0;
}

int
pgagroal_io_accept_init(struct ev* ev,int fd,io_cb cb)
{
   return pgagroal_io_init(ev,fd,ACCEPT,cb,NULL,0,0);
}

int
pgagroal_io_receive_init(struct ev* ev,int fd,io_cb cb)
{
   return pgagroal_io_init(ev,fd,RECEIVE,cb,NULL,0,0);
}

int
pgagroal_io_send_init(struct ev* ev,int fd,io_cb cb,void* buf,int buf_len,int bid)
{
   return pgagroal_io_init(ev,fd,SEND,cb,buf,buf_len,0);
}

int
pgagroal_io_init(struct ev* io,int fd,int event,io_cb callback,void* buf,size_t buf_len,int bid)
{
   int ret;
   int i;

   ret = set_non_blocking(fd);
   if (ret)
   {
      fprintf(stderr,"set_non_blocking\n");
      return -1;
   }

   ret = ev_table_insert(io,fd,event,(event_cb)callback,buf,buf_len);
   if (ret)
   {
      fprintf(stdout,"io_init: ev_table_insert\n");
      return ERROR;
   }

   return 0;
}

int
pgagroal_ev_loop(struct ev* ev)
{
   int ret;
   struct epoll_event events[MAX_EVENTS];
   ev->running = true;
   while (atomic_load(&ev->running))
   {
      int nfds = epoll_wait(ev->epoll_fd,events,MAX_EVENTS,100);
      if (nfds == -1)
      {
         perror("epoll_wait");
         return ERROR;
      }
      if (atomic_load(&ev->running) == false)
      {
        break;
      }
      for (int i = 0; i < nfds; i++)
      {
         ret = ev_handler(ev,events[i].data.fd);
         if (ret)
         {
            fprintf(stderr,"ev_handler\n");
            return ERROR;
         }
      }
   }
   return OK;
}

int
ev_handler(struct ev* ev,int fd)
{
   int ret;
   int ti;
   int event;

   if (fd == ev->signalfd)
   {
      ret = signal_handler(ev,fd);
   }
   else
   {
      /* table lookup for fd */
      ti = ev->ev_table_imap[fd];
      event = ev->ev_table[ti].event;

      switch (event)
      {
         case ACCEPT:
            ret = accept_handler(ev,ti);
            break;
         case SEND:
            ret = send_handler(ev,ti);
            break;
         case RECEIVE:
            ret = receive_handler(ev,ti);
            break;
         case PERIODIC:
            ret = pgagroal_periodic_handler(ev,ti);
            break;
         default:
            return 1;
      }
   }

   /* deal with ret */
   if (ret == CLOSE_FD)
   {
      ev_table_remove(ev,ti);
   }

   return 0;
}

int
accept_handler(struct ev* ev,int ti)
{
   int listen_fd = ev->ev_table[ti].epoll_ev.data.fd;
   while (1)
   {
      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);
      int client_fd = accept(listen_fd,(struct sockaddr*)&client_addr,&client_len);

      //
      // pgagroal can deal with accept returning -1
      //
      if (client_fd == -1)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK)
         {
            ev->ev_table[ti].cb.io(ev->data,listen_fd,client_fd,errno);
            break;
         }
         else
         {
            fprintf(stderr,"accept\n");
            break;
         }
      }

      if (ev->ev_table[ti].cb.io)
      {
         ev->ev_table[ti].cb.io(ev->data,listen_fd,client_fd,0);
      }
   }
   return 0;
}

int
receive_handler(struct ev* ev,int ti)
{
   int ret = OK;
   int nrecv = 0;
   int total_recv = 0;
   int capacity = MISC_LENGTH;
   int fd = ev->ev_table[ti].epoll_ev.data.fd;
   void* buf = malloc(sizeof(char) * capacity);
   if (!buf)
   {
      perror("Failed to allocate memory");
      return EV_ALLOC_ERROR;
   }

   while (1)
   {
      nrecv = recv(fd,buf,capacity,0);
      if (nrecv == -1)
      {
         if (errno != EAGAIN && errno != EWOULDBLOCK)
         {
            perror("receive_handler: recv");
         }
         break;
      }
      else if (nrecv == 0) /* connection closed */
      {
         ret = CLOSE_FD;
         goto clean;
      }
      total_recv += nrecv;
      if (total_recv == capacity && capacity < MAX_BUF_LEN)
      {
         int new_capacity = capacity * 2;
         if (new_capacity > MAX_BUF_LEN)
         {
            new_capacity = MAX_BUF_LEN;
         }
         char* new_buf = realloc(buf,new_capacity);
         if (!new_buf)
         {
            perror("Failed to reallocate memory");
            ret = EV_ALLOC_ERROR;
            goto clean;
         }
         buf = new_buf;
         capacity = new_capacity;
      }

      if (capacity == MAX_BUF_LEN && total_recv == capacity)
      {
         break;
      }
   }

   if (ev->ev_table[ti].cb.io)
   {
      ret = ev->ev_table[ti].cb.io(ev->data,fd,0,(void*) buf,total_recv);
   }

clean:
   free(buf);
   return ret;
}

int
send_handler(struct ev* ev,int ti)
{
   int fd = ev->ev_table[ti].epoll_ev.data.fd;
   ssize_t nsent;
   size_t total_sent = 0;
   void* buf = ev->ev_table[ti].buf;
   size_t buf_len = ev->ev_table[ti].buf_len;

   while (total_sent < buf_len)
   {
      nsent = send(fd,buf + total_sent,buf_len - total_sent,0);
      if (nsent == -1)
      {
         if (errno != EAGAIN && errno != EWOULDBLOCK)
         {
            perror("send");
            break;
         }
         else if (errno == EPIPE)
         {
            return CLOSE_FD;
         }
      }
      else
      {
         total_sent += nsent;
      }
   }
   if (total_sent < buf_len)
   {
      pgagroal_io_send_init(ev,fd,ev->ev_table[ti].cb.io,buf + total_sent,buf_len - total_sent,0);
   }

   return 0;
}

int
pgagroal_signal_init(struct ev* io,int signum,signal_cb cb)
{
   int ret;
   int fd;

   sigaddset(&io->sigset,signum);

   ret = sigprocmask(SIG_BLOCK,&io->sigset,NULL);
   if (ret == -1)
   {
      perror("signal_init_epoll: sigprocmask");
      return -1;
   }

   fd = signalfd(io->signalfd,&io->sigset,SFD_NONBLOCK);
   if (fd == -1)
   {
      perror("signal_init_epoll: signalfd");
      return ERROR;
   }

   ret = pgagroal_signal_table_insert(io,signum,cb);
   if (ret)
   {
      fprintf(stdout,"signal_init_epoll: ev_table_insert\n");
      close(fd);
      return ERROR;
   }

   return OK;
}

int
pgagroal_signal_table_insert(struct ev* ev,int signum,signal_cb cb)
{
   switch (signum)
   {
      case SIGTERM:
         ev->sig_table[_SIGTERM].cb = cb;
         ev->sig_table[_SIGTERM].signum = signum;
         break;
      case SIGHUP:
         ev->sig_table[_SIGHUP].cb = cb;
         ev->sig_table[_SIGHUP].signum = signum;
         break;
      case SIGINT:
         ev->sig_table[_SIGINT].cb = cb;
         ev->sig_table[_SIGINT].signum = signum;
         break;
      case SIGTRAP:
         ev->sig_table[_SIGTRAP].cb = cb;
         ev->sig_table[_SIGTRAP].signum = signum;
         break;
      case SIGABRT:
         ev->sig_table[_SIGABRT].cb = cb;
         ev->sig_table[_SIGABRT].signum = signum;
         break;
      case SIGALRM:
         ev->sig_table[_SIGALRM].cb = cb;
         ev->sig_table[_SIGALRM].signum = signum;
         break;
      default:
         fprintf(stderr,"signal not supported\n");
         return 1;
   }
   return 0;
}

int
signal_handler(struct ev* ev,int sfd)
{
   int ret;
   struct signalfd_siginfo info;

   ret = read(sfd,&info,sizeof(info));
   if (ret != sizeof(info))
   {
      perror("signal_handler: read");
      return ERROR;
   }

   int signum = info.ssi_signo;
   if (signum >= 0)
   {
      switch (signum)
      {
         case SIGTERM:
            ev->sig_table[_SIGTERM].cb(ev->data,0);
            break;
         case SIGHUP:
            ev->sig_table[_SIGHUP].cb(ev->data,0);
            break;
         case SIGINT:
            ev->sig_table[_SIGINT].cb(ev->data,0);
            break;
         case SIGTRAP:
            ev->sig_table[_SIGTRAP].cb(ev->data,0);
            break;
         case SIGABRT:
            ev->sig_table[_SIGABRT].cb(ev->data,0);
            break;
         case SIGALRM:
            ev->sig_table[_SIGALRM].cb(ev->data,0);
            break;
         default:
            fprintf(stderr,"signal not supported\n");
            return 1;
      }
   }
   return 0;
}

int
ev_table_insert(struct ev* ev,int fd,int event,event_cb cb,void* buf,size_t buf_len)
{
   int ret;
   int i,ti;
   struct ev_entry* table = ev->ev_table;

   if (ev->ev_table_imap[fd] == EMPTY)
   {
      /* get next empty ti */
      for (i = 0; i < MAX_EVENTS; i++)
      {
         if (ev->ev_table[i].epoll_ev.data.fd == EMPTY)
         {
            ti = i;
            break;
         }
      }
      if (i >= MAX_EVENTS)
      {
         fprintf(stderr,"ev_table_insert: table is full\n");
         return ERROR;
      }

      ev->ev_table_imap[fd] = ti;
   }
   else  /* the file descriptor is already inserted, keep the position in the table */
   {
      ti = ev->ev_table_imap[fd];
   }

   table[ti].epoll_ev.data.fd = fd;
   table[ti].event = event;
   table[ti].cb = cb;
   table[ti].buf = buf;
   table[ti].buf_len = buf_len;

   switch (event)
   {
      case READ:
      case ACCEPT:
      case RECEIVE:
      case SIGNAL:
      case PERIODIC:
         ev->ev_table[ti].epoll_ev.events = EPOLLIN | EPOLLET;
         break;
      case WRITE:
      case SEND:
         ev->ev_table[ti].epoll_ev.events = EPOLLOUT | EPOLLET;
         break;
      default:
         return 1;
   }

   ret = epoll_ctl(ev->epoll_fd,EPOLL_CTL_ADD,fd,&table[ti].epoll_ev);
   if (ret == -1)
   {
      perror("epoll_ctl: listen_fd");
      return 1;
   }

   return OK;
}

int
ev_table_remove(struct ev* ev,int ti)
{
   int ret;
   int fd = ev->ev_table[ti].epoll_ev.data.fd;

   /* removal */
   ev->ev_table_imap[fd] = EMPTY;
   ev->ev_table[ti].epoll_ev.data.fd = EMPTY;

   ret = epoll_ctl(ev->epoll_fd,EPOLL_CTL_DEL,fd,NULL);
   if (ret == -1)
   {
      perror("ev_table_remove: epoll_ctl");
   }

   ret = close(fd);
   if (ret == -1) /* returned errno: EBADF || EIO || EINTR || ENOSPC */
   {
      return 1;
   }

   return 0;
}

int
pgagroal_periodic_init(struct ev* ev,int msec,periodic_cb cb)
{
   int ret;
   int fd;
   int sec,nsec;
   struct timespec now;
   struct itimerspec new_value;

   /* TODO:
    *  what kind of clock to use?
    */
   ret = clock_gettime(CLOCK_MONOTONIC,&now);
   if (ret == -1)
   {
      perror("clock_gettime");
      return ERROR;
   }

   sec = msec / 1000;
   nsec = (msec % 1000) * 1000000;

   new_value.it_value.tv_sec = sec;
   new_value.it_value.tv_nsec = nsec;

//   if (new_value.it_value.tv_nsec >= 1000000000)
//   {
//      new_value.it_value.tv_sec += 1;
//      new_value.it_value.tv_nsec -= 1000000000;
//   }

   new_value.it_interval.tv_sec = sec;
   new_value.it_interval.tv_nsec = nsec;

   fd = timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK);  /* no need to set it to non-blocking due to TFD_NONBLOCK */

   ret = timerfd_settime(fd,0,&new_value,NULL);
   if (ret == -1)
   {
      perror("timerfd_settime");
      return ERROR;
   }

   ret = ev_table_insert(ev,fd,PERIODIC,(event_cb)cb,NULL,0);
   if (ret)
   {
      fprintf(stderr,"periodic_init: ev_table_insert\n");
      close(fd); /* TODO: any other error cleanups? */
      return ERROR;
   }

   return OK;
}

int
pgagroal_periodic_handler(struct ev* ev,int t_index)
{
   struct ev_entry* table = ev->ev_table;
   uint64_t exp;
   int fd = table[t_index].epoll_ev.data.fd;
   int nread = read(fd,&exp,sizeof(uint64_t));
   if (nread != sizeof(uint64_t))
   {
      perror("periodic_handler: read");
      return ERROR;
   }
   table[t_index].cb.signal(ev->data,0);
   return OK;
}

int
pgagroal_ev_free(struct ev** ev_out)
{
   if (ev_out == NULL || *ev_out == NULL)
   {
      return OK;
   }

   int ti;
   struct ev* ev = *ev_out;

   /* remove valid descriptors from ev_table */
   for (int fd = 0; fd < EV_MAX_FDS; fd++)
   {
      ti = ev->ev_table_imap[fd];
      if (ti != EMPTY)
      {
         ev_table_remove(ev,ti);
      }
   }

   close(ev->epoll_fd);

   free(ev);

   *ev_out = NULL;

   return OK;
}

int
set_non_blocking(int fd)
{
   int flags = fcntl(fd,F_GETFL,0);
   if (flags == -1)
   {
      return -1;
   }
   return fcntl(fd,F_SETFL,flags | O_NONBLOCK);
}

#endif
