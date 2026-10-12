/** \file
	\brief MCP plugin configuration, stored as JSON (MCP.cfg) in tSIP profile directory
*/
#ifndef ConfigH
#define ConfigH

#include <string>
#include <vector>

struct Config
{
	unsigned int port;			///< HTTP port, server listens on 127.0.0.1 only
	std::string token;			///< bearer token required from MCP client; generated if empty
	bool confirmCalls;			///< ask user (message box) before tool starts / transfers a call or sends message
	bool allowRunScript;		///< run_script tool (any Lua code) - disabled by default
	enum ActivityStatus {
		ACTIVITY_OFF = 0,
		ACTIVITY_ACTIONS,		///< show only tools changing something (calls, DTMF, ...)
		ACTIVITY_ALL			///< show also tools reading data (history, contacts, ...)
	};
	ActivityStatus activityStatus;		///< show tool calls in tSIP status bar / tray hint
	unsigned int activityStatusSeconds;	///< how long status is shown after last tool call
	std::vector<std::string> allowPrefixes;	///< if not empty: make_call allowed only for numbers starting with one of these
	std::vector<std::string> denyPrefixes;	///< make_call denied for numbers starting with one of these
	std::vector<std::string> enabledTools;	///< if not empty: only these tools are available

	Config(void);

	enum LoadStatus {
		LOAD_OK = 0,
		LOAD_NOT_FOUND = -1,	///< file does not exist (e.g. first start)
		LOAD_INVALID = -2		///< file exists but can not be read / is not valid JSON object
	};
	/** \brief Load from file; missing fields keep defaults
		\return LoadStatus value
	*/
	int Load(const std::string &path);
	int Save(const std::string &path) const;

	/** \brief Generate random token if it is empty
		\return true if token was generated (config should be saved)
	*/
	bool EnsureToken(void);

	bool IsToolEnabled(const std::string &name) const;

	/** \brief Check number against allow/deny prefixes
		\param reason set to explanation if not allowed
	*/
	bool IsNumberAllowed(const std::string &number, std::string &reason) const;
};

/** \brief Fallback path of config file: DLL path with .cfg extension
	(normally config is stored in tSIP profile directory, see main.cpp) */
std::string GetDllConfigPath(void);

#endif
