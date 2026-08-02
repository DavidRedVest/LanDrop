#pragma once

// 自实现的 SHA-256(FIPS 180-4),无第三方依赖——核心层不能链接 QCryptographicHash,
// 只用于我们自己 FtpServer 的可选完整性校验扩展命令(见重构计划)。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace core {

class Sha256 {
public:
    Sha256();

    void update(const void* data, size_t len);
    // 结束计算并返回 32 字节摘要。调用后该对象不能再 update()。
    std::array<uint8_t, 32> finalize();

    // 便捷函数:一次性计算一段内存的 SHA-256。
    static std::array<uint8_t, 32> hash(const void* data, size_t len);
    static std::string toHex(const std::array<uint8_t, 32>& digest);

private:
    void processBlock(const uint8_t* block);

    uint32_t m_state[8];
    uint64_t m_totalLen; // 已经喂入的字节数(finalize 时换算成 bit 长度写入末尾)
    uint8_t m_buffer[64];
    size_t m_bufferLen;
};

} // namespace core
