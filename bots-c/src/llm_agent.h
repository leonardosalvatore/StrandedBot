#ifndef LLM_AGENT_H
#define LLM_AGENT_H

#include <stdbool.h>
#include <pthread.h>

#define LLM_DEFAULT_URL "http://localhost:8080/v1/chat/completions"

void llm_agent_start(const char *initial_prompt, bool interactive_mode);
void llm_agent_stop(void);
bool llm_agent_running(void);

/* Interactive replies from UI */
void llm_agent_submit_reply(const char *text);
bool llm_agent_waiting_for_reply(void);
float llm_agent_reply_seconds_remaining(void);

#endif /* LLM_AGENT_H */
