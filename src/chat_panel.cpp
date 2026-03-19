#include "chat_panel.h"
#include "config.h"
#include "resource.h"
#include "api_client.h"
#include "tools.h"
#include "pluginmain.h"
#include "pluginsdk/_scriptapi_module.h"

#include <QWidget>
#include <QIcon>
#include <QPixmap>
#include <commctrl.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <nlohmann/json.hpp>

#include <thread>
#include <string>
#include <sstream>

using json = nlohmann::json;
using namespace Microsoft::WRL;

ChatPanel* g_chatPanel = nullptr;

std::string ChatPanel::getRippyDir()
{
    std::string result;

    // Check settings first
    char dir[MAX_PATH] = {};
    if (BridgeSettingGet(CFG_SECTION, CFG_RIPPY_DIR, dir) && strlen(dir) > 0)
    {
        result = dir;
        if (result.back() != '\\' && result.back() != '/')
            result += "\\";
        result += ".rippy";
    }
    else
    {
        // Default: next to the x64dbg executable
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        result = exePath;
        auto lastSlash = result.find_last_of("\\/");
        if (lastSlash != std::string::npos)
            result = result.substr(0, lastSlash);
        result += "\\.rippy";
    }

    // Ensure directory structure exists
    CreateDirectoryA(result.c_str(), nullptr);
    CreateDirectoryA((result + "\\skills").c_str(), nullptr);
    CreateDirectoryA((result + "\\conversations").c_str(), nullptr);

    return result;
}

// --- Skill frontmatter parser ---

struct SkillInfo
{
    std::string command;
    std::string name;
    std::string description;
    std::string path; // full file path
    std::string body; // content after frontmatter
};

static bool parseSkillFile(const std::string& path, SkillInfo& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) { fclose(f); return false; }

    std::string content(size, '\0');
    fread(content.data(), 1, size, f);
    fclose(f);

    // Find frontmatter delimiters (---)
    size_t start = content.find("---");
    if (start == std::string::npos) return false;
    start += 3;
    // Skip past newline
    while (start < content.size() && (content[start] == '\r' || content[start] == '\n'))
        start++;

    size_t end = content.find("---", start);
    if (end == std::string::npos) return false;

    // Parse key: value pairs from frontmatter
    std::string fm = content.substr(start, end - start);
    std::istringstream ss(fm);
    std::string line;
    while (std::getline(ss, line))
    {
        // Strip \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // Trim whitespace
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());

        if (key == "command") out.command = val;
        else if (key == "name") out.name = val;
        else if (key == "description") out.description = val;
    }

    // Body is everything after the second ---
    size_t bodyStart = end + 3;
    while (bodyStart < content.size() && (content[bodyStart] == '\r' || content[bodyStart] == '\n'))
        bodyStart++;
    out.body = content.substr(bodyStart);
    out.path = path;

    return !out.command.empty() && !out.name.empty();
}

static std::vector<SkillInfo> g_skills;

static void scanSkillsFromDir(const std::string& rippyDir)
{
    g_skills.clear();
    std::string pattern = rippyDir + "\\skills\\*.md";

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string path = rippyDir + "\\skills\\" + fd.cFileName;
        SkillInfo skill;
        if (parseSkillFile(path, skill))
            g_skills.push_back(std::move(skill));
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    dprintf("Scanned %d skill(s) from %s\n", (int)g_skills.size(), pattern.c_str());
}

ChatPanel::~ChatPanel()
{
    shutdown();
}

bool ChatPanel::initialize()
{
    dputs("ChatPanel::initialize() starting...");

    // Create QWidget for the tab
    m_widget = new QWidget(nullptr);
    m_widget->setWindowTitle("Rippy");
    m_widget->setObjectName("RippyChat");
    m_widget->resize(800, 600);

    // Set tab icon from embedded resource
    HRSRC hIconRes = FindResource(hinst, MAKEINTRESOURCE(IDR_TAB_ICON), RT_RCDATA);
    if (hIconRes)
    {
        HGLOBAL hIconData = LoadResource(hinst, hIconRes);
        DWORD iconSize = SizeofResource(hinst, hIconRes);
        if (hIconData && iconSize)
        {
            const uchar* iconBytes = static_cast<const uchar*>(LockResource(hIconData));
            QPixmap pixmap;
            pixmap.loadFromData(iconBytes, iconSize, "PNG");
            m_widget->setWindowIcon(QIcon(pixmap));
        }
    }

    dputs("QWidget created, adding tab...");

    // Register as x64dbg tab
    GuiAddQWidgetTab(m_widget);

    // Get native HWND
    HWND hwnd = (HWND)m_widget->winId();
    if (!hwnd)
    {
        dputs("ERROR: Failed to get HWND from QWidget");
        return false;
    }

    dprintf("QWidget HWND: 0x%p\n", hwnd);

    // Subclass for WM_SIZE
    SetWindowSubclass(hwnd, SubclassProc, 1, (DWORD_PTR)this);

    // Initialize WebView2
    initWebView2(hwnd);
    return true;
}

void ChatPanel::initWebView2(HWND hwnd)
{
    dputs("initWebView2() starting...");

    // Determine user data directory
    wchar_t appData[MAX_PATH] = {};
    HRESULT shResult = SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    if (FAILED(shResult))
    {
        dprintf("ERROR: SHGetFolderPathW failed: 0x%08X\n", shResult);
        return;
    }

    std::wstring baseDir = std::wstring(appData) + L"\\x64dbg-rippy";

    // Clean up stale per-process WebView2 dirs from previous runs.
    // A dir is stale if its PID no longer exists.
    {
        WIN32_FIND_DATAW fd;
        std::wstring pattern = baseDir + L"\\WebView2_*";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                // Extract PID from "WebView2_12345"
                std::wstring name(fd.cFileName);
                auto pos = name.find(L'_');
                if (pos == std::wstring::npos)
                    continue;
                DWORD pid = 0;
                try { pid = std::stoul(name.substr(pos + 1)); }
                catch (...) { continue; }

                // Check if that process is still alive
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (hProc)
                {
                    CloseHandle(hProc);
                    continue; // still running, skip
                }

                // Dead process — remove its directory tree
                std::wstring staleDir = baseDir + L"\\" + name;
                // Use SHFileOperation for recursive delete
                std::wstring dirDoubleNull = staleDir + L'\0'; // needs double-null
                SHFILEOPSTRUCTW op = {};
                op.wFunc = FO_DELETE;
                op.pFrom = dirDoubleNull.c_str();
                op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
                SHFileOperationW(&op);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    // Use per-process directory to avoid locking conflicts with multiple x64dbg instances
    std::wstring userDataDir = baseDir + L"\\WebView2_"
        + std::to_wstring(GetCurrentProcessId());
    dprintf("WebView2 user data dir: %ls\n", userDataDir.c_str());

    // Verify the HWND is valid and visible
    RECT parentRect;
    GetClientRect(hwnd, &parentRect);
    dprintf("Parent HWND client rect: %ld x %ld\n",
        parentRect.right - parentRect.left,
        parentRect.bottom - parentRect.top);

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT
            {
                dprintf("WebView2 environment callback fired, hr=0x%08X, env=%p\n", result, env);

                if (FAILED(result) || !env)
                {
                    dprintf("ERROR: Failed to create WebView2 environment: 0x%08X\n", result);
                    return S_OK;
                }

                dputs("Creating WebView2 controller...");

                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT
                        {
                            dprintf("WebView2 controller callback fired, hr=0x%08X, controller=%p\n", result, controller);

                            if (FAILED(result) || !controller)
                            {
                                dprintf("ERROR: Failed to create WebView2 controller: 0x%08X\n", result);
                                return S_OK;
                            }

                            m_controller = controller;
                            m_controller->AddRef();

                            m_controller->get_CoreWebView2(&m_webview);

                            // Size to parent
                            RECT rc;
                            GetClientRect(hwnd, &rc);
                            dprintf("Setting WebView2 bounds: %ld x %ld\n",
                                rc.right - rc.left, rc.bottom - rc.top);
                            m_controller->put_Bounds(rc);
                            m_controller->put_IsVisible(TRUE);

                            // Register message handler (JS -> C++)
                            EventRegistrationToken token;
                            m_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2* sender,
                                           ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                                    {
                                        LPWSTR msgRaw = nullptr;
                                        args->TryGetWebMessageAsString(&msgRaw);
                                        if (msgRaw)
                                        {
                                            std::wstring msg(msgRaw);
                                            CoTaskMemFree(msgRaw);
                                            onWebMessage(msg);
                                        }
                                        return S_OK;
                                    }).Get(),
                                &token);

                            // Load the chat UI from embedded resource
                            HRESULT navHr = E_FAIL;
                            HRSRC hRes = FindResource(hinst, MAKEINTRESOURCE(IDR_CHAT_HTML), RT_RCDATA);
                            if (hRes)
                            {
                                HGLOBAL hData = LoadResource(hinst, hRes);
                                DWORD size = SizeofResource(hinst, hRes);
                                if (hData && size)
                                {
                                    const char* htmlUtf8 = static_cast<const char*>(LockResource(hData));
                                    int len = MultiByteToWideChar(CP_UTF8, 0, htmlUtf8, size, nullptr, 0);
                                    std::wstring whtml(len, 0);
                                    MultiByteToWideChar(CP_UTF8, 0, htmlUtf8, size, whtml.data(), len);
                                    navHr = m_webview->NavigateToString(whtml.c_str());
                                }
                            }
                            dprintf("NavigateToString result: 0x%08X\n", navHr);

                            // Send welcome message once navigation finishes
                            EventRegistrationToken navToken;
                            m_webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2* sender,
                                           ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT
                                    {
                                        m_webviewReady = true;
                                        sendWelcomeMessage();
                                        scanSkillsFromDir(getRippyDir());
                                        sendSkillsList();
                                        return S_OK;
                                    }).Get(),
                                &navToken);

                            dputs("WebView2 initialized successfully!");
                            return S_OK;
                        }).Get());

                return S_OK;
            }).Get());

    dprintf("CreateCoreWebView2EnvironmentWithOptions returned: 0x%08X\n", hr);

    if (FAILED(hr))
    {
        dprintf("ERROR: CreateCoreWebView2EnvironmentWithOptions failed: 0x%08X\n", hr);
    }
}

void ChatPanel::destroyWebView()
{
    m_webviewReady = false;

    if (m_controller)
    {
        m_controller->Close();
        m_controller->Release();
        m_controller = nullptr;
    }
    if (m_webview)
    {
        m_webview->Release();
        m_webview = nullptr;
    }
}

void ChatPanel::shutdown()
{
    dputs("ChatPanel::shutdown()");

    // Remove subclass FIRST — prevents SubclassProc from firing
    // with a stale m_controller during teardown
    if (m_widget)
    {
        HWND hwnd = (HWND)m_widget->winId();
        if (hwnd)
            RemoveWindowSubclass(hwnd, SubclassProc, 1);
    }

    destroyWebView();

    // Don't delete the QWidget or call GuiCloseQWidgetTab here —
    // during x64dbg shutdown the GUI is already being torn down and
    // these calls can crash. x64dbg owns the tab lifecycle; it will
    // destroy plugin widgets as part of its own cleanup.
    m_widget = nullptr;
}

void ChatPanel::showTab()
{
    if (!m_widget)
    {
        // Widget was destroyed — re-create everything
        if (!initialize())
        {
            dputs("Failed to re-initialize chat panel");
            return;
        }
    }
    else
    {
        // Widget still exists but tab may have been closed —
        // re-add it so it's registered in the tab bar again.
        // GuiAddQWidgetTab is safe to call if already added.
        GuiAddQWidgetTab(m_widget);
    }
    GuiShowQWidgetTab(m_widget);
}

void ChatPanel::refreshStatus()
{
    sendWelcomeMessage();
    sendSkillsList();
}

void ChatPanel::rescanSkills()
{
    scanSkillsFromDir(getRippyDir());
    sendSkillsList();
}

void ChatPanel::sendSkillsList()
{
    json skills = json::array();
    for (const auto& s : g_skills)
    {
        skills.push_back({
            {"command", s.command},
            {"name", s.name},
            {"description", s.description}
        });
    }
    json msg = {{"type", "skills_list"}, {"skills", skills}};
    postToJS(msg.dump());
}

void ChatPanel::loadSkill(const std::string& command)
{
    for (const auto& s : g_skills)
    {
        if (s.command == command)
        {
            m_activeSkill = s.body;
            json msg = {{"type", "info"}, {"message", "loaded skill: " + s.name}};
            postToJS(msg.dump());
            return;
        }
    }

    json msg = {{"type", "error"}, {"message", "skill not found: " + command}};
    postToJS(msg.dump());
}

void ChatPanel::saveConversation()
{
    std::string rippyDir = getRippyDir();
    std::string saveDir = rippyDir + "\\conversations";
    CreateDirectoryA(rippyDir.c_str(), nullptr);
    CreateDirectoryA(saveDir.c_str(), nullptr);

    // Generate filename with timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    char filename[128] = {};
    snprintf(filename, sizeof(filename), "rippy_%04d%02d%02d_%02d%02d%02d.md",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::string fullPath = saveDir + "\\" + filename;

    // Build markdown from history
    std::string md = "# Rippy Conversation\n\n";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& msg : m_history)
        {
            std::string role = msg.value("role", "");
            const auto& content = msg["content"];

            if (role == "user")
            {
                if (content.is_string())
                {
                    md += "## User\n\n" + content.get<std::string>() + "\n\n";
                }
                else if (content.is_array())
                {
                    // Tool results
                    for (const auto& block : content)
                    {
                        if (block.value("type", "") == "tool_result")
                        {
                            md += "**Tool Result** (`" + block.value("tool_use_id", "") + "`):\n";
                            md += "```\n" + block.value("content", "") + "\n```\n\n";
                        }
                    }
                }
            }
            else if (role == "assistant")
            {
                md += "## Assistant\n\n";
                if (content.is_array())
                {
                    for (const auto& block : content)
                    {
                        std::string blockType = block.value("type", "");
                        if (blockType == "text")
                            md += block.value("text", "") + "\n\n";
                        else if (blockType == "tool_use")
                        {
                            md += "**Tool Call**: `" + block.value("name", "") + "`\n";
                            md += "```json\n" + block.value("input", json::object()).dump(2) + "\n```\n\n";
                        }
                    }
                }
                else if (content.is_string())
                {
                    md += content.get<std::string>() + "\n\n";
                }
            }
        }
    }

    // Write to file
    FILE* f = fopen(fullPath.c_str(), "wb");
    if (f)
    {
        fwrite(md.data(), 1, md.size(), f);
        fclose(f);
        json msg = {{"type", "info"}, {"message", "saved to " + fullPath}};
        postToJS(msg.dump());
    }
    else
    {
        json msg = {{"type", "error"}, {"message", "failed to save: " + fullPath}};
        postToJS(msg.dump());
    }
}

void ChatPanel::sendWelcomeMessage()
{
    char apiKey[512] = {};
    bool hasKey = BridgeSettingGet(CFG_SECTION, CFG_API_KEY, apiKey) && strlen(apiKey) > 0;

    if (hasKey)
    {
        char provider[64] = {};
        char model[256] = {};
        BridgeSettingGet(CFG_SECTION, CFG_PROVIDER, provider);
        BridgeSettingGet(CFG_SECTION, CFG_MODEL, model);

        std::string welcome = "rippy ready. provider: ";
        welcome += (strlen(provider) > 0) ? provider : PROVIDER_ANTHROPIC;
        welcome += ", model: ";
        welcome += (strlen(model) > 0) ? model : "default";

        json msg = {{"type", "info"}, {"message", welcome}};
        postToJS(msg.dump());
    }
    else
    {
        json msg = {{"type", "setup_needed"}};
        postToJS(msg.dump());
    }
}

void ChatPanel::onWebMessage(const std::wstring& jsonMsg)
{
    // Convert wstring to UTF-8 string
    int len = WideCharToMultiByte(CP_UTF8, 0, jsonMsg.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, jsonMsg.c_str(), -1, utf8.data(), len, nullptr, nullptr);

    try
    {
        auto msg = json::parse(utf8);
        std::string type = msg["type"].get<std::string>();

        if (type == "send_message")
        {
            std::string content = msg["content"].get<std::string>();

            if (m_requestInFlight)
                return;

            // Add user message to history
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_history.push_back({{"role", "user"}, {"content", content}});
            }

            // Echo user message to UI
            json userMsg = {{"type", "user_message"}, {"content", content}};
            postToJS(userMsg.dump());

            // Show loading
            json loadingMsg = {{"type", "loading"}, {"visible", true}};
            postToJS(loadingMsg.dump());

            // Send to API on background thread
            sendChatAsync();
        }
        else if (type == "clear_history")
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_history.clear();
            m_activeSkill.clear();
        }
        else if (type == "load_skill")
        {
            std::string command = msg.value("command", "");
            if (!command.empty())
                loadSkill(command);
        }
        else if (type == "cancel")
        {
            if (m_requestInFlight)
            {
                m_cancelRequested = true;
                json msg = {{"type", "info"}, {"message", "cancelled."}};
                postToJS(msg.dump());
                json loadingMsg = {{"type", "loading"}, {"visible", false}};
                postToJS(loadingMsg.dump());
            }
        }
        else if (type == "save_conversation")
        {
            saveConversation();
        }
        else if (type == "open_settings")
        {
            // Open the settings dialog — reuse the same dialog proc from plugin.cpp
            extern void openSettingsDialog();
            openSettingsDialog();
        }
    }
    catch (const std::exception& e)
    {
        dprintf("Error parsing web message: %s\n", e.what());
    }
}

void ChatPanel::postToJS(const std::string& jsonStr)
{
    if (!m_webview || !m_webviewReady)
        return;

    int len = MultiByteToWideChar(CP_UTF8, 0, jsonStr.c_str(), -1, nullptr, 0);
    std::wstring wjson(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, jsonStr.c_str(), -1, wjson.data(), len);

    m_webview->PostWebMessageAsString(wjson.c_str());
}

// Build a dynamic system prompt with debugger state context.
static std::string buildSystemPrompt(const std::string& activeSkill = "")
{
    std::string prompt =
        "You are rippy, an AI reverse engineering assistant embedded in x64dbg. "
        "You have tools to interact with the debugger — use them to inspect state "
        "before answering questions about the program being debugged. "
        "Addresses and numeric values should be displayed in hexadecimal. "
        "Be concise and direct.\n\n"
        "IMPORTANT: Most debugger actions (breakpoints, stepping, memory writes, labels, tracing, etc.) "
        "should be done via execute_command. If you are unsure of exact command syntax, "
        "ALWAYS call get_command_help first to look it up — do not guess. "
        "The dedicated tools (read_memory, disassemble, get_registers, etc.) exist only for "
        "operations that return structured data the command interface cannot provide.";

    if (DbgIsDebugging())
    {
        prompt += "\n\nDebugger state: active";
        if (DbgIsRunning())
        {
            prompt += ", debuggee is running.";
        }
        else
        {
            prompt += ", debuggee is paused.";
            char mod[MAX_MODULE_SIZE] = {};
            duint cip = DbgEval("cip");
            if (DbgGetModuleAt(cip, mod))
            {
                char buf[64] = {};
                snprintf(buf, sizeof(buf), " CIP=0x%llX", (unsigned long long)cip);
                prompt += buf;
                prompt += " in module ";
                prompt += mod;
            }
        }
    }
    else
    {
        prompt += "\n\nDebugger state: no target loaded.";
    }

    if (!activeSkill.empty())
    {
        prompt += "\n\n--- ACTIVE SKILL ---\n\n";
        prompt += activeSkill;
    }

    return prompt;
}

// Extract tool_use blocks from a normalized assistant message.
struct ToolCall
{
    std::string id;
    std::string name;
    json input;
};

static std::vector<ToolCall> extractToolCalls(const json& assistantMsg)
{
    std::vector<ToolCall> calls;
    const auto& content = assistantMsg["content"];
    if (!content.is_array()) return calls;

    for (const auto& block : content)
    {
        if (block.contains("type") && block["type"] == "tool_use")
        {
            calls.push_back({
                block["id"].get<std::string>(),
                block["name"].get<std::string>(),
                block["input"]
            });
        }
    }
    return calls;
}

// Context for marshaling tool execution to GUI thread.
struct ToolExecCtx
{
    std::string name;
    json input;
    json result;
    HANDLE hEvent;
};

void ChatPanel::sendChatAsync()
{
    m_requestInFlight = true;
    m_cancelRequested = false;

    // Read config from settings
    ApiConfig config;
    char buf[1024] = {};

    if (BridgeSettingGet(CFG_SECTION, CFG_PROVIDER, buf))
        config.provider = buf;
    else
        config.provider = PROVIDER_ANTHROPIC;

    memset(buf, 0, sizeof(buf));
    if (BridgeSettingGet(CFG_SECTION, CFG_API_KEY, buf))
        config.apiKey = buf;

    memset(buf, 0, sizeof(buf));
    if (BridgeSettingGet(CFG_SECTION, CFG_ENDPOINT, buf))
        config.endpoint = buf;
    else
        config.endpoint = (config.provider == PROVIDER_ANTHROPIC)
            ? DEFAULT_ANTHROPIC_ENDPOINT
            : DEFAULT_OPENAI_ENDPOINT;

    memset(buf, 0, sizeof(buf));
    if (BridgeSettingGet(CFG_SECTION, CFG_MODEL, buf))
        config.model = buf;
    else
        config.model = (config.provider == PROVIDER_ANTHROPIC)
            ? DEFAULT_ANTHROPIC_MODEL
            : DEFAULT_OPENAI_MODEL;

    // Snapshot history and active skill
    json historySnapshot;
    std::string skillSnapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        historySnapshot = json(m_history);
        skillSnapshot = m_activeSkill;
    }

    // Spawn background thread with tool loop
    std::thread([this, config = std::move(config),
                 historySnapshot = std::move(historySnapshot),
                 skillSnapshot = std::move(skillSnapshot)]() mutable
    {
        json messages = std::move(historySnapshot);
        json tools = getToolDefinitions();
        std::string systemPrompt = buildSystemPrompt(skillSnapshot);
        std::string error;
        json finalAssistantMsg;

        constexpr int MAX_TOOL_ROUNDS = 25;

        try
        {
            for (int round = 0; round < MAX_TOOL_ROUNDS; round++)
            {
                if (m_cancelRequested) break;

                json raw = sendChatRaw(config, messages, tools, systemPrompt);
                json assistantMsg = normalizeResponse(config.provider, raw);
                messages.push_back(assistantMsg);

                auto calls = extractToolCalls(assistantMsg);
                if (calls.empty())
                {
                    // Final text response
                    finalAssistantMsg = std::move(assistantMsg);
                    break;
                }

                // Post tool calls to frontend for display
                {
                    json uiCalls = json::array();
                    for (const auto& tc : calls)
                        uiCalls.push_back({{"id", tc.id}, {"name", tc.name}, {"args", tc.input}});

                    auto* jsMsg = new std::string(json({{"type", "tool_calls"}, {"calls", uiCalls}}).dump());
                    GuiExecuteOnGuiThreadEx([](void* ud) {
                        auto* s = static_cast<std::string*>(ud);
                        if (g_chatPanel) g_chatPanel->postToJS(*s);
                        delete s;
                    }, jsMsg);
                }

                // Execute each tool on GUI thread
                json toolResults = json::array();
                for (const auto& tc : calls)
                {
                    auto* ctx = new ToolExecCtx{tc.name, tc.input, {}, CreateEvent(nullptr, FALSE, FALSE, nullptr)};

                    GuiExecuteOnGuiThreadEx([](void* ud) {
                        auto* c = static_cast<ToolExecCtx*>(ud);
                        try
                        {
                            c->result = executeTool(c->name, c->input);
                        }
                        catch (const std::exception& e)
                        {
                            c->result = {{"error", e.what()}};
                        }
                        SetEvent(c->hEvent);
                    }, ctx);

                    WaitForSingleObject(ctx->hEvent, 30000);
                    CloseHandle(ctx->hEvent);

                    std::string resultStr = ctx->result.dump();
                    if (resultStr.size() > 100000)
                        resultStr = json({{"error", "result truncated (>" + std::to_string(resultStr.size()) + " bytes)"}}).dump();
                    toolResults.push_back({
                        {"type", "tool_result"},
                        {"tool_use_id", tc.id},
                        {"content", resultStr}
                    });
                    delete ctx;
                }

                if (m_cancelRequested) break;

                // Add tool results as user message
                messages.push_back({{"role", "user"}, {"content", toolResults}});

                // Post tool results to frontend
                {
                    json uiResults = json::array();
                    for (const auto& tr : toolResults)
                        uiResults.push_back({{"id", tr["tool_use_id"]}, {"output", tr["content"]}});

                    auto* jsMsg = new std::string(json({{"type", "tool_results"}, {"results", uiResults}}).dump());
                    GuiExecuteOnGuiThreadEx([](void* ud) {
                        auto* s = static_cast<std::string*>(ud);
                        if (g_chatPanel) g_chatPanel->postToJS(*s);
                        delete s;
                    }, jsMsg);
                }
            }
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }

        // Post final result to GUI thread
        auto* result = new std::pair<json, std::string>(
            std::make_pair(std::move(messages), std::move(error)));
        auto* assistantCopy = new json(std::move(finalAssistantMsg));

        struct FinalResult { std::pair<json, std::string>* msgs; json* assistant; };
        auto* fr = new FinalResult{result, assistantCopy};

        GuiExecuteOnGuiThreadEx(
            [](void* userData)
            {
                auto* fr = static_cast<FinalResult*>(userData);
                auto* result = fr->msgs;
                auto* assistant = fr->assistant;

                if (!g_chatPanel)
                {
                    delete result;
                    delete assistant;
                    delete fr;
                    return;
                }

                if (g_chatPanel->m_cancelRequested)
                {
                    // Cancelled — discard partial conversation, already notified frontend
                }
                else if (result->second.empty() && !assistant->is_null())
                {
                    // Success — replace history with full conversation
                    {
                        std::lock_guard<std::mutex> lock(g_chatPanel->m_mutex);
                        g_chatPanel->m_history.clear();
                        for (const auto& msg : result->first)
                            g_chatPanel->m_history.push_back(msg);
                    }
                    std::string text = extractTextContent(*assistant);
                    json msg = {{"type", "assistant_message"}, {"content", text}};
                    g_chatPanel->postToJS(msg.dump());
                }
                else if (!result->second.empty())
                {
                    // Error
                    json msg = {{"type", "error"}, {"message", result->second}};
                    g_chatPanel->postToJS(msg.dump());
                }

                // Hide loading
                json loadingMsg = {{"type", "loading"}, {"visible", false}};
                g_chatPanel->postToJS(loadingMsg.dump());

                g_chatPanel->m_requestInFlight = false;
                delete result;
                delete assistant;
                delete fr;
            },
            fr);
    }).detach();
}

LRESULT CALLBACK ChatPanel::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData)
{
    auto* panel = reinterpret_cast<ChatPanel*>(refData);

    if (msg == WM_SIZE && panel->m_controller)
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        panel->m_controller->put_Bounds(rc);
    }
    else if (msg == WM_NCDESTROY)
    {
        // Tab was closed by the user — clean up WebView2 and null the widget
        // so showTab() knows to re-create it
        dputs("ChatPanel: tab closed (WM_NCDESTROY)");
        RemoveWindowSubclass(hwnd, SubclassProc, subclassId);
        panel->destroyWebView();
        panel->m_widget = nullptr;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
