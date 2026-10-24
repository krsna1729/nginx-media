# Common API setup for integration and benchmark publishers. Newly-created
# sources return their key with the create response; idempotent creates read it
# explicitly afterward.
#
# MEDIA_TEST_CURL is the command that reaches the API.  Callers leave it unset
# to use this host's curl; a harness whose sender lives in its own network
# namespace sets it to a wrapper that enters that namespace, because the
# unauthenticated administration API binds the sender's loopback and is never
# exposed on the wire.  The default keeps this helper usable on its own.
media_test_ingest_key() { # <api> <application> <stream> <source> <type> <priority>
    local api="$1" application="$2" stream="$3" source="$4" type="$5"
    local priority="$6" endpoint="$1/streams/$2/$3" response key
    local -a api_curl=()
    read -r -a api_curl <<< "${MEDIA_TEST_CURL:-curl}"

    "${api_curl[@]}" -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"application\":\"$application\",\"name\":\"$stream\"}" \
        "$api/streams" >/dev/null || return
    response="$("${api_curl[@]}" -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"id\":\"$source\",\"type\":\"$type\",\"priority\":$priority}" \
        "$endpoint/sources")" || return
    key="$(printf '%s' "$response" \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("key",""))')" \
        || return
    if [ -n "$key" ]; then
        printf '%s' "$key"
        return 0
    fi
    "${api_curl[@]}" -fsS "$endpoint/sources/$source/key" \
        | python3 -c 'import json,sys; key=json.load(sys.stdin).get("key"); assert isinstance(key,str) and key; print(key)'
}

media_test_srt_publisher_url() { # <api> <host> <port> <application> <stream> <source> <priority>
    local api="$1" host="$2" port="$3" application="$4" stream="$5"
    local source="$6" priority="$7" key

    key="$(media_test_ingest_key "$api" "$application" "$stream" "$source" \
        srt "$priority")" || return
    printf 'srt://%s:%s?mode=caller&streamid=%s' \
        "$host" "$port" "$key"
}

media_test_rtmp_publisher_url() { # <api> <scheme> <host> <port> <application> <stream> <source> <priority>
    local api="$1" scheme="$2" host="$3" port="$4" application="$5"
    local stream="$6" source="$7" priority="$8" key

    key="$(media_test_ingest_key "$api" "$application" "$stream" "$source" \
        rtmp "$priority")" || return
    printf '%s://%s:%s/%s/%s' "$scheme" "$host" "$port" "$application" "$key"
}
