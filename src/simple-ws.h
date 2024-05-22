#ifndef __SIMPLE_WS_H__
#define __SIMPLE_WS_H__

static int SENDBUF_MIN_DATA_SZ = 512 * 1024;
static int SENDBUF_PAD_SZ = 32 * 1024;

struct msg {
  void *payload;
  size_t len;
};

static void
destroy_message(void *memory) {
  struct msg *msg = memory;
  free(msg->payload);
  msg->payload = NULL;
  msg->len = 0;
}

#endif