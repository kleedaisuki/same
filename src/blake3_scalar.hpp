#pragma once
#include <cstdint>
// Scalar BLAKE3 compression shared by CUDA leaves and the bounded host tree.
// CUDA 叶节点与有界主机树共享的标量 BLAKE3 压缩函数。
#ifdef __CUDACC__
#define SAME_HD __host__ __device__
#else
#define SAME_HD
#endif
namespace same::b3 {
SAME_HD inline std::uint32_t initial(int i) { const std::uint32_t iv[8] = {0x6A09E667,0xBB67AE85,0x3C6EF372,0xA54FF53A,0x510E527F,0x9B05688C,0x1F83D9AB,0x5BE0CD19}; return iv[i]; }
struct Output { std::uint32_t cv[8], block[16]; std::uint64_t counter; std::uint32_t len, flags; };
SAME_HD inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32-n)); }
SAME_HD inline void g(std::uint32_t* v,int a,int b,int c,int d,std::uint32_t x,std::uint32_t y) {
    v[a]+=v[b]+x; v[d]=rotr(v[d]^v[a],16); v[c]+=v[d]; v[b]=rotr(v[b]^v[c],12);
    v[a]+=v[b]+y; v[d]=rotr(v[d]^v[a],8); v[c]+=v[d]; v[b]=rotr(v[b]^v[c],7);
}
SAME_HD inline void compress(const Output& o, std::uint32_t* out, bool root=false) {
    std::uint32_t v[16],m[16],p[16];
    for(int i=0;i<8;++i) v[i]=o.cv[i];
    for(int i=0;i<4;++i) v[8+i]=initial(i);
    v[12]=root?0:static_cast<std::uint32_t>(o.counter); v[13]=root?0:static_cast<std::uint32_t>(o.counter>>32);
    v[14]=o.len; v[15]=o.flags|(root?8:0);
    for(int i=0;i<16;++i) m[i]=o.block[i];
    const int permutation[16]={2,6,3,10,7,0,4,13,1,11,12,5,9,14,15,8};
    for(int r=0;r<7;++r) {
        g(v,0,4,8,12,m[0],m[1]); g(v,1,5,9,13,m[2],m[3]); g(v,2,6,10,14,m[4],m[5]); g(v,3,7,11,15,m[6],m[7]);
        g(v,0,5,10,15,m[8],m[9]); g(v,1,6,11,12,m[10],m[11]); g(v,2,7,8,13,m[12],m[13]); g(v,3,4,9,14,m[14],m[15]);
        for(int i=0;i<16;++i) p[i]=m[permutation[i]];
        for(int i=0;i<16;++i) m[i]=p[i];
    }
    for(int i=0;i<8;++i) out[i]=v[i]^v[i+8];
}
SAME_HD inline Output chunk(const unsigned char* data, unsigned len, std::uint64_t counter) {
    Output o{}; for(int i=0;i<8;++i) o.cv[i]=initial(i); o.counter=counter;
    unsigned blocks=len?((len+63)/64):1;
    for(unsigned b=0;b<blocks;++b) {
        for(int i=0;i<16;++i) o.block[i]=0;
        unsigned n=len-b*64; if(n>64) n=64;
        for(unsigned i=0;i<n;++i) o.block[i/4]|=static_cast<std::uint32_t>(data[b*64+i])<<(8*(i%4));
        o.len=n; o.flags=(b==0?1:0)|(b+1==blocks?2:0);
        if(b+1<blocks) { std::uint32_t cv[8]; compress(o,cv); for(int i=0;i<8;++i) o.cv[i]=cv[i]; }
    }
    return o;
}
inline Output parent(const std::uint32_t* left,const std::uint32_t* right) {
    Output o{}; for(int i=0;i<8;++i) { o.cv[i]=initial(i); o.block[i]=left[i]; o.block[i+8]=right[i]; }
    o.len=64; o.flags=4; return o;
}
}
#undef SAME_HD
