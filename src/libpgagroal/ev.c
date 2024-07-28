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
#include <pgagroal.h>

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

#define for_each(w, first) for (w = first; w; w = w->next)

#define list_add(w, first) \
        do  \
        { \
           w->next = first; \
           first = w; \
        } while (0) \


#define list_delete(w, first, target, ret)                            \
        do                                                                    \
        {                                                                     \
           for (w = first; *w && *w != target; w = &(*w)->next);         \
           if (!(*w)) {                                                  \
              fprintf(stderr, "Error: Target watcher not found\n"); \
              ret = EV_ERROR;                                       \
           } else {                                                      \
              if (!target->next) {                                  \
                 *w = NULL;                                    \
              } else {                                              \
                 *w = target->next;                            \
              }                                                     \
           }                                                             \
        } while (0)                                                           \

/*
 * Code was inspired by: https://git.kernel.dk/cgit/liburing/tree/examples/proxy.c
 * (C) 2024 Jens Axboe <axboe@kernel.dk>
 */

int
_ev_setup_context(struct ev_context* ctx, struct ev_context orig)
{
#if USE_URING

   /* set opts */
   ctx->napi = orig.napi;
   ctx->sqpoll = orig.sqpoll;
   ctx->use_huge = orig.use_huge;
   ctx->defer_tw = orig.defer_tw;
   ctx->snd_ring = orig.snd_ring;
   ctx->snd_bundle = orig.snd_bundle;
   ctx->fixed_files = orig.fixed_files;
   ctx->no_use_buffers = orig.no_use_buffers;
   ctx->buf_count = orig.buf_count;

   /* asserts */
   if (ctx->defer_tw && ctx->sqpoll)
   {
      fprintf(stderr, "Cannot use DEFER_TW and SQPOLL at the same time\n");
      exit(1);
   }

   /* TODO: this is not supposed to be like this.
    * The client is supposed to set this.
    * This is here temporarily.
    */
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
      ctx->params.sq_thread_idle = orig.params.sq_thread_idle;
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
      fprintf(stderr, "io_context_setup: no support for fixed files\n"); /*TODO: add support for fixed files */
      exit(1);
   }

#else /* USE_URING */

   ctx->epoll_flags = orig.epoll_flags;

#endif

   return EV_OK;
}

struct ev_loop*
pgagroal_ev_init(struct ev_context ctx)
{
   int ret = EV_OK;

   struct ev_loop* ev = calloc(1, sizeof(struct ev_loop));

   ret = _ev_setup_context(&ev->ctx, ctx);
   if (ret)
   {
      fprintf(stderr, "ev_init: ev_setup\n");
      return NULL;
   }

   sigemptyset(&ev->sigset);

   /* dummy heads */
   ev->ihead.slot = -1;
   ev->ihead.next = NULL;

   ev->shead.slot = -1;
   ev->shead.next = NULL;

   ev->phead.slot = -1;
   ev->phead.next = NULL;

#if USE_URING

   /* gets a new ring even after fork */
   ret = io_uring_queue_init_params(ev->ctx.entries, &ev->ring, &ev->ctx.params);
   if (ret)
   {
      fprintf(stderr, "err: io_uring_queue_init_params: %s\n", strerror(-ret));
      return NULL;
   }

   if (!ev->ctx.no_use_buffers)
   {
      ret = _ev_setup_buffers(ev);
      if (ret)
      {
         fprintf(stderr, "err: ev_setup_buffers");
         goto error;
      }
   }

#else /* USE_URING */
   
   struct epoll_event event;

   ev->buffer = malloc(sizeof(char) * INITIAL_BUFFER_SIZE);
   ev->capacity = INITIAL_BUFFER_SIZE;

   ev->epollfd = epoll_create1(ev->ctx.epoll_flags);
   if (ev->epollfd == -1)
   {
      perror("epoll_create1");
      goto error;
   }

   /* signals use sig_table */
   ev->signalfd = signalfd(-1, &ev->sigset, SFD_NONBLOCK);
   if (ev->signalfd == -1)
   {
      perror("signalfd");
      goto error;
   }

   event.data.fd = ev->signalfd;
   event.events = EPOLLIN | EPOLLET;

   if (epoll_ctl(ev->epollfd, EPOLL_CTL_ADD, ev->signalfd, &event) == -1)
   {
      perror("epoll_ctl");
      goto error;
   }

#endif

   return ev;

error:
   free(ev);
   return NULL;
}

int
pgagroal_ev_loop_destroy(struct ev_loop* ev)
{
   /* clean signals */
   sigemptyset(&ev->sigset);

#if USE_URING
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

   io_uring_queue_exit(&ev->ring);

#else /* USE_URING */

   close(ev->epollfd);

#endif

   free(ev);
   return EV_OK;
}

void
pgagroal_ev_loop_fork(struct ev_loop* loop)
{
   struct ev_signal *w;
   for_each(w, loop->shead.next)
   {
        sigdelset(&loop->sigset, w->signum);
   }
   if (sigprocmask(SIG_UNBLOCK, &loop->sigset, NULL) == -1)
   {
      fprintf(stderr, "Error: sigprocmask\n");
   }

#if !USE_URING
   (void)signalfd(ev->signalfd, &ev->sigset, SFD_NONBLOCK);
#endif



#if !USE_URING
   (void)signalfd(ev->signalfd, &ev->sigset, SFD_NONBLOCK);
#endif

   return;
}

void
pgagroal_ev_copy(struct ev_loop* copy, struct ev_loop* loop)
{

        return;
}

int
pgagroal_ev_loop_break(struct ev_loop* ev)
{
   ev->running = false;
   return EV_OK;
}

int
pgagroal_ev_atomic_loop_break(struct ev_loop* ev)
{
   atomic_store(&ev->atomic_running, false);
   return EV_OK;
}

int
pgagroal_ev_io_init(struct ev_io* w, int fd, int event, io_cb cb, void* data, int size, int slot)
{
   if (event >= IO_EVENTS_NR)
   {
      fprintf(stderr, "io_init: invalid event flag number: %d\n", event);
      return EV_ERROR;
   }
   w->type = event;

   w->slot = slot;
   w->fd = fd;
   w->cb = cb;
   w->data = data;
   w->size = size;

   return EV_OK;
}

int
pgagroal_ev_io_accept_init(struct ev_io* w, int fd, io_cb cb)
{
   return pgagroal_ev_io_init(w, fd, EV_ACCEPT, cb, NULL, 0, -1);
}

int
pgagroal_ev_io_read_init(struct ev_io* w, int fd, io_cb cb)
{
   return pgagroal_ev_io_init(w, fd, READ, cb, NULL, 0, -1);
}

int
pgagroal_ev_io_send_init(struct ev_io* w, int fd, io_cb cb, void* buf, int buf_len, int bid)
{
   return pgagroal_ev_io_init(w, fd, EV_SEND, cb, buf, buf_len, bid);
}

int
pgagroal_ev_io_receive_init(struct ev_io* w, int fd, io_cb cb)
{
   return pgagroal_ev_io_init(w, fd, EV_RECEIVE, cb, NULL, 0, -1);
}

int
pgagroal_ev_io_connect_init(struct ev_io* w, int fd, io_cb cb, union sockaddr_u* addr)
{
   return pgagroal_ev_io_init(w, fd, CONNECT, cb, (void*)addr, 0, -1);
}

int
pgagroal_ev_io_start(struct ev_loop* ev, struct ev_io* w)
{
   int domain;
   union sockaddr_u* addr;

   list_add(w, ev->ihead.next);

#if USE_URING
   struct io_uring_sqe* sqe = _get_sqe(ev);
   io_uring_sqe_set_data(sqe, w);
   switch (w->type)
   {
      case EV_ACCEPT:
         io_uring_prep_multishot_accept(sqe, w->fd, NULL, NULL, 0);
         break;
      case EV_RECEIVE:
         io_uring_prep_recv_multishot(sqe, w->fd, NULL, 0, 0);
         sqe->flags |= IOSQE_BUFFER_SELECT;
         sqe->buf_group = 0;
         break;
      case EV_SEND:
         io_uring_prep_send(sqe, w->fd, w->data, w->size, MSG_WAITALL | MSG_NOSIGNAL); /* TODO: why these flags? */
         break;
      case CONNECT:
         addr = (union sockaddr_u*)w->data;
         if (ev->ctx.ipv6)
         {
            io_uring_prep_connect(sqe, w->fd, (struct sockaddr*) &addr->addr6, sizeof(struct sockaddr_in6));
         }
         else
         {
            io_uring_prep_connect(sqe, w->fd, (struct sockaddr*) &addr->addr4, sizeof(struct sockaddr_in));
         }
         break;
      case SOCKET:
         if (ev->ctx.ipv6)
         {
            domain = AF_INET6;
         }
         else
         {
            domain = AF_INET;
         }
         io_uring_prep_socket(sqe, domain, SOCK_STREAM, 0, 0); /* TODO: WHAT CAN BE USED HERE ? */
         break;

      case READ: /* unused */
         io_uring_prep_read(sqe, w->fd, w->data, w->size, 0);
         break;

      default:
         fprintf(stderr, "io_init: unknown event type: %d\n", w->type);
         return EV_ERROR;
   }
#else /* USE_URING */
   struct epoll_event event;
   switch (w->type)
   {
      case EV_ACCEPT:
      case EV_RECEIVE:
         event.events = EPOLLIN | EPOLLET;
         break;
      case EV_SEND:
         event.events = EPOLLIN | EPOLLET;
         break;
      default:
         fprintf(stderr, "io_init: unknown event type: %d\n", w->type);
         return EV_ERROR;
   }
   event.data.u64 = (uint64_t)w;

   if (epoll_ctl(ev->epollfd, EPOLL_CTL_ADD, w->fd, &event) == -1)
   {
      perror("epoll_ctl");
      close(w->fd);
      return EV_ERROR;
   }

#endif

   return EV_OK;
}

int
pgagroal_ev_io_stop(struct ev_loop* ev, struct ev_io* target)
{
   int ret = EV_OK;
   struct io_uring_sqe* sqe;
   struct ev_io** w;
   if (!target)
   {
      fprintf(stderr, "err: NULL pointer provided to stop\n");
      return EV_ERROR;
   }
   list_delete(w, &ev->ihead.next, target, ret);

#if USE_URING
   sqe = io_uring_get_sqe(&ev->ring);
   io_uring_prep_cancel64(sqe, (uint64_t)target, 0); /* TODO: flags? */
#else
   if (epoll_ctl(ev->epollfd, EPOLL_CTL_DEL, target->fd, NULL) == -1)
   {
      perror("epoll_ctl: delete failed");
      ret = EV_ERROR;
   }
#endif

   close(target->fd);
   return ret;
}

int
pgagroal_ev_signal_init(struct ev_signal* w, signal_cb cb, int signum)
{
   w->type = EV_SIGNAL;
   w->signum = signum;
   w->cb = cb;
   w->slot = -1;
   w->next = NULL;
   return EV_OK;
}

int
pgagroal_ev_signal_start(struct ev_loop* ev, struct ev_signal* w)
{
   sigaddset(&ev->sigset, w->signum);

   if (sigprocmask(SIG_BLOCK, &ev->sigset, NULL) == -1)
   {
      fprintf(stdout, "sigprocmask failed\n");
      return EV_ERROR;
   }

#if !USE_URING
   (void)signalfd(ev->signalfd, &ev->sigset, SFD_NONBLOCK);
#endif

   list_add(w, ev->shead.next);

   return EV_OK;
}

int
pgagroal_ev_signal_stop(struct ev_loop* ev, struct ev_signal* target)
{
   int ret = EV_OK;
   struct ev_signal** w;

   if (!target)
   {
      fprintf(stderr, "err: NULL pointer provided to stop\n");
      return EV_ERROR;
   }

   list_delete(w, &ev->shead.next, target, ret);
   sigdelset(&ev->sigset, target->signum);
   if (sigprocmask(SIG_UNBLOCK, &ev->sigset, NULL) == -1)
   {
      fprintf(stderr, "Error: sigprocmask\n");
      return EV_ERROR;
   }

#if !USE_URING
   (void)signalfd(ev->signalfd, &ev->sigset, SFD_NONBLOCK);
#endif

   return ret;
}

int
pgagroal_ev_periodic_init(struct ev_periodic* w, periodic_cb cb, int msec)
{
#if USE_URING
   w->ts = (struct __kernel_timespec){
      .tv_sec = msec / 1000,
      .tv_nsec = (msec % 1000) * 1000000
   };

#else
   struct timespec now;
   struct itimerspec new_value;

   /**
    * TODO: evaluate what kind of clock to use (!)
    */
   if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
   {
      perror("clock_gettime");
      return EV_ERROR;
   }

   new_value.it_value.tv_sec = msec / 1000;
   new_value.it_value.tv_nsec = (msec % 1000) * 1000000;

   new_value.it_interval.tv_sec = msec / 1000;
   new_value.it_interval.tv_nsec = (msec % 1000) * 1000000;

   w->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);  /* no need to set it to non-blocking due to TFD_NONBLOCK */
   if (w->fd == -1)
   {
      perror("timerfd_create");
      return EV_ERROR;
   }

   if (timerfd_settime(w->fd, 0, &new_value, NULL) == -1)
   {
      perror("timerfd_settime");
      close(w->fd);
      return EV_ERROR;
   }

#endif

   w->type = EV_PERIODIC;
   w->slot = -1;
   w->cb = cb;
   w->next = NULL;

   return EV_OK;
}

int
pgagroal_ev_periodic_start(struct ev_loop* ev, struct ev_periodic* w)
{

#if USE_URING
   struct io_uring_sqe* sqe = io_uring_get_sqe(&ev->ring);
   io_uring_sqe_set_data(sqe, w);
   io_uring_prep_timeout(sqe, &w->ts, 0, IORING_TIMEOUT_MULTISHOT);

#else
   struct epoll_event event;
   event.events = EPOLLIN | EPOLLET;
   event.data.u64 = (uint64_t)w;

   if (epoll_ctl(ev->epollfd, EPOLL_CTL_ADD, w->fd, &event) == -1)
   {
      perror("epoll_ctl");
      close(w->fd);
      return EV_ERROR;
   }

#endif

   list_add(w, ev->phead.next);

   return EV_OK;
}

int
pgagroal_ev_periodic_stop(struct ev_loop* ev, struct ev_periodic* target)
{
   int ret = EV_OK;
   struct io_uring_sqe* sqe;
   struct ev_periodic** w;
   if (!target)
   {
      fprintf(stderr, "err: NULL pointer provided to stop\n");
      return EV_ERROR;
   }
   list_delete(w, &ev->phead.next, target, ret);

#if USE_URING
   sqe = io_uring_get_sqe(&ev->ring);
   io_uring_prep_cancel64(sqe, (uint64_t)target, 0); /* TODO: flags? */
#else
   if (epoll_ctl(ev->epollfd, EPOLL_CTL_DEL, target->fd, NULL) == -1)
   {
      perror("epoll_ctl: delete failed");
      ret = EV_ERROR;
   }
#endif

   return ret;
}

#if USE_URING

/******************************************************************************
 * IO_URING LOOPS
 *****************************************************************************/

int
pgagroal_ev_atomic_loop(struct ev_loop* ev)
{
   int ret;
   int signum;
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

   ev->running = true; /* safe */
   while (atomic_load(&ev->atomic_running))
   {
      ts = &idle_ts;
      io_uring_submit_and_wait_timeout(&ev->ring, &cqe, to_wait, ts, NULL);

      /* Good idea to leave here to see what happens */
      if (*ev->ring.cq.koverflow)
      {
         printf("overflow %u\n", *ev->ring.cq.koverflow);
         fflush(stdout);
         exit(1);
      }
      if (*ev->ring.sq.kflags & IORING_SQ_CQ_OVERFLOW)
      {
         printf("saw overflow\n");
         fflush(stdout);
         exit(1);
      }

      /* Check for signals before iterating over cqes */
      signum = sigtimedwait(&ev->sigset, NULL, &timeout);
      if (signum > 0)
      {
         ret = _signal_handler(ev, signum);
         if (ret)
         {
            fprintf(stderr, "Signal handler not found\n");
            return EV_ERROR;
         }
         if (!atomic_load(&ev->atomic_running))
         {
            break;
         }
      }

      events = 0;
      io_uring_for_each_cqe(&(ev->ring), head, cqe)
      {
         if (_ev_handler(ev, cqe))
         {
            fprintf(stderr, "ev_loop: io_handle_event\n");
            return EV_ERROR;
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
pgagroal_ev_loop(struct ev_loop* ev)
{
   int ret;
   int signum;
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

   ev->running = true; /* safe */
   while (ev->running)
   {
      ts = &idle_ts;
      io_uring_submit_and_wait_timeout(&ev->ring, &cqe, to_wait, ts, NULL);

      /* Good idea to leave here to see what happens */
      if (*ev->ring.cq.koverflow)
      {
         printf("overflow %u\n", *ev->ring.cq.koverflow);
         fflush(stdout);
         exit(1);
      }
      if (*ev->ring.sq.kflags & IORING_SQ_CQ_OVERFLOW)
      {
         printf("saw overflow\n");
         fflush(stdout);
         exit(1);
      }

      /* Check for signals before iterating over cqes */
      signum = sigtimedwait(&ev->sigset, NULL, &timeout);
      if (signum > 0)
      {
         ret = _signal_handler(ev, signum);
         if (ret)
         {
            fprintf(stderr, "Signal handler not found\n");
            return EV_ERROR;
         }
         if (!ev->running)
         {
            printf("dbg: loop not running any longer\n");
            break;
         }
      }

      events = 0;
      io_uring_for_each_cqe(&(ev->ring), head, cqe)
      {
         if (_ev_handler(ev, cqe))
         {
            fprintf(stderr, "ev_loop: io_handle_event\n");
            return EV_ERROR;
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

/******************************************************************************
 * IO_URING HANDLERS
 *****************************************************************************/

int
_ev_handler(struct ev_loop* ev, struct io_uring_cqe* cqe)
{
   struct ev_periodic* w = (struct ev_periodic*)io_uring_cqe_get_data(cqe);
   int type;

   /**
    * Cancelled requests will trigger _ev_handler, but have NULL data.
    */
   if (!w)
   {
      return EV_OK;
   }

   type = w->type;
   if (type == EV_PERIODIC)
   {
      return _periodic_handler(ev, (struct ev_periodic*)w);
   }
   return _io_handler(ev, (struct ev_io*)w, cqe, type);
}

int
_io_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe, int type)
{
   int ret = EV_OK;
   int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
   int bid_end = bid;
   void* buf;

   switch (w->type)
   {
      case EV_ACCEPT:
         return _accept_handler(ev, w, cqe);
      case EV_SEND:
         return _send_handler(ev, w, cqe);
      case EV_RECEIVE:
retry:
         ret = _receive_handler(ev, w, cqe, &buf, &bid_end, false);
         printf("dbg: ret=%d\n", ret);
         switch (ret)
         {
            case EV_CLOSE_FD: /* connection closed */
               close(w->fd);
               pgagroal_ev_io_stop(ev, w);
               ret = EV_OK;
               break;
            /* TODO: Stress test this: buffers should be replenished after each recv */
            case EV_REPLENISH_BUFFERS:
               fprintf(stdout, "_io_handler: need to replenish buffers, requeue request\n");
               // ret = _replenish_buffers(ev, &ev->in_br, ev->bid, ev->bid+1);
               usleep(100);
               goto retry;
               break;
         }
         break;
      default:
         fprintf(stdout, "_io_handler: event not found eventno=%d\n", type);
         exit(1);
   }
   return ret;
}

int
_signal_handler(struct ev_loop* ev, int signum)
{
   struct ev_signal* w;
   for (w = ev->shead.next; w && w->signum != signum; w = w->next)
   {
      /* empty */;
   }
   if (!w)
   {
      fprintf(stderr, "debug: not supposed to happen, no watcher for signal %d\n", signum);
      exit(1);
   }
   w->cb(ev, w, 0);
   return EV_OK;
}

int
_periodic_handler(struct ev_loop* ev, struct ev_periodic* w)
{
   w->cb(ev, w, 0);
   return EV_OK;
}

int
_accept_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe)
{
   w->client_fd = cqe->res;

   int flags = fcntl(w->client_fd, F_GETFL, 0);
   if (flags == -1) {
      perror("fcntl");
      close(w->client_fd);
      return 1;
   }

   if (flags & O_NONBLOCK) {
      printf("client_fd=%d : NON-BLOCKING\n", w->client_fd);
   } else {
      printf("client_fd=%d : BLOCKING\n", w->client_fd);
   }

   w->cb(ev, w, 0);

   return EV_OK;
}

int
_receive_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe, void** send_buf_base, int* bid, bool is_proxy)
{
   int ret = EV_OK;
   struct ev_context ctx = ev->ctx;
   struct io_buf_ring* in_br = &ev->in_br;
   struct io_buf_ring* out_br = &ev->out_br;
   *send_buf_base = (void*) (in_br->buf + *bid * ctx.buf_size);
   struct io_uring_buf* buf;
   void* data;
   int this_bytes;
   int in_bytes;
   int bid_start = *bid;
   int total_in_bytes;

   if (cqe->res == -ENOBUFS)
   {
      fprintf(stderr, "io_receive_handler: Not enough buffers\n");
      return EV_REPLENISH_BUFFERS;
   }

   if (!(cqe->flags & IORING_CQE_F_BUFFER))
   {
      if (!(cqe->res)) /* Closed connection */
      {
         return EV_CLOSE_FD;
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

      io_uring_buf_ring_add(out_br->br, data, this_bytes, *bid, ctx.br_mask, 0);
      io_uring_buf_ring_advance(out_br->br, 1);

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
      ret = _rearm_receive(ev, w);
      if (ret)
      {
         return EV_ERROR;
      }
   }

   w->data = send_buf_base;
   w->size = total_in_bytes;
   w->cb(ev, w, ret);

   ret = _replenish_buffers(ev, in_br, bid_start, *bid);
   if (ret)
   {
      perror("replenish_buffers");
      return EV_ERROR;
   }

   ev->bid = *bid;

   return 0;
}

int
_send_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe)
{
   int ret;
   int buf_len = cqe->res;
   struct ev_context ctx = ev->ctx;
   int bid_end = (ev->bid + buf_len / ctx.buf_size + (int)(buf_len % ctx.buf_size > 0)) % ctx.buf_count;
   ret = _replenish_buffers(ev, &ev->out_br, ev->bid, bid_end);
   if (ret)
   {
      return EV_ERROR;
   }
   return EV_OK;
}

/******************************************************************************
 * IO_URING SHARED BUFFER FUNCTIONS
 *****************************************************************************/

int
_ev_setup_buffers(struct ev_loop* ev)
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

   // ev->next_out_bid = 0;
   return ret;
}

int
_replenish_buffers(struct ev_loop* ev, struct io_buf_ring* br, int bid_start, int bid_end)
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
      io_uring_buf_ring_add(br->br, (void*)br->br->bufs[i].addr, ctx.buf_size, i, ctx.br_mask, 0);
   }

   io_uring_buf_ring_advance(br->br, count);

   return 0;
}

int
_rearm_receive(struct ev_loop* ev, struct ev_io* w)
{
   struct io_uring_sqe* sqe = _get_sqe(ev);
   io_uring_prep_recv_multishot(sqe, w->fd, NULL, 0, 0);

   io_uring_sqe_set_data(sqe, w);
   sqe->flags |= IOSQE_BUFFER_SELECT;
   sqe->buf_group = 0;
   return EV_OK;
}

/******************************************************************************
 * IO_URING UTILS
 *****************************************************************************/

struct io_uring_sqe*
_get_sqe(struct ev_loop* ev)
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
next_bid(struct ev_loop* ev, int* bid)
{
   struct ev_context ctx = ev->ctx;
   *bid = (*bid + 1) % ctx.buf_count;
}

#else /* USE_URING */

/******************************************************************************
 * EPOLL LOOP
 *****************************************************************************/

int
pgagroal_ev_loop(struct ev_loop* ev)
{
   int ret;
   ev_watcher w;
   struct epoll_event event;
   struct epoll_event events[MAX_EVENTS];
   ev->running = true;
   while (ev->running)
   {
      int nfds = epoll_wait(ev->epollfd, events, MAX_EVENTS, 100);
      if (nfds == -1)
      {
         perror("epoll_wait");
         return EV_ERROR;
      }
      if (!ev->running)
      {
         break;
      }
      for (int i = 0; i < nfds; i++)
      {
         event = events[i];
         if (event.data.fd == ev->signalfd)
         {
            ret = _signal_handler(ev);
         }
         else
         {
            ret = _ev_handler(ev, (void*)events[i].data.u64);

         }
         if (ret)
         {
            perror("_ev_handler\n");
            return EV_ERROR;
         }
      }
   }
   return EV_OK;
}

int
pgagroal_ev_atomic_loop(struct ev_loop* ev)
{
   int ret;
   ev_watcher w;
   struct epoll_event event;
   struct epoll_event events[MAX_EVENTS];
   atomic_store(&ev->atomic_running, true);
   while (atomic_load(&ev->atomic_running))
   {
      int nfds = epoll_wait(ev->epollfd, events, MAX_EVENTS, 100);
      if (nfds == -1)
      {
         perror("epoll_wait");
         return EV_ERROR;
      }
      if (!atomic_load(&ev->atomic_running))
      {
         break;
      }
      for (int i = 0; i < nfds; i++)
      {
         event = events[i];
         if (event.data.fd == ev->signalfd)
         {
            ret = _signal_handler(ev);
         }
         else
         {
            ret = _ev_handler(ev, (void*)events[i].data.u64);

         }
         if (ret)
         {
            perror("_ev_handler\n");
            return EV_ERROR;
         }
      }
   }
   return EV_OK;
}

/******************************************************************************
 * EPOLL HANDLERS
 *****************************************************************************/

int
_ev_handler(struct ev_loop* ev, void* wp)
{
   struct ev_periodic* w = (struct ev_periodic*)wp;
   if (w->type == EV_PERIODIC)
   {
      return _periodic_handler(ev, (struct ev_periodic*)w);
   }
   return _io_handler(ev, (struct ev_io*)w);
}

int
_signal_handler(struct ev_loop* ev)
{
   int ret;
   struct signalfd_siginfo info;
   int signum;
   struct ev_signal* w;

   ret = read(ev->signalfd, &info, sizeof(info));
   if (ret != sizeof(info))
   {
      perror("_signal_handler: read");
      return EV_ERROR;
   }

   for (w = ev->shead.next; w && w->signum != info.ssi_signo; w = w->next)
   {
      /* empty */;
   }
   if (!w)
   {
      perror("couldn't find signal\n");
      return EV_ERROR;
   }

   w->cb(ev, w, 0);

   return EV_OK;
}

int
_periodic_handler(struct ev_loop* ev, struct ev_periodic* w)
{
   uint64_t exp;
   int nread = read(w->fd, &exp, sizeof(uint64_t));
   if (nread != sizeof(uint64_t))
   {
      perror("periodic_handler: read");
      return EV_ERROR;
   }

   w->cb(ev, w, 0);

   return EV_OK;
}

int
_io_handler(struct ev_loop* ev, struct ev_io* w)
{
   int ret = EV_OK;

   switch (w->type)
   {
      case EV_ACCEPT:
         return _accept_handler(ev, w);
      case EV_SEND:
         return _send_handler(ev, w);
      case EV_RECEIVE:
         ret = _receive_handler(ev, w);
         switch (ret)
         {
            case EV_CLOSE_FD: /* connection closed */
               pgagroal_ev_io_stop(ev, w);
               ret = EV_OK;
               break;
         }
      default:
         fprintf(stdout, "_io_handler: not supposed to happen\n");
         exit(1);
   }

   return ret;
}

int
_accept_handler(struct ev_loop* ev, struct ev_io* w)
{
   int ret = EV_OK;
   int listen_fd = w->fd;
   while (1)
   {
      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);
      int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);

      /**
       * NOTE: pgagroal should deal with accept returning -1
       */
      if (client_fd == -1)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK)
         {
            w->client_fd = -1;
            w->cb(ev, w, errno);
            ret = EV_CLOSE_FD;
            break;
         }
         else
         {
            fprintf(stderr, "accept\n");
            break;
         }
      }

      w->client_fd = client_fd;
      w->cb(ev, w, 0);

   }

   return ret;
}

int
_receive_handler(struct ev_loop* ev, struct ev_io* w)
{
   int ret = EV_OK;
   int nrecv = 0;
   int total_recv = 0;
   int capacity = ev->capacity;
   int fd = w->fd;
   void* buf = ev->buffer;
   if (!buf)
   {
      perror("malloc error");
      return EV_ALLOC_ERROR;
   }

   /**
    * TODO: Implement capacity shrinking?
    */
   while (1)
   {
      nrecv = recv(fd, buf, capacity, 0);
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
         ret = EV_CLOSE_FD;
         goto clean;
      }
      total_recv += nrecv;
      if (total_recv == capacity && capacity < MAX_BUFFER_SIZE)
      {
         int new_capacity = capacity * 2;
         if (new_capacity > MAX_BUFFER_SIZE)
         {
            new_capacity = MAX_BUFFER_SIZE;
         }
         char* new_buf = realloc(buf, new_capacity);
         if (!new_buf)
         {
            perror("Failed to reallocate memory");
            ret = EV_ALLOC_ERROR;
            goto clean;
         }
         buf = new_buf;
         capacity = new_capacity;
      }

      if (capacity == MAX_BUFFER_SIZE && total_recv == capacity)
      {
         break;
      }
   }

   ev->capacity = capacity;

   w->data = buf;
   w->size = total_recv;
   w->cb(ev, w, 0);

clean:

   free(buf);
   return ret;
}

int
_send_handler(struct ev_loop* ev, struct ev_io* w)
{
   int ret = EV_OK;
   ssize_t nsent;
   size_t total_sent = 0;
   int fd = w->fd;
   void* buf = w->data;
   size_t buf_len = w->size;

   while (total_sent < buf_len)
   {
      nsent = send(fd, buf + total_sent, buf_len - total_sent, 0);
      if (nsent == -1)
      {
         if (errno != EAGAIN && errno != EWOULDBLOCK)
         {
            perror("send");
            ret = EV_ERROR;
            break;
         }
         else if (errno == EPIPE)
         {
            ret = EV_CLOSE_FD;
         }
      }
      else
      {
         total_sent += nsent;
      }
   }

   /*
    * NOTE: Maybe there is an advantage in rearming here since the loop uses non blocking sockets.
    *       But I don't know the case where error occurred and exited the loop and can be recovered.
    *
    *       Example:
    *       if (total_sent < buf_len)
    *            pgagroal_io_send_init(w, fd, cb, buf + total_sent, buf_len, 0);
    */

   return ret;
}

int
set_non_blocking(int fd)
{
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags == -1)
   {
      return -1;
   }
   return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#endif

//
// int
// pgagroal_ev_io_init(struct ev_io* w, int fd, int event, io_cb cb, void* data, int size, int slot)
// {
//    if (event >= IO_EVENTS_NR)
//    {
//       fprintf(stderr, "io_init: invalid event flag number: %d\n", event);
//       return 1;
//    }
//
//    w->slot = slot;
//    w->fd = fd;
//    w->cb = cb;
//    w->data = data;
//    w->size = size;
//
//    return EV_OK;
// }
//
// int
// pgagroal_ev_io_stop(struct ev_loop* ev, struct ev_io* target)
// {
//    int ret = EV_OK;
//    struct io_uring_sqe* sqe;
//    struct ev_io** w;
//    if (!target)
//    {
//       fprintf(stderr, "err: NULL pointer provided to stop\n");
//       return EV_ERROR;
//    }
//    list_delete(w, &ev->ihead.next, target, ret);
//    sqe = io_uring_get_sqe(&ev->ring);
//    io_uring_prep_cancel64(sqe, (uint64_t)target, 0); /* TODO: flags? */
//
//    return ret;
// }
//
// int
// pgagroal_ev_signal_stop(struct ev_loop* ev, struct ev_signal* target)
// {
//    int ret = EV_OK;
//    struct ev_signal** w;
//    if (!target)
//    {
//       fprintf(stderr, "err: NULL pointer provided to stop\n");
//       return EV_ERROR;
//    }
//
//    list_delete(w, &ev->shead.next, target, ret);
//    sigdelset(&ev->sigset, target->signum);
//    if (sigprocmask(SIG_UNBLOCK, &ev->sigset, NULL) == -1)
//    {
//       fprintf(stderr, "Error: sigprocmask\n");
//       return EV_ERROR;
//    }
//
//    return ret;
// }
//
// int
// pgagroal_ev_periodic_stop(struct ev_loop* ev, struct ev_periodic* target)
// {
//    int ret = EV_OK;
//    struct io_uring_sqe* sqe;
//    struct ev_periodic** w;
//    if (!target)
//    {
//       fprintf(stderr, "err: NULL pointer provided to stop\n");
//       return EV_ERROR;
//    }
//    list_delete(w, &ev->phead.next, target, ret);
//
//    sqe = io_uring_get_sqe(&ev->ring);
//    io_uring_prep_cancel64(sqe, (uint64_t)target, 0); /* TODO: flags? */
//
//    return ret;
// }
//
// int
// pgagroal_ev_io_accept_init(struct ev_io* w, int fd, io_cb cb)
// {
//    return pgagroal_ev_io_init(w, fd, EV_ACCEPT, cb, NULL, 0, -1);
// }
//
// int
// pgagroal_ev_io_read_init(struct ev_io* w, int fd, io_cb cb)
// {
//    return pgagroal_ev_io_init(w, fd, READ, cb, NULL, 0, -1);
// }
//
// int
// pgagroal_io_send_init(struct ev_io* w, int fd, io_cb cb, void* buf, int buf_len, int bid)
// {
//    return pgagroal_ev_io_init(w, fd, EV_SEND, cb, buf, buf_len, bid);
// }
//
// int
// pgagroal_io_receive_init(struct ev_io* w, int fd, io_cb cb)
// {
//    return pgagroal_ev_io_init(w, fd, EV_RECEIVE, cb, NULL, 0, -1);
// }
//
// int
// pgagroal_io_connect_init(struct ev_io* w, int fd, io_cb cb, union sockaddr_u* addr)
// {
//    return pgagroal_ev_io_init(w, fd, CONNECT, cb, (void*)addr, 0, -1);
// }

// int
// _io_table_insert(struct ev_io* ev,int fd,io_cb cb,int event)
// {
//    int i;
//    const int io_table_size = sizeof(ev->io_table) / sizeof(struct io_entry);
//
//    /* if fd is already registered, add cb to fd entry */
//    for (i = 0; i < io_table_size; i++)
//    {
//       if (ev->io_table[i].fd == EMPTY)
//       {
//          break; /* stop looking once reach unregistered entries */
//       }
//       if (ev->io_table[i].fd == fd)
//       {
//          ev->io_table[i].fd = fd;
//          ev->io_table[i].cbs[event] = cb;
//
//          return i;
//       }
//    }
//
//    if (ev->io_count >= io_table_size)
//    {
//       fprintf(stderr,"periodic_table_insert: ev->periodic_count >= periodic_table_size\n");
//       return -1;
//    }
//
//    i = ev->io_count++;
//
//    ev->io_table[i].fd = fd;
//    ev->io_table[i].cbs[event] = cb;
//
//    return i;
// }
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
//int
//_signal_table_insert(struct ev_io* ev,int signum,int slot,signal_cb cb)
//{
//   int ret = EV_OK;
//   int sig;
//
//   switch (signum)
//   {
//      case SIGTERM:
//         sig = EV_SIGTERM;
//         break;
//      case SIGHUP:
//         sig = _SIGHUP;
//         break;
//      case SIGINT:
//         sig = _SIGINT;
//         break;
//      case SIGTRAP:
//         sig = _SIGTRAP;
//         break;
//      case SIGABRT:
//         sig = _SIGABRT;
//         break;
//      case SIGALRM:
//         sig = _SIGALRM;
//         break;
//      default:
//         fprintf(stderr,"signal not supported\n");
//         return EV_ERROR;
//   }
// ev->sig_table[sig].cb = cb;
// ev->sig_table[sig].signum = signum;
// ev->sig_table[sig].slot = slot;
//
//   return ret;
//
//   /* TODO: this code is kept unreachable because it is currently not possible to have the following
//    *  implementation, based on signalfd
//    *
//    *      const int signal_table_size = sizeof(ev->sig_table) / sizeof(struct signal_entry);
//    *      if (ev->signal_count >= signal_table_size)
//    *      {
//    *         fprintf(stderr,"signal_table_insert: ev->signal_count >= signal_table_size\n");
//    *         return -1;
//    *      }
//    *      i = ev->signal_count++;
//    *      ev->sig_table[i].signum = signum;
//    *      ev->sig_table[i].cb = cb;
//    *      return i;
//    */
//}
// int
// _periodic_table_insert(struct ev_io* ev,struct __kernel_timespec ts,periodic_cb cb)
// {
//    int i;
//    const int periodic_table_size = sizeof(ev->per_table) / sizeof(struct periodic_entry);
//
//    if (ev->periodic_count >= periodic_table_size)
//    {
//       fprintf(stderr,"periodic_table_insert: ev->periodic_count >= periodic_table_size\n");
//       return 1;
//    }
//
//    i = ev->periodic_count++;
//
//    ev->per_table[i].ts.tv_sec = ts.tv_sec;
//    ev->per_table[i].ts.tv_nsec = ts.tv_nsec;
//    ev->per_table[i].cb = cb;
//
//    return i;
// }
// int
// _prepare_send(struct io_loop* ev,int fd,void* buf,size_t size,int t_index)
// {
//    struct io_uring_sqe* sqe = _get_sqe(ev);
//    io_uring_prep_send(sqe,fd,buf,size,MSG_WAITALL | MSG_NOSIGNAL);
//    _encode_user_data(sqe,SEND,ev->id,0,fd,t_index);
//    return EV_OK;
// }
// int
// _socket_handler(struct ev_io* ev,struct io_uring_cqe* cqe,void** buf,int* bid)
// {
//    return EV_OK;
// }
// int
// _connect_handler(struct ev_io* ev,struct io_uring_cqe* cqe)
// {
//    int ret = EV_OK;
//    struct user_data ud = _decode_user_data(cqe);
//    io_cb cb = ev->io_table[ud.ind].cbs[CONNECT];
//
//    _ev_set_out(ev, NULL, 0, ud.fd, 0);
//    cb(ev, 0);
//
//    return ret;
// }
// void
// _ev_set_out(struct ev_io* ev, void *data, int size, int fd, int client_fd)
// {
//         ev->out.data = data;
//         ev->out.size = size;
//         ev->out.fd = fd;
//         ev->out.client_fd = client_fd;
// }
//
//
//  void
// _encode_user_data(struct io_uring_sqe* sqe, uint8_t event, uint16_t id, uint8_t bid, uint16_t fd, uint16_t ind)
// {
//    struct user_data ud = {
//       .event = event,
//       .id = id,
//       .bid = bid,
//       .fd = fd,
//       .ind = ind,
//    };
//    io_uring_sqe_set_data64(sqe, ud.as_u64);
// }
//
// struct user_data
// _decode_user_data(struct io_uring_cqe* cqe)
// {
//    struct user_data ud = { .as_u64 = cqe->user_data };
//    return ud;
// }
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
