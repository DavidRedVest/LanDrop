#include "path_utf8.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace core {

#ifdef _WIN32

namespace {
std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideLen);
    return wide;
}

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int narrowLen =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(narrowLen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), narrowLen, nullptr,
                         nullptr);
    return narrow;
}
} // namespace

// 显式走 UTF-8 <-> UTF-16(wchar_t)转换,不经过 fs::path 的窄字符串构造函数——
// 那个构造函数在 Windows 上会假定输入是当前 ANSI 代码页,而不是 UTF-8。
std::filesystem::path utf8ToPath(const std::string& utf8) {
    return std::filesystem::path(utf8ToWide(utf8));
}

// 同理,不用 path::string()(同样假定目标是 ANSI 代码页),而是从 path::wstring()
// (无损的原生表示)显式转换到 UTF-8。
std::string pathToUtf8(const std::filesystem::path& path) {
    return wideToUtf8(path.wstring());
}

#else

// POSIX:窄字符编码本身就是 UTF-8,std::filesystem::path 按字节透传,不需要
// 额外转换。
std::filesystem::path utf8ToPath(const std::string& utf8) {
    return std::filesystem::path(utf8);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    return path.string();
}

#endif

} // namespace core
