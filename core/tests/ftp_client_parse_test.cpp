// 纯解析逻辑的单元测试——不需要真实 FTP 服务器(Phase C 还没做),覆盖
// FtpClient::parsePasvReply / parseListLine 这两个 static 纯函数。
#include "../ftp/ftp_client.h"

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

void testParsePasvReply() {
    std::string ip;
    uint16_t port = 0;

    check(core::FtpClient::parsePasvReply("227 Entering Passive Mode (192,168,1,5,200,10).", ip, port),
          "parsePasvReply standard format returns true");
    check(ip == "192.168.1.5", "parsePasvReply extracts correct IP (got '" + ip + "')");
    check(port == 200 * 256 + 10, "parsePasvReply extracts correct port (got " + std::to_string(port) + ")");

    check(core::FtpClient::parsePasvReply("227 Passive (127,0,0,1,0,80)", ip, port),
          "parsePasvReply without trailing period still parses");
    check(ip == "127.0.0.1", "parsePasvReply loopback IP correct");
    check(port == 80, "parsePasvReply low port correct (got " + std::to_string(port) + ")");

    check(!core::FtpClient::parsePasvReply("227 malformed reply with no numbers", ip, port),
          "parsePasvReply rejects malformed reply");
}

void testParseListLineMlsd() {
    std::vector<core::FtpFileEntry> entries;
    core::FtpClient::parseListLine("Type=file;Size=1234;Modify=20240102030405; readme.txt", true, entries);
    check(entries.size() == 1, "MLSD file line produces one entry");
    if (!entries.empty()) {
        check(entries[0].name == "readme.txt", "MLSD file name parsed (got '" + entries[0].name + "')");
        check(!entries[0].isDirectory, "MLSD file entry is not a directory");
        check(entries[0].size == 1234, "MLSD file size parsed (got " + std::to_string(entries[0].size) + ")");
    }

    entries.clear();
    core::FtpClient::parseListLine("Type=dir;Modify=20240102030405; subdir", true, entries);
    check(entries.size() == 1 && entries[0].isDirectory, "MLSD dir entry recognized as directory");

    entries.clear();
    core::FtpClient::parseListLine("Type=cdir;Modify=20240102030405; .", true, entries);
    check(entries.empty(), "MLSD skips '.' entry");

    entries.clear();
    core::FtpClient::parseListLine("Type=pdir;Modify=20240102030405; ..", true, entries);
    check(entries.empty(), "MLSD skips '..' entry");
}

void testParseListLineUnix() {
    std::vector<core::FtpFileEntry> entries;
    core::FtpClient::parseListLine("-rw-r--r-- 1 user group 4096 Jan 02 03:04 file.txt", false, entries);
    check(entries.size() == 1, "Unix LIST file line produces one entry");
    if (!entries.empty()) {
        check(entries[0].name == "file.txt", "Unix LIST file name parsed (got '" + entries[0].name + "')");
        check(!entries[0].isDirectory, "Unix LIST file entry is not a directory");
        check(entries[0].size == 4096, "Unix LIST file size parsed (got " + std::to_string(entries[0].size) + ")");
    }

    entries.clear();
    core::FtpClient::parseListLine("drwxr-xr-x 2 user group 4096 Jan 02 2024 subdir", false, entries);
    check(entries.size() == 1 && entries[0].isDirectory, "Unix LIST dir entry recognized as directory");

    entries.clear();
    core::FtpClient::parseListLine("lrwxrwxrwx 1 user group 7 Jan 02 2024 link -> target", false, entries);
    check(entries.size() == 1, "Unix LIST symlink line produces one entry");
    if (!entries.empty()) {
        check(entries[0].name == "link", "Unix LIST symlink name strips '-> target' (got '" + entries[0].name + "')");
    }

    entries.clear();
    core::FtpClient::parseListLine("total 24", false, entries);
    check(entries.empty(), "Unix LIST 'total N' header line is ignored");
}

} // namespace

int main() {
    testParsePasvReply();
    testParseListLineMlsd();
    testParseListLineUnix();

    if (g_failures == 0) {
        std::cout << "=== ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << g_failures << " FAILURE(S) ===" << std::endl;
    return 1;
}
