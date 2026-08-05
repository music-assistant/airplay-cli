/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AP2_IO_H_
#define __AP2_IO_H_

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef enum {
    AP2_SEND_FATAL = -1,
    AP2_SEND_DROPPED = 0,
    AP2_SEND_SENT = 1,
} ap2_send_result_t;

typedef enum {
    AP2_FEEDBACK_FAILED = -1,
    AP2_FEEDBACK_SKIPPED = 0,
    AP2_FEEDBACK_SUCCEEDED = 1,
} ap2_feedback_result_t;

typedef struct {
    int status;
    int cseq;
    size_t header_len;
    size_t body_len;
    size_t message_len;
} ap2_rtsp_response_t;

/*
 * Emit one [STATUS] line on stderr: one line, one write.
 *
 * Three threads emit these (the audio loop, the command pipe, the session
 * reader) and the human-readable LOG_* output shares the stream, so formatting
 * the body and the newline as two writes lets another thread land between them
 * and corrupt a line the caller then drops. The relied-on guarantee is POSIX
 * pipe atomicity: concurrent writes of at most PIPE_BUF bytes are not
 * interleaved, and PIPE_BUF is at least 512 everywhere (exactly 512 on macOS),
 * so the buffer is sized to emit at most 511 bytes. That is well past the
 * longest line we produce (a `mrp artwork=` report, ~200 chars); a line that
 * would still overflow is clamped rather than split. The flush is a no-op while
 * stderr stays unbuffered (set at startup) and is kept so a future buffering
 * change cannot silently delay status lines.
 *
 * This lives here rather than in cliairplay.c so the AP2 client's own status
 * emissions share the one implementation of the single-write guarantee.
 *
 * :param fmt: printf-style format for the line body, without the newline.
 */
void ap2_io_status_line(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* va_list form of ap2_io_status_line, for a caller wrapping it in its own
 * variadic helper. */
void ap2_io_status_vline(const char *fmt, va_list args);

uint64_t ap2_io_monotonic_ms(void);
/* Returns 1 when acquired, 0 on deadline expiry, and -1 on mutex error. */
int ap2_io_mutex_lock_deadline(pthread_mutex_t *mutex, uint64_t deadline_ms);
int ap2_io_poll_fd(int fd, short events, uint64_t deadline_ms);
bool ap2_io_write_all_deadline(int fd, const uint8_t *data, size_t len,
                               uint64_t deadline_ms);
ssize_t ap2_io_read_deadline(int fd, uint8_t *buf, size_t len,
                             uint64_t deadline_ms);
ap2_send_result_t ap2_io_send_datagram_deadline(
    int fd, const uint8_t *data, size_t len,
    const struct sockaddr *addr, socklen_t addr_len, uint64_t deadline_ms);

ap2_feedback_result_t ap2_io_feedback_result(int rtsp_status,
                                             bool request_started);

/* Returns true when a failed RTSP exchange may be survived as a keepalive
 * miss instead of killing the channel: only POST /feedback, only a
 * timeout-shaped error (a hard peer error means the connection is gone), and
 * only while fewer than max_misses consecutive beats have been missed
 * (prior_misses counts the misses before this one). */
bool ap2_io_feedback_miss_tolerated(const char *uri, int err,
                                    unsigned prior_misses,
                                    unsigned max_misses);

/* Returns 1 for a complete response, 0 for incomplete input, and -1 for
 * malformed input. */
int ap2_io_parse_rtsp_response(const uint8_t *data, size_t len,
                               ap2_rtsp_response_t *response);

/* Copy the value of the named header (case-insensitive, surrounding blanks
 * trimmed) out of a raw RTSP/HTTP response into out. Returns true when the
 * header is present; out is always NUL-terminated. */
bool ap2_io_header_value(const uint8_t *data, size_t len, const char *name,
                         char *out, size_t out_size);

/*
 * Render a raw RTSP/HTTP response as one printable diagnostic line: the status
 * line and every header joined with " | ", then the body as hex. Used to log
 * what a receiver actually answered when an exchange fails (a 401 challenge,
 * a TLV error body); the body is capped so a large reply cannot flood the log.
 *
 * :param data: raw response bytes (headers, and body when present).
 * :param len: number of valid bytes in data.
 * :param body_cap: maximum number of body bytes rendered; the rest is elided.
 * :param out: destination buffer, always NUL-terminated (truncated to fit).
 * :param out_size: size of out in bytes.
 * :returns: number of characters written, excluding the NUL.
 */
size_t ap2_io_format_response_dump(const uint8_t *data, size_t len,
                                   size_t body_cap, char *out, size_t out_size);

/* Locate the response with expect_cseq in a buffer that may start with
 * complete responses left over from abandoned (timed-out) requests; those
 * stale responses (lower CSeq) are skipped and counted in *discarded.
 * Returns 1 with *response and *match_offset set when the expected response is
 * found (it must end the buffer: with one outstanding request nothing may
 * trail it), 0 when more data is needed, and -1 for malformed input, a
 * response from the future or trailing bytes after the match. */
int ap2_io_match_rtsp_response(const uint8_t *data, size_t len,
                               int expect_cseq,
                               ap2_rtsp_response_t *response,
                               size_t *match_offset, unsigned *discarded);

#endif
