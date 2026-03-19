#include "tools.h"
#include "resource.h"
#include "pluginmain.h"

#include "pluginsdk/_scriptapi_memory.h"
#include "pluginsdk/_scriptapi_register.h"

#include <mutex>

using json = nlohmann::json;

// --- Command reference (loaded once from embedded resource) ---

static std::string g_commandRef;
static std::once_flag g_commandRefOnce;

static const std::string& getCommandRef()
{
    std::call_once(g_commandRefOnce, []() {
        HRSRC hRes = FindResource(hinst, MAKEINTRESOURCE(IDR_COMMAND_REF), RT_RCDATA);
        if (hRes)
        {
            HGLOBAL hData = LoadResource(hinst, hRes);
            DWORD size = SizeofResource(hinst, hRes);
            if (hData && size)
            {
                const char* data = static_cast<const char*>(LockResource(hData));
                g_commandRef.assign(data, size);
            }
        }
    });
    return g_commandRef;
}

// --- Tool implementations ---
// Each returns a json object. Must be called on the GUI thread.

static json tool_eval_expression(const json& input)
{
    std::string expr = input.value("expression", "");
    if (expr.empty())
        return {{"error", "expression required"}};
    bool ok = false;
    duint val = DbgEval(expr.c_str(), &ok);
    if (!ok)
        return {{"error", "failed to evaluate: " + expr}};

    char hexBuf[32] = {};
    snprintf(hexBuf, sizeof(hexBuf), "0x%llX", (unsigned long long)val);
    return {{"value", hexBuf}, {"decimal", (uint64_t)val}};
}

static json tool_read_memory(const json& input)
{
    std::string addrExpr = input.value("address", "");
    if (addrExpr.empty())
        return {{"error", "address required"}};
    int size = input.value("size", 64);
    if (size > 4096) size = 4096;

    bool ok = false;
    duint addr = DbgEval(addrExpr.c_str(), &ok);
    if (!ok)
        return {{"error", "failed to evaluate address: " + addrExpr}};

    std::vector<uint8_t> buf(size);
    duint bytesRead = 0;
    if (!Script::Memory::Read(addr, buf.data(), size, &bytesRead))
        return {{"error", "memory read failed"}};

    // Format as hex dump with ASCII
    std::string dump;
    for (duint i = 0; i < bytesRead; i += 16)
    {
        char line[128] = {};
        int pos = snprintf(line, sizeof(line), "%llX  ", (unsigned long long)(addr + i));

        for (int j = 0; j < 16 && (i + j) < bytesRead; j++)
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", buf[i + j]);

        for (duint j = bytesRead - i; j < 16; j++)
            pos += snprintf(line + pos, sizeof(line) - pos, "   ");

        pos += snprintf(line + pos, sizeof(line) - pos, " |");

        for (int j = 0; j < 16 && (i + j) < bytesRead && pos < (int)sizeof(line) - 2; j++)
        {
            uint8_t c = buf[i + j];
            line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        if (pos < (int)sizeof(line) - 1) line[pos++] = '|';
        line[pos] = 0;

        dump += line;
        dump += "\n";
    }

    return {{"dump", dump}, {"bytes_read", bytesRead}};
}

static json tool_disassemble(const json& input)
{
    std::string addrExpr = input.value("address", "");
    if (addrExpr.empty())
        return {{"error", "address required"}};
    int count = input.value("count", 10);
    if (count > 100) count = 100;

    bool ok = false;
    duint addr = DbgEval(addrExpr.c_str(), &ok);
    if (!ok)
        return {{"error", "failed to evaluate address: " + addrExpr}};

    std::string result;
    for (int i = 0; i < count; i++)
    {
        DISASM_INSTR instr = {};
        DbgDisasmAt(addr, &instr);
        if (instr.instr_size == 0) break;

        char line[256] = {};
        snprintf(line, sizeof(line), "%llX  %s\n",
            (unsigned long long)addr, instr.instruction);
        result += line;
        addr += instr.instr_size;
    }

    return {{"disassembly", result}};
}

static json tool_get_registers(const json&)
{
    if (!DbgIsDebugging() || DbgIsRunning())
        return {{"error", "debuggee is not paused"}};

    json regs;
#ifdef _WIN64
    char buf[32];
    auto fmtReg = [&](const char* name, duint val) {
        snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)val);
        regs[name] = buf;
    };
    fmtReg("RAX", Script::Register::GetRAX());
    fmtReg("RBX", Script::Register::GetRBX());
    fmtReg("RCX", Script::Register::GetRCX());
    fmtReg("RDX", Script::Register::GetRDX());
    fmtReg("RSI", Script::Register::GetRSI());
    fmtReg("RDI", Script::Register::GetRDI());
    fmtReg("RBP", Script::Register::GetRBP());
    fmtReg("RSP", Script::Register::GetRSP());
    fmtReg("RIP", Script::Register::GetRIP());
    fmtReg("R8",  Script::Register::GetR8());
    fmtReg("R9",  Script::Register::GetR9());
    fmtReg("R10", Script::Register::GetR10());
    fmtReg("R11", Script::Register::GetR11());
    fmtReg("R12", Script::Register::GetR12());
    fmtReg("R13", Script::Register::GetR13());
    fmtReg("R14", Script::Register::GetR14());
    fmtReg("R15", Script::Register::GetR15());
    fmtReg("RFLAGS", Script::Register::GetCFLAGS());
#else
    char buf[32];
    auto fmtReg = [&](const char* name, duint val) {
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned int)val);
        regs[name] = buf;
    };
    fmtReg("EAX", Script::Register::GetEAX());
    fmtReg("EBX", Script::Register::GetEBX());
    fmtReg("ECX", Script::Register::GetECX());
    fmtReg("EDX", Script::Register::GetEDX());
    fmtReg("ESI", Script::Register::GetESI());
    fmtReg("EDI", Script::Register::GetEDI());
    fmtReg("EBP", Script::Register::GetEBP());
    fmtReg("ESP", Script::Register::GetESP());
    fmtReg("EIP", Script::Register::GetEIP());
    fmtReg("EFLAGS", Script::Register::GetCFLAGS());
#endif
    return regs;
}

static json tool_get_memory_map(const json&)
{
    MEMMAP memmap = {};
    if (!DbgMemMap(&memmap))
        return {{"error", "failed to get memory map"}};

    std::string result;
    for (int i = 0; i < memmap.count && i < 500; i++)
    {
        const auto& page = memmap.page[i];
        char line[256] = {};
        snprintf(line, sizeof(line), "%llX  %llX  %08X  %s\n",
            (unsigned long long)page.mbi.BaseAddress,
            (unsigned long long)page.mbi.RegionSize,
            (unsigned int)page.mbi.Protect,
            page.info);
        result += line;
    }

    if (memmap.page)
        BridgeFree(memmap.page);

    return {{"map", result}, {"count", memmap.count}};
}

static json tool_get_symbol(const json& input)
{
    std::string addrExpr = input.value("address", "");
    if (addrExpr.empty())
        return {{"error", "address required"}};
    bool ok = false;
    duint addr = DbgEval(addrExpr.c_str(), &ok);
    if (!ok)
        return {{"error", "failed to evaluate address: " + addrExpr}};

    char label[MAX_LABEL_SIZE] = {};
    if (DbgGetLabelAt(addr, SEG_DEFAULT, label))
        return {{"symbol", label}};

    SYMBOLINFOCPP info;
    if (DbgGetSymbolInfoAt(addr, &info))
    {
        json result;
        if (info.undecoratedSymbol && strlen(info.undecoratedSymbol) > 0)
            result["symbol"] = info.undecoratedSymbol;
        else if (info.decoratedSymbol && strlen(info.decoratedSymbol) > 0)
            result["symbol"] = info.decoratedSymbol;
        else
            result["symbol"] = nullptr;
        return result;
    }

    return {{"symbol", nullptr}};
}

static json tool_execute_command(const json& input)
{
    std::string cmd = input.value("command", "");
    if (cmd.empty())
        return {{"error", "command required"}};
    bool ok = DbgCmdExecDirect(cmd.c_str());
    return {{"success", ok}};
}

static std::string toLower(const std::string& s)
{
    std::string out = s;
    for (auto& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

// Split reference into sections, cache the result.
struct Section
{
    std::string header;      // first line (lowercased)
    std::string headerRaw;   // first line (original)
    std::string body;        // full section text
    std::string bodyLower;   // full section text (lowercased)
};

static std::vector<Section> g_sections;
static std::once_flag g_sectionsOnce;

static const std::vector<Section>& getSections()
{
    std::call_once(g_sectionsOnce, []() {
        const std::string& ref = getCommandRef();
        if (ref.empty()) return;

        size_t pos = 0;
        while (pos < ref.size())
        {
            size_t hdr = ref.find("\n# ", pos);
            if (hdr == std::string::npos)
            {
                if (pos == 0 && ref.substr(0, 2) == "# ")
                    hdr = 0;
                else
                    break;
            }
            else
            {
                hdr++; // skip \n
            }

            size_t next = ref.find("\n# ", hdr + 1);
            if (next == std::string::npos) next = ref.size();
            else next++;

            std::string body = ref.substr(hdr, next - hdr);
            size_t eol = body.find('\n');
            std::string headerRaw = (eol != std::string::npos) ? body.substr(0, eol) : body;

            g_sections.push_back({toLower(headerRaw), headerRaw, body, toLower(body)});
            pos = next;
        }
    });
    return g_sections;
}

// Search the command reference for sections matching a query.
// Tokenizes query, scores by header match (priority) then body match.
static json tool_get_command_help(const json& input)
{
    std::string query = input.value("query", "");
    const auto& sections = getSections();
    if (sections.empty())
        return {{"error", "command reference not available"}};

    // Tokenize query into lowercase words
    std::string queryLower = toLower(query);
    std::vector<std::string> tokens;
    {
        size_t i = 0;
        while (i < queryLower.size())
        {
            while (i < queryLower.size() && (queryLower[i] == ' ' || queryLower[i] == ','))
                i++;
            size_t start = i;
            while (i < queryLower.size() && queryLower[i] != ' ' && queryLower[i] != ',')
                i++;
            if (i > start)
                tokens.push_back(queryLower.substr(start, i - start));
        }
    }

    if (tokens.empty())
        return {{"result", "provide a search term"}};

    // Score each section: header match = 10 per token, body match = 1 per token
    struct Match { int score; size_t idx; };
    std::vector<Match> matches;

    for (size_t i = 0; i < sections.size(); i++)
    {
        int score = 0;
        for (const auto& tok : tokens)
        {
            if (sections[i].header.find(tok) != std::string::npos)
                score += 10;
            else if (sections[i].bodyLower.find(tok) != std::string::npos)
                score += 1;
        }
        if (score > 0)
            matches.push_back({score, i});
    }

    // Sort by score descending
    std::sort(matches.begin(), matches.end(),
        [](const Match& a, const Match& b) { return a.score > b.score; });

    // Collect top results up to size limit
    std::string result;
    for (const auto& m : matches)
    {
        const auto& sec = sections[m.idx];
        if (result.size() + sec.body.size() > 8000)
        {
            result += "\n[... more results, refine your query]\n";
            break;
        }
        result += sec.body;
        result += "\n---\n";
    }

    if (result.empty())
        return {{"result", "no matching commands found for: " + query}};

    return {{"result", result}};
}

// --- Tool registry ---

static const char* EXECUTE_COMMAND_DESC =
    "Execute an x64dbg debugger command. This is the primary way to perform actions in the debugger. "
    "If you are unsure of exact command syntax, call get_command_help first.\n\n"
    "SYNTAX: Commands use comma-separated arguments. All numbers are HEX by default. "
    "Use quotes for arguments with spaces. Expressions (registers, math, symbols) work in arguments.\n\n"
    "COMMON COMMANDS:\n"
    "  Execution: run/go, StepInto/sti, StepOver/sto, StepOut, pause, skip\n"
    "  Breakpoints: bp/SetBPX addr, bc/DeleteBPX addr, bplist, bpe/EnableBPX addr, bpd/DisableBPX addr\n"
    "  Hardware BP: bph addr,r/w/x,[1/2/4/8], bphc addr\n"
    "  Memory: alloc [size], free addr, Fill addr,size,byte, memcpy dst,src,size\n"
    "  Registers: set reg=value (e.g. rax=0, rip=kernel32:CreateFileA)\n"
    "  Labels: lbl addr,text, lbldel addr\n"
    "  Comments: cmt addr,text, cmtdel addr\n"
    "  Tracing: TraceIntoConditional/ticnd cond, TraceOverConditional/tocnd cond\n"
    "  Threads: switchthread tid, suspendthread tid, resumethread tid, createthread addr\n"
    "  Search: find addr,hex_pattern, findall addr,hex_pattern, refstr addr,\"string\"\n"
    "  Analysis: analyse/analyze, symdownload, symload\n"
    "  GUI: disasm/dis/d addr, dump addr, graph addr, cls\n"
    "  Data: savedata \"file\",addr,size\n\n"
    "VALUES: Hex default (0x optional). Decimal with dot prefix (.123). "
    "Registers: rax,eax,al,r8-r15,cip,csp,cax etc. Flags: _zf,_cf,_sf etc. "
    "Memory: [addr], byte:[addr], dword:[addr]. "
    "Symbols: module:export, module:$rva, module:entry. "
    "Variables: $result, $lastalloc.";

json getToolDefinitions()
{
    return json::array({
        {
            {"name", "eval_expression"},
            {"description", "Evaluate an x64dbg expression and return its numeric value. "
                "Supports registers, math, symbols, memory dereference ([addr]), module exports (kernel32:CreateFileA), etc."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {{"expression", {{"type", "string"}, {"description", "Expression to evaluate (e.g. 'rip', 'rsp+8', '[rsp]', 'kernel32:CreateFileA')"}}}}},
                {"required", json::array({"expression"})}
            }}
        },
        {
            {"name", "read_memory"},
            {"description", "Read bytes from debuggee memory and return a hex dump with ASCII."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Address expression"}}},
                    {"size", {{"type", "integer"}, {"description", "Bytes to read (default 64, max 4096)"}}}
                }},
                {"required", json::array({"address"})}
            }}
        },
        {
            {"name", "disassemble"},
            {"description", "Disassemble instructions at an address."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Address expression"}}},
                    {"count", {{"type", "integer"}, {"description", "Number of instructions (default 10, max 100)"}}}
                }},
                {"required", json::array({"address"})}
            }}
        },
        {
            {"name", "get_registers"},
            {"description", "Get all general-purpose register values. Debuggee must be paused."},
            {"input_schema", {{"type", "object"}, {"properties", json::object()}}}
        },
        {
            {"name", "get_memory_map"},
            {"description", "Get the memory map of the debuggee (base, size, protection, info for each region)."},
            {"input_schema", {{"type", "object"}, {"properties", json::object()}}}
        },
        {
            {"name", "get_symbol"},
            {"description", "Get the symbol or label name at an address."},
            {"input_schema", {
                {"type", "object"},
                {"properties", {{"address", {{"type", "string"}, {"description", "Address expression"}}}}},
                {"required", json::array({"address"})}
            }}
        },
        {
            {"name", "execute_command"},
            {"description", EXECUTE_COMMAND_DESC},
            {"input_schema", {
                {"type", "object"},
                {"properties", {{"command", {{"type", "string"}, {"description", "x64dbg command string"}}}}},
                {"required", json::array({"command"})}
            }}
        },
        {
            {"name", "get_command_help"},
            {"description",
                "Search the x64dbg command reference for documentation on a command or topic. "
                "Use this when you're unsure of exact syntax, parameters, or behavior of an x64dbg command. "
                "Use a SINGLE keyword for best results (e.g. 'hardware' not 'SetHardwareBreakpoint bph'). "
                "The search tokenizes your query and ranks by header matches.\n\n"
                "COMMAND INDEX (search by name or category):\n"
                "  debug-control: run/go, pause, StepInto/sti, StepOver/sto, StepOut, skip, InitDebug, StopDebug, AttachDebugger, DetachDebugger, erun, eStepInto\n"
                "  breakpoint-control: SetBPX/bp/bpx, DeleteBPX/bc, EnableBPX/bpe, DisableBPX/bpd, bplist, SetHardwareBreakpoint/bph, SetMemoryBPX, SetExceptionBPX, bpgoto\n"
                "  conditional-bp: SetBreakpointCondition, SetBreakpointLog, SetBreakpointCommand, SetBreakpointName, SetBreakpointFastResume, SetBreakpointSilent (+ Hardware/Memory/Exception/Librarian variants)\n"
                "  memory-operations: alloc, free, Fill/memset, memcpy, savedata, getpagerights, setpagerights, minidump\n"
                "  tracing: TraceIntoConditional/ticnd, TraceOverConditional/tocnd, RunToUserCode, RunToParty, TraceSetLog, TraceSetLogFile, StartTraceRecording, StopTraceRecording\n"
                "  thread-control: switchthread, createthread, killthread, suspendthread, resumethread, suspendallthreads, resumeallthreads\n"
                "  searching: find, findall, findallmem, findasm, reffind, reffindrange, refstr, modcallfind, findguid\n"
                "  user-database: labelset/lbl, labeldel, commentset/cmt, commentdel, bookmarkset, functionadd, functiondel, dbsave, dbload, dbclear\n"
                "  analysis: analyse/analyze, cfanalyze, exanalyse, analxrefs, analrecur, symdownload, symload, virtualmod\n"
                "  gui: disasm/dis/d, dump, sdump, graph, ClearLog/cls, memmapdump\n"
                "  general-purpose: mov/set, add, sub, mul, div, and, or, xor, not, neg, push, pop, shl, shr, cmp, test, bswap, rol, ror\n"
                "  misc: asm, HideDebugger, loadlib, gpa, chd, setcommandline, getcommandline\n"
                "  variables: var, vardel, varlist\n"
                "  types: AddType, AddStruct, AddMember, DataByte, DataWord, DataDword, DataQword, DataAscii, DataUnicode, DataCode\n"
                "  script: scriptload, scriptrun, scriptcmd, msg, msgyn\n"
                "  plugins: plugload, plugunload, StartScylla\n"
                "  watch: AddWatch, DelWatch, SetWatchExpression, SetWatchdog\n"
                "  REFERENCE sections: Values, Expressions, Expression-functions, Formatting, Variables"
            },
            {"input_schema", {
                {"type", "object"},
                {"properties", {{"query", {{"type", "string"}, {"description", "Single keyword to search for (e.g. 'hardware', 'trace', 'alloc', 'label', 'breakpoint'). One word works best."}}}}},
                {"required", json::array({"query"})}
            }}
        }
    });
}

json executeTool(const std::string& name, const json& input)
{
    if (name == "eval_expression")    return tool_eval_expression(input);
    if (name == "read_memory")        return tool_read_memory(input);
    if (name == "disassemble")        return tool_disassemble(input);
    if (name == "get_registers")      return tool_get_registers(input);
    if (name == "get_memory_map")     return tool_get_memory_map(input);
    if (name == "get_symbol")         return tool_get_symbol(input);
    if (name == "execute_command")    return tool_execute_command(input);
    if (name == "get_command_help")   return tool_get_command_help(input);

    return {{"error", "unknown tool: " + name}};
}
