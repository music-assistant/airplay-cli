#include "ap2_io.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

uint64_t ap2_io_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int ap2_io_poll_fd(int fd, short events, uint64_t deadline_ms)
{
    for (;;) {
        uint64_t now = ap2_io_monotonic_ms();
        if (now >= deadline_ms) {
            errno = ETIMEDOUT;
            return 0;
        }
        int timeout = (int)(deadline_ms - now);
        struct pollfd pfd = {.fd = fd, .events = events};
        int ret = poll(&pfd, 1, timeout);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) {
            if (ret == 0) errno = ETIMEDOUT;
            return ret;
        }
        if (pfd.revents & events) return 1;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ECONNRESET;
            return -1;
        }
    }
}

bool ap2_io_write_all_deadline(int fd, const uint8_t *data, int len,
                               uint64_t deadline_ms)
{
    int original_flags = fcntl(fd, F_GETFL);
    if (original_flags < 0) return false;
    bool restore_flags = !(original_flags & O_NONBLOCK);
    if (restore_flags && fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0)
        return false;

    bool ok = false;
    int off = 0;
    while (off < len) {
        if (ap2_io_poll_fd(fd, POLLOUT, deadline_ms) <= 0) break;
        ssize_t n = send(fd, data + off, (size_t)(len - off), MSG_DONTWAIT);
        if (n > 0) {
            off += (int)n;
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                             errno == EWOULDBLOCK)) {
            continue;
        } else {
            if (n == 0) errno = EPIPE;
            break;
        }
    }
    if (off == len) ok = true;
    int saved_errno = errno;
    if (restore_flags) fcntl(fd, F_SETFL, original_flags);
    errno = saved_errno;
    return ok;
}

ssize_t ap2_io_read_deadline(int fd, uint8_t *buf, size_t len,
                             uint64_t deadline_ms)
{
    int original_flags = fcntl(fd, F_GETFL);
    if (original_flags < 0) return -1;
    bool restore_flags = !(original_flags & O_NONBLOCK);
    if (restore_flags && fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0)
        return -1;

    ssize_t result;
    for (;;) {
        if (ap2_io_poll_fd(fd, POLLIN, deadline_ms) <= 0) {
            result = -1;
            break;
        }
        ssize_t n = recv(fd, buf, len, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                      errno == EWOULDBLOCK))
            continue;
        if (n == 0) errno = ECONNRESET;
        result = n;
        break;
    }
    int saved_errno = errno;
    if (restore_flags) fcntl(fd, F_SETFL, original_flags);
    errno = saved_errno;
    return result;
}
