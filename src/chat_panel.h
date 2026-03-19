#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>

// Forward declarations — avoid pulling in Qt/WebView2 headers here
class QWidget;
struct ICoreWebView2Controller;
struct ICoreWebView2;
struct ICoreWebView2Environment;

class ChatPanel
{
public:
    ChatPanel() = default;
    ~ChatPanel();

    // Must be called on the GUI thread.
    bool initialize();
    void shutdown();
    void showTab();
    void refreshStatus();
    void rescanSkills();

    // Returns the .rippy directory path (from settings or auto-detected).
    static std::string getRippyDir();

private:
    QWidget* m_widget = nullptr;
    ICoreWebView2Controller* m_controller = nullptr;
    ICoreWebView2* m_webview = nullptr;

    std::vector<nlohmann::json> m_history;
    std::string m_activeSkill; // loaded skill body (appended to system prompt)
    std::mutex m_mutex;
    std::atomic<bool> m_requestInFlight{false};
    std::atomic<bool> m_cancelRequested{false};
    bool m_webviewReady = false;

    void initWebView2(HWND hwnd);
    void onWebMessage(const std::wstring& jsonMsg);
    void postToJS(const std::string& json);
    void sendChatAsync();
    void sendWelcomeMessage();
    void saveConversation();
    void loadSkill(const std::string& command);
    void sendSkillsList();
    void destroyWebView();

    static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR subclassId,
                                         DWORD_PTR refData);
};

// Global instance
extern ChatPanel* g_chatPanel;
