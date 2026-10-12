/* Native nginx pools, socket pairs and shared mmap; no runtime shim. */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <assert.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/wait.h>

/* Include the implementations to inspect the private cycle-owned matrix.
 * Assertions exercise descriptor and process behavior, never source text. */
#include "../../src/core/ngx_media_route.c"
#include "../../src/core/ngx_media_owner_dir.c"

ngx_module_t ngx_core_module = { .index = 0 };
volatile ngx_cycle_t *ngx_cycle;
ngx_pid_t ngx_pid;

static ngx_log_t test_log;

static ngx_cycle_t *
make_cycle(ngx_uint_t workers)
{
    ngx_pool_t *pool = ngx_create_pool(16384, &test_log);
    assert(pool != NULL);
    ngx_cycle_t *cycle = ngx_pcalloc(pool, sizeof(*cycle));
    ngx_core_conf_t *conf = ngx_pcalloc(pool, sizeof(*conf));
    assert(cycle != NULL && conf != NULL);
    cycle->pool = pool;
    cycle->log = &test_log;
    cycle->conf_ctx = ngx_pcalloc(pool, sizeof(void *));
    assert(cycle->conf_ctx != NULL);
    cycle->conf_ctx[0] = (void ***) conf;
    conf->worker_processes = workers;
    return cycle;
}

static int
fd_inventory(int *highest)
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory != NULL);
    int count = 0;
    *highest = -1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        char *end;
        long fd = strtol(entry->d_name, &end, 10);
        if (*end != '\0' || fd == dirfd(directory)) {
            continue;
        }
        count++;
        if (fd > *highest) {
            *highest = (int) fd;
        }
    }
    assert(closedir(directory) == 0);
    return count;
}

static void
expect_empty(int fd)
{
    ngx_media_ipc_header_t message;
    assert(recv(fd, &message, sizeof(message), MSG_DONTWAIT) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void
transfer(int sender, int receiver, uint8_t type)
{
    ngx_media_ipc_header_t sent = {
        .version = NGX_MEDIA_IPC_VERSION, .type = type,
        .hash = UINT64_C(0x123456789abcdef0), .incarnation = 77, .session = 88
    };
    ngx_media_ipc_header_t received;
    assert(send(sender, &sent, sizeof(sent), MSG_NOSIGNAL) == (ssize_t) sizeof(sent));
    assert(recv(receiver, &received, sizeof(received), 0) == (ssize_t) sizeof(received));
    assert(memcmp(&sent, &received, sizeof(sent)) == 0);
}

static void
routing_lifecycle(void)
{
    int highest;
    int baseline = fd_inventory(&highest);
    ngx_cycle_t *old = make_cycle(2);
    assert(ngx_media_route_master_init(old, &test_log) == NGX_OK);
    ngx_media_route_generation_t *old_route = ngx_media_route_generation(old);
    assert(old_route != NULL);
    int old_master_a = old_route->fds[0][1];
    int old_master_b = old_route->fds[1][0];
    int old_worker_a = dup(old_master_a);
    int old_worker_b = dup(old_master_b);
    assert(old_worker_a >= 0 && old_worker_b >= 0);

    ngx_cycle_t *current = make_cycle(2);
    assert(ngx_media_route_master_init(current, &test_log) == NGX_OK);
    ngx_media_route_generation_t *route = ngx_media_route_generation(current);
    assert(route->fds[0][1] != old_master_a);
    assert(route->fds[1][0] != old_master_b);
    for (uint8_t type = NGX_MEDIA_IPC_MSG_OPEN;
         type <= NGX_MEDIA_IPC_MSG_VIDEO; type++)
    {
        transfer(old_worker_a, old_worker_b, type);
        expect_empty(route->fds[1][0]);
        transfer(route->fds[0][1], route->fds[1][0], type);
        expect_empty(old_worker_b);
    }

    ngx_destroy_pool(old->pool);
    assert(fcntl(old_master_a, F_GETFD) == -1 && errno == EBADF);
    assert(fcntl(old_master_b, F_GETFD) == -1 && errno == EBADF);
    transfer(old_worker_a, old_worker_b, NGX_MEDIA_IPC_MSG_CLOSE);
    transfer(route->fds[0][1], route->fds[1][0], NGX_MEDIA_IPC_MSG_OPEN);
    assert(fd_inventory(&highest) == baseline + 4);

    /* Fill inherited descriptor holes, then leave exactly one pair free. */
    struct rlimit saved, bounded;
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    bounded = saved;
    bounded.rlim_cur = (rlim_t) highest + 3;
    assert(bounded.rlim_cur <= saved.rlim_max);
    assert(setrlimit(RLIMIT_NOFILE, &bounded) == 0);
    int fillers[bounded.rlim_cur];
    size_t filled = 0;
    for (;;) {
        int fd = fcntl(old_worker_a, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) {
            assert(errno == EMFILE);
            break;
        }
        assert(filled < bounded.rlim_cur);
        fillers[filled++] = fd;
    }
    assert(filled >= 2);
    assert(close(fillers[--filled]) == 0);
    assert(close(fillers[--filled]) == 0);
    ngx_cycle_t *failed = make_cycle(3);
    assert(ngx_media_route_master_init(failed, &test_log) == NGX_ERROR);
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    while (filled != 0) {
        assert(close(fillers[--filled]) == 0);
    }
    assert(fd_inventory(&highest) == baseline + 4);
    ngx_destroy_pool(failed->pool);
    transfer(route->fds[0][1], route->fds[1][0], NGX_MEDIA_IPC_MSG_VIDEO);

    ngx_cycle_t *oversized = make_cycle(NGX_MEDIA_ROUTE_MAX_WORKERS + 1);
    assert(ngx_media_route_master_init(oversized, &test_log) == NGX_ERROR);
    ngx_destroy_pool(oversized->pool);
    assert(fd_inventory(&highest) == baseline + 4);

    for (int i = 0; i < 20; i++) {
        ngx_cycle_t *next = make_cycle(i % 2 == 0 ? 1 : 2);
        assert(ngx_media_route_master_init(next, &test_log) == NGX_OK);
        ngx_destroy_pool(current->pool);
        current = next;
        assert(fd_inventory(&highest) == baseline + 2 + (i % 2 == 0 ? 0 : 2));
        transfer(old_worker_a, old_worker_b, NGX_MEDIA_IPC_MSG_CLOSE);
    }
    ngx_destroy_pool(current->pool);
    assert(close(old_worker_a) == 0);
    assert(close(old_worker_b) == 0);
    assert(fd_inventory(&highest) == baseline);
    puts("route lifecycle: isolated same-count pairs, draining copies, atomic failure and bounded descriptors");
}

static void
owner_takeover(void)
{
    ngx_cycle_t *old = make_cycle(2);
    assert(ngx_media_owner_dir_shm_create(old, 2, &test_log) == NGX_OK);
    ngx_media_owner_dir_t *old_dir = ngx_media_owner_dir_attach(old, &test_log);
    assert(old_dir != NULL);
    int ready[2], resume[2];
    assert(pipe(ready) == 0 && pipe(resume) == 0);
    ngx_pid_t old_pid = fork();
    assert(old_pid >= 0);
    if (old_pid == 0) {
        assert(close(ready[0]) == 0 && close(resume[1]) == 0);
        ngx_pid = getpid();
        assert(ngx_media_owner_dir_claim(old_dir, 1, 1) != NULL);
        ngx_media_owner_dir_heartbeat(old_dir, 1, 1, 5, 111, 1);
        assert(ngx_media_owner_dir_revision_next(old_dir) == 1);
        char byte = 'r';
        assert(write(ready[1], &byte, 1) == 1);
        assert(read(resume[0], &byte, 1) == 1);
        ngx_media_owner_dir_heartbeat(old_dir, 1, 1, 99, 999, 9);
        ngx_media_owner_dir_release(old_dir, 1, 1);
        assert(ngx_media_owner_dir_claim(old_dir, 1, 1) == NULL);
        assert(ngx_media_owner_dir_revision_next(old_dir) == 3);
        _exit(0);
    }
    assert(close(ready[1]) == 0 && close(resume[0]) == 0);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    ngx_media_owner_record_t record;
    assert(ngx_media_owner_dir_observe(old_dir, 1, &record) == NGX_OK);
    assert(record.pid == old_pid && record.frames == 111);

    ngx_cycle_t *current = make_cycle(2);
    assert(ngx_media_owner_dir_shm_create(current, 2, &test_log) == NGX_OK);
    ngx_media_owner_dir_t *dir = ngx_media_owner_dir_attach(current, &test_log);
    assert(dir != NULL);
    assert(ngx_media_owner_dir_claim(dir, 1, 0) == NULL);
    assert(ngx_media_owner_dir_claim(dir, 1, 1) != NULL);
    ngx_media_owner_dir_heartbeat(dir, 1, 1, 10, 222, 2);
    assert(ngx_media_owner_dir_revision_next(dir) == 2);
    assert(write(resume[1], "g", 1) == 1);
    int status;
    assert(waitpid(old_pid, &status, 0) == old_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(ngx_media_owner_dir_observe(dir, 1, &record) == NGX_OK);
    assert(record.pid == ngx_pid && record.slot == 1);
    assert(record.generation == 10 && record.frames == 222 && record.sources == 2);
    assert(ngx_media_owner_dir_revision_next(dir) == 4);

    /* Slot selection, capacity and deleted-slot reuse stay deterministic. */
    assert(ngx_media_owner_dir_claim(dir, 3, 1) != NULL);
    assert(ngx_media_owner_dir_claim(dir, 5, 1) == NULL);
    ngx_media_owner_dir_release(dir, 1, 1);
    ngx_media_owner_dir_heartbeat(dir, 1, 1, 99, 999, 9);
    assert(ngx_media_owner_dir_observe(dir, 1, &record) == NGX_ERROR);
    assert(ngx_media_owner_dir_observe(dir, 3, &record) == NGX_OK);
    assert(record.hash == 3 && record.pid == ngx_pid && record.slot == 1);
    assert(ngx_media_owner_dir_claim(dir, 5, 1) != NULL);
    assert(ngx_media_owner_dir_observe(dir, 3, &record) == NGX_OK);
    assert(record.hash == 3 && record.pid == ngx_pid);
    ngx_media_owner_dir_heartbeat(dir, 5, 1, 12, 333, 1);
    assert(ngx_media_owner_dir_observe(dir, 5, &record) == NGX_OK);
    assert(record.pid == ngx_pid && record.frames == 333);
    ngx_media_owner_dir_heartbeat(dir, 5, 0, 99, 999, 9);
    assert(ngx_media_owner_dir_observe(dir, 5, &record) == NGX_OK);
    assert(record.frames == 333);
    assert(close(ready[0]) == 0 && close(resume[1]) == 0);
    ngx_destroy_pool(old->pool);
    ngx_destroy_pool(current->pool);
    size_t size = sizeof(ngx_media_owner_dir_shm_t)
                  + sizeof(ngx_media_owner_record_t);
    assert(munmap(ngx_media_owner_dir_mapped, size) == 0);
    puts("owner directory: current PID survives stale heartbeat/release/reclaim and child exit; revisions remain ordered");
}

int
main(void)
{
    ngx_pid = getpid();
    ngx_pagesize = (ngx_uint_t) sysconf(_SC_PAGESIZE);
    ngx_cacheline_size = 64;
    ngx_time_init();
    routing_lifecycle();
    owner_takeover();
    return 0;
}
