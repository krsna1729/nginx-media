#include "ngx_media_test.h"
#include "ngx_media_srt_streamid.h"

static ngx_int_t
parse(const char *s, ngx_media_srt_streamid_t *id)
{
    return ngx_media_srt_streamid_parse((const u_char *) s, strlen(s), id);
}

static ngx_int_t
streq(const ngx_str_t *str, const char *lit)
{
    size_t  len = strlen(lit);

    return (str->len == len && ngx_memcmp(str->data, lit, len) == 0) ? 1 : 0;
}

int
main(void)
{
    ngx_media_srt_streamid_t  id;
    char                      long_id[NGX_MEDIA_SRT_STREAMID_MAX + 32];

    TEST_CASE("canonical publish stream id");
    TEST_ASSERT_EQ_INT(parse("#!::r=live/news,m=publish,s=encoder-a", &id),
                       NGX_OK);
    TEST_ASSERT(streq(&id.resource, "live/news"));
    TEST_ASSERT(streq(&id.application, "live"));
    TEST_ASSERT(streq(&id.stream, "news"));
    TEST_ASSERT(streq(&id.source, "encoder-a"));
    TEST_ASSERT(streq(&id.mode, "publish"));
    TEST_ASSERT_EQ_U64(id.mode_kind, NGX_MEDIA_SRT_MODE_PUBLISH);
    TEST_ASSERT_EQ_U64(id.pairs, 3);
    TEST_ASSERT_EQ_U64(id.raw.len, strlen("#!::r=live/news,m=publish,s=encoder-a"));

    TEST_CASE("prefix is optional and request mode is recognised");
    TEST_ASSERT_EQ_INT(parse("r=live/news,m=request", &id), NGX_OK);
    TEST_ASSERT_EQ_U64(id.mode_kind, NGX_MEDIA_SRT_MODE_REQUEST);
    TEST_ASSERT_EQ_U64(id.source.len, 0);

    TEST_CASE("unknown mode maps to UNKNOWN without failing");
    TEST_ASSERT_EQ_INT(parse("r=live/news,m=Publish", &id), NGX_OK);
    TEST_ASSERT_EQ_U64(id.mode_kind, NGX_MEDIA_SRT_MODE_UNKNOWN);

    TEST_CASE("unknown keys are ignored, resource is mandatory");
    TEST_ASSERT_EQ_INT(parse("r=live/news,m=publish,s=e,x=y,t=1", &id), NGX_OK);
    TEST_ASSERT_EQ_U64(id.pairs, 5);
    TEST_ASSERT_EQ_INT(parse("m=publish,s=encoder-a", &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("m=publish", &id), NGX_ERROR);

    TEST_CASE("resource without a slash yields an empty application");
    TEST_ASSERT_EQ_INT(parse("r=news,m=publish", &id), NGX_OK);
    TEST_ASSERT_EQ_U64(id.application.len, 0);
    TEST_ASSERT(streq(&id.stream, "news"));

    TEST_CASE("malformed pairs are rejected");
    TEST_ASSERT_EQ_INT(parse("", &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live/news,", &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live/news,m", &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live/", &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live/ne ws", &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live/news\r\n", &id), NGX_ERROR);

    TEST_CASE("a percent-encoded stream id is refused, not decoded");
    /*
     * The encoding belongs to the URL that carried the stream id.  Decoding
     * it here would be compensating for a client that did not decode its own
     * URL - which is a client bug, not a contract - and it would make a
     * literal percent in an operator's stream id mean two different things.
     * A caller that wants the bytes decoded decodes them before setting
     * SRTO_STREAMID.
     */
    TEST_ASSERT_EQ_INT(parse("#!::r%3Dlive%2Fnews%2Cm%3Dpublish", &id),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live%2Fnews", &id), NGX_ERROR);

    TEST_CASE("duplicate keys are rejected");
    TEST_ASSERT_EQ_INT(parse("r=live/news,r=live/other", &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live/news,m=publish,m=request", &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live/news,s=a,s=b", &id), NGX_ERROR);

    TEST_CASE("oversized names and stream ids are rejected");
    memset(long_id, 'a', sizeof(long_id));
    long_id[sizeof(long_id) - 1] = '\0';
    TEST_ASSERT_EQ_INT(parse(long_id, &id), NGX_ERROR);

    memset(long_id, 'a', sizeof(long_id));
    memcpy(long_id, "r=", 2);
    long_id[2 + NGX_MEDIA_SRT_STREAMID_NAME_MAX] = '\0';
    TEST_ASSERT_EQ_INT(parse(long_id, &id), NGX_OK);

    long_id[2 + NGX_MEDIA_SRT_STREAMID_NAME_MAX] = 'a';
    long_id[2 + NGX_MEDIA_SRT_STREAMID_NAME_MAX + 1] = '\0';
    TEST_ASSERT_EQ_INT(parse(long_id, &id), NGX_ERROR);

    TEST_CASE("NULL tolerance");
    TEST_ASSERT_EQ_INT(ngx_media_srt_streamid_parse(NULL, 8, &id), NGX_ERROR);
    TEST_ASSERT_EQ_INT(parse("r=live/news", NULL), NGX_ERROR);
    ngx_media_srt_streamid_init(NULL);

    TEST_LEAKS();

    TEST_MAIN_END();
}
