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
      2000,     /* 2s segments by default */
      NGX_MEDIA_HLS_PUSH_POST,
      0,        /* YouTube expires segments itself: no DELETE */
    },

    { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
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
    const ngx_str_t *url, ngx_media_hls_push_settings_t *settings,
    const char **why)
{
    static const char  *ok = "";
    ngx_uint_t          min, max, window;

    if (why == NULL) {
        why = &ok;
    }

    *why = "";

    if (profile != NULL && profile->https_required) {

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
    min = (profile != NULL) ? profile->min_segment_ms
                            : NGX_MEDIA_HLS_PUSH_SEGMENT_MIN;
    max = (profile != NULL) ? profile->max_segment_ms
                            : NGX_MEDIA_HLS_PUSH_SEGMENT_MAX;
    window = (profile != NULL) ? profile->max_window
                               : NGX_MEDIA_HLS_PUSH_WINDOW_MAX;

    if (settings->segment_duration_ms == 0) {
        settings->segment_duration_ms = (profile != NULL)
                                        ? profile->default_segment_ms : 0;

    } else if (settings->segment_duration_ms < min
               || settings->segment_duration_ms > max)
    {
        *why = (profile != NULL)
               ? "segment duration is outside the profile's range"
               : "segment duration must be 1000-30000 ms";
        return NGX_DECLINED;
    }

    /*
     * The longest segment the destination takes: the platform's limit, or
     * for a generic destination twice what it asked for (RFC 8216 lets a
     * segment run to the target duration, and a keyframe interval rarely
     * lands exactly on it).
     */
    settings->segment_max_ms = (profile != NULL)
                               ? profile->max_segment_ms
                               : settings->segment_duration_ms * 2;

    if (settings->playlist_window == 0) {
        settings->playlist_window = (profile != NULL) ? profile->max_window
                                                      : 0;

    } else if (settings->playlist_window > window) {
        *why = (profile != NULL)
               ? "playlist window exceeds the profile's maximum"
               : "playlist window must be 1-32 segments";
        return NGX_DECLINED;
    }

    if (settings->method == 0) {
        settings->method = (profile != NULL) ? profile->default_method
                                             : NGX_MEDIA_HLS_PUSH_PUT;
    }

    if (settings->delete_expired < 0) {
        settings->delete_expired = (profile != NULL)
                                   ? (ngx_int_t) profile->delete_expired : 1;
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
