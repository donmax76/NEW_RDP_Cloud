#pragma once
// proc_enum.h — process enumeration via NtQuerySystemInformation(SystemProcessInformation).
// Replaces CreateToolhelp32Snapshot + Process32First/Next everywhere in the codebase.
// T1057 surface reduction: no Toolhelp32 IAT entries, no high-confidence sandbox signatures.
// Dynamic resolution keeps "NtQuerySystemInformation" out of the import table as well.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

#ifndef STATUS_INFO_LENGTH_MISMATCH
#  define STATUS_INFO_LENGTH_MISMATCH ((LONG)0xC0000004L)
#endif

typedef LONG NTSTATUS_PE;
typedef NTSTATUS_PE (NTAPI *PfnNtQSI)(ULONG, PVOID, ULONG, PULONG);

// ── SYSTEM_PROCESS_INFORMATION layout (x64, Windows 7-11) ────────────────────
// Explicit padding fields replicate the binary layout exactly so the compiler
// injects no hidden alignment bytes.  Verified against public symbol data and
// phnt / ProcessHacker source trees.
//
//  Offset  Size  Field
//  0x00     4    NextEntryOffset
//  0x04     4    NumberOfThreads
//  0x08     8    WorkingSetPrivSize (Vista+)
//  0x10     4    HardFaultCount     (Win7+)
//  0x14     4    ThreadsHighWM
//  0x18     8    CycleTime
//  0x20     8    CreateTime
//  0x28     8    UserTime           (100ns, accumulated kernel+user split)
//  0x30     8    KernelTime
//  0x38     2    ImgNameLen (bytes, NOT wchars)
//  0x3A     2    ImgNameMax
//  0x3C     4    _pad0 → aligns ImgNameBuf pointer to 8 bytes
//  0x40     8    ImgNameBuf
//  0x48     4    BasePriority
//  0x4C     4    _pad1 → aligns Pid to 8
//  0x50     8    Pid  (ULONG_PTR)
//  0x58     8    ParentPid
//  0x60     4    HandleCount
//  0x64     4    SessionId
//  0x68     8    UniqueProcessKey
//  0x70     8    PeakVirtSize
//  0x78     8    VirtSize
//  0x80     4    PageFaultCount
//  0x84     4    _pad2 → aligns WorkingSet fields to 8
//  0x88     8    PeakWorkingSet
//  0x90     8    WorkingSet         (= PROCESS_MEMORY_COUNTERS.WorkingSetSize)
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 8)
struct PeNtSpi {
    ULONG      NextEntryOffset;   // 0x00
    ULONG      NumberOfThreads;   // 0x04
    LONGLONG   WorkingSetPriv;    // 0x08
    ULONG      HardFaultCount;    // 0x10
    ULONG      ThreadsHighWM;     // 0x14
    ULONGLONG  CycleTime;         // 0x18
    LONGLONG   CreateTime;        // 0x20
    LONGLONG   UserTime;          // 0x28
    LONGLONG   KernelTime;        // 0x30
    USHORT     ImgNameLen;        // 0x38  (byte length)
    USHORT     ImgNameMax;        // 0x3A
    ULONG      _pad0;             // 0x3C
    PWSTR      ImgNameBuf;        // 0x40
    LONG       BasePriority;      // 0x48
    ULONG      _pad1;             // 0x4C
    ULONG_PTR  Pid;               // 0x50
    ULONG_PTR  ParentPid;         // 0x58
    ULONG      HandleCount;       // 0x60
    ULONG      SessionId;         // 0x64
    ULONG_PTR  UniqueProcessKey;  // 0x68
    SIZE_T     PeakVirtSize;      // 0x70
    SIZE_T     VirtSize;          // 0x78
    ULONG      PageFaultCount;    // 0x80
    ULONG      _pad2;             // 0x84
    SIZE_T     PeakWorkingSet;    // 0x88
    SIZE_T     WorkingSet;        // 0x90
};
#pragma pack(pop)

// Resolve NtQuerySystemInformation once at first call.
inline PfnNtQSI _pe_ntqsi() {
    static PfnNtQSI fn = nullptr;
    if (!fn) {
        HMODULE h = GetModuleHandleA("ntdll.dll");
        if (h) fn = reinterpret_cast<PfnNtQSI>(
                        GetProcAddress(h, "NtQuerySystemInformation"));
    }
    return fn;
}

// ── pe_enumerate ─────────────────────────────────────────────────────────────
// Walk all SYSTEM_PROCESS_INFORMATION entries via NtQSI class 5.
// cb(entry) is called for each process; return false from cb to stop early.
// Returns false if NtQSI is unavailable or returns a failure NTSTATUS.
inline bool pe_enumerate(std::function<bool(const PeNtSpi*)> cb) {
    auto fn = _pe_ntqsi();
    if (!fn) return false;

    std::vector<BYTE> buf(0x20000); // 128 KB initial — grows as needed
    ULONG   ret = 0;
    NTSTATUS_PE st = 0;
    for (int tries = 0; tries < 8; ++tries) {
        st = fn(5 /*SystemProcessInformation*/, buf.data(), (ULONG)buf.size(), &ret);
        if (st != STATUS_INFO_LENGTH_MISMATCH) break;
        buf.resize(buf.size() * 2);
    }
    if (st < 0) return false;

    const BYTE* base = buf.data();
    const BYTE* end  = base + buf.size();
    const BYTE* p    = base;
    for (;;) {
        if (p + sizeof(PeNtSpi) > end) break;
        const auto* e = reinterpret_cast<const PeNtSpi*>(p);
        if (!cb(e)) break;
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }
    return true;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

// Image name → UTF-8  (ImgNameLen is byte length, NOT wchar count)
inline std::string pe_img_name(const PeNtSpi* e) {
    if (!e->ImgNameBuf || !e->ImgNameLen) return {};
    int wlen = e->ImgNameLen / 2;
    int n = WideCharToMultiByte(CP_UTF8, 0, e->ImgNameBuf, wlen,
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, e->ImgNameBuf, wlen, r.data(), n,
                        nullptr, nullptr);
    return r;
}

// Image name → wide string
inline std::wstring pe_img_namew(const PeNtSpi* e) {
    if (!e->ImgNameBuf || !e->ImgNameLen) return {};
    return std::wstring(e->ImgNameBuf, e->ImgNameLen / 2);
}

// Total CPU time in 100ns units (kernel + user, same units as GetProcessTimes)
inline ULONGLONG pe_cpu_time(const PeNtSpi* e) {
    return static_cast<ULONGLONG>(e->KernelTime) +
           static_cast<ULONGLONG>(e->UserTime);
}
