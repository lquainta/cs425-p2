#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "harness/unity.h"
#include "../src/lab.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* Encodes a packet with no payload. */
static size_t make(uint8_t type, uint32_t seq, uint8_t *buf)
{
  struct packet p = {0};
  p.type = type;
  p.seq = seq;
  return pkt_encode(&p, buf, PKT_MAX_LEN);
}

static struct packet ack(uint32_t seq)
{
  struct packet p = {0};
  p.type = PKT_ACK;
  p.seq = seq;
  return p;
}

/* Decodes packet i of a sender's actions, which must be valid. */
static struct packet sent(const struct gbn_send_actions *out, size_t i)
{
  struct packet p;
  TEST_ASSERT_TRUE(i < out->count);
  TEST_ASSERT_TRUE(pkt_decode(out->pkt[i], out->len[i], &p));
  return p;
}

/* Fills a buffer with a recognisable pattern. */
static uint8_t *pattern(size_t n)
{
  uint8_t *d = malloc(n > 0 ? n : 1);
  TEST_ASSERT_NOT_NULL(d);
  for (size_t i = 0; i < n; i++)
  {
    d[i] = (uint8_t)(i * 31u + 7u);
  }
  return d;
}

/* ------------------------------------------------------------------------ */
/* Checksum                                                                  */
/* ------------------------------------------------------------------------ */

void test_checksum_rfc1071_example(void)
{
  const uint8_t b[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
  TEST_ASSERT_EQUAL_HEX16(0x220d, inet_checksum(b, sizeof b));
}

void test_checksum_odd_length_pads_with_zero(void)
{
  // The "Hi!" packet from the assignment, checksum field zeroed.
  const uint8_t b[] = {0, 0, 0, 0, 0, 0, 0, 2, 0, 3, 'H', 'i', '!'};
  TEST_ASSERT_EQUAL_HEX16(0x9691, inet_checksum(b, sizeof b));
  // One byte on its own is the high half of a word.
  const uint8_t one[] = {0x12};
  TEST_ASSERT_EQUAL_HEX16((uint16_t)~0x1200, inet_checksum(one, 1));
}

void test_checksum_empty_and_carry(void)
{
  TEST_ASSERT_EQUAL_HEX16(0xffff, inet_checksum(NULL, 0));
  // 0xffff + 0x0001 carries out and folds back in to 0x0001.
  const uint8_t b[] = {0xff, 0xff, 0x00, 0x01};
  TEST_ASSERT_EQUAL_HEX16(0xfffe, inet_checksum(b, sizeof b));
}

void test_checksum_catches_every_single_bit_flip(void)
{
  uint8_t buf[PKT_MAX_LEN];
  struct packet p = {0};
  p.type = PKT_DATA;
  p.seq = 12345;
  p.len = 100;
  for (size_t i = 0; i < p.len; i++)
  {
    p.payload[i] = (uint8_t)i;
  }
  size_t n = pkt_encode(&p, buf, sizeof buf);
  TEST_ASSERT_EQUAL_HEX16(0, inet_checksum(buf, n));
  for (size_t bit = 0; bit < n * 8; bit++)
  {
    buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    TEST_ASSERT_NOT_EQUAL(0, inet_checksum(buf, n));
    buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
  }
}

/* ------------------------------------------------------------------------ */
/* Encoding                                                                  */
/* ------------------------------------------------------------------------ */

void test_encode_data_matches_worked_example(void)
{
  const uint8_t want[] = {0x00, 0x00, 0x96, 0x91, 0x00, 0x00, 0x00,
                          0x02, 0x00, 0x03, 0x48, 0x69, 0x21};
  struct packet p = {0};
  p.type = PKT_DATA;
  p.seq = 2;
  p.len = 3;
  memcpy(p.payload, "Hi!", 3);
  uint8_t buf[PKT_MAX_LEN];
  TEST_ASSERT_EQUAL_size_t(sizeof want, pkt_encode(&p, buf, sizeof buf));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, sizeof want);
}

void test_encode_ack_matches_worked_example(void)
{
  const uint8_t want[] = {0x01, 0x00, 0xfe, 0xfc, 0x00,
                          0x00, 0x00, 0x03, 0x00, 0x00};
  uint8_t buf[PKT_MAX_LEN];
  TEST_ASSERT_EQUAL_size_t(PKT_HDR_LEN, make(PKT_ACK, 3, buf));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, sizeof want);
}

void test_encode_uses_network_byte_order(void)
{
  uint8_t buf[PKT_MAX_LEN];
  make(PKT_FIN, 0x01020304, buf);
  TEST_ASSERT_EQUAL_HEX8(0x02, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0x01, buf[4]);
  TEST_ASSERT_EQUAL_HEX8(0x02, buf[5]);
  TEST_ASSERT_EQUAL_HEX8(0x03, buf[6]);
  TEST_ASSERT_EQUAL_HEX8(0x04, buf[7]);
}

void test_encode_rejects_long_payload_and_small_buffer(void)
{
  uint8_t buf[PKT_MAX_LEN];
  struct packet p = {0};
  p.len = PKT_MAX_PAYLOAD + 1;
  TEST_ASSERT_EQUAL_size_t(0, pkt_encode(&p, buf, sizeof buf));
  p.len = 5;
  TEST_ASSERT_EQUAL_size_t(0, pkt_encode(&p, buf, PKT_HDR_LEN + 4));
  TEST_ASSERT_EQUAL_size_t(PKT_HDR_LEN + 5, pkt_encode(&p, buf, PKT_HDR_LEN + 5));
  p.len = PKT_MAX_PAYLOAD;
  TEST_ASSERT_EQUAL_size_t(PKT_MAX_LEN, pkt_encode(&p, buf, sizeof buf));
}

/* ------------------------------------------------------------------------ */
/* Decoding and validation                                                   */
/* ------------------------------------------------------------------------ */

void test_decode_round_trip(void)
{
  struct packet p = {0}, q;
  p.type = PKT_DATA;
  p.seq = 0xdeadbeef;
  p.len = PKT_MAX_PAYLOAD;
  for (size_t i = 0; i < p.len; i++)
  {
    p.payload[i] = (uint8_t)(255 - i);
  }
  uint8_t buf[PKT_MAX_LEN];
  size_t n = pkt_encode(&p, buf, sizeof buf);
  TEST_ASSERT_TRUE(pkt_decode(buf, n, &q));
  TEST_ASSERT_EQUAL_UINT8(PKT_DATA, q.type);
  TEST_ASSERT_EQUAL_HEX32(0xdeadbeef, q.seq);
  TEST_ASSERT_EQUAL_UINT16(PKT_MAX_PAYLOAD, q.len);
  TEST_ASSERT_EQUAL_MEMORY(p.payload, q.payload, p.len);
}

void test_decode_rejects_short_datagram(void)
{
  uint8_t buf[PKT_MAX_LEN];
  struct packet q;
  make(PKT_ACK, 1, buf);
  TEST_ASSERT_FALSE(pkt_decode(buf, 0, &q));
  TEST_ASSERT_FALSE(pkt_decode(buf, PKT_HDR_LEN - 1, &q));
  TEST_ASSERT_TRUE(pkt_decode(buf, PKT_HDR_LEN, &q));
}

void test_decode_rejects_length_mismatch(void)
{
  uint8_t buf[PKT_MAX_LEN + 1] = {0};
  struct packet p = {0}, q;
  p.type = PKT_DATA;
  p.len = 20;
  size_t n = pkt_encode(&p, buf, sizeof buf);
  TEST_ASSERT_FALSE(pkt_decode(buf, n - 1, &q)); // datagram shorter than length
  TEST_ASSERT_FALSE(pkt_decode(buf, n + 1, &q)); // datagram longer than length
  TEST_ASSERT_TRUE(pkt_decode(buf, n, &q));
}

void test_decode_rejects_length_over_limit(void)
{
  // A length field of 1025 with a matching 1035 byte datagram and a correct
  // checksum is still too long.
  uint8_t buf[PKT_MAX_LEN + 1] = {0};
  struct packet q;
  buf[8] = 0x04;
  buf[9] = 0x01;
  uint16_t c = inet_checksum(buf, sizeof buf);
  buf[2] = (uint8_t)(c >> 8);
  buf[3] = (uint8_t)c;
  TEST_ASSERT_EQUAL_HEX16(0, inet_checksum(buf, sizeof buf));
  TEST_ASSERT_FALSE(pkt_decode(buf, sizeof buf, &q));
}

/* Rewrites one header byte and fixes the checksum, so only that byte is wrong. */
static void set_byte_resum(uint8_t *buf, size_t n, size_t at, uint8_t v)
{
  buf[at] = v;
  buf[2] = buf[3] = 0;
  uint16_t c = inet_checksum(buf, n);
  buf[2] = (uint8_t)(c >> 8);
  buf[3] = (uint8_t)c;
}

void test_decode_rejects_unknown_type(void)
{
  uint8_t buf[PKT_MAX_LEN];
  struct packet q;
  size_t n = make(PKT_FIN, 4, buf);
  TEST_ASSERT_TRUE(pkt_decode(buf, n, &q));
  set_byte_resum(buf, n, 0, 3);
  TEST_ASSERT_FALSE(pkt_decode(buf, n, &q));
  set_byte_resum(buf, n, 0, 0xff);
  TEST_ASSERT_FALSE(pkt_decode(buf, n, &q));
}

void test_decode_rejects_nonzero_reserved(void)
{
  uint8_t buf[PKT_MAX_LEN];
  struct packet q;
  size_t n = make(PKT_ACK, 4, buf);
  set_byte_resum(buf, n, 1, 1);
  TEST_ASSERT_FALSE(pkt_decode(buf, n, &q));
}

void test_decode_rejects_flipped_bit(void)
{
  uint8_t buf[PKT_MAX_LEN];
  struct packet q;
  struct packet p = {0};
  p.type = PKT_DATA;
  p.seq = 7;
  p.len = 3;
  memcpy(p.payload, "abc", 3);
  size_t n = pkt_encode(&p, buf, sizeof buf);
  buf[11] ^= 0x10; // in the payload
  TEST_ASSERT_FALSE(pkt_decode(buf, n, &q));
  buf[11] ^= 0x10;
  buf[7] ^= 0x01; // in the seq
  TEST_ASSERT_FALSE(pkt_decode(buf, n, &q));
}

/* ------------------------------------------------------------------------ */
/* Receiver                                                                  */
/* ------------------------------------------------------------------------ */

static struct packet data_pkt(uint32_t seq, const char *text)
{
  struct packet p = {0};
  p.type = PKT_DATA;
  p.seq = seq;
  p.len = (uint16_t)strlen(text);
  memcpy(p.payload, text, p.len);
  return p;
}

static struct packet fin_pkt(uint32_t seq)
{
  struct packet p = {0};
  p.type = PKT_FIN;
  p.seq = seq;
  return p;
}

/* Asserts the receiver's actions carry ACK seq. */
static void assert_ack(const struct gbn_recv_actions *out, uint32_t seq)
{
  struct packet a;
  TEST_ASSERT_TRUE(out->send_ack);
  TEST_ASSERT_TRUE(pkt_decode(out->ack, out->ack_len, &a));
  TEST_ASSERT_EQUAL_UINT8(PKT_ACK, a.type);
  TEST_ASSERT_EQUAL_UINT32(seq, a.seq);
  TEST_ASSERT_EQUAL_UINT16(0, a.len);
}

void test_receiver_delivers_in_order(void)
{
  struct gbn_receiver r;
  struct gbn_recv_actions out;
  gbn_receiver_init(&r, 0);
  struct packet p = data_pkt(0, "hello");
  gbn_receiver_on_packet(&r, &p, 10, &out);
  TEST_ASSERT_EQUAL_size_t(5, out.deliver_len);
  TEST_ASSERT_EQUAL_MEMORY("hello", out.deliver, 5);
  TEST_ASSERT_FALSE(out.close_file);
  assert_ack(&out, 1);

  p = data_pkt(1, "world");
  gbn_receiver_on_packet(&r, &p, 20, &out);
  TEST_ASSERT_EQUAL_MEMORY("world", out.deliver, 5);
  assert_ack(&out, 2);
}

void test_receiver_reacks_duplicate(void)
{
  struct gbn_receiver r;
  struct gbn_recv_actions out;
  gbn_receiver_init(&r, 0);
  struct packet p = data_pkt(0, "a");
  gbn_receiver_on_packet(&r, &p, 1, &out);
  gbn_receiver_on_packet(&r, &p, 2, &out);
  TEST_ASSERT_NULL(out.deliver);
  TEST_ASSERT_EQUAL_size_t(0, out.deliver_len);
  assert_ack(&out, 1);
}

void test_receiver_discards_packet_beyond_gap(void)
{
  struct gbn_receiver r;
  struct gbn_recv_actions out;
  gbn_receiver_init(&r, 0);
  struct packet p = data_pkt(0, "a");
  gbn_receiver_on_packet(&r, &p, 1, &out);
  // DATA 1 is lost; 2 and 3 arrive and are thrown away with ACK 1 each time.
  for (uint32_t seq = 2; seq <= 3; seq++)
  {
    p = data_pkt(seq, "later");
    gbn_receiver_on_packet(&r, &p, 2, &out);
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 1);
  }
  // An early FIN is out of order too.
  p = fin_pkt(4);
  gbn_receiver_on_packet(&r, &p, 3, &out);
  TEST_ASSERT_FALSE(out.close_file);
  assert_ack(&out, 1);
  TEST_ASSERT_EQUAL_UINT32(1, r.expected);
}

void test_receiver_fin_closes_and_lingers(void)
{
  struct gbn_receiver r;
  struct gbn_recv_actions out;
  gbn_receiver_init(&r, 0);
  struct packet p = data_pkt(0, "x");
  gbn_receiver_on_packet(&r, &p, 1, &out);
  p = fin_pkt(1);
  gbn_receiver_on_packet(&r, &p, 100, &out);
  TEST_ASSERT_TRUE(out.close_file);
  TEST_ASSERT_NULL(out.deliver);
  assert_ack(&out, 2);
  TEST_ASSERT_EQUAL_UINT64(100 + GBN_LINGER_MS, gbn_receiver_deadline(&r));

  // Still running during the linger, and a repeated FIN gets the same ACK.
  TEST_ASSERT_EQUAL_INT(GBN_RUNNING, gbn_receiver_on_tick(&r, 1000));
  gbn_receiver_on_packet(&r, &p, 1500, &out);
  TEST_ASSERT_FALSE(out.close_file);
  assert_ack(&out, 2);
  // The repeat does not extend the linger.
  TEST_ASSERT_EQUAL_UINT64(100 + GBN_LINGER_MS, gbn_receiver_deadline(&r));
  TEST_ASSERT_EQUAL_INT(GBN_RUNNING, gbn_receiver_on_tick(&r, 100 + GBN_LINGER_MS - 1));
  TEST_ASSERT_EQUAL_INT(GBN_DONE, gbn_receiver_on_tick(&r, 100 + GBN_LINGER_MS));

  // Once done it ignores everything.
  gbn_receiver_on_packet(&r, &p, 5000, &out);
  TEST_ASSERT_FALSE(out.send_ack);
  TEST_ASSERT_EQUAL_INT(GBN_DONE, gbn_receiver_on_tick(&r, 99999));
}

void test_receiver_empty_file_is_one_fin(void)
{
  struct gbn_receiver r;
  struct gbn_recv_actions out;
  gbn_receiver_init(&r, 0);
  struct packet p = fin_pkt(0);
  gbn_receiver_on_packet(&r, &p, 5, &out);
  TEST_ASSERT_TRUE(out.close_file);
  assert_ack(&out, 1);
}

void test_receiver_ignores_ack(void)
{
  struct gbn_receiver r;
  struct gbn_recv_actions out;
  gbn_receiver_init(&r, 0);
  struct packet p = ack(5);
  gbn_receiver_on_packet(&r, &p, 7, &out);
  TEST_ASSERT_FALSE(out.send_ack);
  TEST_ASSERT_NULL(out.deliver);
  TEST_ASSERT_EQUAL_UINT32(0, r.expected);
  // It was still a valid packet, so it resets the idle clock.
  TEST_ASSERT_EQUAL_UINT64(7 + GBN_IDLE_MS, gbn_receiver_deadline(&r));
}

void test_receiver_gives_up_when_idle(void)
{
  struct gbn_receiver r;
  struct gbn_recv_actions out;
  gbn_receiver_init(&r, 1000);
  TEST_ASSERT_EQUAL_UINT64(1000 + GBN_IDLE_MS, gbn_receiver_deadline(&r));
  TEST_ASSERT_EQUAL_INT(GBN_RUNNING, gbn_receiver_on_tick(&r, 1000 + GBN_IDLE_MS - 1));
  // Valid traffic resets the idle clock.
  struct packet p = data_pkt(0, "z");
  gbn_receiver_on_packet(&r, &p, 20000, &out);
  TEST_ASSERT_EQUAL_INT(GBN_RUNNING, gbn_receiver_on_tick(&r, 1000 + GBN_IDLE_MS));
  TEST_ASSERT_EQUAL_INT(GBN_GAVE_UP, gbn_receiver_on_tick(&r, 20000 + GBN_IDLE_MS));
}

/* ------------------------------------------------------------------------ */
/* Sender                                                                    */
/* ------------------------------------------------------------------------ */

void test_sender_init_rejects_bad_arguments(void)
{
  struct gbn_sender s;
  uint8_t d[1] = {0};
  TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, d, 1, 0, 250));
  TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, d, 1, GBN_MAX_WINDOW + 1, 250));
  TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, d, 1, 8, 0));
  TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, NULL, 1, 8, 250));
  // The data is not read by init, so an oversized length is safe to pass.
  TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, d, GBN_MAX_FILE + 1, 8, 250));
  // Nothing was allocated, and freeing is still safe.
  TEST_ASSERT_NULL(s.slots);
  gbn_sender_free(&s);

  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 1, GBN_MAX_WINDOW, 1));
  TEST_ASSERT_EQUAL_INT(GBN_RUNNING, gbn_sender_state(&s));
  gbn_sender_free(&s);
  gbn_sender_free(&s); // twice is fine
}

void test_sender_packetizes_file(void)
{
  // Worked example 1: 2500 bytes are DATA 0..2 (1024, 1024, 452) and FIN 3.
  struct gbn_sender s;
  struct gbn_send_actions out;
  uint8_t *d = pattern(2500);
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 2500, 8, 250));
  TEST_ASSERT_EQUAL_UINT32(3, s.npkts);
  gbn_sender_start(&s, 0, &out);
  TEST_ASSERT_EQUAL_size_t(3, out.count);
  const uint16_t lens[] = {1024, 1024, 452};
  for (uint32_t i = 0; i < 3; i++)
  {
    struct packet p = sent(&out, i);
    TEST_ASSERT_EQUAL_UINT8(PKT_DATA, p.type);
    TEST_ASSERT_EQUAL_UINT32(i, p.seq);
    TEST_ASSERT_EQUAL_UINT16(lens[i], p.len);
    TEST_ASSERT_EQUAL_MEMORY(d + i * 1024, p.payload, p.len);
  }
  TEST_ASSERT_TRUE(out.timer_on);
  TEST_ASSERT_EQUAL_UINT64(250, out.timer_due);

  // The FIN waits until every DATA packet is acknowledged.
  struct packet a = ack(2);
  gbn_sender_on_packet(&s, &a, 50, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  a = ack(3);
  gbn_sender_on_packet(&s, &a, 60, &out);
  TEST_ASSERT_EQUAL_size_t(1, out.count);
  struct packet f = sent(&out, 0);
  TEST_ASSERT_EQUAL_UINT8(PKT_FIN, f.type);
  TEST_ASSERT_EQUAL_UINT32(3, f.seq);
  TEST_ASSERT_EQUAL_UINT16(0, f.len);
  TEST_ASSERT_EQUAL_UINT64(310, out.timer_due);

  a = ack(4);
  gbn_sender_on_packet(&s, &a, 70, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  TEST_ASSERT_FALSE(out.timer_on);
  TEST_ASSERT_EQUAL_INT(GBN_DONE, gbn_sender_state(&s));
  gbn_sender_free(&s);
  free(d);
}

void test_sender_empty_file_sends_only_fin(void)
{
  struct gbn_sender s;
  struct gbn_send_actions out;
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, NULL, 0, 8, 250));
  // No timer runs before the first send, so a timeout means nothing yet.
  gbn_sender_on_timeout(&s, 1000, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  TEST_ASSERT_FALSE(out.timer_on);
  gbn_sender_start(&s, 0, &out);
  TEST_ASSERT_EQUAL_size_t(1, out.count);
  struct packet f = sent(&out, 0);
  TEST_ASSERT_EQUAL_UINT8(PKT_FIN, f.type);
  TEST_ASSERT_EQUAL_UINT32(0, f.seq);
  // The FIN is in flight: starting again must not send it a second time.
  gbn_sender_start(&s, 1, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  struct packet a = ack(1);
  gbn_sender_on_packet(&s, &a, 10, &out);
  TEST_ASSERT_EQUAL_INT(GBN_DONE, gbn_sender_state(&s));
  gbn_sender_free(&s);
}

void test_sender_exact_multiple_of_1024(void)
{
  struct gbn_sender s;
  struct gbn_send_actions out;
  uint8_t *d = pattern(2048);
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 2048, 8, 250));
  TEST_ASSERT_EQUAL_UINT32(2, s.npkts);
  gbn_sender_start(&s, 0, &out);
  TEST_ASSERT_EQUAL_size_t(2, out.count);
  TEST_ASSERT_EQUAL_UINT16(1024, sent(&out, 0).len);
  TEST_ASSERT_EQUAL_UINT16(1024, sent(&out, 1).len);
  struct packet a = ack(2);
  gbn_sender_on_packet(&s, &a, 10, &out);
  // No empty DATA packet: straight to FIN 2.
  TEST_ASSERT_EQUAL_size_t(1, out.count);
  TEST_ASSERT_EQUAL_UINT8(PKT_FIN, sent(&out, 0).type);
  TEST_ASSERT_EQUAL_UINT32(2, sent(&out, 0).seq);
  gbn_sender_free(&s);
  free(d);
}

void test_sender_window_full_waits(void)
{
  struct gbn_sender s;
  struct gbn_send_actions out;
  uint8_t *d = pattern(10 * 1024);
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 10 * 1024, 4, 250));
  gbn_sender_start(&s, 0, &out);
  TEST_ASSERT_EQUAL_size_t(4, out.count);
  TEST_ASSERT_EQUAL_UINT32(0, s.base);
  TEST_ASSERT_EQUAL_UINT32(4, s.next);
  // Starting again, or a duplicate ACK, sends nothing more while it is full.
  gbn_sender_start(&s, 1, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  struct packet a = ack(0);
  gbn_sender_on_packet(&s, &a, 2, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  // One ACK opens room for exactly one more.
  a = ack(1);
  gbn_sender_on_packet(&s, &a, 3, &out);
  TEST_ASSERT_EQUAL_size_t(1, out.count);
  TEST_ASSERT_EQUAL_UINT32(4, sent(&out, 0).seq);
  gbn_sender_free(&s);
  free(d);
}

void test_sender_cumulative_ack_slides_several(void)
{
  struct gbn_sender s;
  struct gbn_send_actions out;
  uint8_t *d = pattern(10 * 1024);
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 10 * 1024, 4, 250));
  gbn_sender_start(&s, 0, &out);
  // ACK 3 covers 0, 1 and 2 at once, even though ACK 1 and 2 were lost.
  struct packet a = ack(3);
  gbn_sender_on_packet(&s, &a, 100, &out);
  TEST_ASSERT_EQUAL_UINT32(3, s.base);
  TEST_ASSERT_EQUAL_size_t(3, out.count);
  for (uint32_t i = 0; i < 3; i++)
  {
    TEST_ASSERT_EQUAL_UINT32(4 + i, sent(&out, i).seq);
  }
  // Progress restarts the timer.
  TEST_ASSERT_EQUAL_UINT64(350, out.timer_due);
  gbn_sender_free(&s);
  free(d);
}

void test_sender_ignores_duplicate_and_bogus_acks(void)
{
  struct gbn_sender s;
  struct gbn_send_actions out;
  uint8_t *d = pattern(10 * 1024);
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 10 * 1024, 4, 250));
  gbn_sender_start(&s, 0, &out);
  struct packet a = ack(2);
  gbn_sender_on_packet(&s, &a, 10, &out);
  TEST_ASSERT_EQUAL_UINT64(260, out.timer_due);

  // A repeat of ACK 2, or an older ACK, changes nothing, timer included.
  gbn_sender_on_packet(&s, &a, 50, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  a = ack(1);
  gbn_sender_on_packet(&s, &a, 60, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  TEST_ASSERT_EQUAL_UINT32(2, s.base);
  TEST_ASSERT_EQUAL_UINT64(260, out.timer_due);

  // An ACK for a packet never sent is ignored.
  a = ack(s.next + 1);
  gbn_sender_on_packet(&s, &a, 70, &out);
  TEST_ASSERT_EQUAL_UINT32(2, s.base);

  // So is anything that is not an ACK.
  struct packet p = data_pkt(3, "x");
  gbn_sender_on_packet(&s, &p, 80, &out);
  p = fin_pkt(4);
  gbn_sender_on_packet(&s, &p, 80, &out);
  TEST_ASSERT_EQUAL_UINT32(2, s.base);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  gbn_sender_free(&s);
  free(d);
}

void test_sender_timeout_resends_whole_window(void)
{
  struct gbn_sender s;
  struct gbn_send_actions out;
  uint8_t *d = pattern(10 * 1024);
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 10 * 1024, 4, 250));
  gbn_sender_start(&s, 0, &out);
  struct packet a = ack(2);
  gbn_sender_on_packet(&s, &a, 10, &out); // base 2, next 6

  // Before the timer is due, nothing happens.
  gbn_sender_on_timeout(&s, 259, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);

  gbn_sender_on_timeout(&s, 260, &out);
  TEST_ASSERT_EQUAL_size_t(4, out.count);
  for (uint32_t i = 0; i < 4; i++)
  {
    struct packet p = sent(&out, i);
    TEST_ASSERT_EQUAL_UINT32(2 + i, p.seq);
    TEST_ASSERT_EQUAL_MEMORY(d + (2 + i) * 1024, p.payload, 1024);
  }
  TEST_ASSERT_EQUAL_UINT64(510, out.timer_due);
  TEST_ASSERT_EQUAL_UINT32(1, s.timeouts);

  // Progress resets the count of timeouts in a row.
  a = ack(3);
  gbn_sender_on_packet(&s, &a, 300, &out);
  TEST_ASSERT_EQUAL_UINT32(0, s.timeouts);
  gbn_sender_free(&s);
  free(d);
}

void test_sender_stop_and_wait(void)
{
  // A window of 1 is stop-and-wait: one packet in flight at a time.
  struct gbn_sender s;
  struct gbn_send_actions out;
  uint8_t *d = pattern(3000);
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 3000, 1, 100));
  gbn_sender_start(&s, 0, &out);
  TEST_ASSERT_EQUAL_size_t(1, out.count);
  for (uint32_t seq = 1; seq <= 3; seq++)
  {
    struct packet a = ack(seq);
    gbn_sender_on_packet(&s, &a, seq * 10, &out);
    TEST_ASSERT_EQUAL_size_t(1, out.count);
    TEST_ASSERT_EQUAL_UINT32(seq, sent(&out, 0).seq);
    TEST_ASSERT_TRUE(out.timer_on);
  }
  TEST_ASSERT_EQUAL_UINT8(PKT_FIN, sent(&out, 0).type);
  // A timeout on the FIN resends the FIN.
  gbn_sender_on_timeout(&s, 1000, &out);
  TEST_ASSERT_EQUAL_size_t(1, out.count);
  TEST_ASSERT_EQUAL_UINT8(PKT_FIN, sent(&out, 0).type);
  gbn_sender_free(&s);
  free(d);
}

void test_sender_gives_up_after_ten_timeouts(void)
{
  struct gbn_sender s;
  struct gbn_send_actions out;
  uint8_t *d = pattern(100);
  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, d, 100, 8, 250));
  gbn_sender_start(&s, 0, &out);
  uint64_t now = 0;
  for (int i = 1; i < GBN_MAX_TIMEOUTS; i++)
  {
    now = out.timer_due;
    gbn_sender_on_timeout(&s, now, &out);
    TEST_ASSERT_EQUAL_size_t(1, out.count);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, gbn_sender_state(&s));
  }
  gbn_sender_on_timeout(&s, out.timer_due, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  TEST_ASSERT_FALSE(out.timer_on);
  TEST_ASSERT_EQUAL_INT(GBN_GAVE_UP, gbn_sender_state(&s));

  // A late ACK, a start, or another timeout changes nothing now.
  struct packet a = ack(1);
  gbn_sender_on_packet(&s, &a, now + 1000, &out);
  gbn_sender_start(&s, now + 1000, &out);
  gbn_sender_on_timeout(&s, now + 5000, &out);
  TEST_ASSERT_EQUAL_size_t(0, out.count);
  TEST_ASSERT_EQUAL_INT(GBN_GAVE_UP, gbn_sender_state(&s));
  gbn_sender_free(&s);
  free(d);
}

/* ------------------------------------------------------------------------ */
/* End to end through an in-memory channel                                   */
/* ------------------------------------------------------------------------ */

/* xorshift32: a tiny generator whose sequence is the same on every platform. */
static uint32_t rng_next(uint32_t *state)
{
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *state = x;
}

static double rng_unit(uint32_t *state)
{
  return (double)rng_next(state) / 4294967296.0;
}

#define CHAN_CAP 4096
#define CHAN_DELAY 50

struct msg
{
  uint64_t at;
  size_t len;
  uint8_t bytes[PKT_MAX_LEN];
};

/* One direction of the channel: a FIFO with a fixed delay, like the relay. */
struct chan
{
  struct msg *q;
  size_t head, count;
  double loss, corrupt, dup;
  uint32_t *rng;
  unsigned dropped, corrupted, duplicated;
};

static void chan_push(struct chan *c, const uint8_t *b, size_t n, uint64_t at)
{
  TEST_ASSERT_TRUE(c->count < CHAN_CAP);
  struct msg *m = &c->q[(c->head + c->count) % CHAN_CAP];
  m->at = at;
  m->len = n;
  memcpy(m->bytes, b, n);
  c->count++;
}

/* Damages a datagram exactly the way the relay does. */
static void chan_send(struct chan *c, const uint8_t *b, size_t n, uint64_t now)
{
  if (rng_unit(c->rng) < c->loss)
  {
    c->dropped++;
    return;
  }
  if (rng_unit(c->rng) < c->corrupt)
  {
    uint8_t copy[PKT_MAX_LEN];
    memcpy(copy, b, n);
    uint32_t bit = rng_next(c->rng) % (uint32_t)(n * 8);
    copy[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    c->corrupted++;
    chan_push(c, copy, n, now + CHAN_DELAY);
    return;
  }
  chan_push(c, b, n, now + CHAN_DELAY);
  if (rng_unit(c->rng) < c->dup)
  {
    c->duplicated++;
    chan_push(c, b, n, now + CHAN_DELAY);
  }
}

static const struct msg *chan_ready(const struct chan *c, uint64_t now)
{
  return c->count > 0 && c->q[c->head].at <= now ? &c->q[c->head] : NULL;
}

static void chan_pop(struct chan *c)
{
  c->head = (c->head + 1) % CHAN_CAP;
  c->count--;
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
  return a < b ? a : b;
}

struct result
{
  enum gbn_state sender, receiver;
  uint8_t *got;
  size_t got_len;
  bool closed;
  unsigned damaged;
};

/* Runs a whole transfer between the two state machines, with a simulated
 * clock that jumps straight to the next event. */
static struct result transfer(const uint8_t *data, size_t size, uint32_t window,
                              double rate, uint32_t seed)
{
  struct result res = {0};
  res.got = malloc(size > 0 ? size : 1);
  TEST_ASSERT_NOT_NULL(res.got);

  uint32_t rng = seed;
  struct chan fwd = {0}, back = {0};
  fwd.q = malloc(CHAN_CAP * sizeof *fwd.q);
  back.q = malloc(CHAN_CAP * sizeof *back.q);
  TEST_ASSERT_NOT_NULL(fwd.q);
  TEST_ASSERT_NOT_NULL(back.q);
  fwd.loss = fwd.corrupt = fwd.dup = rate;
  back.loss = back.corrupt = back.dup = rate;
  fwd.rng = back.rng = &rng;

  struct gbn_sender s;
  struct gbn_receiver r;
  struct gbn_send_actions so;
  struct gbn_recv_actions ro;
  struct packet p;
  uint64_t now = 0;

  TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, data, size, window, 250));
  gbn_receiver_init(&r, now);
  gbn_sender_start(&s, now, &so);
  for (size_t i = 0; i < so.count; i++)
  {
    chan_send(&fwd, so.pkt[i], so.len[i], now);
  }

  while (gbn_sender_state(&s) == GBN_RUNNING || r.state == GBN_RUNNING)
  {
    // Jump to the next thing that can happen.
    uint64_t next = UINT64_MAX;
    if (fwd.count > 0)
      next = min_u64(next, fwd.q[fwd.head].at);
    if (back.count > 0)
      next = min_u64(next, back.q[back.head].at);
    if (gbn_sender_state(&s) == GBN_RUNNING && so.timer_on)
      next = min_u64(next, so.timer_due);
    if (r.state == GBN_RUNNING)
      next = min_u64(next, gbn_receiver_deadline(&r));
    now = next;

    const struct msg *m;
    while ((m = chan_ready(&fwd, now)) != NULL)
    {
      if (pkt_decode(m->bytes, m->len, &p))
      {
        gbn_receiver_on_packet(&r, &p, now, &ro);
        if (ro.deliver_len > 0)
        {
          TEST_ASSERT_TRUE(res.got_len + ro.deliver_len <= size);
          TEST_ASSERT_FALSE(res.closed);
          memcpy(res.got + res.got_len, ro.deliver, ro.deliver_len);
          res.got_len += ro.deliver_len;
        }
        res.closed = res.closed || ro.close_file;
        if (ro.send_ack)
          chan_send(&back, ro.ack, ro.ack_len, now);
      }
      chan_pop(&fwd);
    }
    while ((m = chan_ready(&back, now)) != NULL)
    {
      if (pkt_decode(m->bytes, m->len, &p))
      {
        gbn_sender_on_packet(&s, &p, now, &so);
        for (size_t i = 0; i < so.count; i++)
          chan_send(&fwd, so.pkt[i], so.len[i], now);
      }
      chan_pop(&back);
    }
    gbn_sender_on_timeout(&s, now, &so);
    for (size_t i = 0; i < so.count; i++)
      chan_send(&fwd, so.pkt[i], so.len[i], now);
    gbn_receiver_on_tick(&r, now);
  }

  res.sender = gbn_sender_state(&s);
  res.receiver = r.state;
  res.damaged = fwd.dropped + fwd.corrupted + fwd.duplicated + back.dropped +
                back.corrupted + back.duplicated;
  gbn_sender_free(&s);
  free(fwd.q);
  free(back.q);
  return res;
}

static void check_transfer(size_t size, uint32_t window, double rate, uint32_t seed)
{
  uint8_t *d = pattern(size);
  struct result res = transfer(d, size, window, rate, seed);
  TEST_ASSERT_EQUAL_INT(GBN_DONE, res.sender);
  TEST_ASSERT_EQUAL_INT(GBN_DONE, res.receiver);
  TEST_ASSERT_TRUE(res.closed);
  TEST_ASSERT_EQUAL_size_t(size, res.got_len);
  if (size > 0)
  {
    TEST_ASSERT_EQUAL_MEMORY(d, res.got, size);
  }
  if (rate > 0)
  {
    TEST_ASSERT_TRUE(res.damaged > 0); // the channel really did misbehave
  }
  free(res.got);
  free(d);
}

void test_transfer_clean_channel(void)
{
  check_transfer(2500, 1, 0, 1);
  check_transfer(2500, 16, 0, 1);
  check_transfer(0, 8, 0, 1);
  check_transfer(4096, 4, 0, 1);
}

void test_transfer_lossy_channel_several_seeds(void)
{
  // 20% loss, 20% corruption and 20% duplication, in each direction.
  const uint32_t seeds[] = {1, 7, 42, 2026, 0xc0ffee};
  const uint32_t windows[] = {4, 8, 16, 64};
  for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++)
  {
    for (size_t w = 0; w < sizeof windows / sizeof windows[0]; w++)
    {
      check_transfer(100 * 1024 + 123, windows[w], 0.2, seeds[i]);
    }
  }
}

void test_transfer_lossy_stop_and_wait(void)
{
  check_transfer(20 * 1024 + 5, 1, 0.1, 99);
  check_transfer(0, 1, 0.2, 5);
}

void test_transfer_dead_channel_gives_up(void)
{
  // Everything is lost: the sender gives up and the receiver times out.
  uint8_t *d = pattern(5000);
  struct result res = transfer(d, 5000, 8, 1.0, 3);
  TEST_ASSERT_EQUAL_INT(GBN_GAVE_UP, res.sender);
  TEST_ASSERT_EQUAL_INT(GBN_GAVE_UP, res.receiver);
  TEST_ASSERT_EQUAL_size_t(0, res.got_len);
  free(res.got);
  free(d);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_checksum_rfc1071_example);
  RUN_TEST(test_checksum_odd_length_pads_with_zero);
  RUN_TEST(test_checksum_empty_and_carry);
  RUN_TEST(test_checksum_catches_every_single_bit_flip);
  RUN_TEST(test_encode_data_matches_worked_example);
  RUN_TEST(test_encode_ack_matches_worked_example);
  RUN_TEST(test_encode_uses_network_byte_order);
  RUN_TEST(test_encode_rejects_long_payload_and_small_buffer);
  RUN_TEST(test_decode_round_trip);
  RUN_TEST(test_decode_rejects_short_datagram);
  RUN_TEST(test_decode_rejects_length_mismatch);
  RUN_TEST(test_decode_rejects_length_over_limit);
  RUN_TEST(test_decode_rejects_unknown_type);
  RUN_TEST(test_decode_rejects_nonzero_reserved);
  RUN_TEST(test_decode_rejects_flipped_bit);
  RUN_TEST(test_receiver_delivers_in_order);
  RUN_TEST(test_receiver_reacks_duplicate);
  RUN_TEST(test_receiver_discards_packet_beyond_gap);
  RUN_TEST(test_receiver_fin_closes_and_lingers);
  RUN_TEST(test_receiver_empty_file_is_one_fin);
  RUN_TEST(test_receiver_ignores_ack);
  RUN_TEST(test_receiver_gives_up_when_idle);
  RUN_TEST(test_sender_init_rejects_bad_arguments);
  RUN_TEST(test_sender_packetizes_file);
  RUN_TEST(test_sender_empty_file_sends_only_fin);
  RUN_TEST(test_sender_exact_multiple_of_1024);
  RUN_TEST(test_sender_window_full_waits);
  RUN_TEST(test_sender_cumulative_ack_slides_several);
  RUN_TEST(test_sender_ignores_duplicate_and_bogus_acks);
  RUN_TEST(test_sender_timeout_resends_whole_window);
  RUN_TEST(test_sender_stop_and_wait);
  RUN_TEST(test_sender_gives_up_after_ten_timeouts);
  RUN_TEST(test_transfer_clean_channel);
  RUN_TEST(test_transfer_lossy_channel_several_seeds);
  RUN_TEST(test_transfer_lossy_stop_and_wait);
  RUN_TEST(test_transfer_dead_channel_gives_up);
  return UNITY_END();
}
