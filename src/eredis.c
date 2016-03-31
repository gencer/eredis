/*
 * Copyright (c) 2016 by Eulerian Technologies SAS
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials
 *   provided with the distribution.
 *
 * * Neither the name of Eulerian Technologies nor the names of its
 *   contributors may be used to endorse or promote products
 *   derived from this software without specific prior written
 *   permission.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * @file eredis.c
 * @brief ERedis main
 * @author Guillaume Fougnies <guillaume@eulerian.com>
 * @version 0.1
 * @date 2016-03-29
 */

/* Embedded hiredis */
#include "async.c"
#include "hiredis.c"
#include "net.c"
#include "read.c"
#include "sds.c"

/* Other */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include <ev.h>

/* hiredis ev */
#include "adapters/libev.h"

/* mine */
#include "eredis.h"

/* Host status */
#define HOST_DISCONNECTED 0x0
#define HOST_CONNECTED    0x1
#define HOST_FAILED       0x2

/* TCP Keep-Alive */
#define HOST_TCP_KEEPALIVE

/* Verbose */
#define EREDIS_VERBOSE    0

/* Retry to connect host every second, 10 times */
#define HOST_DISCONNECTED_RETRIES         10
/* Retry to connect "failed" host every 20 seconds */
#define HOST_FAILED_RETRY_AFTER           20

/* Max readers - DEFAULT */
#define DEFAULT_HOST_READER_MAX           10
/* Timeout - DEFAULT */
#define DEFAULT_HOST_TIMEOUT              5
/* Retry - DEFAULT */
#define DEFAULT_HOST_READER_RETRY         1

/* Number of msg to keep in writer queue if any host is connected */
#define QUEUE_MAX_UNSHIFT                 10000

#define EREDIS_READER_MAX_BUF             (2 * REDIS_READER_MAX_BUF)

/*
 * Misc flags
 */
#define EREDIS_F_INRUN                    0x01
#define EREDIS_F_INTHR                    0x02
#define EREDIS_F_READY                    0x04
#define EREDIS_F_SHUTDOWN                 0x08

/* and helpers */
#define IS_INRUN(e)                       (e->flags & EREDIS_F_INRUN)
#define IS_INTHR(e)                       (e->flags & EREDIS_F_INTHR)
#define IS_READY(e)                       (e->flags & EREDIS_F_READY)
#define IS_SHUTDOWN(e)                    (e->flags & EREDIS_F_SHUTDOWN)

#define SET_INRUN(e)                      e->flags |= EREDIS_F_INRUN
#define SET_INTHR(e)                      e->flags |= EREDIS_F_INTHR
#define SET_READY(e)                      e->flags |= EREDIS_F_READY
#define SET_SHUTDOWN(e)                   e->flags |= EREDIS_F_SHUTDOWN

#define UNSET_INRUN(e)                    e->flags &= ~EREDIS_F_INRUN
#define UNSET_INTHR(e)                    e->flags &= ~EREDIS_F_INTHR
#define UNSET_READY(e)                    e->flags &= ~EREDIS_F_READY
#define UNSET_SHUTDOWN(e)                 e->flags &= ~EREDIS_F_SHUTDOWN

/*
 * Host container
 */
typedef struct host_s {
  redisAsyncContext *async_ctx;
  eredis_t          *e;
  char              *target;

  /* 'target' is host if port>0 and unix otherwise */
  int               port:16;
  int               status:8;
  /* Connect failure counter:
   * HOST_DISCONNECTED + HOST_DISCONNECTED_RETRIES failure -> HOST_FAILED
   * HOST_FAILED       + HOST_FAILED_RETRY_AFTER -> retry
   */
  int               failures:8;
} host_t;

/*
 * Write Queue of commands
 */
typedef struct wqueue_ent_s {
  struct wqueue_ent_s *next, *prev;
  char                *s;
  int                 l;
} wqueue_ent_t;

/*
 * Read command
 */
typedef struct rcmd_s {
  char                *s;
  int                 l;
} rcmd_t;

/*
 * Reader container
 */
typedef struct eredis_reader_s {
  struct eredis_reader_s  *next, *prev;
  struct eredis_s         *e;
  redisContext            *ctx;
  void                    *reply;
  host_t                  *host;
  rcmd_t                  *cmds;
  int                     cmds_requested; /* delivered requests */
  int                     cmds_replied;   /* delivered replies */
  int                     cmds_nb;
  int                     cmds_alloc;
  int                     free:8;
  int                     retry:8;
} eredis_reader_t;

/*
 * ERedis
 */
typedef struct eredis_s {
  host_t            *hosts;
  int               hosts_nb;
  int               hosts_alloc;
  int               hosts_connected;

  struct timeval    sync_to;
  pthread_mutex_t   reader_lock;
  pthread_cond_t    reader_cond;
  struct {
    eredis_reader_t  *fst;
    int               nb;
  } rqueue;

  int               reader_max;
  int               reader_retry;
  int               flags;

  ev_timer          connect_timer;
  ev_async          send_async;

  int               send_async_pending;
  struct ev_loop    *loop;

  struct {
    wqueue_ent_t     *fst;
    int             nb;
  } wqueue;

  pthread_t         async_thr;
  pthread_mutex_t   async_lock;
} eredis_t;


/**
 * @brief Build a new eredis environment
 *
 * @return eredis
 */
  eredis_t *
eredis_new( void )
{
  eredis_t *e;

  e = calloc( 1, sizeof(eredis_t) );
  if (!e)
    return NULL;

  e->sync_to.tv_sec = DEFAULT_HOST_TIMEOUT;
  e->reader_max     = DEFAULT_HOST_READER_MAX;
  e->reader_retry   = DEFAULT_HOST_READER_RETRY;

  pthread_mutex_init( &e->async_lock,   NULL );
  pthread_mutex_init( &e->reader_lock,  NULL );
  pthread_cond_init(  &e->reader_cond,  NULL );

  return e;
}

/**
 * @brief Set timeout for all redis connections
 *
 * Default is DEFAULT_HOST_TIMEOUT (5 seconds)
 *
 * @param e          eredis
 * @param timeout_ms timeout in milliseconds
 */
  void
eredis_timeout( eredis_t *e, int timeout_ms )
{
  e->sync_to.tv_sec  = timeout_ms / 1000;
  e->sync_to.tv_usec = (timeout_ms % 1000) * 1000;
}

/**
 * @brief Set max number of reader
 *
 * Default is DEFAULT_HOST_READER_MAX (10)
 *
 * @param e   eredis
 * @param max max number of reader
 */
  void
eredis_r_max( eredis_t *e, int max )
{
  e->reader_max = max;
}

/**
 * @brief Set reader max retry
 *
 * Default is DEFAULT_HOST_READER_RETRY (1)
 *
 * @param e     eredis
 * @param retry number of retry
 */
  void
eredis_r_retry( eredis_t *e, int retry )
{
  e->reader_retry = retry;
}

/**
 * @brief Add a host to eredis
 *
 * Must be called after 'new' and before any call to 'run'.
 * The first added host will be the reference host for reader.
 *
 * If a dead 'first host' become unavailable, reader's requests
 * will switch to any other host available.
 *
 * When the dead 'first host' come back to life, reader's requests
 * will switch back to it.
 *
 * @param e       eredis
 * @param target  hostname, ip or unix socket
 * @param port    port number (0 to activate unix socket)
 */
  void
eredis_host_add( eredis_t *e, char *target, int port )
{
  host_t *h;
  if (e->hosts_nb <= e->hosts_alloc) {
    e->hosts_alloc += 8;
    e->hosts = realloc( e->hosts, sizeof(host_t) * e->hosts_alloc );
  }

#if EREDIS_VERBOSE>0
  printf("eredis: adding host: %s (%d)\n", target, port);
#endif
  h             = &e->hosts[ e->hosts_nb ];
  h->async_ctx  = NULL;
  h->e          = e;
  h->target     = strdup( target );
  h->port       = port;
  h->status     = HOST_DISCONNECTED;
  h->failures   = 0;

  e->hosts_nb ++;
}

/**
 * @brief Quick and dirty host file loader
 *
 * The file can contain comments (starting '#').
 * One line per target.
 * Hostname and port must be separated by ':'.
 * Unix sockets do not take any port value.
 *
 * @param e     eredis
 * @param file  host list file
 *
 * @return number of host loaded, -1 on error
 */
  int
eredis_host_file( eredis_t *e, char *file )
{
  struct stat st;
  int fd;
  int ret = -1, len;
  char *bufo = NULL,*buf;

  fd = open( file, O_RDONLY );
  if (fd<0)
    return -1;

  if (fstat( fd, &st ))
    goto out;

  len = st.st_size;
  if (len>16384) /* strange */
    goto out;

  bufo = buf = (char*) malloc(sizeof(char)*(len+1));
  len = read( fd, buf, len );
  if (len != st.st_size)
    goto out;

  ret = 0;
  *(buf + len) = '\0';
  while (*buf) {
    int port = 0;
    char *tk, *end = strchr(buf,'\n');
    if (! end)
      end = buf + strlen(buf);
    buf += strspn(buf, " \t");
    if (*buf == '#')
      goto next;
    tk = end;
    while (tk>buf && (*tk == ' ' || *tk == '\t'))
      tk --;
    *tk = '\0';
    tk = strchr(buf, ':'); /* port */
    if (tk) {
      *tk = '\0';
      port = atoi(tk+1);
    }
    eredis_host_add( e, buf, port );
    ret ++;
next:
    buf = end+1;
  }

out:
  close(fd);
  if (bufo)
    free(bufo);
  return ret;
}

/* Embedded reader code */
#include "reader.c"
/* Embedded queue code */
#include "queue.c"

/* Redis - ev - connect callback */
  static void
_redis_connect_cb (const redisAsyncContext *c, int status)
{
  host_t *h = (host_t*) c->data;

  if (status == REDIS_OK) {
#if EREDIS_VERBOSE>0
    fprintf(stderr, "eredis: connected %s\n", h->target);
#endif
    h->failures = 0;
    h->status   = HOST_CONNECTED;
    h->e->hosts_connected ++;

    return;
  }

  /* Status increments */
  switch (h->status) {
    case HOST_FAILED:
      h->failures %= HOST_FAILED_RETRY_AFTER;
      h->failures ++;
      break;

    case HOST_DISCONNECTED:
      if ((++ h->failures) > HOST_DISCONNECTED_RETRIES) {
        h->failures = 0;
        h->status   = HOST_FAILED;
      }
      break;
  }

  h->async_ctx  = NULL;
  /* Free is take care by hiredis */
}

/* Redis - ev - disconnect callback */
  static void
_redis_disconnect_cb (const redisAsyncContext *c, int status)
{
  host_t *h = (host_t*) c->data;
#if EREDIS_VERBOSE>0
  fprintf(stderr, "eredis: disconnected %s\n", h->target);
#endif

  (void)status;

  if (h->status != HOST_CONNECTED) {
    fprintf(
      stderr,
      "Error: strange behavior: "
      "redis_disconnect_cb called on !HOST_CONNECTED\n");
  }
  else
    h->e->hosts_connected --;

  h->failures   = 0;
  h->status     = HOST_DISCONNECTED;
  h->async_ctx  = NULL;
  /* Free is take care by hiredis */
}

/* Internal host connect - Sync or Async */
  static int
_host_connect( host_t *h, eredis_reader_t *r )
{
  redisContext *c;

  if (r) {
    /* Sync - not in EV context */
    c = (h->port) ?
      redisConnect( h->target, h->port )
      :
      redisConnectUnix( h->target );

    if (! c) {
      fprintf(stderr,
              "eredis: error: connect sync %s NULL\n",
              h->target);
      return 0;
    }
    if (c->err) {
#if EREDIS_VERBOSE>0
      fprintf(stderr,
              "eredis: error: connect sync %s %d\n",
              h->target, c->err);
#endif
      redisFree( c );
      return 0;
    }

    r->ctx   = c;
    r->host  = h;
  }

  else {
    redisAsyncContext *ac;

    /* ASync - in EV context */
    ac = (h->port) ?
      redisAsyncConnect( h->target, h->port )
      :
      redisAsyncConnectUnix( h->target );

    if (! ac) {
      fprintf(stderr,
              "eredis: error: connect async %s undef\n",
              h->target);
      return 0;
    }
    if (ac->err) {
#if EREDIS_VERBOSE>0
      fprintf(stderr,
              "eredis: error: connect async %s %d\n",
              h->target, ac->err);
#endif
      redisAsyncFree( ac );
      return 0;
    }

    h->async_ctx = ac;

    /* data for _redis_*_cb */
    ac->data = h;

    /* Order is important here */

    /* attach */
    redisLibevAttach( h->e->loop, ac );

    /* set callbacks */
    redisAsyncSetDisconnectCallback( ac, _redis_disconnect_cb );
    redisAsyncSetConnectCallback( ac, _redis_connect_cb );

    c = (redisContext*) ac;
  }

  /* Apply keep-alive */
#ifdef HOST_TCP_KEEPALIVE
  if (h->port) {
    redisEnableKeepAlive( c );
    if (r && (h->e->sync_to.tv_sec||h->e->sync_to.tv_usec)) {
      redisSetTimeout( c, h->e->sync_to );
    }
  }
#endif

  /* Override the maxbuf */
  c->reader->maxbuf = EREDIS_READER_MAX_BUF;

  return 1;
}

/*
 * EV send callback
 *
 * EV_ASYNC send_async
 */
  static void
_eredis_ev_send_cb (struct ev_loop *loop, ev_async *w, int revents)
{
  int i, nb, l;
  char *s;
  eredis_t *e;

  (void) revents;
  (void) loop;

  e = (eredis_t*) w->data;

  e->send_async_pending = 0;

  while ((s = _eredis_wqueue_shift( e, &l ))) {
    for (nb = 0, i=0; i<e->hosts_nb; i++) {
      host_t *h = &e->hosts[i];

      if (h->status == HOST_CONNECTED) {
        __redisAsyncCommand( h->async_ctx, NULL, NULL, s, l );
        nb ++;
      }
    }

    if (
      (! nb)  /* failed to deliver to any host */
      &&
      (e->wqueue.nb < QUEUE_MAX_UNSHIFT))
    {
      /* Unshift and stop */
      _eredis_wqueue_unshift( e, s, l );
      break;
    }

    free( s );
  }
}

/*
 * EV send async trigger for new commands to send
 * (External to the event loop)
 */
  static inline void
_eredis_ev_send_trigger (eredis_t *e)
{
  if (IS_READY(e) && !IS_SHUTDOWN(e) && !e->send_async_pending) {
    e->send_async_pending = 1;
    ev_async_send( e->loop, &e->send_async );
  }
}

/*
 * EV connect callback
 *
 * EV_TIMER connect_timer
 */
  static void
_eredis_ev_connect_cb (struct ev_loop *loop, ev_timer *w, int revents)
{
  int i;
  eredis_t *e;

  (void) revents;
  (void) loop;

  e = (eredis_t*) w->data;

  if (IS_SHUTDOWN(e)) {
    if (e->hosts_connected) {
      for (i=0; i<e->hosts_nb; i++) {
        host_t *h = &e->hosts[i];
        if (h->status == HOST_CONNECTED) {
          if (h->async_ctx)
            redisAsyncDisconnect( h->async_ctx );
        }
      }
    }
    else {
      /* Connect timer */
      ev_timer_stop( e->loop, &e->connect_timer );
      /* Async send */
      ev_async_stop( e->loop, &e->send_async );
      /* Event break */
      ev_break( e->loop, EVBREAK_ALL );
    }
    return;
  }

  for (i=0; i<e->hosts_nb; i++) {
    host_t *h = &e->hosts[i];
    switch (h->status) {
      case HOST_CONNECTED:
        break;

      case HOST_FAILED:
        if ((h->failures < HOST_FAILED_RETRY_AFTER)
            ||
            ( ! _host_connect( h, 0 ))) {
          h->failures %= HOST_FAILED_RETRY_AFTER;
          h->failures ++;
        }
        break;

      case HOST_DISCONNECTED:
        if (! _host_connect( h, 0 )) {
          if ((++ h->failures) > HOST_DISCONNECTED_RETRIES) {
            h->failures = 0;
            h->status   = HOST_FAILED;
          }
        }
        break;

      default:
        break;
    }
  }

  if (! IS_READY(e)) {
    /* Ready flag - need a connected host or a connection failure */
    int nb = 0;
    /* build ready flag */
    for (i=0; i<e->hosts_nb; i++) {
      host_t *h = &e->hosts[i];
      if (h->status == HOST_CONNECTED || h->failures)
        nb ++;
    }
    if (nb == e->hosts_nb) {
      SET_READY(e);
      e->send_async_pending = 1;
      ev_async_send( e->loop, &e->send_async );
    }
  }
}

/*
 * Internal generic eredis runner for the event loop (write)
 */
  static void
_eredis_run( eredis_t *e )
{
  if (! e->loop) {
    ev_timer *levt;
    ev_async *leva;

    e->loop = ev_loop_new( EVFLAG_AUTO );

    /* Connect timer */
    levt = &e->connect_timer;
    ev_timer_init( levt, _eredis_ev_connect_cb, 0., 1. );
    levt->data = e;
    ev_timer_start( e->loop, levt );

    /* Async send */
    leva = &e->send_async;
    ev_async_init( leva, _eredis_ev_send_cb );
    leva->data = e;
    ev_async_start( e->loop, leva );
  }

  SET_INRUN(e);

  if (IS_INTHR(e))
    /* Thread mode - release the thread creator */
    pthread_mutex_unlock( &(e->async_lock) );

  ev_run( e->loop, 0 );

  UNSET_INRUN(e);
}


/**
 * @brief run eredis event loop (for writes) in blocking mode
 *
 * The loop will be stopped by a call to 'eredis_shutdown' or
 * 'eredis_free'.
 * From another thread or a signal.
 *
 * @param e eredis
 *
 * @return 0 on success
 */
  int
eredis_run( eredis_t *e )
{
  _eredis_run( e );
  return 0;
}

  static void *
_eredis_run_thr( void *ve )
{
  eredis_t *e = ve;
  SET_INTHR( e );
  _eredis_run( e );
  UNSET_INTHR( e );
  pthread_exit( NULL );
}

/**
 * @brief run eredis event loop (for writes) in a dedicated thread
 *
 * Will block until the thread is ready.
 *
 * @param e eredis
 *
 * @return 0 on success
 */
  int
eredis_run_thr( eredis_t *e )
{
  int err = 0;

  if (IS_INTHR(e))
    return 0;

  pthread_mutex_lock( &(e->async_lock) );

  if (! IS_INRUN(e)) {
    err = (int)
      pthread_create( &e->async_thr, NULL, _eredis_run_thr, (void*)e );

    while (! IS_INRUN(e))
      /* Trigger from running thread */
      pthread_mutex_lock( &(e->async_lock) );
  }

  pthread_mutex_unlock( &(e->async_lock) );

  return err;
}


/**
 * @brief Shutdown the event loop
 *
 * @param e eredis
 */
  void
eredis_shutdown( eredis_t *e )
{
  /* Flag for shutdown */
  SET_SHUTDOWN(e);
}

/**
 * @brief Stop eredis and free all ressources allocated
 *
 * @param e eredis
 */
  void
eredis_free( eredis_t *e )
{
  int i;
  char *s;
  eredis_reader_t *r;

  /* Flag for shutdown */
  SET_SHUTDOWN(e);

  /* Loop trash */
  if (e->loop) {
    if (IS_INTHR( e )) /* Thread - wait to EVBREAK_ALL */
      pthread_join( e->async_thr, NULL );
    else /* Re-run loop until EVBREAK_ALL */
      eredis_run( e );

    ev_loop_destroy( e->loop );
    e->loop = NULL;
  }

  /* Shutdown what's left */
  for (i=0; i<e->hosts_nb; i++) {
    host_t *h = &e->hosts[i];
    if (h->async_ctx)
      redisAsyncFree( h->async_ctx );
    free(h->target);
  }
  free(e->hosts);

  /* Clear rqueue */
  while ((r = _eredis_rqueue_shift( e ))) {
    if (r->free)
      _eredis_reader_free( r );
    else
      fprintf(stderr,"eredis: eredis_free: reader not in 'free' state!?\n");
  }

  /* Clear wqueue */
  while ((s = _eredis_wqueue_shift( e, NULL )))
    free(s);

  pthread_mutex_destroy( &e->async_lock );
  pthread_mutex_destroy( &e->reader_lock );
  pthread_cond_destroy( &e->reader_cond );

  free(e);
}

/* Embedded rw code */
#include "rw.c"
