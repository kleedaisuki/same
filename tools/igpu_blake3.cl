/** @file
 * @brief 实验用完整二叉树 BLAKE3；仅支持至少 2048 字节的二次幂输入。
 * Experimental BLAKE3 for power-of-two inputs of at least 2048 bytes only.
 * Not a production backend; unkeyed 32-byte digests, little-endian devices only.
 * 非生产后端；仅非密钥模式、32 字节摘要及小端设备。
 */
/** BLAKE3 固定初始向量。 / Fixed BLAKE3 initialization vector. */
__constant uint IV[8] = {0x6A09E667,0xBB67AE85,0x3C6EF372,0xA54FF53A,
                        0x510E527F,0x9B05688C,0x1F83D9AB,0x5BE0CD19};
/** 模 2^32 混合。 / Mix modulo 2^32. */
void g(uint *v, int a, int b, int c, int d, uint x, uint y) {
    v[a]+=v[b]+x; v[d]=rotate(v[d]^v[a],16U);
    v[c]+=v[d]; v[b]=rotate(v[b]^v[c],20U);
    v[a]+=v[b]+y; v[d]=rotate(v[d]^v[a],24U);
    v[c]+=v[d]; v[b]=rotate(v[b]^v[c],25U);
}
/** 完整块压缩；输入链值原地更新。 / Compress a full block, updating CV in place. */
void compress(uint *cv, uint *m, uint counter, uint flags) {
    uint v[16], p[16];
    const int perm[16]={2,6,3,10,7,0,4,13,1,11,12,5,9,14,15,8};
    for(int i=0;i<8;i++) v[i]=cv[i];
    for(int i=0;i<4;i++) v[8+i]=IV[i];
    v[12]=counter; v[13]=0; v[14]=64; v[15]=flags;
    #pragma unroll
    for(int r=0;r<7;r++) {
        g(v,0,4,8,12,m[0],m[1]); g(v,1,5,9,13,m[2],m[3]);
        g(v,2,6,10,14,m[4],m[5]); g(v,3,7,11,15,m[6],m[7]);
        g(v,0,5,10,15,m[8],m[9]); g(v,1,6,11,12,m[10],m[11]);
        g(v,2,7,8,13,m[12],m[13]); g(v,3,4,9,14,m[14],m[15]);
        for(int i=0;i<16;i++) p[i]=m[perm[i]];
        for(int i=0;i<16;i++) m[i]=p[i];
    }
    for(int i=0;i<8;i++) cv[i]=v[i]^v[i+8];
}
/** 每工作项计算一个完整 1024 字节叶。 / One full 1024-byte leaf per work item. */
__kernel void leaves(__global const uint *input, __global uint *output) {
    uint id=(uint)get_global_id(0), cv[8], m[16];
    for(int i=0;i<8;i++) cv[i]=IV[i];
    for(int b=0;b<16;b++) {
        for(int i=0;i<16;i++) m[i]=input[id*256+b*16+i];
        compress(cv,m,id,(b==0?1U:0U)|(b==15?2U:0U));
    }
    for(int i=0;i<8;i++) output[id*8+i]=cv[i];
}
/** 每层使用不同缓冲，最终层施加 ROOT。 / Ping-pong levels; apply ROOT on final level. */
__kernel void parents(__global const uint *input, __global uint *output, uint root) {
    uint id=(uint)get_global_id(0), cv[8], m[16];
    for(int i=0;i<8;i++) cv[i]=IV[i];
    for(int i=0;i<16;i++) m[i]=input[id*16+i];
    compress(cv,m,0,4U|(root?8U:0U));
    for(int i=0;i<8;i++) output[id*8+i]=cv[i];
}
