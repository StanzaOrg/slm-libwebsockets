#include <libwebsockets.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "simple-ws.h"

struct per_session_data {
  struct lws* wsi;
  int connection_id;
  unsigned char* sendbuf;
  int sendbufsz;
  struct lws_ring* ring;
  uint32_t tail;
  uint32_t buffered;
};

struct swsc_context {
  struct lws_context *lws;
  lws_sorted_usec_list_t sul_idle_time;
#ifdef TIME_POLL
  long pollms;
#endif
  int port;
  int refid;
  // Callbacks to stanza
  int (*established)(int, struct per_session_data*);
  int (*closed)(int, int);
  int (*receive)(int, int, const char *data, int len);
  int (*writable)(int, int);
};


static int
callback_simple_ws_client(
  struct lws *wsi,
  enum lws_callback_reasons reason,
  void *user,
  void *in,
  size_t len)
{
  struct per_session_data *pss =
    (struct per_session_data*) user;

  struct swsc_context * swsc =
    (struct swsc_context*) lws_context_user(lws_get_context(wsi));

  const struct msg *pmsg;
  struct msg amsg;
  char first, final;

  switch(reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      lwsl_user("LWS_CALLBACK_CLIENT_ESTABLISHED: wsi %p\n", wsi);
      pss->wsi = wsi;
      pss->ring = lws_ring_create(sizeof(struct msg), 4096, destroy_message);
      pss->tail = 0;
      pss->connection_id = swsc->established(swsc->refid, pss);
      break;
    case LWS_CALLBACK_CLIENT_CLOSED:
      lwsl_user("LWS_CALLBACK_CLIENT_CLOSED: wsi %p\n", wsi);
      if (pss->sendbuf) {
        free(pss->sendbuf);
        pss->sendbuf = NULL;
        pss->sendbufsz = 0;
      }
      swsc->closed(swsc->refid, pss->connection_id);
      break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
      //lwsl_user("LWS_CALLBACK_CLIENT_RECEIVE: wsi %p %ld\n", wsi, len);
      first = lws_is_first_fragment(wsi);
      final = lws_is_final_fragment(wsi);
      if(first && final) {
        // lwsl_user("first and final");
        swsc->receive(swsc->refid, pss->connection_id,
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
            lwsl_user("Delivering %d bytes", (int)(write - inbuf));
            swsc->receive(swsc->refid, pss->connection_id,
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
     break;
    case LWS_CALLBACK_CLIENT_WRITEABLE:
      //lwsl_user("LWS_CALLBACK_CLIENT_WRITEABLE: wsi %p\n", wsi);
      swsc->writable(swsc->refid, pss->connection_id);
      break;
 		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      lwsl_user("LWS_CALLBACK_CLIENT_CONNECTION_ERROR: wsi %p\n", wsi);
			pss->wsi = NULL;
			break;
    default:
      break;
  }
  return 0;
}

static struct lws_protocols protocols[] = {
  {
    "websocket",
    callback_simple_ws_client,
    sizeof(struct per_session_data),
    32768,
    0, NULL, 0
  },
  LWS_PROTOCOL_LIST_TERM
};

struct swsc_context *
swsc_create(
  int port,
  int (*established)(int, struct per_session_data*),
  int (*closed)(int, int),
  int (*receive)(int, int, const char*, int),
  int (*writable)(int, int)
) {

  int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
  struct swsc_context *swsc = calloc(1, sizeof(struct swsc_context));

  if(!swsc) {
    lwsl_err("swsc init failed\n");
    return NULL;
  }

  swsc->port = port;

  swsc->established = established;
  swsc->closed = closed;
  swsc->receive = receive;
  swsc->writable = writable;

  lws_set_log_level(logs, NULL);
  lwsl_user("jitx websocket client\n");

  struct lws_context_creation_info info = {
    .iface = "127.0.0.1",
    .port = CONTEXT_PORT_NO_LISTEN,
    //.mounts = &mount,
    .protocols = protocols,
    .user = swsc,
    // TODO: options?
    //.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE,
  };

  unsetenv("http_proxy");
  unsetenv("https_proxy");

  swsc->lws = lws_create_context(&info);
  if (!swsc->lws) {
    lwsl_err("lws init failed\n");
    return NULL;
  }

  // Connect to server
  struct lws_client_connect_info ccinfo;
  memset(&ccinfo, 0, sizeof(ccinfo));
  ccinfo.context = swsc->lws;
  ccinfo.address = "127.0.0.1";
  ccinfo.port = port;
  ccinfo.path = "/";
  ccinfo.host = lws_canonical_hostname( swsc->lws );
  ccinfo.origin = "origin";
  ccinfo.protocol = "websocket";

  struct lws* wsi = lws_client_connect_via_info(&ccinfo);
  lwsl_user("Connected with wsi %p on localhost:%d\n", wsi, port);

  // Deal with pending traffic
  lws_service(swsc->lws, 0);

  return swsc;
}

int
swsc_refid(struct swsc_context *swsc, int refid)
{
  swsc->refid = refid;
  return 0;
}

static void wakeup_timeout(lws_sorted_usec_list_t *idle) {
  struct swsc_context *swsc = lws_container_of(idle, struct swsc_context,
                                               sul_idle_time);
  lws_cancel_service(swsc->lws);
}

#define NS_IN_S 1000000000l
int
swsc_serve(struct swsc_context *swsc, int idle_time)
{
  if (swsc->lws) {
    if (idle_time >= 0)
      lws_sul_schedule(swsc->lws, 0, &swsc->sul_idle_time, wakeup_timeout,
                       idle_time * LWS_US_PER_MS);
#ifdef TIME_POLL
    struct timespec tsa, tsb;
    clock_gettime(CLOCK_MONOTONIC, &tsa);
    int r = lws_service(swsc->lws, 0);
    clock_gettime(CLOCK_MONOTONIC, &tsb);
    long nsec = tsb.tv_nsec - tsb.tv_nsec;
    time_t carry = 0;
    if (nsec < 0) {
      carry = 1;
      nsec += NS_IN_S;
    }
    swsc->pollms += (carry + tsb.tv_sec - tsa.tv_sec) * 1000 + nsec / 1000;
#else
    int r = lws_service(swsc->lws, 0);
#endif
    lws_sul_schedule(swsc->lws, 0, &swsc->sul_idle_time, NULL,
                     LWS_SET_TIMER_USEC_CANCEL);
    return r;
  }
  return -1;
}



int
swsc_destroy(struct swsc_context *swsc)
{
  if (swsc->lws) {
    lws_context_destroy(swsc->lws);
#ifdef TIME_POLL
    lwsl_user("time spent polling: %ld", swsc->pollms);
#endif
    swsc->lws = NULL;
    free(swsc);
  }
  return 0;
}

int
swsc_port(struct swsc_context *swsc)
{
  return swsc->port;
}

int
swsc_sending(struct per_session_data * pss)
{
  if(pss->wsi) {
    lws_callback_on_writable(pss->wsi);
    return 0;
  }
  return 1;
  return 1;
}

int
swsc_write(struct per_session_data *pss, unsigned char* buf, int len)
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
