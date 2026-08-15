#include <errno.h>
#include <pthread.h>
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

static bool test_rtsp_parser(void)
{
    static const uint8_t response[] =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 17\r\n"
        "Content-Length: 4\r\n\r\n"
        "test";
    ap2_rtsp_response_t parsed;
    CHECK(ap2_io_parse_rtsp_response(
              response, sizeof(response) - 2, &parsed) == 0);
    CHECK(ap2_io_parse_rtsp_response(
              response, sizeof(response) - 1, &parsed) == 1);
    CHECK(parsed.status == 200);
    CHECK(parsed.cseq == 17);
    CHECK(parsed.body_len == 4);
    CHECK(parsed.message_len == sizeof(response) - 1);

    static const uint8_t malformed[] =
        "RTSP/1.0 200 OK\r\nCSeq: nope\r\nContent-Length: 0\r\n\r\n";
    CHECK(ap2_io_parse_rtsp_response(
              malformed, sizeof(malformed) - 1, &parsed) == -1);
    return true;
}

static bool test_feedback_miss_policy(void)
{
    /* Only /feedback, only timeout-shaped errors, only under the budget. */
    CHECK(ap2_io_feedback_miss_tolerated("/feedback", ETIMEDOUT, 0, 3));
    CHECK(ap2_io_feedback_miss_tolerated("/feedback", ETIMEDOUT, 1, 3));
    /* The final miss of the budget kills the channel. */
    CHECK(!ap2_io_feedback_miss_tolerated("/feedback", ETIMEDOUT, 2, 3));
    CHECK(!ap2_io_feedback_miss_tolerated("/feedback", ETIMEDOUT, 7, 3));
    /* Hard peer errors are never survivable. */
    CHECK(!ap2_io_feedback_miss_tolerated("/feedback", ECONNRESET, 0, 3));
    CHECK(!ap2_io_feedback_miss_tolerated("/feedback", EPIPE, 0, 3));
    CHECK(!ap2_io_feedback_miss_tolerated("/feedback", EPROTO, 0, 3));
    /* Other requests keep their single-strike behaviour. */
    CHECK(!ap2_io_feedback_miss_tolerated("/command", ETIMEDOUT, 0, 3));
    CHECK(!ap2_io_feedback_miss_tolerated("rtsp://x/session", ETIMEDOUT, 0, 3));
    CHECK(!ap2_io_feedback_miss_tolerated(NULL, ETIMEDOUT, 0, 3));
    /* A budget of one means no tolerance at all. */
    CHECK(!ap2_io_feedback_miss_tolerated("/feedback", ETIMEDOUT, 0, 1));
    return true;
}

static bool test_feedback_miss_budget_parser(void)
{
    unsigned budget = 3;
    /* Bare decimals inside the bounds are taken as-is. */
    CHECK(ap2_io_parse_feedback_miss_budget("1", 30, &budget) && budget == 1);
    CHECK(ap2_io_parse_feedback_miss_budget("8", 30, &budget) && budget == 8);
    CHECK(ap2_io_parse_feedback_miss_budget("30", 30, &budget) && budget == 30);
    /* Everything else is refused and leaves the caller's default alone. */
    budget = 3;
    CHECK(!ap2_io_parse_feedback_miss_budget(NULL, 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("0", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("31", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("-1", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("+4", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("4 ", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("4s", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("0x8", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("99999999999999999999", 30, &budget));
    CHECK(!ap2_io_parse_feedback_miss_budget("5", 30, NULL));
    CHECK(budget == 3);
    return true;
}

static bool test_match_rtsp_response_skips_stale(void)
{
    static const uint8_t stream[] =
        "RTSP/1.0 200 OK\r\nCSeq: 5\r\nContent-Length: 0\r\n\r\n"
        "RTSP/1.0 500 Internal Server Error\r\nCSeq: 6\r\n"
        "Content-Length: 2\r\n\r\nxy"
        "RTSP/1.0 200 OK\r\nCSeq: 7\r\nContent-Length: 4\r\n\r\nbody";
    const size_t len = sizeof(stream) - 1;
    ap2_rtsp_response_t parsed;
    size_t match_offset = 0;
    unsigned discarded = 0;

    /* An exact single response still matches with nothing discarded. */
    static const uint8_t single[] =
        "RTSP/1.0 200 OK\r\nCSeq: 7\r\nContent-Length: 4\r\n\r\nbody";
    CHECK(ap2_io_match_rtsp_response(single, sizeof(single) - 1, 7, &parsed,
                                     &match_offset, &discarded) == 1);
    CHECK(discarded == 0);
    CHECK(match_offset == 0);
    CHECK(parsed.status == 200 && parsed.body_len == 4);

    /* Two stale responses ahead of the expected one are skipped. */
    CHECK(ap2_io_match_rtsp_response(stream, len, 7, &parsed,
                                     &match_offset, &discarded) == 1);
    CHECK(discarded == 2);
    CHECK(parsed.status == 200);
    CHECK(parsed.cseq == 7);
    CHECK(parsed.body_len == 4);
    CHECK(memcmp(stream + match_offset + parsed.header_len, "body", 4) == 0);

    /* Only stale data so far: consumed entirely, more data needed. */
    CHECK(ap2_io_match_rtsp_response(stream, len, 9, &parsed,
                                     &match_offset, &discarded) == 0);
    CHECK(discarded == 3);

    /* Truncated tail after a stale prefix: more data needed. */
    CHECK(ap2_io_match_rtsp_response(stream, len - 2, 7, &parsed,
                                     &match_offset, &discarded) == 0);
    CHECK(discarded == 2);

    /* A response from the future means protocol desync. */
    CHECK(ap2_io_match_rtsp_response(stream, len, 4, &parsed,
                                     &match_offset, &discarded) == -1);

    /* Trailing bytes after the matched response are not allowed. */
    CHECK(ap2_io_match_rtsp_response(stream, len, 5, &parsed,
                                     &match_offset, &discarded) == -1);

    /* Malformed input propagates as -1. */
    static const uint8_t malformed[] =
        "RTSP/1.0 200 OK\r\nCSeq: nope\r\nContent-Length: 0\r\n\r\n";
    CHECK(ap2_io_match_rtsp_response(malformed, sizeof(malformed) - 1, 5,
                                     &parsed, &match_offset, &discarded) == -1);
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

typedef struct {
    pthread_mutex_t *rtsp_lock;
    int channel_fd;
    int cseq;
    uint64_t nonce;
    int channel_touches;
    int lock_result;
    int lock_errno;
    uint64_t elapsed_ms;
} rtsp_transaction_t;

static void *run_rtsp_transaction(void *arg)
{
    rtsp_transaction_t *transaction = arg;
    uint64_t started = ap2_io_monotonic_ms();
    transaction->lock_result = ap2_io_mutex_lock_deadline(
        transaction->rtsp_lock, started + 50);
    transaction->lock_errno = errno;
    transaction->elapsed_ms = ap2_io_monotonic_ms() - started;
    if (transaction->lock_result != 1) return NULL;

    transaction->cseq++;
    transaction->nonce++;
    if (send(transaction->channel_fd, "R", 1, 0) == 1)
        transaction->channel_touches++;
    pthread_mutex_unlock(transaction->rtsp_lock);
    return NULL;
}

static bool test_rtsp_lock_timeout_preserves_request_state(void)
{
    pthread_mutex_t rtsp_lock = PTHREAD_MUTEX_INITIALIZER;
    int channel[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, channel) == 0);
    CHECK(pthread_mutex_lock(&rtsp_lock) == 0);

    rtsp_transaction_t transaction = {
        .rtsp_lock = &rtsp_lock,
        .channel_fd = channel[0],
    };
    unsigned feedback_failures = 0;
    for (int tick = 0; tick < 3; tick++) {
        pthread_t thread;
        CHECK(pthread_create(
                  &thread, NULL, run_rtsp_transaction, &transaction) == 0);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(transaction.lock_result == 0);
        CHECK(transaction.lock_errno == ETIMEDOUT);
        CHECK(transaction.elapsed_ms >= 30 && transaction.elapsed_ms < 500);
        CHECK(transaction.cseq == 0);
        CHECK(transaction.nonce == 0);
        CHECK(transaction.channel_touches == 0);
        ap2_feedback_result_t result =
            ap2_io_feedback_result(-ETIMEDOUT, false);
        if (result == AP2_FEEDBACK_FAILED) feedback_failures++;
        CHECK(result == AP2_FEEDBACK_SKIPPED);
        CHECK(feedback_failures == 0);
    }

    uint8_t byte;
    errno = 0;
    CHECK(recv(channel[1], &byte, 1, MSG_DONTWAIT) < 0);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

    CHECK(pthread_mutex_unlock(&rtsp_lock) == 0);
    run_rtsp_transaction(&transaction);
    CHECK(transaction.lock_result == 1);
    CHECK(transaction.cseq == 1);
    CHECK(transaction.nonce == 1);
    CHECK(transaction.channel_touches == 1);
    CHECK(ap2_io_feedback_result(200, true) ==
          AP2_FEEDBACK_SUCCEEDED);
    CHECK(recv(channel[1], &byte, 1, MSG_DONTWAIT) == 1);
    CHECK(byte == 'R');

    CHECK(pthread_mutex_destroy(&rtsp_lock) == 0);
    close(channel[0]);
    close(channel[1]);
    return true;
}

static bool test_datagram_outcomes(void)
{
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    uint8_t payload[1024] = {0};
    CHECK(ap2_io_send_datagram_deadline(
              pair[0], payload, sizeof(payload), NULL, 0,
              ap2_io_monotonic_ms() + 40) == AP2_SEND_SENT);

    int sendbuf = 2048;
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF,
                     &sendbuf, sizeof(sendbuf)) == 0);
    while (send(pair[0], payload, sizeof(payload), MSG_DONTWAIT) >= 0) {}
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS);
    CHECK(ap2_io_send_datagram_deadline(
              pair[0], payload, sizeof(payload), NULL, 0,
              ap2_io_monotonic_ms() + 40) == AP2_SEND_DROPPED);

    close(pair[0]);
    CHECK(ap2_io_send_datagram_deadline(
              pair[0], payload, sizeof(payload), NULL, 0,
              ap2_io_monotonic_ms() + 40) == AP2_SEND_FATAL);
    close(pair[1]);
    return true;
}

static bool test_feedback_lock_timeouts_are_skipped(void)
{
    unsigned failures = 2;
    for (int tick = 0; tick < 4; tick++) {
        ap2_feedback_result_t result =
            ap2_io_feedback_result(-ETIMEDOUT, false);
        CHECK(result == AP2_FEEDBACK_SKIPPED);
        if (result == AP2_FEEDBACK_FAILED) failures++;
        CHECK(failures == 2);
    }

    CHECK(ap2_io_feedback_result(-ETIMEDOUT, true) ==
          AP2_FEEDBACK_FAILED);
    CHECK(ap2_io_feedback_result(500, true) == AP2_FEEDBACK_FAILED);
    CHECK(ap2_io_feedback_result(200, true) == AP2_FEEDBACK_SUCCEEDED);
    failures = 0;
    CHECK(failures == 0);
    return true;
}

/* A 401 challenge is the evidence an auth failure turns on: its header must be
 * readable out of the raw response, and the whole exchange must render as one
 * printable log line. */
static bool test_auth_response_diagnostics(void)
{
    static const uint8_t unauthorized[] =
        "RTSP/1.0 401 Unauthorized\r\n"
        "CSeq: 3\r\n"
        "WWW-Authenticate: Digest realm=\"raop\", nonce=\"abc\"\r\n"
        "Content-Length: 3\r\n\r\n"
        "\x01\x02\xff";
    const size_t len = sizeof(unauthorized) - 1;

    char value[128];
    CHECK(ap2_io_header_value(unauthorized, len, "www-authenticate",
                              value, sizeof(value)));
    CHECK(strcmp(value, "Digest realm=\"raop\", nonce=\"abc\"") == 0);
    CHECK(ap2_io_header_value(unauthorized, len, "CSeq", value, sizeof(value)));
    CHECK(strcmp(value, "3") == 0);
    /* Absent headers report as such, and the status line is not a header. */
    CHECK(!ap2_io_header_value(unauthorized, len, "Session", value, sizeof(value)));
    CHECK(value[0] == '\0');
    CHECK(!ap2_io_header_value(unauthorized, len, "RTSP/1.0", value, sizeof(value)));

    char dump[256];
    size_t written = ap2_io_format_response_dump(unauthorized, len, 512,
                                                 dump, sizeof(dump));
    CHECK(written == strlen(dump));
    CHECK(strstr(dump, "RTSP/1.0 401 Unauthorized | CSeq: 3 | ") == dump);
    CHECK(strstr(dump, "WWW-Authenticate: Digest") != NULL);
    CHECK(strstr(dump, "body[3]=0102ff") != NULL);
    CHECK(strchr(dump, '\n') == NULL && strchr(dump, '\r') == NULL);

    /* The body cap elides the rest instead of flooding the log. */
    CHECK(ap2_io_format_response_dump(unauthorized, len, 2, dump, sizeof(dump)));
    CHECK(strstr(dump, "body[3]=0102...") != NULL);

    /* A truncated reply (no header terminator) still renders, and a tiny
     * destination truncates instead of overflowing. */
    CHECK(ap2_io_format_response_dump(unauthorized, 12, 512, dump, sizeof(dump)));
    CHECK(strcmp(dump, "RTSP/1.0 401") == 0);
    char small[8];
    CHECK(ap2_io_format_response_dump(unauthorized, len, 512, small,
                                      sizeof(small)) == sizeof(small) - 1);
    CHECK(strcmp(small, "RTSP/1.") == 0);
    return true;
}

int main(void)
{
    if (!test_fake_peer_response()) return 1;
    fprintf(stderr, "fake peer response passed\n");
    if (!test_rtsp_parser()) return 1;
    fprintf(stderr, "RTSP parser passed\n");
    if (!test_feedback_miss_policy()) return 1;
    fprintf(stderr, "feedback miss policy passed\n");
    if (!test_feedback_miss_budget_parser()) return 1;
    fprintf(stderr, "feedback miss budget parser passed\n");
    if (!test_match_rtsp_response_skips_stale()) return 1;
    fprintf(stderr, "stale response matcher passed\n");
    if (!test_read_timeout_is_bounded()) return 1;
    fprintf(stderr, "read timeout passed\n");
    if (!test_stalled_write_is_bounded()) return 1;
    fprintf(stderr, "stalled write passed\n");
    if (!test_closed_peer_is_visible()) return 1;
    fprintf(stderr, "closed peer passed\n");
    if (!test_rtsp_lock_timeout_preserves_request_state()) return 1;
    fprintf(stderr, "RTSP lock deadline passed\n");
    if (!test_datagram_outcomes()) return 1;
    fprintf(stderr, "datagram outcomes passed\n");
    if (!test_feedback_lock_timeouts_are_skipped()) return 1;
    fprintf(stderr, "feedback contention accounting passed\n");
    if (!test_auth_response_diagnostics()) return 1;
    fprintf(stderr, "auth response diagnostics passed\n");
    puts("ap2_io tests passed");
    return 0;
}
