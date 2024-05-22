#include <libwebsockets.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "simple-ws.h"

struct per_session_data;

struct swss_context {
  int refid;
  struct lws_context *lws;
  struct lws_protocol_vhost_options pvo[3];
  lws_sorted_usec_list_t sul_idle_time;
#ifdef TIME_POLL
  long pollms;
#endif
  int port;
  int init;

  int (*established)(int, struct per_session_data*);
  int (*closed)(int, int);
  int (*receive)(int, int, const char *data, int len);
  int (*writable)(int, int);
};

struct per_session_data {
  int connection_id;
  unsigned char *sendbuf;
  int sendbufsz;
  struct lws *wsi;
  struct lws_ring *ring;
  uint32_t tail;
  uint32_t buffered;
};

struct vhd__simple_ws {
  struct swss_context *swss;
};

static int
callback_simple_ws_server(
  struct lws *wsi, 
  enum lws_callback_reasons reason,
  void *user, 
  void *in, 
  size_t len)
{
  struct per_session_data *pss =
    (struct per_session_data *) user;
  struct vhd__simple_ws *vhd = (struct vhd__simple_ws*)
    lws_protocol_vh_priv_get(lws_get_vhost(wsi), lws_get_protocol(wsi));

  const struct msg *pmsg;
  struct msg amsg;
  char first, final;

  switch(reason) {
    case LWS_CALLBACK_PROTOCOL_INIT: {
      struct lws_vhost *vh = lws_get_vhost(wsi);
      vhd = lws_protocol_vh_priv_zalloc(vh, lws_get_protocol(wsi),
                                        sizeof(struct vhd__simple_ws));
      if (!vhd) return -1;
      vhd->swss = (struct swss_context*) lws_pvo_search(
        (const struct lws_protocol_vhost_options *)in,
        "swss"
      )->value;
      vhd->swss->port = lws_get_vhost_listen_port(vh);
      vhd->swss->init = 1;
    } break;
    case LWS_CALLBACK_ESTABLISHED:
      lwsl_user("LWS_CALLBACK_ESTABLISHED: wsi %p\n", wsi);
      pss->wsi = wsi;
      pss->ring = lws_ring_create(sizeof(struct msg), 4096, destroy_message);
      pss->tail = 0;
      pss->connection_id = vhd->swss->established(vhd->swss->refid, pss);
      break;
    case LWS_CALLBACK_CLOSED:
      lwsl_user("LWS_CALLBACK_CLOSED: wsi %p\n", wsi);
      pss->wsi = NULL;
      if (pss->sendbuf) {
        free(pss->sendbuf);
        pss->sendbuf = NULL;
        pss->sendbufsz = 0;
      }
      vhd->swss->closed(vhd->swss->refid, pss->connection_id);
      break;
    case LWS_CALLBACK_RECEIVE:
      //lwsl_user("LWS_CALLBACK_RECEIVE: wsi %p %ld\n", wsi, len);
      first = lws_is_first_fragment(wsi);
      final = lws_is_final_fragment(wsi);
      if(first && final) {
        // lwsl_user("first and final");
        vhd->swss->receive(vhd->swss->refid, pss->connection_id,
                           (const char*) in, len);
      } else if(final) {
        // lwsl_user("final");
        uint32_t total = pss->buffered + len;
        char *inbuf = malloc(total);
        if(!inbuf) {
          lwsl_user("Aggregate allocation failed. Dropping message.");
          pss->buffered = 0;
          break;
        }
        char *write = inbuf;
        int overrun = 0;
        while((pmsg = lws_ring_get_element(pss->ring, &pss->tail))) {
          if(!overrun) {
            if(((write - inbuf) + pmsg->len) > total) {
              lwsl_user("Buffer overrun. Dropping message.");
              free(inbuf);
              overrun = 1;
            }
            memcpy(write, pmsg->payload, pmsg->len);
            write += pmsg->len;
            lws_ring_consume_single_tail(pss->ring, &pss->tail, 1);
          }
        }
        pss->buffered = 0;
        if(!overrun) {
          if(((write - inbuf) + len) > total) {
            lwsl_user("Buffer overrun. Dropping message.");
            free(inbuf);
          } else {
            memcpy(write, in, len);
            write += len;
            // lwsl_user("Delivering %d bytes", (int)(write - inbuf));
            vhd->swss->receive(vhd->swss->refid, pss->connection_id,
                               (const char*) inbuf, write - inbuf);
            free(inbuf);
          }
        }
      } else {
        int n = (int)lws_ring_get_count_free_elements(pss->ring);
        if(!n) {
          lwsl_user("Unsufficient ring buffer slots. Dropping message.");
          break;
        }
        amsg.payload = malloc(len);
        if(!amsg.payload) {
          lwsl_user("Allocation failed. Dropping message.");
          break;
        }
        amsg.len = len;
        memcpy(amsg.payload, in, len);
        if(!lws_ring_insert(pss->ring, &amsg, 1)) {
          destroy_message(&amsg);
          lwsl_user("Unable to buffer. Dropping message.");
        }
        pss->buffered += len;
      }
      break;
    case LWS_CALLBACK_SERVER_WRITEABLE:
      //lwsl_user("LWS_CALLBACK_SERVER_WRITEABLE: wsi %p\n", wsi);
      vhd->swss->writable(vhd->swss->refid, pss->connection_id);
      break;
    default:
      break;
  }

  return 0;
}

static struct lws_protocols protocols[] = {
  { "http", lws_callback_http_dummy, 0, 0, 0, NULL, 0 },
  {
    "websocket",
    callback_simple_ws_server,
    sizeof(struct per_session_data),
    32768,
    0, NULL, 0
  },
  LWS_PROTOCOL_LIST_TERM
};

static const struct lws_http_mount mount = {
/* .mount_next */       NULL,             /* linked-list "next" */
/* .mountpoint */       "/",              /* mountpoint URL */
/* .origin */           "./mount-origin", /* serve from dir */
/* .def */              "index.html",     /* default filename */
/* .protocol */         NULL,
/* .cgienv */           NULL,
/* .extra_mimetypes */  NULL,
/* .interpret */        NULL,
/* .cgi_timeout */      0,
/* .cache_max_age */    0,
/* .auth_mask */        0,
/* .cache_reusable */   0,
/* .cache_revalidate */ 0,
/* .cache_intermediaries */ 0,
// not in 4.3 /* .cache_no */         0,
/* .origin_protocol */  LWSMPRO_FILE, /* files in a dir */
/* .mountpoint_len */   1, /* char count */
/* .basic_auth_login_file */ NULL,
};

struct swss_context *
swss_create(
  int port,
  int (*established)(int, struct per_session_data*),
  int (*closed)(int, int),
  int (*receive)(int, int, const char*, int),
  int (*writable)(int, int)
) {
  struct lws_context *context;
  int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
  struct swss_context *swss = calloc(1, sizeof(struct swss_context));

  if(!swss) {
    lwsl_err("swss init failed\n");
    return NULL;
  }

  swss->established = established;
  swss->closed = closed;
  swss->receive = receive;
  swss->writable = writable;

  swss->pvo[0].options = &swss->pvo[1];
  swss->pvo[0].name = "websocket";
  swss->pvo[0].value = "";
  swss->pvo[1].next = &swss->pvo[2];
  swss->pvo[1].name = "default";
  swss->pvo[1].value = "1";
  swss->pvo[2].name = "swss";
  swss->pvo[2].value = (void*) swss;

  lws_set_log_level(logs, NULL);
  lwsl_user("jitx websocket server\n");

  struct lws_context_creation_info info = {
    .iface = "127.0.0.1",
    .port = port,
    .mounts = &mount,
    .protocols = protocols,
    .options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE,
    .pvo = swss->pvo
  };

  swss->lws = lws_create_context(&info);
  if (!swss->lws) {
    lwsl_err("lws init failed\n");
    return NULL;
  }

  int n = 0;
  // bind sockets and stuff.
  while(n >= 0 && !swss->init)
    n = lws_service(swss->lws, 0);
  return swss;
}

int
swss_refid(struct swss_context *swss, int refid)
{
  swss->refid = refid;
  return 0;
}

static void wakeup_timeout(lws_sorted_usec_list_t *idle) {
  struct swss_context *swss = lws_container_of(idle, struct swss_context,
                                               sul_idle_time);
  lws_cancel_service(swss->lws);
}


#define NS_IN_S 1000000000l
int
swss_serve(struct swss_context *swss, int idle_time)
{
  if (swss->lws) {
    if (idle_time >= 0)
      lws_sul_schedule(swss->lws, 0, &swss->sul_idle_time, wakeup_timeout,
                       idle_time * LWS_US_PER_MS);
#ifdef TIME_POLL
    struct timespec tsa, tsb;
    clock_gettime(CLOCK_MONOTONIC, &tsa);
    int r = lws_service(swss->lws, 0);
    clock_gettime(CLOCK_MONOTONIC, &tsb);
    long nsec = tsb.tv_nsec - tsb.tv_nsec;
    time_t carry = 0;
    if (nsec < 0) {
      carry = 1;
      nsec += NS_IN_S;
    }
    swss->pollms += (carry + tsb.tv_sec - tsa.tv_sec) * 1000 + nsec / 1000;
#else
    int r = lws_service(swss->lws, 0);
#endif
    lws_sul_schedule(swss->lws, 0, &swss->sul_idle_time, NULL,
                     LWS_SET_TIMER_USEC_CANCEL);
    return r;
  }
  return -1;
}

int
swss_destroy(struct swss_context *swss)
{
  if (swss->lws) {
    lws_context_destroy(swss->lws);
#ifdef TIME_POLL
    lwsl_user("time spent polling: %ld", swss->pollms);
#endif
    swss->lws = NULL;
    free(swss);
  }
  return 0;
}

int
swss_port(struct swss_context *swss)
{
  return swss->port;
}

int
swss_sending(struct per_session_data *pss)
{
  if (pss->wsi) {
    lws_callback_on_writable(pss->wsi);
    return 0;
  }
  return 1;
}

int
swss_write(struct per_session_data *pss, unsigned char* buf, int len)
{
  if (pss->wsi) {
    if (pss->sendbuf || pss->sendbufsz < len + LWS_PRE) {
      int datasz = len < SENDBUF_MIN_DATA_SZ ? SENDBUF_MIN_DATA_SZ : len + SENDBUF_PAD_SZ;
      int bufsz = datasz + LWS_PRE;
      pss->sendbuf = realloc(pss->sendbuf, bufsz);
      if (!pss->sendbuf) {
        pss->sendbufsz = 0;
        lwsl_err("Failed to reallocate send buffer size: %d\n", bufsz);
        return 1;
      }
      pss->sendbufsz = bufsz;
    }
    memcpy(pss->sendbuf + LWS_PRE, buf, len);
    lws_write(pss->wsi, pss->sendbuf + LWS_PRE, len, LWS_WRITE_TEXT);
    return 0;
  }
  return 1;
}
