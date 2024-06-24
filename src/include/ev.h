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
#define BUFFER_COUNT 8        /* 4KiB * 8 = 32 KiB */

#define EMPTY (-1)
#define INITIAL_BUF_LEN (1 << 12) /* 8 KiB */
#define MAX_BUF_LEN     (1 << 17) /* 128 KiB */

#define EV_MAX_FDS     64
#define MAX_IO         48  /* this is limited by the value of 'ind' in user data */
#define MAX_SIGNALS     8  /* this is limited by the value of 'ind' in user data */
#define MAX_PERIODIC    8  /* this is limited by the value of 'ind' in user data */
#define MAX_EVENTS     (MAX_IO + MAX_SIGNALS + MAX_PERIODIC)

enum supported_events {
   ACCEPT       = 0,
   RECEIVE      = 1,
   SEND         = 2,
   CONNECT      = 3,
   SOCKET       = 4,
   READ         = 5,
   WRITE        = 6,
   IO_EVENTS_NR = 7,   /* TODO: This is ugly. Find a better way to do this. */
   SIGNAL       = 8,
   PERIODIC     = 9,
   EVENTS_NR    = 10,
};

enum supported_signals {
   _SIGTERM = 0,
   _SIGHUP  = 1,
   _SIGINT  = 2,
   _SIGTRAP = 3,
   _SIGABRT = 4,
   _SIGALRM = 5,
};

/* Return codes used for passing states around */
enum ev_return_codes{
   EV_OK = 0,
   EV_ERROR = 1,
   _CLOSE_FD,
   _REPLENISH_BUFFERS,
   _REARMED,
   EV_ALLOC_ERROR,
};

/* TODO: remove struct ev */
struct ev_watcher ev;

struct io_in
{
        int fd;
        int client_fd;
        char* buffer;
        int buffer_length;
};

/* Define a function pointer type for I/O callbacks */
/* Define a function pointer type for signal callbacks */
/* Define a function pointer type for periodic callbacks */
/* Define a union that can hold any of the above callback types */
/* TODO: ideas to change io_cb:
 *       - use va_args
 *       - create accept_cb / receive_cb / send_cb ...
 */
typedef void(*io_cb)(void*data,int err);
/*
 * Extracting:
 *     va_list args;
 *     va_start(args, err);
 *     void* buf = va_arg(args, void*);
 *     int buf_len = va_arg(args, int);
 *     ...
 *     va_end(args);
 */
typedef void(*signal_cb)(void* data,int err);
typedef int(*periodic_cb)(void* data,int err);
typedef union event_cb
{
   io_cb io;
   signal_cb signal;
   periodic_cb periodic;
} event_cb;

union sockaddr_u
{
   struct sockaddr_in addr4;
   struct sockaddr_in6 addr6;
};

struct ev_setup_opts
{
   bool napi;
   bool sqpoll;
   int sq_thread_idle;  /* set to 1000 */
   bool use_huge;
   bool defer_tw;
   bool snd_ring;
   bool snd_bundle;
   bool fixed_files;

   int buf_count;
   int buf_size;
};

#if USE_URING

struct io_entry
{
   int fd;
   io_cb cbs[IO_EVENTS_NR];  /* either accept, receive, send, read or write callback */
};

struct signal_entry
{
   int signum;  /* signum is not being used in the current implementation (refer to [1]) */
   signal_cb cb;
};

struct periodic_entry
{
   struct __kernel_timespec ts;
   periodic_cb cb;
};

struct node
{
   union
   {
      struct io_entry io;
      struct periodic_entry periodic;
      struct signal_entry signal;
   } entry;
   struct node* next;
};

struct io_buf_ring
{
   struct io_uring_buf_ring* br;
   void* buf;
   int bgid;
};

struct user_data
{
   union
   {
      struct
      {
         uint8_t event;
         uint8_t bid;       /* unused: buffer index */
         uint16_t id;        /* unused: connection id */
         uint16_t fd;
         uint16_t ind;        /* index of the table used to retrieve the callback associated with the event */
      };
      uint64_t as_u64;
   };
};

struct ev_context
{
   int entries;

   /* startup configuration */
   bool napi;
   bool sqpoll;
   bool use_huge;
   bool defer_tw;
   bool snd_ring;
   bool snd_bundle;
   bool fixed_files;
   bool ipv6;
   /* ring mapped buffers */
   int buf_size;
   int buf_count;
   size_t buffer_ring_size;
   struct io_uring_buf_ring* buffer_rings;
   uint8_t* buffer_base;
   int br_mask;
   struct io_uring_params params;
};

struct ev_watcher
{
   atomic_bool running;  /* used to kill the loop */

   struct ev_context ctx;
   int id;

   sigset_t sigset;

   int signal_count;
   int monitored_signals[MAX_SIGNALS];
   struct signal_entry sig_table[MAX_SIGNALS];

   /* User data that can be retrieved from inside of functions */
   struct {
           void*   data;   
           uint64_t data_len;
           int32_t fd;
           int32_t client_fd;
   } out;


   int io_count;
   struct io_entry io_table[MAX_IO];

   uint64_t expirations;
   int periodic_count;
   struct periodic_entry per_table[MAX_PERIODIC];

   struct io_uring ring;
   struct io_uring_sqe* sqe;
   struct io_uring_cqe* cqe;

   int bid;  /* next buffer ring id */
   int next_out_bid;
   struct io_buf_ring in_br;
   struct io_buf_ring out_br;
   /* TODO: Do iovecs ?
    *  int iovecs_nr;
    *  struct iovec *iovecs;
    */
};

#else /* use epoll */

struct ev_entry
{
   int event;
   event_cb cb;
   void* buf;
   size_t buf_len;
   struct epoll_event epoll_ev;
};

struct signal_entry
{
   int signum;
   signal_cb cb;
};

struct ev_context
{
   int flags;
};

struct ev
{
   atomic_bool running;  /* used to kill the loop */

   struct ev_context ctx;
   int id;

   sigset_t sigset;

   int signalfd;
   int signal_count;
   int monitored_signals[MAX_SIGNALS];
   struct signal_entry sig_table[MAX_SIGNALS];

   void* data;   /* pointer to user defined data that can be retrieved from inside of functions */

   int epoll_fd;
   int flags;
   int events_nr;
   int ev_table_imap[MAX_EVENTS];   /* inverse map: fd -> ev_table_i */
   struct ev_entry ev_table[MAX_EVENTS];
};

#endif /* USE_URING */

/* Private Functions */

#if USE_URING

int pgagroal_ev_setup(struct ev_context* ,struct ev_setup_opts opts);
int pgagroal_ev_setup_buffers(struct ev_watcher* ev);
int ev_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe);
struct io_uring_sqe* get_sqe(struct ev_watcher* ev);
int io_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe);
int pgagroal_io_table_insert(struct ev_watcher* ev,int fd,io_cb cb,int event);
int periodic_table_insert(struct ev_watcher* ev,struct __kernel_timespec ts,periodic_cb cb);
int pgagroal_signal_table_insert(struct ev_watcher* ev,int signum,signal_cb cb);
int send_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe);
int receive_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe,void** buf,int*,bool is_proxy);
int accept_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe);
int connect_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe);
int socket_handler(struct ev_watcher* ev,struct io_uring_cqe* cqe,void** buf,int*);
int signal_handler(struct ev_watcher* ev,int t_index,int signum);
void encode_user_data(struct io_uring_sqe* sqe,uint8_t event,uint16_t id,uint8_t bid,uint16_t fd,uint16_t ind);
struct user_data decode_user_data(struct io_uring_cqe* cqe);
int rearm_receive(struct ev_watcher* ev,int fd,int t_index);
int prepare_send(struct ev_watcher* ev,int fd,void* buf,size_t data_len,int t_index);
int replenish_buffers(struct ev_watcher* ev,struct io_buf_ring* br,int bid_start,int bid_end);
void pgagroal_ev_set_out(struct ev_watcher*, void*, int, int, int);

#else /* use epoll */

int io_handler(struct ev* ev);
int send_handler(struct ev* ev,int t_index);
int receive_handler(struct ev* ev,int t_index);
int accept_handler(struct ev* ev,int t_index);
int connect_handler(struct ev* ev,int t_index);
int ev_handler(struct ev* ev,int);
int ev_table_insert(struct ev* ev,int fd,int event,event_cb cb,void* buf,size_t buf_len);
int ev_table_remove(struct ev* ev,int ti);
int signal_handler(struct ev* ev,int ti);
int set_non_blocking(int fd);

#endif /* USE_URING */

/*******************
* Public Interface
*******************/

/** Creates an event context to register events.
 *
 */
int pgagroal_ev_init(struct ev_watcher** ev_out,struct ev_setup_opts opts);
int pgagroal_ev_free(struct ev_watcher** ev_out);
int pgagroal_ev_start(struct ev_watcher* ev);
int pgagroal_ev_break(struct ev_watcher* ev);

/** Registers an I/O operation in the event loop.
 *
 */
int pgagroal_io_init(struct ev_watcher* ev,int fd,int event,io_cb callback,void* buf,size_t buf_len,int bid);
int pgagroal_io_stop(struct ev_watcher* ev,int fd);
/* TODO  int pgagroal_io_stop(struct ev* ev, int fd); */
int pgagroal_io_accept_init(struct ev_watcher* ev,int fd,io_cb cb);
int pgagroal_io_read_init(struct ev_watcher* ev,int fd,io_cb cb);
int pgagroal_io_receive_init(struct ev_watcher* ev,int fd,io_cb cb);
int pgagroal_io_connect_init(struct ev_watcher* ev,int fd,io_cb cb,union sockaddr_u* addr);
int pgagroal_io_send_init(struct ev_watcher* ev,int fd,io_cb cb,void* buf,int buf_len,int bid);

/** Creates a periodic timeout.
 * @param ev:
 * @param msec:
 * @param cb:
 * @return
 */
int pgagroal_periodic_init(struct ev_watcher* ev,int msec,periodic_cb cb);
/* TODO int pgagroal_periodic_stop(); */
int pgagroal_periodic_handler(struct ev_watcher* ev,int t_index);

/** Handles the triggered signals.
 * [1] *NOTE*: the io_uring implementation currently receives signum as a workaround.
 * Remember that the ideal way to deal with signals here may be through signalfd and
 * registering the signals to a table. It is cleaner and it is consistent with the rest
 * of the event handling.
 */

int pgagroal_signal_init(struct ev_watcher* io,int signum,signal_cb cb);
/* TODO int pgagroal_signal_stop(); */

#endif /* EV_H */
