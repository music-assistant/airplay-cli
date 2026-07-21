#include "ap2_io.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>

uint64_t ap2_io_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

bool ap2_io_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && (flags & O_NONBLOCK ||
                          fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
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

static int ap2_io_send_flags(void)
{
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return flags;
}

bool ap2_io_write_all_deadline(int fd, const uint8_t *data, int len,
                               uint64_t deadline_ms)
{
    int original_flags = fcntl(fd, F_GETFL);
    if (original_flags < 0) return false;
    bool restore_flags = !(original_flags & O_NONBLOCK);
    if (restore_flags &&
        fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0)
        return false;

    bool ok = false;
    int off = 0;
    while (off < len) {
        if (ap2_io_poll_fd(fd, POLLOUT, deadline_ms) <= 0) break;
        ssize_t n = send(fd, data + off, (size_t)(len - off),
                         ap2_io_send_flags());
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
    if (restore_flags &&
        fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0)
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

ap2_io_datagram_result_t ap2_io_send_datagram_deadline(
    int fd, const uint8_t *data, size_t len,
    const struct sockaddr *address, socklen_t address_len,
    uint64_t deadline_ms)
{
    for (;;) {
        ssize_t sent = address
                           ? sendto(fd, data, len, ap2_io_send_flags(),
                                    address, address_len)
                           : send(fd, data, len, ap2_io_send_flags());
        if (sent == (ssize_t)len) return AP2_IO_DATAGRAM_SENT;
        if (sent >= 0) {
            errno = EIO;
            return AP2_IO_DATAGRAM_FATAL;
        }
        if (errno == EINTR) {
            if (ap2_io_monotonic_ms() >= deadline_ms) {
                errno = ETIMEDOUT;
                return AP2_IO_DATAGRAM_DROPPED;
            }
            continue;
        }
        if (errno == ENOBUFS) return AP2_IO_DATAGRAM_DROPPED;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return AP2_IO_DATAGRAM_FATAL;
        int ready = ap2_io_poll_fd(fd, POLLOUT, deadline_ms);
        if (ready > 0) continue;
        if (ready == 0) return AP2_IO_DATAGRAM_DROPPED;
        return AP2_IO_DATAGRAM_FATAL;
    }
}
