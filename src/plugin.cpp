#include "plugin.h"
#include "pluginmain.h"
#include "resource.h"
#include "chat_panel.h"
#include "config.h"

#include <string>
#include <shlobj.h>

constexpr int MENU_SETTINGS = 1;
constexpr int MENU_SHOW_CHAT = 2;

static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // Load provider
        char provider[64] = {};
        bool isAnthropic = true;
        if (BridgeSettingGet(CFG_SECTION, CFG_PROVIDER, provider) && strcmp(provider, PROVIDER_OPENAI) == 0)
            isAnthropic = false;

        CheckRadioButton(hDlg, IDC_RADIO_ANTHROPIC, IDC_RADIO_OPENAI,
            isAnthropic ? IDC_RADIO_ANTHROPIC : IDC_RADIO_OPENAI);

        // Load API key
        char apiKey[512] = {};
        if (BridgeSettingGet(CFG_SECTION, CFG_API_KEY, apiKey))
            SetDlgItemTextA(hDlg, IDC_EDIT_APIKEY, apiKey);

        // Load endpoint
        char endpoint[512] = {};
        if (BridgeSettingGet(CFG_SECTION, CFG_ENDPOINT, endpoint) && strlen(endpoint) > 0)
            SetDlgItemTextA(hDlg, IDC_EDIT_ENDPOINT, endpoint);
        else
            SetDlgItemTextA(hDlg, IDC_EDIT_ENDPOINT,
                isAnthropic ? DEFAULT_ANTHROPIC_ENDPOINT : DEFAULT_OPENAI_ENDPOINT);

        // Load model
        char model[256] = {};
        if (BridgeSettingGet(CFG_SECTION, CFG_MODEL, model) && strlen(model) > 0)
            SetDlgItemTextA(hDlg, IDC_EDIT_MODEL, model);
        else
            SetDlgItemTextA(hDlg, IDC_EDIT_MODEL,
                isAnthropic ? DEFAULT_ANTHROPIC_MODEL : DEFAULT_OPENAI_MODEL);

        // Load rippy directory
        char rippyDir[MAX_PATH] = {};
        if (BridgeSettingGet(CFG_SECTION, CFG_RIPPY_DIR, rippyDir) && strlen(rippyDir) > 0)
            SetDlgItemTextA(hDlg, IDC_EDIT_RIPPYDIR, rippyDir);

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_RADIO_ANTHROPIC:
        {
            char curEndpoint[512] = {};
            char curModel[256] = {};
            GetDlgItemTextA(hDlg, IDC_EDIT_ENDPOINT, curEndpoint, sizeof(curEndpoint));
            GetDlgItemTextA(hDlg, IDC_EDIT_MODEL, curModel, sizeof(curModel));

            if (strlen(curEndpoint) == 0 || strcmp(curEndpoint, DEFAULT_OPENAI_ENDPOINT) == 0)
                SetDlgItemTextA(hDlg, IDC_EDIT_ENDPOINT, DEFAULT_ANTHROPIC_ENDPOINT);
            if (strlen(curModel) == 0 || strcmp(curModel, DEFAULT_OPENAI_MODEL) == 0)
                SetDlgItemTextA(hDlg, IDC_EDIT_MODEL, DEFAULT_ANTHROPIC_MODEL);
            return TRUE;
        }

        case IDC_RADIO_OPENAI:
        {
            char curEndpoint[512] = {};
            char curModel[256] = {};
            GetDlgItemTextA(hDlg, IDC_EDIT_ENDPOINT, curEndpoint, sizeof(curEndpoint));
            GetDlgItemTextA(hDlg, IDC_EDIT_MODEL, curModel, sizeof(curModel));

            if (strlen(curEndpoint) == 0 || strcmp(curEndpoint, DEFAULT_ANTHROPIC_ENDPOINT) == 0)
                SetDlgItemTextA(hDlg, IDC_EDIT_ENDPOINT, DEFAULT_OPENAI_ENDPOINT);
            if (strlen(curModel) == 0 || strcmp(curModel, DEFAULT_ANTHROPIC_MODEL) == 0)
                SetDlgItemTextA(hDlg, IDC_EDIT_MODEL, DEFAULT_OPENAI_MODEL);
            return TRUE;
        }

        case IDC_BTN_BROWSE:
        {
            BROWSEINFOA bi = {};
            bi.hwndOwner = hDlg;
            bi.lpszTitle = "Select Rippy Directory";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
            if (pidl)
            {
                char path[MAX_PATH] = {};
                if (SHGetPathFromIDListA(pidl, path))
                    SetDlgItemTextA(hDlg, IDC_EDIT_RIPPYDIR, path);
                CoTaskMemFree(pidl);
            }
            return TRUE;
        }

        case IDOK:
        {
            bool isAnthropic = IsDlgButtonChecked(hDlg, IDC_RADIO_ANTHROPIC) == BST_CHECKED;

            char apiKey[512] = {};
            GetDlgItemTextA(hDlg, IDC_EDIT_APIKEY, apiKey, sizeof(apiKey));

            char endpoint[512] = {};
            GetDlgItemTextA(hDlg, IDC_EDIT_ENDPOINT, endpoint, sizeof(endpoint));

            char model[256] = {};
            GetDlgItemTextA(hDlg, IDC_EDIT_MODEL, model, sizeof(model));

            char rippyDir[MAX_PATH] = {};
            GetDlgItemTextA(hDlg, IDC_EDIT_RIPPYDIR, rippyDir, sizeof(rippyDir));

            if (strlen(apiKey) == 0)
            {
                MessageBoxA(hDlg, "API key cannot be empty.", "x64dbg-rippy", MB_ICONWARNING);
                return TRUE;
            }

            BridgeSettingSet(CFG_SECTION, CFG_PROVIDER, isAnthropic ? PROVIDER_ANTHROPIC : PROVIDER_OPENAI);
            BridgeSettingSet(CFG_SECTION, CFG_API_KEY, apiKey);
            BridgeSettingSet(CFG_SECTION, CFG_ENDPOINT, endpoint);
            BridgeSettingSet(CFG_SECTION, CFG_MODEL, model);
            BridgeSettingSet(CFG_SECTION, CFG_RIPPY_DIR, rippyDir);
            BridgeSettingFlush();

            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }

    return FALSE;
}

void openSettingsDialog()
{
    INT_PTR result = DialogBoxA(hinst, MAKEINTRESOURCEA(IDD_SETTINGS), hwndDlg, SettingsDlgProc);
    if (result == IDOK && g_chatPanel)
    {
        g_chatPanel->refreshStatus();
        g_chatPanel->rescanSkills();
    }
}

static void cb_menu_entry(CBTYPE cbType, void* callbackInfo)
{
    PLUG_CB_MENUENTRY* info = (PLUG_CB_MENUENTRY*)callbackInfo;
    if (info->hEntry == MENU_SETTINGS)
    {
        openSettingsDialog();
    }
    else if (info->hEntry == MENU_SHOW_CHAT)
    {
        if (g_chatPanel)
            g_chatPanel->showTab();
    }
}

static void initChatPanelOnGuiThread(void* /*unused*/)
{
    g_chatPanel = new ChatPanel();
    if (!g_chatPanel->initialize())
    {
        dputs("Failed to initialize chat panel");
        delete g_chatPanel;
        g_chatPanel = nullptr;
    }
}

bool pluginInit(PLUG_INITSTRUCT* initStruct)
{
    dprintf("pluginInit(pluginHandle: %d)\n", pluginHandle);
    _plugin_registercallback(pluginHandle, CB_MENUENTRY, cb_menu_entry);

    // Default the Rippy directory if not configured
    char rippyDir[MAX_PATH] = {};
    if (!BridgeSettingGet(CFG_SECTION, CFG_RIPPY_DIR, rippyDir) || strlen(rippyDir) == 0)
    {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir = exePath;
        auto lastSlash = dir.find_last_of("\\/");
        if (lastSlash != std::string::npos)
            dir = dir.substr(0, lastSlash);
        BridgeSettingSet(CFG_SECTION, CFG_RIPPY_DIR, dir.c_str());
        dprintf("Defaulted RippyDir to: %s\n", dir.c_str());
    }

    return true;
}

void pluginStop()
{
    dprintf("pluginStop(pluginHandle: %d)\n", pluginHandle);
    if (g_chatPanel)
    {
        g_chatPanel->shutdown();
        delete g_chatPanel;
        g_chatPanel = nullptr;
    }
}

void pluginSetup()
{
    dprintf("pluginSetup(pluginHandle: %d)\n", pluginHandle);

    _plugin_menuaddentry(hMenu, MENU_SETTINGS, "Settings...");
    _plugin_menuaddentry(hMenu, MENU_SHOW_CHAT, "Show Chat");

    // Initialize chat panel on the GUI thread
    GuiExecuteOnGuiThreadEx(initChatPanelOnGuiThread, nullptr);
}
