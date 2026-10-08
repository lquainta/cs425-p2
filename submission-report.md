# Submission Report

- Submission generated at 10/08/2026 at 21:53:02

- Machine info: Linux runnervmmprz5 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux

## Note to Students

Please read this report carefully before submission.
Ensure that all sections are complete and accurate.
Look for any errors in the build or test outputs.
If you find any issues, correct them before submitting.
Post any questions on the class discussion board for help.


---

## README

# P2 - Reliable Data Transfer

- Name: Landon Quaintance
- Email: LandonQuaintance@u.boisestate.edu
- Class: CS425-001

A reliable file transfer over UDP using Go-Back-N. The sender and receiver
register with the course relay, which pairs them up and drops, corrupts and
duplicates what passes between them. Checksums, sequence numbers, cumulative
ACKs, one retransmission timer and a sliding window get the file across
byte-identical anyway.

```bash
make all
python3 cs425_relay.py --delay 50                       # terminal 1
./build/release/myapp recv -s lquainta-1 127.0.0.1 out.bin   # terminal 2
./build/release/myapp send -s lquainta-1 -w 16 -l 0.1 -c 0.05 127.0.0.1 in.bin
cmp in.bin out.bin && echo identical
```

Run with no arguments to see the full usage. Exit codes: 0 on a successful
transfer, 1 for a bad command line (or a file that cannot be opened), 2 when the
relay refuses us, never answers, or the transfer gives up.

## Design

The program is split into three layers so that all of the protocol logic can be
unit tested without a network, a clock or a file.

1. **Packets** (`lab.h` / `lab.c`): `inet_checksum`, `pkt_encode` and
   `pkt_decode`. Pure functions, bytes in and bytes or a struct out. Every
   multi-byte field goes through `htons`/`htonl` and their inverses into a byte
   buffer; no struct is ever copied onto the wire. `pkt_decode` checks the
   length field against the size `recvfrom` actually returned before it copies a
   single payload byte, and rejects short datagrams, unknown types, a non-zero
   reserved byte and a bad checksum. A rejected datagram is treated exactly like
   a lost one.
2. **Go-Back-N state machines** (`lab.h` / `lab.c`): `struct gbn_sender` and
   `struct gbn_receiver`, plus the functions that feed them events: a packet
   arrived, or here is the time (so a timer can expire). The current time is
   always a parameter in milliseconds, and every event function fills in an
   actions struct that says which packets to send, which payload to write, and
   when the timer is due. Nothing in this layer calls `sendto`, `recvfrom`,
   `poll`, `clock_gettime` or `fwrite`.
   - The sender holds `base`, `next`, one timer, and an encoded copy of every
     packet from `base` to `next - 1` in a ring of `window` slots. An ACK with
     `base < seq <= next` slides `base` (possibly several packets at once) and
     restarts the timer; any other ACK is a duplicate and changes nothing. A
     timeout resends every slot from `base` to `next - 1`. The FIN is packet
     `npkts`, sent once every DATA packet is acknowledged and retransmitted like
     any other packet. Ten timeouts in a row without progress means give up.
     Stop-and-wait is just `-w 1`.
   - The receiver holds `expected`. In-order DATA is delivered and acknowledged;
     anything else is dropped and answered with `ACK expected` again. The FIN
     completes the file and starts a two second linger during which a repeated
     FIN gets the same final ACK. Thirty seconds without a valid packet means
     give up.
3. **I/O** (`main.c`): the command line (`getopt`), `getaddrinfo`, one connected
   UDP socket for the whole run, the relay hello with its five one-second
   retries, the `poll` loop driven by `clock_gettime(CLOCK_MONOTONIC)`, and the
   file. It is deliberately thin: read an event, hand it to the state machine,
   carry out what comes back.

The payoff is in the tests. `tests/lab-test.c` drives layer 2 with a fake clock
and an in-memory channel that loses, corrupts and duplicates 20% of the packets
in each direction (with a fixed-seed generator, the same on every platform), and
checks that the delivered bytes match the file for several seeds and window
sizes. A lost packet in a test is just one that is not passed along, and a
timeout is just a bigger number for "now". The same code then runs unchanged
against the real relay.

## Results

A 1 MiB file of random bytes (1024 DATA packets plus the FIN, 1025 packets in
all) was sent through `cs425_relay.py --delay 50` on one macOS machine, with
the default 250 ms timeout. Each combination ran three times; the time is the
sender's whole run, from start to exit, and every copy compared identical with
`cmp`.

| Window | Loss | Corrupt | Dup | Runs (s)                 | Mean time (s) | Throughput (KiB/s) | Retransmissions (mean) |
|-------:|-----:|--------:|----:|--------------------------|--------------:|-------------------:|-----------------------:|
|      1 |    0 |       0 |   0 | 105.50, 105.44, 105.44   |        105.46 |               9.71 |                      0 |
|     16 |    0 |       0 |   0 | 6.77, 6.77, 6.77         |          6.77 |             151.30 |                      0 |
|      1 | 0.05 |       0 |   0 | 137.35, 138.84, 136.58   |        137.59 |               7.44 |                    128 |
|     16 | 0.05 |       0 |   0 | 25.85, 24.94, 21.32      |         24.04 |              42.60 |                    896 |

**Round trip time.** With a window of 1 the sender waits one round trip per
packet, so the RTT it saw is 105.46 s / 1025 = **102.9 ms**. The relay's
`--delay 50` accounts for 100 ms of that. The other ~3 ms is everything that
is not the deliberate delay: the relay is a Python program, so each datagram is
received, scheduled and re-sent by an interpreter whose timers wake up a little
late, and that happens twice per round trip (DATA one way, ACK the other). On
top of that come four trips through the loopback stack, the scheduler waking
our `poll` on each end, and building and checksumming a packet, plus the
one-off hello and startup at the beginning of the run. Each piece is small, but
1025 round trips add them all up.

**Speedup at window 16.** With 16 packets in flight the sender gets 16 packets
per round trip instead of one, so the transfer should take about 1025 / 16
round trips. It took 6.77 s, a speedup of 105.46 / 6.77 = **15.6x**, close to
16 but not quite. On loopback, putting a packet on the wire takes microseconds,
so the window rather than the link sets the pace, which is why the speedup is
almost linear. It falls short because the end of the transfer cannot be
pipelined: the FIN is sent only once every DATA packet is acknowledged, so it
costs one whole round trip by itself. 1024 DATA packets take 64 round trips,
plus 1 for the FIN, gives 65 x 102.9 ms = 6.69 s. That predicts 15.8x, and
startup accounts for the rest.

**Why loss hurts the window 16 run so much more.** At 5% loss the window 1 run
took 30% longer, and the window 16 run took 255% longer (3.6x). With a window
of 1, a lost DATA or ACK costs one timeout (250 ms instead of a ~103 ms round
trip) and one resent packet: about 128 retransmissions, each costing roughly
250 ms, which adds the ~32 s we measured. With a window of 16, a loss costs far
more:

- The timer stalls the whole pipe. While the sender waits 250 ms for the
  timeout, it cannot send anything new, so it loses about 2.4 round trips' worth
  of 16 packets each, not one packet's worth.
- Go-Back-N resends everything from `base` to `next - 1`, up to 16 packets,
  even though the receiver already got and discarded most of them. That is why
  the window 16 runs retransmitted ~900 packets against ~128 for window 1.
- With 16 packets in flight, the chance that at least one is lost in a window is
  1 - 0.95^16, about 56%, so stalls happen in most windows. The resent burst can
  itself lose a packet and trigger another full timeout.

Lost ACKs are the one thing the larger window handles better: a later
cumulative ACK covers a lost one. At window 1 every lost ACK costs a timeout.
Even so, the window 16 run is still 5.7x faster than window 1 under the same
loss.

## Testing

- `make check`: 36 Unity tests covering the RFC 1071 example, odd lengths and
  every single-bit flip; the worked packet examples byte for byte; short,
  length-mismatched, oversized, unknown-type and bad-reserved datagrams; the
  receiver with duplicates, gaps, early and repeated FINs, the linger and the
  idle timeout; the sender with a full window, cumulative and duplicate ACKs,
  ACKs for packets never sent, a timeout resending the whole window, giving up
  after ten timeouts, an empty file and a file that is an exact multiple of
  1024 bytes; and whole transfers through the lossy in-memory channel.
- `make report`: 100% line and branch coverage of `src/lab.c`. The only
  exclusions are the malloc failure path and the branches inside the system
  `htons`/`ntohs` macros.
- `make leak-test` / `make leak`: clean under AddressSanitizer.
- Manual runs through `cs425_relay.py` with loss, corruption and duplication up
  to 0.3, windows 1 to 64, and empty, 4 KiB and 300 KB files all compared
  identical with `cmp`. A sender with no receiver prints `relay refused: no
  receiver` and exits 2; with no relay running, both sides give up after five
  hellos and exit 2.

## Known Bugs or Issues

None known. As the assignment specifies, the receiver's two second linger is
fixed, so if every repeat of the final ACK is lost the sender can still give up
(exit 2) on a transfer that actually arrived intact. With realistic loss rates
this takes eight losses in a row.

## Experience

This project was interesting. I was pretty confused about what was going on at first, but it started making sense once Claude had the code working and I began running transfers through the relay myself. Before this I didn't really understand the difference between TCP and UDP. Seeing a file arrive byte-identical even with the relay dropping and corrupting packets made it clear that UDP gives you nothing, and that everything TCP promises comes from pieces like checksums, sequence numbers, ACKs and a retransmission timer. The measurements helped too: going from a window of 1 to 16 made the transfer about 15 times faster, but 5% loss slowed the window 16 run down far more because every timeout resends the whole window.
---


## Build Output

This section was generated by running `make all` in the project root directory.

```bash
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/lab.c -o build/debug/lab.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug/main.c.o
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address build/debug/lab.c.o build/debug/main.c.o -o build/debug/myapp_d -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/lab.c -o build/release/lab.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/main.c -o build/release/main.c.o
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion build/release/lab.c.o build/release/main.c.o -o build/release/myapp 
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/lab.c -o build/tests/lab.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/main.c -o build/tests/main.c.o
mkdir -p build/tests/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/lab-test.c -o build/tests/lab-test.c.o
mkdir -p build/tests/harness/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/harness/unity.c -o build/tests/harness/unity.c.o
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage build/tests/lab.c.o build/tests/main.c.o build/tests/lab-test.c.o build/tests/harness/unity.c.o -o build/tests/myapp_t -fprofile-arcs -ftest-coverage
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/lab.c -o build/debug-test/lab.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug-test/main.c.o
mkdir -p build/debug-test/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/lab-test.c -o build/debug-test/lab-test.c.o
mkdir -p build/debug-test/harness/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/harness/unity.c -o build/debug-test/harness/unity.c.o
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address build/debug-test/lab.c.o build/debug-test/main.c.o build/debug-test/lab-test.c.o build/debug-test/harness/unity.c.o -o build/debug-test/myapp_td -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
Builds completed. You can run the application with: ./build/release/myapp
You can run the debug build with: ./build/debug/myapp_d
You can run the test build with: ./build/tests/myapp_t
You can run the debug-test build with: ./build/debug-test/myapp_td
```

---

## Coverage Report

This section was generated by running `make report` in the project root directory.

```bash
tests/lab-test.c:962:test_checksum_rfc1071_example:PASS
tests/lab-test.c:963:test_checksum_odd_length_pads_with_zero:PASS
tests/lab-test.c:964:test_checksum_empty_and_carry:PASS
tests/lab-test.c:965:test_checksum_catches_every_single_bit_flip:PASS
tests/lab-test.c:966:test_encode_data_matches_worked_example:PASS
tests/lab-test.c:967:test_encode_ack_matches_worked_example:PASS
tests/lab-test.c:968:test_encode_uses_network_byte_order:PASS
tests/lab-test.c:969:test_encode_rejects_long_payload_and_small_buffer:PASS
tests/lab-test.c:970:test_decode_round_trip:PASS
tests/lab-test.c:971:test_decode_rejects_short_datagram:PASS
tests/lab-test.c:972:test_decode_rejects_length_mismatch:PASS
tests/lab-test.c:973:test_decode_rejects_length_over_limit:PASS
tests/lab-test.c:974:test_decode_rejects_unknown_type:PASS
tests/lab-test.c:975:test_decode_rejects_nonzero_reserved:PASS
tests/lab-test.c:976:test_decode_rejects_flipped_bit:PASS
tests/lab-test.c:977:test_receiver_delivers_in_order:PASS
tests/lab-test.c:978:test_receiver_reacks_duplicate:PASS
tests/lab-test.c:979:test_receiver_discards_packet_beyond_gap:PASS
tests/lab-test.c:980:test_receiver_fin_closes_and_lingers:PASS
tests/lab-test.c:981:test_receiver_empty_file_is_one_fin:PASS
tests/lab-test.c:982:test_receiver_ignores_ack:PASS
tests/lab-test.c:983:test_receiver_gives_up_when_idle:PASS
tests/lab-test.c:984:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:985:test_sender_packetizes_file:PASS
tests/lab-test.c:986:test_sender_empty_file_sends_only_fin:PASS
tests/lab-test.c:987:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:988:test_sender_window_full_waits:PASS
tests/lab-test.c:989:test_sender_cumulative_ack_slides_several:PASS
tests/lab-test.c:990:test_sender_ignores_duplicate_and_bogus_acks:PASS
tests/lab-test.c:991:test_sender_timeout_resends_whole_window:PASS
tests/lab-test.c:992:test_sender_stop_and_wait:PASS
tests/lab-test.c:993:test_sender_gives_up_after_ten_timeouts:PASS
tests/lab-test.c:994:test_transfer_clean_channel:PASS
tests/lab-test.c:995:test_transfer_lossy_channel_several_seeds:PASS
tests/lab-test.c:996:test_transfer_lossy_stop_and_wait:PASS
tests/lab-test.c:997:test_transfer_dead_channel_gives_up:PASS

-----------------------
36 Tests 0 Failures 0 Ignored 
OK
./build/tests/myapp_t
tests/lab-test.c:962:test_checksum_rfc1071_example:PASS
tests/lab-test.c:963:test_checksum_odd_length_pads_with_zero:PASS
tests/lab-test.c:964:test_checksum_empty_and_carry:PASS
tests/lab-test.c:965:test_checksum_catches_every_single_bit_flip:PASS
tests/lab-test.c:966:test_encode_data_matches_worked_example:PASS
tests/lab-test.c:967:test_encode_ack_matches_worked_example:PASS
tests/lab-test.c:968:test_encode_uses_network_byte_order:PASS
tests/lab-test.c:969:test_encode_rejects_long_payload_and_small_buffer:PASS
tests/lab-test.c:970:test_decode_round_trip:PASS
tests/lab-test.c:971:test_decode_rejects_short_datagram:PASS
tests/lab-test.c:972:test_decode_rejects_length_mismatch:PASS
tests/lab-test.c:973:test_decode_rejects_length_over_limit:PASS
tests/lab-test.c:974:test_decode_rejects_unknown_type:PASS
tests/lab-test.c:975:test_decode_rejects_nonzero_reserved:PASS
tests/lab-test.c:976:test_decode_rejects_flipped_bit:PASS
tests/lab-test.c:977:test_receiver_delivers_in_order:PASS
tests/lab-test.c:978:test_receiver_reacks_duplicate:PASS
tests/lab-test.c:979:test_receiver_discards_packet_beyond_gap:PASS
tests/lab-test.c:980:test_receiver_fin_closes_and_lingers:PASS
tests/lab-test.c:981:test_receiver_empty_file_is_one_fin:PASS
tests/lab-test.c:982:test_receiver_ignores_ack:PASS
tests/lab-test.c:983:test_receiver_gives_up_when_idle:PASS
tests/lab-test.c:984:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:985:test_sender_packetizes_file:PASS
tests/lab-test.c:986:test_sender_empty_file_sends_only_fin:PASS
tests/lab-test.c:987:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:988:test_sender_window_full_waits:PASS
tests/lab-test.c:989:test_sender_cumulative_ack_slides_several:PASS
tests/lab-test.c:990:test_sender_ignores_duplicate_and_bogus_acks:PASS
tests/lab-test.c:991:test_sender_timeout_resends_whole_window:PASS
tests/lab-test.c:992:test_sender_stop_and_wait:PASS
tests/lab-test.c:993:test_sender_gives_up_after_ten_timeouts:PASS
tests/lab-test.c:994:test_transfer_clean_channel:PASS
tests/lab-test.c:995:test_transfer_lossy_channel_several_seeds:PASS
tests/lab-test.c:996:test_transfer_lossy_stop_and_wait:PASS
tests/lab-test.c:997:test_transfer_dead_channel_gives_up:PASS

-----------------------
36 Tests 0 Failures 0 Ignored 
OK
mkdir -p ./build/report/html
mkdir -p ./build/report/txt
gcovr -r . --html --html-details --exclude-directories build/tests/harness --exclude '.*main\.c$' --exclude '.*test\.c$' -o ./build/report/html/coverage_report.html
(INFO) Reading coverage data...

(INFO) Writing coverage report...

gcovr -r . --txt                 --exclude-directories build/tests/harness --exclude '.*main\.c$' --exclude '.*test\.c$'
(INFO) Reading coverage data...

(INFO) Writing coverage report...

------------------------------------------------------------------------------
                           GCC Code Coverage Report
Directory: .
------------------------------------------------------------------------------
File                                       Lines     Exec  Cover   Missing
------------------------------------------------------------------------------
src/lab.c                                    172      172   100%
------------------------------------------------------------------------------
TOTAL                                        172      172   100%
------------------------------------------------------------------------------
```

---

## Address Sanitizer Report

This section was generated by running `make leak-test` in the project root directory.

```bash
tests/lab-test.c:962:test_checksum_rfc1071_example:PASS
tests/lab-test.c:963:test_checksum_odd_length_pads_with_zero:PASS
tests/lab-test.c:964:test_checksum_empty_and_carry:PASS
tests/lab-test.c:965:test_checksum_catches_every_single_bit_flip:PASS
tests/lab-test.c:966:test_encode_data_matches_worked_example:PASS
tests/lab-test.c:967:test_encode_ack_matches_worked_example:PASS
tests/lab-test.c:968:test_encode_uses_network_byte_order:PASS
tests/lab-test.c:969:test_encode_rejects_long_payload_and_small_buffer:PASS
tests/lab-test.c:970:test_decode_round_trip:PASS
tests/lab-test.c:971:test_decode_rejects_short_datagram:PASS
tests/lab-test.c:972:test_decode_rejects_length_mismatch:PASS
tests/lab-test.c:973:test_decode_rejects_length_over_limit:PASS
tests/lab-test.c:974:test_decode_rejects_unknown_type:PASS
tests/lab-test.c:975:test_decode_rejects_nonzero_reserved:PASS
tests/lab-test.c:976:test_decode_rejects_flipped_bit:PASS
tests/lab-test.c:977:test_receiver_delivers_in_order:PASS
tests/lab-test.c:978:test_receiver_reacks_duplicate:PASS
tests/lab-test.c:979:test_receiver_discards_packet_beyond_gap:PASS
tests/lab-test.c:980:test_receiver_fin_closes_and_lingers:PASS
tests/lab-test.c:981:test_receiver_empty_file_is_one_fin:PASS
tests/lab-test.c:982:test_receiver_ignores_ack:PASS
tests/lab-test.c:983:test_receiver_gives_up_when_idle:PASS
tests/lab-test.c:984:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:985:test_sender_packetizes_file:PASS
tests/lab-test.c:986:test_sender_empty_file_sends_only_fin:PASS
tests/lab-test.c:987:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:988:test_sender_window_full_waits:PASS
tests/lab-test.c:989:test_sender_cumulative_ack_slides_several:PASS
tests/lab-test.c:990:test_sender_ignores_duplicate_and_bogus_acks:PASS
tests/lab-test.c:991:test_sender_timeout_resends_whole_window:PASS
tests/lab-test.c:992:test_sender_stop_and_wait:PASS
tests/lab-test.c:993:test_sender_gives_up_after_ten_timeouts:PASS
tests/lab-test.c:994:test_transfer_clean_channel:PASS
tests/lab-test.c:995:test_transfer_lossy_channel_several_seeds:PASS
tests/lab-test.c:996:test_transfer_lossy_stop_and_wait:PASS
tests/lab-test.c:997:test_transfer_dead_channel_gives_up:PASS

-----------------------
36 Tests 0 Failures 0 Ignored 
OK
```

---

## Src Files
### lab.c

```c

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

```

### lab.h

```c

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

```

### main.c

```c

/*
 * Layer 3: the I/O. The command line, the socket, the relay hello, the poll
 * loop, the clock, and the file all live here, and only here. Each loop reads
 * one event, hands it to a state machine from lab.h, and carries out the
 * actions that come back.
 */

#include "lab.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

#define EXIT_USAGE 1
#define EXIT_FAIL 2

#define HELLO_ATTEMPTS 5
#define HELLO_WAIT_MS 1000

static const char USAGE[] =
    "Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n"
    "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
    "       myapp recv -s <session> [-p port] <relay> <file>\n"
    "\n"
    "  -s <session>     session name shared by the sender and the receiver\n"
    "  -w <window>      Go-Back-N window size in packets, 1 to 64 (default: 8)\n"
    "  -T <timeout-ms>  retransmission timeout in milliseconds (default: 250)\n"
    "  -l <loss>        probability the relay drops a packet (default: 0)\n"
    "  -c <corrupt>     probability the relay flips a bit (default: 0)\n"
    "  -d <dup>         probability the relay duplicates a packet (default: 0)\n"
    "  -p <port>        relay port (default: 4250)\n"
    "  <relay>          host name or address of the relay\n"
    "  <file>           file to send, or file to write what is received\n";

struct options
{
  bool send;
  const char *session;
  uint32_t window;
  uint32_t timeout_ms;
  const char *loss;
  const char *corrupt;
  const char *dup;
  const char *port;
  const char *relay;
  const char *file;
};

/* ------------------------------------------------------------------------ */
/* Command line                                                              */
/* ------------------------------------------------------------------------ */

static bool parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *out)
{
  if (!isdigit((unsigned char)text[0]))
  {
    return false; // strtoul would quietly accept "-1" and " 1"
  }
  errno = 0;
  char *end;
  unsigned long v = strtoul(text, &end, 10);
  if (errno != 0 || *end != '\0' || v < min || v > max)
  {
    return false;
  }
  *out = (uint32_t)v;
  return true;
}

/* A rate is a plain decimal from 0 to 0.5, the form the relay accepts. */
static bool valid_rate(const char *text)
{
  size_t digits = 0, dots = 0;
  for (const char *c = text; *c != '\0'; c++)
  {
    if (isdigit((unsigned char)*c))
    {
      digits++;
    }
    else if (*c == '.' && dots == 0)
    {
      dots++;
    }
    else
    {
      return false;
    }
  }
  size_t n = strlen(text);
  return digits > 0 && text[n - 1] != '.' && strtod(text, NULL) <= 0.5;
}

static bool valid_session(const char *text)
{
  size_t n = strlen(text);
  if (n < 1 || n > 32)
  {
    return false;
  }
  for (size_t i = 0; i < n; i++)
  {
    char c = text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
    {
      return false;
    }
  }
  return true;
}

static bool parse_options(int argc, char *argv[], struct options *o)
{
  *o = (struct options){.window = 8, .timeout_ms = 250, .loss = "0",
                        .corrupt = "0", .dup = "0", .port = "4250"};
  const char *mode = argv[1];
  if (strcmp(mode, "send") == 0)
  {
    o->send = true;
  }
  else if (strcmp(mode, "recv") != 0)
  {
    fprintf(stderr, "myapp: unknown mode '%s'\n", mode);
    return false;
  }

  // Parse what follows the mode, as if the mode were the program name.
  int sub_argc = argc - 1;
  char **sub_argv = argv + 1;
  const char *optstring = o->send ? ":s:w:T:l:c:d:p:" : ":s:p:";
  uint32_t port;
  int opt;
  opterr = 0;
  while ((opt = getopt(sub_argc, sub_argv, optstring)) != -1)
  {
    switch (opt)
    {
    case 's':
      o->session = optarg;
      break;
    case 'w':
      if (!parse_u32(optarg, GBN_MIN_WINDOW, GBN_MAX_WINDOW, &o->window))
      {
        fprintf(stderr, "myapp: window must be %d to %d\n", GBN_MIN_WINDOW,
                GBN_MAX_WINDOW);
        return false;
      }
      break;
    case 'T':
      if (!parse_u32(optarg, 1, 600000, &o->timeout_ms))
      {
        fprintf(stderr, "myapp: timeout must be 1 to 600000 ms\n");
        return false;
      }
      break;
    case 'l':
    case 'c':
    case 'd':
      if (!valid_rate(optarg))
      {
        fprintf(stderr, "myapp: -%c must be a decimal from 0 to 0.5\n", opt);
        return false;
      }
      *(opt == 'l' ? &o->loss : opt == 'c' ? &o->corrupt : &o->dup) = optarg;
      break;
    case 'p':
      if (!parse_u32(optarg, 1, 65535, &port))
      {
        fprintf(stderr, "myapp: port must be 1 to 65535\n");
        return false;
      }
      o->port = optarg;
      break;
    case ':':
      fprintf(stderr, "myapp: -%c needs a value\n", optopt);
      return false;
    default:
      fprintf(stderr, "myapp: unknown option -%c for %s\n", optopt, mode);
      return false;
    }
  }

  if (o->session == NULL || !valid_session(o->session))
  {
    fprintf(stderr, "myapp: -s needs a session name of 1 to 32 characters "
                    "from a-z, 0-9 and -\n");
    return false;
  }
  if (sub_argc - optind != 2)
  {
    fprintf(stderr, "myapp: expected <relay> <file>\n");
    return false;
  }
  o->relay = sub_argv[optind];
  o->file = sub_argv[optind + 1];
  return true;
}

/* ------------------------------------------------------------------------ */
/* Clock and socket                                                          */
/* ------------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Waits up to wait_ms for a datagram. Returns 1 if one is ready, 0 on
 * timeout, and -1 on an error poll should never report. */
static int wait_readable(int fd, uint64_t wait_ms)
{
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int timeout = wait_ms > INT_MAX ? INT_MAX : (int)wait_ms;
  int r = poll(&pfd, 1, timeout);
  if (r < 0)
  {
    return errno == EINTR ? 0 : -1;
  }
  return r;
}

/* Sends the hello and waits for the relay's answer, retrying once a second.
 * Returns 0 on OK, 1 when the relay refused (reason already printed), and -1
 * when it never answered. */
static int say_hello(int fd, const char *hello)
{
  char reply[PKT_MAX_LEN + 1];
  for (int attempt = 0; attempt < HELLO_ATTEMPTS; attempt++)
  {
    // A send error such as ECONNREFUSED just means nobody answered.
    (void)send(fd, hello, strlen(hello), 0);
    uint64_t deadline = now_ms() + HELLO_WAIT_MS;
    uint64_t now;
    while ((now = now_ms()) < deadline)
    {
      if (wait_readable(fd, deadline - now) <= 0)
      {
        continue;
      }
      ssize_t n = recv(fd, reply, sizeof reply - 1, 0);
      if (n < 0)
      {
        continue; // e.g. ECONNREFUSED: the relay is not there (yet)
      }
      reply[n] = '\0';
      if (strcmp(reply, "OK") == 0)
      {
        return 0;
      }
      if (strncmp(reply, "ERR", 3) == 0)
      {
        fprintf(stderr, "myapp: relay refused: %s\n",
                reply[3] == ' ' ? reply + 4 : reply + 3);
        return 1;
      }
      // Anything else is not an answer to this hello; keep waiting.
    }
  }
  return -1;
}

/* Resolves the relay, and registers with it from one connected UDP socket
 * that the rest of the run then uses. Returns the socket, or -1. */
static int open_relay(const struct options *o, const char *hello)
{
  struct addrinfo hints = {0}, *res, *ai;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int status = getaddrinfo(o->relay, o->port, &hints, &res);
  if (status != 0)
  {
    fprintf(stderr, "myapp: cannot resolve %s: %s\n", o->relay,
            gai_strerror(status));
    return -1;
  }

  int fd = -1, r = -1;
  for (ai = res; ai != NULL; ai = ai->ai_next)
  {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
    {
      continue;
    }
    // Connecting a UDP socket fixes its peer, so recv only ever returns
    // datagrams from the relay.
    r = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (r == 0)
    {
      r = say_hello(fd, hello);
    }
    if (r == 0)
    {
      break;
    }
    close(fd);
    fd = -1;
    if (r == 1)
    {
      break; // the relay answered with ERR: another address will not help
    }
  }
  freeaddrinfo(res);
  if (fd < 0 && r != 1)
  {
    fprintf(stderr, "myapp: no answer from the relay at %s port %s\n",
            o->relay, o->port);
  }
  return fd;
}

/* ------------------------------------------------------------------------ */
/* Sender                                                                    */
/* ------------------------------------------------------------------------ */

/* Reads a whole file into memory. Returns 0 or -1 with the reason printed. */
static int read_file(const char *path, uint8_t **data, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    fprintf(stderr, "myapp: cannot open %s: %s\n", path, strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_size > (off_t)GBN_MAX_FILE)
  {
    fprintf(stderr, "myapp: %s must be a regular file of at most 16 MiB\n", path);
    fclose(f);
    return -1;
  }
  *size = (size_t)st.st_size;
  *data = malloc(*size > 0 ? *size : 1);
  if (*data == NULL || fread(*data, 1, *size, f) != *size)
  {
    fprintf(stderr, "myapp: cannot read %s\n", path);
    free(*data);
    *data = NULL;
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static void transmit(int fd, const struct gbn_send_actions *out, uint64_t *sent)
{
  for (size_t i = 0; i < out->count; i++)
  {
    // A failed send is just a lost packet: the timer will cover it.
    (void)send(fd, out->pkt[i], out->len[i], 0);
  }
  *sent += out->count;
}

static int run_sender(const struct options *o)
{
  uint8_t *data;
  size_t size;
  if (read_file(o->file, &data, &size) != 0)
  {
    return EXIT_USAGE;
  }

  char hello[128];
  snprintf(hello, sizeof hello, "HELLO %s send %s %s %s", o->session, o->loss,
           o->corrupt, o->dup);
  int fd = open_relay(o, hello);
  if (fd < 0)
  {
    free(data);
    return EXIT_FAIL;
  }

  struct gbn_sender s;
  if (gbn_sender_init(&s, data, size, o->window, o->timeout_ms) != 0)
  {
    fprintf(stderr, "myapp: out of memory\n");
    close(fd);
    free(data);
    return EXIT_FAIL;
  }

  struct gbn_send_actions out;
  struct packet p;
  uint8_t buf[PKT_MAX_LEN + 1];
  uint64_t sent = 0, started = now_ms();
  int rc = EXIT_FAIL;

  gbn_sender_start(&s, started, &out);
  transmit(fd, &out, &sent);
  while (gbn_sender_state(&s) == GBN_RUNNING)
  {
    uint64_t now = now_ms();
    if (out.timer_on && now >= out.timer_due)
    {
      gbn_sender_on_timeout(&s, now, &out);
      transmit(fd, &out, &sent);
      continue;
    }
    int ready = wait_readable(fd, out.timer_on ? out.timer_due - now : 1000);
    if (ready < 0)
    {
      perror("myapp: poll");
      goto done;
    }
    if (ready == 0)
    {
      continue;
    }
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n < 0 || !pkt_decode(buf, (size_t)n, &p))
    {
      continue; // damaged, malformed, or an ICMP error: treat as lost
    }
    gbn_sender_on_packet(&s, &p, now_ms(), &out);
    transmit(fd, &out, &sent);
  }

  if (gbn_sender_state(&s) == GBN_DONE)
  {
    uint64_t needed = s.npkts + 1u;
    fprintf(stderr, "myapp: sent %zu bytes in %.3f s: %llu packets, "
                    "%llu of them retransmissions\n",
            size, (double)(now_ms() - started) / 1000.0,
            (unsigned long long)sent, (unsigned long long)(sent - needed));
    rc = EXIT_SUCCESS;
  }
  else
  {
    fprintf(stderr, "myapp: giving up after %d timeouts in a row with no "
                    "progress\n", GBN_MAX_TIMEOUTS);
  }

done:
  gbn_sender_free(&s);
  close(fd);
  free(data);
  return rc;
}

/* ------------------------------------------------------------------------ */
/* Receiver                                                                  */
/* ------------------------------------------------------------------------ */

static int run_receiver(const struct options *o)
{
  FILE *f = fopen(o->file, "wb");
  if (f == NULL)
  {
    fprintf(stderr, "myapp: cannot create %s: %s\n", o->file, strerror(errno));
    return EXIT_USAGE;
  }

  char hello[64];
  snprintf(hello, sizeof hello, "HELLO %s recv", o->session);
  int fd = open_relay(o, hello);
  if (fd < 0)
  {
    fclose(f);
    return EXIT_FAIL;
  }

  struct gbn_receiver r;
  struct gbn_recv_actions out;
  struct packet p;
  uint8_t buf[PKT_MAX_LEN + 1];
  int rc = EXIT_FAIL;

  gbn_receiver_init(&r, now_ms());
  uint64_t now;
  while (gbn_receiver_on_tick(&r, now = now_ms()) == GBN_RUNNING)
  {
    int ready = wait_readable(fd, gbn_receiver_deadline(&r) - now);
    if (ready < 0)
    {
      perror("myapp: poll");
      goto done;
    }
    if (ready == 0)
    {
      continue;
    }
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n < 0 || !pkt_decode(buf, (size_t)n, &p))
    {
      continue;
    }
    gbn_receiver_on_packet(&r, &p, now_ms(), &out);
    // Write before acknowledging, so an ACK never promises bytes we lost.
    if (out.deliver_len > 0 && fwrite(out.deliver, 1, out.deliver_len, f) !=
                                   out.deliver_len)
    {
      fprintf(stderr, "myapp: cannot write %s\n", o->file);
      goto done;
    }
    if (out.close_file)
    {
      int err = fclose(f);
      f = NULL;
      if (err != 0)
      {
        fprintf(stderr, "myapp: cannot write %s\n", o->file);
        goto done;
      }
    }
    if (out.send_ack)
    {
      (void)send(fd, out.ack, out.ack_len, 0);
    }
  }

  if (r.state == GBN_DONE)
  {
    rc = EXIT_SUCCESS;
  }
  else
  {
    fprintf(stderr, "myapp: nothing from the sender for %u seconds, giving up\n",
            GBN_IDLE_MS / 1000u);
  }

done:
  if (f != NULL)
  {
    fclose(f);
  }
  close(fd);
  return rc;
}

int main(int argc, char *argv[])
{
  if (argc < 2)
  {
    fputs(USAGE, stdout);
    return EXIT_SUCCESS;
  }
  struct options o;
  if (!parse_options(argc, argv, &o))
  {
    fputs(USAGE, stderr);
    return EXIT_USAGE;
  }
  return o.send ? run_sender(&o) : run_receiver(&o);
}

```

## Tests Files
### lab-test.c

```c

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

```

## Scripts Files
Report generated on 10/08/2026 at 21:53:04


---

## End of Report

SHA-256 Hash of the report: 33e4023a7d6d2c780b935df884f479f1d7daf20b4be5598132568b52fe08d721

Do not edit the generated report. Any changes will be reported as academic dishonesty

---
## GitHub Info
- GitHub repo name: lquainta/cs425-p2
- The repository visibility is public.
- The workflow was triggered by lquainta
