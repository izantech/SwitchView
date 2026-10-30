#include "link.h"
#include "switchview_proto.h"
#if __has_include("compat/platform.h")
#include "compat/platform.h"
#else
#include "platform.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#define KEEPALIVE_MS 20   // 50 Hz state refresh while idle
#define PING_MS 500       // link-health probe interval
#define UP_TIMEOUT_MS 2000
#define RESOCKET_MS 5000  // no echo for this long: rebuild the socket (interface may have changed)

// Text pacing: the firmware types ~10 chars/s from a 255-byte queue. A token
// bucket lets single keystrokes go out instantly while long pastes never
// overflow the Pico's queue.
#define TEXT_BUCKET_MAX 200.0f

struct link {
  sv_socket_t fd;
  struct sockaddr_in dst;
  char desc[64];
  struct link_state cur;
  int dirty;
  uint32_t last_state_ms, last_text_ms, last_ping_ms, last_echo_ms, last_resocket_ms;
  float tokens;
  int mdx, mdy, mwheel, mdirty;
  unsigned mbtn;
  char textq[1024];
  unsigned tq_head, tq_tail;
  struct link_stats st;
};

static sv_socket_t open_socket(void)
{
  sv_socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd != SV_INVALID_SOCKET) sv_set_nonblocking(fd);
  return fd;
}

struct link *link_open(const char *host, int port)
{
  struct link *l = calloc(1, sizeof *l);
  if (!l) return NULL;
  l->fd = open_socket();
  if (l->fd == SV_INVALID_SOCKET) { perror("socket"); free(l); return NULL; }
  l->dst.sin_family = AF_INET;
  l->dst.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &l->dst.sin_addr) != 1) {
    fprintf(stderr, "link: bad host %s\n", host);
    sv_close_socket(l->fd);
    free(l);
    return NULL;
  }
  snprintf(l->desc, sizeof l->desc, "udp %s:%d", host, port);
  l->cur.hat = SV_HAT_CENTER;
  l->dirty = 1;
  l->tokens = TEXT_BUCKET_MAX;
  l->st.leds = -1;
  return l;
}

void link_close(struct link *l)
{
  if (!l) return;
  // Leave the Switch with everything released.
  struct link_state zero = {.hat = SV_HAT_CENTER};
  link_set_state(l, &zero);
  link_poll(l, l->last_state_ms + KEEPALIVE_MS + 1);
  sv_close_socket(l->fd);
  free(l);
}

static void send_packet(struct link *l, const sv_packet *p)
{
  if (sendto(l->fd, (const char *)p, SV_PACKET_SIZE, MSG_DONTWAIT, (struct sockaddr *)&l->dst, sizeof l->dst) < 0)
    l->st.send_errors++;
  else
    l->st.packets++;
}

void link_set_state(struct link *l, const struct link_state *s)
{
  if (memcmp(&l->cur, s, sizeof *s) == 0) return;
  l->cur = *s;
  l->dirty = 1;
}

void link_mouse(struct link *l, int dx, int dy, int wheel, unsigned buttons)
{
  l->mdx += dx; l->mdy += dy; l->mwheel += wheel;
  if (buttons != l->mbtn) { l->mbtn = buttons; l->mdirty = 1; }
  if (dx || dy || wheel) l->mdirty = 1;
}

void link_send_text(struct link *l, const char *text)
{
  for (; *text; text++) {
    unsigned next = (l->tq_head + 1) % sizeof l->textq;
    if (next == l->tq_tail) break;
    l->textq[l->tq_head] = *text;
    l->tq_head = next;
  }
}

void link_poll(struct link *l, uint32_t now_ms)
{
  if (l->dirty || now_ms - l->last_state_ms >= KEEPALIVE_MS) {
    sv_packet p = {.magic = {SV_MAGIC0, SV_MAGIC1}, .type = SV_PKT_STATE,
                   .buttons = l->cur.buttons, .lx = l->cur.lx, .ly = l->cur.ly,
                   .rx = l->cur.rx, .ry = l->cur.ry, .hat = l->cur.hat};
    send_packet(l, &p);
    l->dirty = 0;
    l->last_state_ms = now_ms;
    l->st.last_send_ms = now_ms;
  }

  if (l->mdirty) {
    int cl = l->mdx < -32767 ? -32767 : l->mdx > 32767 ? 32767 : l->mdx;
    int cy = l->mdy < -32767 ? -32767 : l->mdy > 32767 ? 32767 : l->mdy;
    int cw = l->mwheel < -127 ? -127 : l->mwheel > 127 ? 127 : l->mwheel;
    sv_packet p = {.magic = {SV_MAGIC0, SV_MAGIC1}, .type = SV_PKT_MOUSE, .buttons = l->mbtn,
                   .lx = (int16_t)cl, .ly = (int16_t)cy, .rx = (int16_t)cw, .hat = SV_HAT_CENTER};
    send_packet(l, &p);
    l->mdx -= cl; l->mdy -= cy; l->mwheel -= cw;
    l->mdirty = 0;
  }

  l->tokens += (now_ms - l->last_text_ms) * (SV_TEXT_CHARS_PER_SEC / 1000.0f);
  if (l->tokens > TEXT_BUCKET_MAX) l->tokens = TEXT_BUCKET_MAX;
  l->last_text_ms = now_ms;
  if (l->tq_head != l->tq_tail && l->tokens >= 1.0f) {
    sv_packet p = {.magic = {SV_MAGIC0, SV_MAGIC1}, .type = SV_PKT_TEXT, .hat = SV_HAT_CENTER};
    while (p.length < SV_TEXT_MAX && l->tq_head != l->tq_tail && l->tokens >= 1.0f) {
      p.text[p.length++] = (uint8_t)l->textq[l->tq_tail];
      l->tq_tail = (l->tq_tail + 1) % sizeof l->textq;
      l->tokens -= 1.0f;
    }
    send_packet(l, &p);
    l->st.last_send_ms = now_ms;
  }
  l->st.text_queued = (l->tq_head + sizeof l->textq - l->tq_tail) % sizeof l->textq;

  // Link health: ping, read echoes, and rebuild the socket if the Pico has
  // gone quiet for a while (covers the PC changing networks underneath us).
  if (now_ms - l->last_ping_ms >= PING_MS) {
    sv_packet p = {.magic = {SV_MAGIC0, SV_MAGIC1}, .type = SV_PKT_PING, .buttons = now_ms,
                   .hat = SV_HAT_CENTER};
    send_packet(l, &p);
    l->last_ping_ms = now_ms;
  }
  sv_packet in;
  struct sockaddr_in from;
  socklen_t fl = sizeof from;
  while (recvfrom(l->fd, (char *)&in, sizeof in, MSG_DONTWAIT, (struct sockaddr *)&from, &fl) >= SV_PACKET_SIZE) {
    if (in.magic[0] == SV_MAGIC0 && in.magic[1] == SV_MAGIC1 && in.type == SV_PKT_PING) {
      l->last_echo_ms = now_ms;
      l->st.rtt_ms = (float)(now_ms - in.buttons);
      l->st.leds = in.text[1] ? in.text[0] : -1;
    }
  }
  int up = l->last_echo_ms && now_ms - l->last_echo_ms < UP_TIMEOUT_MS;
  if (up != l->st.up) fprintf(stderr, "link: Pico %s\n", up ? "up" : "not answering");
  l->st.up = up;
  if (!up && now_ms - l->last_resocket_ms >= RESOCKET_MS) {
    sv_socket_t fd = open_socket();
    if (fd != SV_INVALID_SOCKET) { sv_close_socket(l->fd); l->fd = fd; }
    l->last_resocket_ms = now_ms;
  }
}

void link_get_stats(struct link *l, struct link_stats *out) { *out = l->st; }
const char *link_describe(struct link *l) { return l->desc; }
