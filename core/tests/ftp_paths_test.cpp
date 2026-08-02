// core::normalizeFtpPath / isValidFtpName 的纯单元测试——覆盖旧 Qt 版本
// (common/utils.cpp 的 normalizePath)真实出过的那个"忘记去掉前导斜杠导致所有
// 操作都落到根目录"的 bug,以及路径穿越("..")逃逸尝试。不需要真实 socket。
#include "../ftp/ftp_paths.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {
int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "[OK] " << what << std::endl;
    } else {
        std::cout << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

void testNormalizeFtpPath() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "landrop_ftp_paths_test_root";
    std::error_code ec;
    fs::create_directories(root / "sub", ec);
    const std::string rootStr = fs::weakly_canonical(root).string();

    // 回归测试:前导 "/" 必须被正确剥离,结果应该是 root 下的具体子路径,
    // 而不是退化回 root 本身(这正是旧代码真实犯过的错)。
    const std::string p1 = core::normalizeFtpPath(rootStr, "/sub/file.txt");
    // 用已经规范化过的 rootStr(而不是原始 root)拼期望值,避免 temp_directory_path()
    // 本身经过符号链接(比如 macOS 的 /tmp -> /private/tmp)导致字符串形式不一致。
    const std::string expected1 = (fs::path(rootStr) / "sub" / "file.txt").string();
    check(p1 == expected1, "leading '/' is stripped, result targets root/sub/file.txt not root itself (got '" + p1 + "')");

    const std::string p2 = core::normalizeFtpPath(rootStr, "/a/b.txt");
    check(p2 != rootStr, "normalized path with a nonempty relative part is never just the root path");

    // 路径穿越:任何试图用 ".." 跳出 root 的路径都必须被钳制回 root 本身。
    const std::string p3 = core::normalizeFtpPath(rootStr, "/../../../etc/passwd");
    check(p3 == rootStr, "'..' escape attempt is clamped back to root (got '" + p3 + "')");

    const std::string p4 = core::normalizeFtpPath(rootStr, "/sub/../../outside");
    check(p4 == rootStr, "'..' escape via a subdirectory is clamped back to root (got '" + p4 + "')");

    // 字符串前缀陷阱:root=".../landrop_ftp_paths_test_root",不能被一个名字恰好
    // 以同样字符串开头的兄弟目录(...test_root_evil)骗过。
    const std::string p5 = core::normalizeFtpPath(rootStr, "/../landrop_ftp_paths_test_root_evil/x");
    check(p5 == rootStr, "sibling directory sharing root's string prefix does not pass the containment check");

    // 根目录本身
    const std::string p6 = core::normalizeFtpPath(rootStr, "/");
    check(p6 == rootStr, "empty relative path (root itself) normalizes to root");

    fs::remove_all(root, ec);
}

void testIsValidFtpName() {
    check(core::isValidFtpName("file.txt"), "simple name is valid");
    check(core::isValidFtpName("a"), "single character name is valid");
    check(!core::isValidFtpName(""), "empty name is invalid");
    check(!core::isValidFtpName(".hidden"), "name starting with '.' is invalid");
    check(!core::isValidFtpName(".."), "'..' itself is invalid");
    check(!core::isValidFtpName("a..b"), "name containing '..' is invalid");
    check(!core::isValidFtpName("a/b"), "name containing '/' is invalid");
    check(!core::isValidFtpName("a\\b"), "name containing '\\' is invalid");
    check(!core::isValidFtpName("a:b"), "name containing ':' is invalid");
    check(!core::isValidFtpName(std::string(256, 'x')), "name longer than 255 chars is invalid");
    check(core::isValidFtpName(std::string(255, 'x')), "name exactly 255 chars is valid");
}

} // namespace

int main() {
    testNormalizeFtpPath();
    testIsValidFtpName();

    if (g_failures == 0) {
        std::cout << "=== ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << g_failures << " FAILURE(S) ===" << std::endl;
    return 1;
}
