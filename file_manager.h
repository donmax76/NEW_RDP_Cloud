#pragma once
#include "host.h"
#include "logger.h"

class FileManager {
public:
    // List directory contents as JSON (Win32 FindFirstFile for reliability on drive roots)
    static std::string list_dir(const std::string& path) {
        std::ostringstream json;
        json << "{\"cmd\":\"file_list_result\",\"path\":\"" << json_escape(path) << "\",\"items\":[";

        // Ensure search pattern ends with \*
        std::string search = path;
        if (!search.empty() && search.back() != '\\' && search.back() != '/') search += '\\';
        search += '*';

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
        bool first = true;
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::string name = fd.cFileName;
                if (name == "." || name == "..") continue;

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
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
        json << "]}";
        return json.str();
    }

    // Read a chunk of file (offset, length), returns data
    static std::vector<uint8_t> read_file_chunk(const std::string& path, uint64_t offset, uint32_t length) {
        std::ifstream f(path, std::ios::binary);
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
        std::ifstream f(path, std::ios::binary);
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
        fs::create_directories(fs::path(path).parent_path(), ec);
        // Open for random write; create if not exists
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) {
            // File doesn't exist yet — create it
            f.clear();
            f.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
        }
        if (!f) return false;
        f.seekp((std::streamoff)offset);
        f.write((const char*)data, len);
        return f.good();
    }

    static bool delete_path(const std::string& path) {
        std::error_code ec;
        fs::remove_all(path, ec);
        return !ec;
    }

    static bool create_directory(const std::string& path) {
        std::error_code ec;
        return fs::create_directories(path, ec);
    }

    static bool rename_path(const std::string& from, const std::string& to) {
        std::error_code ec;
        fs::rename(from, to, ec);
        return !ec;
    }

    static bool copy_path(const std::string& from, const std::string& to) {
        std::error_code ec;
        fs::copy(from, to, fs::copy_options::recursive|fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    static std::string read_text_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    }

    static bool write_text_file(const std::string& path, const std::string& content) {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path);
        if (!f) return false;
        f << content;
        return f.good();
    }
};
