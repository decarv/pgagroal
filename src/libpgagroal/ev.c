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

#if DEBUG
#define HAVE_EPOLL 1
#define HAVE_URING 1
#endif

/* pgagroal */
#include <ev.h>
#include <pgagroal.h>
#include <logging.h>
#include <shmem.h>

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
#if HAVE_URING
#include <liburing.h>
#include <netdb.h>
#include <sys/eventfd.h>
#endif
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <netinet/in.h>

#define FALLBACK_BACKEND "epoll"
#define TYPEOF(watcher) watcher->io->type

#define for_each(w, first) for (w = first; w; w = w->next)

#define list_add(w, first)    \
        do {                  \
           w->next = first;   \
           first = w;         \
        } while (0)           \

#define list_delete(w, first, target, ret)                                      \
        do {                                                                    \
           for (w = first; *w && *w != target; w = &(*w)->next);                \
           if (!(*w)) {                                                         \
              pgagroal_log_warn("%s: target watcher not found\n", __func__);    \
              ret = EV_ERROR;                                                   \
           } else {                                                             \
              if (!target->next) {                                              \
                 *w = NULL;                                                     \
              } else {                                                          \
                 *w = target->next;                                             \
              }                                                                 \
           }                                                                    \
        } while (0)                                                             \

static int (*loop_init)(struct ev_loop*);
static int (*loop_fork)(struct ev_loop**);
static int (*loop_destroy)(struct ev_loop*);
static int (*loop_start)(struct ev_loop*);
static void (*loop_break)(struct ev_loop*);

static int (*io_start)(struct ev_loop*, struct ev_io*);
static int (*io_stop)(struct ev_loop*, struct ev_io*);
static int io_init(struct ev_io* w, int fd, int event, io_cb cb, void* data, int size, int slot);

static int (*signal_start)(struct ev_loop*, struct ev_signal*);
static int (*signal_stop)(struct ev_loop*, struct ev_signal*);

static int (*periodic_init)(struct ev_periodic*, int);
static int (*periodic_start)(struct ev_loop*, struct ev_periodic*);
static int (*periodic_stop)(struct ev_loop*, struct ev_periodic*);

static bool (*is_running)(struct ev_loop* ev);
static void (*set_running)(struct ev_loop* ev);

static int setup_ops(struct ev_loop*);
static int setup_context(struct ev_context*, struct configuration*);

#if HAVE_URING
static int __io_uring_init(struct ev_loop*);
static int __io_uring_destroy(struct ev_loop*);
static int __io_uring_handler(struct ev_loop*, struct io_uring_cqe*);
static int __io_uring_loop(struct ev_loop*);
static int __io_uring_fork(struct ev_loop**);
static int __io_uring_io_start(struct ev_loop*, struct ev_io*);
static int __io_uring_io_stop(struct ev_loop*, struct ev_io*);
static int __io_uring_setup_buffers(struct ev_loop*);
static int __io_uring_periodic_init(struct ev_periodic* w, int msec);
static int __io_uring_periodic_start(struct ev_loop* loop, struct ev_periodic* w);
static int __io_uring_periodic_stop(struct ev_loop* loop, struct ev_periodic* w);
static int __io_uring_signal_handler(struct ev_loop* ev, int signum);
static int __io_uring_signal_start(struct ev_loop* ev, struct ev_signal* w);
static int __io_uring_signal_stop(struct ev_loop* ev, struct ev_signal* w);
static int __io_uring_receive_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe,
                                      void** unused, int* bid, bool is_proxy);
static int __io_uring_send_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe);
static int __io_uring_accept_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe);
static int __io_uring_periodic_handler(struct ev_loop* ev, struct ev_periodic* w);
#endif

#if HAVE_EPOLL
static int __epoll_init(struct ev_loop*);
static int __epoll_destroy(struct ev_loop*);
static int __epoll_handler(struct ev_loop*, void*);
static int __epoll_loop(struct ev_loop*);
static int __epoll_fork(struct ev_loop**);
static int __epoll_io_start(struct ev_loop*, struct ev_io*);
static int __epoll_io_stop(struct ev_loop*, struct ev_io*);
static int __epoll_io_handler(struct ev_loop*, struct ev_io*);
static int __epoll_send_handler(struct ev_loop*, struct ev_io*);
static int __epoll_accept_handler(struct ev_loop*, struct ev_io*);
static int __epoll_receive_handler(struct ev_loop*, struct ev_io*);
static int __epoll_periodic_init(struct ev_periodic*, int);
static int __epoll_periodic_start(struct ev_loop*, struct ev_periodic*);
static int __epoll_periodic_stop(struct ev_loop*, struct ev_periodic*);
static int __epoll_periodic_handler(struct ev_loop*, struct ev_periodic*);
static int __epoll_signal_stop(struct ev_loop*, struct ev_signal*);
static int __epoll_signal_handler(struct ev_loop*);
static int __epoll_signal_start(struct ev_loop*, struct ev_signal*);
inline static int
__epoll_set_non_blocking(int fd)
{
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags == -1)
   {
      return EV_ERROR;
   }
   return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
#endif

static inline bool
__is_running(struct ev_loop* ev)
{
   return ev->running;
}
static inline bool
__is_running_atomic(struct ev_loop* ev)
{
   return atomic_load(&ev->atomic_running);
}

static inline void
__set_running(struct ev_loop* ev)
{
   ev->running = true;
}
static inline void
__set_running_atomic(struct ev_loop* ev)
{
   atomic_store(&ev->atomic_running, true);
}

static inline void
__break(struct ev_loop* loop)
{
   loop->running = false;
}
static inline void
__break_atomic(struct ev_loop* loop)
{
   atomic_store(&loop->atomic_running, false);
}

static enum ev_backend backend_value(char* backend);

void
pgagroal_ev_print_backends(void)
{
   int cnt = 0;
   char log[MISC_LENGTH] = { 0 };
#if HAVE_URING
   strcat(log, "io_uring, ");
   cnt++;
#endif
#if HAVE_EPOLL
   strcat(log, "epoll, ");
   cnt++;
#endif
#if HAVE_KQUEUE
   strcat(log, "kqueue, ");
   cnt++;
#endif
   if (cnt > 0)
   {
      log[strlen(log) - 2] = '\0';
      pgagroal_log_debug("available ev backends: %s", log);
   }
   else
   {
      pgagroal_log_debug("no ev backends available");
   }
}

inline static int
supported_engines(void)
{
   int supported = 0;
#if HAVE_URING
   supported |= EV_BACKEND_IO_URING;
#endif
#if HAVE_EPOLL
   supported |= EV_BACKEND_EPOLL;
#endif
#if HAVE_KQUEUE
   supported |= EV_BACKEND_KQUEUE;
#endif
   return supported;
}

static enum ev_backend
backend_value(char* backend)
{
   int value = 0;
   if (!strcmp(backend, "io_uring"))
   {
      value = EV_BACKEND_IO_URING;
   }
   else if (!strcmp(backend, "epoll"))
   {
      value = EV_BACKEND_EPOLL;
   }
   else if (!strcmp(backend, "kqueue"))
   {
      value = EV_BACKEND_KQUEUE;
   }
   return value;
}

struct ev_loop*
pgagroal_ev_init(struct configuration* config)
{
   int ret = EV_OK;
   struct ev_loop* ev = calloc(1, sizeof(struct ev_loop));

   if (!config)
   {
           struct configuration default_config = { 0 };
           strcpy(default_config.ev_backend, FALLBACK_BACKEND);
           if (!config)
           {
              config = &default_config;
           }
   }
   ev->config = config;

   ret = setup_context(&ev->ctx, config);
   if (ret)
   {
      pgagroal_log_error("ev_backend: context setup error\n");
      goto error;
   }

   /* dummy heads */

   ev->ihead.slot = -1;
   ev->ihead.next = NULL;
   ev->shead.slot = -1;
   ev->shead.next = NULL;
   ev->phead.slot = -1;
   ev->phead.next = NULL;

   ret = setup_ops(ev);
   if (ret)
   {
      pgagroal_log_error("setup_ops: setup error\n");
      goto error;
   }

   /* init */

   sigemptyset(&ev->sigset);

   ret = loop_init(ev);
   if (ret)
   {
      pgagroal_log_error("loop init error");
      goto error;
   }
   pgagroal_log_trace("loop init ok");
   return ev;

error:
   free(ev);
   return NULL;
}

int
pgagroal_ev_loop(struct ev_loop* loop)
{
   return loop_start(loop);
}

int
pgagroal_ev_loop_fork(struct ev_loop** loop)
{
   loop_fork(loop);

   return EV_OK;
}

int
pgagroal_ev_loop_destroy(struct ev_loop* ev)
{
   sigemptyset(&ev->sigset);
   return loop_destroy(ev);
}

void
pgagroal_ev_loop_break(struct ev_loop* ev)
{
   loop_break(ev);
}

int
pgagroal_ev_io_accept_init(struct ev_io* w, int fd, io_cb cb)
{
   return io_init(w, fd, EV_ACCEPT, cb, NULL, 0, -1);
}

int
pgagroal_ev_io_read_init(struct ev_io* w, int fd, io_cb cb)
{
   return io_init(w, fd, READ, cb, NULL, 0, -1);
}

int
pgagroal_ev_io_send_init(struct ev_io* w, int fd, io_cb cb, void* buf, int buf_len, int bid)
{
   return io_init(w, fd, EV_SEND, cb, buf, buf_len, bid);
}

int
pgagroal_ev_io_receive_init(struct ev_io* w, int fd, io_cb cb)
{
   return io_init(w, fd, EV_RECEIVE, cb, NULL, 0, -1);
}

int
pgagroal_ev_io_connect_init(struct ev_io* w, int fd, io_cb cb, union sockaddr_u* addr)
{
   return io_init(w, fd, CONNECT, cb, (void*)addr, 0, -1);
}

int
pgagroal_ev_io_start(struct ev_loop* ev, struct ev_io* w)
{
   list_add(w, ev->ihead.next);
   return io_start(ev, w);
}

int
pgagroal_ev_io_stop(struct ev_loop* ev, struct ev_io* target)
{
   int ret = EV_OK;
   struct ev_io** w;
   if (!target)
   {
      pgagroal_log_fatal("impossible situation: null pointer provided to stop\n");
   }
   io_stop(ev, target);
   list_delete(w, &ev->ihead.next, target, ret);
   /* pgagroal likes to deal with this: close(target->fd); */
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
      pgagroal_log_error("%s: sigprocmask failed\n", __func__);
      return EV_ERROR;
   }
   signal_start(ev, w);
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
      pgagroal_log_error("NULL pointer provided to stop\n");
      return EV_ERROR;
   }

   sigdelset(&ev->sigset, target->signum);
   if (sigprocmask(SIG_UNBLOCK, &ev->sigset, NULL) == -1)
   {
      pgagroal_log_error("Error: sigprocmask\n");
      return EV_ERROR;
   }
   signal_stop(ev, target);
   list_delete(w, &ev->shead.next, target, ret);
   return ret;
}

int
pgagroal_ev_periodic_init(struct ev_periodic* w, periodic_cb cb, int msec)
{
   if (periodic_init(w, msec))
   {
      pgagroal_log_fatal("%s: __periodic_init failed", __func__);
   }
   w->type = EV_PERIODIC;
   w->slot = -1;
   w->cb = cb;
   w->next = NULL;
   return EV_OK;
}

int
pgagroal_ev_periodic_start(struct ev_loop* loop, struct ev_periodic* w)
{
   periodic_start(loop, w);
   list_add(w, loop->phead.next);
   return EV_OK;
}

int
pgagroal_ev_periodic_stop(struct ev_loop* ev, struct ev_periodic* target)
{
   int ret = EV_OK;
   struct ev_periodic** w;
   if (!target)
   {
      pgagroal_log_error("null pointer provided to stop\n");
      return EV_ERROR;
   }
   ret = periodic_stop(ev, target);
   list_delete(w, &ev->phead.next, target, ret);
   return ret;
}

/*
 * TODO: when developing config, make sure that the user has the backend that she claims to have.
 * When it gets to this point, checks will not be made any longer, unless inside specific __ev_function.
 * These functions will contain preprocessor checks so that the compilation does not break...
 *
 * int (*init)(struct ev_loop *);
 * int (*loop)(struct ev_loop *);
 * int (*io_start)(struct ev_loop*, struct ev_io*);
 * int (*io_stop)(struct ev_loop*, struct ev_io*);
 * int (*signal_init)(struct ev_loop*, struct ev_signal*);
 * int (*signal_start)(struct ev_loop*, struct ev_signal*);
 * int (*signal_stop)(struct ev_loop*, struct ev_signal*);
 * int (*periodic_init)(struct ev_loop*, struct ev_periodic*);
 * int (*periodic_start)(struct ev_loop*, struct ev_periodic*);
 * int (*periodic_stop)(struct ev_loop*, struct ev_periodic*);
 *
 */
static int
setup_ops(struct ev_loop* ev)
{
   int ret = EV_OK;
   bool mtt = ev->ctx.multithreading;

   is_running = mtt ? __is_running_atomic : __is_running;
   set_running = mtt ? __set_running_atomic: __set_running;
   loop_break = mtt ? __break_atomic: __break;

   if (ev->ctx.backend == EV_BACKEND_IO_URING)
   {
#if HAVE_URING
      loop_init = __io_uring_init;
      loop_fork = __io_uring_fork;
      loop_destroy = __io_uring_destroy;
      loop_start = __io_uring_loop;
      io_start = __io_uring_io_start;
      io_stop = __io_uring_io_stop;
      periodic_init = __io_uring_periodic_init;
      periodic_start = __io_uring_periodic_start;
      periodic_stop = __io_uring_periodic_stop;
      signal_start = __io_uring_signal_start;
      signal_stop = __io_uring_signal_stop;
#endif
   }
   else if (ev->ctx.backend == EV_BACKEND_EPOLL)
   {
#if HAVE_EPOLL
      loop_init = __epoll_init;
      loop_fork = __epoll_fork;
      loop_destroy = __epoll_destroy;
      loop_start = __epoll_loop;
      io_start = __epoll_io_start;
      io_stop = __epoll_io_stop;
      periodic_init = __epoll_periodic_init;
      periodic_start = __epoll_periodic_start;
      periodic_stop = __epoll_periodic_stop;
      signal_start = __epoll_signal_start;
      signal_stop = __epoll_signal_stop;
#endif
   }
   else if (ev->ctx.backend == EV_BACKEND_KQUEUE)
   {
      pgagroal_log_fatal("no support for kqueue yet!");
   }

   return ret;
}

static int
setup_context(struct ev_context* ctx, struct configuration* config)
{
   /*
    * TODO: should be an option.
    */
   ctx->multithreading = false;

   /* set backend */

   pgagroal_ev_print_backends();

   if (!strlen(config->ev_backend))
   {
      pgagroal_log_warn("ev_backend not set in configuration file. Selected default: '%s'", FALLBACK_BACKEND);
      strcpy(config->ev_backend, FALLBACK_BACKEND);
   }

   ctx->backend = backend_value(config->ev_backend);
   if (!ctx->backend)
   {
      pgagroal_log_warn("ev_backend '%s' not supported. Selected default: '%s'", config->ev_backend, FALLBACK_BACKEND);
      strcpy(config->ev_backend, FALLBACK_BACKEND);
      ctx->backend = EV_BACKEND_EPOLL;
   }
   if (ctx->backend == EV_BACKEND_IO_URING && config->tls)
   {
      pgagroal_log_warn("ev_backend '%s' not supported with tls on. Selected default: '%s'", config->ev_backend, FALLBACK_BACKEND);
      strcpy(config->ev_backend, FALLBACK_BACKEND);
      ctx->backend = EV_BACKEND_EPOLL;
   }

   if (!(ctx->backend & supported_engines()))
   {
      pgagroal_log_fatal("backend '%s' not supported by your system", config->ev_backend);
   }

   pgagroal_log_debug("backend '%s' selected", config->ev_backend);

   if (ctx->backend == EV_BACKEND_IO_URING)
   {
#if HAVE_URING
        /*
         * TODO: there is a whole lot of settings that could go here.
         *       see pgagroal_configuration below
         */

      /* set opts */
      // ctx->napi = config.napi;
      // ctx->sqpoll = config.sqpoll;
      // ctx->use_huge = config.use_huge;
      // ctx->defer_tw = config.defer_tw;
      // ctx->snd_ring = config.snd_ring;
      // ctx->snd_bundle = config.snd_bundle;
      // ctx->fixed_files = config.fixed_files;
      // ctx->no_use_buffers = config.no_use_buffers;
      // ctx->buf_count = config.buf_count;

      /* asserts */
      if (ctx->defer_tw && ctx->sqpoll)
      {
         pgagroal_log_fatal("cannot use DEFER_TW and SQPOLL at the same time\n");
         exit(EXIT_FAILURE);
      }

      /* TODO: this is not supposed to be like this.
       * The client is supposed to set this.
       * This is here temporarily.
       */
      ctx->entries = 8;
      ctx->params.cq_entries = 32;
      ctx->params.flags = 0;
      ctx->params.flags |= IORING_SETUP_SINGLE_ISSUER;      /* TODO: makes sense for pgagroal? */
      ctx->params.flags |= IORING_SETUP_CLAMP;
      ctx->params.flags |= IORING_SETUP_CQSIZE;
      ctx->params.flags |= IORING_SETUP_DEFER_TASKRUN;

      /* default configuration */

      if (ctx->defer_tw)
      {
         ctx->params.flags |= IORING_SETUP_DEFER_TASKRUN;      /* overwritten by SQPOLL */
      }
      if (ctx->sqpoll)
      {
         ctx->params.flags |= IORING_SETUP_SQPOLL;
         // ctx->params.sq_thread_idle = config.params.sq_thread_idle;
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
         pgagroal_log_fatal("no support for fixed files\n");      /*TODO: add support for fixed files */
         exit(EXIT_FAILURE);
      }
#endif
   }
   else if (ctx->backend == EV_BACKEND_EPOLL)
   {
      // ctx->epoll_flags = orig.epoll_flags;
   }
   else if (ctx->backend == EV_BACKEND_KQUEUE)
   {

   }

   return EV_OK;
}

static int
io_init(struct ev_io* w, int fd, int event, io_cb cb, void* data, int size, int slot)
{
   if (event >= IO_EVENTS_NR)
   {
      pgagroal_log_fatal("%s: invalid event flag number: %d\n", __func__, event);
   }
   w->type = event;
   w->slot = slot;
   w->fd = fd;
   w->cb = cb;
   w->data = data;
   w->size = size;
   return EV_OK;
}

#if HAVE_URING
static inline struct io_uring_sqe*
__io_uring_get_sqe(struct ev_loop* ev)
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

static inline int
__io_uring_rearm_receive(struct ev_loop* ev, struct ev_io* w)
{
   struct io_uring_sqe* sqe = __io_uring_get_sqe(ev);
   io_uring_prep_recv_multishot(sqe, w->fd, NULL, 0, 0);
   io_uring_sqe_set_data(sqe, w);
   sqe->flags |= IOSQE_BUFFER_SELECT;
   sqe->buf_group = 0;
   return EV_OK;
}

static inline int
__io_uring_replenish_buffers(struct ev_loop* ev, struct io_buf_ring* br, int bid_start, int bid_end)
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
   return EV_OK;
}

static int
__io_uring_init(struct ev_loop* loop)
{
   int ret = EV_OK;
   ret = io_uring_queue_init_params(loop->ctx.entries, &loop->ring, &loop->ctx.params);  /* on fork: gets a new ring */
   if (ret)
   {
      pgagroal_log_fatal("io_uring_queue_init_params: %s\n", strerror(-ret));
   }
   if (!loop->ctx.no_use_buffers)
   {
      ret = __io_uring_setup_buffers(loop);
      if (ret)
      {
         pgagroal_log_fatal("%s: __io_uring_setup_buffers: %s\n", __func__, strerror(-ret));
      }
   }
   return ret;
}

static int
__io_uring_destroy(struct ev_loop* ev)
{
   /* free buffer rings */
   io_uring_free_buf_ring(&ev->ring, ev->in_br.br, ev->ctx.buf_count, ev->in_br.bgid);
   ev->in_br.br = NULL;
   io_uring_free_buf_ring(&ev->ring, ev->out_br.br, ev->ctx.buf_count, ev->out_br.bgid);
   ev->out_br.br = NULL;
   if (ev->ctx.use_huge)
   {
      /* TODO: munmap(cbr->buf, buf_size * nr_bufs); */
   }
   else
   {
      free(ev->in_br.buf);
      free(ev->out_br.buf);
   }
   io_uring_queue_exit(&ev->ring);
   free(ev);
   return EV_OK;
}

static int
__io_uring_io_start(struct ev_loop* ev, struct ev_io* w)
{
   int domain;
   union sockaddr_u* addr;
   struct io_uring_sqe* sqe = __io_uring_get_sqe(ev);
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
         pgagroal_log_fatal("%s: unknown event type: %d\n", __func__, w->type);
         return EV_ERROR;
   }
   return EV_OK;
}

static int
__io_uring_io_stop(struct ev_loop* ev, struct ev_io* target)
{
   int ret = EV_OK;
   struct io_uring_sqe* sqe;
   sqe = io_uring_get_sqe(&ev->ring);
   io_uring_prep_cancel64(sqe, (uint64_t)target, 0); /* TODO: flags? */
   return ret;
}

static int
__io_uring_signal_start(struct ev_loop* ev, struct ev_signal* w)
{
   return EV_OK;
}

static int
__io_uring_signal_stop(struct ev_loop* ev, struct ev_signal* w)
{
   return EV_OK;
}

static int
__io_uring_periodic_init(struct ev_periodic* w, int msec)
{
   /* TODO: how optimized is designated initializers really */
   w->ts = (struct __kernel_timespec) {
      .tv_sec = msec / 1000,
      .tv_nsec = (msec % 1000) * 1000000
   };
   return EV_OK;
}

static int
__io_uring_periodic_start(struct ev_loop* loop, struct ev_periodic* w)
{
   struct io_uring_sqe* sqe = io_uring_get_sqe(&loop->ring);
   io_uring_sqe_set_data(sqe, w);
   io_uring_prep_timeout(sqe, &w->ts, 0, IORING_TIMEOUT_MULTISHOT);
   return EV_OK;
}

static int
__io_uring_periodic_stop(struct ev_loop* loop, struct ev_periodic* w)
{
   struct io_uring_sqe* sqe;
   sqe = io_uring_get_sqe(&loop->ring);
   io_uring_prep_cancel64(sqe, (uint64_t)w, 0); /* TODO: flags? */
   return EV_OK;
}

/*
 * Based on: https://git.kernel.dk/cgit/liburing/tree/examples/proxy.c
 * (C) 2024 Jens Axboe <axboe@kernel.dk>
 */
static int
__io_uring_loop(struct ev_loop* ev)
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

   set_running(ev);
   while (is_running(ev))
   {
      ts = &idle_ts;
      io_uring_submit_and_wait_timeout(&ev->ring, &cqe, to_wait, ts, NULL);

      /* Good idea to leave here to see what happens */
      if (*ev->ring.cq.koverflow)
      {
         pgagroal_log_error("io_uring overflow %u\n", *ev->ring.cq.koverflow);
         exit(EXIT_FAILURE);
      }
      if (*ev->ring.sq.kflags & IORING_SQ_CQ_OVERFLOW)
      {
         pgagroal_log_error("io_uring overflow\n");
         exit(EXIT_FAILURE);
      }

      /* Check for signals before iterating over cqes */
      signum = sigtimedwait(&ev->sigset, NULL, &timeout);
      if (signum > 0)
      {
         ret = __io_uring_signal_handler(ev, signum);
         if (ret == EV_ERROR)
         {
            pgagroal_log_error("__io_uring_signal_handler_error\n");
            return EV_ERROR;
         }
         if (!is_running(ev))
         {
            break;
         }
      }

      events = 0;
      io_uring_for_each_cqe(&(ev->ring), head, cqe)
      {
         ret = __io_uring_handler(ev, cqe);
         if (ret == EV_ERROR)
         {
            pgagroal_log_error("__io_uring_handler error\n");
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

static int
__io_uring_fork(struct ev_loop** loop)
{
   struct ev_loop* tmp = *loop;
   *loop = pgagroal_ev_init(tmp->config);
   __epoll_destroy(tmp);

   return EV_OK;
}

static int
__io_uring_handler(struct ev_loop* ev, struct io_uring_cqe* cqe)
{
   int ret = EV_OK;
   ev_watcher w;
   w.io = (ev_io*)io_uring_cqe_get_data(cqe);

   int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
   int bid_end = bid;
   void* buf;

   /*
    * Cancelled requests will trigger the handler, but have NULL data.
    */
   if (!w.io)
   {
      return EV_OK;
   }

   /* io handler */
   switch (w.io->type) /* could be w->{periodic,signal} */
   {
      case EV_PERIODIC:
         return __io_uring_periodic_handler(ev, w.periodic);
      case EV_ACCEPT:
         return __io_uring_accept_handler(ev, w.io, cqe);
      case EV_SEND:
         return __io_uring_send_handler(ev, w.io, cqe);
      case EV_RECEIVE:
retry:
         ret = __io_uring_receive_handler(ev, w.io, cqe, &buf, &bid_end, false);
         switch (ret)
         {
            case EV_CLOSE_FD: /* connection closed */
               /* pgagroal deals with closing fd */
               break;
            case EV_REPLENISH_BUFFERS: /* TODO: stress test this: buffers should be replenished after each recv */
               pgagroal_log_warn("__io_uring_receive_handler: request requeued\n");
               usleep(100);
               goto retry;
               break;
         }
         break;
      default:
         pgagroal_log_fatal("%s: _io_handler: event not found eventno=%d", __func__, w.io->type);
   }
   return ret;
}

static int
__io_uring_periodic_handler(struct ev_loop* ev, struct ev_periodic* w)
{
   w->cb(ev, w, 0);
   return EV_OK;
}

static int
__io_uring_accept_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe)
{
   w->client_fd = cqe->res;
   int flags = fcntl(w->client_fd, F_GETFL, 0);
   if (flags == -1)
   {
      perror("fcntl");
      close(w->client_fd);
      return EV_ERROR;
   }
   w->cb(ev, w, 0);
   return EV_OK;
}

static int
__io_uring_send_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe)
{
   int ret;
   int buf_len = cqe->res;
   struct ev_context ctx = ev->ctx;
   int bid_end = (ev->bid + buf_len / ctx.buf_size + (int)(buf_len % ctx.buf_size > 0)) % ctx.buf_count;
   ret = __io_uring_replenish_buffers(ev, &ev->out_br, ev->bid, bid_end);
   if (ret)
   {
      return EV_ERROR;
   }
   return EV_OK;
}

static int
__io_uring_signal_handler(struct ev_loop* ev, int signum)
{
   struct ev_signal* w;
   for (w = ev->shead.next; w && w->signum != signum; w = w->next)
   {
      /* empty */;
   }
   if (!w)
   {
      pgagroal_log_error("no watcher for signal %d\n", signum);
      exit(EXIT_FAILURE);
   }
   w->cb(ev, w, 0);
   return EV_OK;
}

static int
__io_uring_receive_handler(struct ev_loop* ev, struct ev_io* w, struct io_uring_cqe* cqe, void** unused, int* bid, bool is_proxy)
{
   int ret = EV_OK;
   struct ev_context ctx = ev->ctx;
   struct io_buf_ring* in_br = &ev->in_br;
   struct io_buf_ring* out_br = &ev->out_br;
   void* recv_buf_base = (void*) (in_br->buf + *bid * ctx.buf_size);
   struct io_uring_buf* buf;
   void* data;
   int this_bytes;
   int in_bytes;
   int bid_start = *bid;
   int total_in_bytes;

   if (cqe->res == -ENOBUFS)
   {
      pgagroal_log_warn("io_receive_handler: Not enough buffers\n");
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
   }

   /* From the docs: https://man7.org/linux/man-pages/man3/io_uring_prep_recv_multishot.3.html
    * "If a posted CQE does not have the IORING_CQE_F_MORE flag set then the multishot receive will be
    * done and the application should issue a new request."
    */
   if (!(cqe->flags & IORING_CQE_F_MORE))
   {
      pgagroal_log_warn("need to rearm receive: added timeout");
      ret = __io_uring_rearm_receive(ev, w);
      if (ret)
      {
         return EV_ERROR;
      }
   }

   w->data = recv_buf_base;
   w->size = total_in_bytes;
   w->cb(ev, w, ret);

   ret = __io_uring_replenish_buffers(ev, in_br, bid_start, *bid);
   if (ret)
   {
      perror("replenish_buffers");
      return EV_ERROR;
   }

   ev->bid = *bid;

   return EV_OK;
}

static int
__io_uring_setup_buffers(struct ev_loop* ev)
{
   int ret = EV_OK;
   void* ptr;
   struct ev_context ctx = ev->ctx;

   struct io_buf_ring* in_br = &ev->in_br;
   struct io_buf_ring* out_br = &ev->out_br;

   if (ctx.use_huge)
   {
      pgagroal_log_warn("use_huge not implemented yet\n"); /* TODO */
   }
   if (posix_memalign(&in_br->buf, ALIGNMENT, ctx.buf_count * ctx.buf_size))
   {
      pgagroal_log_error("posix_memalign");
      perror("posix_memalign");
   }

   in_br->br = io_uring_setup_buf_ring(&ev->ring, ctx.buf_count, 0, 0, &ret);
   out_br->br = io_uring_setup_buf_ring(&ev->ring, ctx.buf_count, 1, 0, &ret);
   if (!in_br->br || !out_br->br)
   {
      pgagroal_log_fatal("buffer ring register failed %d\n", ret);
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

   // TODO: add functionality ev->next_out_bid = 0;
   return ret;
}

void
_next_bid(struct ev_loop* ev, int* bid)
{
   struct ev_context ctx = ev->ctx;
   *bid = (*bid + 1) % ctx.buf_count;
}
#endif

/*********************************************************************************
*                                                                               *
*                                     EPOLL                                     *
*                                                                               *
*********************************************************************************/

#if HAVE_EPOLL
int
__epoll_loop(struct ev_loop* ev)
{
   int ret;
   struct epoll_event event;
   struct epoll_event events[MAX_EVENTS];
   set_running(ev);
   while (is_running(ev))
   {
      int nfds = epoll_wait(ev->epollfd, events, MAX_EVENTS, 10); /* TODO epoll_waitp */
      if (nfds == -1)
      {
         perror("epoll_wait");
         return EV_ERROR;
      }

      if (!is_running(ev))
      {
         break;
      }
      for (int i = 0; i < nfds; i++)
      {
         event = events[i];
         if (event.data.fd == ev->signalfd)
         {
            ret = __epoll_signal_handler(ev);
         }
         else
         {
            ret = __epoll_handler(ev, (void*)events[i].data.u64);

         }
         if (ret == EV_ERROR)
         {
            perror("_ev_handler\n");
            return EV_ERROR;
         }
      }
   }
   return EV_OK;
}

static int
__epoll_init(struct ev_loop* ev)
{
   struct epoll_event event;

   ev->buffer = malloc(sizeof(char) * (MAX_BUFFER_SIZE));
   ev->capacity = MAX_BUFFER_SIZE;

   ev->epollfd = epoll_create1(ev->ctx.epoll_flags);
   if (ev->epollfd == -1)
   {
      pgagroal_log_error("epoll init error");
      return EV_ERROR;
   }

   /* signals use sig_table */
   ev->signalfd = signalfd(-1, &ev->sigset, SFD_NONBLOCK);
   if (ev->signalfd == -1)
   {
      pgagroal_log_error("signalfd init error");
      return EV_ERROR;
   }

   event.data.fd = ev->signalfd;
   event.events = EPOLLIN | EPOLLET;

   if (epoll_ctl(ev->epollfd, EPOLL_CTL_ADD, ev->signalfd, &event) == -1)
   {
      pgagroal_log_error("epoll_ctl");
      return EV_ERROR;
   }
   return EV_OK;
}

static int
__epoll_fork(struct ev_loop** loop)
{
   struct ev_loop* tmp = *loop;
   *loop = pgagroal_ev_init(tmp->config);
   __epoll_destroy(tmp);

   return EV_OK;
}

static int
__epoll_destroy(struct ev_loop* ev)
{
   close(ev->epollfd);
   free(ev);
   return EV_OK;
}

static int
__epoll_handler(struct ev_loop* ev, void* wp)
{
   struct ev_periodic* w = (struct ev_periodic*)wp;
   if (w->type == EV_PERIODIC)
   {
      return __epoll_periodic_handler(ev, (struct ev_periodic*)w);
   }
   return __epoll_io_handler(ev, (struct ev_io*)w);
}

static int
__epoll_signal_start(struct ev_loop* ev, struct ev_signal* w)
{
   ev->signalfd = signalfd(ev->signalfd, &ev->sigset, SFD_NONBLOCK);
   return EV_OK;
}

static int
__epoll_signal_stop(struct ev_loop* ev, struct ev_signal* w)
{
   return signalfd(ev->signalfd, &ev->sigset, SFD_NONBLOCK);
}

static int
__epoll_signal_handler(struct ev_loop* ev)
{
   int ret;
   struct signalfd_siginfo info;
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

static int
__epoll_periodic_init(struct ev_periodic* w, int msec)
{
   struct timespec now;
   struct itimerspec new_value;

   /*
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
   return EV_OK;
}

static int
__epoll_periodic_start(struct ev_loop* loop, struct ev_periodic* w)
{
   struct epoll_event event;
   event.events = EPOLLIN | EPOLLET;
   event.data.u64 = (uint64_t)w;
   if (epoll_ctl(loop->epollfd, EPOLL_CTL_ADD, w->fd, &event) == -1)
   {
      perror("epoll_ctl");
      close(w->fd);
      return EV_ERROR;
   }
   return EV_OK;
}

static int
__epoll_periodic_stop(struct ev_loop* loop, struct ev_periodic* w)
{
   if (epoll_ctl(loop->epollfd, EPOLL_CTL_DEL, w->fd, NULL) == -1)
   {
      pgagroal_log_error("%s: epoll_ctl: delete failed", __func__);
      return EV_ERROR;
   }
   return EV_OK;
}

static int
__epoll_periodic_handler(struct ev_loop* ev, struct ev_periodic* w)
{
   uint64_t exp;
   int nread = read(w->fd, &exp, sizeof(uint64_t));
   if (nread != sizeof(uint64_t))
   {
      pgagroal_log_error("periodic_handler: read");
      return EV_ERROR;
   }
   w->cb(ev, w, 0);
   return EV_OK;
}

static int
__epoll_io_start(struct ev_loop* ev, struct ev_io* w)
{
   struct epoll_event event;
   switch (w->type)
   {
      case EV_ACCEPT:
      case EV_RECEIVE:
         event.events = EPOLLIN | EPOLLET;
         break;
      case EV_SEND:
         event.events = EPOLLOUT | EPOLLET;
         break;
      default:
         pgagroal_log_fatal("%s: unknown event type: %d\n", __func__, w->type);
         return EV_ERROR;
   }
   __epoll_set_non_blocking(w->fd); /* TODO: err handling */
   event.data.u64 = (uint64_t)w;

   if (epoll_ctl(ev->epollfd, EPOLL_CTL_ADD, w->fd, &event) == -1)
   {
      perror("epoll_ctl");
      close(w->fd);
      return EV_ERROR;
   }
   return EV_OK;
}

static int
__epoll_io_stop(struct ev_loop* ev, struct ev_io* target)
{
   int ret = EV_OK;

   /* pgagroal likes to deal with closing fds, so dealing with EPOLL_CTL_DEL is unnecessary */
#if 0
   bool fd_is_open = fcntl(target->fd, F_GETFD) != -1 || errno != EBADF;
   if (fd_is_open)
   {
      printf("File descriptor %d is open.\n", fd);
   }
   else
   {
      printf("File descriptor %d is not open.\n", fd);
   }
   if (epoll_ctl(ev->epollfd, EPOLL_CTL_DEL, target->fd, NULL) == -1)
   {
      pgagroal_log_error("%s: epoll_ctl: delete failed: fd=%d", __func__, target->fd);
      perror("epoll_ctl: ");
      ret = EV_ERROR;
   }
#endif

   return ret;
}

static int
__epoll_io_handler(struct ev_loop* ev, struct ev_io* w)
{
   int ret = EV_OK;
   switch (w->type)
   {
      case EV_ACCEPT:
         return __epoll_accept_handler(ev, w);
      case EV_SEND:
         return __epoll_send_handler(ev, w);
      case EV_RECEIVE:
         switch (__epoll_receive_handler(ev, w))
         {
            case EV_CLOSE_FD: /* connection closed */
               /* pgagroal deals with closing fd */
               break;
         }
         break;
      default:
         pgagroal_log_fatal("%s: unknown value for event type %d\n", __func__);
   }

   return ret;
}

static int
__epoll_receive_handler(struct ev_loop* ev, struct ev_io* w)
{
   int ret = EV_OK;
   int nrecv = 0;
   int total_recv = 0;
   int capacity = ev->capacity;
   void* buf = ev->buffer;
   if (!buf)
   {
      perror("malloc error");
      return EV_ALLOC_ERROR;
   }

   if (!w->ssl)
   {
           while (1)
           {
              nrecv = recv(w->fd, buf + total_recv, capacity, 0);
              if (nrecv == -1)
              {
                 if (errno != EAGAIN && errno != EWOULDBLOCK)
                 {
                    pgagroal_log_error("receive_handler: recv\n");
                 }
                 break;
              }
              else if (nrecv == 0) /* connection closed */
              {
                 ret = EV_CLOSE_FD;
                 pgagroal_log_debug("Connection closed fd=%d client_fd=%d\n", w->fd, w->client_fd);
                 break;
              }

              total_recv += nrecv;
#if 0
              if (total_recv == capacity && capacity < MAX_BUFFER_SIZE) /* resize buffer */
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
                 }
                 buf = new_buf;
                 capacity = new_capacity;
              }

              if (capacity >= MAX_BUFFER_SIZE && total_recv >= capacity)
              {
                 break;
              }
              ev->capacity = capacity;
#endif
           }

   w->data = buf;
   w->size = total_recv;

   }

   w->cb(ev, w, ret);
   return ret;
}

static int
__epoll_accept_handler(struct ev_loop* ev, struct ev_io* w)
{
   int ret = EV_OK;
   int listen_fd = w->fd;
   int client_fd;
   struct sockaddr_in client_addr;
   socklen_t client_len = sizeof(client_addr);
   client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);

   /*
    * NOTE: pgagroal deals with accept returning -1
    */
   if (client_fd == -1)
   {
      ret = EV_ERROR;
   }
   w->client_fd = client_fd;
   w->cb(ev, w, ret);

   return ret;
}

static int
__epoll_send_handler(struct ev_loop* ev, struct ev_io* w)
{
   int ret = EV_OK;
   ssize_t nsent;
   size_t total_sent = 0;
   int fd = w->fd;
   void* buf = w->data;
   size_t buf_len = w->size;

   if (!w->ssl)
   {
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
#endif
