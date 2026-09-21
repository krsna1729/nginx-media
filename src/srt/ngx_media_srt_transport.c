#include "ngx_media_srt_transport.h"

ngx_media_srt_ops_t *ngx_media_srt_backend = &ngx_media_srt_haivision_ops;

void
ngx_media_srt_set_backend(ngx_media_srt_ops_t *ops)
{
    if (ops != NULL) {
        ngx_media_srt_backend = ops;
    }
}

ngx_media_srt_listener_t *
ngx_media_srt_listen(const u_char *host, ngx_uint_t port, ngx_log_t *log)
{
    if (ngx_media_srt_backend == NULL
        || ngx_media_srt_backend->listen == NULL)
    {
        return NULL;
    }

    return ngx_media_srt_backend->listen(host, port, log);
}

void
ngx_media_srt_listen_close(ngx_media_srt_listener_t *listener)
{
    if (listener == NULL || ngx_media_srt_backend == NULL
        || ngx_media_srt_backend->listen_close == NULL)
    {
        return;
    }

    ngx_media_srt_backend->listen_close(listener);
}

ngx_media_srt_session_t *
ngx_media_srt_accept(ngx_media_srt_listener_t *listener, ngx_msec_t timeout_ms,
    ngx_log_t *log)
{
    if (listener == NULL || ngx_media_srt_backend == NULL
        || ngx_media_srt_backend->accept == NULL)
    {
        return NULL;
    }

    return ngx_media_srt_backend->accept(listener, timeout_ms, log);
}

ngx_int_t
ngx_media_srt_session_streamid(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap)
{
    if (session == NULL || buf == NULL || cap == 0
        || ngx_media_srt_backend == NULL
        || ngx_media_srt_backend->streamid == NULL)
    {
        return -1;
    }

    return ngx_media_srt_backend->streamid(session, buf, cap);
}

ngx_int_t
ngx_media_srt_session_recv(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap, ngx_msec_t timeout_ms)
{
    if (session == NULL || buf == NULL || cap == 0
        || ngx_media_srt_backend == NULL
        || ngx_media_srt_backend->recv == NULL)
    {
        return -1;
    }

    return ngx_media_srt_backend->recv(session, buf, cap, timeout_ms);
}

void
ngx_media_srt_session_stats(ngx_media_srt_session_t *session,
    ngx_media_srt_stats_t *out)
{
    if (out != NULL) {
        ngx_memzero(out, sizeof(ngx_media_srt_stats_t));
    }

    if (session == NULL || out == NULL || ngx_media_srt_backend == NULL
        || ngx_media_srt_backend->stats == NULL)
    {
        return;
    }

    ngx_media_srt_backend->stats(session, out);
}

void
ngx_media_srt_session_close(ngx_media_srt_session_t *session)
{
    if (session == NULL || ngx_media_srt_backend == NULL
        || ngx_media_srt_backend->session_close == NULL)
    {
        return;
    }

    ngx_media_srt_backend->session_close(session);
}

void
ngx_media_srt_shutdown(void)
{
    if (ngx_media_srt_backend == NULL || ngx_media_srt_backend->shutdown == NULL) {
        return;
    }

    ngx_media_srt_backend->shutdown();
}
