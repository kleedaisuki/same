#pragma once
/// 嵌入内核，部署不依赖源文件路径。 / Embedded kernel independent of deployment paths.
inline constexpr const char* opencl_source = R"CLC(
inline uint initial(int i) {
    const uint iv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
    return iv[i];
}
/// 保留末次压缩的输入，以便正确追加 ROOT 标志。 / Preserve final compression input so ROOT can be
/// applied correctly.
typedef struct {
    /// 输入链值（chaining value）和零填充的小端消息字。 / Input chaining value and zero-padded
    /// little-endian message words.
    uint cv[8], block[16];
    /// 叶节点的块索引；父节点为零。 / Chunk index for leaves; zero for parents.
    ulong counter;
    /// 有效字节数及 CHUNK_START/CHUNK_END/PARENT/ROOT 域分离标志。 / Valid byte count and
    /// CHUNK_START/CHUNK_END/PARENT/ROOT domain-separation flags.
    uint len, flags;
} Output;
/// 32 位循环右移；调用方保证 0 < n < 32。 / Rotate a 32-bit word right; callers guarantee 0 < n
/// < 32.
inline uint rotr(uint x, int n) {
    return (x >> n) | (x << (32 - n));
}
/// 单次加法、循环移位、异或混合；索引指向 16 字状态内的四个不同字。 / One add-rotate-XOR mixing
/// step on four distinct indices in a 16-word state.
inline void g(uint* v, int a, int b, int c, int d, uint x,
                      uint y) {
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
inline void compress(Output o, uint* out, bool root) {
    uint v[16], m[16], p[16];
    for (int i = 0; i < 8; ++i)
        v[i] = o.cv[i];
    for (int i = 0; i < 4; ++i)
        v[8 + i] = initial(i);
    v[12] = root ? 0 : ((uint)o.counter);
    v[13] = root ? 0 : ((uint)(o.counter >> 32));
    v[14] = o.len;
    v[15] = o.flags | (root ? 8 : 0);
    for (int i = 0; i < 16; ++i)
        m[i] = o.block[i];
    const int permutation[16] = {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};
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

/// 完整叶在设备计算；尾叶由主机保留 ROOT 语义。 / Device full leaves; host retains final ROOT output.
__kernel void leaves(__global const uint* input, __global uint* cvs, ulong first) {
    size_t i = get_global_id(0);
    Output o;
    for (int j=0;j<8;++j) o.cv[j]=initial(j);
    o.counter=first+(ulong)i; o.len=64;
    for (int b=0;b<16;++b) {
        for (int j=0;j<16;++j) o.block[j]=input[i*256+b*16+j];
        o.flags=(b==0?1:0)|(b==15?2:0);
        compress(o,o.cv,false);
    }
    for (int j=0;j<8;++j) cvs[i*8+j]=o.cv[j];
}
)CLC";
