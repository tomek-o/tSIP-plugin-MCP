/** \file
	\brief Plugin settings dialog (plain WinAPI, dialog template built in memory -
	no resource compiler needed); shown from plugin ShowSettings()
*/
#ifndef SettingsDialogH
#define SettingsDialogH

#include <windows.h>
#include <string>

struct Config;

namespace SettingsDialog
{
	/** \brief Show modal settings dialog
		\param cfg in: current settings, out: edited settings (only if true returned)
		\param statusText server status shown in dialog (e.g. "running: http://...")
		\param configPath shown in dialog
		\return true if user accepted changes (OK)
	*/
	bool Show(HWND parent, Config &cfg, const std::string &statusText, const std::string &configPath);
}

#endif
