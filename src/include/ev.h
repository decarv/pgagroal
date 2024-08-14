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

#ifndef EV_H
#define EV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pgagroal.h>

/* system */
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <netdb.h>
#include <err.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/signalfd.h>

#if HAVE_URING
#include <liburing.h>
#else
#include <sys/epoll.h>
#endif

#define ALIGNMENT sysconf(_SC_PAGESIZE)  /* TODO: should be used for huge_pages */
#define BUFFER_SIZE 65535

/**
 * NOTE: You may try to decrease this value, but it may fail tests because
 *       there is no good strategy for replenishing buffers yet. */
#define BUFFER_COUNT 8

#define INITIAL_BUFFER_SIZE 65535
#ifndef MAX_BUFFER_SIZE
#define MAX_BUFFER_SIZE 65535
#endif

#define MAX_EVENTS 128

/* TODO(hc): cleanup unused enums */
enum ev_type {
   EV_ACCEPT    = 0,
   EV_RECEIVE   = 1,
   EV_SEND      = 2,
   CONNECT      = 3,
   SOCKET       = 4,
   READ         = 5,
   WRITE        = 6,
   IO_EVENTS_NR = 7,
   EV_SIGNAL    = 8,
   EV_PERIODIC  = 9,
   EVENTS_NR    = 10,
};

/**
 * TODO(hc): Improve error handling for pgagroal_ev
 */
enum ev_return_codes {
   EV_OK = 0,
   EV_ERROR = 1,
   EV_CLOSE_FD,
   EV_REPLENISH_BUFFERS,
   EV_REARMED,
   EV_ALLOC_ERROR,
};

enum ev_backend {
   EV_BACKEND_IO_URING = (1 << 1),
   EV_BACKEND_EPOLL    = (1 << 2),
   EV_BACKEND_KQUEUE   = (1 << 3),
};

union sockaddr_u
{
   struct sockaddr_in addr4;
   struct sockaddr_in6 addr6;
};

/**
 * @struct ev_context
 * @brief Context for the event handling subsystem.
 *
 * This structure is used to configure and manage state for event handling, the
 * same struct is valid for any backend. If the backend does not use one flag,
 * the library will just ignore it.
 *
 * TODO:
 *      * The context for the backend still has to be fully implemented. Currently
 *        the library does not support different flags settings.
 *
 */
struct ev_context
{
   /* filled in by the user */
   int epoll_flags;
   int entries;
   bool napi;
   bool sqpoll;
   bool use_huge;
   bool defer_tw;
   bool snd_ring;
   bool snd_bundle;
   bool fixed_files;
   bool ipv6;

   enum ev_backend backend;

   bool multithreading;

   bool no_use_buffers;  /* ring mapped buffers */
   int buf_size;         /* ring mapped buffers */
   int buf_count;        /* ring mapped buffers */

   /* filled in by the library */
   int br_mask;

#if HAVE_URING
   struct io_uring_params params;
#endif
};

struct ev_loop;

typedef struct ev_io
{
   enum ev_type type;
   int slot;
   int fd;
   int client_fd;
   void* data;
   int size;
   void (*cb)(struct ev_loop*, struct ev_io* watcher, int err);
   struct ev_io* next;
   bool ssl;
} ev_io;

typedef struct ev_signal
{
   enum ev_type type;
   int slot;
   int signum;
   void (*cb)(struct ev_loop*, struct ev_signal* watcher, int err);
   struct ev_signal* next;
} ev_signal;

typedef struct ev_periodic
{
   enum ev_type type; /* leave this here */
   int ind;
   int slot;
#if HAVE_URING
   struct __kernel_timespec ts;
#endif

   void (*cb)(struct ev_loop*, struct ev_periodic* watcher, int err);
   struct ev_periodic* next;

   int fd; /* TODO: specific to epoll, could be dinamically added: {io_uring,epoll,kqueue}_periodic */

} ev_periodic;

typedef union ev_watcher
{
   struct ev_io* io;
   struct ev_signal* signal;
   struct ev_periodic* periodic;
} ev_watcher;

#if HAVE_URING

struct io_buf_ring
{
   struct io_uring_buf_ring* br;
   void* buf;
   int bgid;
};

#endif

struct ev_ops
{
   int (*init)(struct ev_loop*);
   int (*loop)(struct ev_loop*);
   int (*io_start)(struct ev_loop*, struct ev_io*);
   int (*io_stop)(struct ev_loop*, struct ev_io*);
   int (*signal_init)(struct ev_loop*, struct ev_signal*);      /* TODO IMPLEMENT */
   int (*signal_start)(struct ev_loop*, struct ev_signal*);
   int (*signal_stop)(struct ev_loop*, struct ev_signal*);      /* TODO IMPLEMENT */
   int (*periodic_init)(struct ev_loop*, struct ev_periodic*);      /* TODO IMPLEMENT */
   int (*periodic_start)(struct ev_loop*, struct ev_periodic*);      /* TODO IMPLEMENT */
   int (*periodic_stop)(struct ev_loop*, struct ev_periodic*);      /* TODO IMPLEMENT */
};

/**
 * TODO: this struct could be separated into ev_loop / ev_loop_io_{uring,epoll} so that it
 *       could be dinamically plugged into ev_loop the backend.
 */
struct ev_loop
{
   volatile bool running;
   atomic_bool atomic_running;
   struct ev_context ctx;

   struct ev_io ihead;
   struct ev_signal shead;
   struct ev_periodic phead;

   sigset_t sigset;

   struct ev_ops ops;

   struct configuration* config;

#if HAVE_URING
   struct io_uring_cqe* cqe;

   struct io_uring ring;
   struct io_buf_ring in_br;
   struct io_buf_ring out_br;

   /**
    * TODO: Improve the usage of .bid, .next_out_bid so they can represent next buffer
    */
   int bid;  /* next buffer ring id */

   /**
    * TODO: Implement iovecs.
    *   int iovecs_nr;
    *   struct iovec *iovecs;
    */

#endif /* HAVE_URING . TODO (hc) : remove if we are supporting both at once */

   int epollfd;
   int signalfd;
   void* buffer;
   int capacity;

};

typedef void (*io_cb)(struct ev_loop*, struct ev_io* watcher, int err);
typedef void (*signal_cb)(struct ev_loop*, struct ev_signal* watcher, int err);
typedef void (*periodic_cb)(struct ev_loop*, struct ev_periodic* watcher, int err);

/** This set of functions initializes, starts, breaks, and destroys an event loop.
 * @param w:
 * @param fd:
 * @param loop:
 * @param addr:
 * @param buf:
 * @param buf_len:
 * @param cb:
 * @param bid:
 * @return
 */
struct ev_loop* pgagroal_ev_init(struct configuration* config);
int pgagroal_ev_loop_destroy(struct ev_loop* loop);
int pgagroal_ev_loop(struct ev_loop* loop);
void pgagroal_ev_loop_break(struct ev_loop* loop);

/** This function should be called after each fork to destroy a copied loop.
 * @param loop: loop that should be destroyed.
 */
int pgagroal_ev_loop_fork(struct ev_loop** loop);
static inline bool
pgagroal_ev_loop_is_running(struct ev_loop* ev)
{
   return ev->running;
}
static inline bool
pgagroal_ev_atomic_loop_is_running(struct ev_loop* ev)
{
   return atomic_load(&ev->atomic_running);
}

void pgagroal_ev_print_backends(void);
int pgagroal_ev_supported_engines(void);
bool pgagroal_ev_supported(char*);

/** This set of functions initialize, start and stop watchers for io operations.
 * @param w:
 * @param fd:
 * @param ev_loop:
 * @param addr:
 * @param buf:
 * @param buf_len:
 * @param cb:
 * @param bid:
 * @return
 */
int _ev_io_init(struct ev_io* w, int, int, io_cb, void*, int, int);
int pgagroal_ev_io_accept_init(struct ev_io* w, int fd, io_cb cb);
int pgagroal_ev_io_read_init(struct ev_io* w, int fd, io_cb cb);
int pgagroal_ev_io_receive_init(struct ev_io* w, int fd, io_cb cb);
int pgagroal_ev_io_connect_init(struct ev_io* w, int fd, io_cb cb, union sockaddr_u* addr);
int pgagroal_io_send_init(struct ev_io* w, int fd, io_cb cb, void* buf, int buf_len, int bid);
int pgagroal_ev_io_start(struct ev_loop* loop, struct ev_io* w);
int pgagroal_ev_io_stop(struct ev_loop* loop, struct ev_io* w);

/** This set of functions initialize, start and stop watchers for periodic timeouts.
 * @param w:
 * @param ev_loop:
 * @param msec:
 * @param cb:
 * @return
 */
int pgagroal_ev_periodic_init(struct ev_periodic* w, periodic_cb cb, int msec);
int pgagroal_ev_periodic_start(struct ev_loop* loop, struct ev_periodic* w);
int pgagroal_ev_periodic_stop(struct ev_loop* loop, struct ev_periodic* w);

/** This set of functions initialize, start and stop watchers for io operations.
 * @param w:
 * @param ev_loop:
 * @param signum:
 * @param cb:
 * @return
 *
 */
int pgagroal_ev_signal_init(struct ev_signal* w, signal_cb cb, int signum);
int pgagroal_ev_signal_start(struct ev_loop* loop, struct ev_signal* w);
int pgagroal_ev_signal_stop(struct ev_loop* loop, struct ev_signal* w);

#ifdef __cplusplus
}
#endif

#endif /* EV_H */
