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

The biggest design decision was where to draw the line between layer 2 and
layer 3. Making every state machine function take `now` as a parameter and
return an actions struct, instead of calling `send` or reading the clock
itself, is what made the lossy end-to-end test possible: the whole transfer
runs in simulated time in a fraction of a second, and a failing seed replays
exactly.

A few details were easy to get wrong:

- The FIN has to wait until every DATA packet is acknowledged, and it has to be
  kept in a window slot like any other packet so a timeout can resend it.
- An ACK larger than `next` acknowledges something never sent. It can only come
  from damage the checksum missed, but accepting it would let `base` run past
  `next`, so the sender ignores it.
- Coverage on macOS reported missed branches inside `htons`, which is a macro
  that branches on whether its argument is a constant. Those belong to the
  system headers, so they are excluded.
- AddressSanitizer on macOS cannot detect leaks (`detect_leaks is not supported
  on this platform`), so leaks were checked locally with `leaks --atExit` and
  for real by `make leak-test` in CI on Linux.

## AI Use

This project was written with Claude Code (Anthropic's command line coding
agent), as the course AI policy allows. The agent read the assignment and the
relay, proposed the three layer design, wrote the code and the tests, ran the
measurements in the Results section through the real relay, and drafted this
README.
