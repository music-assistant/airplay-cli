#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ap2_feedback.h"
#include "ap2_io.h"

static void test_rtsp_parser(void)
{
    static const uint8_t response[] =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 17\r\n"
        "Content-Length: 4\r\n\r\n"
        "test";
    ap2_rtsp_response_t parsed;
    assert(ap2_io_parse_rtsp_response(
               response, sizeof(response) - 2, &parsed) == 0);
    assert(ap2_io_parse_rtsp_response(
               response, sizeof(response) - 1, &parsed) == 1);
    assert(parsed.status == 200);
    assert(parsed.cseq == 17);
    assert(parsed.body_len == 4);
    assert(parsed.message_len == sizeof(response) - 1);

    static const uint8_t malformed[] =
        "RTSP/1.0 200 OK\r\nCSeq: nope\r\nContent-Length: 0\r\n\r\n";
    assert(ap2_io_parse_rtsp_response(
               malformed, sizeof(malformed) - 1, &parsed) == -1);
}

static void test_deadline_io(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    uint8_t byte;
    uint64_t started = ap2_io_monotonic_ms();
    errno = 0;
    assert(ap2_io_read_deadline(pair[0], &byte, 1, started + 40) < 0);
    assert(errno == ETIMEDOUT);
    assert(ap2_io_monotonic_ms() - started < 500);
    puts("  read timeout");
    fflush(stdout);

    int sendbuf = 4096;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF,
                      &sendbuf, sizeof(sendbuf)) == 0);
    uint8_t payload[65536];
    memset(payload, 0x5a, sizeof(payload));
    started = ap2_io_monotonic_ms();
    errno = 0;
    assert(!ap2_io_write_all_deadline(
        pair[0], payload, sizeof(payload), started + 40));
    assert(errno == ETIMEDOUT);
    assert(ap2_io_monotonic_ms() - started < 500);
    puts("  write timeout");
    fflush(stdout);
    close(pair[0]);
    close(pair[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    close(pair[1]);
    errno = 0;
    assert(ap2_io_read_deadline(
               pair[0], &byte, 1, ap2_io_monotonic_ms() + 40) == 0);
    assert(errno == ECONNRESET);
    puts("  peer close");
    fflush(stdout);
    close(pair[0]);
}

static void test_datagram_outcomes(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    uint8_t payload[1024] = {0};
    assert(ap2_io_send_datagram_deadline(
               pair[0], payload, sizeof(payload), NULL, 0,
               ap2_io_monotonic_ms() + 40) == AP2_SEND_SENT);

    int sendbuf = 2048;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF,
                      &sendbuf, sizeof(sendbuf)) == 0);
    while (send(pair[0], payload, sizeof(payload), MSG_DONTWAIT) >= 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS);
    assert(ap2_io_send_datagram_deadline(
               pair[0], payload, sizeof(payload), NULL, 0,
               ap2_io_monotonic_ms() + 40) == AP2_SEND_DROPPED);

    close(pair[0]);
    assert(ap2_io_send_datagram_deadline(
               pair[0], payload, sizeof(payload), NULL, 0,
               ap2_io_monotonic_ms() + 40) == AP2_SEND_FATAL);
    close(pair[1]);
}

static atomic_uint worker_ticks;

static bool worker_tick(void *arg)
{
    (void)arg;
    atomic_fetch_add(&worker_ticks, 1);
    return true;
}

static void test_worker_without_cmdpipe(void)
{
    ap2_periodic_worker_t worker;
    atomic_init(&worker_ticks, 0);
    assert(ap2_periodic_worker_init(&worker, 20, worker_tick, NULL));
    assert(ap2_periodic_worker_start(&worker));
    usleep(85000);
    ap2_periodic_worker_stop(&worker);
    unsigned stopped_at = atomic_load(&worker_ticks);
    assert(stopped_at >= 3);
    usleep(30000);
    assert(atomic_load(&worker_ticks) == stopped_at);
    ap2_periodic_worker_destroy(&worker);
}

int main(void)
{
    puts("rtsp parser");
    fflush(stdout);
    test_rtsp_parser();
    puts("deadline io");
    fflush(stdout);
    test_deadline_io();
    puts("datagram outcomes");
    fflush(stdout);
    test_datagram_outcomes();
    puts("worker");
    fflush(stdout);
    test_worker_without_cmdpipe();
    puts("ap2_io tests passed");
    return 0;
}
