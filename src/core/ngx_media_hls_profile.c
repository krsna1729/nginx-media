#include "ngx_media_hls_profile.h"

/*
 * The YouTube Live HLS ingest contract, revision 1.
 *
 * Kept as data rather than as branches in the publisher: when the platform
 * changes its rules, this table changes and nothing else does.
 */
static const ngx_media_hls_profile_t  ngx_media_hls_profiles[] = {

    { "youtube_live", 1,
      1,        /* https required */
      1000,     /* 1s minimum segment */
      4000,     /* 4s maximum segment */
      5,        /* no more than 5 outstanding segments */
      1,        /* MPEG-TS */
    },

    { NULL, 0, 0, 0, 0, 0, 0 }
};

const ngx_media_hls_profile_t *
ngx_media_hls_profile_find(const ngx_str_t *name)
{
    const ngx_media_hls_profile_t  *profile;

    if (name == NULL || name->len == 0) {
        return NULL;
    }

    for (profile = ngx_media_hls_profiles; profile->name != NULL; profile++) {

        if (ngx_strlen(profile->name) == name->len
            && ngx_strncmp(profile->name, name->data, name->len) == 0)
        {
            return profile;
        }
    }

    return NULL;
}

ngx_int_t
ngx_media_hls_profile_apply(const ngx_media_hls_profile_t *profile,
    const ngx_str_t *url, ngx_uint_t *segment_duration_ms,
    ngx_uint_t *playlist_window, ngx_uint_t *http_post, const char **why)
{
    static const char  *ok = "";

    if (why == NULL) {
        why = &ok;
    }

    *why = "";

    if (profile == NULL) {
        return NGX_OK;
    }

    if (profile->https_required) {

        if (url == NULL || url->len < 8
            || ngx_strncasecmp(url->data, (u_char *) "https://", 8) != 0)
        {
            *why = "the profile requires an https endpoint";
            return NGX_DECLINED;
        }
    }

    /*
     * Defaults first, then validation: an unset duration takes the profile's
     * default, and a duration the platform would reject is refused rather
     * than quietly clamped - silently changing an operator's number is worse
     * than telling them it is wrong.
     */
    if (*segment_duration_ms == 0) {
        *segment_duration_ms = 2000;

    } else if (*segment_duration_ms < profile->min_segment_ms
               || *segment_duration_ms > profile->max_segment_ms)
    {
        *why = "segment duration is outside the profile's range";
        return NGX_DECLINED;
    }

    if (*playlist_window == 0) {
        *playlist_window = profile->max_window;

    } else if (*playlist_window > profile->max_window) {
        *why = "playlist window exceeds the profile's maximum";
        return NGX_DECLINED;
    }

    if (*http_post == 0) {
        *http_post = 1;
    }

    return NGX_OK;
}

void
ngx_media_redact_url(const ngx_str_t *url, u_char *buf, size_t cap,
    ngx_str_t *out)
{
    u_char  *end, *userinfo, *query;
    size_t   len;

    out->data = buf;
    out->len = 0;

    if (url == NULL || url->len == 0 || buf == NULL || cap < 2) {
        return;
    }

    end = url->data + url->len;

    /* everything from the query on is credential material */
    query = ngx_strlchr(url->data, end, '?');

    if (query != NULL) {
        end = query;
    }

    /* and userinfo before the host, as in https://user:key@host/ */
    userinfo = ngx_strlchr(url->data, end, '@');

    if (userinfo != NULL) {
        u_char  *scheme = ngx_strlchr(url->data, userinfo, '/');

        if (scheme != NULL && (size_t) (scheme + 1 - url->data) + 3 < cap) {
            len = (size_t) (scheme + 1 - url->data);
            ngx_memcpy(buf, url->data, len);
            ngx_memcpy(buf + len, "***", 3);
            buf[len + 3] = '\0';
            out->len = len + 3;

            return;
        }
    }

    len = (size_t) (end - url->data);

    if (len >= cap) {
        len = cap - 1;
    }

    ngx_memcpy(buf, url->data, len);
    buf[len] = '\0';
    out->len = len;
}
