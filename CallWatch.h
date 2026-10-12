/** \file
	\brief Tracking call states from tSIP "call_state" events, waiting for
	call state changes (wait_for_call tool)
*/
#ifndef CallWatchH
#define CallWatchH

#include <json/json.h>
#include <string>

namespace CallWatch
{
	enum { MAX_TIMEOUT_S = 60, DEFAULT_TIMEOUT_S = 30 };

	/** \brief Enable waiting; call from plugin Start() */
	void Start(void);

	/** \brief Wake all waiting threads (they return "stopping") and disable waiting;
		call from plugin Stop() before joining server threads - never blocks
	*/
	void Shutdown(void);

	/** \brief Handle "call_state" event (JSON object) - called from tSIP main thread, short */
	void OnCallStateEvent(const char *json);

	/** \brief Wait for condition
		\param condition "established", "ended" or "incoming" (new incoming call)
		\param uid call uid for established/ended; 0 = only active call
		\param timeoutS timeout in seconds, limited to MAX_TIMEOUT_S
		\param result JSON object for tool result
		\return empty string or error text (invalid arguments)
	*/
	std::string Wait(const std::string &condition, unsigned int uid, int timeoutS, Json::Value &result);
}

#endif
