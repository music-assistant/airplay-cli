#include "ap2_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

uint64_t ap2_io_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int ap2_io_mutex_lock_deadline(pthread_mutex_t *mutex, uint64_t deadline_ms)
{
    if (!mutex) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        uint64_t now = ap2_io_monotonic_ms();
        if (now >= deadline_ms) {
            errno = ETIMEDOUT;
            return 0;
        }

        int err = pthread_mutex_trylock(mutex);
        if (err == 0) return 1;
        if (err != EBUSY) {
            errno = err;
            return -1;
        }

        uint64_t remaining_ms = deadline_ms - now;
        long sleep_ns = remaining_ms > 1
                            ? 1000000L
                            : (long)remaining_ms * 1000000L;
        struct timespec delay = {.tv_nsec = sleep_ns};
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    }
}

int ap2_io_poll_fd(int fd, short events, uint64_t deadline_ms)
{
    for (;;) {
        uint64_t now = ap2_io_monotonic_ms();
        if (now >= deadline_ms) {
            errno = ETIMEDOUT;
            return 0;
        }
        uint64_t remaining = deadline_ms - now;
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
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

bool ap2_io_write_all_deadline(int fd, const uint8_t *data, size_t len,
                               uint64_t deadline_ms)
{
    int original_flags = fcntl(fd, F_GETFL);
    if (original_flags < 0) return false;
    bool restore_flags = !(original_flags & O_NONBLOCK);
    if (restore_flags && fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0)
        return false;

    bool ok = false;
    size_t offset = 0;
    while (offset < len) {
        if (ap2_io_monotonic_ms() >= deadline_ms) {
            errno = ETIMEDOUT;
            break;
        }
        if (ap2_io_poll_fd(fd, POLLOUT, deadline_ms) <= 0) break;
#ifdef MSG_NOSIGNAL
        ssize_t written = send(fd, data + offset, len - offset,
                               MSG_DONTWAIT | MSG_NOSIGNAL);
#else
        ssize_t written = send(fd, data + offset, len - offset, MSG_DONTWAIT);
#endif
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 &&
                   (errno == EINTR || errno == EAGAIN ||
                    errno == EWOULDBLOCK || errno == ENOBUFS)) {
            continue;
        } else {
            if (written == 0) errno = EPIPE;
            break;
        }
    }
    if (offset == len) ok = true;
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

    ssize_t result = -1;
    for (;;) {
        if (ap2_io_poll_fd(fd, POLLIN, deadline_ms) <= 0) break;
        result = recv(fd, buf, len, MSG_DONTWAIT);
        if (result < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (result == 0) errno = ECONNRESET;
        break;
    }
    int saved_errno = errno;
    if (restore_flags) fcntl(fd, F_SETFL, original_flags);
    errno = saved_errno;
    return result;
}

ap2_send_result_t ap2_io_send_datagram_deadline(
    int fd, const uint8_t *data, size_t len,
    const struct sockaddr *addr, socklen_t addr_len, uint64_t deadline_ms)
{
    bool backpressured = false;
    for (;;) {
        if (backpressured && ap2_io_monotonic_ms() >= deadline_ms) {
            errno = ETIMEDOUT;
            return AP2_SEND_DROPPED;
        }

        ssize_t sent = addr
                           ? sendto(fd, data, len, MSG_DONTWAIT, addr, addr_len)
                           : send(fd, data, len, MSG_DONTWAIT);
        if (sent == (ssize_t)len) return AP2_SEND_SENT;
        if (sent >= 0) {
            errno = EMSGSIZE;
            return AP2_SEND_FATAL;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS)
            return AP2_SEND_FATAL;

        backpressured = true;
        int ready = ap2_io_poll_fd(fd, POLLOUT, deadline_ms);
        if (ready > 0) continue;
        if (ready == 0) return AP2_SEND_DROPPED;
        return AP2_SEND_FATAL;
    }
}

ap2_feedback_result_t ap2_io_feedback_result(int rtsp_status,
                                             bool request_started)
{
    if (rtsp_status == 200) return AP2_FEEDBACK_SUCCEEDED;
    if (rtsp_status == -ETIMEDOUT && !request_started)
        return AP2_FEEDBACK_SKIPPED;
    return AP2_FEEDBACK_FAILED;
}

static size_t ap2_find_header_end(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}

static bool ap2_parse_decimal(const uint8_t *data, size_t len, long *value)
{
    if (!len || len >= 32) return false;
    char text[32];
    memcpy(text, data, len);
    text[len] = '\0';
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || parsed < 0) return false;
    *value = parsed;
    return true;
}

int ap2_io_parse_rtsp_response(const uint8_t *data, size_t len,
                               ap2_rtsp_response_t *response)
{
    if (!data || !response) return -1;
    size_t header_len = ap2_find_header_end(data, len);
    if (!header_len) return 0;

    const uint8_t *line_end = NULL;
    for (size_t i = 0; i + 1 < header_len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            line_end = data + i;
            break;
        }
    }
    if (!line_end) return -1;

    int status = 0;
    char status_line[128];
    size_t status_len = (size_t)(line_end - data);
    if (status_len >= sizeof(status_line)) return -1;
    memcpy(status_line, data, status_len);
    status_line[status_len] = '\0';
    if (sscanf(status_line, "RTSP/%*s %d", &status) != 1 || status <= 0)
        return -1;

    size_t body_len = 0;
    int cseq = -1;
    size_t offset = status_len + 2;
    while (offset + 2 <= header_len) {
        size_t end = offset;
        while (end + 1 < header_len &&
               !(data[end] == '\r' && data[end + 1] == '\n'))
            end++;
        if (end == offset) break;

        size_t colon = offset;
        while (colon < end && data[colon] != ':') colon++;
        if (colon == end) return -1;
        size_t value = colon + 1;
        while (value < end && (data[value] == ' ' || data[value] == '\t'))
            value++;
        size_t value_end = end;
        while (value_end > value &&
               (data[value_end - 1] == ' ' || data[value_end - 1] == '\t'))
            value_end--;

        long parsed = 0;
        size_t name_len = colon - offset;
        if (name_len == 14 &&
            strncasecmp((const char *)data + offset, "Content-Length", 14) == 0) {
            if (!ap2_parse_decimal(data + value, value_end - value, &parsed))
                return -1;
            body_len = (size_t)parsed;
        } else if (name_len == 4 &&
                   strncasecmp((const char *)data + offset, "CSeq", 4) == 0) {
            if (!ap2_parse_decimal(data + value, value_end - value, &parsed) ||
                parsed > INT_MAX)
                return -1;
            cseq = (int)parsed;
        }
        offset = end + 2;
    }

    if (body_len > SIZE_MAX - header_len) return -1;
    size_t message_len = header_len + body_len;
    if (len < message_len) return 0;
    response->status = status;
    response->cseq = cseq;
    response->header_len = header_len;
    response->body_len = body_len;
    response->message_len = message_len;
    return 1;
}
