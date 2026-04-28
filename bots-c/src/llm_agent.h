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

/* Queue a user message from the compose box. The next agent turn will
 * prepend it to the LLM conversation as a "user" role message. Multiple
 * submissions before a flush are concatenated with newlines. Thread-safe. */
void llm_agent_queue_user_message(const char *text);

#endif /* LLM_AGENT_H */
