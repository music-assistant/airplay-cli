#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ap2_io.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static bool test_fake_peer_response(void)
{
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char response[] =
        "RTSP/1.0 200 OK\r\nCSeq: 7\r\nContent-Length: 0\r\n\r\n";
    CHECK(write(pair[1], response, sizeof(response) - 1) ==
          (ssize_t)(sizeof(response) - 1));

    uint8_t buf[128] = {0};
    ssize_t n = ap2_io_read_deadline(
        pair[0], buf, sizeof(buf), ap2_io_monotonic_ms() + 100);
    CHECK(n == (ssize_t)(sizeof(response) - 1));
    CHECK(memcmp(buf, response, sizeof(response) - 1) == 0);
    close(pair[0]);
    close(pair[1]);
    return true;
}

static bool test_read_timeout_is_bounded(void)
{
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    uint8_t byte;
    uint64_t started = ap2_io_monotonic_ms();
    errno = 0;
    ssize_t n = ap2_io_read_deadline(pair[0], &byte, 1, started + 80);
    uint64_t elapsed = ap2_io_monotonic_ms() - started;
    CHECK(n < 0);
    CHECK(errno == ETIMEDOUT);
    CHECK(elapsed >= 50 && elapsed < 500);
    close(pair[0]);
    close(pair[1]);
    return true;
}

static bool test_stalled_write_is_bounded(void)
{
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int sendbuf = 4096;
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF,
                     &sendbuf, sizeof(sendbuf)) == 0);

    int len = 4 * 1024 * 1024;
    uint8_t *payload = malloc((size_t)len);
    CHECK(payload != NULL);
    memset(payload, 0x5a, (size_t)len);
    uint64_t started = ap2_io_monotonic_ms();
    errno = 0;
    bool ok = ap2_io_write_all_deadline(
        pair[0], payload, len, started + 100);
    uint64_t elapsed = ap2_io_monotonic_ms() - started;
    CHECK(!ok);
    CHECK(errno == ETIMEDOUT);
    CHECK(elapsed >= 50 && elapsed < 1000);
    free(payload);
    close(pair[0]);
    close(pair[1]);
    return true;
}

static bool test_closed_peer_is_visible(void)
{
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    close(pair[1]);
    uint8_t byte;
    errno = 0;
    ssize_t n = ap2_io_read_deadline(
        pair[0], &byte, 1, ap2_io_monotonic_ms() + 100);
    CHECK(n == 0);
    CHECK(errno == ECONNRESET);
    close(pair[0]);
    return true;
}

int main(void)
{
    if (!test_fake_peer_response()) return 1;
    fprintf(stderr, "fake peer response passed\n");
    if (!test_read_timeout_is_bounded()) return 1;
    fprintf(stderr, "read timeout passed\n");
    if (!test_stalled_write_is_bounded()) return 1;
    fprintf(stderr, "stalled write passed\n");
    if (!test_closed_peer_is_visible()) return 1;
    fprintf(stderr, "closed peer passed\n");
    puts("ap2_io tests passed");
    return 0;
}
