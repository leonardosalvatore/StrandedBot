#ifndef LLM_AGENT_H
#define LLM_AGENT_H

#include <stdbool.h>
#include <pthread.h>

#define LLM_DEFAULT_URL "http://localhost:53425/v1/chat/completions"

void llm_agent_start(const char *initial_prompt);
void llm_agent_stop(void);
bool llm_agent_running(void);

/* Interactive replies from UI */
void llm_agent_submit_reply(const char *text);
bool llm_agent_waiting_for_reply(void);
float llm_agent_reply_seconds_remaining(void);

/* Set the sticky OPERATOR DIRECTIVE from the compose box. The agent
 * re-injects this string at the top of every turn's user message until
 * the operator submits a new one (last-writer-wins). Empty / NULL is a
 * no-op; to clear an active directive, SEND a "resume default" sentence
 * or a single space. Thread-safe. The function name is preserved for
 * backwards compatibility with the existing UI wiring. */
void llm_agent_queue_user_message(const char *text);

/* Drop all chat history except the seed (system prompt + the mission
 * prompt originally passed to llm_agent_start). Safe to call from the UI
 * thread — the request is honored at the next turn boundary, not
 * mid-HTTP-call. The bot resumes from a clean slate; one SYSLOG line is
 * emitted to confirm. */
void llm_agent_wipe_memory(void);

/* Pause / resume the per-turn loop. While paused the agent thread
 * sleeps between turns instead of advancing the simulation. PAUSE only
 * takes effect at the next turn boundary, so an in-flight tool call
 * still completes before the loop blocks. */
void llm_agent_set_paused(bool paused);
bool llm_agent_paused(void);

#endif /* LLM_AGENT_H */
