import { h, Fragment, render } from 'preact';
import { useState, useEffect, useRef, useCallback } from 'preact/hooks';
import './styles.css';
import DOMPurify from 'dompurify';
import { marked } from 'marked';

marked.setOptions({ breaks: true, gfm: true });

// --- Utilities ---

function sanitize(html) {
    return DOMPurify.sanitize(html, {
        ALLOWED_TAGS: [
            'b', 'i', 'em', 'strong', 'code', 'pre', 'br', 'p', 'span',
            'div', 'ul', 'ol', 'li', 'a', 'h1', 'h2', 'h3', 'h4',
            'table', 'thead', 'tbody', 'tr', 'th', 'td',
            'blockquote', 'hr', 'img', 'sub', 'sup', 'del',
        ],
        ALLOWED_ATTR: ['class', 'href', 'src', 'alt', 'title', 'style'],
        ALLOW_DATA_ATTR: false,
    });
}

function renderMarkdown(text) {
    return sanitize(marked.parse(text));
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// --- Widget registry ---

const widgets = {};

function registerWidget(name, renderFn) {
    widgets[name] = renderFn;
}

window.rippy = { registerWidget };

window.openSettings = function () {
    window.chrome.webview.postMessage(JSON.stringify({ type: 'open_settings' }));
};

// --- Early message buffer ---

const earlyMessages = [];
let onMessageHandler = null;

window.chrome.webview.addEventListener('message', function (event) {
    const msg = JSON.parse(event.data);
    if (onMessageHandler) {
        onMessageHandler(msg);
    } else {
        earlyMessages.push(msg);
    }
});

// --- Slash commands ---

const BUILTIN_COMMANDS = [
    { name: 'help', description: 'show available commands', builtin: true },
    { name: 'clear', description: 'clear chat and start a new session', builtin: true },
    { name: 'reload', description: 'reload RIPPY.md, skills, and start fresh', builtin: true },
    { name: 'save', description: 'save conversation as markdown', builtin: true },
    { name: 'skills', description: 'list available skills', builtin: true },
    { name: 'settings', description: 'open settings dialog', builtin: true },
];

// --- Components ---

function Banner() {
    const text = 'RIPPY';
    const colors = ['#ff5555', '#ffaa00', '#55ff55', '#55ddff', '#aa77ff'];
    return (
        <div class="entry">
            {text.split('').map((c, i) => (
                <span key={i} style={{ color: colors[i], fontWeight: 'bold' }}>{c}</span>
            ))}
        </div>
    );
}

function WidgetEntry({ msg }) {
    const ref = useRef(null);

    useEffect(() => {
        if (!ref.current) return;
        const el = widgets[msg.widget](msg);
        if (el instanceof HTMLElement) {
            ref.current.innerHTML = '';
            ref.current.appendChild(el);
        } else {
            ref.current.innerHTML = sanitize(String(el));
        }
    }, [msg]);

    return (
        <div class="entry">
            <div class="response" ref={ref} />
        </div>
    );
}

function ToolCallEntry({ msg }) {
    const [expanded, setExpanded] = useState({});

    const toggle = useCallback((id) => {
        setExpanded(prev => ({ ...prev, [id]: !prev[id] }));
    }, []);

    const calls = msg.calls || [];
    const resultsMap = {};
    if (msg.results) {
        for (const r of msg.results) {
            resultsMap[r.id] = r.output;
        }
    }

    return (
        <div class="entry">
            {calls.map(call => {
                const isOpen = expanded[call.id];
                const result = resultsMap[call.id];
                const argSummary = Object.entries(call.args || {})
                    .map(([k, v]) => k + '=' + JSON.stringify(v))
                    .join(', ');
                return (
                    <div class="tool-call" key={call.id}>
                        <div class="tool-call-header" onClick={() => toggle(call.id)}>
                            <span class="tool-call-arrow">{isOpen ? '\u25BC' : '\u25B6'}</span>
                            <span class="tool-call-name">{call.name}</span>
                            {!isOpen && argSummary && (
                                <span class="tool-call-args-summary">{argSummary}</span>
                            )}
                        </div>
                        {isOpen && (
                            <div class="tool-call-body">
                                <div class="tool-call-args">{JSON.stringify(call.args, null, 2)}</div>
                                {result != null && (
                                    <pre class="tool-call-result">{typeof result === 'string' ? result : JSON.stringify(result, null, 2)}</pre>
                                )}
                            </div>
                        )}
                        {!isOpen && result != null && (
                            <pre class="tool-call-result-preview" onClick={() => toggle(call.id)}>
                                {(typeof result === 'string' ? result : JSON.stringify(result)).slice(0, 120)}
                            </pre>
                        )}
                    </div>
                );
            })}
        </div>
    );
}

function PermissionPrompt({ msg, onRespond }) {
    const [responded, setResponded] = useState(false);
    const [choice, setChoice] = useState(null);

    const respond = useCallback((allowed) => {
        if (responded) return;
        setResponded(true);
        setChoice(allowed);
        onRespond(msg.id, allowed);
    }, [responded, msg.id, onRespond]);

    const label = msg.action === 'read' ? 'read file' : 'list directory';

    if (responded) {
        return (
            <div class="entry">
                <div class={'permission-resolved ' + (choice ? 'allowed' : 'denied')}>
                    {choice ? 'allowed' : 'denied'}: {label} {msg.path}
                </div>
            </div>
        );
    }

    return (
        <div class="entry">
            <div class="permission-prompt">
                <span class="permission-text">rippy wants to {label}: <strong>{msg.path}</strong></span>
                <span class="permission-buttons">
                    <a class="permission-allow" onClick={() => respond(true)}>allow</a>
                    <a class="permission-deny" onClick={() => respond(false)}>deny</a>
                </span>
            </div>
        </div>
    );
}

function MessageEntry({ msg, onPermissionRespond }) {
    switch (msg.type) {
        case 'user_message':
            return (
                <div class="entry msg-user">
                    <div class="msg-body">{msg.content}</div>
                </div>
            );

        case 'assistant_message':
            if (msg.widget && widgets[msg.widget]) {
                return <WidgetEntry msg={msg} />;
            }
            return (
                <div class="entry msg-assistant">
                    <div class="msg-body" dangerouslySetInnerHTML={{ __html: renderMarkdown(msg.content) }} />
                </div>
            );

        case 'error':
            return (
                <div class="entry">
                    <div class="error-text">{msg.message}</div>
                </div>
            );

        case 'info':
            return (
                <div class="entry">
                    <div class="info-text">{msg.message}</div>
                </div>
            );

        case 'setup_needed':
            return (
                <div class="entry">
                    <div class="setup-text" dangerouslySetInnerHTML={{
                        __html: 'no api key configured. <a onclick="openSettings()">open settings</a> to get started.'
                    }} />
                </div>
            );

        case 'tool_calls':
            return <ToolCallEntry msg={msg} />;

        case 'permission_request':
            return <PermissionPrompt msg={msg} onRespond={onPermissionRespond} />;

        default:
            return null;
    }
}

function Loading({ visible, onStop }) {
    return (
        <div id="loading" class={visible ? 'visible' : ''}>
            thinking<span class="dots" />
            {' '}<a class="stop-link" onClick={onStop}>stop</a>
        </div>
    );
}

function SlashMenu({ filter, selectedIndex, onSelect, commands }) {
    const matches = (commands || []).filter(cmd =>
        cmd.name.startsWith(filter)
    );

    if (matches.length === 0) return null;

    return (
        <div class="slash-menu">
            {matches.map((cmd, i) => (
                <div
                    key={cmd.name}
                    class={'slash-menu-item' + (i === selectedIndex ? ' selected' : '')}
                    onMouseDown={(e) => { e.preventDefault(); onSelect(cmd.name); }}
                >
                    <span class="slash-menu-name">/{cmd.name}</span>
                    <span class="slash-menu-desc">{cmd.description}</span>
                </div>
            ))}
        </div>
    );
}

function InputArea({ disabled, onSend, onSlashCommand, commands }) {
    const [value, setValue] = useState('');
    const [slashFilter, setSlashFilter] = useState(null); // null = menu hidden
    const [selectedIndex, setSelectedIndex] = useState(0);
    const textareaRef = useRef(null);

    const autoResize = useCallback(() => {
        const el = textareaRef.current;
        if (!el) return;
        el.style.height = 'auto';
        el.style.height = Math.min(el.scrollHeight, 120) + 'px';
    }, []);

    const getFilteredCommands = useCallback((filter) => {
        return (commands || []).filter(cmd => cmd.name.startsWith(filter || ''));
    }, [commands]);

    const acceptCompletion = useCallback((name) => {
        setValue('/' + name);
        setSlashFilter(null);
        setSelectedIndex(0);
        // Focus back on textarea
        if (textareaRef.current) textareaRef.current.focus();
    }, []);

    const send = useCallback(() => {
        const text = value.trim();
        if (!text) return;
        setValue('');
        setSlashFilter(null);
        setSelectedIndex(0);

        // Check for slash command
        if (text.startsWith('/')) {
            const cmdName = text.slice(1).split(/\s/)[0];
            onSlashCommand(cmdName);
        } else {
            onSend(text);
        }
        setTimeout(() => autoResize(), 0);
    }, [value, onSend, onSlashCommand, autoResize]);

    const onInput = useCallback((e) => {
        const val = e.target.value;
        setValue(val);
        autoResize();

        // Update slash menu state
        if (val.startsWith('/') && !val.includes(' ')) {
            const filter = val.slice(1);
            setSlashFilter(filter);
            setSelectedIndex(0);
        } else {
            setSlashFilter(null);
        }
    }, [autoResize]);

    const onKeyDown = useCallback((e) => {
        // Slash menu navigation
        if (slashFilter !== null) {
            const matches = getFilteredCommands(slashFilter);
            if (e.key === 'ArrowDown') {
                e.preventDefault();
                setSelectedIndex(prev => Math.min(prev + 1, matches.length - 1));
                return;
            }
            if (e.key === 'ArrowUp') {
                e.preventDefault();
                setSelectedIndex(prev => Math.max(prev - 1, 0));
                return;
            }
            if (e.key === 'Tab') {
                e.preventDefault();
                if (matches.length > 0) {
                    const idx = Math.min(selectedIndex, matches.length - 1);
                    acceptCompletion(matches[idx].name);
                }
                return;
            }
            if (e.key === 'Escape') {
                e.preventDefault();
                setSlashFilter(null);
                return;
            }
        }

        if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            // If slash menu is open and an item is selected, complete it first
            if (slashFilter !== null) {
                const matches = getFilteredCommands(slashFilter);
                if (matches.length > 0) {
                    const idx = Math.min(selectedIndex, matches.length - 1);
                    // If typed exactly matches a command, send it
                    const exactMatch = matches.find(m => m.name === slashFilter);
                    if (exactMatch) {
                        setValue('');
                        setSlashFilter(null);
                        setSelectedIndex(0);
                        onSlashCommand(exactMatch.name);
                        setTimeout(() => autoResize(), 0);
                        return;
                    }
                    // Otherwise autocomplete
                    acceptCompletion(matches[idx].name);
                    return;
                }
            }
            send();
        }
    }, [slashFilter, selectedIndex, getFilteredCommands, acceptCompletion, send, onSlashCommand, autoResize]);

    return (
        <div id="input-area">
            {slashFilter !== null && (
                <SlashMenu
                    filter={slashFilter}
                    selectedIndex={selectedIndex}
                    commands={commands}
                    onSelect={(name) => {
                        setValue('');
                        setSlashFilter(null);
                        setSelectedIndex(0);
                        onSlashCommand(name);
                    }}
                />
            )}
            <textarea
                ref={textareaRef}
                id="input"
                rows="1"
                placeholder="ask rippy... (/ for commands)"
                autofocus
                disabled={disabled}
                value={value}
                onInput={onInput}
                onKeyDown={onKeyDown}
            />
        </div>
    );
}

function App() {
    const [messages, setMessages] = useState([]);
    const [isLoading, setIsLoading] = useState(false);
    const [hasApiKey, setHasApiKey] = useState(false);
    const [skills, setSkills] = useState([]);
    const outputRef = useRef(null);

    // Merge built-in commands + dynamic skills for autocomplete
    const allCommands = [...BUILTIN_COMMANDS, ...skills.map(s => ({
        name: s.command, description: s.description, builtin: false
    }))];

    // Auto-scroll when messages change or loading toggles
    useEffect(() => {
        if (outputRef.current) {
            outputRef.current.scrollTop = outputRef.current.scrollHeight;
        }
    }, [messages, isLoading]);

    // WebView2 message dispatch
    useEffect(() => {
        function dispatch(msg) {
            switch (msg.type) {
                case 'user_message':
                    setMessages(prev => [...prev, msg]);
                    break;
                case 'assistant_message':
                    setMessages(prev => [...prev, msg]);
                    setIsLoading(false);
                    break;
                case 'loading':
                    setIsLoading(msg.visible);
                    break;
                case 'error':
                    setMessages(prev => [...prev, msg]);
                    setIsLoading(false);
                    break;
                case 'info':
                    setMessages(prev => [...prev, msg]);
                    setHasApiKey(true);
                    break;
                case 'setup_needed':
                    setMessages(prev => [...prev, msg]);
                    setHasApiKey(false);
                    break;
                case 'tool_calls':
                    setMessages(prev => [...prev, msg]);
                    break;
                case 'skills_list':
                    setSkills(msg.skills || []);
                    break;
                case 'permission_request':
                    setMessages(prev => [...prev, msg]);
                    break;
                case 'tool_results':
                    setMessages(prev => {
                        const updated = [...prev];
                        for (let i = updated.length - 1; i >= 0; i--) {
                            if (updated[i].type === 'tool_calls') {
                                updated[i] = { ...updated[i], results: msg.results };
                                break;
                            }
                        }
                        return updated;
                    });
                    break;
            }
        }

        earlyMessages.forEach(dispatch);
        earlyMessages.length = 0;
        onMessageHandler = dispatch;
        return () => { onMessageHandler = null; };
    }, []);

    const handleSend = useCallback((text) => {
        window.chrome.webview.postMessage(JSON.stringify({
            type: 'send_message',
            content: text,
        }));
    }, []);

    const handleStop = useCallback(() => {
        window.chrome.webview.postMessage(JSON.stringify({ type: 'cancel' }));
    }, []);

    const handlePermissionRespond = useCallback((id, allowed) => {
        window.chrome.webview.postMessage(JSON.stringify({
            type: 'permission_response',
            id: id,
            allowed: allowed,
        }));
    }, []);

    const handleSlashCommand = useCallback((cmd) => {
        switch (cmd) {
            case 'help': {
                let helpText =
                    '/help — show this message\n' +
                    '/clear — clear chat and start a new session\n' +
                    '/reload — reload RIPPY.md, skills, and clear chat\n' +
                    '/save — save conversation as markdown\n' +
                    '/skills — list available skills\n' +
                    '/settings — open settings dialog';
                if (skills.length > 0) {
                    helpText += '\n\nskills:';
                    for (const s of skills) {
                        helpText += '\n/' + s.command + ' — ' + s.description;
                    }
                }
                setMessages(prev => [...prev, { type: 'info', message: helpText }]);
                break;
            }
            case 'clear':
                window.chrome.webview.postMessage(JSON.stringify({ type: 'clear_history' }));
                setMessages([]);
                break;
            case 'reload':
                window.chrome.webview.postMessage(JSON.stringify({ type: 'reload' }));
                setMessages([]);
                break;
            case 'save':
                window.chrome.webview.postMessage(JSON.stringify({ type: 'save_conversation' }));
                break;
            case 'skills': {
                if (skills.length === 0) {
                    setMessages(prev => [...prev, { type: 'info', message: 'no skills found. add .md files to .rippy/skills/' }]);
                } else {
                    let text = 'available skills:';
                    for (const s of skills) {
                        text += '\n  /' + s.command + ' — ' + s.description;
                    }
                    setMessages(prev => [...prev, { type: 'info', message: text }]);
                }
                break;
            }
            case 'settings':
                window.chrome.webview.postMessage(JSON.stringify({ type: 'open_settings' }));
                break;
            default:
                // Try as a skill command
                window.chrome.webview.postMessage(JSON.stringify({ type: 'load_skill', command: cmd }));
        }
    }, [skills]);

    return (
        <Fragment>
            <div id="output" ref={outputRef}>
                <Banner />
                {messages.map((msg, i) => (
                    <MessageEntry key={i} msg={msg} onPermissionRespond={handlePermissionRespond} />
                ))}
            </div>
            <Loading visible={isLoading} onStop={handleStop} />
            <InputArea
                disabled={!hasApiKey || isLoading}
                onSend={handleSend}
                onSlashCommand={handleSlashCommand}
                commands={allCommands}
            />
        </Fragment>
    );
}

render(<App />, document.getElementById('app'));
