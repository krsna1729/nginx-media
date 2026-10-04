#define _POSIX_C_SOURCE 200809L

/* Real libsrt callers: connect without sending, then report peer closure. */
#include <srt/srt.h>

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PEERS 16
#define CLOSE_DEADLINE_MS 3000

static long long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int
connected(const SRTSOCKET *peers, int count)
{
    int i, alive = 0;

    for (i = 0; i < count; i++) {
        alive += srt_getsockstate(peers[i]) == SRTS_CONNECTED;
    }
    return alive;
}

static int
send_fixture(SRTSOCKET peer, const char *path)
{
    FILE *file;
    char buf[1316];
    size_t n;
    int rc = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), file)) != 0) {
        if (srt_sendmsg(peer, buf, (int) n, -1, 1) != (int) n) {
            rc = -1;
            break;
        }
    }
    if (ferror(file)) {
        rc = -1;
    }
    fclose(file);
    return rc;
}

int
main(int argc, char **argv)
{
    SRTSOCKET peers[MAX_PEERS];
    struct sockaddr_in address;
    struct timespec pause = {0, 10000000};
    char command[4096];
    int i, count = 0, rc = 1, sender = 1, timeout = 1000;
    int peer_idle = 30000;
    SRT_TRANSTYPE live = SRTT_LIVE;
    long long deadline;

    if (argc < 3 || argc > MAX_PEERS + 2) {
        fprintf(stderr, "usage: %s PORT STREAMID [STREAMID ...]\n", argv[0]);
        return 1;
    }
    if (srt_startup() != 0) {
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short) strtoul(argv[1], NULL, 10));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (i = 2; i < argc; i++) {
        peers[count] = srt_create_socket();
        if (peers[count] == SRT_INVALID_SOCK) {
            goto done;
        }
        count++;
        if (srt_setsockopt(peers[count - 1], 0, SRTO_TRANSTYPE, &live,
                          sizeof(live)) != 0
            || srt_setsockopt(peers[count - 1], 0, SRTO_SENDER, &sender,
                              sizeof(sender)) != 0
            || srt_setsockopt(peers[count - 1], 0, SRTO_CONNTIMEO, &timeout,
                              sizeof(timeout)) != 0
            || srt_setsockopt(peers[count - 1], 0, SRTO_SNDTIMEO, &timeout,
                              sizeof(timeout)) != 0
            || srt_setsockopt(peers[count - 1], 0, SRTO_PEERIDLETIMEO,
                              &peer_idle, sizeof(peer_idle)) != 0
            || (argv[i][0] != '\0'
                && srt_setsockopt(peers[count - 1], 0, SRTO_STREAMID, argv[i],
                                  (int) strlen(argv[i])) != 0)
            || srt_connect(peers[count - 1], (struct sockaddr *) &address,
                            sizeof(address)) != 0)
        {
            fprintf(stderr, "peer %d failed to connect: %s\n", count,
                    srt_getlasterror_str());
            goto done;
        }
    }

    printf("CONNECTED %d\n", count);
    fflush(stdout);
    while (fgets(command, sizeof(command), stdin) != NULL) {
        command[strcspn(command, "\n")] = '\0';
        if (strcmp(command, "closed") == 0) {
            deadline = now_ms() + CLOSE_DEADLINE_MS;
            while (connected(peers, count) != 0 && now_ms() < deadline) {
                nanosleep(&pause, NULL);
            }
            printf("CONNECTED %d\n", connected(peers, count));
        } else if (strcmp(command, "alive") == 0) {
            printf("CONNECTED %d\n", connected(peers, count));
        } else if (strncmp(command, "send ", 5) == 0 && count == 1) {
            if (send_fixture(peers[0], command + 5) != 0) {
                fprintf(stderr, "fixture send failed\n");
                goto done;
            }
            printf("SENT\n");
        } else if (strcmp(command, "quit") == 0) {
            break;
        } else {
            fprintf(stderr, "unknown command\n");
            goto done;
        }
        fflush(stdout);
    }
    rc = 0;

done:
    for (i = 0; i < count; i++) {
        srt_close(peers[i]);
    }
    srt_cleanup();
    return rc;
}
