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
