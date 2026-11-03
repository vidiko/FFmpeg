/*
 * Copyright (c) 2026 lusoris
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavformat/avformat.h"
#include "libavformat/httpauth.h"
#include "libavformat/internal.h"
#include "libavformat/rtsp.h"
#include "libavformat/url.h"
#include "libavutil/adler32.h"
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"

typedef struct TestContext {
    unsigned char buf[MAX_URL_SIZE];
    int len, fail_write, error, writes;
} TestContext;

static int test_write(URLContext *h, const unsigned char *buf, int size)
{
    TestContext *ctx = h->priv_data;

    if (ctx->writes++ == ctx->fail_write)
        return ctx->error;
    if (size > MAX_URL_SIZE - ctx->len)
        return AVERROR(ENOSPC);
    memcpy(ctx->buf + ctx->len, buf, size);
    ctx->len += size;
    return size;
}

static const URLProtocol test_protocol = {
    .name      = "test",
    .url_write = test_write,
};

static char user_agent[] = "rtsp-test";
static AVFormatContext s;
static RTSPState rt;
static TestContext ctx;

static void init_context(void)
{
    URLContext *url;

    ffurl_closep(&rt.rtsp_hd_out);
    if (!(url = av_mallocz(sizeof(*url))))
        exit(1);
    *url = (URLContext) { .prot = &test_protocol, .priv_data = &ctx,
                          .flags = AVIO_FLAG_WRITE };
    s = (AVFormatContext) { .priv_data = &rt };
    rt = (RTSPState) { .rtsp_hd_out = url, .seq = 41,
                       .user_agent = user_agent };
    ctx = (TestContext) { .fail_write = -1 };
}

static int ends_with_crlfcrlf(const TestContext *c)
{
    return c->len >= 4 && !memcmp(c->buf + c->len - 4, "\r\n\r\n", 4);
}

static const char *err_name(int ret)
{
    if (ret >= 0)                    return "0";
    if (ret == AVERROR(EINVAL))       return "EINVAL";
    if (ret == AVERROR(ENOSPC))       return "ENOSPC";
    if (ret == AVERROR(EPIPE))        return "EPIPE";
    return ret == AVERROR_PATCHWELCOME ? "PATCHWELCOME" : "other";
}

static void print_result(const char *name, const char *method, int ret,
                         int seq_before, int seq_after)
{
    printf("%s method=%s rc=%s bytes=%d cseq=%d/%d crlf=%d adler32=%08x\n",
           name, method, err_name(ret), ctx.len, seq_before, seq_after,
           ends_with_crlfcrlf(&ctx),
           (unsigned)av_adler32_update(0, ctx.buf, ctx.len));
}

static int send_rtsp(const char *method, const char *headers,
                     const unsigned char *content, int clen)
{
    return ff_rtsp_send_cmd_with_content_async(&s, method,
                                               "rtsp://example/test",
                                               headers, content, clen);
}

static int run_case(const char *name, const char *method, const char *headers,
                    const unsigned char *content, int clen,
                    int fail, int error, int tunnel)
{
    int seq_before, ret;

    init_context();
    if (fail >= 0) {
        ctx.fail_write = fail;
        ctx.error = error;
    }
    if (tunnel)
        rt.control_transport = RTSP_MODE_TUNNEL;
    seq_before = rt.seq;
    ret = send_rtsp(method, headers, content, clen);
    print_result(name, method, ret, seq_before, rt.seq);
    return ctx.len;
}

static void set_digest_auth(void)
{
    av_strlcpy(rt.auth, "user:pass", sizeof(rt.auth));
    rt.auth_state.auth_type = HTTP_AUTH_DIGEST;
    av_strlcpy(rt.auth_state.realm, "realm", sizeof(rt.auth_state.realm));
    av_strlcpy(rt.auth_state.digest_params.nonce, "nonce",
               sizeof(rt.auth_state.digest_params.nonce));
    av_strlcpy(rt.auth_state.digest_params.qop, "auth",
               sizeof(rt.auth_state.digest_params.qop));
    rt.auth_state.digest_params.nc = 7;
}

static int run_digest_case(const char *name, const char *headers)
{
    int seq_before, nc_before, ret;

    init_context();
    set_digest_auth();
    seq_before = rt.seq;
    nc_before = rt.auth_state.digest_params.nc;
    ret = send_rtsp("OPTIONS", headers, NULL, 0);
    printf("%s method=OPTIONS rc=%s bytes=%d cseq=%d/%d nc=%d/%d\n",
           name, err_name(ret), ctx.len, seq_before, rt.seq, nc_before,
           rt.auth_state.digest_params.nc);
    return ctx.len;
}

int main(void)
{
    unsigned char content[] = "body";
    char headers[MAX_URL_SIZE + 1];
    int bare_len, fit_len;

    bare_len = run_case("normal", "OPTIONS", NULL, NULL, 0, -1, 0, 0);

    fit_len = MAX_URL_SIZE - 1 - bare_len;
    memset(headers, 'a', fit_len + 1);
    memcpy(headers, "X: ", 3);
    headers[fit_len - 2] = '\r';
    headers[fit_len - 1] = '\n';
    headers[fit_len] = 0;
    run_case("exact", "OPTIONS", headers, NULL, 0, -1, 0, 0);

    headers[fit_len] = 'a';
    headers[fit_len + 1] = 0;
    run_case("overflow", "OPTIONS", headers, NULL, 0, -1, 0, 0);

    run_case("header-fail", "OPTIONS", NULL, NULL, 0, 0, AVERROR(ENOSPC), 0);
    run_case("body-fail", "ANNOUNCE", NULL, content, sizeof(content) - 1,
             1, AVERROR(EPIPE), 0);
    run_case("tunnel-body", "ANNOUNCE", NULL, content, sizeof(content) - 1,
             -1, 0, 1);

    fit_len = MAX_URL_SIZE - run_digest_case("digest", NULL);
    memset(headers, 'a', fit_len + 1);
    headers[fit_len] = 0;
    run_digest_case("digest-overflow", headers);

    ffurl_closep(&rt.rtsp_hd_out);
    return 0;
}
