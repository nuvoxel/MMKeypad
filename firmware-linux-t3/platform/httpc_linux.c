/* Minimal esp_http_client over BSD sockets + mbedtls. Originally just enough
 * for art.c's album-art HTTPS GET; also used by the firmware updater
 * (fwupdate.c / nv_ota_t3.c), so it supports custom headers, a request body
 * (write), and the response status code.
 *
 * Response header/body state is per-client (album-art fetch and the updater can
 * run on different threads concurrently).
 *
 * Cert verification is intentionally OFF for now (a later refinement wires a CA
 * bundle); album art and update metadata are low-value to MITM on the LAN
 * and the license itself is signature-verified offline regardless. */
#include "esp_http_client.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* Embedded Mozilla CA bundle (platform/ca_bundle.c) — the trust anchors the T3
 * verifies TLS server certs against. */
extern const unsigned char nv_ca_bundle_pem[];
extern const unsigned int nv_ca_bundle_pem_len;

#define HDRS_CAP 1024
#define STASH_CAP 4096

struct esp_http_client {
    char host[256];
    char path[512];
    int https;
    int port;
    int fd;
    esp_http_client_method_t method;
    char hdrs[HDRS_CAP]; /* accumulated custom request headers ("K: V\r\n"...) */
    int hdrs_len;
    int status; /* parsed response status code */
    /* mbedtls state (https only) */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_net_context net;
    mbedtls_x509_crt cacert;
    int tls_up;
    /* body bytes read past the header boundary during fetch_headers */
    char stash[STASH_CAP];
    int stash_len, stash_off;
};

static int parse_url(const char *url, esp_http_client_handle_t c) {
    if (strncmp(url, "https://", 8) == 0) {
        c->https = 1; c->port = 443; url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        c->https = 0; c->port = 80; url += 7;
    } else {
        return -1;
    }
    const char *slash = strchr(url, '/');
    size_t hlen = slash ? (size_t)(slash - url) : strlen(url);
    if (hlen >= sizeof(c->host)) hlen = sizeof(c->host) - 1;
    memcpy(c->host, url, hlen);
    c->host[hlen] = 0;
    snprintf(c->path, sizeof(c->path), "%s", slash ? slash : "/");
    return 0;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    if (!cfg || !cfg->url) return NULL;
    esp_http_client_handle_t c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = -1;
    c->method = cfg->method;
    if (parse_url(cfg->url, c) != 0) { free(c); return NULL; }
    return c;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key,
                                     const char *value) {
    if (!c || !key || !value) return ESP_FAIL;
    int n = snprintf(c->hdrs + c->hdrs_len, HDRS_CAP - c->hdrs_len, "%s: %s\r\n",
                     key, value);
    if (n < 0 || n >= HDRS_CAP - c->hdrs_len) return ESP_FAIL; /* overflow */
    c->hdrs_len += n;
    return ESP_OK;
}

static int ssl_send(void *ctx, const unsigned char *b, size_t l) {
    return (int)send(*(int *)ctx, b, l, 0);
}
static int ssl_recv(void *ctx, unsigned char *b, size_t l) {
    return (int)recv(*(int *)ctx, b, l, 0);
}

static int sock_send_all(esp_http_client_handle_t c, const char *b, int n) {
    int off = 0;
    while (off < n) {
        int w;
        if (c->https)
            w = mbedtls_ssl_write(&c->ssl, (const unsigned char *)b + off, n - off);
        else
            w = (int)send(c->fd, b + off, n - off, 0);
        if (w <= 0) return -1;
        off += w;
    }
    return n;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len) {
    if (!c) return ESP_FAIL;

    /* TCP connect */
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", c->port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(c->host, portstr, &hints, &res) != 0 || !res) return ESP_FAIL;
    c->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c->fd < 0 || connect(c->fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res); return ESP_FAIL;
    }
    freeaddrinfo(res);

    if (c->https) {
        mbedtls_ssl_init(&c->ssl);
        mbedtls_ssl_config_init(&c->conf);
        mbedtls_ctr_drbg_init(&c->drbg);
        mbedtls_entropy_init(&c->entropy);
        const char *pers = "mmkeypad-http";
        mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)pers, strlen(pers));
        mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        /* Verify the server cert against the embedded CA bundle (len includes the
         * trailing NUL, as mbedtls PEM parsing requires). VERIFY_REQUIRED makes the
         * handshake fail on any chain/hostname error. */
        mbedtls_x509_crt_init(&c->cacert);
        if (mbedtls_x509_crt_parse(&c->cacert, nv_ca_bundle_pem, nv_ca_bundle_pem_len) < 0)
            return ESP_FAIL;
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
        mbedtls_ssl_setup(&c->ssl, &c->conf);
        mbedtls_ssl_set_hostname(&c->ssl, c->host);
        mbedtls_ssl_set_bio(&c->ssl, &c->fd, ssl_send, ssl_recv, NULL);
        int r;
        while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
                return ESP_FAIL;
        }
        c->tls_up = 1;
    }

    /* Request line + standard + custom headers. Body (if any) is sent by
     * esp_http_client_write() next; we advertise its length here. */
    const char *verb = (c->method == HTTP_METHOD_POST) ? "POST" : "GET";
    char req[HDRS_CAP + 1024];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: mmkeypad\r\n"
                     "Accept: */*\r\n",
                     verb, c->path, c->host);
    if (c->hdrs_len > 0 && n < (int)sizeof(req))
        n += snprintf(req + n, sizeof(req) - n, "%s", c->hdrs);
    if (write_len > 0 && n < (int)sizeof(req))
        n += snprintf(req + n, sizeof(req) - n, "Content-Length: %d\r\n", write_len);
    if (n < (int)sizeof(req))
        n += snprintf(req + n, sizeof(req) - n, "Connection: close\r\n\r\n");
    if (n >= (int)sizeof(req)) return ESP_FAIL;

    return sock_send_all(c, req, n) == n ? ESP_OK : ESP_FAIL;
}

int esp_http_client_write(esp_http_client_handle_t c, const char *buffer, int len) {
    if (!c) return -1;
    if (len <= 0) return 0;
    return sock_send_all(c, buffer, len);
}

static int raw_read(esp_http_client_handle_t c, char *buf, int len) {
    if (c->https) {
        int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r;
    }
    return (int)recv(c->fd, buf, len, 0);
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    if (!c) return -1;
    char hdr[4096];
    int total = 0, bodylen = -1, hdr_end = -1;
    while (total < (int)sizeof(hdr) - 1) {
        int r = raw_read(c, hdr + total, sizeof(hdr) - 1 - total);
        if (r <= 0) break;
        total += r;
        hdr[total] = 0;
        char *e = strstr(hdr, "\r\n\r\n");
        if (e) { hdr_end = (int)(e - hdr) + 4; break; }
    }
    if (hdr_end < 0) return -1;
    /* status line: "HTTP/1.1 200 OK" */
    if (strncmp(hdr, "HTTP/", 5) == 0) {
        char *sp = strchr(hdr, ' ');
        if (sp) c->status = atoi(sp + 1);
    }
    char *cl = strstr(hdr, "Content-Length:");
    if (!cl) cl = strstr(hdr, "content-length:");
    if (cl) bodylen = atoi(cl + 15);
    c->stash_len = total - hdr_end;
    c->stash_off = 0;
    if (c->stash_len > STASH_CAP) c->stash_len = STASH_CAP;
    memcpy(c->stash, hdr + hdr_end, c->stash_len);
    return bodylen;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    return c ? c->status : 0;
}

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len) {
    if (!c) return -1;
    if (c->stash_off < c->stash_len) {
        int n = c->stash_len - c->stash_off;
        if (n > len) n = len;
        memcpy(buf, c->stash + c->stash_off, n);
        c->stash_off += n;
        return n;
    }
    return raw_read(c, buf, len);
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    if (!c) return ESP_OK;
    if (c->tls_up) { mbedtls_ssl_close_notify(&c->ssl); c->tls_up = 0; }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    if (!c) return ESP_OK;
    esp_http_client_close(c);
    if (c->https) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->entropy);
        mbedtls_x509_crt_free(&c->cacert);
    }
    free(c);
    return ESP_OK;
}
