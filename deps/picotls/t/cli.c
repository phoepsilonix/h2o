/*
 * Copyright (c) 2016 DeNA Co., Ltd., Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#define OPENSSL_API_COMPAT 0x00908000L
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/engine.h>
#include <openssl/pem.h>
#if PICOTLS_USE_BROTLI
#include "brotli/decode.h"
#endif
#include "picotls.h"
#include "picotls/openssl.h"
#if PICOTLS_USE_BROTLI
#include "picotls/certificate_compression.h"
#endif
#include "util.h"

/* sentinels indicating that the endpoint is in benchmark mode */
static const char input_file_is_benchmark[] = "is:benchmark";

static void shift_buffer(ptls_buffer_t *buf, size_t delta)
{
    if (delta != 0) {
        assert(delta <= buf->off);
        if (delta != buf->off)
            memmove(buf->base, buf->base + delta, buf->off - delta);
        buf->off -= delta;
    }
}

static void setup_ptlslog(const char *fn)
{
    int fd;
    if ((fd = open(fn, O_WRONLY | O_CREAT | O_APPEND, 0666)) == -1) {
        fprintf(stderr, "failed to open file:%s:%s\n", fn, strerror(errno));
        exit(1);
    }
    ptls_log_add_fd(fd, 1., NULL, NULL, NULL, 1);
    ptls_log.may_include_appdata = 1;
}

static int handle_connection(int sockfd, ptls_context_t *ctx, const char *server_name, const char *input_file,
                             ptls_handshake_properties_t *hsprop, int request_key_update, int keep_sender_open)
{
    static const int inputfd_is_benchmark = -2;

    ptls_t *tls = ptls_new(ctx, server_name == NULL);
    ptls_buffer_t rbuf, encbuf, ptbuf;
    enum { IN_HANDSHAKE, IN_1RTT, IN_SHUTDOWN } state = IN_HANDSHAKE;
    int inputfd = 0, ret = 0;
    size_t early_bytes_sent = 0;
    uint64_t data_received = 0;
    ssize_t ioret;

    uint64_t start_at = ctx->get_time->cb(ctx->get_time);

    ptls_buffer_init(&rbuf, "", 0);
    ptls_buffer_init(&encbuf, "", 0);
    ptls_buffer_init(&ptbuf, "", 0);

    if (fcntl(sockfd, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        goto Exit;
    }

    if (input_file == input_file_is_benchmark) {
        if (!ptls_is_server(tls))
            inputfd = inputfd_is_benchmark;
    } else if (input_file != NULL) {
        if ((inputfd = open(input_file, O_RDONLY)) == -1) {
            fprintf(stderr, "failed to open file:%s:%s\n", input_file, strerror(errno));
            ret = 1;
            goto Exit;
        }
    }

    if (server_name != NULL) {
        ptls_set_server_name(tls, server_name, 0);
        if ((ret = ptls_handshake(tls, &encbuf, NULL, NULL, hsprop)) != PTLS_ERROR_IN_PROGRESS) {
            fprintf(stderr, "ptls_handshake:%d\n", ret);
            ret = 1;
            goto Exit;
        }
    }

    while (1) {
        /* check if data is available */
        fd_set readfds, writefds, exceptfds;
        int maxfd = 0;
        struct timeval timeout;
        do {
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            FD_ZERO(&exceptfds);
            FD_SET(sockfd, &readfds);
            if (encbuf.off != 0 || inputfd == inputfd_is_benchmark)
                FD_SET(sockfd, &writefds);
            FD_SET(sockfd, &exceptfds);
            maxfd = sockfd + 1;
            if (inputfd >= 0) {
                FD_SET(inputfd, &readfds);
                FD_SET(inputfd, &exceptfds);
                if (maxfd <= inputfd)
                    maxfd = inputfd + 1;
            }
            timeout.tv_sec = encbuf.off != 0 ? 0 : 3600;
            timeout.tv_usec = 0;
        } while (select(maxfd, &readfds, &writefds, &exceptfds, &timeout) == -1);

        /* consume incoming messages */
        if (FD_ISSET(sockfd, &readfds) || FD_ISSET(sockfd, &exceptfds)) {
            char bytebuf[16384];
            size_t off = 0, leftlen;
            while ((ioret = read(sockfd, bytebuf, sizeof(bytebuf))) == -1 && errno == EINTR)
                ;
            if (ioret == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                /* no data */
                ioret = 0;
            } else if (ioret <= 0) {
                goto Exit;
            }
            while ((leftlen = ioret - off) != 0) {
                if (state == IN_HANDSHAKE) {
                    if ((ret = ptls_handshake(tls, &encbuf, bytebuf + off, &leftlen, hsprop)) == 0) {
                        state = IN_1RTT;
                        assert(ptls_is_server(tls) || hsprop->client.early_data_acceptance != PTLS_EARLY_DATA_ACCEPTANCE_UNKNOWN);
                        ech_save_retry_configs();
                        /* release data sent as early-data, if server accepted it */
                        if (hsprop->client.early_data_acceptance == PTLS_EARLY_DATA_ACCEPTED)
                            shift_buffer(&ptbuf, early_bytes_sent);
                        if (request_key_update)
                            ptls_update_key(tls, 1);
                    } else if (ret == PTLS_ERROR_IN_PROGRESS) {
                        /* ok */
                    } else {
                        if (ret == PTLS_ALERT_ECH_REQUIRED) {
                            assert(!ptls_is_server(tls));
                            ech_save_retry_configs();
                        }
                        if (encbuf.off != 0)
                            repeat_while_eintr(write(sockfd, encbuf.base, encbuf.off), { break; });
                        fprintf(stderr, "ptls_handshake:%d\n", ret);
                        goto Exit;
                    }
                } else {
                    if ((ret = ptls_receive(tls, &rbuf, bytebuf + off, &leftlen)) == 0) {
                        if (rbuf.off != 0) {
                            data_received += rbuf.off;
                            if (input_file != input_file_is_benchmark)
                                repeat_while_eintr(write(1, rbuf.base, rbuf.off), { goto Exit; });
                            rbuf.off = 0;
                        }
                    } else if (ret == PTLS_ERROR_IN_PROGRESS) {
                        /* ok */
                    } else {
                        fprintf(stderr, "ptls_receive:%d\n", ret);
                        goto Exit;
                    }
                }
                off += leftlen;
            }
        }

        /* encrypt data to send, if any is available */
        if (encbuf.off == 0 || state == IN_HANDSHAKE) {
            static const size_t block_size = 16384;
            if (inputfd >= 0 && (FD_ISSET(inputfd, &readfds) || FD_ISSET(inputfd, &exceptfds))) {
                if ((ret = ptls_buffer_reserve(&ptbuf, block_size)) != 0)
                    goto Exit;
                while ((ioret = read(inputfd, ptbuf.base + ptbuf.off, block_size)) == -1 && errno == EINTR)
                    ;
                if (ioret > 0) {
                    ptbuf.off += ioret;
                } else if (ioret == 0) {
                    /* closed */
                    if (input_file != NULL)
                        close(inputfd);
                    inputfd = -1;
                }
            } else if (inputfd == inputfd_is_benchmark) {
                if (ptbuf.capacity < block_size) {
                    if ((ret = ptls_buffer_reserve(&ptbuf, block_size - ptbuf.capacity)) != 0)
                        goto Exit;
                    memset(ptbuf.base + ptbuf.capacity, 0, block_size - ptbuf.capacity);
                }
                ptbuf.off = block_size;
            }
        }
        if (ptbuf.off != 0) {
            if (state == IN_HANDSHAKE) {
                size_t send_amount = 0;
                if (server_name != NULL && hsprop->client.max_early_data_size != NULL) {
                    size_t max_can_be_sent = *hsprop->client.max_early_data_size;
                    if (max_can_be_sent > ptbuf.off)
                        max_can_be_sent = ptbuf.off;
                    send_amount = max_can_be_sent - early_bytes_sent;
                }
                if (send_amount != 0) {
                    if ((ret = ptls_send(tls, &encbuf, ptbuf.base, send_amount)) != 0) {
                        fprintf(stderr, "ptls_send(early_data):%d\n", ret);
                        goto Exit;
                    }
                    early_bytes_sent += send_amount;
                }
            } else {
                if ((ret = ptls_send(tls, &encbuf, ptbuf.base, ptbuf.off)) != 0) {
                    fprintf(stderr, "ptls_send(1rtt):%d\n", ret);
                    goto Exit;
                }
                ptbuf.off = 0;
            }
        }

        /* send any data */
        if (encbuf.off != 0) {
            while ((ioret = write(sockfd, encbuf.base, encbuf.off)) == -1 && errno == EINTR)
                ;
            if (ioret == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                /* no data */
            } else if (ioret <= 0) {
                goto Exit;
            } else {
                shift_buffer(&encbuf, ioret);
            }
        }

        /* close the sender side when necessary */
        if (state == IN_1RTT && inputfd == -1) {
            if (!keep_sender_open) {
                ptls_buffer_t wbuf;
                uint8_t wbuf_small[32];
                ptls_buffer_init(&wbuf, wbuf_small, sizeof(wbuf_small));
                if ((ret = ptls_send_alert(tls, &wbuf, PTLS_ALERT_LEVEL_WARNING, PTLS_ALERT_CLOSE_NOTIFY)) != 0) {
                    fprintf(stderr, "ptls_send_alert:%d\n", ret);
                }
                if (wbuf.off != 0)
                    repeat_while_eintr(write(sockfd, wbuf.base, wbuf.off), {
                        ptls_buffer_dispose(&wbuf);
                        goto Exit;
                    });
                ptls_buffer_dispose(&wbuf);
                shutdown(sockfd, SHUT_WR);
            }
            state = IN_SHUTDOWN;
        }
    }

Exit:
    if (input_file == input_file_is_benchmark) {
        double elapsed = (ctx->get_time->cb(ctx->get_time) - start_at) / 1000.0;
        ptls_cipher_suite_t *cipher_suite = ptls_get_cipher(tls);
        fprintf(stderr, "received %" PRIu64 " bytes in %.3f seconds (%f.3Mbps); %s\n", data_received, elapsed,
                data_received * 8 / elapsed / 1000 / 1000, cipher_suite != NULL ? cipher_suite->aead->name : "unknown cipher");
    }

    if (sockfd != -1)
        close(sockfd);
    if (input_file != NULL && input_file != input_file_is_benchmark && inputfd >= 0)
        close(inputfd);
    ptls_buffer_dispose(&rbuf);
    ptls_buffer_dispose(&encbuf);
    ptls_buffer_dispose(&ptbuf);
    ptls_free(tls);

    return ret != 0;
}

static int run_server(struct sockaddr *sa, socklen_t salen, ptls_context_t *ctx, const char *input_file,
                      ptls_handshake_properties_t *hsprop, int request_key_update)
{
    int listen_fd, conn_fd, on = 1;

    if ((listen_fd = socket(sa->sa_family, SOCK_STREAM, 0)) == -1) {
        perror("socket(2) failed");
        return 1;
    }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        return 1;
    }
    if (bind(listen_fd, sa, salen) != 0) {
        perror("bind(2) failed");
        return 1;
    }
    if (listen(listen_fd, SOMAXCONN) != 0) {
        perror("listen(2) failed");
        return 1;
    }

    fprintf(stderr, "server started on port %d\n", ntohs(((struct sockaddr_in *)sa)->sin_port));
    while (1) {
        fprintf(stderr, "waiting for connections\n");
        if ((conn_fd = accept(listen_fd, NULL, 0)) != -1)
            handle_connection(conn_fd, ctx, NULL, input_file, hsprop, request_key_update, 0);
    }

    return 0;
}

static int run_client(struct sockaddr *sa, socklen_t salen, ptls_context_t *ctx, const char *server_name, const char *input_file,
                      ptls_handshake_properties_t *hsprop, int request_key_update, int keep_sender_open)
{
    int fd;

    if ((fd = socket(sa->sa_family, SOCK_STREAM, 0)) == 1) {
        perror("socket(2) failed");
        return 1;
    }
    if (connect(fd, sa, salen) != 0) {
        perror("connect(2) failed");
        return 1;
    }

    int ret = handle_connection(fd, ctx, server_name, input_file, hsprop, request_key_update, keep_sender_open);
    return ret;
}

static void usage(const char *cmd)
{
    printf("Usage: %s [options] host port\n"
           "\n"
           "Options:\n"
           "  -4                   force IPv4\n"
           "  -6                   force IPv6\n"
           "  -a                   require client authentication\n"
           "  -b                   enable brotli compression\n"
           "  -B                   benchmark mode for measuring sustained bandwidth. Run\n"
           "                       both endpoints with this option for some time, then kill\n"
           "                       the client. Server will report the ingress bandwidth.\n"
           "  -C certificate-file  certificate chain used for client authentication\n"
           "  -c certificate-file  certificate chain used for server authentication\n"
           "  -i file              a file to read from and send to the peer (default: stdin)\n"
           "  -I                   keep send side open after sending all data (client-only)\n"
           "  -j log-file          file to log probe events in JSON-Lines\n"
           "  -k key-file          specifies the credentials for signing the certificate\n"
           "  -K key-file          ECH private key for each ECH config provided by -E\n"
           "  -l log-file          file to log events (incl. traffic secrets)\n"
           "  -n                   negotiates the key exchange method (i.e. wait for HRR)\n"
           "  -N named-group       named group to be used (default: secp256r1); if \"null\"\n"
           "                       is specified alongside `-p`, external PSK handshake with\n"
           "                       no ECDHE is performed\n"
           "  -s session-file      file to read/write the session ticket\n"
           "  -S                   require public key exchange when resuming a session\n"
           "  -E echconfiglist     file that contains ECHConfigList or an empty file to\n"
           "                       grease ECH; will be overwritten when receiving\n"
           "                       retry_configs from the server\n"
           "  -e                   when resuming a session, send first 8,192 bytes of input\n"
           "                       as early data\n"
           "  -r public-key-file   use raw public keys (RFC 7250). When set and running as a\n"
           "                       client, the argument specifies the public keys that the\n"
           "                       server is expected to use. When running as a server, the\n"
           "                       argument is ignored.\n"
           "  -p psk-identity      name of the PSK key; if set, -c and -C specify the\n"
           "                       pre-shared secret\n"
           "  -P psk-hash          hash function associated to the PSK (default: sha256)\n"
           "  -T new_session_count,resumption_count\n"
           "                       set number of session tickets to request\n"
           "  -u                   update the traffic key when handshake is complete\n"
           "  -v                   verify peer using the default certificates\n"
           "  -V CA-root-file      verify peer using the CA Root File\n"
           "  -y cipher-suite      cipher-suite to be used\n"
           "  -h                   print this help\n"
           "\n"
           "Supported named groups: " PTLS_GROUP_NAME_SECP256R1
#if PTLS_OPENSSL_HAVE_SECP384R1
           ", " PTLS_GROUP_NAME_SECP384R1
#endif
#if PTLS_OPENSSL_HAVE_SECP521R1
           ", " PTLS_GROUP_NAME_SECP521R1
#endif
#if PTLS_OPENSSL_HAVE_X25519
           ", " PTLS_GROUP_NAME_X25519
#endif
#if PTLS_OPENSSL_HAVE_X25519MLKEM768
           ", " PTLS_GROUP_NAME_X25519MLKEM768
#endif
#if PTLS_OPENSSL_HAVE_MLKEM
           ", " PTLS_GROUP_NAME_SECP256R1MLKEM768 ", " PTLS_GROUP_NAME_SECP384R1MLKEM1024 ", " PTLS_GROUP_NAME_MLKEM512 ", "
           PTLS_GROUP_NAME_MLKEM768 ", " PTLS_GROUP_NAME_MLKEM1024
#endif
           "\n"
           "Supported signature algorithms: rsa, secp256r1"
#if PTLS_OPENSSL_HAVE_SECP384R1
           ", secp384r1"
#endif
#if PTLS_OPENSSL_HAVE_SECP521R1
           ", secp521r1"
#endif
#if PTLS_OPENSSL_HAVE_ED25519
           ", ed25519"
#endif
           "\n",
           cmd);
    printf("Supported cipher suites:");
    for (size_t i = 0; ptls_openssl_cipher_suites_all[i] != NULL; ++i) {
        if (i != 0)
            printf(",");
        printf(" %s", ptls_openssl_cipher_suites_all[i]->name);
    }
    printf("\n\n");
}

int main(int argc, char **argv)
{
    ERR_load_crypto_strings();
    OpenSSL_add_all_algorithms();
#if !defined(OPENSSL_NO_ENGINE)
    /* Load all compiled-in ENGINEs */
    ENGINE_load_builtin_engines();
    ENGINE_register_all_ciphers();
    ENGINE_register_all_digests();
#endif

    res_init();

    ptls_key_exchange_algorithm_t *key_exchanges[128] = {NULL};
    ptls_cipher_suite_t *cipher_suites[128] = {NULL};
    ptls_context_t ctx = {
        .random_bytes = ptls_openssl_random_bytes,
        .get_time = &ptls_get_time,
        .key_exchanges = key_exchanges,
        .cipher_suites = cipher_suites,
        .ech = {.client = {ptls_openssl_hpke_cipher_suites, ptls_openssl_hpke_kems}, .server = {NULL /* activated by -K option */}},
    };
    ptls_handshake_properties_t hsprop = {{{{NULL}}}};
    const char *host, *port, *input_file = NULL, *psk_hash = "sha256";
    int is_server = 0, use_early_data = 0, request_key_update = 0, keep_sender_open = 0, ch;
    struct sockaddr_storage sa;
    socklen_t salen;
    int family = 0;
    const char *raw_pub_key_file = NULL, *cert_location = NULL;

    while ((ch = getopt(argc, argv, "46abBC:c:i:Ij:k:nN:es:Sr:p:P:E:K:l:T:uy:vV:h")) != -1) {
        switch (ch) {
        case '4':
            family = AF_INET;
            break;
        case '6':
            family = AF_INET6;
            break;
        case 'a':
            ctx.require_client_authentication = 1;
            break;
        case 'b':
#if PICOTLS_USE_BROTLI
            ctx.decompress_certificate = &ptls_decompress_certificate;
#else
            fprintf(stderr, "support for `-b` option was turned off during configuration\n");
            exit(1);
#endif
            break;
        case 'B':
            input_file = input_file_is_benchmark;
            break;
        case 'C':
        case 'c':
            if (cert_location != NULL) {
                fprintf(stderr, "-C/-c can only be specified once\n");
                return 1;
            }
            cert_location = optarg;
            is_server = ch == 'c';
            break;
        case 'i':
            input_file = optarg;
            break;
        case 'I':
            keep_sender_open = 1;
            break;
        case 'j':
            setup_ptlslog(optarg);
            break;
        case 'k':
            load_private_key(&ctx, optarg);
            break;
        case 'n':
            hsprop.client.negotiate_before_key_exchange = 1;
            break;
        case 'e':
            use_early_data = 1;
            break;
        case 'r':
            raw_pub_key_file = optarg;
            break;
        case 'p':
            ctx.pre_shared_key.identity = ptls_iovec_init(optarg, strlen(optarg));
            break;
        case 'P':
            psk_hash = optarg;
            break;
        case 's':
            setup_session_file(&ctx, &hsprop, optarg);
            break;
        case 'S':
            ctx.require_dhe_on_psk = 1;
            break;
        case 'E':
            ech_setup_configs(optarg);
            break;
        case 'K':
            ech_setup_key(&ctx, optarg);
            break;
        case 'l':
            setup_log_event(&ctx, optarg);
            break;
        case 'v':
            setup_verify_certificate(&ctx, NULL);
            break;
        case 'V':
            setup_verify_certificate(&ctx, optarg);
            break;
        case 'N':
            if (strcasecmp(optarg, "null") == 0) {
                /* disable use of key exchanges entirely */
                ctx.key_exchanges = NULL;
            } else {
                ptls_key_exchange_algorithm_t **named;
                for (named = ptls_openssl_key_exchanges_all; *named != NULL; ++named)
                    if (strcasecmp((*named)->name, optarg) == 0)
                        break;
                if (*named == NULL) {
                    fprintf(stderr, "could not find key exchange: %s\n", optarg);
                    return 1;
                }
                size_t i;
                for (i = 0; key_exchanges[i] != NULL; ++i)
                    ;
                key_exchanges[i++] = *named;
            }
            break;
        case 'u':
            request_key_update = 1;
            break;
        case 'y': {
            /* find the cipher suite to be added from `ptls_openssl_cipher_suites_all` */
            ptls_cipher_suite_t *added = NULL;
            for (size_t i = 0; ptls_openssl_cipher_suites_all[i] != NULL; ++i) {
                if (strcasecmp(ptls_openssl_cipher_suites_all[i]->name, optarg) == 0) {
                    added = ptls_openssl_cipher_suites_all[i];
                    break;
                }
            }
            if (added == NULL) {
                fprintf(stderr, "unknown cipher-suite: %s, see -h for list of cipher-suites supported\n", optarg);
                exit(1);
            }

            size_t slot;
            for (slot = 0; cipher_suites[slot] != NULL; ++slot) {
                if (cipher_suites[slot]->id == added->id) {
                    fprintf(stderr, "cipher-suite %s is already in list\n", added->name);
                    exit(1);
                }
            }
            cipher_suites[slot] = added;
        } break;
        case 'T':
            if (sscanf(optarg, "%" SCNu8 ",%" SCNu8, &ctx.ticket_requests.client.new_session_count,
                       &ctx.ticket_requests.client.resumption_count) != 2) {
                fprintf(stderr, "invalid argument passed to -T, should be in the form of <new_session_count>,<resumption_count>\n");
                exit(1);
            }
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    if (raw_pub_key_file != NULL) {
        int is_dash = !strcmp(raw_pub_key_file, "-");
        if (is_server) {
            ctx.certificates.list = malloc(sizeof(*ctx.certificates.list));
            load_raw_public_key(ctx.certificates.list, cert_location);
            ctx.certificates.count = 1;
        } else if (!is_dash) {
            ptls_iovec_t raw_pub_key;
            EVP_PKEY *pubkey;
            load_raw_public_key(&raw_pub_key, raw_pub_key_file);
            pubkey = d2i_PUBKEY(NULL, (const unsigned char **)&raw_pub_key.base, raw_pub_key.len);
            if (pubkey == NULL) {
                fprintf(stderr, "Failed to create an EVP_PKEY from the key found in %s\n", raw_pub_key_file);
                return 1;
            }
            setup_raw_pubkey_verify_certificate(&ctx, pubkey);
            EVP_PKEY_free(pubkey);
        }
        ctx.use_raw_public_keys = 1;
    } else if (ctx.pre_shared_key.identity.base != NULL) {
        if (cert_location == NULL) {
            fprintf(stderr, "-p must be used with -C or -c\n");
            return 1;
        }
        ctx.pre_shared_key.secret = load_file(cert_location);
    } else {
        if (cert_location != NULL)
            load_certificate_chain(&ctx, cert_location);
    }

    if ((ctx.certificates.count == 0) != (ctx.sign_certificate == NULL)) {
        fprintf(stderr, "-C/-c and -k options must be used together\n");
        return 1;
    }

    if (is_server) {
#if PICOTLS_USE_BROTLI
        if (ctx.certificates.count != 0 && ctx.decompress_certificate != NULL) {
            static ptls_emit_compressed_certificate_t ecc;
            if (ptls_init_compressed_certificate(&ecc, ctx.certificates.list, ctx.certificates.count, ptls_iovec_init(NULL, 0)) !=
                0) {
                fprintf(stderr, "failed to create a brotli-compressed version of the certificate chain.\n");
                exit(1);
            }
            ctx.emit_certificate = &ecc.super;
        }
#endif
        setup_session_cache(&ctx);
    } else {
        /* client */
        if (use_early_data) {
            static size_t max_early_data_size;
            hsprop.client.max_early_data_size = &max_early_data_size;
        }
        ctx.send_change_cipher_spec = 1;
        hsprop.client.ech.configs = ech.config_list;
        hsprop.client.ech.retry_configs = &ech.retry.configs;
    }
    if (key_exchanges[0] == NULL)
        key_exchanges[0] = &ptls_openssl_secp256r1;
    if (cipher_suites[0] == NULL) {
        for (size_t i = 0; ptls_openssl_cipher_suites[i] != NULL; ++i)
            cipher_suites[i] = ptls_openssl_cipher_suites[i];
    }
    if (ctx.pre_shared_key.identity.base != NULL) {
        size_t i;
        for (i = 0; cipher_suites[i] != NULL; ++i)
            if (strcmp(cipher_suites[i]->hash->name, psk_hash) == 0)
                break;
        if (cipher_suites[i] == NULL) {
            fprintf(stderr, "no compatible cipher-suite for psk hash: %s\n", psk_hash);
            exit(1);
        }
        ctx.pre_shared_key.hash = cipher_suites[i]->hash;
    }
    if (argc != 2) {
        fprintf(stderr, "missing host and port\n");
        return 1;
    }
    host = (--argc, *argv++);
    port = (--argc, *argv++);

    if (resolve_address((struct sockaddr *)&sa, &salen, host, port, family, SOCK_STREAM, IPPROTO_TCP) != 0)
        exit(1);

    if (is_server) {
        return run_server((struct sockaddr *)&sa, salen, &ctx, input_file, &hsprop, request_key_update);
    } else {
        return run_client((struct sockaddr *)&sa, salen, &ctx, host, input_file, &hsprop, request_key_update, keep_sender_open);
    }
}
