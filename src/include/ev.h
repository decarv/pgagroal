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
 *
 * TODO:
 *      - evaluate the replacement of tables ({io,sig,periodic}_table) for linked lists. This allows for better management;
 *
 */

#ifndef EV_H
#define EV_H

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

#if USE_URING
#include <liburing.h>
#else
#include <sys/epoll.h>
#endif

#define ALIGNMENT sysconf(_SC_PAGESIZE)  /* TODO: should be used for huge_pages */
#define BUFFER_SIZE (1 << 14) /* 4KiB */
/**
 * NOTE: You may try to decrease this value, but it may fail tests because there is no good strategy 
 *       for replenishing buffers yet.
 *
 */
#define BUFFER_COUNT 8        /* 4KiB * 8 = 32 KiB */

#define INITIAL_BUFFER_SIZE 8192
#ifndef MAX_BUFFER_SIZE
#define MAX_BUFFER_SIZE 65535
#endif

#define MAX_EVENTS 128

enum supported_events {
   EV_ACCEPT       = 0,
   EV_RECEIVE      = 1,
   EV_SEND         = 2,
   CONNECT      = 3,
   SOCKET       = 4,
   READ         = 5,
   WRITE        = 6,
   IO_EVENTS_NR = 7,   /* TODO: This is ugly. Find a better way to do this. */
   EV_SIGNAL       = 8,
   EV_PERIODIC     = 9,
   EVENTS_NR    = 10,
};

/**
 * TODO: Delete
 */
enum supported_signals {
   EV_SIGTERM = 0,
   _SIGHUP  = 1,
   _SIGINT  = 2,
   _SIGTRAP = 3,
   _SIGABRT = 4,
   _SIGALRM = 5,
};

/**
 * TODO: Error handling in the rest of pgagroal code.
 */
enum ev_return_codes{
   EV_OK = 0,
   EV_ERROR = 1,
   EV_CLOSE_FD,
   EV_REPLENISH_BUFFERS,
   EV_REARMED,
   EV_ALLOC_ERROR,
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
   bool no_use_buffers;  /* ring mapped buffers */
   int buf_size;      /* ring mapped buffers */
   int buf_count;     /* ring mapped buffers */

   /* filled in by the library */
   int br_mask;

#if USE_URING
   struct io_uring_params params;
#endif
};


struct ev_loop;

typedef struct ev_io {
   int type; /* leave this here */
   int slot;
   int fd;
   int client_fd;
   void* data;   
   int size;
   void(*cb)(struct ev_loop*,struct ev_io*watcher,int err);
   struct ev_io *next;
} ev_io;

typedef struct ev_signal {
   int type; /* leave this here */
   int slot;
   int signum;
   void(*cb)(struct ev_loop*,struct ev_signal*watcher,int err);
   struct ev_signal *next;
} ev_signal;

typedef struct ev_periodic {
   int type; /* leave this here */
   int ind;
   int slot;
#if USE_URING
   struct __kernel_timespec ts;
#else
   int fd;
#endif
   void(*cb)(struct ev_loop*,struct ev_periodic*watcher,int err);
   struct ev_periodic *next;
} ev_periodic;

// TODO: DELETE
typedef union ev_watcher {
        struct ev_io io;
        struct ev_signal signal;
        struct ev_periodic periodic;
} ev_watcher;

#if USE_URING

struct io_buf_ring
{
   struct io_uring_buf_ring* br;
   void* buf;
   int bgid;
};

#endif

struct ev_loop {
   volatile bool running;
   atomic_bool atomic_running;
   struct ev_context ctx;

   struct ev_io ihead;
   struct ev_signal shead;
   struct ev_periodic phead;

   sigset_t sigset;

#if USE_URING
   struct io_uring ring;
   struct io_buf_ring in_br;
   struct io_buf_ring out_br;

   /* TODO: Improve the usage of .bid, .next_out_bid so they can represent next buffer */
   int bid;  /* next buffer ring id */

   /* TODO: Implement iovecs.
    *  int iovecs_nr;
    *  struct iovec *iovecs;
    */

#else /* USE_URING */

   int epollfd;
   int signalfd;
   void* buffer; 
   int capacity;
   
#endif
};

typedef void(*io_cb)(struct ev_loop*,struct ev_io*watcher,int err);
typedef void(*signal_cb)(struct ev_loop*,struct ev_signal*watcher,int err);
typedef void(*periodic_cb)(struct ev_loop*,struct ev_periodic*watcher,int err);

#if USE_URING

int _ev_setup_context(struct ev_context*,struct ev_context);
int _ev_setup_buffers(struct ev_loop* ev);

int _ev_handler(struct ev_loop* w,struct io_uring_cqe* cqe);
int _io_handler(struct ev_loop*,struct ev_io* w,struct io_uring_cqe* cqe, int type);
int _send_handler(struct ev_loop*,struct ev_io* w,struct io_uring_cqe* cqe);
int _receive_handler(struct ev_loop*,struct ev_io* w,struct io_uring_cqe* cqe,void** buf,int*,bool is_proxy);
int _accept_handler(struct ev_loop*,struct ev_io* w,struct io_uring_cqe* cqe);
int _connect_handler(struct ev_io* w,struct io_uring_cqe* cqe);
int _socket_handler(struct ev_loop*,struct ev_io* w,struct io_uring_cqe* cqe,void** buf,int*);
int _signal_handler(struct ev_loop*, int signum);
int _periodic_handler(struct ev_loop*,struct ev_periodic* w);


int _io_table_insert(struct ev_io* w,int fd,io_cb cb,int event);
int _signal_table_insert(struct ev_signal* w,int signum,int slot,signal_cb cb);
int _periodic_table_insert(struct ev_periodic* w,struct __kernel_timespec ts,periodic_cb cb);

int _prepare_send(struct ev_io* w,int fd,void* buf,size_t data_len,int t_index);
int _rearm_receive(struct ev_loop*,struct ev_io* w);
int _replenish_buffers(struct ev_loop* ev,struct io_buf_ring* br,int bid_start,int bid_end);
void _ev_set_out(struct ev_io*w, void*, int, int, int);
struct io_uring_sqe* _get_sqe(struct ev_loop* ev);

struct user_data _decode_user_data(struct io_uring_cqe* cqe);
void _encode_user_data(struct io_uring_sqe* sqe,uint8_t event,uint16_t id,uint8_t bid,uint16_t fd,uint16_t ind);

#else /* USE_URING */

int _ev_handler(struct ev_loop* ev,void*watcher);
int _io_handler(struct ev_loop* ev, struct ev_io* w);
int _send_handler(struct ev_loop* ev, struct ev_io* w);
int _accept_handler(struct ev_loop* ev, struct ev_io* w);
int _receive_handler(struct ev_loop* ev, struct ev_io* w);
int _connect_handler(struct ev_loop* ev, struct ev_io* w);
int _periodic_handler(struct ev_loop*,struct ev_periodic* w);
int _signal_handler(struct ev_loop* ev);

int set_non_blocking(int fd);

#endif /* USE_URING */

/******************************************************************************
 * Client interface: the rest of pgagroal's code is supposed to interface with
 * this library through the following functions.
 ******************************************************************************/

/** This set of functions initialize, start and breaks, and destroy an event loop.
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
struct ev_loop* pgagroal_ev_init(struct ev_context opts);
int pgagroal_ev_loop_destroy(struct ev_loop* loop);
int pgagroal_ev_loop(struct ev_loop* loop);
int pgagroal_ev_loop_break(struct ev_loop* loop);
/** This function should be called after each fork to destroy a copied loop.
 * @param loop: loop that should be destroyed.
 */
void pgagroal_ev_loop_fork(struct ev_loop* loop);
static inline bool pgagroal_ev_loop_is_running(struct ev_loop* ev) { return ev->running; }
static inline bool pgagroal_ev_atomic_loop_is_running(struct ev_loop* ev) { return atomic_load(&ev->atomic_running); }

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
int pgagroal_ev_io_init(struct ev_io* w,int ,int ,io_cb ,void*,int,int);
int pgagroal_ev_io_accept_init(struct ev_io* w,int fd,io_cb cb);
int pgagroal_ev_io_read_init(struct ev_io* w,int fd,io_cb cb);
int pgagroal_ev_io_receive_init(struct ev_io* w,int fd,io_cb cb);
int pgagroal_ev_io_connect_init(struct ev_io* w,int fd,io_cb cb,union sockaddr_u* addr);
int pgagroal_io_send_init(struct ev_io* w,int fd,io_cb cb,void* buf,int buf_len,int bid);
int pgagroal_ev_io_start(struct ev_loop* loop, struct ev_io* w);
int pgagroal_ev_io_stop(struct ev_loop* loop, struct ev_io* w);

/** This set of functions initialize, start and stop watchers for periodic timeouts.
 * @param w:
 * @param ev_loop:
 * @param msec:
 * @param cb:
 * @return
 */
int pgagroal_ev_periodic_init(struct ev_periodic* w,periodic_cb cb,int msec);
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
int pgagroal_ev_signal_init(struct ev_signal* w,signal_cb cb,int signum);
int pgagroal_ev_signal_start(struct ev_loop* loop, struct ev_signal* w);
int pgagroal_ev_signal_stop(struct ev_loop* loop, struct ev_signal* w);

#endif /* EV_H */
