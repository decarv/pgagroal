/* io.c
 * Copyright (C) 2024 Henrique de Carvalho <decarv.henrique@gmail.com>
 *
 * This code is based on: https://git.kernel.dk/cgit/liburing/tree/examples/proxy.c
 * (C) 2024 Jens Axboe <axboe@kernel.dk>
 *
 */

/* ev */
#include "../include/ev_io_uring.h"

/* system */
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <liburing.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <time.h>
#include <sys/poll.h>

static struct ev_config ctx = { 0 };
static int signal_fd;
static int running;

/**
 * Deprecated.
   //io_handler handlers[] =
   //{
   //   [ACCEPT] = accept_handler,
   //   [RECEIVE] = receive_handler,
   //   [SEND] = send_handler,
   //   [CONNECT] = connect_handler,
   //   [SOCKET] = socket_handler,
   //   [SIGNAL] = NULL,
   //   [PERIODIC] = NULL,
   //};
 */

/**
 * Event Handling Interface
 *  1. ev_init: Init the event handling context
 *  2. {signal|periodic|io}_init: returns an fd
 *  3. ev_register_{signal|periodic|io}: registers the file descriptor
 */

/**
 * EV Context
 */

int
ev_setup(struct ev_setup_opts opts)
{
   int ret;

   /* set opts */

   ctx.napi = opts.napi;
   ctx.sqpoll = opts.sqpoll;
   ctx.use_huge = opts.use_huge;
   ctx.defer_tw = opts.defer_tw;
   ctx.snd_ring = opts.snd_ring;
   ctx.snd_bundle = opts.snd_bundle;
   ctx.fixed_files = opts.fixed_files;

   ctx.buf_count = opts.buf_count;

   if (ctx.defer_tw && ctx.sqpoll)
   {
      fprintf(stderr, "Cannot use DEFER_TW and SQPOLL at the same time\n");
      exit(1);
   }

   /* setup params TODO: pull from opts */

   ctx.entries = (1 << 10);
   ctx.params.cq_entries = (1 << 10);
   ctx.params.flags = 0;
   ctx.params.flags |= IORING_SETUP_SINGLE_ISSUER; /* TODO: makes sense for pgagroal? */
   ctx.params.flags |= IORING_SETUP_CLAMP;
   ctx.params.flags |= IORING_SETUP_CQSIZE;

   /* default optsuration */

   if (ctx.defer_tw)
   {
      ctx.params.flags |= IORING_SETUP_DEFER_TASKRUN; /* overwritten by SQPOLL */
   }
   if (ctx.sqpoll)
   {
      ctx.params.flags |= IORING_SETUP_SQPOLL;
      ctx.params.sq_thread_idle = opts.sq_thread_idle;
   }
   if (!ctx.sqpoll && !ctx.defer_tw)
   {
      ctx.params.flags |= IORING_SETUP_COOP_TASKRUN;
   }
   if (!ctx.buf_count)
   {
      ctx.buf_count = BUFFER_COUNT;
   }
   if (!ctx.buf_size)
   {
      ctx.buf_size = BUFFER_SIZE;
   }
   ctx.br_mask = (ctx.buf_count - 1);

   if (ctx.fixed_files)
   {
      fprintf(stderr, "io_context_setup: no support for fixed files\n"); /* TODO */
      exit(1);
   }

   return 0;
}

int
ev_init(struct ev** ev_out, void* data, struct ev_setup_opts opts)
{
   int ret;
   struct ev* ev;

   ret = ev_setup(opts);
   if (ret)
   {
      fprintf(stderr, "ev_init: ev_setup\n");
      return 1;
   }

   *ev_out = calloc(1, sizeof(struct ev));
   if (!*ev_out)
   {
      fprintf(stderr, "ev_init: calloc\n");
      return 1;
   }

   ev = *ev_out;

   ret = io_uring_queue_init_params(ctx.entries, &ev->ring, &ctx.params);
   if (ret)
   {
      fprintf(stderr, "ev_init: io_uring_queue_init_params: %s\n", strerror(-ret));
      fprintf(stderr, "Make sure to setup context with io_context_setup\n");
      return 1;
   }

   ret = ev_setup_buffers(ev);
   if (ret)
   {
      fprintf(stderr, "");
      return 1;
   }

   for (int i = 0; i < MAX_FDS; i++)
   {
      ev->io_table[i].fd = EMPTY_FD;
   }

   ev->data = data;

   sigemptyset(&ev->sigset);

   return 0;
}

int
ev_loop(struct ev* ev)
{
   struct __kernel_timespec active_ts, idle_ts;
   siginfo_t siginfo;
   sigset_t pending;
   int flags;
   static int wait_usec = 1000000;
   idle_ts.tv_sec = 0;
   idle_ts.tv_nsec = 100000000LL;
   active_ts = idle_ts;
   if (wait_usec > 1000000)
   {
      active_ts.tv_sec = wait_usec / 1000000;
      wait_usec -= active_ts.tv_sec * 1000000;
   }
   active_ts.tv_nsec = wait_usec * 1000;

   flags = 0;
   running = true;
   while (running)
   {
      struct __kernel_timespec* ts = &idle_ts;
      struct io_uring_cqe* cqe;
      unsigned int head;
      int ret, events, to_wait;

      to_wait = 1; /* wait for any 1 */

      io_uring_submit_and_wait(&ev->ring, 0);

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

      ret = sigwaitinfo(&ev->sigset, &siginfo);
      if (ret > 0)
      {
         ret = handle_signal(ev, siginfo.si_signo);
         if (ret)
         {
            fprintf(stderr, "Signal handler not found\n");
            return 1;
         }
      }
   }

   return 0;
}

int
ev_setup_buffers(struct ev* ev)
{
   int ret;
   void* ptr;

   struct io_buf_ring* in_br = &ev->in_br;
   struct io_buf_ring* out_br = &ev->in_br;

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
   out_br->br = io_uring_setup_buf_ring(&ev->ring, ctx.buf_count, 0, 0, &ret);
   if (!in_br->br || !out_br->br)
   {
      fprintf(stderr, "Buffer ring register failed %d\n", ret);
      return 1;
   }

   ptr = in_br->buf;
   for (int i = 0; i < ctx.buf_count; i++)
   {
      printf("add bid %d, data %p\n", i, ptr);
      io_uring_buf_ring_add(in_br->br, ptr, ctx.buf_size, i, ctx.br_mask, i);
      ptr += ctx.buf_size;
   }
   io_uring_buf_ring_advance(in_br->br, ctx.buf_count);

   ptr = out_br->buf;
   for (int i = 0; i < ctx.buf_count; i++)
   {
      printf("add bid %d, data %p\n", i, ptr);
      io_uring_buf_ring_add(out_br->br, ptr, ctx.buf_size, i, ctx.br_mask, i);
      ptr += ctx.buf_size;
   }
   io_uring_buf_ring_advance(out_br->br, ctx.buf_count);

   ev->next_out_bid = 0;
   return ret;
}

int
ev_cleanup(struct ev* ev)
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
   return 0;
}

int
ev_handler(struct ev* ev, struct io_uring_cqe* cqe)
{
   int ret = 0;
   struct user_data ud;
   int accept_fd = -1;
   void* buf = NULL;
   struct fd_entry* entry = NULL;
   struct __kernel_timespec* ts_entry = NULL;
   int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
   int bid_start = bid;
   int bid_end = bid;
   int buf_len;

   ud = decode_user_data(cqe);

   if (ud.event < 0 || ud.event >= EVENTS_NR)
   {
      fprintf(stderr, "handle_event: event \n");
      return 1;
   }

   if (ud.event == PERIODIC)
   {
      return periodic_handler(ev, ud.ind);
   }
   else if (ud.event == SIGNAL)
   {
      return signal_handler(ev,ud.ind);
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
io_init(struct ev* ev,int fd,int event,io_cb cb,void* buf,size_t buf_len,int bid)
{
   int ret = 0;
   int t_index = -1;
   int domain;
   struct io_uring_sqe* sqe = get_sqe(ev);
   struct io_entry* entry = NULL;
   union sockaddr_u* addr;

   if (event >= IO_EVENTS_NR)
   {
      fprintf(stderr,"io_init: invalid event flag number: %d\n",event);
      return 1;
   }

   t_index = io_table_insert(ev,fd,cb,event);
   if (t_index < 0)
   {
      fprintf(stderr,"io_init: io_table_insert\n");
      return 1;
   }

   switch (event)
   {

      case ACCEPT:
         encode_user_data(sqe,ACCEPT,ev->id,ev->bid,fd,t_index);
         io_uring_prep_multishot_accept(sqe,fd,NULL,NULL,0);
         break;

      case RECEIVE:
         io_uring_prep_recv_multishot(sqe,fd,NULL,0,0);
         encode_user_data(sqe,RECEIVE,ev->id,0,fd,t_index);
         sqe->flags |= IOSQE_BUFFER_SELECT;
         sqe->buf_group = 0;
         break;

      case SEND:
         io_uring_prep_send(sqe,fd,buf,buf_len,MSG_WAITALL | MSG_NOSIGNAL);     /* TODO: why these flags? */
         encode_user_data(sqe,SEND,ev->id,bid,fd,t_index);
         break;

      case CONNECT:
         addr = (union sockaddr_u*)buf;
         /* expects addr to be set correctly */
         if (ctx.ipv6)
         {
            io_uring_prep_connect(sqe,fd,(struct sockaddr*) &addr->addr6,sizeof(struct sockaddr_in6));
         }
         else
         {
            io_uring_prep_connect(sqe,fd,(struct sockaddr*) &addr->addr4,sizeof(struct sockaddr_in));
         }
         encode_user_data(sqe,CONNECT,ev->id,0,fd,t_index);
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
         io_uring_prep_socket(sqe,domain,SOCK_STREAM,0,0);         /* TODO: WHAT CAN BE USED HERE ? */
         encode_user_data(sqe,SOCKET,ev->id,0,0,t_index);
         break;

      default:
         fprintf(stderr,"io_init: unknown event type: %d\n",event);
         return 1;
   }

   return 0;
}

int
io_accept_init(struct ev* ev,int fd,io_cb cb)
{
   return io_init(ev,fd,ACCEPT,cb,NULL,0,-1);
}

int
io_send_init(struct ev* ev,int fd,io_cb cb,void* buf,int buf_len,int bid)
{
   return io_init(ev,fd,SEND,cb,buf,buf_len,bid);
}

int
io_receive_init(struct ev* ev,int fd,io_cb cb)
{
   return io_init(ev,fd,RECEIVE,cb,NULL,0,-1);
}

int
io_connect_init(struct ev* ev,int fd,io_cb cb,union sockaddr_u* addr)
{
   return io_init(ev,fd,CONNECT,cb,(void*)addr,0,-1);
}

int
io_table_insert(struct ev* ev,int fd,io_cb cb,int event)
{
   int i;
   const int io_table_size = sizeof(ev->io_table) / sizeof(struct io_entry);

   /* if fd is already registered, add cb to fd entry */
   for (i = 0; i < io_table_size; i++)
   {
      if (ev->io_table[i].fd == EMPTY_FD)
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

int
io_handler(struct ev* ev,struct io_uring_cqe* cqe)
{
   int ret;
   int accept_fd;
   struct user_data ud = decode_user_data(cqe);
   int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
   int bid_start = bid;
   int bid_end = bid;
   int count;
   void* buf;

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
            case ERROR:
               return ERROR;
            case CLOSE_FD: /* connection closed */
               close(ud.fd);
               ev->io_table[ud.ind].fd = -1; /* remove io_table entry */
               return OK;
            case REPLENISH_BUFFERS:
               printf("DEBUG - ev_handler - replenish buffers triggered\n");
               /* TODO */
               return OK;
         }
      }
   }

   return ERROR;
}

int
replenish_buffers(struct io_buf_ring* br,int bid_start,int bid_end)
{
   int count;

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
signal_init(struct ev* ev,int signum,signal_cb cb)
{
   int ret;

   /* register signal */
   ret = signal_table_insert(ev,signum,cb);
   if (ret)
   {
      fprintf(stderr,"signal_init: signal_table_insert\n");
      return 1;
   }

   /* prepare signal */
   sigaddset(&ev->sigset,signum);

   ret = sigprocmask(SIG_BLOCK,&ev->sigset,NULL);
   if (ret == -1)
   {
      fprintf(stdout,"sigprocmask\n");
      return 1;
   }

   return 0;
}

int
signal_table_insert(struct ev* ev,int signum,signal_cb cb)
{
   int i;
   const int signal_table_size = sizeof(ev->sig_table) / sizeof(struct signal_entry);
   if (ev->signal_count >= signal_table_size)
   {
      fprintf(stderr,"signal_table_insert: ev->signal_count >= signal_table_size\n");
      return 1;
   }

   i = ev->signal_count++;
   ev->sig_table[i].signum = signum;
   ev->sig_table[i].cb = cb;

   return 0;
}

int
signal_init_epoll(struct ev* ev,int signum,signal_cb cb)
{
   int ret;
   int fd;

   sigaddset(&ev->sigset,signum);

   ret = sigprocmask(SIG_BLOCK,&ev->sigset,NULL);
   if (ret == -1)
   {
      fprintf(stdout,"signal_init_epoll: sigprocmask\n");
      return -1;
   }

   fd = signalfd(-1,&ev->sigset,0);    /* TODO: SFD_NONBLOCK | SFD_CLOEXEC Flags? */
   if (fd == -1)
   {
      perror("signal_init_epoll: signalfd");
      return -1;
   }

   return fd;
}

int
signal_handler(struct ev* ev,int t_index)
{
   if (t_index < 0 || t_index >= ev->signal_count)
   {
      fprintf(stderr,"signal_handler: (t_index < 0 || t_index >= ev->signal_count). t_index: %d\n",t_index);
      return 1;
   }

   return ev->sig_table[t_index].cb(ev->data,0);
}

/**
 * Periodic Events
 */

int
periodic_init(struct ev* ev,int msec,periodic_cb cb)
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
   io_uring_prep_timeout(sqe,&ts,0,IORING_TIMEOUT_MULTISHOT);
   ev->periodic_count++;

   return 0;
}

int
periodic_table_insert(struct ev* ev,struct __kernel_timespec ts,periodic_cb cb)
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

   return 0;
}

int
periodic_init_epoll(struct ev* ev,double interval)
{
   int ret;
   int fd;
   struct itimerspec new_value;
   memset(&new_value,0,sizeof(struct itimerspec));

   fd = timerfd_create(CLOCK_MONOTONIC,0);
   if (fd == -1)
   {
      perror("timerfd_create\n");
      return 1;
   }

   new_value.it_interval.tv_sec = (int)interval;
   new_value.it_interval.tv_nsec = (interval - (int)interval) * 1e9;
   new_value.it_value.tv_sec = (int)interval;
   new_value.it_value.tv_nsec = (interval - (int)interval) * 1e9;

   if (timerfd_settime(fd,0,&new_value,NULL) == -1)
   {
      perror("timerfd_settime");
      return -1;
   }

   return fd;
}

int
periodic_handler(struct ev* ev,int t_index)
{
   if (t_index < 0 || t_index >= ev->periodic_count)
   {
      fprintf(stderr,"periodic_handler: (t_index < 0 || t_index >= ev->periodic_count). t_index: %d\n",t_index);
      return 1;
   }

   return ev->per_table[t_index].cb(ev->data,0);
}

/*
 * Utils
 */

int
prepare_receive(struct ev* io,int fd,int t_index)
{
   int ret;
   struct io_uring_sqe* sqe = get_sqe(io);
   io_uring_prep_recv_multishot(sqe,fd,NULL,0,0);
   encode_user_data(sqe,RECEIVE,io->id,0,fd,t_index);
   sqe->flags |= IOSQE_BUFFER_SELECT;
   sqe->buf_group = 0;
   return 0;
}

int
prepare_send(struct ev* ev,int fd,void* buf,size_t data_len,int t_index)
{
   int ret;
   struct io_uring_sqe* sqe = get_sqe(ev);
   io_uring_prep_send(sqe,fd,buf,data_len,MSG_WAITALL | MSG_NOSIGNAL);
   encode_user_data(sqe,SEND,ev->id,0,fd,t_index);
   return 0;
}

/**
 * HANDLERS
 */

int
accept_handler(struct ev* ev,struct io_uring_cqe* cqe)
{
   int ret;
   struct user_data ud = decode_user_data(cqe);
   int accept_fd = cqe->res;
   int t_index = ud.ind;
   ret = ev->io_table[t_index].cbs[ACCEPT](ev->data,accept_fd,0,NULL,0);

   return ret;
}

int
connect_handler(struct ev* ev,struct io_uring_cqe* cqe)
{
   int ret;
   struct user_data ud = decode_user_data(cqe);
   int t_index = ud.ind;
   ret = ev->io_table[t_index].cbs[CONNECT](ev->data,ud.fd,0,NULL,0);
   return ret;
}

int
receive_handler(struct ev* ev,struct io_uring_cqe* cqe,void** send_buf_base,int* bid,bool is_proxy)
{
   int ret;
   struct user_data ud = decode_user_data(cqe);
   struct io_buf_ring* in_br = &ev->in_br;
   struct io_buf_ring* out_br = &ev->out_br;
   *send_buf_base = (void*) (in_br->buf + *bid * ctx.buf_size);
   struct io_uring_buf* buf;
   void* data;
   int pending_recv = 0;
   int this_bytes;
   int nr_packets = 0;
   int in_bytes;
   int bid_start = *bid;

   if (cqe->res == -ENOBUFS)
   {
      fprintf(stderr,"io_receive_handler: Not enough buffers\n");
      return REPLENISH_BUFFERS;
   }

   if (!(cqe->flags & IORING_CQE_F_BUFFER))
   {
      if (!(cqe->res)) /* Closed connection */
      {
         return CLOSE_FD;
      }
   }

   in_bytes = cqe->res;

   /* If the size of the buffer (this_bytes) is greater than the size of the received bytes, then continue.
    * Otherwise, we iterate over another buffer. */
   while (in_bytes)
   {
      buf = &(in_br->br->bufs[*bid]);
      data = (char*) buf->addr;
      this_bytes = buf->len;

      /* Break if the received bytes is smaller than buffer length. Otherwise, continue iterating over the buffers. */
      if (this_bytes > in_bytes)
      {
         this_bytes = in_bytes;
      }

      io_uring_buf_ring_add(out_br->br,data,this_bytes,*bid,ctx.br_mask,0);
      io_uring_buf_ring_advance(out_br->br,1);

      in_bytes -= this_bytes;

      *bid = (*bid + 1) & (ctx.buf_count - 1);
      nr_packets++;
   }

   /* From the docs: https://man7.org/linux/man-pages/man3/io_uring_prep_recv_multishot.3.html
    * "If a posted CQE does not have the IORING_CQE_F_MORE flag set then the multishot receive will be
    * done and the application should issue a new request."
    */
   if (!(cqe->flags & IORING_CQE_F_MORE))
   {
      ret = prepare_receive(ev,ud.fd,ud.ind);
      if (ret)
      {
         return 1;
      }
   }

   ret = replenish_buffers(in_br,bid_start,*bid);
   if (ret)
   {
      return 1;
   }

   return 0;
}

int
send_handler(struct ev* ev,struct io_uring_cqe* cqe)
{
   int ret;
   int buf_len = cqe->res;
   struct user_data ud = decode_user_data(cqe);
   if (ud.bid < 0)
   {
      return OK;
   }
   int bid_end = (ud.bid + buf_len / ctx.buf_size + (int)(buf_len % ctx.buf_size > 0)) % ctx.buf_count;
   ret = replenish_buffers(&ev->out_br,ud.bid,bid_end);
   if (ret)
   {
      return 1;
   }
   return 0;
}

int
socket_handler(struct ev* ev,struct io_uring_cqe* cqe,void** buf,int* bid)
{
   int ret;
   int fd;

   fd = cqe->res;

   /* TODO: do something cool */

   return 0;
}

/**
 * io_uring utils
 */

void
encode_user_data(struct io_uring_sqe* sqe,uint8_t event,uint16_t id,uint16_t bid,uint16_t fd,uint16_t ind)
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

//int
//io_decode_op(struct io_uring_cqe* cqe)
//{
//   struct user_data ud = { .as_u64 = cqe->user_data };
//   return ud.event;
//}
//
//int
//io_cqe_to_bid(struct io_uring_cqe* cqe)
//{
//   struct user_data ud = { .as_u64 = cqe->user_data };
//   return ud.bid;
//}

struct io_uring_sqe*
get_sqe(struct ev* ev)
{
   struct io_uring* ring = &ev->ring;
   struct io_uring_sqe* sqe;
   do /* necessary if SQPOLL ? */
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
next_bid(int* bid)
{
   *bid = (*bid + 1) % ctx.buf_count;
}
