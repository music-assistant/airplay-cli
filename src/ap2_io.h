#ifndef __AP2_IO_H_
#define __AP2_IO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

uint64_t ap2_io_monotonic_ms(void);
int ap2_io_poll_fd(int fd, short events, uint64_t deadline_ms);
bool ap2_io_write_all_deadline(int fd, const uint8_t *data, int len,
                               uint64_t deadline_ms);
ssize_t ap2_io_read_deadline(int fd, uint8_t *buf, size_t len,
                             uint64_t deadline_ms);

#endif
