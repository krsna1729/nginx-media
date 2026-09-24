#define _POSIX_C_SOURCE 200809L

#include <srt/srt.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>

#define STREAMID_CAP 512
#define DESTINATION_CAP 256
#define RECEIVE_CAP 65536
#define TS_PACKET_SIZE 188
#define TS_PID_COUNT 8192
#define TS_SEEN_BYTES (TS_PID_COUNT / 8)

typedef struct {
    SRTSOCKET sock;
    char destination[DESTINATION_CAP];
    uint64_t bytes_received;
    struct timespec first_byte_at;
    uint64_t ts_packets;
    uint64_t ts_sync_errors;
    uint64_t ts_continuity_errors;
    uint64_t ts_tei_errors;
    uint8_t *ts_last_cc;
    uint8_t *ts_seen;
    unsigned char ts_partial[TS_PACKET_SIZE];
    size_t ts_partial_len;
    int ts_sync_locked;
    int has_first_byte;
    int stalled;
    int transport_error;
} peer_t;

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t snapshot_requested;
static volatile sig_atomic_t measurement_toggle_requested;

static void
handle_signal(int signo)
{
    if (signo == SIGUSR1) {
        snapshot_requested = 1;
    } else if (signo == SIGUSR2) {
        measurement_toggle_requested = 1;
    } else {
        stop_requested = 1;
    }
}

static void
usage(const char *program)
{
    fprintf(stderr,
            "usage: %s PORT EXPECTED_PEERS READY_FILE RESULT_CSV "
            "[STALL_DEST_ID] [quality]\n",
            program);
}

static int
parse_unsigned(const char *text, unsigned long max, unsigned long *value)
{
    char *end;
    const char *p;
    unsigned long parsed;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0 || parsed > max) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int
extract_destination(const char *streamid, char *destination, size_t cap)
{
    const char *field = streamid;
    const char *end;
    size_t len, i;

    while (*field != '\0') {
        end = strchr(field, ',');
        if (end == NULL) {
            end = field + strlen(field);
        }

        if ((size_t) (end - field) >= 2 && field[0] == 's' && field[1] == '=') {
            len = (size_t) (end - field - 2);
            if (len == 0 || len >= cap) {
                return -1;
            }
            for (i = 0; i < len; i++) {
                unsigned char c = (unsigned char) field[2 + i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || (c >= '0' && c <= '9') || c == '_' || c == '-'
                      || c == '.'))
                {
                    return -1;
                }
            }
            memcpy(destination, field + 2, len);
            destination[len] = '\0';
            return 0;
        }
        field = (*end == ',') ? end + 1 : end;
    }

    return -1;
}

static int
publish_ready(const char *path)
{
    char *temporary;
    size_t length = strlen(path) + sizeof(".tmp.XXXXXX");
    int fd, saved_errno;

    temporary = malloc(length);
    if (temporary == NULL) {
        fprintf(stderr, "cannot allocate ready-file path\n");
        return -1;
    }
    (void) snprintf(temporary, length, "%s.tmp.XXXXXX", path);

    fd = mkstemp(temporary);
    if (fd == -1) {
        fprintf(stderr, "cannot create ready file %s: %s\n", path,
                strerror(errno));
        free(temporary);
        return -1;
    }
    if (close(fd) == -1 || rename(temporary, path) == -1) {
        saved_errno = errno;
        (void) unlink(temporary);
        fprintf(stderr, "cannot publish ready file %s: %s\n", path,
                strerror(saved_errno));
        free(temporary);
        return -1;
    }

    free(temporary);
    return 0;
}

static int
write_results(const char *path, const peer_t *peers, size_t count)
{
    FILE *file;
    const struct timespec *first = NULL;
    size_t i;
    double first_ms;

    for (i = 0; i < count; i++) {
        if (!peers[i].has_first_byte) {
            continue;
        }
        if (first == NULL
            || peers[i].first_byte_at.tv_sec < first->tv_sec
            || (peers[i].first_byte_at.tv_sec == first->tv_sec
                && peers[i].first_byte_at.tv_nsec < first->tv_nsec))
        {
            first = &peers[i].first_byte_at;
        }
    }

    file = fopen(path, "w");
    if (file == NULL) {
        fprintf(stderr, "cannot create result CSV %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    if (fprintf(file,
                "destination_id,bytes_received,first_ms,stalled,transport_error,"
                "ts_packets,ts_sync_errors,ts_continuity_errors,ts_tei_errors\n") < 0)
    {
        fprintf(stderr, "cannot write result CSV %s\n", path);
        (void) fclose(file);
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (fprintf(file, "%s,%llu,", peers[i].destination,
                    (unsigned long long) peers[i].bytes_received) < 0)
        {
            fprintf(stderr, "cannot write result CSV %s\n", path);
            (void) fclose(file);
            return -1;
        }
        if (peers[i].has_first_byte && first != NULL) {
            first_ms = (peers[i].first_byte_at.tv_sec - first->tv_sec) * 1000.0
                       + (peers[i].first_byte_at.tv_nsec - first->tv_nsec)
                             / 1000000.0;
            if (fprintf(file, "%.3f", first_ms) < 0) {
                fprintf(stderr, "cannot write result CSV %s\n", path);
                (void) fclose(file);
                return -1;
            }
        }
        if (fprintf(file, ",%d,%d,%llu,%llu,%llu,%llu\n",
                    peers[i].stalled, peers[i].transport_error,
                    (unsigned long long) peers[i].ts_packets,
                    (unsigned long long) peers[i].ts_sync_errors,
                    (unsigned long long) peers[i].ts_continuity_errors,
                    (unsigned long long) peers[i].ts_tei_errors) < 0)
        {
            fprintf(stderr, "cannot write result CSV %s\n", path);
            (void) fclose(file);
            return -1;
        }
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "cannot finish result CSV %s: %s\n", path,
                strerror(errno));
        return -1;
    }

    return 0;
}

static int
write_snapshot(const char *path, const peer_t *peers, size_t count,
    const struct timespec *snapshot_time)
{
    char *temporary;
    size_t length = strlen(path) + sizeof(".tmp.XXXXXX");
    size_t i;
    int fd, saved_errno;
    FILE *file;
    uint64_t snapshot_ns;
    snapshot_ns = (uint64_t) snapshot_time->tv_sec * 1000000000ULL
                  + (uint64_t) snapshot_time->tv_nsec;

    temporary = malloc(length);
    if (temporary == NULL) {
        fprintf(stderr, "cannot allocate receiver snapshot path\n");
        return -1;
    }
    (void) snprintf(temporary, length, "%s.tmp.XXXXXX", path);
    fd = mkstemp(temporary);
    if (fd == -1) {
        fprintf(stderr, "cannot create receiver snapshot %s: %s\n", path,
                strerror(errno));
        free(temporary);
        return -1;
    }
    file = fdopen(fd, "w");
    if (file == NULL) {
        saved_errno = errno;
        (void) close(fd);
        (void) unlink(temporary);
        fprintf(stderr, "cannot open receiver snapshot %s: %s\n", path,
                strerror(saved_errno));
        free(temporary);
        return -1;
    }
    if (fprintf(file, "destination_id,bytes_received,snapshot_ns\n") < 0) {
        goto failed;
    }
    for (i = 0; i < count; i++) {
        if (fprintf(file, "%s,%llu,%llu\n", peers[i].destination,
                    (unsigned long long) peers[i].bytes_received,
                    (unsigned long long) snapshot_ns) < 0)
        {
            goto failed;
        }
    }
    if (fclose(file) != 0) {
        file = NULL;
        goto failed;
    }
    file = NULL;
    if (rename(temporary, path) == -1) {
        goto failed;
    }

    free(temporary);
    return 0;

failed:
    saved_errno = errno;
    if (file != NULL) {
        (void) fclose(file);
    }
    (void) unlink(temporary);
    fprintf(stderr, "cannot write receiver snapshot %s: %s\n", path,
            strerror(saved_errno));
    free(temporary);
    return -1;
}

static void
mark_transport_error(int epoll_id, peer_t *peer, const char *reason)
{
    fprintf(stderr, "SRT transport error for %s: %s\n", peer->destination,
            reason);
    peer->transport_error = 1;
    if (peer->sock != SRT_INVALID_SOCK) {
        (void) srt_epoll_remove_usock(epoll_id, peer->sock);
        (void) srt_close(peer->sock);
        peer->sock = SRT_INVALID_SOCK;
    }
}

static void
ts_inspect_packet(peer_t *peer, const unsigned char *packet)
{
    unsigned int pid, cc, afc, adaptation_length, seen_index;
    unsigned char seen_mask;
    int payload, discontinuity = 0;

    if (packet[0] != 0x47) {
        peer->ts_sync_errors++;
        return;
    }

    pid = ((unsigned int) (packet[1] & 0x1f) << 8) | packet[2];
    afc = (packet[3] >> 4) & 0x03;
    cc = packet[3] & 0x0f;
    if (afc == 0) {
        peer->ts_sync_errors++;
        return;
    }

    if (packet[1] & 0x80) {
        peer->ts_tei_errors++;
    }
    if (afc & 0x02) {
        adaptation_length = packet[4];
        if (adaptation_length > 183) {
            peer->ts_sync_errors++;
            return;
        }
        if (adaptation_length > 0 && (packet[5] & 0x80)) {
            discontinuity = 1;
        }
    }

    peer->ts_packets++;
    payload = (afc & 0x01) != 0;
    seen_index = pid >> 3;
    seen_mask = (unsigned char) (1U << (pid & 7));

    if (discontinuity) {
        peer->ts_seen[seen_index] &= (uint8_t) ~seen_mask;
    }
    if (payload) {
        if ((peer->ts_seen[seen_index] & seen_mask)
            && cc != ((peer->ts_last_cc[pid] + 1) & 0x0f))
        {
            peer->ts_continuity_errors++;
        }
        peer->ts_last_cc[pid] = (uint8_t) cc;
        peer->ts_seen[seen_index] |= seen_mask;
    } else if ((peer->ts_seen[seen_index] & seen_mask)
               && cc != peer->ts_last_cc[pid])
    {
        peer->ts_continuity_errors++;
    }
}

static size_t
ts_find_sync(const unsigned char *data, size_t length, size_t offset)
{
    size_t i;

    for (i = offset; i < length; i++) {
        if (data[i] == 0x47 && length - i >= TS_PACKET_SIZE
            && (length - i < TS_PACKET_SIZE * 2
                || data[i + TS_PACKET_SIZE] == 0x47))
        {
            return i;
        }
    }

    return length;
}

static void
ts_inspect(peer_t *peer, const unsigned char *data, size_t length)
{
    size_t offset = 0, copied, remaining, sync;

    if (!peer->ts_sync_locked) {
        sync = ts_find_sync(data, length, 0);
        if (sync == length) {
            return;
        }
        offset = sync;
        peer->ts_sync_locked = 1;
    }

    if (peer->ts_partial_len != 0) {
        copied = TS_PACKET_SIZE - peer->ts_partial_len;
        if (copied > length) {
            copied = length;
        }
        memcpy(peer->ts_partial + peer->ts_partial_len, data, copied);
        peer->ts_partial_len += copied;
        offset += copied;
        if (peer->ts_partial_len == TS_PACKET_SIZE) {
            if (peer->ts_partial[0] == 0x47) {
                ts_inspect_packet(peer, peer->ts_partial);
            } else {
                peer->ts_sync_errors++;
                peer->ts_sync_locked = 0;
            }
            peer->ts_partial_len = 0;
        } else {
            return;
        }
    }

    while (length - offset >= TS_PACKET_SIZE) {
        if (data[offset] != 0x47) {
            peer->ts_sync_errors++;
            peer->ts_sync_locked = 0;
            sync = ts_find_sync(data, length, offset + 1);
            if (sync == length) {
                return;
            }
            offset = sync;
            peer->ts_sync_locked = 1;
            continue;
        }
        ts_inspect_packet(peer, data + offset);
        offset += TS_PACKET_SIZE;
    }

    remaining = length - offset;
    if (remaining != 0) {
        memcpy(peer->ts_partial, data + offset, remaining);
        peer->ts_partial_len = remaining;
    }
}

int
main(int argc, char **argv)
{
    unsigned long port_arg, expected_arg;
    size_t expected, connected = 0;
    peer_t *peers = NULL;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    int epoll_id = SRT_ERROR;
    struct sockaddr_in address;
    struct sigaction action;
    struct timespec snapshot_time;
    SRT_EPOLL_EVENT *events = NULL;
    char *buffer = NULL;
    const char *stall_id = NULL;
    char *snapshot_path = NULL;
    int failure = 0, ready = 0, initialized = 0, i;
    int quality_mode = 0, measurement_active = 0;

    if ((argc < 5 || argc > 7)
        || parse_unsigned(argc > 1 ? argv[1] : NULL, 65535, &port_arg) != 0
        || parse_unsigned(argc > 2 ? argv[2] : NULL, INT32_MAX - 2,
                          &expected_arg) != 0)
    {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 6) {
        if (strcmp(argv[5], "quality") == 0) {
            quality_mode = 1;
        } else {
            if (argv[5][0] == '\0' || strlen(argv[5]) >= DESTINATION_CAP) {
                fprintf(stderr, "STALL_DEST_ID is empty or too long\n");
                return EXIT_FAILURE;
            }
            stall_id = argv[5];
        }
    }
    if (argc == 7) {
        if (strcmp(argv[6], "quality") != 0 || quality_mode) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        quality_mode = 1;
    }

    expected = (size_t) expected_arg;
    if (expected > SIZE_MAX / sizeof(*peers)
        || expected + 1 > SIZE_MAX / sizeof(*events))
    {
        fprintf(stderr, "EXPECTED_PEERS is too large\n");
        return EXIT_FAILURE;
    }

    snapshot_path = malloc(strlen(argv[4]) + sizeof(".snapshot"));
    if (snapshot_path == NULL) {
        fprintf(stderr, "cannot allocate receiver snapshot path\n");
        failure = 1;
        goto done;
    }
    (void) sprintf(snapshot_path, "%s.snapshot", argv[4]);
    peers = calloc(expected, sizeof(*peers));
    events = calloc(expected + 1, sizeof(*events));
    buffer = malloc(RECEIVE_CAP);
    if (peers == NULL || events == NULL || buffer == NULL) {
        fprintf(stderr, "cannot allocate receiver state for %zu peers\n", expected);
        failure = 1;
        goto done;
    }
    for (i = 0; i < (int) expected; i++) {
        peers[i].sock = SRT_INVALID_SOCK;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1
        || sigaction(SIGTERM, &action, NULL) == -1
        || sigaction(SIGUSR1, &action, NULL) == -1
        || (quality_mode && sigaction(SIGUSR2, &action, NULL) == -1))
    {
        fprintf(stderr, "cannot install signal handlers: %s\n", strerror(errno));
        failure = 1;
        goto done;
    }

    if (srt_startup() == SRT_ERROR) {
        fprintf(stderr, "SRT startup failed: %s\n", srt_getlasterror_str());
        failure = 1;
        goto done;
    }
    initialized = 1;
    /*
     * Per-packet receive loss, empty accepts, and redundant epoll-removal
     * warnings flood the measured path. Receiver byte, TS, and transport
     * counters preserve the quality evidence without those log categories.
     */
    srt_dellogfa(SRT_LOGFA_BUF_RECV);
    srt_dellogfa(SRT_LOGFA_QUE_RECV);
    srt_dellogfa(SRT_LOGFA_TSBPD);
    srt_dellogfa(SRT_LOGFA_CONN);
    srt_dellogfa(SRT_LOGFA_EPOLL_UPD);

    listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK) {
        fprintf(stderr, "cannot create SRT listener: %s\n", srt_getlasterror_str());
        failure = 1;
        goto done;
    }
    {
        int nonblocking = 0;
        if (srt_setsockopt(listener, 0, SRTO_RCVSYN, &nonblocking,
                           sizeof(nonblocking)) == SRT_ERROR)
        {
            fprintf(stderr, "cannot configure SRT listener: %s\n",
                    srt_getlasterror_str());
            failure = 1;
            goto done;
        }
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t) port_arg);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(listener, (struct sockaddr *) &address, sizeof(address)) == SRT_ERROR
        || srt_listen(listener, (int) expected) == SRT_ERROR)
    {
        fprintf(stderr, "cannot bind/listen on 127.0.0.1:%lu: %s\n", port_arg,
                srt_getlasterror_str());
        failure = 1;
        goto done;
    }

    epoll_id = srt_epoll_create();
    if (epoll_id == SRT_ERROR) {
        fprintf(stderr, "cannot create SRT epoll: %s\n", srt_getlasterror_str());
        failure = 1;
        goto done;
    }
    {
        int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        if (srt_epoll_add_usock(epoll_id, listener, &mask) == SRT_ERROR) {
            fprintf(stderr, "cannot monitor SRT listener: %s\n",
                    srt_getlasterror_str());
            failure = 1;
            goto done;
        }
    }

    while (!stop_requested) {
        int count = srt_epoll_uwait(epoll_id, events, (int) (expected + 1), 1000);
        if (count == SRT_ERROR) {
            int system_error;
            int error = srt_getlasterror(&system_error);
            if (stop_requested) {
                break;
            }
            if (error != SRT_EASYNCRCV) {
                fprintf(stderr, "SRT epoll wait failed: %s\n",
                        srt_getlasterror_str());
                failure = 1;
                break;
            }
            count = 0;
        }

        for (i = 0; i < count && !failure && !stop_requested; i++) {
            SRTSOCKET sock = events[i].fd;
            int event_mask = events[i].events;

            if (event_mask & SRT_EPOLL_ERR) {
                size_t peer_index;

                if (sock != listener) {
                    for (peer_index = 0; peer_index < connected; peer_index++) {
                        if (peers[peer_index].sock == sock) {
                            break;
                        }
                    }
                    if (peer_index == connected) {
                        /* A replaced session can still have a queued event. */
                        continue;
                    }
                    if (quality_mode) {
                        mark_transport_error(epoll_id, &peers[peer_index],
                                             "SRT_EPOLL_ERR");
                        continue;
                    }
                }
                fprintf(stderr, "SRT transport error on socket %lld\n",
                        (long long) sock);
                failure = 1;
                break;
            }
            if (!(event_mask & SRT_EPOLL_IN)) {
                continue;
            }

            if (sock == listener) {
                for (;;) {
                    SRTSOCKET accepted = srt_accept(listener, NULL, NULL);
                    char streamid[STREAMID_CAP];
                    char destination[DESTINATION_CAP];
                    int streamid_len = sizeof(streamid) - 1;
                    int nonblocking = 0;
                    int is_new;
                    size_t peer_index;

                    if (accepted == SRT_INVALID_SOCK) {
                        int system_error;
                        int error = srt_getlasterror(&system_error);
                        if (error == SRT_EASYNCRCV) {
                            break;
                        }
                        fprintf(stderr, "SRT accept failed: %s\n",
                                srt_getlasterror_str());
                        failure = 1;
                        break;
                    }
                    memset(streamid, 0, sizeof(streamid));
                    if (srt_getsockopt(accepted, 0, SRTO_STREAMID, streamid,
                                       &streamid_len) == SRT_ERROR
                        || streamid_len < 0
                        || streamid_len >= (int) sizeof(streamid))
                    {
                        fprintf(stderr, "cannot read peer stream ID: %s\n",
                                srt_getlasterror_str());
                        (void) srt_close(accepted);
                        failure = 1;
                        break;
                    }
                    streamid[streamid_len] = '\0';
                    if (extract_destination(streamid, destination,
                                            sizeof(destination)) != 0)
                    {
                        fprintf(stderr,
                                "peer stream ID has no valid s= destination: %s\n",
                                streamid);
                        (void) srt_close(accepted);
                        failure = 1;
                        break;
                    }
                    for (peer_index = 0; peer_index < connected; peer_index++) {
                        if (strcmp(peers[peer_index].destination, destination) == 0) {
                            break;
                        }
                    }
                    is_new = peer_index == connected;
                    if (is_new && connected == expected) {
                        fprintf(stderr,
                                "received unexpected peer %s after %zu peers\n",
                                destination, expected);
                        (void) srt_close(accepted);
                        failure = 1;
                        break;
                    }
                    if (is_new) {
                        (void) memcpy(peers[peer_index].destination, destination,
                                      strlen(destination) + 1);
                        peers[peer_index].stalled =
                            stall_id != NULL
                            && strcmp(destination, stall_id) == 0;
                    } else {
                        if (peers[peer_index].sock != SRT_INVALID_SOCK) {
                            if (!peers[peer_index].stalled) {
                                (void) srt_epoll_remove_usock(
                                    epoll_id, peers[peer_index].sock);
                            }
                            (void) srt_close(peers[peer_index].sock);
                            peers[peer_index].sock = SRT_INVALID_SOCK;
                        }
                        peers[peer_index].transport_error = 1;
                    }
                    if (srt_setsockopt(accepted, 0, SRTO_RCVSYN, &nonblocking,
                                       sizeof(nonblocking)) == SRT_ERROR)
                    {
                        fprintf(stderr, "cannot configure accepted socket: %s\n",
                                srt_getlasterror_str());
                        (void) srt_close(accepted);
                        failure = 1;
                        break;
                    }
                    if (quality_mode
                        && (peers[peer_index].ts_last_cc == NULL
                            || peers[peer_index].ts_seen == NULL))
                    {
                        peers[peer_index].ts_last_cc = malloc(TS_PID_COUNT);
                        peers[peer_index].ts_seen = calloc(TS_SEEN_BYTES, 1);
                        if (peers[peer_index].ts_last_cc == NULL
                            || peers[peer_index].ts_seen == NULL)
                        {
                            fprintf(stderr, "cannot allocate TS counters for %s\n",
                                    peers[peer_index].destination);
                            free(peers[peer_index].ts_last_cc);
                            free(peers[peer_index].ts_seen);
                            peers[peer_index].ts_last_cc = NULL;
                            peers[peer_index].ts_seen = NULL;
                            (void) srt_close(accepted);
                            failure = 1;
                            break;
                        }
                    }
                    if (!peers[peer_index].stalled) {
                        int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                        if (srt_epoll_add_usock(epoll_id, accepted, &mask)
                            == SRT_ERROR)
                        {
                            fprintf(stderr, "cannot monitor accepted socket: %s\n",
                                    srt_getlasterror_str());
                            (void) srt_close(accepted);
                            failure = 1;
                            break;
                        }
                    }
                    peers[peer_index].sock = accepted;
                    if (is_new) {
                        connected++;
                    }
                    if (connected == expected && !ready) {
                        if (publish_ready(argv[3]) != 0) {
                            failure = 1;
                            break;
                        }
                        ready = 1;
                    }
                }
            } else {
                size_t peer_index;
                for (peer_index = 0; peer_index < connected; peer_index++) {
                    if (peers[peer_index].sock == sock) {
                        break;
                    }
                }
                if (peer_index == connected) {
                    /* A replaced session can still have a queued event. */
                    continue;
                }
                for (;;) {
                    int received = srt_recvmsg(sock, buffer, RECEIVE_CAP);
                    if (received == SRT_ERROR) {
                        int system_error;
                        int error = srt_getlasterror(&system_error);
                        if (error == SRT_EASYNCRCV) {
                            break;
                        }
                        if (quality_mode) {
                            mark_transport_error(
                                epoll_id, &peers[peer_index],
                                srt_getlasterror_str());
                        } else {
                            fprintf(stderr, "SRT receive failed for %s: %s\n",
                                    peers[peer_index].destination,
                                    srt_getlasterror_str());
                            failure = 1;
                        }
                        break;
                    }
                    if (received == 0) {
                        if (quality_mode) {
                            mark_transport_error(epoll_id, &peers[peer_index],
                                                 "peer closed receive stream");
                        } else {
                            fprintf(stderr,
                                    "SRT peer %s closed its receive stream\n",
                                    peers[peer_index].destination);
                            failure = 1;
                        }
                        break;
                    }
                    if (UINT64_MAX - peers[peer_index].bytes_received
                        < (uint64_t) received)
                    {
                        fprintf(stderr, "byte counter overflow for %s\n",
                                peers[peer_index].destination);
                        failure = 1;
                        break;
                    }
                    peers[peer_index].bytes_received += (uint64_t) received;
                    if (measurement_active) {
                        ts_inspect(&peers[peer_index],
                                   (const unsigned char *) buffer,
                                   (size_t) received);
                    }
                    if (!peers[peer_index].has_first_byte) {
                        if (clock_gettime(CLOCK_MONOTONIC,
                                          &peers[peer_index].first_byte_at)
                            == -1)
                        {
                            fprintf(stderr,
                                    "cannot timestamp first byte for %s: %s\n",
                                    peers[peer_index].destination,
                                    strerror(errno));
                            failure = 1;
                            break;
                        }
                        peers[peer_index].has_first_byte = 1;
                    }
                }
            }
        }
        if (measurement_toggle_requested) {
            size_t peer_index;
            int start_measurement = !measurement_active;

            measurement_toggle_requested = 0;
            if (start_measurement) {
                for (peer_index = 0; peer_index < connected; peer_index++) {
                    peers[peer_index].ts_packets = 0;
                    peers[peer_index].ts_sync_errors = 0;
                    peers[peer_index].ts_continuity_errors = 0;
                    peers[peer_index].ts_tei_errors = 0;
                    peers[peer_index].ts_sync_locked = 0;
                    peers[peer_index].ts_partial_len = 0;
                    memset(peers[peer_index].ts_seen, 0, TS_SEEN_BYTES);
                }
                measurement_active = 1;
            } else {
                measurement_active = 0;
            }
            if (clock_gettime(CLOCK_MONOTONIC, &snapshot_time) == -1
                || write_snapshot(snapshot_path, peers, connected,
                                  &snapshot_time) != 0)
            {
                failure = 1;
                break;
            }
        }
        if (snapshot_requested) {
            snapshot_requested = 0;
            if (clock_gettime(CLOCK_MONOTONIC, &snapshot_time) == -1
                || write_snapshot(snapshot_path, peers, connected,
                                  &snapshot_time) != 0)
            {
                failure = 1;
                break;
            }
        }

    }
    if (!failure && connected != expected) {
        fprintf(stderr, "stopped with %zu of %zu expected peers connected\n",
                connected, expected);
        failure = 1;
    }
    if (write_results(argv[4], peers, connected) != 0) {
        failure = 1;
    }

done:
    if (epoll_id != SRT_ERROR) {
        (void) srt_epoll_release(epoll_id);
    }
    if (peers != NULL) {
        for (i = 0; i < (int) connected; i++) {
            if (peers[i].sock != SRT_INVALID_SOCK) {
                (void) srt_close(peers[i].sock);
            }
        }
    }
    if (listener != SRT_INVALID_SOCK) {
        (void) srt_close(listener);
    }
    if (buffer != NULL) {
        free(buffer);
    }
    if (peers != NULL) {
        for (i = 0; i < (int) expected; i++) {
            free(peers[i].ts_last_cc);
            free(peers[i].ts_seen);
        }
    }
    free(events);
    free(snapshot_path);
    free(peers);
    if (initialized) {
        (void) srt_cleanup();
    }

    return failure ? EXIT_FAILURE : EXIT_SUCCESS;
}
