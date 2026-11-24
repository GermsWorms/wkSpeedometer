/* Exact local executable identity gate; no provider DLLs or game calls. */
#ifndef WA_IMAGE_GUARD_H
#define WA_IMAGE_GUARD_H
#include <stdint.h>
#include <string.h>
typedef struct {uint32_t h[8];uint64_t bytes;unsigned used;unsigned char block[64];} wa_sha256;
static uint32_t wa_ror(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void wa_sha_block(wa_sha256 *s,const unsigned char *p){
 static const uint32_t k[64]={
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
 uint32_t w[64],a,b,c,d,e,f,g,h,t,u;unsigned i;
 for(i=0;i<16;i++)w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
 for(i=16;i<64;i++)w[i]=w[i-16]+(wa_ror(w[i-15],7)^wa_ror(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(wa_ror(w[i-2],17)^wa_ror(w[i-2],19)^(w[i-2]>>10));
 a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
 for(i=0;i<64;i++){t=h+(wa_ror(e,6)^wa_ror(e,11)^wa_ror(e,25))+((e&f)^((~e)&g))+k[i]+w[i];u=(wa_ror(a,2)^wa_ror(a,13)^wa_ror(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
 s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void wa_sha_init(wa_sha256 *s){
 const uint32_t initial[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 memset(s,0,sizeof(*s));memcpy(s->h,initial,sizeof(initial));
}
static void wa_sha_update(wa_sha256 *s,const unsigned char *p,size_t n){
 s->bytes+=n;
 while(n){size_t take=64-s->used;if(take>n)take=n;memcpy(s->block+s->used,p,take);s->used+=(unsigned)take;p+=take;n-=take;if(s->used==64){wa_sha_block(s,s->block);s->used=0;}}
}
static void wa_sha_final(wa_sha256 *s,unsigned char digest[32]){
 uint64_t bits=s->bytes*8;unsigned i;
 s->block[s->used++]=0x80;
 if(s->used>56){memset(s->block+s->used,0,64-s->used);wa_sha_block(s,s->block);s->used=0;}
 memset(s->block+s->used,0,56-s->used);
 for(i=0;i<8;i++)s->block[63-i]=(unsigned char)(bits>>(i*8));
 wa_sha_block(s,s->block);
 for(i=0;i<32;i++)digest[i]=(unsigned char)(s->h[i/4]>>(24-(i%4)*8));
}
static void wa_hex(const unsigned char *d,unsigned n,char *out){static const char x[]="0123456789abcdef";unsigned i;for(i=0;i<n;i++){out[2*i]=x[d[i]>>4];out[2*i+1]=x[d[i]&15];}out[2*n]=0;}
/* 1: Steam's WA 3.8.1 (the whole file, SHA-256 d6c449c7...); 2: another WA.exe with the same 3.8.1 code and the same
 * layout (the code section's SHA-256 fbb4fa4b..., and .text / .rdata / .data at the same addresses and sizes): GOG's
 * edition, a copy of Steam's with its Steam text blanked; 0: not it. why: what was found (for the log). */
static int wa_supported_image(const char *path,char *why,size_t whyn){
 static const unsigned char whole[32]={0xd6,0xc4,0x49,0xc7,0x59,0x16,0xa0,0x94,0x1d,0x11,0x63,0xaf,0xc2,0xa9,0x1f,0x1e,0x72,0xa3,0x16,0x00,0x60,0x9b,0xb9,0xb7,0x66,0xf2,0x30,0x2c,0xa8,0x01,0x64,0xeb};
 static const unsigned char text[32]={0xfb,0xb4,0xfa,0x4b,0x5d,0xaf,0x3b,0xc2,0x57,0xb8,0xf0,0xe0,0x64,0xa5,0x66,0x3a,0xbb,0x36,0xf8,0x74,0x04,0xa4,0x68,0x50,0x82,0x1d,0x79,0x71,0x2e,0x26,0xfa,0xf6};
 static const struct{const char *name;uint32_t va,vsize;}layout[3]={{".text",0x1000u,0x2184d3u},{".rdata",0x21a000u,0x79477u},{".data",0x294000u,0x230158u}};
 unsigned char *b=NULL,digest[32];char hx[65];DWORD n=0;LARGE_INTEGER size;wa_sha256 hash;uint32_t pe,opt,nsec,i,k;int result=0;
 HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
 if(why&&whyn)why[0]=0;
 if(file==INVALID_HANDLE_VALUE){if(why)snprintf(why,whyn,"WA.exe can't be read");return 0;}
 if(!GetFileSizeEx(file,&size)||size.QuadPart<4096||size.QuadPart>(64<<20)||!(b=(unsigned char*)malloc((size_t)size.QuadPart))||
    !ReadFile(file,b,(DWORD)size.QuadPart,&n,NULL)||n!=(DWORD)size.QuadPart){CloseHandle(file);free(b);if(why)snprintf(why,whyn,"WA.exe can't be read");return 0;}
 CloseHandle(file);
 wa_sha_init(&hash);wa_sha_update(&hash,b,n);wa_sha_final(&hash,digest);
 if(!memcmp(digest,whole,32)){free(b);if(why)snprintf(why,whyn,"Steam's WA.exe 3.8.1");return 1;}
 wa_hex(digest,32,hx);
 if(why)snprintf(why,whyn,"WA.exe of %lu bytes, SHA-256 %s",(unsigned long)n,hx);
 pe=*(uint32_t*)(b+0x3c);
 if(pe>n-24-40*3||memcmp(b+pe,"PE\0\0",4)){free(b);return 0;}
 nsec=*(uint16_t*)(b+pe+6);opt=*(uint16_t*)(b+pe+20);
 if(nsec<3||pe+24+opt+40*nsec>n){free(b);return 0;}
 for(k=0;k<3;k++){
  const unsigned char *s=b+pe+24+opt+40*k;
  if(strncmp((const char*)s,layout[k].name,8)||*(uint32_t*)(s+12)!=layout[k].va||*(uint32_t*)(s+8)!=layout[k].vsize){
   if(why){size_t at=strlen(why);snprintf(why+at,whyn-at,"; section %u is not 3.8.1's %s",k,layout[k].name);}free(b);return 0;}
 }
 {const unsigned char *s=b+pe+24+opt;uint32_t vs=*(uint32_t*)(s+8),rs=*(uint32_t*)(s+16),rp=*(uint32_t*)(s+20),len=vs<rs?vs:rs;
  if(rp>n||len>n-rp){free(b);return 0;}
  wa_sha_init(&hash);wa_sha_update(&hash,b+rp,len);wa_sha_final(&hash,digest);
  i=memcmp(digest,text,32)==0;wa_hex(digest,32,hx);
  if(why){size_t at=strlen(why);snprintf(why+at,whyn-at,i?"; its code is 3.8.1's (as Steam's)":"; its code is not 3.8.1's (code SHA-256 %s)",hx);}
  result=i?2:0;}
 free(b);return result;
}
#endif
