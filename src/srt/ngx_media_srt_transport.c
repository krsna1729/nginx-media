#include "ngx_media_srt_udp.h"
#include "ngx_media_srt_transport.h"

/*
 * The backends are weak so that a build can carry either, both or neither of
 * them without a compile-time flag: whichever implementation is linked in is
 * the one this default resolves to, and media_srt_backend switches between
 * them at runtime when both are present (goal doc 11.1).
 */
extern ngx_media_srt_ops_t  ngx_media_srt_haivision_ops
    __attribute__((weak));
extern ngx_media_srt_ops_t  ngx_media_srt_udp_ops
    __attribute__((weak));

/* the backend chosen with media_srt_backend; NULL means "the default" */
static ngx_media_srt_ops_t  *ngx_media_srt_selected;

ngx_media_srt_ops_t *
ngx_media_srt_backend(void)
{
    if (ngx_media_srt_selected != NULL) {
        return ngx_media_srt_selected;
    }

    if (&ngx_media_srt_haivision_ops != NULL) {
        return &ngx_media_srt_haivision_ops;
    }

    return &ngx_media_srt_udp_ops;
}

void
ngx_media_srt_set_backend(ngx_media_srt_ops_t *ops)
{
    if (ops != NULL) {
        ngx_media_srt_selected = ops;
    }
}

ngx_media_srt_listener_t *
ngx_media_srt_listen(const u_char *host, ngx_uint_t port, ngx_log_t *log)
{
    if (ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->listen == NULL)
    {
        return NULL;
    }

    return ngx_media_srt_backend()->listen(host, port, log);
}

void
ngx_media_srt_listen_close(ngx_media_srt_listener_t *listener)
{
    if (listener == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->listen_close == NULL)
    {
        return;
    }

    ngx_media_srt_backend()->listen_close(listener);
}

ngx_media_srt_session_t *
ngx_media_srt_accept(ngx_media_srt_listener_t *listener, ngx_msec_t timeout_ms,
    ngx_log_t *log)
{
    if (listener == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->accept == NULL)
    {
        return NULL;
    }

    return ngx_media_srt_backend()->accept(listener, timeout_ms, log);
}

ngx_media_srt_session_t *
ngx_media_srt_accept_ready(ngx_media_srt_listener_t *listener, ngx_log_t *log)
{
    if (listener == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->accept_ready == NULL)
    {
        return NULL;
    }

    return ngx_media_srt_backend()->accept_ready(listener, log);
}

ngx_int_t
ngx_media_srt_session_streamid(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap)
{
    if (session == NULL || buf == NULL || cap == 0
        || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->streamid == NULL)
    {
        return -1;
    }

    return ngx_media_srt_backend()->streamid(session, buf, cap);
}

ngx_int_t
ngx_media_srt_session_recv(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap, ngx_msec_t timeout_ms)
{
    if (session == NULL || buf == NULL || cap == 0
        || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->recv == NULL)
    {
        return -1;
    }

    return ngx_media_srt_backend()->recv(session, buf, cap, timeout_ms);
}

void
ngx_media_srt_session_stats(ngx_media_srt_session_t *session,
    ngx_media_srt_stats_t *out)
{
    if (out != NULL) {
        ngx_memzero(out, sizeof(ngx_media_srt_stats_t));
    }

    if (session == NULL || out == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->stats == NULL)
    {
        return;
    }

    ngx_media_srt_backend()->stats(session, out);
}

void
ngx_media_srt_session_close(ngx_media_srt_session_t *session)
{
    if (session == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->session_close == NULL)
    {
        return;
    }

    ngx_media_srt_backend()->session_close(session);
}

void
ngx_media_srt_shutdown(void)
{
    if (ngx_media_srt_backend() == NULL || ngx_media_srt_backend()->shutdown == NULL) {
        return;
    }

    ngx_media_srt_backend()->shutdown();
}

ngx_media_srt_poll_t *
ngx_media_srt_poll_create(ngx_log_t *log)
{
    if (ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->poll_create == NULL)
    {
        return NULL;
    }

    return ngx_media_srt_backend()->poll_create(log);
}

void
ngx_media_srt_poll_destroy(ngx_media_srt_poll_t *poll)
{
    if (poll == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->poll_destroy == NULL)
    {
        return;
    }

    ngx_media_srt_backend()->poll_destroy(poll);
}

ngx_int_t
ngx_media_srt_poll_add_listener(ngx_media_srt_poll_t *poll,
    ngx_media_srt_listener_t *listener)
{
    if (poll == NULL || listener == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->poll_add_listener == NULL)
    {
        return NGX_ERROR;
    }

    return ngx_media_srt_backend()->poll_add_listener(poll, listener);
}

ngx_int_t
ngx_media_srt_poll_add_session(ngx_media_srt_poll_t *poll,
    ngx_media_srt_session_t *session)
{
    if (poll == NULL || session == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->poll_add_session == NULL)
    {
        return NGX_ERROR;
    }

    return ngx_media_srt_backend()->poll_add_session(poll, session);
}

void
ngx_media_srt_poll_remove_session(ngx_media_srt_poll_t *poll,
    ngx_media_srt_session_t *session)
{
    if (poll == NULL || session == NULL || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->poll_remove_session == NULL)
    {
        return;
    }

    ngx_media_srt_backend()->poll_remove_session(poll, session);
}

ngx_int_t
ngx_media_srt_poll_wait(ngx_media_srt_poll_t *poll, ngx_msec_t timeout_ms,
    ngx_media_srt_poll_event_t *events, ngx_uint_t max, ngx_uint_t *count)
{
    if (poll == NULL || events == NULL || count == NULL
        || ngx_media_srt_backend() == NULL
        || ngx_media_srt_backend()->poll_wait == NULL)
    {
        return NGX_ERROR;
    }

    return ngx_media_srt_backend()->poll_wait(poll, timeout_ms, events, max,
                                            count);
}

ngx_media_srt_session_t *
ngx_media_srt_connect(const u_char *host, ngx_uint_t port,
    const u_char *streamid, size_t streamid_len, ngx_msec_t timeout_ms,
    ngx_log_t *log)
{
    if (ngx_media_srt_backend()->connect == NULL) {
        return NULL;
    }

    return ngx_media_srt_backend()->connect(host, port, streamid, streamid_len,
                                          timeout_ms, log);
}

ngx_int_t
ngx_media_srt_session_send(ngx_media_srt_session_t *session, const u_char *buf,
    size_t len, ngx_msec_t timeout_ms)
{
    if (ngx_media_srt_backend()->send == NULL) {
        return NGX_ERROR;
    }

    return ngx_media_srt_backend()->send(session, buf, len, timeout_ms);
}

const char *
ngx_media_srt_last_error(void)
{
    if (ngx_media_srt_backend()->last_error == NULL) {
        return "";
    }

    return ngx_media_srt_backend()->last_error();
}
