#pragma once

// 协议层(FTP 命令参数、wire 上的文件名文本)统一用 UTF-8 编码的 std::string;
// std::filesystem::path 在 Windows 上内部按 wchar_t(UTF-16)存储,它的窄字符串
// 构造函数和 string() 访问器默认按"当前进程的 ANSI 代码页"转换,不是 UTF-8——
// 文件名一旦包含当前代码页之外的字符(比如系统语言是英文时的中文文件名),
// path::string() 会直接抛 std::system_error("No mapping for the Unicode
// character exists in the target multi-byte code page"),这是一次真实报障的
// 根因(Windows 服务端传输中文文件名文件失败)。macOS/Linux 的窄字符编码本身就是
// UTF-8,不受这个问题影响,所以这两个函数在非 Windows 平台上是纯粹的直通。
//
// core/ 里任何需要把协议层的文件名/路径字符串和 std::filesystem::path 相互转换
// 的地方,都应该用这两个函数,不要直接用 fs::path(std::string) 或 path.string()。

#include <filesystem>
#include <string>

namespace core {

std::filesystem::path utf8ToPath(const std::string& utf8);
std::string pathToUtf8(const std::filesystem::path& path);

} // namespace core
