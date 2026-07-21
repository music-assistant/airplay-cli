#ifndef __AP2_IO_H_
#define __AP2_IO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef enum {
    AP2_IO_DATAGRAM_FATAL = -1,
    AP2_IO_DATAGRAM_DROPPED = 0,
    AP2_IO_DATAGRAM_SENT = 1,
} ap2_io_datagram_result_t;

typedef struct {
    int status;
    int cseq;
    size_t header_len;
    size_t body_len;
    size_t message_len;
} ap2_rtsp_response_t;

uint64_t ap2_io_monotonic_ms(void);
bool ap2_io_set_nonblocking(int fd);
int ap2_io_poll_fd(int fd, short events, uint64_t deadline_ms);
bool ap2_io_write_all_deadline(int fd, const uint8_t *data, size_t len,
                               uint64_t deadline_ms);
ssize_t ap2_io_read_deadline(int fd, uint8_t *buf, size_t len,
                             uint64_t deadline_ms);
ap2_io_datagram_result_t ap2_io_send_datagram_deadline(
    int fd, const uint8_t *data, size_t len,
    const struct sockaddr *address, socklen_t address_len,
    uint64_t deadline_ms);
/* Returns 1 for a complete response, 0 for incomplete input, and -1 for
 * malformed input. */
int ap2_io_parse_rtsp_response(const uint8_t *data, size_t len,
                               ap2_rtsp_response_t *response);

#endif /* __AP2_IO_H_ */
