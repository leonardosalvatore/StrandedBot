#ifndef HTTP_POST_H
#define HTTP_POST_H

/*
 * Minimal HTTP/1.1 POST client using POSIX sockets.
 * Only targets localhost -- no TLS, no chunked encoding, no redirects.
 * Returns malloc'd response body (caller must free), or NULL on error.
 */
char *http_post(const char *host, int port, const char *path,
                const char *content_type, const char *body,
                int timeout_sec);

#endif /* HTTP_POST_H */
