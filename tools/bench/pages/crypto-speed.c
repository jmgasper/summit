// PBKDF2-HMAC-SHA256 for a WebAssembly throughput probe (no libc).
typedef unsigned int u32;
typedef unsigned char u8;
static const u32 K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define R(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void compress(u32* h, const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4*i]<<24 | (u32)p[4*i+1]<<16 | (u32)p[4*i+2]<<8 | p[4*i+3];
    for (int i = 16; i < 64; i++) { u32 s0 = R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3), s1 = R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10); w[i] = w[i-16]+s0+w[i-7]+s1; }
    u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) { u32 t1 = hh+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+K[i]+w[i]; u32 t2 = (R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c)); hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2; }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}
static void init(u32* h) { h[0]=0x6a09e667; h[1]=0xbb67ae85; h[2]=0x3c6ef372; h[3]=0xa54ff53a; h[4]=0x510e527f; h[5]=0x9b05688c; h[6]=0x1f83d9ab; h[7]=0x5be0cd19; }
// HMAC state after the key pad blocks: inner and outer.
static void digest32(const u32* start, const u8* msg32, u8* out) {
    u32 h[8]; for (int i = 0; i < 8; i++) h[i] = start[i];
    u8 block[64];
    for (int i = 0; i < 32; i++) block[i] = msg32[i];
    block[32] = 0x80; for (int i = 33; i < 62; i++) block[i] = 0;
    block[62] = (64+32)*8 >> 8; block[63] = (u8)((64+32)*8);
    compress(h, block);
    for (int i = 0; i < 8; i++) { out[4*i]=h[i]>>24; out[4*i+1]=h[i]>>16; out[4*i+2]=h[i]>>8; out[4*i+3]=h[i]; }
}
// 32-byte password, 32-byte salt||counter in u (first block already computed by caller is simplified).
__attribute__((export_name("pbkdf2"))) u32 pbkdf2(u32 iterations) {
    u8 key[64]; for (int i = 0; i < 64; i++) key[i] = (u8)(i * 7 + 1);
    u8 pad[64]; u32 ih[8], oh[8];
    init(ih); for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36; compress(ih, pad);
    init(oh); for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5c; compress(oh, pad);
    u8 u[32], t[32], x[32];
    for (int i = 0; i < 32; i++) u[i] = (u8)i;
    for (int i = 0; i < 32; i++) t[i] = 0;
    for (u32 n = 0; n < iterations; n++) {
        digest32(ih, u, x);
        digest32(oh, x, u);
        for (int i = 0; i < 32; i++) t[i] ^= u[i];
    }
    return (u32)t[0] | (u32)t[1] << 8 | (u32)t[2] << 16 | (u32)t[3] << 24;
}
