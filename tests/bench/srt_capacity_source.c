#define _POSIX_C_SOURCE 200809L
#include <srt/srt.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_PEERS 2048
#define LANES 16
#define PAYLOAD 1316
#define TS_PACKET 188
#define TS_PIDS 8192

/* Direct receiver control: no nginx producer or egress scheduler is involved. */
typedef struct {
    SRTSOCKET sock;
    uint64_t attempted, accepted, blocked, errors;
} peer_t;

static volatile sig_atomic_t stopping, snapshot_requested;

static void
signal_handler(int signo)
{
    if (signo == SIGUSR1) snapshot_requested = 1;
    else stopping = 1;
}

static uint64_t
now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static int
number(const char *text, uint64_t maximum, uint64_t *value)
{
    char *end;
    unsigned long long parsed;
    if (text[0] < '0' || text[0] > '9') return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || *end || parsed == 0 || parsed > maximum) return -1;
    *value = (uint64_t) parsed;
    return 0;
}

static int
write_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t n = write(fd, data, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n;
        length -= (size_t) n;
    }
    return 0;
}

static int
snapshot(const char *path, const char *temporary, const peer_t *peers,
    size_t count)
{
    char buffer[65536];
    size_t used = 0;
    uint64_t stamp = now_ns();
    int fd, rc = -1;
    if (!stamp) return -1;
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    used = (size_t) snprintf(buffer, sizeof(buffer),
        "destination_id,attempted_bytes,accepted_bytes,blocked_messages,"
        "transport_errors,snapshot_ns\n");
    for (size_t i = 0; i < count; i++) {
        char row[256];
        int n = snprintf(row, sizeof(row), "d%04zu,%llu,%llu,%llu,%llu,%llu\n",
            i, (unsigned long long) peers[i].attempted,
            (unsigned long long) peers[i].accepted,
            (unsigned long long) peers[i].blocked,
            (unsigned long long) peers[i].errors, (unsigned long long) stamp);
        if (n < 0 || (size_t) n >= sizeof(row)) goto done;
        if (used + (size_t) n > sizeof(buffer)) {
            if (write_all(fd, buffer, used) != 0) goto done;
            used = 0;
        }
        memcpy(buffer + used, row, (size_t) n);
        used += (size_t) n;
    }
    if (write_all(fd, buffer, used) == 0) rc = 0;
done:
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && rename(temporary, path) != 0) rc = -1;
    if (rc != 0) (void) unlink(temporary);
    return rc;
}

static int
configure(SRTSOCKET sock, const char *id)
{
    int yes = 1, no = 0, timeout = 3000, latency = 120;
    int buffer_bytes = 1024 * 1024;
    struct linger linger = {0, 0};
    SRT_TRANSTYPE live = SRTT_LIVE;
    return srt_setsockopt(sock, 0, SRTO_TRANSTYPE, &live, sizeof(live))
        || srt_setsockopt(sock, 0, SRTO_SENDER, &yes, sizeof(yes))
        || srt_setsockopt(sock, 0, SRTO_REUSEADDR, &yes, sizeof(yes))
        || srt_setsockopt(sock, 0, SRTO_SNDSYN, &no, sizeof(no))
        || srt_setsockopt(sock, 0, SRTO_CONNTIMEO, &timeout, sizeof(timeout))
        || srt_setsockopt(sock, 0, SRTO_LATENCY, &latency, sizeof(latency))
        || srt_setsockopt(sock, 0, SRTO_SNDBUF, &buffer_bytes, sizeof(buffer_bytes))
        || srt_setsockopt(sock, 0, SRTO_LINGER, &linger, sizeof(linger))
        || srt_setsockopt(sock, 0, SRTO_STREAMID, id, (int) strlen(id));
}

/* Keep CC continuous when a long control run loops its prepared TS file. */
static int
continuity(unsigned char *data, size_t length, unsigned char *cc,
    unsigned char *seen)
{
    for (size_t pos = 0; pos < length; pos += TS_PACKET) {
        unsigned char *packet = data + pos;
        unsigned pid;
        if (packet[0] != 0x47 || (packet[1] & 0x80)) return -1;
        pid = ((unsigned) (packet[1] & 0x1f) << 8) | packet[2];
        if (pid == 0x1fff) continue;
        if (!seen[pid]) {
            seen[pid] = 1;
            cc[pid] = packet[3] & 0x0f;
        } else if (packet[3] & 0x10) {
            cc[pid] = (unsigned char) ((cc[pid] + 1) & 0x0f);
        }
        packet[3] = (unsigned char) ((packet[3] & 0xf0) | cc[pid]);
    }
    return 0;
}

int
main(int argc, char **argv)
{
    peer_t peers[MAX_PEERS];
    unsigned short ports[LANES] = {0};
    unsigned char cc[TS_PIDS] = {0}, seen[TS_PIDS] = {0};
    char temporary[PATH_MAX];
    struct sockaddr_in remote, local;
    struct stat info;
    struct sigaction action;
    unsigned char *data = MAP_FAILED;
    uint64_t port, count, listeners, rate, next;
    size_t size = 0, offset = 0;
    int fd = -1, ready_fd = -1, initialized = 0, rc = 1;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%u\n", (unsigned) srt_getversion());
        return 0;
    }
    if (argc != 9 || number(argv[3], 65535, &port)
        || number(argv[4], MAX_PEERS, &count)
        || number(argv[5], 64, &listeners)
        || number(argv[6], 10000000000ULL, &rate)
        || port + listeners - 1 > 65535
        || strlen(argv[8]) + sizeof(".tmp") > sizeof(temporary))
    {
        fprintf(stderr, "usage: %s TS_FILE IPV4 PORT PEERS LISTENERS "
            "PER_PEER_BPS READY_FILE SNAPSHOT_CSV\n", argv[0]);
        return 2;
    }
    memset(peers, 0, (size_t) count * sizeof(*peers));
    for (size_t i = 0; i < (size_t) count; i++) peers[i].sock = SRT_INVALID_SOCK;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    if (inet_pton(AF_INET, argv[2], &remote.sin_addr) != 1) {
        fprintf(stderr, "invalid receiver IPv4 address\n");
        return 2;
    }
    snprintf(temporary, sizeof(temporary), "%s.tmp", argv[8]);
    fd = open(argv[1], O_RDONLY);
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_size <= 0 || (uintmax_t) info.st_size > SIZE_MAX
        || info.st_size % TS_PACKET != 0)
    {
        fprintf(stderr, "source must be a nonempty regular MPEG-TS file\n");
        goto done;
    }
    size = (size_t) info.st_size;
    data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) goto done;
    if (srt_startup() != 0) goto done;
    initialized = 1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0
        || sigaction(SIGINT, &action, NULL) != 0
        || sigaction(SIGUSR1, &action, NULL) != 0) goto done;
    for (size_t i = 0; i < (size_t) count && !stopping; i++) {
        char id[64];
        int local_length = (int) sizeof(local);
        size_t lane = i % LANES;
        peers[i].sock = srt_create_socket();
        snprintf(id, sizeof(id), "#!::r=receiver-control,s=d%04zu", i);
        if (peers[i].sock == SRT_INVALID_SOCK || configure(peers[i].sock, id)) goto done;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_port = htons(ports[lane]);
        if (srt_bind(peers[i].sock, (struct sockaddr *) &local, sizeof(local)) != 0
            || srt_getsockname(peers[i].sock, (struct sockaddr *) &local, &local_length) != 0)
            goto done;
        ports[lane] = ntohs(local.sin_port);
        remote.sin_port = htons((unsigned short) (port + i % listeners));
        if (srt_connect(peers[i].sock, (struct sockaddr *) &remote, sizeof(remote)) != 0)
            goto done;
    }
    if (stopping) goto done;
    ready_fd = open(argv[7], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (ready_fd < 0 || write_all(ready_fd, "ready\n", 6) != 0) goto done;
    int ready_closed = close(ready_fd);
    ready_fd = -1;
    if (ready_closed != 0) goto done;
    next = now_ns();
    if (!next) goto done;
    while (!stopping) {
        size_t length = size - offset;
        if (length > PAYLOAD) length = PAYLOAD;
        if (snapshot_requested) {
            snapshot_requested = 0;
            if (snapshot(argv[8], temporary, peers, (size_t) count) != 0) goto done;
        }
        if (continuity(data + offset, length, cc, seen) != 0) {
            fprintf(stderr, "invalid TS packet in control source\n");
            goto done;
        }
        for (size_t i = 0; i < (size_t) count; i++) {
            int n;
            peers[i].attempted += length;
            n = srt_sendmsg(peers[i].sock, (const char *) data + offset, (int) length, -1, 1);
            if (n == (int) length) peers[i].accepted += length;
            else if (n == SRT_ERROR && srt_getlasterror(NULL) == SRT_EASYNCSND)
                peers[i].blocked++;
            else peers[i].errors++;
        }
        offset += length;
        if (offset == size) offset = 0;
        next += (uint64_t) length * 8 * 1000000000ULL / rate;
        struct timespec deadline = {(time_t) (next / 1000000000ULL),
                                   (long) (next % 1000000000ULL)};
        int error;
        do {
            error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
            if (error == EINTR && snapshot_requested) {
                snapshot_requested = 0;
                if (snapshot(argv[8], temporary, peers, (size_t) count) != 0) goto done;
            }
        } while (error == EINTR && !stopping);
        if (error != 0 && error != EINTR) goto done;
    }
    if (snapshot(argv[8], temporary, peers, (size_t) count) != 0) goto done;
    rc = 0;
done:
    if (rc != 0) fprintf(stderr, "SRT control source failed: %s\n", srt_getlasterror_str());
    if (ready_fd >= 0) (void) close(ready_fd);
    for (size_t i = 0; i < (size_t) count; i++)
        if (peers[i].sock != SRT_INVALID_SOCK) (void) srt_close(peers[i].sock);
    if (initialized) (void) srt_cleanup();
    if (data != MAP_FAILED) (void) munmap(data, size);
    if (fd >= 0) (void) close(fd);
    return rc;
}
