/*
 * AirPlay remote-command types shared by the protocol layers and CLI.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_REMOTE_H_
#define __AP2_REMOTE_H_

#include <stddef.h>

typedef enum {
    AP2_REMOTE_COMMAND_PLAY = 0,
    AP2_REMOTE_COMMAND_PAUSE,
    AP2_REMOTE_COMMAND_PLAY_PAUSE,
    AP2_REMOTE_COMMAND_NEXT,
    AP2_REMOTE_COMMAND_PREVIOUS,
} ap2_remote_command_t;

typedef void (*ap2_remote_command_cb_t)(ap2_remote_command_t command,
                                        void *userdata);

static inline const char *ap2_remote_command_name(ap2_remote_command_t command)
{
    switch (command) {
    case AP2_REMOTE_COMMAND_PLAY: return "play";
    case AP2_REMOTE_COMMAND_PAUSE: return "pause";
    case AP2_REMOTE_COMMAND_PLAY_PAUSE: return "play_pause";
    case AP2_REMOTE_COMMAND_NEXT: return "next";
    case AP2_REMOTE_COMMAND_PREVIOUS: return "previous";
    default: return NULL;
    }
}

#endif /* __AP2_REMOTE_H_ */
