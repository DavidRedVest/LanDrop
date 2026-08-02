// 真正的端到端集成测试:启动一个真实 core::FtpServer(监听真实回环端口,根目录
// 是磁盘上的一个临时目录),用一个真实 core::FtpClient 通过真实 TCP 连接驱动它,
// 覆盖登录鉴权(含错误密码)、PWD/CWD/MKD、STOR/RETR 往返字节校验、REST 续传、
// LIST 列目录、RNFR+RNTO、DELE、RMD。这是这个项目一贯的验证方法论——实际跑一遍,
// 不只是代码走查——阶段 B+C 都完成后正好可以互相测试。
#include "../ftp/ftp_client.h"
#include "../ftp/ftp_server.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

std::string readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "landrop_ftp_server_test_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    core::FtpServer server;
    server.setRootPath(root.string());
    server.setAuthenticator(
        [](const std::string& user, const std::string& pass) { return user == "testuser" && pass == "testpass"; });

    constexpr uint16_t kPort = 19570;
    check(server.start(kPort), "FtpServer::start binds and listens on a real port");

    // 错误密码必须被拒绝——安全默认值(未设置 Authenticator 时一律拒绝)在这里
    // 反过来验证:设置了 Authenticator 之后,错误凭据确实会被它拒绝,而不是被
    // 哪里的默认放行逻辑漏过去。
    {
        core::FtpClient bad;
        check(bad.connect("127.0.0.1", kPort, 3000) == core::FtpResult::Ok, "second client connects for bad-login test");
        check(bad.login("testuser", "wrongpass") != core::FtpResult::Ok, "login with wrong password is rejected");
        bad.disconnect();
    }

    core::FtpClient client;
    check(client.connect("127.0.0.1", kPort, 3000) == core::FtpResult::Ok, "FtpClient connects to real FtpServer");
    check(client.login("testuser", "testpass") == core::FtpResult::Ok, "login succeeds with correct credentials");

    std::string pwd;
    check(client.pwd(pwd) == core::FtpResult::Ok && pwd == "/", "PWD reports root '/' right after login (got '" + pwd + "')");

    check(client.mkdir("/sub") == core::FtpResult::Ok, "MKD /sub succeeds");
    check(fs::is_directory(root / "sub"), "MKD actually created the directory on disk");

    check(client.cwd("/sub") == core::FtpResult::Ok, "CWD /sub succeeds");
    check(client.pwd(pwd) == core::FtpResult::Ok && pwd == "/sub", "PWD reflects new cwd (got '" + pwd + "')");

    // 构造一段有一定大小、非全零、可重复校验的本地源文件。
    const fs::path localSrc = fs::temp_directory_path() / "landrop_ftp_server_test_src.bin";
    {
        std::ofstream out(localSrc, std::ios::binary);
        for (int i = 0; i < 300000; ++i) out.put(static_cast<char>((i * 7 + 3) % 251));
    }
    const std::string srcContent = readWholeFile(localSrc.string());

    check(client.uploadFile(localSrc.string(), "/sub/uploaded.bin") == core::FtpResult::Ok, "STOR uploads file");
    check(fs::exists(root / "sub" / "uploaded.bin"), "uploaded file exists on server disk");
    check(fs::file_size(root / "sub" / "uploaded.bin") == srcContent.size(), "uploaded file size matches source");

    std::vector<core::FtpFileEntry> entries;
    check(client.list("/sub", entries) == core::FtpResult::Ok, "LIST /sub succeeds");
    bool found = false;
    for (const auto& e : entries) {
        if (e.name == "uploaded.bin") {
            found = true;
            check(!e.isDirectory, "listed uploaded.bin is not marked as a directory");
            check(e.size == srcContent.size(), "listed uploaded.bin size matches (got " + std::to_string(e.size) + ")");
        }
    }
    check(found, "LIST /sub includes uploaded.bin");

    const fs::path localDst = fs::temp_directory_path() / "landrop_ftp_server_test_dst.bin";
    fs::remove(localDst, ec);
    check(client.downloadFile("/sub/uploaded.bin", localDst.string()) == core::FtpResult::Ok, "RETR downloads file");
    check(readWholeFile(localDst.string()) == srcContent, "downloaded content matches uploaded content byte-for-byte");

    // 续传:截断本地文件到一半,从该 offset 续传下载,验证最终内容和完整原文件一致。
    {
        const auto fullSize = fs::file_size(localDst, ec);
        const auto half = fullSize / 2;
        fs::resize_file(localDst, half, ec);
        check(!ec && fs::file_size(localDst) == half, "truncate downloaded file to half size for resume test");
        check(client.downloadFile("/sub/uploaded.bin", localDst.string(), half) == core::FtpResult::Ok,
              "RETR with REST resumes download from offset");
        check(readWholeFile(localDst.string()) == srcContent, "resumed download content matches original byte-for-byte");
    }

    check(client.rename("/sub/uploaded.bin", "/sub/renamed.bin") == core::FtpResult::Ok, "RNFR+RNTO renames file");
    check(fs::exists(root / "sub" / "renamed.bin"), "renamed file exists on disk");
    check(!fs::exists(root / "sub" / "uploaded.bin"), "old name no longer exists on disk");

    check(client.removeFile("/sub/renamed.bin") == core::FtpResult::Ok, "DELE removes file");
    check(!fs::exists(root / "sub" / "renamed.bin"), "deleted file gone from disk");

    check(client.cwd("/") == core::FtpResult::Ok, "CWD back to root");
    check(client.rmdir("/sub") == core::FtpResult::Ok, "RMD removes directory");
    check(!fs::exists(root / "sub"), "removed directory gone from disk");

    client.disconnect();
    server.stop();
    check(!server.isRunning(), "FtpServer::stop() actually stops the server");

    fs::remove(localSrc, ec);
    fs::remove(localDst, ec);
    fs::remove_all(root, ec);

    if (g_failures == 0) {
        std::cout << "=== ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << g_failures << " FAILURE(S) ===" << std::endl;
    return 1;
}
