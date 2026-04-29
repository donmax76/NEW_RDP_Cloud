#pragma once
#include "host.h"
#include "logger.h"

class FileManager {
public:
    // ===== Unicode helpers (public so callers can use them too) =====
    // All paths come in as UTF-8 std::string from JSON/WS layer. Win32 ANSI APIs
    // would corrupt non-ASCII chars (Azerbaijani ə, ı, ş, ç, ğ, ö, ü etc.) by
    // interpreting them as CP_ACP. Convert to wide for any Win32 / fs::path usage.
    static std::wstring u8_to_w(const std::string& s) {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        if (n <= 0) return L"";
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }
    static std::string w_to_u8(const std::wstring& w) {
        if (w.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return "";
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }
    // Build fs::path from UTF-8 std::string (avoids ANSI default ctor on Windows)
    static fs::path u8_path(const std::string& s) { return fs::path(u8_to_w(s)); }

    // List directory contents as JSON (Win32 FindFirstFileW for Unicode correctness)
    static std::string list_dir(const std::string& path) {
        std::ostringstream json;
        json << "{\"cmd\":\"file_list_result\",\"path\":\"" << json_escape(path) << "\",\"items\":[";

        // Ensure search pattern ends with \* (in wide form)
        std::wstring wsearch = u8_to_w(path);
        if (!wsearch.empty() && wsearch.back() != L'\\' && wsearch.back() != L'/') wsearch += L'\\';
        wsearch += L'*';

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(wsearch.c_str(), &fd);
        bool first = true;
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::wstring wname = fd.cFileName;
                if (wname == L"." || wname == L"..") continue;
                std::string name = w_to_u8(wname);  // UTF-8 for JSON

                bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                int64_t size = 0;
                if (!is_dir) {
                    size = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                }

                // Convert FILETIME to readable date
                // FILETIME = 100ns intervals since 1601-01-01
                char date_buf[32] = "\xe2\x80\x94"; // em-dash UTF-8
                FILETIME ft = fd.ftLastWriteTime;
                if (ft.dwHighDateTime != 0 || ft.dwLowDateTime != 0) {
                    ULARGE_INTEGER ull;
                    ull.LowPart = ft.dwLowDateTime;
                    ull.HighPart = ft.dwHighDateTime;
                    // Convert to Unix seconds: (ticks / 10000000) - 11644473600
                    int64_t mtime = (int64_t)(ull.QuadPart / 10000000ULL) - 11644473600LL;
                    if (mtime > 0) {
                        time_t t = (time_t)mtime;
                        std::tm tm_buf{};
                        gmtime_s(&tm_buf, &t);
                        std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M", &tm_buf);
                    }
                }

                // Attributes string
                std::string attrs;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)    attrs += "H";
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)    attrs += "S";
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)  attrs += "R";
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)   attrs += "A";

                if (!first) json << ",";
                json << "{\"name\":\"" << json_escape(name) << "\""
                     << ",\"type\":\"" << (is_dir ? "dir" : "file") << "\""
                     << ",\"size\":" << size
                     << ",\"modified\":\"" << date_buf << "\""
                     << ",\"attrs\":\"" << attrs << "\"}";
                first = false;
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        json << "]}";
        return json.str();
    }

    // Read a chunk of file (offset, length), returns data.
    // path is UTF-8; convert to wide-backed fs::path so non-ASCII names work.
    static std::vector<uint8_t> read_file_chunk(const std::string& path, uint64_t offset, uint32_t length) {
        std::ifstream f(u8_path(path), std::ios::binary);
        if (!f) return {};
        f.seekg((std::streamoff)offset);
        std::vector<uint8_t> buf(length);
        f.read((char*)buf.data(), length);
        size_t n = (size_t)f.gcount();
        buf.resize(n);
        return buf;
    }

    // Read file, return base64 chunks via callback
    static bool read_file_chunks(const std::string& path,
        std::function<void(const uint8_t*, size_t, size_t, size_t)> cb,
        size_t chunk_size = 1*1024*1024)
    {
        std::ifstream f(u8_path(path), std::ios::binary);
        if (!f) return false;
        f.seekg(0, std::ios::end);
        size_t total = (size_t)f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(chunk_size);
        size_t offset = 0;
        while (f) {
            f.read((char*)buf.data(), chunk_size);
            size_t n = (size_t)f.gcount();
            if (n == 0) break;
            cb(buf.data(), n, offset, total);
            offset += n;
        }
        return true;
    }

    // Write file chunk. 'last' = true when this is the final chunk (passed from WCHK binary protocol).
    // Offset==0 and any chunk: create/truncate if new file; seek to offset otherwise.
    static bool write_chunk(const std::string& path, const uint8_t* data, size_t len,
                            size_t offset, bool last)
    {
        std::error_code ec;
        fs::path wpath = u8_path(path);
        fs::create_directories(wpath.parent_path(), ec);
        // Open for random write; create if not exists
        std::fstream f(wpath, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) {
            // File doesn't exist yet — create it
            f.clear();
            f.open(wpath, std::ios::binary | std::ios::out | std::ios::trunc);
        }
        if (!f) return false;
        f.seekp((std::streamoff)offset);
        f.write((const char*)data, len);
        return f.good();
    }

    static bool delete_path(const std::string& path) {
        std::error_code ec;
        fs::remove_all(u8_path(path), ec);
        return !ec;
    }

    static bool create_directory(const std::string& path) {
        std::error_code ec;
        return fs::create_directories(u8_path(path), ec);
    }

    static bool rename_path(const std::string& from, const std::string& to) {
        std::error_code ec;
        fs::rename(u8_path(from), u8_path(to), ec);
        return !ec;
    }

    static bool copy_path(const std::string& from, const std::string& to) {
        std::error_code ec;
        fs::copy(u8_path(from), u8_path(to),
                 fs::copy_options::recursive|fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    static std::string read_text_file(const std::string& path) {
        std::ifstream f(u8_path(path));
        if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    }

    static bool write_text_file(const std::string& path, const std::string& content) {
        std::error_code ec;
        fs::path wpath = u8_path(path);
        fs::create_directories(wpath.parent_path(), ec);
        std::ofstream f(wpath);
        if (!f) return false;
        f << content;
        return f.good();
    }
};
