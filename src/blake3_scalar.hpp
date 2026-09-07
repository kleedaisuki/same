#pragma once
#include <cstdint>
/**
 * @file
 * @brief CUDA 叶节点和主机归并共享的非密钥 BLAKE3 标量实现。 / Scalar unkeyed BLAKE3 shared by CUDA
 * leaves and host reduction. 仅支持 32 字节摘要，非完整可扩展输出函数（XOF）接口；运算均为模 2^32
 * 无符号运算。 Supports 32-byte digests, not a full extendable-output function (XOF) API; word
 * arithmetic is unsigned modulo 2^32.
 */
/// CUDA 编译时同时生成主机和设备版本。 / Generate host and device variants under CUDA.
#ifdef __CUDACC__
#define SAME_HD __host__ __device__
#else
#define SAME_HD
#endif
namespace same::b3 {
/// 读取固定初始向量；i 必须在 [0, 8) 内。 / Read the fixed initialization vector; i must be in [0,
/// 8).
SAME_HD inline std::uint32_t initial(int i) {
    const std::uint32_t iv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
    return iv[i];
}
/// 保留末次压缩的输入，以便正确追加 ROOT 标志。 / Preserve final compression input so ROOT can be
/// applied correctly.
struct Output {
    /// 输入链值（chaining value）和零填充的小端消息字。 / Input chaining value and zero-padded
    /// little-endian message words.
    std::uint32_t cv[8], block[16];
    /// 叶节点的块索引；父节点为零。 / Chunk index for leaves; zero for parents.
    std::uint64_t counter;
    /// 有效字节数及 CHUNK_START/CHUNK_END/PARENT/ROOT 域分离标志。 / Valid byte count and
    /// CHUNK_START/CHUNK_END/PARENT/ROOT domain-separation flags.
    std::uint32_t len, flags;
};
/// 32 位循环右移；调用方保证 0 < n < 32。 / Rotate a 32-bit word right; callers guarantee 0 < n
/// < 32.
SAME_HD inline std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}
/// 单次加法、循环移位、异或混合；索引指向 16 字状态内的四个不同字。 / One add-rotate-XOR mixing
/// step on four distinct indices in a 16-word state.
SAME_HD inline void g(std::uint32_t* v, int a, int b, int c, int d, std::uint32_t x,
                      std::uint32_t y) {
    v[a] += v[b] + x;
    v[d] = rotr(v[d] ^ v[a], 16);
    v[c] += v[d];
    v[b] = rotr(v[b] ^ v[c], 12);
    v[a] += v[b] + y;
    v[d] = rotr(v[d] ^ v[a], 8);
    v[c] += v[d];
    v[b] = rotr(v[b] ^ v[c], 7);
}
/**
 * @brief 执行七轮压缩，写入八字链值或根摘要。 / Run seven compression rounds and write an
 * eight-word chaining value or root digest. out 至少容纳 8 字；root 模式使用输出块计数
 * 0，而非叶节点计数。 out must hold eight words; root mode uses output-block counter zero rather
 * than the leaf counter. ROOT 必须用于 Output，不能再次压缩已算出的链值来替代。 ROOT must be
 * applied to Output, not simulated by recompressing an already computed chaining value.
 */
SAME_HD inline void compress(const Output& o, std::uint32_t* out, bool root = false) {
    std::uint32_t v[16], m[16], p[16];
    for (int i = 0; i < 8; ++i)
        v[i] = o.cv[i];
    for (int i = 0; i < 4; ++i)
        v[8 + i] = initial(i);
    v[12] = root ? 0 : static_cast<std::uint32_t>(o.counter);
    v[13] = root ? 0 : static_cast<std::uint32_t>(o.counter >> 32);
    v[14] = o.len;
    v[15] = o.flags | (root ? 8 : 0);
    for (int i = 0; i < 16; ++i)
        m[i] = o.block[i];
    const int permutation[16] = {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};
    // 固定轮数展开避免 CUDA 消息调度落入线程本地内存。 / Unroll fixed rounds to keep
    // CUDA message scheduling out of thread-local memory.
#ifdef __CUDA_ARCH__
#pragma unroll
#endif
    for (int r = 0; r < 7; ++r) {
        g(v, 0, 4, 8, 12, m[0], m[1]);
        g(v, 1, 5, 9, 13, m[2], m[3]);
        g(v, 2, 6, 10, 14, m[4], m[5]);
        g(v, 3, 7, 11, 15, m[6], m[7]);
        g(v, 0, 5, 10, 15, m[8], m[9]);
        g(v, 1, 6, 11, 12, m[10], m[11]);
        g(v, 2, 7, 8, 13, m[12], m[13]);
        g(v, 3, 4, 9, 14, m[14], m[15]);
        for (int i = 0; i < 16; ++i)
            p[i] = m[permutation[i]];
        for (int i = 0; i < 16; ++i)
            m[i] = p[i];
    }
    for (int i = 0; i < 8; ++i)
        out[i] = v[i] ^ v[i + 8];
}
/**
 * @brief 为 0..1024 字节的单个叶块构造末次压缩输入。 / Build final compression input for one
 * 0..1024-byte chunk. data 至少含 len 字节；len 为零时不解引用。每个 64 字节块按小端装载并零填充。
 * data must contain len bytes and is not dereferenced when empty; 64-byte blocks are loaded
 * little-endian and zero-padded. 空输入仍产生一个长度为零、同时带 START 和 END
 * 的块；最后一块留待调用者压缩。 Empty input still creates a zero-length START|END block; the final
 * block remains uncompressed for the caller.
 */
SAME_HD inline Output chunk(const unsigned char* data, unsigned len, std::uint64_t counter) {
    Output o{};
    for (int i = 0; i < 8; ++i)
        o.cv[i] = initial(i);
    o.counter = counter;
    unsigned blocks = len ? ((len + 63) / 64) : 1;
    for (unsigned b = 0; b < blocks; ++b) {
        for (int i = 0; i < 16; ++i)
            o.block[i] = 0;
        unsigned n = len - b * 64;
        if (n > 64)
            n = 64;
        for (unsigned i = 0; i < n; ++i)
            o.block[i / 4] |= static_cast<std::uint32_t>(data[b * 64 + i]) << (8 * (i % 4));
        o.len = n;
        o.flags = (b == 0 ? 1 : 0) | (b + 1 == blocks ? 2 : 0);
        if (b + 1 < blocks) {
            std::uint32_t cv[8];
            compress(o, cv);
            for (int i = 0; i < 8; ++i)
                o.cv[i] = cv[i];
        }
    }
    return o;
}
/// 左右各八字链值依序组成父节点，长度 64、计数 0、仅置 PARENT 标志。 / Join two ordered eight-word
/// child values into a 64-byte, counter-zero PARENT node.
SAME_HD inline Output parent(const std::uint32_t* left, const std::uint32_t* right) {
    Output o{};
    for (int i = 0; i < 8; ++i) {
        o.cv[i] = initial(i);
        o.block[i] = left[i];
        o.block[i + 8] = right[i];
    }
    o.len = 64;
    o.flags = 4;
    return o;
}
} // namespace same::b3
#undef SAME_HD
