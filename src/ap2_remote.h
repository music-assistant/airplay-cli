/*
 * AirPlay remote-command types shared by the protocol layers and CLI.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AP2_REMOTE_H_
#define __AP2_REMOTE_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AP2_REMOTE_COMMAND_PLAY = 0,
    AP2_REMOTE_COMMAND_PAUSE,
    AP2_REMOTE_COMMAND_PLAY_PAUSE,
    AP2_REMOTE_COMMAND_NEXT,
    AP2_REMOTE_COMMAND_PREVIOUS,
    AP2_REMOTE_COMMAND_SEEK,
} ap2_remote_command_t;

/* position_ms carries the target of a SEEK and is -1 for every other command. */
typedef void (*ap2_remote_command_cb_t)(ap2_remote_command_t command,
                                        int64_t position_ms,
                                        void *userdata);

static inline const char *ap2_remote_command_name(ap2_remote_command_t command)
{
    switch (command) {
    case AP2_REMOTE_COMMAND_PLAY: return "play";
    case AP2_REMOTE_COMMAND_PAUSE: return "pause";
    case AP2_REMOTE_COMMAND_PLAY_PAUSE: return "play_pause";
    case AP2_REMOTE_COMMAND_NEXT: return "next";
    case AP2_REMOTE_COMMAND_PREVIOUS: return "previous";
    case AP2_REMOTE_COMMAND_SEEK: return "seek";
    default: return NULL;
    }
}

#endif /* __AP2_REMOTE_H_ */
