#pragma once
// Threat process scanner — used by both EXE helper and DLL CaptureHelper.
// Returns list of (proc_name, window_title) for visible (non-iconic) windows
// belonging to known monitoring/sniffing tools.
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <string>
#include <vector>
#include <map>
#include <cctype>

// UWP frozen-state detection via PssCaptureSnapshot or job object query.
// PROCESS_EXTENDED_BASIC_INFORMATION via NtQueryInformationProcess class 0
// (ProcessBasicInformation) with extended Size returns Flags incl. IsFrozen.
struct PROCESS_EXTENDED_BASIC_INFORMATION_TS {
    SIZE_T Size;
    PROCESS_BASIC_INFORMATION BasicInfo;
    ULONG Flags;
};

inline bool ts_is_process_frozen(DWORD pid) {
    static auto pNtQIP = (NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG))
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!pNtQIP) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    PROCESS_EXTENDED_BASIC_INFORMATION_TS info = {};
    info.Size = sizeof(info);
    // Class 0 = ProcessBasicInformation; with Size>=sizeof(extended), kernel fills Flags
    NTSTATUS st = pNtQIP(h, 0, &info, sizeof(info), nullptr);
    CloseHandle(h);
    if (st < 0) return false;
    return (info.Flags & 0x10) != 0; // bit 4 = IsFrozen
}

// Check if a UWP process has at least one foreground window with the
// expected substring in its title (e.g. "Privacy", "Конфиденциальность").
struct TsTitleCheckCtx {
    DWORD pid;
    const char** keywords;
    bool found;
};

inline BOOL CALLBACK ts_title_enum(HWND hwnd, LPARAM lp) {
    auto* c = (TsTitleCheckCtx*)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != c->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (IsIconic(hwnd)) return TRUE;
    char title[512] = {};
    int n = GetWindowTextA(hwnd, title, sizeof(title) - 1);
    if (n <= 0) return TRUE;
    std::string lo;
    for (int i = 0; i < n; i++) lo += (char)tolower((unsigned char)title[i]);
    for (int i = 0; c->keywords[i]; i++) {
        if (lo.find(c->keywords[i]) != std::string::npos) {
            c->found = true;
            return FALSE;
        }
    }
    return TRUE;
}

static const char* kThreatNames[] = {
    "taskmgr.exe","resmon.exe","perfmon.exe",
    "procexp.exe","procexp64.exe","procmon.exe","procmon64.exe",
    "processhacker.exe","systeminformer.exe",
    "pchunter.exe","pchunter64.exe","autoruns.exe","autoruns64.exe",
    "wireshark.exe","dumpcap.exe","tshark.exe",
    "fiddler.exe","httpdebugger.exe","httpdebuggerpro.exe","charles.exe",
    "tcpview.exe","tcpview64.exe","netmon.exe","smsniff.exe",
    "x64dbg.exe","x32dbg.exe","ollydbg.exe","ida.exe","ida64.exe","windbg.exe",
    "systemsettings.exe",
    nullptr
};

inline std::string ts_lower(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) r += (char)tolower((unsigned char)c);
    return r;
}

inline bool ts_is_threat(const std::string& exeLower, const char*& matchedName) {
    for (int i = 0; kThreatNames[i]; i++) {
        if (exeLower == kThreatNames[i]) { matchedName = kThreatNames[i]; return true; }
    }
    return false;
}

struct TsWinEnumCtx { std::map<DWORD, std::string> visible_pids; };

inline BOOL CALLBACK ts_enum_proc(HWND hwnd, LPARAM lp) {
    auto* ctx = (TsWinEnumCtx*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (IsIconic(hwnd)) return TRUE;
    char title[256] = {};
    int tlen = GetWindowTextA(hwnd, title, sizeof(title) - 1);
    if (tlen <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && ctx->visible_pids.find(pid) == ctx->visible_pids.end())
        ctx->visible_pids[pid] = title;
    return TRUE;
}

// Scan ALL running processes (not only those with visible windows).
// Visibility check via EnumWindows is unreliable for elevated/UIPI-protected
// windows (Task Manager runs elevated, helper is non-elevated → can't see).
// Window title is added if EnumWindows happens to see it, otherwise empty.
inline int ts_scan_all(std::vector<std::pair<std::string,std::string>>& out) {
    out.clear();
    // Build pid → title map (best-effort, may miss elevated windows)
    TsWinEnumCtx ctx;
    EnumWindows(ts_enum_proc, (LPARAM)&ctx);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            char buf[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, buf, MAX_PATH, NULL, NULL);
            std::string lo = ts_lower(buf);
            const char* matched = nullptr;
            if (ts_is_threat(lo, matched)) {
                // Skip frozen UWP processes (Suspended state)
                if (ts_is_process_frozen(pe.th32ProcessID)) continue;
                std::string title;
                auto it = ctx.visible_pids.find(pe.th32ProcessID);
                if (it != ctx.visible_pids.end()) title = it->second;

                // Special case: systemsettings.exe — UWP stays running in background.
                // Flag only when the user is actually looking at the Settings window
                // (visible, not minimized, has a title). Windows 11 doesn't change
                // the title per sub-page, so we can't filter to a specific category
                // without UIAutomation — any visible Settings window counts.
                if (lo == "systemsettings.exe") {
                    if (ctx.visible_pids.empty()) continue; // Session 0 blind
                    if (title.empty()) continue;            // no visible window
                }
                out.emplace_back(matched, title);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return (int)out.size();
}
