#include "http_post.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

char *http_post(const char *host, int port, const char *path,
                const char *content_type, const char *body,
                int timeout_sec)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct timeval tv = {.tv_sec = timeout_sec, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    struct hostent *he = gethostbyname(host);
    if (he)
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    else
        inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    size_t body_len = body ? strlen(body) : 0;
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port, content_type, body_len);

    if (send(fd, header, hlen, 0) < 0) { close(fd); return NULL; }
    if (body_len > 0) {
        size_t sent = 0;
        while (sent < body_len) {
            ssize_t n = send(fd, body + sent, body_len - sent, 0);
            if (n <= 0) { close(fd); return NULL; }
            sent += n;
        }
    }

    /* Read full response */
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    while (1) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) { free(buf); close(fd); return NULL; }
            buf = p;
        }
        ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += n;
    }
    buf[len] = '\0';
    close(fd);

    /* Find body after \r\n\r\n */
    char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) { free(buf); return NULL; }
    body_start += 4;

    char *result = strdup(body_start);
    free(buf);
    return result;
}
