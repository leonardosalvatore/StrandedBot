#ifndef BOTS_LLAMA_LAUNCHER_H
#define BOTS_LLAMA_LAUNCHER_H

#include <stdbool.h>

/* Set the TCP port used by port_open / start / wait_ready. Must be called
 * before any of those functions; defaults to 53425 if never set. */
void llama_launcher_set_port(int port);
int  llama_launcher_get_port(void);

/* When enabled, the child (start-llama-server.sh + llama-server) inherits
 * the parent's stdout/stderr so every line shows up in the same terminal.
 * When disabled (the default), both streams are redirected to /dev/null
 * in the child before exec, so the parent's output stays clean. Must be
 * called before llama_launcher_start() to take effect. */
void llama_launcher_set_log_enabled(bool enabled);
bool llama_launcher_get_log_enabled(void);

/* Quick TCP connect() probe on 127.0.0.1:<port>. Returns true if something
 * is already listening (in which case the caller should skip the spawn). */
bool llama_launcher_port_open(void);

/* Fork + execvp the script at `script_path`. The current configured port
 * is passed as `-p <port>` on the script command line. Child inherits the
 * parent's stdout/stderr so the server's logs flow into the same terminal.
 * The PID is remembered so llama_launcher_stop() can kill it later.
 *
 * No-op (returns true) if the port is already open. Returns false if the
 * fork/exec failed. */
bool llama_launcher_start(const char *script_path);

/* Poll the configured port every 250ms for up to `timeout_sec` seconds.
 * Returns true as soon as the server starts accepting connections, false on
 * timeout. */
bool llama_launcher_wait_ready(int timeout_sec);

/* Send SIGTERM to the child pid (if any) and waitpid it. Idempotent; safe
 * to register with atexit. */
void llama_launcher_stop(void);

#endif /* BOTS_LLAMA_LAUNCHER_H */
