#ifndef LAB_H
#define LAB_H

/*
 * Reliable file transfer over UDP with Go-Back-N.
 *
 * This header holds the two layers that never touch a socket, a clock, or a
 * file, so the unit tests can drive them directly:
 *
 *   1. Packets: the Internet checksum and the wire format.
 *   2. The Go-Back-N sender and receiver state machines. Time is always passed
 *      in as a parameter in milliseconds, and every event function fills in an
 *      "actions" struct saying what to send, what to deliver, and when the
 *      timer is due.
 *
 * The third layer, the I/O (socket, relay hello, poll loop, clock, file), lives
 * in main.c and does nothing but feed events in and carry actions out.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Layer 1: packets                                                          */
/* ------------------------------------------------------------------------ */

#define PKT_HDR_LEN 10
#define PKT_MAX_PAYLOAD 1024
#define PKT_MAX_LEN (PKT_HDR_LEN + PKT_MAX_PAYLOAD)

/** Packet types, as carried in the first byte of the header. */
enum pkt_type
{
  PKT_DATA = 0,
  PKT_ACK = 1,
  PKT_FIN = 2
};

/** A decoded packet. The payload is copied, so it outlives the datagram. */
struct packet
{
  uint8_t type;
  uint32_t seq;
  uint16_t len;
  uint8_t payload[PKT_MAX_PAYLOAD];
};

/**
 * @brief Computes the RFC 1071 Internet checksum of a buffer.
 *
 * The buffer is summed as big-endian 16 bit words with one's complement
 * addition (carries folded back in), and an odd length is padded with one
 * zero byte for the calculation only.
 *
 * @param buf The bytes to sum. May be NULL only when len is 0.
 * @param len The number of bytes.
 * @return The one's complement of the one's complement sum. A buffer that
 *         already contains its own correct checksum yields 0.
 */
uint16_t inet_checksum(const uint8_t *buf, size_t len);

/**
 * @brief Encodes a packet into its wire format, checksum included.
 *
 * Fields are written in network byte order.
 *
 * @param p The packet to encode. Its len must be at most PKT_MAX_PAYLOAD.
 * @param buf Where to write the bytes.
 * @param cap The size of buf.
 * @return The number of bytes written, PKT_HDR_LEN + p->len, or 0 if the
 *         payload is too long or buf is too small.
 */
size_t pkt_encode(const struct packet *p, uint8_t *buf, size_t cap);

/**
 * @brief Decodes and validates a datagram.
 *
 * A datagram is accepted only if it is at least PKT_HDR_LEN bytes, its length
 * field matches its size and is at most PKT_MAX_PAYLOAD, its type is known,
 * its reserved byte is 0, and its checksum verifies.
 *
 * @param buf The datagram as received.
 * @param n The size recvfrom returned.
 * @param out Filled in only when the datagram is valid.
 * @return true if the datagram is a valid packet, false if it must be
 *         discarded as if it had been lost.
 */
bool pkt_decode(const uint8_t *buf, size_t n, struct packet *out);

/* ------------------------------------------------------------------------ */
/* Layer 2: Go-Back-N state machines                                         */
/* ------------------------------------------------------------------------ */

#define GBN_MIN_WINDOW 1
#define GBN_MAX_WINDOW 64
#define GBN_MAX_TIMEOUTS 10
#define GBN_MAX_FILE (16u * 1024u * 1024u)
#define GBN_LINGER_MS 2000u
#define GBN_IDLE_MS 30000u

/** Where a state machine is in its life. */
enum gbn_state
{
  GBN_RUNNING = 0,
  GBN_DONE,    /* transfer finished: exit 0 */
  GBN_GAVE_UP  /* too many timeouts or too long idle: exit 2 */
};

/** What the sender wants done after an event. */
struct gbn_send_actions
{
  size_t count;                        /* packets to send, in order */
  const uint8_t *pkt[GBN_MAX_WINDOW];  /* each points into the sender's window */
  size_t len[GBN_MAX_WINDOW];
  bool timer_on;                       /* whether the timer is running */
  uint64_t timer_due;                  /* when it expires, if it is */
};

/**
 * The Go-Back-N sender. The file lives in memory; packets base to next - 1
 * are kept encoded in a ring of window slots for retransmission. The FIN is
 * packet npkts and is sent only once every DATA packet is acknowledged.
 */
struct gbn_sender
{
  const uint8_t *data; /* the file contents, not owned */
  size_t size;
  uint32_t npkts;      /* number of DATA packets */
  uint32_t window;
  uint32_t timeout_ms;
  uint32_t base;       /* oldest unacknowledged packet */
  uint32_t next;       /* next packet never sent */
  uint8_t *slots;      /* window * PKT_MAX_LEN bytes, slot = seq % window */
  size_t *slot_len;
  bool timer_on;
  uint64_t timer_due;
  unsigned timeouts;   /* timeouts in a row with no progress */
  enum gbn_state state;
};

/**
 * @brief Prepares a sender for a file already read into memory.
 *
 * @param s The sender to initialize.
 * @param data The file contents. Must stay valid until gbn_sender_free.
 *             May be NULL when size is 0.
 * @param size The file size, at most GBN_MAX_FILE.
 * @param window The window size, GBN_MIN_WINDOW to GBN_MAX_WINDOW.
 * @param timeout_ms The retransmission timeout, greater than 0.
 * @return 0 on success, -1 on a bad argument or if memory ran out.
 */
int gbn_sender_init(struct gbn_sender *s, const uint8_t *data, size_t size,
                    uint32_t window, uint32_t timeout_ms);

/** @brief Releases the sender's window buffers. Safe to call twice. */
void gbn_sender_free(struct gbn_sender *s);

/**
 * @brief Sends the first window (or the FIN, for an empty file).
 * @param s The sender.
 * @param now The current time in milliseconds.
 * @param out What to send and when the timer is due.
 */
void gbn_sender_start(struct gbn_sender *s, uint64_t now,
                      struct gbn_send_actions *out);

/**
 * @brief Feeds a valid packet that arrived from the receiver.
 *
 * Only an ACK with base < seq <= next moves anything: base slides to seq, the
 * timer restarts (or stops), and new packets fill the opened window. Every
 * other packet, including a duplicate ACK, changes nothing.
 *
 * @param s The sender.
 * @param p The decoded packet.
 * @param now The current time in milliseconds.
 * @param out What to send and when the timer is due.
 */
void gbn_sender_on_packet(struct gbn_sender *s, const struct packet *p,
                          uint64_t now, struct gbn_send_actions *out);

/**
 * @brief Tells the sender the time, so it can act if its timer has expired.
 *
 * On expiry it resends every packet from base to next - 1 and restarts the
 * timer, unless this is the GBN_MAX_TIMEOUTS-th timeout in a row without
 * progress, in which case it gives up. Before the timer is due it does
 * nothing.
 *
 * @param s The sender.
 * @param now The current time in milliseconds.
 * @param out What to send and when the timer is due.
 */
void gbn_sender_on_timeout(struct gbn_sender *s, uint64_t now,
                           struct gbn_send_actions *out);

/** @brief Returns whether the sender is running, done, or gave up. */
enum gbn_state gbn_sender_state(const struct gbn_sender *s);

/** What the receiver wants done after an event. */
struct gbn_recv_actions
{
  bool send_ack;
  uint8_t ack[PKT_HDR_LEN]; /* the encoded ACK, when send_ack is set */
  size_t ack_len;
  const uint8_t *deliver;   /* in-order payload to write, or NULL */
  size_t deliver_len;
  bool close_file;          /* the FIN arrived: the file is complete */
};

/** The Go-Back-N receiver: one number, plus what it needs to end cleanly. */
struct gbn_receiver
{
  uint32_t expected;     /* next packet index wanted */
  bool finished;         /* FIN received, now lingering */
  uint64_t linger_until; /* when the linger ends */
  uint64_t last_valid;   /* when the last valid packet arrived */
  enum gbn_state state;
};

/**
 * @brief Prepares a receiver that has just registered with the relay.
 * @param r The receiver.
 * @param now The current time in milliseconds; the idle clock starts here.
 */
void gbn_receiver_init(struct gbn_receiver *r, uint64_t now);

/**
 * @brief Feeds a valid packet that arrived from the sender.
 *
 * DATA with seq == expected is delivered and acknowledged. A FIN with
 * seq == expected completes the file and starts the linger. Any other DATA or
 * FIN is discarded and answered with ACK expected again. An ACK is ignored.
 *
 * @param r The receiver.
 * @param p The decoded packet. out->deliver points into p->payload.
 * @param now The current time in milliseconds.
 * @param out What to write and which ACK to send.
 */
void gbn_receiver_on_packet(struct gbn_receiver *r, const struct packet *p,
                            uint64_t now, struct gbn_recv_actions *out);

/**
 * @brief Tells the receiver the time, so it can end the linger or give up.
 * @param r The receiver.
 * @param now The current time in milliseconds.
 * @return The receiver's state after the tick.
 */
enum gbn_state gbn_receiver_on_tick(struct gbn_receiver *r, uint64_t now);

/**
 * @brief Returns when the receiver next needs a tick: the end of the linger,
 *        or the moment it would give up for lack of traffic.
 */
uint64_t gbn_receiver_deadline(const struct gbn_receiver *r);

#endif // LAB_H
