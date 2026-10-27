typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef short s16;
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned long long u64;
typedef long long s64;
extern u8 b8[64];
extern u16 h16[32];
extern u32 w32[32];
struct __attribute__((packed)) pk_t { u8 pad; u32 w; };
extern struct pk_t pk[8];
#define sp ((volatile u32 *)0x1f800000)   // scratchpad
u32 hlp0(u32, u32);
u32 hlp1(u32, u32);
u32 hlp2(u32, u32);
u32 hlp3(u32, u32);
extern u32 (*const hlp[4])(u32, u32);
