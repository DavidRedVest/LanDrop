#include "ftp_paths.h"

#include <filesystem>

namespace core {

namespace fs = std::filesystem;

namespace {

// weakly_canonical 对不存在的路径也能工作(只要求"能存在的最长前缀"存在,其余部分
// 只做词法规范化),但极少数情况下(权限问题、Windows 上超过 MAX_PATH 的深层路径等)
// 仍可能失败——不能让这种边缘情况直接崩掉整个会话线程,退化为普通的词法
// absolute + lexically_normal。
//
// 这里曾经是一个真实 bug:退化分支写的是不带 error_code 的 fs::absolute(p),
// 而这个重载在失败时是会抛 filesystem_error 的——和上面注释里"不能崩掉整个会话
// 线程"的意图正好相反。真实后果不止"这一个连接断了":FtpSession::run() 是这个
// 会话专属 std::thread 的顶层函数,没有任何 try/catch,一旦这里抛出未捕获异常,
// C++ 会调用 std::terminate() 直接杀掉整个进程——也就是"一个客户端传输大文件
// 触发这个边缘情况,能把服务端和其它所有正连着的客户端一起带崩"。
// 现在两级都用不抛异常的 error_code 重载;最后 combined.lexically_normal() 只是
// 纯词法操作,不触碰文件系统,不会失败,是真正兜底的一层。
fs::path safeWeaklyCanonical(const fs::path& p) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(p, ec);
    if (!ec) return result;

    fs::path absolute = fs::absolute(p, ec);
    if (!ec) return absolute.lexically_normal();

    return p.lexically_normal();
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
