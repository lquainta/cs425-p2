#include "lab.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Layer 1: packets                                                          */
/* ------------------------------------------------------------------------ */

/* On some platforms htons and friends are macros that branch on whether the
 * argument is a compile-time constant. Those branches belong to the system
 * headers, so they are excluded from branch coverage. */

static void put16(uint8_t *buf, uint16_t host)
{
  uint16_t net = htons(host); // GCOVR_EXCL_BR_LINE
  memcpy(buf, &net, sizeof net);
}

static void put32(uint8_t *buf, uint32_t host)
{
  uint32_t net = htonl(host); // GCOVR_EXCL_BR_LINE
  memcpy(buf, &net, sizeof net);
}

static uint16_t get16(const uint8_t *buf)
{
  uint16_t net;
  memcpy(&net, buf, sizeof net);
  return ntohs(net); // GCOVR_EXCL_BR_LINE
}

static uint32_t get32(const uint8_t *buf)
{
  uint32_t net;
  memcpy(&net, buf, sizeof net);
  return ntohl(net); // GCOVR_EXCL_BR_LINE
}

uint16_t inet_checksum(const uint8_t *buf, size_t len)
{
  uint32_t sum = 0;
  size_t i = 0;

  for (; i + 1 < len; i += 2)
  {
    sum += ((uint32_t)buf[i] << 8) | buf[i + 1];
    // Fold as we go so the sum can never overflow, whatever the length.
    sum = (sum & 0xffffu) + (sum >> 16);
  }
  if (i < len)
  {
    // Odd length: the last byte is the high half of a zero-padded word.
    sum += (uint32_t)buf[i] << 8;
    sum = (sum & 0xffffu) + (sum >> 16);
  }
  return (uint16_t)(~sum & 0xffffu);
}

size_t pkt_encode(const struct packet *p, uint8_t *buf, size_t cap)
{
  if (p->len > PKT_MAX_PAYLOAD)
  {
    return 0;
  }
  size_t n = PKT_HDR_LEN + (size_t)p->len;
  if (cap < n)
  {
    return 0;
  }

  buf[0] = p->type;
  buf[1] = 0;
  put16(buf + 2, 0); // the checksum is computed with this field zeroed
  put32(buf + 4, p->seq);
  put16(buf + 8, p->len);
  memcpy(buf + PKT_HDR_LEN, p->payload, p->len);
  put16(buf + 2, inet_checksum(buf, n));
  return n;
}

bool pkt_decode(const uint8_t *buf, size_t n, struct packet *out)
{
  if (n < PKT_HDR_LEN)
  {
    return false;
  }
  // Never trust the length field: check it against what actually arrived
  // before copying a single payload byte.
  uint16_t len = get16(buf + 8);
  if (len > PKT_MAX_PAYLOAD || PKT_HDR_LEN + (size_t)len != n)
  {
    return false;
  }
  if (buf[0] > PKT_FIN || buf[1] != 0)
  {
    return false;
  }
  // Summing a packet with its checksum left in gives 0xffff, whose
  // complement is 0, exactly when nothing changed on the way.
  if (inet_checksum(buf, n) != 0)
  {
    return false;
  }

  out->type = buf[0];
  out->seq = get32(buf + 4);
  out->len = len;
  memcpy(out->payload, buf + PKT_HDR_LEN, len);
  return true;
}

/* ------------------------------------------------------------------------ */
/* Layer 2: the Go-Back-N sender                                             */
/* ------------------------------------------------------------------------ */

int gbn_sender_init(struct gbn_sender *s, const uint8_t *data, size_t size,
                    uint32_t window, uint32_t timeout_ms)
{
  memset(s, 0, sizeof *s);
  if (window < GBN_MIN_WINDOW || window > GBN_MAX_WINDOW || timeout_ms == 0 ||
      size > GBN_MAX_FILE || (data == NULL && size > 0))
  {
    return -1;
  }

  s->slots = malloc((size_t)window * PKT_MAX_LEN);
  s->slot_len = calloc(window, sizeof *s->slot_len);
  if (s->slots == NULL || s->slot_len == NULL) // GCOVR_EXCL_START
  {
    gbn_sender_free(s);
    return -1;
  } // GCOVR_EXCL_STOP

  s->data = data;
  s->size = size;
  s->npkts = (uint32_t)((size + PKT_MAX_PAYLOAD - 1) / PKT_MAX_PAYLOAD);
  s->window = window;
  s->timeout_ms = timeout_ms;
  s->state = GBN_RUNNING;
  return 0;
}

void gbn_sender_free(struct gbn_sender *s)
{
  free(s->slots);
  free(s->slot_len);
  s->slots = NULL;
  s->slot_len = NULL;
}

/* Adds the stored copy of packet seq to the list of packets to send. */
static void queue_slot(const struct gbn_sender *s, uint32_t seq,
                       struct gbn_send_actions *out)
{
  size_t i = seq % s->window;
  out->pkt[out->count] = s->slots + i * PKT_MAX_LEN;
  out->len[out->count] = s->slot_len[i];
  out->count++;
}

/* Encodes packet next (DATA, or the FIN once next == npkts) into its window
 * slot, queues it, and starts the timer if it was not running. */
static void send_next(struct gbn_sender *s, uint64_t now,
                      struct gbn_send_actions *out)
{
  struct packet p = {0};
  p.seq = s->next;
  if (s->next < s->npkts)
  {
    size_t off = (size_t)s->next * PKT_MAX_PAYLOAD;
    size_t left = s->size - off;
    p.type = PKT_DATA;
    p.len = (uint16_t)(left < PKT_MAX_PAYLOAD ? left : PKT_MAX_PAYLOAD);
    memcpy(p.payload, s->data + off, p.len);
  }
  else
  {
    p.type = PKT_FIN;
  }

  size_t i = s->next % s->window;
  s->slot_len[i] = pkt_encode(&p, s->slots + i * PKT_MAX_LEN, PKT_MAX_LEN);
  queue_slot(s, s->next, out);
  s->next++;
  if (!s->timer_on)
  {
    s->timer_on = true;
    s->timer_due = now + s->timeout_ms;
  }
}

/* Sends everything the window allows. The FIN goes only once every DATA
 * packet is acknowledged, so it is never in flight alongside data. */
static void fill_window(struct gbn_sender *s, uint64_t now,
                        struct gbn_send_actions *out)
{
  while (s->next < s->npkts && s->next < s->base + s->window)
  {
    send_next(s, now, out);
  }
  if (s->base == s->npkts && s->next == s->npkts)
  {
    send_next(s, now, out);
  }
}

static void report_timer(const struct gbn_sender *s, struct gbn_send_actions *out)
{
  out->timer_on = s->timer_on;
  out->timer_due = s->timer_due;
}

void gbn_sender_start(struct gbn_sender *s, uint64_t now,
                      struct gbn_send_actions *out)
{
  out->count = 0;
  if (s->state == GBN_RUNNING)
  {
    fill_window(s, now, out);
  }
  report_timer(s, out);
}

void gbn_sender_on_packet(struct gbn_sender *s, const struct packet *p,
                          uint64_t now, struct gbn_send_actions *out)
{
  out->count = 0;
  // Only an ACK that acknowledges something new counts. seq <= base is a
  // duplicate, and seq > next would acknowledge a packet never sent.
  if (s->state == GBN_RUNNING && p->type == PKT_ACK && p->seq > s->base &&
      p->seq <= s->next)
  {
    s->base = p->seq;
    s->timeouts = 0;
    if (s->base > s->npkts)
    {
      // The FIN is acknowledged: the whole file arrived.
      s->state = GBN_DONE;
      s->timer_on = false;
    }
    else
    {
      s->timer_on = s->base < s->next;
      s->timer_due = now + s->timeout_ms;
      fill_window(s, now, out);
    }
  }
  report_timer(s, out);
}

void gbn_sender_on_timeout(struct gbn_sender *s, uint64_t now,
                           struct gbn_send_actions *out)
{
  out->count = 0;
  if (s->state == GBN_RUNNING && s->timer_on && now >= s->timer_due)
  {
    s->timeouts++;
    if (s->timeouts >= GBN_MAX_TIMEOUTS)
    {
      s->state = GBN_GAVE_UP;
      s->timer_on = false;
    }
    else
    {
      // Go back N: resend everything still unacknowledged.
      for (uint32_t seq = s->base; seq < s->next; seq++)
      {
        queue_slot(s, seq, out);
      }
      s->timer_due = now + s->timeout_ms;
    }
  }
  report_timer(s, out);
}

enum gbn_state gbn_sender_state(const struct gbn_sender *s)
{
  return s->state;
}

/* ------------------------------------------------------------------------ */
/* Layer 2: the Go-Back-N receiver                                           */
/* ------------------------------------------------------------------------ */

void gbn_receiver_init(struct gbn_receiver *r, uint64_t now)
{
  memset(r, 0, sizeof *r);
  r->last_valid = now;
  r->state = GBN_RUNNING;
}

void gbn_receiver_on_packet(struct gbn_receiver *r, const struct packet *p,
                            uint64_t now, struct gbn_recv_actions *out)
{
  memset(out, 0, sizeof *out);
  if (r->state != GBN_RUNNING)
  {
    return;
  }
  r->last_valid = now;
  if (p->type == PKT_ACK)
  {
    return; // the receiver never sends data, so an ACK means nothing here
  }

  if (!r->finished && p->seq == r->expected)
  {
    if (p->type == PKT_DATA)
    {
      out->deliver = p->payload;
      out->deliver_len = p->len;
    }
    else
    {
      r->finished = true;
      r->linger_until = now + GBN_LINGER_MS;
      out->close_file = true;
    }
    r->expected++;
  }

  // In order or not, the answer is always "everything before expected".
  struct packet ack = {0};
  ack.type = PKT_ACK;
  ack.seq = r->expected;
  out->ack_len = pkt_encode(&ack, out->ack, sizeof out->ack);
  out->send_ack = true;
}

enum gbn_state gbn_receiver_on_tick(struct gbn_receiver *r, uint64_t now)
{
  if (r->state == GBN_RUNNING && now >= gbn_receiver_deadline(r))
  {
    r->state = r->finished ? GBN_DONE : GBN_GAVE_UP;
  }
  return r->state;
}

uint64_t gbn_receiver_deadline(const struct gbn_receiver *r)
{
  return r->finished ? r->linger_until : r->last_valid + GBN_IDLE_MS;
}
