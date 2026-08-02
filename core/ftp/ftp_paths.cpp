#include "ftp_paths.h"

#include <filesystem>

namespace core {

namespace fs = std::filesystem;

namespace {

// weakly_canonical 对不存在的路径也能工作(只要求"能存在的最长前缀"存在,其余部分
// 只做词法规范化),但极少数情况下(权限问题等)仍可能抛出 filesystem_error——不能
// 让这种边缘情况直接崩掉整个会话线程,退化为普通的词法 absolute + lexically_normal。
fs::path safeWeaklyCanonical(const fs::path& p) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(p, ec);
    if (ec) return fs::absolute(p).lexically_normal();
    return result;
}
} // namespace

std::string normalizeFtpPath(const std::string& rootPath, const std::string& relativePath) {
    // 必须先去掉前导 "/" 再拼接——见头文件注释,这正是旧代码里真实出现过的 bug。
    std::string rel = relativePath;
    size_t start = 0;
    while (start < rel.size() && (rel[start] == '/' || rel[start] == '\\')) ++start;
    rel = rel.substr(start);

    const fs::path base = safeWeaklyCanonical(fs::path(rootPath));
    const fs::path combined = rel.empty() ? base : safeWeaklyCanonical(base / rel);

    const std::string baseStr = base.string();
    const std::string combinedStr = combined.string();

    if (combinedStr == baseStr) return combinedStr;

    // combined 必须严格是 base 的路径意义上的子路径(不只是字符串前缀——比如
    // base="/a/b", combined="/a/bc" 不能算合法,即使字符串前缀匹配)。
    if (combinedStr.compare(0, baseStr.size(), baseStr) != 0) return baseStr;
    if (combinedStr.size() <= baseStr.size()) return baseStr;
    const char sep = combinedStr[baseStr.size()];
    if (sep != '/' && sep != '\\') return baseStr;

    return combinedStr;
}

bool isValidFtpName(const std::string& name) {
    if (name.empty() || name.size() > 255) return false;

    static const std::string illegal = "<>:\"/\\|?*";
    for (char c : name) {
        if (illegal.find(c) != std::string::npos) return false;
    }

    if (name.front() == '.') return false;
    if (name.find("..") != std::string::npos) return false;

    return true;
}

} // namespace core
