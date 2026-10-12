/** \file
	\brief Showing MCP client activity in tSIP application status
	(status bar / tray hint), e.g. "AI (MCP): make_call 123"
*/
#ifndef ActivityH
#define ActivityH

#include <string>

struct Config;

namespace Activity
{
	/** \brief Start clearing thread; call from plugin Start() */
	void Start(const Config &cfg);

	/** \brief Stop clearing thread and clear status; call from plugin Stop() (main thread) */
	void Stop(void);

	/** \brief Report tool call - thread-safe, does not wait for tSIP main thread
		\param tool tool name
		\param detail e.g. call target, may be empty (UTF-8)
		\param readOnly tool only reads data (not shown in "actions" mode)
	*/
	void Report(const std::string &tool, const std::string &detail, bool readOnly);
}

#endif
