#ifndef NGX_MEDIA_KEY_H
#define NGX_MEDIA_KEY_H

#include "ngx_media.h"

/*
 * Ingest keys (goal doc 11.6).
 *
 * A publisher attaches to a source by presenting a key: an SRT stream id, an
 * RTMP app/name, an HLS push path prefix.  The key is a bearer credential -
 * whoever holds it can publish as that source - so the product never stores
 * it: a key is generated here, hashed here, and the plaintext leaves the
 * process exactly once, in the response that issued it.
 *
 * A key is entirely random: 26 characters of Crockford base32 (130 bits),
 * with no part derived from the graph.  The readable handle for a source is
 * its id, which the API, the metrics and the logs all carry - a key that
 * spelled the source's name would go stale the moment the source was renamed,
 * and would let a key holder enumerate the graph.  One token, no separators a
 * transport might treat specially: SRT carries it as a stream id, RTMP as the
 * stream name of whatever app the device insists on, HLS push as one path
 * segment.  Nothing parses it - it either matches a provisioned key or is
 * refused - and what it feeds comes from the provisioning.
 */

/* the secret's length in characters, and the entropy that implies */
#define NGX_MEDIA_KEY_SECRET_LEN  26

/* SHA-256 of the key: what the graph keeps and what is compared */
#define NGX_MEDIA_KEY_HASH_LEN    NGX_MEDIA_SOURCE_KEY_HASH

/*
 * The first six bytes of the hash, in hex: what a refusal is logged with, and
 * what an operator greps the API for.  Long enough that two keys in one graph
 * will not share one.  One definition, in ngx_media.h, because the source
 * struct sizes its own array with it.
 */
#define NGX_MEDIA_KEY_PRINT_LEN   NGX_MEDIA_SOURCE_KEY_PRINT

/*
 * Issues a key for one source.  `application`, `name` and `id` are the
 * readable half; the secret is generated from the system's entropy source.
 * The plaintext is written to `out` (which must have room for
 * NGX_MEDIA_KEY_MAX), its hash to `hash` and its fingerprint to `print`.
 */
/* room for the secret, and a NUL a caller may want to add */
#define NGX_MEDIA_KEY_MAX  (NGX_MEDIA_KEY_SECRET_LEN + 8)

ngx_int_t ngx_media_key_issue(u_char *out, size_t cap, size_t *out_len,
    u_char *hash, u_char *print);

/* the hash and fingerprint of a key a publisher presented */
void ngx_media_key_hash(const u_char *key, size_t len, u_char *hash);
void ngx_media_key_print(const u_char *hash, u_char *print);

#endif /* NGX_MEDIA_KEY_H */
