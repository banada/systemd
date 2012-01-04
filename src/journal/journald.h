/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#ifndef foojournaldhfoo
#define foojournaldhfoo

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <inttypes.h>
#include <sys/types.h>
#include <stdbool.h>

#include "journal-file.h"
#include "hashmap.h"
#include "util.h"
#include "journal-rate-limit.h"
#include "list.h"

typedef struct StdoutStream StdoutStream;

typedef struct Server {
        int epoll_fd;
        int signal_fd;
        int syslog_fd;
        int native_fd;
        int stdout_fd;

        JournalFile *runtime_journal;
        JournalFile *system_journal;
        Hashmap *user_journals;

        uint64_t seqnum;

        char *buffer;
        size_t buffer_size;

        JournalRateLimit *rate_limit;
        usec_t rate_limit_interval;
        unsigned rate_limit_burst;

        JournalMetrics runtime_metrics;
        JournalMetrics system_metrics;

        bool compress;

        uint64_t cached_available_space;
        usec_t cached_available_space_timestamp;

        uint64_t var_available_timestamp;

        LIST_HEAD(StdoutStream, stdout_streams);
        unsigned n_stdout_streams;
} Server;

/* gperf lookup function */
const struct ConfigPerfItem* journald_gperf_lookup(const char *key, unsigned length);

#endif