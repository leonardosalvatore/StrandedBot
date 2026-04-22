/* Needed for nanosleep() on glibc. Must be defined before any system
 * header is included. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "llama_launcher.h"

#include "raylib.h"  /* TraceLog */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define LLAMA_HOST "127.0.0.1"
#define LLAMA_PORT_DEFAULT 53425

static pid_t g_child_pid       = 0;
static bool  g_atexit_registered = false;
static bool  g_signals_installed  = false;
static int   g_port              = LLAMA_PORT_DEFAULT;

void llama_launcher_set_port(int port) {
    if (port > 0 && port < 65536) g_port = port;
}
int llama_launcher_get_port(void) { return g_port; }

/* ── HTTP /health probe ───────────────────────────────────────────────────── */
/* TCP-listening is not enough: llama-server binds the port ~6 s after spawn
 * but the model keeps loading for another ~30 s, during which /v1/chat/...
 * returns HTTP 503 "Loading model". /health reflects the real readiness:
 *   200 "ok"       -> ready for inference
 *   503 Loading... -> bound but still loading
 * We do a tiny synchronous GET and return true only on a 200 response. */
static bool llama_health_ok(int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, LLAMA_HOST, &addr.sin_addr);

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) { close(fd); return false; }
    if (rc != 0) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) { close(fd); return false; }
        int err = 0; socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            close(fd); return false;
        }
    }

    /* Back to blocking for the short request/response. Use a send/recv
     * timeout to stay bounded. */
    fcntl(fd, F_SETFL, flags);
    struct timeval tv = { .tv_sec = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    static const char req[] =
        "GET /health HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send(fd, req, sizeof(req) - 1, 0) < 0) { close(fd); return false; }

    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    /* "HTTP/1.1 200 " — substring match is robust against minor header
     * formatting changes in the server. */
    return strncmp(buf, "HTTP/1.", 7) == 0 && strstr(buf, " 200 ") != NULL;
}

/* ── Port probe ───────────────────────────────────────────────────────────── */
bool llama_launcher_port_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, LLAMA_HOST, &addr.sin_addr);

    /* Use a non-blocking connect with a short select() timeout so we never
     * stall for the kernel's default 127s RST-fallback when nothing is
     * listening locally. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    bool ok = false;
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 }; /* 200 ms */
        if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0; socklen_t elen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 &&
                err == 0) {
                ok = true;
            }
        }
    }
    close(fd);
    return ok;
}

/* ── Cleanup ──────────────────────────────────────────────────────────────── */
/* The child calls setpgid(0,0), so its pid is its own pgid. We signal the
 * whole process group (kill(-pid, ...)) so any helpers the script forks
 * (bash subshells, llama-server worker threads, orphaned grandchildren) die
 * together with the server. Signalling just the leader would leave children
 * reparented to init if the leader dies first. */
void llama_launcher_stop(void) {
    if (g_child_pid <= 0) return;
    pid_t pid = g_child_pid;
    g_child_pid = 0;

    TraceLog(LOG_INFO, "LLAMA: stopping llama-server (pgid %d)", (int)pid);

    if (kill(-pid, SIGTERM) != 0 && errno != ESRCH) {
        /* Fall back to the leader only; some kernels reject -pid if the
         * group has no members other than the exiting leader. */
        if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
            TraceLog(LOG_WARNING, "LLAMA: SIGTERM failed: %s", strerror(errno));
        }
    }

    /* Give it ~2 seconds to exit, then escalate. */
    for (int i = 0; i < 40; i++) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0) return;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* ── Signal plumbing ──────────────────────────────────────────────────────── */
/* When the user Ctrl-C's the parent, we must still SIGTERM the child before
 * exiting. We do this in a minimal async-signal-safe handler that just calls
 * kill() and then re-raises the signal with the default handler. */
static void on_fatal_signal(int sig) {
    if (g_child_pid > 0) {
        /* Signal the entire child process group so the bash wrapper AND the
         * exec'd llama-server (and any grandchildren) all receive SIGTERM. */
        pid_t pid = g_child_pid;
        g_child_pid = 0; /* prevent atexit from double-killing */
        kill(-pid, SIGTERM);
        kill(pid,  SIGTERM);
    }
    /* Restore default and re-raise so the shell's exit code reflects the
     * original signal. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_signal_handlers(void) {
    if (g_signals_installed) return;
    g_signals_installed = true;
    struct sigaction sa = {0};
    sa.sa_handler = on_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
}

/* ── Spawn ────────────────────────────────────────────────────────────────── */
bool llama_launcher_start(const char *script_path) {
    if (!script_path || !script_path[0]) {
        TraceLog(LOG_WARNING, "LLAMA: empty start_script; skipping auto-spawn");
        return false;
    }
    if (llama_launcher_port_open()) {
        TraceLog(LOG_INFO, "LLAMA: :%d already listening, skipping spawn",
                 g_port);
        return true;
    }

    TraceLog(LOG_INFO, "LLAMA: spawning %s -p %d", script_path, g_port);

    /* Serialise the port in the parent; argv copies below are owned by the
     * child's own address space after exec, so it's safe to use stack-local
     * strings here. */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", g_port);

    pid_t pid = fork();
    if (pid < 0) {
        TraceLog(LOG_ERROR, "LLAMA: fork failed: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        /* Child: detach from the parent's process group so Ctrl-C on the
         * parent doesn't deliver SIGINT to the child directly (we rely on
         * our signal handler to SIGTERM it explicitly and wait). */
        setpgid(0, 0);

        /* Child shares the parent's stdout/stderr, so llama-server's logs
         * appear in the same terminal. No pipe machinery needed. */
        char *argv[4];
        argv[0] = (char *)script_path;
        argv[1] = "-p";
        argv[2] = port_str;
        argv[3] = NULL;
        execvp(script_path, argv);
        fprintf(stderr, "LLAMA: execvp(%s) failed: %s\n",
                script_path, strerror(errno));
        _exit(127);
    }

    g_child_pid = pid;
    if (!g_atexit_registered) {
        atexit(llama_launcher_stop);
        g_atexit_registered = true;
    }
    install_signal_handlers();
    return true;
}

/* ── Readiness wait ───────────────────────────────────────────────────────── */
/* Two-phase readiness:
 *   Phase A — port-open (usually 5-10 s): server has bound the socket.
 *   Phase B — /health returns 200 (+20-40 s): model is loaded, inference ready.
 * Before this was split, we returned true on phase A, and the first chat
 * request hit `503 Loading model`. Now we only return true after phase B. */
bool llama_launcher_wait_ready(int timeout_sec) {
    if (timeout_sec <= 0) timeout_sec = 120; /* model load can be slow */
    int iters = timeout_sec * 4; /* 250 ms cadence */
    bool port_seen = false;
    for (int i = 0; i < iters; i++) {
        if (!port_seen && llama_launcher_port_open()) {
            TraceLog(LOG_INFO, "LLAMA: port :%d listening after ~%d ms, "
                               "waiting for model to finish loading...",
                     g_port, i * 250);
            port_seen = true;
        }
        if (port_seen && llama_health_ok(300)) {
            TraceLog(LOG_INFO, "LLAMA: /health OK after ~%d ms total",
                     i * 250);
            return true;
        }
        /* If the child died while we were waiting, bail early. */
        if (g_child_pid > 0) {
            int status = 0;
            pid_t r = waitpid(g_child_pid, &status, WNOHANG);
            if (r == g_child_pid) {
                TraceLog(LOG_ERROR,
                         "LLAMA: child exited before ready (status %d)",
                         status);
                g_child_pid = 0;
                return false;
            }
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 250 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    TraceLog(LOG_WARNING,
             "LLAMA: /health not 200 after %d s "
             "(port %s); continuing anyway",
             timeout_sec, port_seen ? "open" : "closed");
    return false;
}
