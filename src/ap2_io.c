/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

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

/* One buffer, one stdio call. POSIX requires stdio functions to lock the
 * FILE for the whole call, so this fwrite cannot interleave with another
 * thread's fprintf to the same stream - that lock, not the write itself, is
 * the serialization every emitter in this process shares (nothing writes the
 * fd raw). Staying under 512 bytes additionally keeps the line inside
 * PIPE_BUF, the backstop if such a writer ever appears. */
void ap2_io_status_vline(const char *fmt, va_list args)
{
    char line[512];
    int written = vsnprintf(line, sizeof(line) - 1, fmt, args);
    size_t len = written < 0 ? 0 : (size_t)written;
    if (len > sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';
    fwrite(line, 1, len, stderr);
    fflush(stderr);
}

void ap2_io_status_line(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ap2_io_status_vline(fmt, args);
    va_end(args);
}

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

bool ap2_io_feedback_miss_tolerated(const char *uri, int err,
                                    unsigned prior_misses,
                                    unsigned max_misses)
{
    if (!uri || strcmp(uri, "/feedback") != 0) return false;
    if (err != ETIMEDOUT) return false;
    return prior_misses + 1 < max_misses;
}

bool ap2_io_parse_feedback_miss_budget(const char *setting,
                                       unsigned max_budget,
                                       unsigned *budget)
{
    if (!setting || !*setting || !budget) return false;
    for (const char *c = setting; *c; c++)
        if (*c < '0' || *c > '9') return false;
    /* Bounded by max_budget, so a decimal longer than that can only be an
     * over-bound value; refuse it before strtoul can wrap. */
    if (strlen(setting) > 9) return false;
    unsigned long value = strtoul(setting, NULL, 10);
    if (value < 1 || value > max_budget) return false;
    *budget = (unsigned)value;
    return true;
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

bool ap2_io_header_value(const uint8_t *data, size_t len, const char *name,
                         char *out, size_t out_size)
{
    if (!out || !out_size) return false;
    out[0] = '\0';
    if (!data || !name) return false;

    size_t header_end = ap2_find_header_end(data, len);
    if (header_end >= 4) header_end -= 4;
    else header_end = len;

    size_t name_len = strlen(name);
    size_t offset = 0;
    bool status_line = true;
    while (offset < header_end) {
        size_t end = offset;
        while (end + 1 < header_end &&
               !(data[end] == '\r' && data[end + 1] == '\n'))
            end++;
        if (end + 1 >= header_end) end = header_end;
        if (status_line) {
            status_line = false;
            offset = end + 2;
            continue;
        }

        size_t colon = offset;
        while (colon < end && data[colon] != ':') colon++;
        if (colon < end && colon - offset == name_len &&
            strncasecmp((const char *)data + offset, name, name_len) == 0) {
            size_t value = colon + 1;
            while (value < end && (data[value] == ' ' || data[value] == '\t'))
                value++;
            size_t value_end = end;
            while (value_end > value &&
                   (data[value_end - 1] == ' ' || data[value_end - 1] == '\t'))
                value_end--;
            size_t copy = value_end - value;
            if (copy > out_size - 1) copy = out_size - 1;
            memcpy(out, data + value, copy);
            out[copy] = '\0';
            return true;
        }
        offset = end + 2;
    }
    return false;
}

static size_t ap2_dump_append(char *out, size_t out_size, size_t pos,
                              const char *text, size_t text_len)
{
    for (size_t i = 0; i < text_len && pos + 1 < out_size; i++)
        out[pos++] = text[i];
    out[pos] = '\0';
    return pos;
}

size_t ap2_io_format_response_dump(const uint8_t *data, size_t len,
                                   size_t body_cap, char *out, size_t out_size)
{
    if (!out || !out_size) return 0;
    out[0] = '\0';
    if (!data) return 0;

    size_t header_len = ap2_find_header_end(data, len);
    size_t header_end = header_len ? header_len - 4 : len;
    size_t pos = 0;

    /* Header block on one line: CRLF becomes " | " and any control byte folds
     * to '.', so a malformed or binary reply still logs as a single line. */
    for (size_t i = 0; i < header_end; i++) {
        if (data[i] == '\r' && i + 1 < header_end && data[i + 1] == '\n') {
            pos = ap2_dump_append(out, out_size, pos, " | ", 3);
            i++;
            continue;
        }
        char c = (char)data[i];
        if (c < 0x20 || c == 0x7f) c = '.';
        pos = ap2_dump_append(out, out_size, pos, &c, 1);
    }
    if (!header_len) return pos;

    size_t body_len = len > header_len ? len - header_len : 0;
    char head[48];
    int head_len = snprintf(head, sizeof(head), " | body[%zu]=", body_len);
    if (head_len > 0)
        pos = ap2_dump_append(out, out_size, pos, head, (size_t)head_len);

    size_t shown = body_len < body_cap ? body_len : body_cap;
    for (size_t i = 0; i < shown; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", data[header_len + i]);
        pos = ap2_dump_append(out, out_size, pos, hex, 2);
    }
    if (shown < body_len) pos = ap2_dump_append(out, out_size, pos, "...", 3);
    return pos;
}

int ap2_io_match_rtsp_response(const uint8_t *data, size_t len,
                               int expect_cseq,
                               ap2_rtsp_response_t *response,
                               size_t *match_offset, unsigned *discarded)
{
    if (discarded) *discarded = 0;
    size_t offset = 0;
    while (offset < len) {
        int parse_status =
            ap2_io_parse_rtsp_response(data + offset, len - offset, response);
        if (parse_status <= 0) return parse_status;
        if (response->cseq == expect_cseq) {
            if (offset + response->message_len != len) return -1;
            if (match_offset) *match_offset = offset;
            return 1;
        }
        if (response->cseq > expect_cseq) return -1;
        if (discarded) (*discarded)++;
        offset += response->message_len;
    }
    return 0;
}
