#include <mbedtls/config.h>
#include <mbedtls/platform.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/certs.h>
#include <mbedtls/x509.h>
#include <mbedtls/error.h>
#include <mbedtls/debug.h>
#include <mbedtls/timing.h>

#include "dump_info.h"
#include "ssr_executive.h"
#include "tunnel.h"
#include "tls_cli.h"
#include "ssrbuffer.h"
#include "picohttpparser.h"
#include <uv.h>
#include <uv_mbed/uv_mbed.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ssrutils.h"

#define TLS_DUMP_INFO   0

#define GET_REQUEST_FORMAT ""                                                               \
    "POST %s HTTP/1.1\r\n"                                                                  \
    "Host: %s:%d\r\n"                                                                       \
    "User-Agent: Mozilla/5.0 (Windows NT 5.1; rv:52.0) Gecko/20100101 Firefox/52.0\r\n"     \
    "Accept: text/html,application/xhtml+xml,application/octet-stream;q=0.9,*/*;q=0.8\r\n"  \
    "Accept-Language: en-US,en;q=0.5\r\n"                                                   \
    "Accept-Encoding: gzip, deflate\r\n"                                                    \
    "Connection: keep-alive\r\n"                                                            \
    "Upgrade-Insecure-Requests: 1\r\n"                                                      \
    "Content-Type: application/octet-stream\r\n"                                            \
    "Content-Length: %d\r\n"                                                                \
    "\r\n"                                                                                  \

#define ALPN_LIST_SIZE  10
#define DFL_PSK_IDENTITY "Client_identity"
#define MAX_REQUEST_SIZE      0x8000
#define DFL_REQUEST_SIZE        -1
#define DFL_TRANSPORT           MBEDTLS_SSL_TRANSPORT_STREAM

enum tls_cli_state {
    tls_state_stopped,
    tls_state_connected,
    tls_state_data_coming,
    tls_state_shutting_down,
};

struct tls_cli_ctx {
#if 0
    struct uv_work_s *req;
    struct uv_async_s *async;
    mbedtls_ssl_context *ssl_ctx;
#endif
    struct tunnel_ctx *tunnel; /* weak pointer */
    struct server_config *config; /* weak pointer */
    uv_mbed_t *mbed;
};

struct tls_cli_ctx * create_tls_cli_ctx(struct tunnel_ctx *tunnel, struct server_config *config);
void destroy_tls_cli_ctx(struct tls_cli_ctx *ctx);

static void tls_cli_main_work_thread(uv_work_t *req);
static void tunnel_tls_send_data(struct tunnel_ctx *tunnel, const uint8_t *data, size_t size);
static bool tls_cli_send_data(mbedtls_ssl_context *ssl_ctx,
    const char *url_path, const char *domain, unsigned short domain_port,
    const uint8_t *data, size_t size);
static void tls_cli_state_changed_notice_cb(uv_async_t *handle);
static void tls_cli_after_cb(uv_work_t *req, int status);
static void tls_cli_state_changed_async_send(struct tls_cli_ctx *ctx, enum tls_cli_state state, const uint8_t *buf, size_t len);

struct tls_cli_state_ctx {
    struct buffer_t *data;
    struct tls_cli_ctx *ctx; /* weak ptr */
    enum tls_cli_state state;
};

void _mbed_connect_done_cb(uv_mbed_t* mbed, int status, void *p);
static void _mbed_alloc_done_cb(uv_mbed_t *mbed, size_t suggested_size, uv_buf_t *buf, void *p);
static void _mbed_data_received_cb(uv_mbed_t *mbed, ssize_t nread, uv_buf_t* buf, void *p);
static void _tls_cli_send_data(struct tls_cli_ctx *, const uint8_t *data, size_t size);
static void _mbed_write_done_cb(uv_mbed_t *mbed, int status, void *p);
static void _mbed_close_done_cb(uv_mbed_t *mbed, void *p);

void tls_client_launch(struct tunnel_ctx *tunnel, struct server_config *config) {
    uv_loop_t *loop = tunnel->listener->loop;
    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)calloc(1, sizeof(*ctx));
    ctx->mbed = uv_mbed_init(loop, NULL, 0);
    ctx->config = config;
    ctx->tunnel = tunnel;

    tunnel->tls_ctx = ctx;
    tunnel->tunnel_tls_send_data = &tunnel_tls_send_data;

    uv_mbed_connect(ctx->mbed, config->remote_host, config->remote_port, _mbed_connect_done_cb, ctx);
}

void _mbed_connect_done_cb(uv_mbed_t* mbed, int status, void *p) {
    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)p;
    struct tunnel_ctx *tunnel = ctx->tunnel;

    if (status < 0) {
        fprintf(stderr, "connect failed: %d: %s\n", status, uv_strerror(status));
        uv_mbed_close(mbed, _mbed_close_done_cb, p);
        return;
    }

    uv_mbed_read(mbed, _mbed_alloc_done_cb, _mbed_data_received_cb, p);

    if (tunnel->tunnel_tls_on_connection_established) {
        tunnel->tunnel_tls_on_connection_established(tunnel);
    }
}

static void _mbed_alloc_done_cb(uv_mbed_t *mbed, size_t suggested_size, uv_buf_t *buf, void *p) {
    char *base = (char *) calloc(suggested_size, sizeof(char));
    *buf = uv_buf_init(base, suggested_size);
}

static void _mbed_data_received_cb(uv_mbed_t *mbed, ssize_t nread, uv_buf_t* buf, void *p) {
    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)p;
    struct tunnel_ctx *tunnel = ctx->tunnel;
    assert(ctx->mbed == mbed);
    if (nread > 0) {
        assert(tunnel->tunnel_tls_on_data_coming);
        if (tunnel->tunnel_tls_on_data_coming) {
            tunnel->tunnel_tls_on_data_coming(tunnel, (uint8_t *)buf->base, (size_t)nread);
        }
    } else if (nread < 0) {
        if (nread == UV_EOF) {
            printf("=====================\nconnection closed\n");
        } else {
            fprintf(stderr, "read error %ld: %s\n", nread, uv_strerror((int) nread));
        }
        uv_mbed_close(mbed, _mbed_close_done_cb, p);
    }

    free(buf->base);
}

static void _tls_cli_send_data(struct tls_cli_ctx *ctx, const uint8_t *data, size_t size) {
    struct server_config *config = ctx->config;
    const char *url_path = config->over_tls_path;
    const char *domain = config->over_tls_server_domain;
    unsigned short domain_port = config->remote_port;
    uv_buf_t o;
    uint8_t *buf = (uint8_t *)calloc(MAX_REQUEST_SIZE + 1, sizeof(*buf));
    int len = mbedtls_snprintf((char *)buf, MAX_REQUEST_SIZE, GET_REQUEST_FORMAT,
        url_path, domain, domain_port, (int)size);

    if (data && size) {
        memcpy(buf + len, data, size);
        len += (int)size;
    }

    o = uv_buf_init((char *)buf, (unsigned int)len);
    uv_mbed_write(ctx->mbed, &o, &_mbed_write_done_cb, ctx);

    free(buf);
}

static void _mbed_write_done_cb(uv_mbed_t *mbed, int status, void *p) {
    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)p;
    assert(ctx->mbed == mbed);
    if (status < 0) {
        fprintf(stderr, "write failed: %d: %s\n", status, uv_strerror(status));
        uv_mbed_close(mbed, _mbed_close_done_cb, p);
    } else {
        printf("request sent %d\n", status);
    }
}

static void _mbed_close_done_cb(uv_mbed_t *mbed, void *p) {
    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)p;
    struct tunnel_ctx *tunnel = ctx->tunnel;
    assert(mbed == ctx->mbed);

    if (tunnel->tunnel_tls_on_shutting_down) {
        tunnel->tunnel_tls_on_shutting_down(tunnel);
    }

    uv_mbed_free(mbed);
    free(ctx);
}

static void tunnel_tls_send_data(struct tunnel_ctx *tunnel, const uint8_t *data, size_t size) {
    struct tls_cli_ctx *ctx = tunnel->tls_ctx;
    _tls_cli_send_data(ctx, data, size);
}

#if 0
void tls_client_launch(struct tunnel_ctx *tunnel, struct server_config *config) {
    uv_loop_t *loop = tunnel->listener->loop;
    struct tls_cli_ctx *ctx = create_tls_cli_ctx(tunnel, config);

    uv_async_init(loop, ctx->async, tls_cli_state_changed_notice_cb);
    uv_queue_work(loop, ctx->req, tls_cli_main_work_thread, tls_cli_after_cb);
}

struct tls_cli_ctx * create_tls_cli_ctx(struct tunnel_ctx *tunnel, struct server_config *config) {
    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)calloc(1, sizeof(*ctx));
    ctx->req = (struct uv_work_s *)calloc(1, sizeof(*ctx->req));
    ctx->req->data = ctx;
    ctx->async = (struct uv_async_s *)calloc(1, sizeof(*ctx->async));
    ctx->ssl_ctx = (mbedtls_ssl_context *)calloc(1, sizeof(mbedtls_ssl_context));
    ctx->tunnel = tunnel;
    ctx->config = config;

    tunnel->tls_ctx = ctx;
    tunnel->tunnel_tls_send_data = &tunnel_tls_send_data;

    return ctx;
}

void destroy_tls_cli_ctx(struct tls_cli_ctx *ctx) {
    if (ctx) {
        free(ctx->req);
        free(ctx->async);
        free(ctx->ssl_ctx);
        free(ctx);
    }
}

static void tls_cli_main_work_thread(uv_work_t* req) {
    /* this function is in work thread, NOT in event-loop thread */

    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)req->data;
    struct server_config *config = ctx->config;

    int ret = 0, len, proto;
    mbedtls_net_context connect_ctx;
    mbedtls_ssl_context *ssl_ctx = ctx->ssl_ctx;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt cacert;
    mbedtls_x509_crt clicert;
    mbedtls_pk_context pkey;
    char *alpn_list[ALPN_LIST_SIZE] = { NULL };
    const char *pers = get_app_name();
#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
    unsigned char psk[MBEDTLS_PSK_MAX_LEN];
    size_t psk_len = 0;
#endif
#if defined(MBEDTLS_TIMING_C)
    mbedtls_timing_delay_context timer = { 0 };
#endif
    unsigned char *buf = (unsigned char *)calloc(MAX_REQUEST_SIZE + 1, sizeof(*buf));
    int request_size = DFL_REQUEST_SIZE;
    int transport = DFL_TRANSPORT; /* TCP only, UDP not supported */
    char *port = NULL;

    mbedtls_net_init( &connect_ctx );
    mbedtls_ssl_init( ssl_ctx );
    mbedtls_ssl_config_init( &conf );
    mbedtls_ctr_drbg_init( &ctr_drbg );

    mbedtls_x509_crt_init( &cacert );
    mbedtls_x509_crt_init( &clicert );
    mbedtls_pk_init( &pkey );

    mbedtls_debug_set_threshold( 1 ); /* Error level */

    mbedtls_entropy_init( &entropy );
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func,
        &entropy, (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        mbedtls_printf(" failed\n  ! mbedtls_ctr_drbg_seed returned -0x%x\n", -ret);
        goto exit;
    }
    if (config->over_tls_root_cert_file && strlen(config->over_tls_root_cert_file)) {
        ret = mbedtls_x509_crt_parse_file(&cacert, config->over_tls_root_cert_file);
    }

    ret = mbedtls_x509_crt_parse( &clicert,
        (const unsigned char *) mbedtls_test_cli_crt,
        mbedtls_test_cli_crt_len );

    ret = mbedtls_pk_parse_key(&pkey,
        (const unsigned char *)mbedtls_test_cli_key,
        mbedtls_test_cli_key_len, NULL, 0 );


    port = ss_itoa(config->remote_port);
#if TLS_DUMP_INFO
    mbedtls_printf("  . Connecting to %s/%s/%s...",
        transport == MBEDTLS_SSL_TRANSPORT_STREAM ? "tcp" : "udp",
        config->remote_host, port);
    fflush( stdout );
#endif /* TLS_DUMP_INFO */

    proto = (transport == MBEDTLS_SSL_TRANSPORT_STREAM) ? MBEDTLS_NET_PROTO_TCP : MBEDTLS_NET_PROTO_UDP;
    ret = mbedtls_net_connect(&connect_ctx, config->remote_host, port, proto);
    if (ret != 0) {
        mbedtls_printf(" failed\n  ! mbedtls_net_connect returned -0x%x\n\n", -ret);
        goto exit;
    }

    if((ret = mbedtls_net_set_nonblock(&connect_ctx)) != 0) {
        mbedtls_printf(" failed\n  ! net_set_(non)block() returned -0x%x\n\n", -ret);
        goto exit;
    }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, transport, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        mbedtls_printf(" failed\n  ! mbedtls_ssl_config_defaults returned -0x%x\n\n", -ret);
        goto exit;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);

    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    // mbedtls_ssl_conf_dbg(&conf, my_debug, stdout);

    mbedtls_ssl_conf_read_timeout(&conf, 0 /* opt.read_timeout */);

#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    mbedtls_ssl_conf_session_tickets(&conf, 1 /* opt.tickets */);
#endif
#if defined(MBEDTLS_SSL_RENEGOTIATION)
    mbedtls_ssl_conf_renegotiation( &conf, 1 /* opt.renegotiation */);
#endif

    mbedtls_ssl_conf_ca_chain( &conf, &cacert, NULL );

    if ((ret = mbedtls_ssl_conf_own_cert(&conf, &clicert, &pkey)) != 0) {
        mbedtls_printf(" failed\n  ! mbedtls_ssl_conf_own_cert returned %d\n\n", ret);
        goto exit;
    }

#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
    ret = mbedtls_ssl_conf_psk(&conf, psk, psk_len, (const unsigned char *)DFL_PSK_IDENTITY, strlen(DFL_PSK_IDENTITY));
    if (ret != 0) {
        mbedtls_printf(" failed\n  ! mbedtls_ssl_conf_psk returned %d\n\n", ret);
        goto exit;
    }
#endif

    if (config->over_tls_root_cert_file && strlen(config->over_tls_root_cert_file)) {
        mbedtls_ssl_conf_ca_chain( &conf, &cacert, NULL );
    }

    if ((ret = mbedtls_ssl_setup(ssl_ctx, &conf)) != 0) {
        mbedtls_printf(" failed\n  ! mbedtls_ssl_setup returned -0x%x\n\n", -ret);
        goto exit;
    }

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    if (config->over_tls_server_domain && strlen(config->over_tls_server_domain)) {
        if ((ret = mbedtls_ssl_set_hostname(ssl_ctx, config->over_tls_server_domain)) != 0) {
            mbedtls_printf(" failed\n  ! mbedtls_ssl_set_hostname returned %d\n\n", ret);
            goto exit;
        }
    }
#endif

    mbedtls_ssl_set_bio(ssl_ctx, &connect_ctx, mbedtls_net_send, mbedtls_net_recv, NULL);

#if defined(MBEDTLS_TIMING_C)
    mbedtls_ssl_set_timer_cb(ssl_ctx, &timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
#endif

    while ((ret = mbedtls_ssl_handshake(ssl_ctx)) != 0) {
        if( ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE &&
            ret != MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS )
        {
            mbedtls_printf(" failed\n  ! mbedtls_ssl_handshake returned -0x%x\n", -ret);
            if( ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED )
                mbedtls_printf(
                "    Unable to verify the server's certificate. "
                "Either it is invalid,\n"
                "    or you didn't set ca_file or ca_path "
                "to an appropriate value.\n"
                "    Alternatively, you may want to use "
                "auth_mode=optional for testing purposes.\n" );
            mbedtls_printf( "\n" );
            goto exit;
        }

#if defined(MBEDTLS_ECP_RESTARTABLE)
        if( ret == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS ) {
            continue;
        }
#endif
    }

#if TLS_DUMP_INFO
    mbedtls_printf(" ok\n    [ Protocol is %s ]\n    [ Ciphersuite is %s ]\n",
        mbedtls_ssl_get_version(ssl_ctx),
        mbedtls_ssl_get_ciphersuite(ssl_ctx));

    if ((ret = mbedtls_ssl_get_record_expansion(ssl_ctx)) >= 0) {
        mbedtls_printf("    [ Record expansion is %d ]\n", ret );
    } else {
        mbedtls_printf("    [ Record expansion is unknown (compression) ]\n");
    }
#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
    mbedtls_printf("    [ Maximum fragment length is %u ]\n",
        (unsigned int)mbedtls_ssl_get_max_frag_len(ssl_ctx));
#endif
#endif /* TLS_DUMP_INFO */

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    /* 5. Verify the server certificate */
#if TLS_DUMP_INFO
    mbedtls_printf("  . Verifying peer X.509 certificate...");

    if ((flags = mbedtls_ssl_get_verify_result(ssl_ctx)) != 0) {
        char vrfy_buf[512] = { 0 };
        mbedtls_printf(" failed\n");
        mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", flags);
        mbedtls_printf("%s\n", vrfy_buf );
    } else {
        mbedtls_printf(" ok\n");
    }
    if (mbedtls_ssl_get_peer_cert(ssl_ctx) != NULL) {
        mbedtls_printf( "  . Peer certificate information    ...\n" );
        mbedtls_x509_crt_info( (char *) buf, MAX_REQUEST_SIZE, "      ",
                       mbedtls_ssl_get_peer_cert( ssl_ctx ) );
        mbedtls_printf( "%s\n", buf );
    }
#endif /* TLS_DUMP_INFO */
#endif /* MBEDTLS_X509_CRT_PARSE_C */

    /* 6. Write the GET request */
#if 0
    if (tls_cli_send_data(ssl_ctx, config->over_tls_path, config->over_tls_server_domain, config->remote_port, buf, 0) == false) {
        goto exit;
    }
#else
    tls_cli_state_changed_async_send(ctx, tls_state_connected, NULL, 0);
#endif

    /* 7. Read the HTTP response */
#if TLS_DUMP_INFO
    mbedtls_printf("  < Read from server:");
    fflush( stdout );
#endif /* TLS_DUMP_INFO */

    /* TLS and DTLS need different reading styles (stream vs datagram) */
    if (transport == MBEDTLS_SSL_TRANSPORT_STREAM) {
        do {
            len = MAX_REQUEST_SIZE;
            memset(buf, 0, MAX_REQUEST_SIZE+1);
            ret = mbedtls_ssl_read(ssl_ctx, buf, len);

#if defined(MBEDTLS_ECP_RESTARTABLE)
            if (ret == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS) {
                continue;
            }
#endif
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }

            if (ret <= 0) {
                switch (ret){
                case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
#if TLS_DUMP_INFO
                    mbedtls_printf(" connection was closed gracefully\n");
#endif /* TLS_DUMP_INFO */
                    ret = 0;
                    goto close_notify;
                case 0:
                case MBEDTLS_ERR_NET_CONN_RESET:
#if TLS_DUMP_INFO
                    mbedtls_printf(" connection was reset by peer\n");
#endif /* TLS_DUMP_INFO */
                    ret = 0;
                    goto reconnect;
                default:
#if TLS_DUMP_INFO
                    mbedtls_printf(" mbedtls_ssl_read returned -0x%x\n", -ret);
#endif /* TLS_DUMP_INFO */
                    goto exit;
                }
            }

            len = ret;
            buf[len] = '\0';
#if 0
            mbedtls_printf(" %d bytes read\n\n%s", len, (char *)buf);
#else
            tls_cli_state_changed_async_send(ctx, tls_state_data_coming, buf, len);
#endif
            /* End of message should be detected according to the syntax of the
             * application protocol (eg HTTP), just use a dummy test here. */
            if (ret > 0 && buf[len-1] == '\n') {
                ret = 0;
                break;
            }
        } while(1);
    }
    else {
        /* Not stream, so datagram, omitted by us */
    }

    /* 8. Done, cleanly close the connection */
close_notify:
#if TLS_DUMP_INFO
    mbedtls_printf("  . Closing the connection...");
    fflush(stdout);
#endif /* TLS_DUMP_INFO */

    /* No error checking, the connection might be closed already */
    do {
        ret = mbedtls_ssl_close_notify(ssl_ctx);
    } while(ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    ret = 0;

#if TLS_DUMP_INFO
    mbedtls_printf( " done\n" );
#endif /* TLS_DUMP_INFO */

    /* 9. Reconnect? */
reconnect: ;

exit:
#ifdef MBEDTLS_ERROR_C
    if (ret != 0) {
        char error_buf[100] = { 0 };
        mbedtls_strerror(ret, error_buf, 100);
        mbedtls_printf("Last error was: -0x%X - %s\n\n", -ret, error_buf);
    }
#endif

    tls_cli_state_changed_async_send(ctx, tls_state_shutting_down, NULL, 0);

    mbedtls_net_free( &connect_ctx );

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    mbedtls_x509_crt_free( &clicert );
    mbedtls_x509_crt_free( &cacert );
    mbedtls_pk_free( &pkey );
#endif
    mbedtls_ssl_free( ssl_ctx );
    mbedtls_ssl_config_free( &conf );
    mbedtls_ctr_drbg_free( &ctr_drbg );
    mbedtls_entropy_free( &entropy );

    free(buf);

#if 0 //defined(_WIN32)
    mbedtls_printf("  + Press Enter to exit this program.\n");
    fflush(stdout); getchar();
#endif

    /* Shell can not handle large exit numbers -> 1 for errors */
    if (ret < 0) {
        ret = 1;
    }
    // return ret;
}

static void tunnel_tls_send_data(struct tunnel_ctx *tunnel, struct buffer_t *data) {
    struct tls_cli_ctx *ctx = tunnel->tls_ctx;
    mbedtls_ssl_context *ssl_ctx = ctx->ssl_ctx;
    struct server_config *config = ctx->config;
    const char *url_path = config->over_tls_path;
    const char *domain = config->over_tls_server_domain;
    unsigned short domain_port = config->remote_port;

    tls_cli_send_data(ssl_ctx, url_path, domain, domain_port, data->buffer, data->len);
}

static bool tls_cli_send_data(mbedtls_ssl_context *ssl_ctx,
    const char *url_path,
    const char *domain,
    unsigned short domain_port,
    const uint8_t *data, size_t size)
{
    int len, written, frags, ret;
    uint8_t *buf = (uint8_t *)calloc(MAX_REQUEST_SIZE + 1, sizeof(*buf));
    bool result = false;

    len = mbedtls_snprintf((char *)buf, MAX_REQUEST_SIZE, GET_REQUEST_FORMAT,
        url_path, domain, domain_port, (int)size);

    if (data && size) {
        memcpy(buf + len, data, size);
        len += (int)size;
    }

    {
        written = 0;
        frags = 0;

        do {
            while ((ret = mbedtls_ssl_write(ssl_ctx, buf + written, len - written)) < 0) {
                if( ret != MBEDTLS_ERR_SSL_WANT_READ &&
                    ret != MBEDTLS_ERR_SSL_WANT_WRITE &&
                    ret != MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS )
                {
                    mbedtls_printf(" failed\n  ! mbedtls_ssl_write returned -0x%x\n\n", -ret);
                    goto exit;
                }
            }
            frags++;
            written += ret;
        } while (written < len);
        result = true;
    }

#if TLS_DUMP_INFO
    buf[written] = '\0';
    mbedtls_printf(" %d bytes written in %d fragments\n\n%s\n", written, frags, (char *)buf);
#endif /* TLS_DUMP_INFO */
exit:
    free(buf);
    return result;
}

static void tls_cli_state_changed_notice_cb(uv_async_t *handle) {
    /* this point is in event-loop thread */
    struct tls_cli_state_ctx *data_arrival = (struct tls_cli_state_ctx *)handle->data;
    struct buffer_t *data = data_arrival->data;
    struct tls_cli_ctx *ctx = data_arrival->ctx;
    enum tls_cli_state state = data_arrival->state;
    struct tunnel_ctx *tunnel = ctx->tunnel;

    free(data_arrival);
    handle->data = NULL;

    switch (state) {
    case tls_state_connected:
        if (tunnel->tunnel_tls_on_connection_established) {
            tunnel->tunnel_tls_on_connection_established(tunnel);
        }
        break;
    case tls_state_data_coming:
        if (tunnel->tunnel_tls_on_data_coming) {
            tunnel->tunnel_tls_on_data_coming(tunnel, data);
        }
        break;
    case tls_state_shutting_down:
        if (tunnel->tunnel_tls_on_shutting_down) {
            tunnel->tunnel_tls_on_shutting_down(tunnel);
        }
        break;
    default:
        assert(false);
        break;
    }
    buffer_release(data);
}

static void tls_async_close_cb(uv_handle_t *handle) {
    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)handle->data;
    destroy_tls_cli_ctx(ctx);
    PRINT_INFO("outgoing connection closed.");
}

static void tls_cli_after_cb(uv_work_t *req, int status) {
    struct tls_cli_ctx *ctx = (struct tls_cli_ctx *)req->data;
    assert(ctx->async->data == NULL);
    ctx->async->data = ctx;
    uv_close((uv_handle_t*) ctx->async, tls_async_close_cb);
}

static void tls_cli_state_changed_async_send(struct tls_cli_ctx *ctx,
    enum tls_cli_state state, const uint8_t *buf, size_t len)
{
    struct tls_cli_state_ctx *ptr = (struct tls_cli_state_ctx *)calloc(1, sizeof(*ptr));
    ptr->ctx = ctx;
    ptr->state = state;
    if (buf && len) {
        int minor_version;
        int status;
        const char *msg;
        size_t msg_len;
        struct phr_header headers[6] = { { NULL } };
        size_t num_headers;
        int n;

        num_headers = sizeof(headers) / sizeof(headers[0]);
        n = phr_parse_response(buf, len, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

        ptr->data = buffer_create_from((uint8_t *)(buf + n), (size_t)(len - n));
    }
    assert(ctx->async->data == NULL);
    ctx->async->data = (void*) ptr;
    uv_async_send(ctx->async);
}
#endif
