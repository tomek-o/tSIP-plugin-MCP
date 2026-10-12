#include "SettingsDialog.h"
#include "Config.h"
#include "Tools.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

enum
{
	IDC_STATUS = 1001,
	IDC_CONFIG_PATH,
	IDC_PORT,
	IDC_TOKEN,
	IDC_REGENERATE,
	IDC_CONFIRM_CALLS,
	IDC_ALLOW_RUN_SCRIPT,
	IDC_ACTIVITY,
	IDC_ACTIVITY_SECONDS,
	IDC_DENY_PREFIXES,
	IDC_ALLOW_PREFIXES,
	IDC_TOOLS,
	IDC_COPY_COMMAND
};

// predefined window class atoms for dialog templates
enum
{
	CLS_BUTTON = 0x0080,
	CLS_EDIT = 0x0081,
	CLS_STATIC = 0x0082,
	CLS_LISTBOX = 0x0083,
	CLS_COMBOBOX = 0x0085
};

/** \brief In-memory DLGTEMPLATE builder */
class DialogTemplate
{
public:
	DialogTemplate(const wchar_t *title, short cx, short cy)
	{
		// DLGTEMPLATE
		AddDword(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_CENTER);
		AddDword(0);			// extended style
		countPos = data.size();
		AddWord(0);				// item count, updated by AddItem
		AddWord(0);				// x
		AddWord(0);				// y
		AddWord(cx);
		AddWord(cy);
		AddWord(0);				// no menu
		AddWord(0);				// default dialog class
		AddString(title);
		AddWord(8);				// font size
		AddString(L"MS Shell Dlg");
	}

	void AddItem(WORD cls, DWORD style, short x, short y, short cx, short cy, WORD id, const wchar_t *text)
	{
		Align();
		AddDword(style | WS_CHILD | WS_VISIBLE);
		AddDword(0);
		AddWord(x);
		AddWord(y);
		AddWord(cx);
		AddWord(cy);
		AddWord(id);
		AddWord(0xFFFF);
		AddWord(cls);
		AddString(text);
		AddWord(0);				// no creation data
		data[countPos]++;
	}

	LPCDLGTEMPLATEW Get(void) const
	{
		return reinterpret_cast<LPCDLGTEMPLATEW>(&data[0]);
	}

private:
	std::vector<WORD> data;
	size_t countPos;

	void AddWord(WORD w)
	{
		data.push_back(w);
	}
	void AddDword(DWORD d)
	{
		AddWord(LOWORD(d));
		AddWord(HIWORD(d));
	}
	void AddString(const wchar_t *s)
	{
		while (*s)
			AddWord(*s++);
		AddWord(0);
	}
	void Align(void)
	{
		// items have to be DWORD-aligned (vector of WORDs: even index)
		if (data.size() % 2)
			AddWord(0);
	}
};

struct DialogContext
{
	Config *cfg;
	std::string statusText;
	std::string configPath;
	std::vector<std::string> toolNames;
};

std::string GetText(HWND dlg, int id)
{
	HWND wnd = GetDlgItem(dlg, id);
	int len = GetWindowTextLengthA(wnd);
	std::string text(len + 1, '\0');
	GetWindowTextA(wnd, &text[0], len + 1);
	text.resize(len);
	return text;
}

std::string JoinList(const std::vector<std::string> &list)
{
	std::string out;
	for (size_t i = 0; i < list.size(); i++)
	{
		if (i)
			out += ", ";
		out += list[i];
	}
	return out;
}

std::vector<std::string> SplitList(const std::string &text)
{
	std::vector<std::string> out;
	std::string item;
	for (size_t i = 0; i <= text.size(); i++)
	{
		char c = (i < text.size()) ? text[i] : ',';
		if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
		{
			if (!item.empty())
				out.push_back(item);
			item.clear();
		}
		else
		{
			item += c;
		}
	}
	return out;
}

std::string GetClaudeCommand(unsigned int port, const std::string &token)
{
	char url[64];
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/mcp", port);
	return std::string("claude mcp add --transport http tsip ") + url +
		" --header \"Authorization: Bearer " + token + "\"";
}

void CopyToClipboard(HWND owner, const std::string &text)
{
	if (!OpenClipboard(owner))
		return;
	EmptyClipboard();
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
	if (mem)
	{
		memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
		GlobalUnlock(mem);
		if (!SetClipboardData(CF_TEXT, mem))
			GlobalFree(mem);
	}
	CloseClipboard();
}

void InitDialog(HWND dlg, DialogContext *ctx)
{
	const Config &cfg = *ctx->cfg;
	SetDlgItemTextA(dlg, IDC_STATUS, ctx->statusText.c_str());
	SetDlgItemTextA(dlg, IDC_CONFIG_PATH, ctx->configPath.c_str());
	SetDlgItemInt(dlg, IDC_PORT, cfg.port, FALSE);
	SetDlgItemTextA(dlg, IDC_TOKEN, cfg.token.c_str());
	CheckDlgButton(dlg, IDC_CONFIRM_CALLS, cfg.confirmCalls ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(dlg, IDC_ALLOW_RUN_SCRIPT, cfg.allowRunScript ? BST_CHECKED : BST_UNCHECKED);

	HWND combo = GetDlgItem(dlg, IDC_ACTIVITY);
	SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("off"));
	SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("actions"));
	SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("all"));
	SendMessageA(combo, CB_SETCURSEL, static_cast<WPARAM>(cfg.activityStatus), 0);
	SetDlgItemInt(dlg, IDC_ACTIVITY_SECONDS, cfg.activityStatusSeconds, FALSE);

	SetDlgItemTextA(dlg, IDC_DENY_PREFIXES, JoinList(cfg.denyPrefixes).c_str());
	SetDlgItemTextA(dlg, IDC_ALLOW_PREFIXES, JoinList(cfg.allowPrefixes).c_str());

	HWND list = GetDlgItem(dlg, IDC_TOOLS);
	for (size_t i = 0; i < ctx->toolNames.size(); i++)
	{
		const std::string &name = ctx->toolNames[i];
		SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
		bool enabled = cfg.enabledTools.empty();
		for (size_t j = 0; j < cfg.enabledTools.size(); j++)
		{
			if (cfg.enabledTools[j] == name)
				enabled = true;
		}
		SendMessageA(list, LB_SETSEL, enabled ? TRUE : FALSE, static_cast<LPARAM>(i));
	}
}

/** \return true if values are valid and stored in cfg */
bool ReadDialog(HWND dlg, DialogContext *ctx, Config &cfg)
{
	BOOL ok = FALSE;
	UINT port = GetDlgItemInt(dlg, IDC_PORT, &ok, FALSE);
	if (!ok || port == 0 || port > 65535)
	{
		MessageBoxA(dlg, "Port has to be a number 1..65535.", "MCP plugin settings", MB_ICONWARNING);
		SetFocus(GetDlgItem(dlg, IDC_PORT));
		return false;
	}
	UINT seconds = GetDlgItemInt(dlg, IDC_ACTIVITY_SECONDS, &ok, FALSE);
	if (!ok || seconds < 1 || seconds > 3600)
	{
		MessageBoxA(dlg, "Activity status time has to be 1..3600 seconds.", "MCP plugin settings", MB_ICONWARNING);
		SetFocus(GetDlgItem(dlg, IDC_ACTIVITY_SECONDS));
		return false;
	}

	std::vector<std::string> enabledTools;
	HWND list = GetDlgItem(dlg, IDC_TOOLS);
	for (size_t i = 0; i < ctx->toolNames.size(); i++)
	{
		if (SendMessageA(list, LB_GETSEL, static_cast<WPARAM>(i), 0) > 0)
			enabledTools.push_back(ctx->toolNames[i]);
	}
	if (enabledTools.empty())
	{
		MessageBoxA(dlg, "Select at least one tool.", "MCP plugin settings", MB_ICONWARNING);
		return false;
	}
	if (enabledTools.size() == ctx->toolNames.size())
		enabledTools.clear();	// empty list = all tools (also tools added in future versions)

	cfg.port = port;
	cfg.token = GetText(dlg, IDC_TOKEN);
	cfg.confirmCalls = IsDlgButtonChecked(dlg, IDC_CONFIRM_CALLS) == BST_CHECKED;
	cfg.allowRunScript = IsDlgButtonChecked(dlg, IDC_ALLOW_RUN_SCRIPT) == BST_CHECKED;
	LRESULT sel = SendMessageA(GetDlgItem(dlg, IDC_ACTIVITY), CB_GETCURSEL, 0, 0);
	if (sel >= Config::ACTIVITY_OFF && sel <= Config::ACTIVITY_ALL)
		cfg.activityStatus = static_cast<Config::ActivityStatus>(sel);
	cfg.activityStatusSeconds = seconds;
	cfg.denyPrefixes = SplitList(GetText(dlg, IDC_DENY_PREFIXES));
	cfg.allowPrefixes = SplitList(GetText(dlg, IDC_ALLOW_PREFIXES));
	cfg.enabledTools = enabledTools;
	return true;
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	DialogContext *ctx = reinterpret_cast<DialogContext*>(GetWindowLongPtrA(dlg, DWLP_USER));
	switch (msg)
	{
	case WM_INITDIALOG:
		SetWindowLongPtrA(dlg, DWLP_USER, lParam);
		InitDialog(dlg, reinterpret_cast<DialogContext*>(lParam));
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
		{
			Config cfg = *ctx->cfg;
			if (ReadDialog(dlg, ctx, cfg))
			{
				*ctx->cfg = cfg;
				EndDialog(dlg, IDOK);
			}
			return TRUE;
		}
		case IDCANCEL:
			EndDialog(dlg, IDCANCEL);
			return TRUE;
		case IDC_REGENERATE:
		{
			if (MessageBoxA(dlg, "Generate new token?\n\nMCP clients using current token will have to be "
				"configured again (use \"Copy Claude Code command\").", "MCP plugin settings",
				MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
			{
				return TRUE;
			}
			Config tmp;
			tmp.token.clear();
			tmp.EnsureToken();
			SetDlgItemTextA(dlg, IDC_TOKEN, tmp.token.c_str());
			return TRUE;
		}
		case IDC_COPY_COMMAND:
		{
			BOOL ok = FALSE;
			UINT port = GetDlgItemInt(dlg, IDC_PORT, &ok, FALSE);
			if (!ok || port == 0 || port > 65535)
				port = ctx->cfg->port;
			CopyToClipboard(dlg, GetClaudeCommand(port, GetText(dlg, IDC_TOKEN)));
			return TRUE;
		}
		}
		break;
	}
	return FALSE;
}

HMODULE GetThisModule(void)
{
	HMODULE module = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(&GetThisModule), &module);
	return module;
}

}	// namespace


bool SettingsDialog::Show(HWND parent, Config &cfg, const std::string &statusText, const std::string &configPath)
{
	DialogContext ctx;
	ctx.cfg = &cfg;
	ctx.statusText = statusText;
	ctx.configPath = configPath;
	{
		// all tools known to this plugin version
		Config all;
		all.allowRunScript = true;
		Json::Value tools = Tools::List(all);
		for (unsigned int i = 0; i < tools.size(); i++)
			ctx.toolNames.push_back(tools[i]["name"].asString());
	}

	const short W = 300;
	const short LX = 7;		// label x
	const short CX = 82;	// control x
	const short CW = W - CX - 7;
	DialogTemplate t(L"MCP server plugin settings", W, 268);
	short y = 7;
	t.AddItem(CLS_STATIC, SS_LEFT, LX, y, 72, 8, 0xFFFF, L"Status:");
	t.AddItem(CLS_STATIC, SS_LEFT | SS_ENDELLIPSIS, CX, y, CW, 8, IDC_STATUS, L"");
	y += 12;
	t.AddItem(CLS_STATIC, SS_LEFT, LX, y, 72, 8, 0xFFFF, L"Config file:");
	t.AddItem(CLS_STATIC, SS_LEFT | SS_PATHELLIPSIS, CX, y, CW, 8, IDC_CONFIG_PATH, L"");
	y += 16;
	t.AddItem(CLS_STATIC, SS_LEFT, LX, y + 2, 72, 8, 0xFFFF, L"Port (127.0.0.1):");
	t.AddItem(CLS_EDIT, ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, CX, y, 40, 12, IDC_PORT, L"");
	y += 16;
	t.AddItem(CLS_STATIC, SS_LEFT, LX, y + 2, 72, 8, 0xFFFF, L"Access token:");
	t.AddItem(CLS_EDIT, ES_READONLY | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, CX, y, CW - 64, 12, IDC_TOKEN, L"");
	t.AddItem(CLS_BUTTON, BS_PUSHBUTTON | WS_TABSTOP, W - 7 - 60, y - 1, 60, 14, IDC_REGENERATE, L"Regenerate");
	y += 18;
	t.AddItem(CLS_BUTTON, BS_AUTOCHECKBOX | WS_TABSTOP, LX, y, W - 14, 10, IDC_CONFIRM_CALLS,
		L"Confirm make_call / transfer_call / send_message (message box)");
	y += 13;
	t.AddItem(CLS_BUTTON, BS_AUTOCHECKBOX | WS_TABSTOP, LX, y, W - 14, 10, IDC_ALLOW_RUN_SCRIPT,
		L"Allow run_script tool (Lua - full access to tSIP, no other checks)");
	y += 16;
	t.AddItem(CLS_STATIC, SS_LEFT, LX, y + 2, 72, 8, 0xFFFF, L"Activity status:");
	t.AddItem(CLS_COMBOBOX, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, CX, y, 60, 60, IDC_ACTIVITY, L"");
	t.AddItem(CLS_STATIC, SS_LEFT, CX + 66, y + 2, 50, 8, 0xFFFF, L"shown for [s]:");
	t.AddItem(CLS_EDIT, ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, CX + 118, y, 30, 12,
		IDC_ACTIVITY_SECONDS, L"");
	y += 18;
	t.AddItem(CLS_STATIC, SS_LEFT, LX, y + 2, 72, 8, 0xFFFF, L"Deny prefixes:");
	t.AddItem(CLS_EDIT, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, CX, y, CW, 12, IDC_DENY_PREFIXES, L"");
	y += 16;
	t.AddItem(CLS_STATIC, SS_LEFT, LX, y + 2, 72, 8, 0xFFFF, L"Allow prefixes:");
	t.AddItem(CLS_EDIT, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, CX, y, CW, 12, IDC_ALLOW_PREFIXES, L"");
	y += 14;
	t.AddItem(CLS_STATIC, SS_LEFT, CX, y, CW, 8, 0xFFFF,
		L"comma separated, e.g. 00, +; empty allow list: all numbers");
	y += 14;
	t.AddItem(CLS_STATIC, SS_LEFT, LX, y, W - 14, 8, 0xFFFF, L"Enabled tools (click to toggle):");
	y += 10;
	t.AddItem(CLS_LISTBOX, LBS_MULTIPLESEL | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
		LX, y, W - 14, 70, IDC_TOOLS, L"");
	y += 76;
	t.AddItem(CLS_BUTTON, BS_PUSHBUTTON | WS_TABSTOP, LX, y, 110, 14, IDC_COPY_COMMAND,
		L"Copy Claude Code command");
	t.AddItem(CLS_BUTTON, BS_DEFPUSHBUTTON | WS_TABSTOP, W - 7 - 50 - 4 - 50, y, 50, 14, IDOK, L"OK");
	t.AddItem(CLS_BUTTON, BS_PUSHBUTTON | WS_TABSTOP, W - 7 - 50, y, 50, 14, IDCANCEL, L"Cancel");

	INT_PTR res = DialogBoxIndirectParamW(GetThisModule(), t.Get(), parent, DialogProc,
		reinterpret_cast<LPARAM>(&ctx));
	return res == IDOK;
}
