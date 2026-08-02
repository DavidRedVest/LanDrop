#pragma once

// 服务端路径安全:把 FTP 虚拟路径(以 "/" 开头的相对路径)钳制到 rootPath 之内。
// 独立于 FtpSession 存在,是为了能在没有真实 socket/服务器的情况下被单元测试
// 直接覆盖(见 core/tests/ftp_paths_test.cpp)——这块逻辑在旧的 Qt 版本
// (common/utils.cpp 的 normalizePath)里出过一次真实 bug:忘记先去掉虚拟路径的
// 前导 "/",导致所有文件操作都落到根目录本身而不是目标文件。这里独立测试就是为了
// 不再犯同一个错。

#include <string>

namespace core {

// relativePath 可以带前导 "/"(FTP 虚拟根路径写法)。任何试图用 ".." 逃出 rootPath
// 的路径都会被钳制回 rootPath 本身,而不是返回一个 rootPath 之外的路径。
std::string normalizeFtpPath(const std::string& rootPath, const std::string& relativePath);

// 校验单个文件名(不是完整路径)是否合法:非空、长度限制、不含非法字符、不以
// '.' 开头、不含 ".."。用于 MKD/RNTO 的目标名校验。
bool isValidFtpName(const std::string& name);

} // namespace core
