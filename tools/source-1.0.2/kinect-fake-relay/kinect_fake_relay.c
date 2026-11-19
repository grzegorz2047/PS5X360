// SPDX-License-Identifier: MIT
// kinect-fake-relay.elf: a TEST payload for the source build's Kinect relay path (fix 44), for a
// console without a Kinect. It takes kinect-tracker.elf's place on 127.0.0.1:9301 and serves the
// same stream (one client at a time; "KNCT", u8 stream (1 depth, 2 colour), 3 zero bytes, u32
// little-endian size, payload) with made-up frames at about 30 per second: an 11-bit depth image
// (a disc moving left and right in front of a far wall) and a Bayer colour image (moving bars).
// The emulator then logs "Kinect USB: receiving camera frames from the relay on 127.0.0.1:9301"
// and, when this payload goes, "Kinect USB: relay closed after N frames".
//
// It stops after 10 minutes, or as soon as /data/kinect-fake-relay.stop exists. It never touches
// a USB device or /data/kinect-tracker. Do not run it with the Helper payload's kinect-tracker.elf:
// the port is the tracker's, and the second one to start gives up. Log: /data/kinect-fake-relay.log.
//
// The host check (tools/check-kinect-fake-relay.py) builds it with the host's compiler and
// FAKE_RELAY_HOST: the log and stop file paths and the run time come from the environment.
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 9301
#define DEPTH_FRAME (640 * 480 * 11 / 8) /* 422400 */
#define COLOUR_FRAME (640 * 480)         /* 307200 */

static const char *log_path = "/data/kinect-fake-relay.log";
static const char *stop_path = "/data/kinect-fake-relay.stop";
static double run_seconds = 600;
static int logfd = -1;

static void say(const char *format, ...) {
  char line[256];
  va_list ap;
  va_start(ap, format);
  int n = vsnprintf(line, sizeof(line) - 1, format, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
  line[n++] = '\n';
  if (logfd >= 0) {
    if (write(logfd, line, (size_t)n) < 0) return;
    fsync(logfd);
  }
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

#ifdef FAKE_RELAY_HOST
static void notify(const char *text) { say("notification: %s", text); }
#else
// A notification on the PS5's screen (as kinect_tracker.c).
int sceKernelSendNotificationRequest(int, void *, size_t, int);
static void notify(const char *text) {
  static unsigned char request[3120];
  memset(request, 0, sizeof(request));
  for (int i = 0; i < 4; i++) request[0x10 + i] = 0xff;
  snprintf((char *)request + 45, 1024, "%s", text);
  sceKernelSendNotificationRequest(0, request, sizeof(request), 0);
}
#endif

// 11-bit values packed most significant bit first, as the camera sends them.
static void pack_depth(const uint16_t *raw, uint8_t *out) {
  uint32_t bits = 0;
  int count = 0;
  size_t at = 0;
  for (size_t n = 0; n < 640 * 480; n++) {
    bits = (bits << 11) | (raw[n] & 2047);
    count += 11;
    while (count >= 8) {
      count -= 8;
      out[at++] = (uint8_t)(bits >> count);
    }
  }
}

// Frame `k`: a disc at about 1.6 m (raw 880) moving across a wall at about 3.7 m (raw 1000).
static void make_frames(unsigned k, uint16_t *raw, uint8_t *depth, uint8_t *colour) {
  const int phase = (int)(k % 120), x0 = 120 + (phase < 60 ? phase : 120 - phase) * 6, y0 = 240;
  for (int y = 0; y < 480; y++)
    for (int x = 0; x < 640; x++) {
      const int dx = x - x0, dy = y - y0;
      uint16_t value = y > 400 ? 2047 : 1000;  // the floor's edge: no reading
      if (dx * dx + dy * dy < 110 * 110) value = 880;
      raw[y * 640 + x] = value;
      colour[y * 640 + x] = (uint8_t)(((x + (int)k * 4) / 40) % 2 ? 200 : 40);
    }
  pack_depth(raw, depth);
}

static int send_all(int fd, const void *data, size_t size) {
  const uint8_t *at = data;
  while (size) {
    ssize_t sent = send(fd, at, size, MSG_NOSIGNAL);
    if (sent <= 0) return -1;
    at += sent;
    size -= (size_t)sent;
  }
  return 0;
}

static int send_frame(int fd, uint8_t stream, const uint8_t *payload, uint32_t size) {
  const uint8_t header[12] = {'K', 'N', 'C', 'T', stream, 0, 0, 0,
                              (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16),
                              (uint8_t)(size >> 24)};
  return send_all(fd, header, sizeof(header)) || send_all(fd, payload, size) ? -1 : 0;
}

static int stopping(double end) { return now() >= end || !access(stop_path, F_OK); }

int main(void) {
#ifdef FAKE_RELAY_HOST
  if (getenv("FAKE_RELAY_LOG")) log_path = getenv("FAKE_RELAY_LOG");
  if (getenv("FAKE_RELAY_STOP")) stop_path = getenv("FAKE_RELAY_STOP");
  if (getenv("FAKE_RELAY_SECONDS")) run_seconds = atof(getenv("FAKE_RELAY_SECONDS"));
#endif
  logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  unlink(stop_path);
  int server = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  if (server < 0) {
    say("KFAKE: socket errno=%d", errno);
    return 1;
  }
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(PORT);
  address.sin_addr.s_addr = htonl(0x7f000001);
  if (bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 1)) {
    say("KFAKE: 127.0.0.1:%d is taken (errno=%d): kinect-tracker.elf or another relay runs; leaving",
        PORT, errno);
    notify("PS5X360 test: Kinect relay port taken, fake relay not started");
    close(server);
    return 1;
  }
  say("KFAKE: fake Kinect relay (TEST, made-up frames) listening on 127.0.0.1:%d for %.0f s", PORT,
      run_seconds);
  notify("PS5X360 test: fake Kinect relay running");
  static uint16_t raw[640 * 480];
  static uint8_t depth[DEPTH_FRAME], colour[COLOUR_FRAME];
  const double end = now() + run_seconds;
  unsigned total = 0;
  while (!stopping(end)) {
    struct pollfd waiting = {server, POLLIN, 0};
    if (poll(&waiting, 1, 500) <= 0) continue;
    int client = accept(server, 0, 0);
    if (client < 0) continue;
    say("KFAKE: emulator connected");
    // A client that stops reading is dropped after 2 s, so the stop file is still seen.
    const struct timeval send_timeout = {2, 0};
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    unsigned frames = 0;
    double next = now();
    while (!stopping(end)) {
      make_frames(total + frames, raw, depth, colour);
      if (send_frame(client, 1, depth, DEPTH_FRAME) || send_frame(client, 2, colour, COLOUR_FRAME))
        break;
      frames++;
      next += 1.0 / 30;
      const double wait = next - now();
      if (wait > 0) usleep((useconds_t)(wait * 1e6));
      else next = now();
    }
    close(client);
    total += frames;
    say("KFAKE: emulator disconnected after %u depth and %u colour frames", frames, frames);
  }
  close(server);
  say("KFAKE: stopped after %u frame pairs", total);
  notify("PS5X360 test: fake Kinect relay stopped");
  if (logfd >= 0) close(logfd);
  return 0;
}
