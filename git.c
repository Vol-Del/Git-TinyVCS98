/*
 * TinyVCS98 - a small Git-compatible command-line client for Windows 98.
 *
 * Reads/writes standard SHA-1 Git objects and index v2, reads Git packfiles,
 * and implements Git smart HTTP protocol v0 over http:// and https:// (built-in TLS 1.2).
 *
 * Intended target: Open Watcom C/C++ 2.x, 32-bit Win32 (Windows 95/98/Me).
 * The source is written in conservative C and can also be built on POSIX for
 * interoperability tests. See README.md for transport and format limitations.
 *
 * Copyright (c) 2026. MIT License; see LICENSE.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#if defined(__WATCOMC__)
# include <direct.h>
# include <io.h>
# include <windows.h>
# include <winsock.h>
# define CHDIR _chdir
# define PATH_SEP '\\'
# define MKDIR(p) mkdir(p)
# define IS_DIR(st) (((st).st_mode & S_IFMT) == S_IFDIR)
# define STRICMP(a,b) stricmp((a),(b))
# define STRNICMP(a,b,n) strnicmp((a),(b),(n))
#else
# include <unistd.h>
# include <sys/time.h>
# include <dirent.h>
# include <sys/socket.h>
# include <netdb.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# define CHDIR chdir
# define PATH_SEP '/'
# define MKDIR(p) mkdir((p), 0777)
# define IS_DIR(st) S_ISDIR((st).st_mode)
# define STRICMP(a,b) ascii_casecmp((a),(b))
# define STRNICMP(a,b,n) ascii_ncasecmp((a),(b),(n))
#endif

#if !defined(__WATCOMC__)
static int ascii_casecmp(const char *a,const char *b)
{ for(;*a&&*b;++a,++b){int x=tolower((unsigned char)*a),y=tolower((unsigned char)*b);if(x!=y)return x-y;}return (unsigned char)*a-(unsigned char)*b; }
static int ascii_ncasecmp(const char *a,const char *b,size_t n)
{ size_t i;for(i=0;i<n&&a[i]&&b[i];++i){int x=tolower((unsigned char)a[i]),y=tolower((unsigned char)b[i]);if(x!=y)return x-y;}if(i==n)return 0;return (unsigned char)a[i]-(unsigned char)b[i];}
#endif

#define MAX_PATH_GIT 1024
#define MAX_ENTRIES 12000UL
#define MAX_OBJECT (64UL * 1024UL * 1024UL)
#define MAX_DEPTH 32

typedef unsigned char U8;
typedef unsigned int U32;
typedef struct { U8 *data; unsigned long len; } Buffer;
typedef struct { char path[MAX_PATH_GIT]; U8 oid[20]; U32 mode; } IndexEntry;
typedef struct { IndexEntry *v; unsigned long n, cap; } Index;
typedef struct { unsigned short len[288]; unsigned short code[288]; unsigned short sym[288]; unsigned int n; } Huff;

static const char *gitdir = ".git";

/* -------------------------- filesystem helpers -------------------------- */
static int path_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t n, m;
    if (!a || !*a) { if (strlen(b) + 1 > cap) return 0; strcpy(out, b); return 1; }
    n = strlen(a); m = strlen(b);
    if (n + m + 2 > cap) return 0;
    strcpy(out, a);
    if (out[n-1] != '/' && out[n-1] != '\\') out[n++] = PATH_SEP;
    memcpy(out+n, b, m+1);
    return 1;
}
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int dir_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && IS_DIR(st); }
static int make_dir(const char *p)
{
    if (dir_exists(p)) return 1;
    if (MKDIR(p) == 0) return 1;
    return errno == EEXIST && dir_exists(p);
}
static int mkdirs(const char *p)
{
    char tmp[MAX_PATH_GIT]; size_t i, n;
    if (!p || !*p || strlen(p) >= sizeof(tmp)) return 0;
    strcpy(tmp, p); n = strlen(tmp);
    for (i=1; i<n; ++i) if (tmp[i]=='/' || tmp[i]=='\\') {
        char c=tmp[i];
        if (i==2 && tmp[1]==':') continue;            /* drive prefix "C:" is not a directory */
        tmp[i]='\0';
        if (*tmp && !make_dir(tmp)) { tmp[i]=c; return 0; }
        tmp[i]=c;
    }
    return make_dir(tmp);
}
static int ensure_parent(const char *p)
{
    char tmp[MAX_PATH_GIT]; char *s1, *s2, *s;
    if (strlen(p) >= sizeof(tmp)) return 0;
    strcpy(tmp, p); s1=strrchr(tmp,'/'); s2=strrchr(tmp,'\\'); s=s1;
    if (!s || (s2 && s2>s)) s=s2;
    if (!s) return 1;
    *s='\0';
    if (!*tmp) return 1;
    return mkdirs(tmp);
}
static int read_file(const char *p, Buffer *b)
{
    FILE *f; long n;
    b->data=NULL; b->len=0; f=fopen(p,"rb"); if(!f)return 0;
    if(fseek(f,0,SEEK_END)!=0 || (n=ftell(f))<0 || (unsigned long)n>MAX_OBJECT || fseek(f,0,SEEK_SET)!=0){fclose(f);return 0;}
    b->data=(U8*)malloc((size_t)n+1);if(!b->data){fclose(f);return 0;}
    if(n && fread(b->data,1,(size_t)n,f)!=(size_t)n){free(b->data);b->data=NULL;fclose(f);return 0;}
    b->data[n]=0;b->len=(unsigned long)n;fclose(f);return 1;
}
static int write_file(const char *p,const U8 *data,unsigned long n)
{
    FILE *f;if(!ensure_parent(p))return 0;f=fopen(p,"wb");if(!f)return 0;
    if(n&&fwrite(data,1,n,f)!=n){fclose(f);return 0;}return fclose(f)==0;
}
static int remove_file(const char *p) { return remove(p)==0 || errno==ENOENT; }
static int buf_append(Buffer *b,const void *p,unsigned long n)
{
    U8 *q;if(n>MAX_OBJECT || b->len>MAX_OBJECT-n)return 0;
    q=(U8*)realloc(b->data,(size_t)(b->len+n+1));if(!q)return 0;b->data=q;
    if(n)memcpy(b->data+b->len,p,n);
    b->len+=n;b->data[b->len]=0;return 1;
}
static int buf_byte(Buffer *b,U8 c){return buf_append(b,&c,1);}
/* Growth-managed output for inflate: doubles capacity instead of reallocating per byte. */
static int grow_reserve(Buffer *b,unsigned long *cap,unsigned long n)
{
    if(n>MAX_OBJECT||b->len>MAX_OBJECT-n)return 0;
    if(b->len+n+1>*cap){
        unsigned long nc=*cap?*cap:4096UL;U8 *q;
        while(nc<b->len+n+1)nc*=2;
        q=(U8*)realloc(b->data,(size_t)nc);if(!q)return 0;
        b->data=q;*cap=nc;
    }
    return 1;
}
static int grow_put(Buffer *b,unsigned long *cap,U8 c){if(!grow_reserve(b,cap,1))return 0;b->data[b->len++]=c;return 1;}
static int grow_put_n(Buffer *b,unsigned long *cap,const U8 *p,unsigned long n)
{
    if(!grow_reserve(b,cap,n))return 0;
    if(n)memcpy(b->data+b->len,p,(size_t)n);
    b->len+=n;return 1;
}
/* Terminate and trim a grow-managed buffer to the exact size used elsewhere in this program. */
static void grow_finish(Buffer *b)
{
    U8 *q;
    if(!b->data)return;
    b->data[b->len]=0;
    q=(U8*)realloc(b->data,(size_t)b->len+1);if(q)b->data=q;
}
static void to_fs_path(char *out,const char *rel)
{
    size_t i;strcpy(out,rel);for(i=0;out[i];++i)if(out[i]=='/'||out[i]=='\\')out[i]=PATH_SEP;
}
static int make_rel_path(char *out,size_t cap,const char *prefix,const char *name)
{
    size_t a=(prefix&&*prefix)?strlen(prefix):0,b=strlen(name);
    if(a+b+(a?2:1)>cap)return 0;
    if(a){memcpy(out,prefix,a);out[a]='/';memcpy(out+a+1,name,b+1);}else memcpy(out,name,b+1);
    return 1;
}
static int normalize_path(const char *in,char *out,size_t cap)
{
    size_t i,n,start,clen;
    if(!in||!*in||strlen(in)>=cap||in[0]=='/'||in[0]=='\\'||strchr(in,':'))return 0;
    n=strlen(in);
    for(i=0;i<n;++i){unsigned char c=(unsigned char)in[i];if(c<32||c==127)return 0;out[i]=(in[i]=='\\')?'/':in[i];}
    out[n]=0;start=0;
    for(i=0;i<=n;++i)if(out[i]=='/'||out[i]==0){clen=i-start;if(!clen||(clen==1&&out[start]=='.')||(clen==2&&out[start]=='.'&&out[start+1]=='.'))return 0;if(start==0&&clen==4&&STRNICMP(out,".git",4)==0)return 0;start=i+1;}
    return 1;
}

/* -------------------------------- SHA-1 ---------------------------------- */
typedef struct { U32 h[5], hi, lo; U8 block[64]; unsigned int used; } SHA1;
static U32 rol(U32 x,unsigned int n){return (x<<n)|(x>>(32-n));}
static U32 get32be(const U8 *p){return ((U32)p[0]<<24)|((U32)p[1]<<16)|((U32)p[2]<<8)|p[3];}
static void put32be(U8 *p,U32 x){p[0]=(U8)(x>>24);p[1]=(U8)(x>>16);p[2]=(U8)(x>>8);p[3]=(U8)x;}
static void put16be(U8 *p,unsigned int x){p[0]=(U8)(x>>8);p[1]=(U8)x;}
static unsigned int get16be(const U8 *p){return ((unsigned int)p[0]<<8)|p[1];}
static void sha1_block(SHA1 *s,const U8 *p)
{
    U32 w[80],a,b,c,d,e,f,k,t;int i;
    for(i=0;i<16;++i)w[i]=get32be(p+i*4);
    for(i=16;i<80;++i)w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];
    for(i=0;i<80;++i){
        if(i<20){f=(b&c)|((~b)&d);k=0x5a827999UL;}
        else if(i<40){f=b^c^d;k=0x6ed9eba1UL;}
        else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdcUL;}
        else{f=b^c^d;k=0xca62c1d6UL;}
        t=rol(a,5)+f+e+k+w[i];e=d;d=c;c=rol(b,30);b=a;a=t;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;
}
static void sha1_init(SHA1 *s)
{
    s->h[0]=0x67452301UL;s->h[1]=0xefcdab89UL;s->h[2]=0x98badcfeUL;s->h[3]=0x10325476UL;s->h[4]=0xc3d2e1f0UL;s->hi=s->lo=0;s->used=0;
}
static void sha1_update(SHA1 *s,const U8 *p,unsigned long n)
{
    U32 old=s->lo;s->lo+=(U32)(n<<3);if(s->lo<old)++s->hi;s->hi+=(U32)(n>>29);
    while(n){unsigned int take=(unsigned int)(64-s->used);if(take>n)take=(unsigned int)n;memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;if(s->used==64){sha1_block(s,s->block);s->used=0;}}
}
static void sha1_final(SHA1 *s,U8 out[20])
{
    U32 hi=s->hi,lo=s->lo;U8 pad[72];unsigned int n,i;pad[0]=0x80;n=(s->used<56)?(56-s->used):(120-s->used);memset(pad+1,0,n-1);sha1_update(s,pad,n);put32be(pad,hi);put32be(pad+4,lo);sha1_update(s,pad,8);for(i=0;i<5;++i)put32be(out+i*4,s->h[i]);
}
static void sha1_data(const U8 *p,unsigned long n,U8 out[20]){SHA1 s;sha1_init(&s);sha1_update(&s,p,n);sha1_final(&s,out);}
static void oid_to_hex(const U8 oid[20],char out[41]){static const char h[]="0123456789abcdef";int i;for(i=0;i<20;++i){out[2*i]=h[oid[i]>>4];out[2*i+1]=h[oid[i]&15];}out[40]=0;}
static int hex_to_oid(const char *s,U8 oid[20])
{
    int i; if(strlen(s)!=40)return 0;for(i=0;i<20;++i){int a,b;char ca=(char)tolower((unsigned char)s[2*i]),cb=(char)tolower((unsigned char)s[2*i+1]);a=isdigit((unsigned char)ca)?ca-'0':ca-'a'+10;b=isdigit((unsigned char)cb)?cb-'0':cb-'a'+10;if(a<0||a>15||b<0||b>15)return 0;oid[i]=(U8)((a<<4)|b);}return 1;
}

/* ------------------------ minimal DEFLATE inflater ---------------------- */
typedef struct {const U8 *p;unsigned long n,bit;} BitReader;
static int bits_read(BitReader *r,unsigned int n,unsigned long *out)
{
    unsigned int i;unsigned long v=0;for(i=0;i<n;++i){if(r->bit>=r->n*8UL)return 0;v|=(unsigned long)((r->p[r->bit>>3]>>(r->bit&7))&1)<<i;++r->bit;}*out=v;return 1;
}
static unsigned int reverse_bits(unsigned int x,unsigned int n)
{
    unsigned int y=0,i;for(i=0;i<n;++i){y=(y<<1)|(x&1);x>>=1;}return y;
}
static int huff_make(Huff *h,const U8 *lengths,unsigned int n)
{
    unsigned int count[16],next[16],code,i,bits,k;memset(count,0,sizeof(count));h->n=n;
    for(i=0;i<n;++i){h->len[i]=lengths[i];if(lengths[i]>15)return 0;if(lengths[i])++count[lengths[i]];}
    code=0;count[0]=0;
    for(bits=1;bits<=15;++bits){code=(code+count[bits-1])<<1;next[bits]=code;if(code+count[bits]>(1U<<bits))return 0;}
    for(i=0;i<n;++i)if(lengths[i]){k=lengths[i];h->code[i]=(unsigned short)reverse_bits(next[k]++,k);}
    return 1;
}
static int huff_read(BitReader *r,const Huff *h,unsigned int *sym)
{
    unsigned int len,i;unsigned long bit,code=0;for(len=1;len<=15;++len){if(!bits_read(r,1,&bit))return 0;code|=bit<<(len-1);for(i=0;i<h->n;++i)if(h->len[i]==len&&h->code[i]==code){*sym=i;return 1;}}return 0;
}
static int inflate_raw(const U8 *in,unsigned long inlen,Buffer *out,unsigned long *consumed)
{
    static const unsigned short lb[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const U8 le[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned short db[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const U8 de[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static const U8 order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    BitReader r;Huff lit,dist,cl;U8 ll[288],dd[32],clen[19];unsigned long final,bt,x,y;unsigned int i,sym,hl,hd,hc,pos,repeat,ds;int done=0;unsigned long cap=0;
    out->data=NULL;out->len=0;r.p=in;r.n=inlen;r.bit=0;
    while(!done){
        if(!bits_read(&r,1,&final)||!bits_read(&r,2,&bt))goto bad;
        done=(int)final;
        if(bt==0){unsigned long len,nlen;r.bit=(r.bit+7UL)&~7UL;if(!bits_read(&r,16,&len)||!bits_read(&r,16,&nlen)||((len^0xffffUL)&0xffffUL)!=(nlen&0xffffUL))goto bad;if(r.bit/8+len>r.n||!grow_put_n(out,&cap,r.p+r.bit/8,len))goto bad;r.bit+=len*8;}
        else if(bt==1||bt==2){
            if(bt==1){for(i=0;i<=143;++i)ll[i]=8;for(i=144;i<=255;++i)ll[i]=9;for(i=256;i<=279;++i)ll[i]=7;for(i=280;i<288;++i)ll[i]=8;for(i=0;i<32;++i)dd[i]=5;if(!huff_make(&lit,ll,288)||!huff_make(&dist,dd,32))goto bad;}
            else {if(!bits_read(&r,5,&x))goto bad;hl=(unsigned int)x+257;if(!bits_read(&r,5,&x))goto bad;hd=(unsigned int)x+1;if(!bits_read(&r,4,&x))goto bad;hc=(unsigned int)x+4;memset(clen,0,sizeof(clen));for(i=0;i<hc;++i){if(!bits_read(&r,3,&x))goto bad;clen[order[i]]=(U8)x;}if(!huff_make(&cl,clen,19))goto bad;pos=0;while(pos<hl+hd){if(!huff_read(&r,&cl,&sym))goto bad;if(sym<=15){if(pos<hl)ll[pos]=(U8)sym;else dd[pos-hl]=(U8)sym;++pos;}else if(sym==16){if(pos==0||!bits_read(&r,2,&x))goto bad;repeat=(unsigned int)x+3;while(repeat--){if(pos>=hl+hd)goto bad;if(pos<hl)ll[pos]=ll[pos-1];else dd[pos-hl]=dd[pos-hl-1];++pos;}}else if(sym==17){if(!bits_read(&r,3,&x))goto bad;repeat=(unsigned int)x+3;while(repeat--){if(pos>=hl+hd)goto bad;if(pos<hl)ll[pos]=0;else dd[pos-hl]=0;++pos;}}else if(sym==18){if(!bits_read(&r,7,&x))goto bad;repeat=(unsigned int)x+11;while(repeat--){if(pos>=hl+hd)goto bad;if(pos<hl)ll[pos]=0;else dd[pos-hl]=0;++pos;}}else goto bad;}if(!huff_make(&lit,ll,hl)||!huff_make(&dist,dd,hd))goto bad;}
            for(;;){unsigned long length,distance;if(!huff_read(&r,&lit,&sym))goto bad;if(sym<256){if(!grow_put(out,&cap,(U8)sym))goto bad;}else if(sym==256)break;else if(sym>=257&&sym<=285){if(!bits_read(&r,le[sym-257],&x))goto bad;length=lb[sym-257]+x;if(!huff_read(&r,&dist,&ds)||ds>29||!bits_read(&r,de[ds],&y))goto bad;distance=db[ds]+y;if(distance==0||distance>out->len)goto bad;while(length--){U8 c=out->data[out->len-distance];if(!grow_put(out,&cap,c))goto bad;}}else goto bad;}
        }else goto bad;
        if(out->len>MAX_OBJECT)goto bad;
    }
    grow_finish(out);
    if(consumed)*consumed=(r.bit+7UL)/8UL;
    return 1;
bad:if(out->data)free(out->data);out->data=NULL;out->len=0;return 0;
}
static U32 adler32_data(const U8 *p,unsigned long n)
{
    U32 a=1,b=0;unsigned long i;for(i=0;i<n;++i){a=(a+p[i])%65521UL;b=(b+a)%65521UL;}return (b<<16)|a;
}
static int zlib_inflate(const U8 *in,unsigned long n,Buffer *out)
{
    unsigned long used;U32 sum;if(n<6||(in[0]&15)!=8||((((unsigned int)in[0]<<8)|in[1])%31)!=0)return 0;if(!inflate_raw(in+2,n-6,out,&used))return 0;if(used>n-6){free(out->data);out->data=NULL;return 0;}sum=adler32_data(out->data,out->len);if(sum!=get32be(in+n-4)){free(out->data);out->data=NULL;out->len=0;return 0;}return 1;
}
static int zlib_inflate_partial(const U8 *in,unsigned long n,Buffer *out,unsigned long *consumed)
{
    unsigned long used;U32 sum;if(n<6||(in[0]&15)!=8||((((unsigned int)in[0]<<8)|in[1])%31)!=0)return 0;
    if(!inflate_raw(in+2,n-2,out,&used))return 0;
    if(used+6>n){free(out->data);out->data=NULL;out->len=0;return 0;}
    sum=adler32_data(out->data,out->len);if(sum!=get32be(in+2+used)){free(out->data);out->data=NULL;out->len=0;return 0;}if(consumed)*consumed=used+6;return 1;
}
static int zlib_deflate_stored(const U8 *in,unsigned long n,Buffer *out)
{
    unsigned long pos=0;U8 h[2]={0x78,0x01};U32 sum;out->data=NULL;out->len=0;if(!buf_append(out,h,2))return 0;
    do{unsigned long remain=n-pos;unsigned int z=(unsigned int)(remain>65535UL?65535UL:remain);U8 block[5];int last=(pos+z==n);block[0]=(U8)(last?1:0);block[1]=(U8)z;block[2]=(U8)(z>>8);block[3]=(U8)(~z);block[4]=(U8)((~z)>>8);if(!buf_append(out,block,5)||!buf_append(out,in+pos,z))goto fail;pos+=z;}while(pos<n||(n==0&&out->len==2));
    sum=adler32_data(in,n);{U8 tail[4];put32be(tail,sum);if(!buf_append(out,tail,4))goto fail;}return 1;
fail:free(out->data);out->data=NULL;out->len=0;return 0;
}

/* ----------------------------- Git objects ------------------------------- */
static int object_path(const U8 oid[20],char *out,size_t cap)
{
    char hex[41],rel[64];oid_to_hex(oid,hex);sprintf(rel,"objects/%c%c/%s",hex[0],hex[1],hex+2);return path_join(out,cap,gitdir,rel);
}
static int store_object(const char *type,const U8 *data,unsigned long len,U8 oid[20])
{
    char header[64],path[MAX_PATH_GIT],dir[MAX_PATH_GIT];Buffer raw,z;int n;SHA1 sh;
    n=sprintf(header,"%s %lu",type,len)+1;raw.data=NULL;raw.len=0;if(!buf_append(&raw,header,(unsigned long)n)||!buf_append(&raw,data,len))goto bad;
    sha1_init(&sh);sha1_update(&sh,raw.data,raw.len);sha1_final(&sh,oid);if(!object_path(oid,path,sizeof(path)))goto bad;if(file_exists(path)){free(raw.data);return 1;}
    if(!zlib_deflate_stored(raw.data,raw.len,&z))goto bad;
    strcpy(dir,path);{char *s=strrchr(dir,PATH_SEP);if(!s)s=strrchr(dir,'/');if(s)*s=0;}
    if(!mkdirs(dir)||!write_file(path,z.data,z.len)){free(z.data);goto bad;}free(z.data);free(raw.data);return 1;
bad:if(raw.data)free(raw.data);return 0;
}
static int parse_object_header(const U8 *raw,unsigned long n,char *type,unsigned long *off,unsigned long *len)
{
    unsigned long p=0,size=0;while(p<n&&raw[p]!=' ')++p;if(p==n||p>15)return 0;memcpy(type,raw,p);type[p]=0;++p;if(p>=n||!isdigit(raw[p]))return 0;while(p<n&&isdigit(raw[p])){if(size>MAX_OBJECT/10)return 0;size=size*10+(raw[p]-'0');++p;}if(p>=n||raw[p]!=0||size!=n-p-1)return 0;*off=p+1;*len=size;return 1;
}
static int find_packed_offset(const U8 oid[20],char *packpath,size_t cap,unsigned long *offset)
{
#if defined(__WATCOMC__)
    char pattern[MAX_PATH_GIT],idxpath[MAX_PATH_GIT],fullpack[MAX_PATH_GIT],name[260];
    struct _finddata_t fd;long h;
    if(!path_join(pattern,sizeof(pattern),gitdir,"objects\\pack\\*.idx"))return 0;
    h=_findfirst(pattern,&fd);if(h==-1L)return 0;
    do {
        Buffer b;unsigned long n,count,base,lo,hi,mid,off;U32 ver;
        if(!path_join(idxpath,sizeof(idxpath),gitdir,"objects\\pack\\"))continue;
        if(strlen(idxpath)+strlen(fd.name)>=sizeof(idxpath))continue;
        strcat(idxpath,fd.name);if(!read_file(idxpath,&b))continue;n=b.len;off=0;
        if(n>=1064 && b.data[0]==0xff&&b.data[1]=='t'&&b.data[2]=='O'&&b.data[3]=='c'){
            ver=get32be(b.data+4);count=get32be(b.data+8+255*4);base=8+1024;
            if(ver==2 && base+count*28UL+40<=n){lo=0;hi=count;while(lo<hi){mid=(lo+hi)/2;if(memcmp(b.data+base+mid*20,oid,20)<0)lo=mid+1;else hi=mid;}if(lo<count&&memcmp(b.data+base+lo*20,oid,20)==0){off=get32be(b.data+base+count*24UL+lo*4UL);if(off&0x80000000UL){unsigned long li=off&0x7fffffffUL,lp=base+count*28UL+li*8UL;if(lp+8<=n){if(get32be(b.data+lp)!=0)off=0;else off=get32be(b.data+lp+4);}else off=0;}}}
        } else if(n>=1064){count=get32be(b.data+255*4);base=1024;lo=0;hi=count;while(lo<hi){mid=(lo+hi)/2;if(memcmp(b.data+base+mid*24+4,oid,20)<0)lo=mid+1;else hi=mid;}if(lo<count&&memcmp(b.data+base+lo*24+4,oid,20)==0)off=get32be(b.data+base+lo*24);}
        if(off){strcpy(name,fd.name);name[strlen(name)-4]=0;strcat(name,".pack");if(path_join(fullpack,sizeof(fullpack),gitdir,"objects\\pack\\")&&strlen(fullpack)+strlen(name)<sizeof(fullpack)){strcat(fullpack,name);if(strlen(fullpack)<cap){strcpy(packpath,fullpack);*offset=off;free(b.data);_findclose(h);return 1;}}}
        free(b.data);
    } while(_findnext(h,&fd)==0);
    _findclose(h);return 0;
#else
    DIR *d;struct dirent *e;char dir[MAX_PATH_GIT],idxpath[MAX_PATH_GIT],fullpack[MAX_PATH_GIT],name[260];
    if(!path_join(dir,sizeof(dir),gitdir,"objects/pack"))return 0;
    d=opendir(dir);if(!d)return 0;
    while((e=readdir(d))!=NULL){Buffer b;unsigned long n,count,base,lo,hi,mid,off;U32 ver;size_t elen;
        elen=strlen(e->d_name);if(elen<5||strcmp(e->d_name+elen-4,".idx")!=0)continue;
        if(!path_join(idxpath,sizeof(idxpath),dir,e->d_name)||!read_file(idxpath,&b))continue;
        n=b.len;off=0;
        if(n>=1064 && b.data[0]==0xff&&b.data[1]=='t'&&b.data[2]=='O'&&b.data[3]=='c'){
            ver=get32be(b.data+4);count=get32be(b.data+8+255*4);base=8+1024;
            if(ver==2 && base+count*28UL+40<=n){lo=0;hi=count;while(lo<hi){mid=(lo+hi)/2;if(memcmp(b.data+base+mid*20,oid,20)<0)lo=mid+1;else hi=mid;}if(lo<count&&memcmp(b.data+base+lo*20,oid,20)==0){off=get32be(b.data+base+count*24UL+lo*4UL);if(off&0x80000000UL){unsigned long li=off&0x7fffffffUL,lp=base+count*28UL+li*8UL;if(lp+8<=n){if(get32be(b.data+lp)!=0)off=0;else off=get32be(b.data+lp+4);}else off=0;}}}
        } else if(n>=1064){count=get32be(b.data+255*4);base=1024;lo=0;hi=count;while(lo<hi){mid=(lo+hi)/2;if(memcmp(b.data+base+mid*24+4,oid,20)<0)lo=mid+1;else hi=mid;}if(lo<count&&memcmp(b.data+base+lo*24+4,oid,20)==0)off=get32be(b.data+base+lo*24);}
        if(off){strcpy(name,e->d_name);name[elen-4]=0;strcat(name,".pack");if(path_join(fullpack,sizeof(fullpack),dir,name)&&strlen(fullpack)<cap){strcpy(packpath,fullpack);*offset=off;free(b.data);closedir(d);return 1;}}
        free(b.data);
    }
    closedir(d);return 0;
#endif
}
static int delta_apply(const U8 *base,unsigned long baselen,const U8 *delta,unsigned long dlen,Buffer *result)
{
    unsigned long p=0,base_size=0,result_size=0,shift=0,off,size;U8 c;unsigned int i;
    for(i=0;i<5;++i){if(p>=dlen)return 0;c=delta[p++];base_size|=(unsigned long)(c&0x7f)<<shift;if(!(c&0x80))break;shift+=7;}
    if(i==5||base_size!=baselen)return 0;
    shift=0;for(i=0;i<5;++i){if(p>=dlen)return 0;c=delta[p++];result_size|=(unsigned long)(c&0x7f)<<shift;if(!(c&0x80))break;shift+=7;}
    if(i==5||result_size>MAX_OBJECT)return 0;
    result->len=0;result->data=(U8*)malloc((size_t)result_size+1);if(!result->data)return 0;
    while(p<dlen){c=delta[p++];if(c&0x80){off=0;size=0;if(c&1){if(p>=dlen)goto bad;off=delta[p++];}if(c&2){if(p>=dlen)goto bad;off|=(unsigned long)delta[p++]<<8;}if(c&4){if(p>=dlen)goto bad;off|=(unsigned long)delta[p++]<<16;}if(c&8){if(p>=dlen)goto bad;off|=(unsigned long)delta[p++]<<24;}if(c&16){if(p>=dlen)goto bad;size=delta[p++];}if(c&32){if(p>=dlen)goto bad;size|=(unsigned long)delta[p++]<<8;}if(c&64){if(p>=dlen)goto bad;size|=(unsigned long)delta[p++]<<16;}if(size==0)size=0x10000UL;if(off>baselen||size>baselen-off||size>result_size-result->len||(memcpy(result->data+result->len,base+off,size),result->len+=size,0))goto bad;}
        else {size=c;if(size==0||size>dlen-p||size>result_size-result->len||(memcpy(result->data+result->len,delta+p,size),result->len+=size,0))goto bad;p+=size;}}
    if(result->len!=result_size)goto bad;
    result->data[result->len]=0;
    return 1;
bad:if(result->data)free(result->data);result->data=NULL;result->len=0;return 0;
}
static int read_pack_entry(const char *packpath,unsigned long offset,int depth,Buffer *body,char *type_name);
static int read_object_depth(const U8 oid[20],Buffer *body,char *type,int depth)
{
    char path[MAX_PATH_GIT],ot[16],packpath[MAX_PATH_GIT];Buffer z,raw;unsigned long off,len;
    if(depth>MAX_DEPTH)return 0;
    if(object_path(oid,path,sizeof(path))&&read_file(path,&z)){
        if(zlib_inflate(z.data,z.len,&raw)){U8 check[20];free(z.data);if(parse_object_header(raw.data,raw.len,ot,&off,&len)){sha1_data(raw.data,raw.len,check);if(!memcmp(check,oid,20)){body->data=(U8*)malloc((size_t)len+1);if(!body->data){free(raw.data);return 0;}memcpy(body->data,raw.data+off,len);body->data[len]=0;body->len=len;strcpy(type,ot);free(raw.data);return 1;}}free(raw.data);}else free(z.data);
    }
    if(find_packed_offset(oid,packpath,sizeof(packpath),&off)){
        if(read_pack_entry(packpath,off,depth+1,body,type)){
            char header[64];int hn;Buffer canonical;U8 check[20];
            hn=sprintf(header,"%s %lu",type,body->len)+1;canonical.data=NULL;canonical.len=0;
            if(!buf_append(&canonical,header,(unsigned long)hn)||!buf_append(&canonical,body->data,body->len)){if(canonical.data)free(canonical.data);free(body->data);body->data=NULL;return 0;}
            sha1_data(canonical.data,canonical.len,check);free(canonical.data);
            if(memcmp(check,oid,20)!=0){free(body->data);body->data=NULL;body->len=0;return 0;}
            return 1;
        }
    }
    fprintf(stderr,"git: object unavailable (not found loose or in supported pack index)\n");return 0;
}
static int read_pack_entry(const char *packpath,unsigned long offset,int depth,Buffer *body,char *type_name)
{
    Buffer pack,compressed,base,result;unsigned long p,size,used,baseoff=0;unsigned int objtype,i;U8 c,base_oid[20];char base_type[16];
    if(depth>MAX_DEPTH||!read_file(packpath,&pack)||pack.len<32||memcmp(pack.data,"PACK",4)!=0){return 0;}
    if(get32be(pack.data+4)<2||get32be(pack.data+4)>3||offset>=pack.len-20){free(pack.data);return 0;}
    p=offset;c=pack.data[p++];objtype=(c>>4)&7;size=c&15;i=4;
    while(c&0x80){if(p>=pack.len-20||i>28){free(pack.data);return 0;}c=pack.data[p++];size|=(unsigned long)(c&0x7f)<<i;i+=7;}
    if(objtype==6){if(p>=pack.len-20){free(pack.data);return 0;}c=pack.data[p++];baseoff=c&0x7f;while(c&0x80){if(p>=pack.len-20){free(pack.data);return 0;}c=pack.data[p++];baseoff=((baseoff+1)<<7)|(c&0x7f);}if(baseoff>offset){free(pack.data);return 0;}baseoff=offset-baseoff;}
    else if(objtype==7){if(p+20>pack.len-20){free(pack.data);return 0;}memcpy(base_oid,pack.data+p,20);p+=20;}
    if(!zlib_inflate_partial(pack.data+p,pack.len-20-p,&compressed,&used)){free(pack.data);return 0;}free(pack.data);if(compressed.len!=size){free(compressed.data);return 0;}
    if(objtype>=1&&objtype<=4){body->data=compressed.data;body->len=compressed.len;strcpy(type_name,objtype==1?"commit":objtype==2?"tree":objtype==3?"blob":"tag");return 1;}
    if(objtype==6){if(!read_pack_entry(packpath,baseoff,depth+1,&base,base_type)){free(compressed.data);return 0;}}
    else if(objtype==7){if(!read_object_depth(base_oid,&base,base_type,depth+1)){free(compressed.data);return 0;}}
    else {free(compressed.data);return 0;}
    if(!delta_apply(base.data,base.len,compressed.data,compressed.len,&result)){free(base.data);free(compressed.data);return 0;}
    free(base.data);free(compressed.data);body->data=result.data;body->len=result.len;strcpy(type_name,base_type);return 1;
}
static int read_object(const U8 oid[20],Buffer *body,char *type){return read_object_depth(oid,body,type,0);}

static int file_blob(const char *path,U8 oid[20])
{
    Buffer f;int ok;if(!read_file(path,&f))return 0;ok=store_object("blob",f.data,f.len,oid);free(f.data);return ok;
}
static int hash_file_blob(const char *path,U8 oid[20])
{
    Buffer f;char header[64];int n;SHA1 sh;if(!read_file(path,&f))return 0;
    n=sprintf(header,"blob %lu",f.len)+1;sha1_init(&sh);sha1_update(&sh,(const U8*)header,(unsigned long)n);sha1_update(&sh,f.data,f.len);sha1_final(&sh,oid);free(f.data);return 1;
}

/* ------------------------------ Git index -------------------------------- */
static void index_free(Index *idx){if(idx->v)free(idx->v);idx->v=NULL;idx->n=idx->cap=0;}
static int index_add(Index *idx,const char *path,const U8 oid[20],U32 mode)
{
    unsigned long i;IndexEntry *q;for(i=0;i<idx->n;++i)if(strcmp(idx->v[i].path,path)==0){memcpy(idx->v[i].oid,oid,20);idx->v[i].mode=mode;return 1;}
    if(idx->n>=MAX_ENTRIES)return 0;
    if(idx->n==idx->cap){unsigned long cap=idx->cap?idx->cap*2:64;q=(IndexEntry*)realloc(idx->v,(size_t)cap*sizeof(IndexEntry));if(!q)return 0;idx->v=q;idx->cap=cap;}
    strcpy(idx->v[idx->n].path,path);memcpy(idx->v[idx->n].oid,oid,20);idx->v[idx->n].mode=mode;++idx->n;return 1;
}
static int index_remove(Index *idx,const char *path)
{
    unsigned long i;for(i=0;i<idx->n;++i)if(strcmp(idx->v[i].path,path)==0){memmove(&idx->v[i],&idx->v[i+1],(size_t)(idx->n-i-1)*sizeof(IndexEntry));--idx->n;return 1;}return 0;
}
static int entry_cmp(const void *a,const void *b){return strcmp(((const IndexEntry*)a)->path,((const IndexEntry*)b)->path);}
static int index_read(Index *idx)
{
    char path[MAX_PATH_GIT];Buffer b;unsigned long pos,i,count;U32 version;U8 sum[20];
    idx->v=NULL;idx->n=idx->cap=0;if(!path_join(path,sizeof(path),gitdir,"index"))return 0;if(!file_exists(path))return 1;if(!read_file(path,&b))return 0;
    if(b.len<32||memcmp(b.data,"DIRC",4)!=0)goto bad;
    version=get32be(b.data+4);count=get32be(b.data+8);if((version!=2&&version!=3)||count>MAX_ENTRIES)goto bad;
    sha1_data(b.data,b.len-20,sum);if(memcmp(sum,b.data+b.len-20,20)!=0)goto bad;
    pos=12;for(i=0;i<count;++i){unsigned long entry_start,path_start,path_len,pad;U32 mode;unsigned int flags;
        entry_start=pos;if(pos+62>b.len-20)goto bad;mode=get32be(b.data+pos+24);flags=get16be(b.data+pos+60);if((flags&0x3000)!=0 || (mode&0170000)==0040000)goto bad;pos+=62;if(flags&0x4000){if(version<3||pos+2>b.len-20)goto bad;if(get16be(b.data+pos)!=0)goto bad;pos+=2;}
        path_start=pos;while(pos<b.len-20&&b.data[pos])++pos;if(pos>=b.len-20)goto bad;path_len=pos-path_start;if(path_len==0||path_len>=MAX_PATH_GIT)goto bad;
        {char name[MAX_PATH_GIT];memcpy(name,b.data+path_start,path_len);name[path_len]=0;if(!index_add(idx,name,b.data+entry_start+40,mode))goto bad;}
        ++pos;pad=(8-((pos-entry_start)&7))&7;if(pos+pad>b.len-20)goto bad;pos+=pad;
    }
    while(pos<b.len-20){unsigned long extlen;if(pos+8>b.len-20)goto bad;extlen=get32be(b.data+pos+4);if(pos+8+extlen>b.len-20)goto bad;
        /* Ignore known cache/resolve/untracked/fsmonitor extensions. Refuse unknown required extensions. */
        if(!memcmp(b.data+pos,"link",4)||!memcmp(b.data+pos,"sdir",4))goto bad;
        if(isupper((unsigned char)b.data[pos])&&memcmp(b.data+pos,"TREE",4)!=0&&memcmp(b.data+pos,"REUC",4)!=0&&memcmp(b.data+pos,"UNTR",4)!=0&&memcmp(b.data+pos,"FSMN",4)!=0&&memcmp(b.data+pos,"EOIE",4)!=0&&memcmp(b.data+pos,"IEOT",4)!=0)goto bad;
        pos+=8+extlen;
    }
    free(b.data);qsort(idx->v,idx->n,sizeof(IndexEntry),entry_cmp);return 1;
bad:free(b.data);index_free(idx);return 0;
}
static int index_write(Index *idx)
{
    Buffer b;U8 head[12],entry[62],sum[20];unsigned long i,j,n,pad;char path[MAX_PATH_GIT];qsort(idx->v,idx->n,sizeof(IndexEntry),entry_cmp);b.data=NULL;b.len=0;
    memcpy(head,"DIRC",4);put32be(head+4,2);put32be(head+8,(U32)idx->n);if(!buf_append(&b,head,12))return 0;
    for(i=0;i<idx->n;++i){unsigned long entry_start=b.len;memset(entry,0,sizeof(entry));put32be(entry+24,idx->v[i].mode?idx->v[i].mode:0100644);memcpy(entry+40,idx->v[i].oid,20);n=(unsigned long)strlen(idx->v[i].path);put16be(entry+60,(unsigned int)(n<0xfff?n:0xfff));if(!buf_append(&b,entry,62)||!buf_append(&b,idx->v[i].path,n)||!buf_byte(&b,0))goto bad;pad=(8-((b.len-entry_start)&7))&7;for(j=0;j<pad;++j)if(!buf_byte(&b,0))goto bad;}
    sha1_data(b.data,b.len,sum);if(!buf_append(&b,sum,20)||!path_join(path,sizeof(path),gitdir,"index")||!write_file(path,b.data,b.len))goto bad;free(b.data);return 1;
bad:if(b.data)free(b.data);return 0;
}

/* -------------------------- work tree traversal ------------------------- */
static int scan_add(Index *idx,const char *rel)
{
#if defined(__WATCOMC__)
    char dir[MAX_PATH_GIT],pattern[MAX_PATH_GIT],child[MAX_PATH_GIT],fs[MAX_PATH_GIT];struct _finddata_t fd;long h;to_fs_path(dir,(rel&&*rel)?rel:".");if(!path_join(pattern,sizeof(pattern),dir,"*.*"))return 0;h=_findfirst(pattern,&fd);if(h==-1L)return 1;
    do{if(!strcmp(fd.name,".")||!strcmp(fd.name,"..")||STRICMP(fd.name,".git")==0)continue;if(rel&&*rel){if(!path_join(child,sizeof(child),rel,fd.name)){_findclose(h);return 0;}}else strcpy(child,fd.name);if(path_join(fs,sizeof(fs),dir,fd.name)==0){_findclose(h);return 0;}if(fd.attrib&_A_SUBDIR){if(!scan_add(idx,child)){_findclose(h);return 0;}}else{U8 oid[20];if(!file_blob(fs,oid)||!index_add(idx,child,oid,0100644)){_findclose(h);return 0;}}}while(_findnext(h,&fd)==0);_findclose(h);return 1;
#else
    DIR *d;struct dirent *e;char dir[MAX_PATH_GIT],child[MAX_PATH_GIT],fs[MAX_PATH_GIT];to_fs_path(dir,(rel&&*rel)?rel:".");d=opendir(dir);if(!d)return 0;
    while((e=readdir(d))!=NULL){struct stat st;if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||!strcmp(e->d_name,".git"))continue;if(rel&&*rel){if(!path_join(child,sizeof(child),rel,e->d_name)){closedir(d);return 0;}}else strcpy(child,e->d_name);to_fs_path(fs,child);if(stat(fs,&st)!=0)continue;if(IS_DIR(st)){if(!scan_add(idx,child)){closedir(d);return 0;}}else if(S_ISREG(st.st_mode)){U8 oid[20];if(!file_blob(fs,oid)||!index_add(idx,child,oid,0100644)){closedir(d);return 0;}}}closedir(d);return 1;
#endif
}
static int stage_one(Index *idx,const char *arg)
{
    char rel[MAX_PATH_GIT],fs[MAX_PATH_GIT];struct stat st;U8 oid[20];unsigned long i;
    if(!strcmp(arg,"-A")||!strcmp(arg,"--all")){if(!scan_add(idx,""))return 0;for(i=0;i<idx->n;){to_fs_path(fs,idx->v[i].path);if(!file_exists(fs))index_remove(idx,idx->v[i].path);else ++i;}return 1;}
    if(!normalize_path(arg,rel,sizeof(rel))){fprintf(stderr,"git: invalid or unsafe path: %s\n",arg);return 0;}to_fs_path(fs,rel);if(stat(fs,&st)!=0){fprintf(stderr,"git: path not found: %s\n",arg);return 0;}if(IS_DIR(st))return scan_add(idx,rel);if(!file_blob(fs,oid))return 0;return index_add(idx,rel,oid,0100644);
}

/* ------------------------------- Git trees ------------------------------- */
typedef struct {char name[256];U8 oid[20];U32 mode;int directory;} TreeNode;
static int tree_node_cmp(const void *av,const void *bv)
{
    const TreeNode *a=(const TreeNode*)av,*b=(const TreeNode*)bv;size_t i=0,an=strlen(a->name),bn=strlen(b->name);while(i<an&&i<bn&&a->name[i]==b->name[i])++i;
    if(i==an&&i==bn)return 0;
    if(i==an)return a->directory?'/':0;
    if(i==bn)return b->directory?'/':0;
    return (unsigned char)a->name[i]-(unsigned char)b->name[i];
}
static int build_tree(Index *idx,const char *prefix,U8 outoid[20])
{
    TreeNode *nodes;unsigned long n=0,cap=32,i,j;size_t plen=(prefix&&*prefix)?strlen(prefix):0;Buffer body;char sub[MAX_PATH_GIT];
    nodes=(TreeNode*)malloc(cap*sizeof(TreeNode));if(!nodes)return 0;
    for(i=0;i<idx->n;++i){const char *p=idx->v[i].path;char *slash;size_t namelen;if(plen){if(strncmp(p,prefix,plen)!=0||p[plen]!='/')continue;p+=plen+1;}slash=strchr((char*)p,'/');namelen=slash?(size_t)(slash-p):strlen(p);if(namelen==0||namelen>=sizeof(nodes[0].name))goto fail;
        for(j=0;j<n;++j)if(strlen(nodes[j].name)==namelen&&!memcmp(nodes[j].name,p,namelen)&&nodes[j].directory==(slash!=NULL))break;
        if(j==n){if(n==cap){TreeNode *q;cap*=2;q=(TreeNode*)realloc(nodes,cap*sizeof(TreeNode));if(!q)goto fail;nodes=q;}memcpy(nodes[n].name,p,namelen);nodes[n].name[namelen]=0;nodes[n].directory=slash!=NULL;if(!slash){memcpy(nodes[n].oid,idx->v[i].oid,20);nodes[n].mode=idx->v[i].mode;}++n;}
        else if(!slash){memcpy(nodes[j].oid,idx->v[i].oid,20);nodes[j].mode=idx->v[i].mode;}
    }
    qsort(nodes,n,sizeof(TreeNode),tree_node_cmp);body.data=NULL;body.len=0;
    for(i=0;i<n;++i){char hdr[300];int k;if(nodes[i].directory){if(!make_rel_path(sub,sizeof(sub),prefix,nodes[i].name))goto fail_body;if(!build_tree(idx,sub,nodes[i].oid))goto fail_body;nodes[i].mode=0040000;}
        k=sprintf(hdr,"%o %s",(unsigned int)nodes[i].mode,nodes[i].name);if(!buf_append(&body,hdr,(unsigned long)k)||!buf_byte(&body,0)||!buf_append(&body,nodes[i].oid,20))goto fail_body;}
    if(!store_object("tree",body.data,body.len,outoid))goto fail_body;
    if(body.data)free(body.data);
    free(nodes);return 1;
fail_body:if(body.data)free(body.data);
fail:free(nodes);return 0;
}
static int parse_commit_tree(const U8 *p,unsigned long n,U8 tree[20])
{
    unsigned long i=0;while(i<n){unsigned long e=i;while(e<n&&p[e]!='\n')++e;if(e==i)break;if(e-i>=45&&!memcmp(p+i,"tree ",5)){char h[41];memcpy(h,p+i+5,40);h[40]=0;return hex_to_oid(h,tree);}i=e+1;}return 0;
}
static int flatten_tree(const U8 treeoid[20],const char *prefix,Index *idx,int depth)
{
    Buffer t;char type[16];unsigned long p=0;if(depth>MAX_DEPTH||!read_object(treeoid,&t,type)||strcmp(type,"tree"))return 0;
    while(p<t.len){unsigned long s=p;char modebuf[16],name[256],path[MAX_PATH_GIT];size_t n;U32 mode;U8 oid[20];while(p<t.len&&t.data[p]!=' ')++p;if(p>=t.len)goto bad;n=p-s;if(n>=sizeof(modebuf))goto bad;memcpy(modebuf,t.data+s,n);modebuf[n]=0;mode=(U32)strtoul(modebuf,NULL,8);++p;s=p;while(p<t.len&&t.data[p])++p;if(p>=t.len||p+21>t.len)goto bad;n=p-s;if(n>=sizeof(name))goto bad;memcpy(name,t.data+s,n);name[n]=0;++p;memcpy(oid,t.data+p,20);p+=20;
        if(!make_rel_path(path,sizeof(path),prefix,name))goto bad;
        if((mode&0170000)==0040000){if(!flatten_tree(oid,path,idx,depth+1))goto bad;}else if(!index_add(idx,path,oid,mode))goto bad;
    }free(t.data);return 1;
bad:free(t.data);return 0;
}
static int lookup_tree(const U8 treeoid[20],const char *wanted,U8 oid[20],U32 *mode,int depth)
{
    Buffer t;char type[16],component[256];const char *slash;size_t clen;unsigned long p=0;if(depth>MAX_DEPTH||!read_object(treeoid,&t,type)||strcmp(type,"tree"))return 0;
    slash=strchr(wanted,'/');clen=slash?(size_t)(slash-wanted):strlen(wanted);if(clen>=sizeof(component)){free(t.data);return 0;}memcpy(component,wanted,clen);component[clen]=0;
    while(p<t.len){unsigned long s=p;char m[16],name[256];size_t ml,nl;U32 md;while(p<t.len&&t.data[p]!=' ')++p;if(p>=t.len)break;ml=p-s;if(ml>=sizeof(m))goto fail;memcpy(m,t.data+s,ml);m[ml]=0;md=(U32)strtoul(m,NULL,8);++p;s=p;while(p<t.len&&t.data[p])++p;if(p>=t.len||p+21>t.len)goto fail;nl=p-s;if(nl>=sizeof(name))goto fail;memcpy(name,t.data+s,nl);name[nl]=0;++p;
        if(!strcmp(name,component)){U8 child[20];int ok;memcpy(child,t.data+p,20);if(slash){free(t.data);if((md&0170000)!=0040000)return 0;ok=lookup_tree(child,slash+1,oid,mode,depth+1);return ok;}memcpy(oid,child,20);if(mode)*mode=md;free(t.data);return 1;}p+=20;
    }
fail:free(t.data);return 0;
}

/* ------------------------------ refs / HEAD ------------------------------ */
static int read_ref_oid(const char *ref,U8 oid[20])
{
    char path[MAX_PATH_GIT],hex[41];Buffer b;unsigned long p;
    if(path_join(path,sizeof(path),gitdir,ref)&&read_file(path,&b)){
        if(b.len>=40){memcpy(hex,b.data,40);hex[40]=0;if(hex_to_oid(hex,oid)){free(b.data);return 1;}}free(b.data);
    }
    if(!path_join(path,sizeof(path),gitdir,"packed-refs")||!read_file(path,&b))return 0;
    p=0;while(p<b.len){unsigned long e=p;while(e<b.len&&b.data[e]!='\n')++e;if(e-p>=42&&b.data[p]!='#'&&b.data[p]!='^'&&b.data[p+40]==' '){if(strlen(ref)==e-p-41&&!memcmp(b.data+p+41,ref,e-p-41)){memcpy(hex,b.data+p,40);hex[40]=0;if(hex_to_oid(hex,oid)){free(b.data);return 1;}}}p=e+1;}
    free(b.data);return 0;
}
static int read_head(U8 oid[20],char *target,size_t cap,int *has_commit)
{
    char hp[MAX_PATH_GIT],hex[41];Buffer b;unsigned long n;*has_commit=0;target[0]=0;
    if(!path_join(hp,sizeof(hp),gitdir,"HEAD")||!read_file(hp,&b))return 0;
    if(b.len>5&&!memcmp(b.data,"ref: ",5)){
        n=b.len-5;if(n&&b.data[b.len-1]=='\n')--n;if(n>=cap){free(b.data);return 0;}memcpy(target,b.data+5,n);target[n]=0;free(b.data);
        if(read_ref_oid(target,oid))*has_commit=1;
        return 1;
    }
    if(b.len>=40){memcpy(hex,b.data,40);hex[40]=0;if(hex_to_oid(hex,oid))*has_commit=1;}free(b.data);return 1;
}

static int head_tree(Index *head)
{
    U8 commit[20],tree[20];char ref[MAX_PATH_GIT],type[16];int has;Buffer c;head->v=NULL;head->n=head->cap=0;if(!read_head(commit,ref,sizeof(ref),&has))return 0;if(!has)return 1;
    c.data=NULL;c.len=0;if(!read_object(commit,&c,type)||strcmp(type,"commit")||!parse_commit_tree(c.data,c.len,tree)){if(c.data)free(c.data);return 0;}free(c.data);return flatten_tree(tree,"",head,0);
}

/* -------------------------------- commands ------------------------------- */
static int require_repository(void)
{
    if(!dir_exists(".git") || !file_exists(".git/HEAD") || !dir_exists(".git/objects")){
        fprintf(stderr,"git: not a Git working directory (run 'git init' first)\n");
        return 0;
    }
    return 1;
}
static int cmd_init(void)
{
    const char *cfg="[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n\tbare = false\n\tlogallrefupdates = true\n[user]\n\tname = TinyVCS98 User\n\temail = tinyvcs98@localhost\n";
    if(file_exists(".git")){fprintf(stderr,"git: .git already exists\n");return 1;}if(!mkdirs(".git/objects/info")||!mkdirs(".git/refs/heads")||!mkdirs(".git/refs/tags"))return 1;
    if(!write_file(".git/HEAD",(const U8*)"ref: refs/heads/master\n",23)||!write_file(".git/config",(const U8*)cfg,(unsigned long)strlen(cfg)))return 1;
    printf("Initialized empty Git repository in .git/\n");return 0;
}
static int cmd_add(int argc,char **argv)
{
    Index idx;int i,ok=1;if(!require_repository())return 1;if(!index_read(&idx)){fprintf(stderr,"git: cannot read .git/index (unsupported or corrupt index)\n");return 1;}if(argc==0){fprintf(stderr,"usage: git add [-A | path ...]\n");index_free(&idx);return 1;}
    for(i=0;i<argc;++i)if(!stage_one(&idx,argv[i]))ok=0;
    if(ok&&!index_write(&idx)){fprintf(stderr,"git: could not write .git/index\n");ok=0;}
    index_free(&idx);return ok?0:1;
}
static int cmd_rm(const char *arg)
{
    Index idx;char rel[MAX_PATH_GIT],fs[MAX_PATH_GIT];if(!require_repository())return 1;if(!normalize_path(arg,rel,sizeof(rel))||!index_read(&idx)){fprintf(stderr,"git: invalid path or index\n");return 1;}if(!index_remove(&idx,rel)){fprintf(stderr,"git: path not staged/tracked: %s\n",rel);index_free(&idx);return 1;}to_fs_path(fs,rel);remove_file(fs);if(!index_write(&idx)){index_free(&idx);return 1;}index_free(&idx);return 0;
}
static int global_get(const char *key,char *out,size_t cap);
static int read_user(char *name,size_t nc,char *email,size_t ec)
{
    Buffer b;unsigned long p;if(!read_file(".git/config",&b)){strcpy(name,"TinyVCS98 User");strcpy(email,"tinyvcs98@localhost");return 1;}
    p=0;while(p<b.len){unsigned long e=p;while(e<b.len&&b.data[e]!='\n')++e;if(e-p>=6&&!memcmp(b.data+p,"[user]",6)){p=e+1;while(p<b.len&&b.data[p]!='['){e=p;while(e<b.len&&b.data[e]!='\n')++e;if(e-p>=4&&!memcmp(b.data+p,"name",4)){char *q=(char*)b.data+p+4;while(q<(char*)b.data+e&&(*q==' '||*q=='\t'||*q=='='))++q;while(e>p&&isspace(b.data[e-1]))--e;if((size_t)((char*)b.data+e-q)<nc){memcpy(name,q,(size_t)((char*)b.data+e-q));name[(char*)b.data+e-q]=0;}}if(e-p>=5&&!memcmp(b.data+p,"email",5)){char *q=(char*)b.data+p+5;while(q<(char*)b.data+e&&(*q==' '||*q=='\t'||*q=='='))++q;while(e>p&&isspace(b.data[e-1]))--e;if((size_t)((char*)b.data+e-q)<ec){memcpy(email,q,(size_t)((char*)b.data+e-q));email[(char*)b.data+e-q]=0;}}p=e+1;}break;}p=e+1;}
    free(b.data);
    {char g[256];
     if((!*name||!strcmp(name,"TinyVCS98 User"))&&global_get("user.name",g,sizeof(g))&&g[0]&&strlen(g)<nc)strcpy(name,g);
     if((!*email||!strcmp(email,"tinyvcs98@localhost"))&&global_get("user.email",g,sizeof(g))&&g[0]&&strlen(g)<ec)strcpy(email,g);}
    if(!*name)strcpy(name,"TinyVCS98 User");if(!*email)strcpy(email,"tinyvcs98@localhost");return 1;
}
static int cmd_commit(const char *message)
{
    Index idx;U8 tree[20],parent[20],commit[20];char target[MAX_PATH_GIT],refpath[MAX_PATH_GIT],hex[42],name[128]="",email[128]="",line[1024];int has,n;Buffer body;time_t now;
    if(!require_repository())return 1;
    if(!message||!*message){fprintf(stderr,"usage: git commit -m \"message\"\n");return 1;}if(!index_read(&idx)){fprintf(stderr,"git: index invalid or unsupported\n");return 1;}if(idx.n==0){fprintf(stderr,"git: empty index\n");index_free(&idx);return 1;}
    if(!build_tree(&idx,"",tree)){index_free(&idx);fprintf(stderr,"git: cannot build tree\n");return 1;}if(!read_head(parent,target,sizeof(target),&has)){index_free(&idx);return 1;}read_user(name,sizeof(name),email,sizeof(email));now=time(NULL);body.data=NULL;body.len=0;oid_to_hex(tree,hex);n=sprintf(line,"tree %s\n",hex);if(!buf_append(&body,line,(unsigned long)n))goto fail;
    if(has){oid_to_hex(parent,hex);n=sprintf(line,"parent %s\n",hex);if(!buf_append(&body,line,(unsigned long)n))goto fail;}
    n=sprintf(line,"author %s <%s> %ld +0000\ncommitter %s <%s> %ld +0000\n\n%s\n",name,email,(long)now,name,email,(long)now,message);if(!buf_append(&body,line,(unsigned long)n)||!store_object("commit",body.data,body.len,commit))goto fail;
    if(target[0]){if(!path_join(refpath,sizeof(refpath),gitdir,target))goto fail;}else if(!path_join(refpath,sizeof(refpath),gitdir,"HEAD"))goto fail;oid_to_hex(commit,hex);strcat(hex,"\n");if(!write_file(refpath,(U8*)hex,41))goto fail;
    free(body.data);index_free(&idx);hex[40]=0;printf("[commit %s] %s\n",hex,message);return 0;
fail:if(body.data)free(body.data);index_free(&idx);fprintf(stderr,"git: commit failed\n");return 1;
}
static int is_indexed(const Index *idx,const char *path)
{
    unsigned long i;for(i=0;i<idx->n;++i)if(!strcmp(idx->v[i].path,path))return 1;return 0;
}
static int scan_untracked(const Index *idx,const char *rel)
{
    int found=0;
#if defined(__WATCOMC__)
    char dir[MAX_PATH_GIT],pattern[MAX_PATH_GIT],child[MAX_PATH_GIT];struct _finddata_t fd;long h;
    to_fs_path(dir,(rel&&*rel)?rel:".");if(!path_join(pattern,sizeof(pattern),dir,"*.*"))return 0;h=_findfirst(pattern,&fd);if(h==-1L)return 0;
    do{if(!strcmp(fd.name,".")||!strcmp(fd.name,"..")||STRICMP(fd.name,".git")==0)continue;if(rel&&*rel){if(!path_join(child,sizeof(child),rel,fd.name))continue;}else strcpy(child,fd.name);
        if(fd.attrib&_A_SUBDIR)found+=scan_untracked(idx,child);else if(!is_indexed(idx,child)){printf("  ?? %s\n",child);++found;}
    }while(_findnext(h,&fd)==0);_findclose(h);
#else
    DIR *d;struct dirent *e;char dir[MAX_PATH_GIT],child[MAX_PATH_GIT],fs[MAX_PATH_GIT];
    to_fs_path(dir,(rel&&*rel)?rel:".");d=opendir(dir);if(!d)return 0;
    while((e=readdir(d))!=NULL){struct stat st;if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||!strcmp(e->d_name,".git"))continue;if(rel&&*rel){if(!path_join(child,sizeof(child),rel,e->d_name))continue;}else strcpy(child,e->d_name);to_fs_path(fs,child);if(stat(fs,&st)!=0)continue;if(IS_DIR(st))found+=scan_untracked(idx,child);else if(S_ISREG(st.st_mode)&&!is_indexed(idx,child)){printf("  ?? %s\n",child);++found;}}
    closedir(d);
#endif
    return found;
}

static int cmd_status(void)
{
    Index idx,head;unsigned long i;int changed=0;char fs[MAX_PATH_GIT],target[MAX_PATH_GIT];U8 oid[20];int has;
    if(!require_repository())return 1;
    if(!index_read(&idx)||!head_tree(&head)){fprintf(stderr,"git: cannot read index or HEAD tree\n");return 1;}if(!read_head(oid,target,sizeof(target),&has))target[0]=0;
    printf("On branch %s\n",target[0]?strrchr(target,'/')+1:"(detached or unborn)");
    for(i=0;i<idx.n;++i){unsigned long j;for(j=0;j<head.n;++j)if(!strcmp(idx.v[i].path,head.v[j].path))break;if(j==head.n)printf("  A  %s (staged)\n",idx.v[i].path),changed=1;else if(memcmp(idx.v[i].oid,head.v[j].oid,20))printf("  M  %s (staged)\n",idx.v[i].path),changed=1;}
    for(i=0;i<head.n;++i){unsigned long j;for(j=0;j<idx.n;++j)if(!strcmp(head.v[i].path,idx.v[j].path))break;if(j==idx.n)printf("  D  %s (staged)\n",head.v[i].path),changed=1;}
    for(i=0;i<idx.n;++i){to_fs_path(fs,idx.v[i].path);if(!file_exists(fs)){printf("  D  %s (working tree)\n",idx.v[i].path);changed=1;}else if(hash_file_blob(fs,oid)&&memcmp(oid,idx.v[i].oid,20)){printf("  M  %s (not staged)\n",idx.v[i].path);changed=1;}}
    if(scan_untracked(&idx,""))changed=1;
    if(!changed)printf("Nothing to commit, working tree clean.\n");
    index_free(&idx);index_free(&head);return 0;
}
static int cmd_log(void)
{
    U8 oid[20];char target[MAX_PATH_GIT],type[16],hex[41];int has,count=0;Buffer c;
    if(!require_repository())return 1;
    if(!read_head(oid,target,sizeof(target),&has)||!has){printf("No commits yet.\n");return 0;}
    while(count<1000){unsigned long p=0;U8 parent[20];int hasparent=0;char ph[41];if(!read_object(oid,&c,type)||strcmp(type,"commit"))break;oid_to_hex(oid,hex);printf("commit %s\n",hex);
        while(p<c.len){unsigned long e=p;while(e<c.len&&c.data[e]!='\n')++e;if(e-p>=7&&!memcmp(c.data+p,"author ",7))printf("Author: %.*s\n",(int)(e-p-7),c.data+p+7);if(e-p>=47&&!memcmp(c.data+p,"parent ",7)){memcpy(ph,c.data+p+7,40);ph[40]=0;if(hex_to_oid(ph,parent))hasparent=1;}if(e==p){++e;while(e<c.len&&c.data[e]=='\n')++e;printf("    %.*s\n",(int)(c.len-e),c.data+e);break;}p=e+1;}
        free(c.data);if(!hasparent)break;memcpy(oid,parent,20);++count;printf("\n");}
    return 0;
}
static int cmd_restore(const char *commithex,const char *arg)
{
    U8 coid[20],tree[20],blob[20];U32 mode;char rel[MAX_PATH_GIT],fs[MAX_PATH_GIT],type[16];Buffer c,b;c.data=NULL;c.len=0;b.data=NULL;b.len=0;
    if(!require_repository())return 1;
    if(!hex_to_oid(commithex,coid)||!normalize_path(arg,rel,sizeof(rel))){fprintf(stderr,"usage: git restore <full-commit-id> <path>\n");return 1;}if(!read_object(coid,&c,type)||strcmp(type,"commit")||!parse_commit_tree(c.data,c.len,tree)){fprintf(stderr,"git: commit unavailable\n");return 1;}free(c.data);if(!lookup_tree(tree,rel,blob,&mode,0)){fprintf(stderr,"git: path not found in commit\n");return 1;}if(!read_object(blob,&b,type)||strcmp(type,"blob")){fprintf(stderr,"git: blob unavailable\n");return 1;}to_fs_path(fs,rel);if(!write_file(fs,b.data,b.len)){free(b.data);return 1;}free(b.data);printf("Restored %s\n",rel);return 0;
}
/* -------------------------- HTTP(S) smart transport ---------------------- */
/* WinSock 1.1 is present on Windows 98. https:// remotes use the built-in TLS 1.2
 * client below, so no OpenSSL/zlib DLLs or Windows crypto/Schannel support is needed. */
#if defined(__WATCOMC__)
typedef SOCKET net_socket_t;
#define NET_CLOSE(s) closesocket(s)
#else
typedef int net_socket_t;
#define NET_CLOSE(s) close(s)
#endif
#define MAX_REMOTE_REFS 1024

typedef struct { char name[MAX_PATH_GIT]; U8 oid[20]; } RemoteRef;
typedef struct { U8 oid[20]; char type[8]; Buffer body; } PackObject;

/* ============================= TLS 1.2 CLIENT ============================ */
/* Self-contained, dependency-free HTTPS support: SHA-256/384/512, HMAC, AES-128-GCM,
 * big-number Montgomery arithmetic (16-bit limbs, so no 64-bit integer type is needed
 * outside SHA-512), ECDHE (secp256r1), RSA-PKCS#1 v1.5 and ECDSA (P-256/P-384) signature
 * verification, an X.509 reader and a TLS 1.2 handshake. Cipher suites offered:
 *   TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256.
 * Server certificates are verified against a PEM CA bundle (see README.md). */
#if defined(__WATCOMC__)
typedef unsigned __int64 U64;
#else
typedef unsigned long long U64;
#endif

static const U32 k256[64]={
0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL};
static const U32 k512[160]={
0x428a2f98UL,0xd728ae22UL,0x71374491UL,0x23ef65cdUL,0xb5c0fbcfUL,0xec4d3b2fUL,0xe9b5dba5UL,0x8189dbbcUL,
0x3956c25bUL,0xf348b538UL,0x59f111f1UL,0xb605d019UL,0x923f82a4UL,0xaf194f9bUL,0xab1c5ed5UL,0xda6d8118UL,
0xd807aa98UL,0xa3030242UL,0x12835b01UL,0x45706fbeUL,0x243185beUL,0x4ee4b28cUL,0x550c7dc3UL,0xd5ffb4e2UL,
0x72be5d74UL,0xf27b896fUL,0x80deb1feUL,0x3b1696b1UL,0x9bdc06a7UL,0x25c71235UL,0xc19bf174UL,0xcf692694UL,
0xe49b69c1UL,0x9ef14ad2UL,0xefbe4786UL,0x384f25e3UL,0x0fc19dc6UL,0x8b8cd5b5UL,0x240ca1ccUL,0x77ac9c65UL,
0x2de92c6fUL,0x592b0275UL,0x4a7484aaUL,0x6ea6e483UL,0x5cb0a9dcUL,0xbd41fbd4UL,0x76f988daUL,0x831153b5UL,
0x983e5152UL,0xee66dfabUL,0xa831c66dUL,0x2db43210UL,0xb00327c8UL,0x98fb213fUL,0xbf597fc7UL,0xbeef0ee4UL,
0xc6e00bf3UL,0x3da88fc2UL,0xd5a79147UL,0x930aa725UL,0x06ca6351UL,0xe003826fUL,0x14292967UL,0x0a0e6e70UL,
0x27b70a85UL,0x46d22ffcUL,0x2e1b2138UL,0x5c26c926UL,0x4d2c6dfcUL,0x5ac42aedUL,0x53380d13UL,0x9d95b3dfUL,
0x650a7354UL,0x8baf63deUL,0x766a0abbUL,0x3c77b2a8UL,0x81c2c92eUL,0x47edaee6UL,0x92722c85UL,0x1482353bUL,
0xa2bfe8a1UL,0x4cf10364UL,0xa81a664bUL,0xbc423001UL,0xc24b8b70UL,0xd0f89791UL,0xc76c51a3UL,0x0654be30UL,
0xd192e819UL,0xd6ef5218UL,0xd6990624UL,0x5565a910UL,0xf40e3585UL,0x5771202aUL,0x106aa070UL,0x32bbd1b8UL,
0x19a4c116UL,0xb8d2d0c8UL,0x1e376c08UL,0x5141ab53UL,0x2748774cUL,0xdf8eeb99UL,0x34b0bcb5UL,0xe19b48a8UL,
0x391c0cb3UL,0xc5c95a63UL,0x4ed8aa4aUL,0xe3418acbUL,0x5b9cca4fUL,0x7763e373UL,0x682e6ff3UL,0xd6b2b8a3UL,
0x748f82eeUL,0x5defb2fcUL,0x78a5636fUL,0x43172f60UL,0x84c87814UL,0xa1f0ab72UL,0x8cc70208UL,0x1a6439ecUL,
0x90befffaUL,0x23631e28UL,0xa4506cebUL,0xde82bde9UL,0xbef9a3f7UL,0xb2c67915UL,0xc67178f2UL,0xe372532bUL,
0xca273eceUL,0xea26619cUL,0xd186b8c7UL,0x21c0c207UL,0xeada7dd6UL,0xcde0eb1eUL,0xf57d4f7fUL,0xee6ed178UL,
0x06f067aaUL,0x72176fbaUL,0x0a637dc5UL,0xa2c898a6UL,0x113f9804UL,0xbef90daeUL,0x1b710b35UL,0x131c471bUL,
0x28db77f5UL,0x23047d84UL,0x32caab7bUL,0x40c72493UL,0x3c9ebe0aUL,0x15c9bebcUL,0x431d67c4UL,0x9c100d4cUL,
0x4cc5d4beUL,0xcb3e42b6UL,0x597f299cUL,0xfc657e2aUL,0x5fcb6fabUL,0x3ad6faecUL,0x6c44198cUL,0x4a475817UL};
static const U32 h256init[8]={0x6a09e667UL,0xbb67ae85UL,0x3c6ef372UL,0xa54ff53aUL,0x510e527fUL,0x9b05688cUL,0x1f83d9abUL,0x5be0cd19UL};
static const U32 h512init[16]={0x6a09e667UL,0xf3bcc908UL,0xbb67ae85UL,0x84caa73bUL,0x3c6ef372UL,0xfe94f82bUL,0xa54ff53aUL,0x5f1d36f1UL,0x510e527fUL,0xade682d1UL,0x9b05688cUL,0x2b3e6c1fUL,0x1f83d9abUL,0xfb41bd6bUL,0x5be0cd19UL,0x137e2179UL};
static const U32 h384init[16]={0xcbbb9d5dUL,0xc1059ed8UL,0x629a292aUL,0x367cd507UL,0x9159015aUL,0x3070dd17UL,0x152fecd8UL,0xf70e5939UL,0x67332667UL,0xffc00b31UL,0x8eb44a87UL,0x68581511UL,0xdb0c2e0dUL,0x64f98fa7UL,0x47b5481dUL,0xbefa4fa4UL};

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
typedef struct { U32 h[8]; U32 hi, lo; U8 block[64]; unsigned int used; } SHA256;
static void sha256_block(SHA256 *s,const U8 *p)
{
    U32 w[64],a,b,c,d,e,f,g,h,t1,t2,s0,s1;int i;
    for(i=0;i<16;++i)w[i]=get32be(p+i*4);
    for(i=16;i<64;++i){
        s0=ROR32(w[i-15],7)^ROR32(w[i-15],18)^(w[i-15]>>3);
        s1=ROR32(w[i-2],17)^ROR32(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for(i=0;i<64;++i){
        t1=h+(ROR32(e,6)^ROR32(e,11)^ROR32(e,25))+((e&f)^((~e)&g))+k256[i]+w[i];
        t2=(ROR32(a,2)^ROR32(a,13)^ROR32(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha256_init(SHA256 *s){int i;for(i=0;i<8;++i)s->h[i]=h256init[i];s->hi=s->lo=0;s->used=0;}
static void sha256_update(SHA256 *s,const U8 *p,unsigned long n)
{
    U32 old=s->lo;s->lo+=(U32)(n<<3);if(s->lo<old)++s->hi;s->hi+=(U32)(n>>29);
    while(n){unsigned int take=(unsigned int)(64-s->used);if(take>n)take=(unsigned int)n;memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;if(s->used==64){sha256_block(s,s->block);s->used=0;}}
}
static void sha256_final(SHA256 *s,U8 out[32])
{
    U32 hi=s->hi,lo=s->lo;U8 pad[72];unsigned int n,i;
    pad[0]=0x80;n=(s->used<56)?(56-s->used):(120-s->used);memset(pad+1,0,n-1);sha256_update(s,pad,n);
    put32be(pad,hi);put32be(pad+4,lo);sha256_update(s,pad,8);
    for(i=0;i<8;++i)put32be(out+i*4,s->h[i]);
}
static void sha256_data(const U8 *p,unsigned long n,U8 out[32]){SHA256 s;sha256_init(&s);sha256_update(&s,p,n);sha256_final(&s,out);}

typedef struct { U64 h[8]; U32 total; U8 block[128]; unsigned int used; unsigned int outlen; } SHA512;
static U64 ror64(U64 x,unsigned int n){return (x>>n)|(x<<(64-n));}
static U64 get64be(const U8 *p){return ((U64)get32be(p)<<32)|(U64)get32be(p+4);}
static void put64be(U8 *p,U64 x){put32be(p,(U32)(x>>32));put32be(p+4,(U32)(x&0xffffffffUL));}
static void sha512_block(SHA512 *s,const U8 *p)
{
    U64 w[80],a,b,c,d,e,f,g,h,t1,t2,s0,s1;int i;
    for(i=0;i<16;++i)w[i]=get64be(p+i*8);
    for(i=16;i<80;++i){
        s0=ror64(w[i-15],1)^ror64(w[i-15],8)^(w[i-15]>>7);
        s1=ror64(w[i-2],19)^ror64(w[i-2],61)^(w[i-2]>>6);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for(i=0;i<80;++i){
        U64 k=((U64)k512[2*i]<<32)|(U64)k512[2*i+1];
        t1=h+(ror64(e,14)^ror64(e,18)^ror64(e,41))+((e&f)^((~e)&g))+k+w[i];
        t2=(ror64(a,28)^ror64(a,34)^ror64(a,39))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha512_init(SHA512 *s,int is384)
{
    const U32 *iv=is384?h384init:h512init;int i;
    for(i=0;i<8;++i)s->h[i]=((U64)iv[2*i]<<32)|(U64)iv[2*i+1];
    s->total=0;s->used=0;s->outlen=is384?48:64;
}
static void sha512_update(SHA512 *s,const U8 *p,unsigned long n)
{
    s->total+=(U32)n;
    while(n){unsigned int take=(unsigned int)(128-s->used);if(take>n)take=(unsigned int)n;memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;if(s->used==128){sha512_block(s,s->block);s->used=0;}}
}
static void sha512_final(SHA512 *s,U8 *out)
{
    U32 bits_hi=s->total>>29,bits_lo=s->total<<3;U8 pad[144];unsigned int n,i;
    pad[0]=0x80;n=(s->used<112)?(112-s->used):(240-s->used);memset(pad+1,0,n-1);
    sha512_update(s,pad,n);
    memset(pad,0,8);put32be(pad+8,bits_hi);put32be(pad+12,bits_lo);sha512_update(s,pad,16);
    {U8 full[64];for(i=0;i<8;++i)put64be(full+i*8,s->h[i]);memcpy(out,full,s->outlen);}
}
/* TLS HashAlgorithm numbers: 4 = SHA-256, 5 = SHA-384, 6 = SHA-512. Returns digest length or 0. */
static unsigned int hash_data(int alg,const U8 *p,unsigned long n,U8 *out)
{
    if(alg==4){sha256_data(p,n,out);return 32;}
    if(alg==5||alg==6){SHA512 s;sha512_init(&s,alg==5);sha512_update(&s,p,n);sha512_final(&s,out);return s.outlen;}
    return 0;
}
static void hmac_sha256(const U8 *key,unsigned long kl,const U8 *msg,unsigned long ml,U8 out[32])
{
    U8 k[64],ip[64],op[64],inner[32];SHA256 s;unsigned int i;
    memset(k,0,64);if(kl>64)sha256_data(key,kl,k);else memcpy(k,key,kl);
    for(i=0;i<64;++i){ip[i]=(U8)(k[i]^0x36);op[i]=(U8)(k[i]^0x5c);}
    sha256_init(&s);sha256_update(&s,ip,64);sha256_update(&s,msg,ml);sha256_final(&s,inner);
    sha256_init(&s);sha256_update(&s,op,64);sha256_update(&s,inner,32);sha256_final(&s,out);
}
/* TLS 1.2 PRF (P_SHA256). */
static void tls_prf(const U8 *secret,unsigned long sl,const char *label,const U8 *seed,unsigned long seedlen,U8 *out,unsigned long outlen)
{
    U8 ls[160],a[32],tmp[32+160],blk[32];unsigned long lsl=strlen(label),take;
    memcpy(ls,label,lsl);memcpy(ls+lsl,seed,seedlen);lsl+=seedlen;
    hmac_sha256(secret,sl,ls,lsl,a);
    while(outlen){
        memcpy(tmp,a,32);memcpy(tmp+32,ls,lsl);hmac_sha256(secret,sl,tmp,32+lsl,blk);
        take=outlen>32?32:outlen;memcpy(out,blk,take);out+=take;outlen-=take;
        hmac_sha256(secret,sl,a,32,a);
    }
}
static int ct_equal(const U8 *a,const U8 *b,unsigned long n){U8 d=0;unsigned long i;for(i=0;i<n;++i)d|=(U8)(a[i]^b[i]);return d==0;}

/* ---------------------------- random numbers ---------------------------- */
static U8 rng_state[32];static U32 rng_ctr;static int rng_ready;
static void rng_mix(SHA256 *s,const void *p,unsigned long n){sha256_update(s,(const U8*)p,n);}
static void rng_seed(void)
{
    SHA256 s;U8 buf[32];int strong=0;time_t t=time(NULL);clock_t cl=clock();void *stackaddr=&s;
    sha256_init(&s);rng_mix(&s,&t,sizeof(t));rng_mix(&s,&cl,sizeof(cl));rng_mix(&s,&stackaddr,sizeof(stackaddr));
#if defined(__WATCOMC__)
    {
        DWORD tick=GetTickCount();POINT pt;LARGE_INTEGER pc;SYSTEMTIME st;HMODULE adv;
        rng_mix(&s,&tick,sizeof(tick));
        pt.x=pt.y=0;GetCursorPos(&pt);rng_mix(&s,&pt,sizeof(pt));
        pc.LowPart=0;pc.HighPart=0;QueryPerformanceCounter(&pc);rng_mix(&s,&pc,sizeof(pc));
        GetLocalTime(&st);rng_mix(&s,&st,sizeof(st));
        adv=LoadLibraryA("advapi32.dll");
        if(adv){
            typedef BOOL (WINAPI *AcqFn)(unsigned long*,const char*,const char*,DWORD,DWORD);
            typedef BOOL (WINAPI *GenFn)(unsigned long,DWORD,BYTE*);
            typedef BOOL (WINAPI *RelFn)(unsigned long,DWORD);
            AcqFn acq=(AcqFn)GetProcAddress(adv,"CryptAcquireContextA");
            GenFn gen=(GenFn)GetProcAddress(adv,"CryptGenRandom");
            RelFn rel=(RelFn)GetProcAddress(adv,"CryptReleaseContext");
            unsigned long prov=0;
            if(acq&&gen&&acq(&prov,NULL,NULL,1,0xF0000000UL)){
                if(gen(prov,32,buf)){rng_mix(&s,buf,32);strong=1;}
                if(rel)rel(prov,0);
            }
            FreeLibrary(adv);
        }
    }
#else
    {
        FILE *f=fopen("/dev/urandom","rb");
        if(f){if(fread(buf,1,32,f)==32){rng_mix(&s,buf,32);strong=1;}fclose(f);}
        {long pid=(long)getpid();rng_mix(&s,&pid,sizeof(pid));}
    }
#endif
    if(!strong)fprintf(stderr,"git: warning: no system random source; TLS keys have weak entropy\n");
    sha256_final(&s,rng_state);rng_ctr=0;rng_ready=1;
}
static void rng_bytes(U8 *out,unsigned long n)
{
    U8 blk[32],in[37];unsigned long take;
    if(!rng_ready)rng_seed();
    while(n){
        memcpy(in,rng_state,32);put32be(in+32,rng_ctr);in[36]=1;sha256_data(in,37,blk);
        in[36]=2;sha256_data(in,37,rng_state);++rng_ctr;
        take=n>32?32:n;memcpy(out,blk,take);out+=take;n-=take;
    }
}

/* ------------------------------ AES-128-GCM ------------------------------ */
typedef struct { U32 rk[44]; U32 m[16][4]; } AesGcm;
static U8 aes_sbox[256];static U32 aes_te[4][256];static int aes_ready;
static void aes_tables(void)
{
    unsigned int p=1,q=1,i,x,s,s2,s3;
    if(aes_ready)return;
    do{
        p=(p^(p<<1)^((p&0x80)?0x11b:0))&0xff;
        q^=q<<1;q^=q<<2;q^=q<<4;q&=0xff;if(q&0x80)q^=0x09;
        x=q^(((q<<1)|(q>>7))&0xff)^(((q<<2)|(q>>6))&0xff)^(((q<<3)|(q>>5))&0xff)^(((q<<4)|(q>>4))&0xff);
        aes_sbox[p]=(U8)((x^0x63)&0xff);
    }while(p!=1);
    aes_sbox[0]=0x63;
    for(i=0;i<256;++i){
        s=aes_sbox[i];s2=((s<<1)^((s&0x80)?0x11b:0))&0xff;s3=s2^s;
        aes_te[0][i]=((U32)s2<<24)|((U32)s<<16)|((U32)s<<8)|(U32)s3;
        aes_te[1][i]=ROR32(aes_te[0][i],8);aes_te[2][i]=ROR32(aes_te[0][i],16);aes_te[3][i]=ROR32(aes_te[0][i],24);
    }
    aes_ready=1;
}
static U32 aes_subword(U32 w){return ((U32)aes_sbox[w>>24]<<24)|((U32)aes_sbox[(w>>16)&255]<<16)|((U32)aes_sbox[(w>>8)&255]<<8)|aes_sbox[w&255];}
static void aes_encrypt_block(const U32 *rk,const U8 in[16],U8 out[16])
{
    U32 s0,s1,s2,s3,t0,t1,t2,t3;int r;
    s0=get32be(in)^rk[0];s1=get32be(in+4)^rk[1];s2=get32be(in+8)^rk[2];s3=get32be(in+12)^rk[3];
    for(r=1;r<10;++r){
        t0=aes_te[0][s0>>24]^aes_te[1][(s1>>16)&255]^aes_te[2][(s2>>8)&255]^aes_te[3][s3&255]^rk[4*r];
        t1=aes_te[0][s1>>24]^aes_te[1][(s2>>16)&255]^aes_te[2][(s3>>8)&255]^aes_te[3][s0&255]^rk[4*r+1];
        t2=aes_te[0][s2>>24]^aes_te[1][(s3>>16)&255]^aes_te[2][(s0>>8)&255]^aes_te[3][s1&255]^rk[4*r+2];
        t3=aes_te[0][s3>>24]^aes_te[1][(s0>>16)&255]^aes_te[2][(s1>>8)&255]^aes_te[3][s2&255]^rk[4*r+3];
        s0=t0;s1=t1;s2=t2;s3=t3;
    }
    t0=(((U32)aes_sbox[s0>>24]<<24)|((U32)aes_sbox[(s1>>16)&255]<<16)|((U32)aes_sbox[(s2>>8)&255]<<8)|aes_sbox[s3&255])^rk[40];
    t1=(((U32)aes_sbox[s1>>24]<<24)|((U32)aes_sbox[(s2>>16)&255]<<16)|((U32)aes_sbox[(s3>>8)&255]<<8)|aes_sbox[s0&255])^rk[41];
    t2=(((U32)aes_sbox[s2>>24]<<24)|((U32)aes_sbox[(s3>>16)&255]<<16)|((U32)aes_sbox[(s0>>8)&255]<<8)|aes_sbox[s1&255])^rk[42];
    t3=(((U32)aes_sbox[s3>>24]<<24)|((U32)aes_sbox[(s0>>16)&255]<<16)|((U32)aes_sbox[(s1>>8)&255]<<8)|aes_sbox[s2&255])^rk[43];
    put32be(out,t0);put32be(out+4,t1);put32be(out+8,t2);put32be(out+12,t3);
}
static void gcm_shift1(U32 m[4])
{
    U32 t=(m[3]&1)?0xe1000000UL:0;
    m[3]=(m[2]<<31)|(m[3]>>1);m[2]=(m[1]<<31)|(m[2]>>1);m[1]=(m[0]<<31)|(m[1]>>1);m[0]=(m[0]>>1)^t;
}
static void gcm_init(AesGcm *g,const U8 key[16])
{
    U8 zero[16],hb[16];U32 v[4];int i,j,k;U32 rcon=0x01000000UL;
    aes_tables();
    for(i=0;i<4;++i)g->rk[i]=get32be(key+4*i);
    for(i=4;i<44;++i){
        U32 t=g->rk[i-1];
        if(i%4==0){t=aes_subword((t<<8)|(t>>24))^rcon;rcon=((rcon<<1)^((rcon&0x80000000UL)?0x1b000000UL:0))&0xffffffffUL;}
        g->rk[i]=g->rk[i-4]^t;
    }
    memset(zero,0,16);aes_encrypt_block(g->rk,zero,hb);
    for(i=0;i<4;++i)v[i]=get32be(hb+4*i);
    memset(g->m[0],0,16);memcpy(g->m[8],v,16);
    for(i=4;i>0;i>>=1){gcm_shift1(v);memcpy(g->m[i],v,16);}
    for(i=2;i<=8;i*=2)for(j=1;j<i;++j)for(k=0;k<4;++k)g->m[i+j][k]=g->m[i][k]^g->m[j][k];
}
static void gcm_mult(const AesGcm *g,U8 x[16])
{
    static const U32 last4[16]={0x0000,0x1c20,0x3840,0x2460,0x7080,0x6ca0,0x48c0,0x54e0,0xe100,0xfd20,0xd940,0xc560,0x9180,0x8da0,0xa9c0,0xb5e0};
    U32 z[4];int i,k;unsigned int lo,hi,rem;
    lo=x[15]&0xf;for(k=0;k<4;++k)z[k]=g->m[lo][k];
    for(i=15;i>=0;--i){
        lo=x[i]&0xf;hi=(x[i]>>4)&0xf;
        if(i!=15){
            rem=z[3]&0xf;z[3]=(z[2]<<28)|(z[3]>>4);z[2]=(z[1]<<28)|(z[2]>>4);z[1]=(z[0]<<28)|(z[1]>>4);z[0]>>=4;
            z[0]^=last4[rem]<<16;for(k=0;k<4;++k)z[k]^=g->m[lo][k];
        }
        rem=z[3]&0xf;z[3]=(z[2]<<28)|(z[3]>>4);z[2]=(z[1]<<28)|(z[2]>>4);z[1]=(z[0]<<28)|(z[1]>>4);z[0]>>=4;
        z[0]^=last4[rem]<<16;for(k=0;k<4;++k)z[k]^=g->m[hi][k];
    }
    for(k=0;k<4;++k)put32be(x+4*k,z[k]);
}
static void gcm_ghash(const AesGcm *g,U8 y[16],const U8 *d,unsigned long n)
{
    unsigned long i;unsigned int j;
    for(i=0;i<n;i+=16){unsigned int take=(unsigned int)((n-i)>16?16:(n-i));for(j=0;j<take;++j)y[j]^=d[i+j];gcm_mult(g,y);}
}
static void gcm_ctr(const AesGcm *g,U8 cb[16],const U8 *in,U8 *out,unsigned long n)
{
    U8 ks[16];unsigned long i;unsigned int j;
    for(i=0;i<n;i+=16){
        unsigned int take=(unsigned int)((n-i)>16?16:(n-i));
        put32be(cb+12,get32be(cb+12)+1);aes_encrypt_block(g->rk,cb,ks);
        for(j=0;j<take;++j)out[i+j]=(U8)(in[i+j]^ks[j]);
    }
}
static void gcm_tag(const AesGcm *g,const U8 iv[12],const U8 *aad,unsigned long al,const U8 *ct,unsigned long cl,U8 tag[16])
{
    U8 y[16],lenblk[16],j0[16],ek[16];int i;
    memset(y,0,16);gcm_ghash(g,y,aad,al);gcm_ghash(g,y,ct,cl);
    memset(lenblk,0,16);put32be(lenblk+4,(U32)(al<<3));put32be(lenblk+12,(U32)(cl<<3));
    gcm_ghash(g,y,lenblk,16);
    memcpy(j0,iv,12);put32be(j0+12,1);aes_encrypt_block(g->rk,j0,ek);
    for(i=0;i<16;++i)tag[i]=(U8)(y[i]^ek[i]);
}
static void gcm_seal(const AesGcm *g,const U8 iv[12],const U8 *aad,unsigned long al,const U8 *in,unsigned long n,U8 *out,U8 tag[16])
{
    U8 cb[16];memcpy(cb,iv,12);put32be(cb+12,1);
    gcm_ctr(g,cb,in,out,n);gcm_tag(g,iv,aad,al,out,n,tag);
}
static int gcm_open(const AesGcm *g,const U8 iv[12],const U8 *aad,unsigned long al,const U8 *in,unsigned long n,U8 *out,const U8 tag[16])
{
    U8 cb[16],t[16];
    gcm_tag(g,iv,aad,al,in,n,t);if(!ct_equal(t,tag,16))return 0;
    memcpy(cb,iv,12);put32be(cb+12,1);gcm_ctr(g,cb,in,out,n);return 1;
}

/* ------------------- big numbers (16-bit limbs, Montgomery) -------------- */
typedef unsigned short BL;
#define BN_MAX 260
typedef struct { BL n[BN_MAX]; BL r2[BN_MAX]; BL one[BN_MAX]; BL ninv; int k; } Mont;
static int bn_set_bytes(BL *a,int k,const U8 *be,unsigned long len)
{
    unsigned long i;int j;
    while(len&&*be==0){++be;--len;}
    if(len>(unsigned long)k*2)return 0;
    for(j=0;j<k;++j)a[j]=0;
    for(i=0;i<len;++i){unsigned long pos=len-1-i;a[pos>>1]|=(BL)((U32)be[i]<<(8*(pos&1)));}
    return 1;
}
static void bn_get_bytes(const BL *a,int k,U8 *out,unsigned long outlen)
{
    unsigned long i;
    for(i=0;i<outlen;++i){unsigned long pos=outlen-1-i;out[i]=(pos>>1)<(unsigned long)k?(U8)((a[pos>>1]>>(8*(pos&1)))&0xff):0;}
}
static int bn_cmp(const BL *a,const BL *b,int k){int i;for(i=k-1;i>=0;--i){if(a[i]!=b[i])return a[i]>b[i]?1:-1;}return 0;}
static int bn_iszero(const BL *a,int k){int i;for(i=0;i<k;++i)if(a[i])return 0;return 1;}
static U32 bn_sub(BL *a,const BL *b,int k)
{
    U32 borrow=0;int i;
    for(i=0;i<k;++i){U32 d=(U32)a[i]-(U32)b[i]-borrow;a[i]=(BL)(d&0xffff);borrow=(d>>16)&1;}
    return borrow;
}
static U32 bn_add(BL *a,const BL *b,int k)
{
    U32 c=0;int i;
    for(i=0;i<k;++i){U32 s=(U32)a[i]+(U32)b[i]+c;a[i]=(BL)(s&0xffff);c=s>>16;}
    return c;
}
static U32 bn_shl1(BL *a,int k){U32 c=0;int i;for(i=0;i<k;++i){U32 v=((U32)a[i]<<1)|c;a[i]=(BL)(v&0xffff);c=v>>16;}return c;}
static int mont_init(Mont *m,const U8 *nbe,unsigned long nlen)
{
    int k,i;U32 x;
    while(nlen&&*nbe==0){++nbe;--nlen;}
    if(!nlen)return 0;
    k=(int)((nlen+1)/2);if(k>256||k<1)return 0;
    m->k=k;if(!bn_set_bytes(m->n,k,nbe,nlen))return 0;
    if(!(m->n[0]&1))return 0;
    x=m->n[0];for(i=0;i<4;++i)x=(x*(2-(U32)m->n[0]*x))&0xffff;
    m->ninv=(BL)((0x10000UL-x)&0xffff);
    memset(m->one,0,sizeof(m->one));m->one[0]=1;
    for(i=0;i<16*k;++i){U32 c=bn_shl1(m->one,k);if(c||bn_cmp(m->one,m->n,k)>=0)bn_sub(m->one,m->n,k);}
    memcpy(m->r2,m->one,sizeof(m->r2));
    for(i=0;i<16*k;++i){U32 c=bn_shl1(m->r2,k);if(c||bn_cmp(m->r2,m->n,k)>=0)bn_sub(m->r2,m->n,k);}
    return 1;
}
static void mont_mul(const Mont *m,BL *r,const BL *a,const BL *b)
{
    U32 t[BN_MAX+2];int i,j,k=m->k;U32 c,s,q;
    for(j=0;j<k+2;++j)t[j]=0;
    for(i=0;i<k;++i){
        c=0;
        for(j=0;j<k;++j){s=t[j]+(U32)a[j]*(U32)b[i]+c;t[j]=s&0xffff;c=s>>16;}
        s=t[k]+c;t[k]=s&0xffff;t[k+1]=s>>16;
        q=((t[0]*(U32)m->ninv)&0xffff);
        s=t[0]+q*(U32)m->n[0];c=s>>16;
        for(j=1;j<k;++j){s=t[j]+q*(U32)m->n[j]+c;t[j-1]=s&0xffff;c=s>>16;}
        s=t[k]+c;t[k-1]=s&0xffff;t[k]=t[k+1]+(s>>16);
    }
    {
        BL res[BN_MAX];for(j=0;j<k;++j)res[j]=(BL)t[j];
        if(t[k]||bn_cmp(res,m->n,k)>=0)bn_sub(res,m->n,k);
        for(j=0;j<k;++j)r[j]=res[j];
    }
}
static void mont_to(const Mont *m,BL *r,const BL *a){mont_mul(m,r,a,m->r2);}
static void mont_from(const Mont *m,BL *r,const BL *a){BL one[BN_MAX];int j;for(j=0;j<m->k;++j)one[j]=0;one[0]=1;mont_mul(m,r,a,one);}
static void mont_add(const Mont *m,BL *r,const BL *a,const BL *b)
{
    BL t[BN_MAX];int j;U32 c;
    for(j=0;j<m->k;++j)t[j]=a[j];
    c=bn_add(t,b,m->k);if(c||bn_cmp(t,m->n,m->k)>=0)bn_sub(t,m->n,m->k);
    for(j=0;j<m->k;++j)r[j]=t[j];
}
static void mont_sub(const Mont *m,BL *r,const BL *a,const BL *b)
{
    BL t[BN_MAX];int j;
    for(j=0;j<m->k;++j)t[j]=a[j];
    if(bn_sub(t,b,m->k))bn_add(t,m->n,m->k);
    for(j=0;j<m->k;++j)r[j]=t[j];
}
static void mont_pow(const Mont *m,BL *r,const BL *base,const U8 *exp,unsigned long el)
{
    BL acc[BN_MAX],b[BN_MAX];unsigned long i;int bit,j;
    for(j=0;j<m->k;++j){acc[j]=m->one[j];b[j]=base[j];}
    for(i=0;i<el;++i)for(bit=7;bit>=0;--bit){
        mont_mul(m,acc,acc,acc);
        if((exp[i]>>bit)&1)mont_mul(m,acc,acc,b);
    }
    for(j=0;j<m->k;++j)r[j]=acc[j];
}

/* ------------------------- RSA PKCS#1 v1.5 verify ------------------------ */
static const U8 di_sha256[]={0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
static const U8 di_sha384[]={0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30};
static const U8 di_sha512[]={0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40};
static int rsa_verify(const U8 *n,unsigned long nl,const U8 *e,unsigned long el,int hash,const U8 *msg,unsigned long ml,const U8 *sig,unsigned long sl)
{
    static Mont m;BL s[BN_MAX],x[BN_MAX];U8 digest[64],em[520],expect[520];
    const U8 *prefix;unsigned long plen,dl,k,i;
    while(nl&&*n==0){++n;--nl;}
    while(el&&*e==0){++e;--el;}
    if(nl<128||nl>512||el==0||el>4||!(e[el-1]&1)||(el==1&&e[0]<3))return 0;
    if(sl!=nl)return 0;
    if(hash==4){prefix=di_sha256;plen=sizeof(di_sha256);}
    else if(hash==5){prefix=di_sha384;plen=sizeof(di_sha384);}
    else if(hash==6){prefix=di_sha512;plen=sizeof(di_sha512);}
    else return 0;
    dl=hash_data(hash,msg,ml,digest);k=nl;
    if(k<plen+dl+11)return 0;
    if(!mont_init(&m,n,nl))return 0;
    if(!bn_set_bytes(s,m.k,sig,sl)||bn_cmp(s,m.n,m.k)>=0)return 0;
    mont_to(&m,x,s);mont_pow(&m,x,x,e,el);mont_from(&m,x,x);
    bn_get_bytes(x,m.k,em,k);
    expect[0]=0;expect[1]=1;for(i=2;i<k-plen-dl-1;++i)expect[i]=0xff;
    expect[k-plen-dl-1]=0;memcpy(expect+k-plen-dl,prefix,plen);memcpy(expect+k-dl,digest,dl);
    return ct_equal(em,expect,k);
}

/* ------------------------------ elliptic curves -------------------------- */
typedef struct { Mont f,o;BL b[BN_MAX],gx[BN_MAX],gy[BN_MAX];U8 pm2[48],nm2[48];unsigned int nb; } Curve;
typedef struct { BL x[BN_MAX],y[BN_MAX],z[BN_MAX]; } Pt;
static const char *curve_hex[2][5]={
 {"ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
  "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
  "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
  "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
  "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"},
 {"fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
  "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
  "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
  "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
  "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"}};
static Curve g_curve[2];static int g_curve_ready[2];
static unsigned int hex_to_bytes(const char *h,U8 *out)
{
    unsigned int n=0;
    while(h[0]&&h[1]){int a=isdigit((unsigned char)h[0])?h[0]-'0':h[0]-'a'+10,b=isdigit((unsigned char)h[1])?h[1]-'0':h[1]-'a'+10;out[n++]=(U8)((a<<4)|b);h+=2;}
    return n;
}
/* id: 0 = secp256r1, 1 = secp384r1 */
static Curve *curve_get(int id)
{
    Curve *c=&g_curve[id];U8 tmp[48];BL plain[BN_MAX];unsigned int l;
    if(g_curve_ready[id])return c;
    l=hex_to_bytes(curve_hex[id][0],tmp);c->nb=l;memcpy(c->pm2,tmp,l);c->pm2[l-1]=(U8)(c->pm2[l-1]-2);
    if(!mont_init(&c->f,tmp,l))return NULL;
    l=hex_to_bytes(curve_hex[id][1],tmp);memcpy(c->nm2,tmp,l);c->nm2[l-1]=(U8)(c->nm2[l-1]-2);
    if(!mont_init(&c->o,tmp,l))return NULL;
    l=hex_to_bytes(curve_hex[id][2],tmp);bn_set_bytes(plain,c->f.k,tmp,l);mont_to(&c->f,c->b,plain);
    l=hex_to_bytes(curve_hex[id][3],tmp);bn_set_bytes(plain,c->f.k,tmp,l);mont_to(&c->f,c->gx,plain);
    l=hex_to_bytes(curve_hex[id][4],tmp);bn_set_bytes(plain,c->f.k,tmp,l);mont_to(&c->f,c->gy,plain);
    g_curve_ready[id]=1;return c;
}
static void pt_inf(const Curve *c,Pt *p){int j;for(j=0;j<c->f.k;++j){p->x[j]=c->f.one[j];p->y[j]=c->f.one[j];p->z[j]=0;}}
static void pt_double(const Curve *c,Pt *r,const Pt *p)
{
    const Mont *m=&c->f;BL delta[BN_MAX],gamma[BN_MAX],beta[BN_MAX],alpha[BN_MAX],t1[BN_MAX],t2[BN_MAX],t3[BN_MAX];Pt o;int k=m->k,j;
    if(bn_iszero(p->z,k)){*r=*p;return;}
    mont_mul(m,delta,p->z,p->z);mont_mul(m,gamma,p->y,p->y);mont_mul(m,beta,p->x,gamma);
    mont_sub(m,t1,p->x,delta);mont_add(m,t2,p->x,delta);mont_mul(m,alpha,t1,t2);
    mont_add(m,t1,alpha,alpha);mont_add(m,alpha,t1,alpha);
    mont_mul(m,t1,alpha,alpha);mont_add(m,t2,beta,beta);mont_add(m,t2,t2,t2);mont_add(m,t3,t2,t2);
    mont_sub(m,o.x,t1,t3);
    mont_add(m,t1,p->y,p->z);mont_mul(m,t1,t1,t1);mont_sub(m,t1,t1,gamma);mont_sub(m,o.z,t1,delta);
    mont_sub(m,t1,t2,o.x);mont_mul(m,t1,alpha,t1);mont_mul(m,t3,gamma,gamma);
    mont_add(m,t3,t3,t3);mont_add(m,t3,t3,t3);mont_add(m,t3,t3,t3);
    mont_sub(m,o.y,t1,t3);
    for(j=0;j<k;++j){r->x[j]=o.x[j];r->y[j]=o.y[j];r->z[j]=o.z[j];}
}
static void pt_add(const Curve *c,Pt *r,const Pt *p,const Pt *q)
{
    const Mont *m=&c->f;int k=m->k,j;
    BL z1z1[BN_MAX],z2z2[BN_MAX],u1[BN_MAX],u2[BN_MAX],s1[BN_MAX],s2[BN_MAX],h[BN_MAX],i2[BN_MAX],jj[BN_MAX],rr[BN_MAX],v[BN_MAX],t[BN_MAX];Pt o;
    if(bn_iszero(p->z,k)){*r=*q;return;}
    if(bn_iszero(q->z,k)){*r=*p;return;}
    mont_mul(m,z1z1,p->z,p->z);mont_mul(m,z2z2,q->z,q->z);
    mont_mul(m,u1,p->x,z2z2);mont_mul(m,u2,q->x,z1z1);
    mont_mul(m,t,q->z,z2z2);mont_mul(m,s1,p->y,t);
    mont_mul(m,t,p->z,z1z1);mont_mul(m,s2,q->y,t);
    mont_sub(m,h,u2,u1);mont_sub(m,rr,s2,s1);mont_add(m,rr,rr,rr);
    if(bn_iszero(h,k)){if(bn_iszero(rr,k)){pt_double(c,r,p);}else pt_inf(c,r);return;}
    mont_add(m,t,h,h);mont_mul(m,i2,t,t);mont_mul(m,jj,h,i2);mont_mul(m,v,u1,i2);
    mont_mul(m,t,rr,rr);mont_sub(m,t,t,jj);mont_sub(m,t,t,v);mont_sub(m,o.x,t,v);
    mont_sub(m,t,v,o.x);mont_mul(m,t,rr,t);mont_mul(m,u1,s1,jj);mont_add(m,u1,u1,u1);mont_sub(m,o.y,t,u1);
    mont_add(m,t,p->z,q->z);mont_mul(m,t,t,t);mont_sub(m,t,t,z1z1);mont_sub(m,t,t,z2z2);mont_mul(m,o.z,t,h);
    for(j=0;j<k;++j){r->x[j]=o.x[j];r->y[j]=o.y[j];r->z[j]=o.z[j];}
}
static void pt_mul(const Curve *c,Pt *r,const U8 *scalar,unsigned int sl,const Pt *p)
{
    Pt acc,base;unsigned int i;int bit;
    base=*p;pt_inf(c,&acc);
    for(i=0;i<sl;++i)for(bit=7;bit>=0;--bit){
        pt_double(c,&acc,&acc);
        if((scalar[i]>>bit)&1)pt_add(c,&acc,&acc,&base);
    }
    *r=acc;
}
static int pt_affine(const Curve *c,const Pt *p,U8 *xo,U8 *yo)
{
    const Mont *m=&c->f;BL zi[BN_MAX],zi2[BN_MAX],t[BN_MAX];
    if(bn_iszero(p->z,m->k))return 0;
    mont_pow(m,zi,p->z,c->pm2,c->nb);mont_mul(m,zi2,zi,zi);
    mont_mul(m,t,p->x,zi2);mont_from(m,t,t);bn_get_bytes(t,m->k,xo,c->nb);
    if(yo){mont_mul(m,t,p->y,zi2);mont_mul(m,t,t,zi);mont_from(m,t,t);bn_get_bytes(t,m->k,yo,c->nb);}
    return 1;
}
static int pt_load(const Curve *c,Pt *p,const U8 *x,const U8 *y)
{
    const Mont *m=&c->f;BL px[BN_MAX],py[BN_MAX],l[BN_MAX],r[BN_MAX],t[BN_MAX];int k=m->k,j;
    if(!bn_set_bytes(px,k,x,c->nb)||!bn_set_bytes(py,k,y,c->nb))return 0;
    if(bn_cmp(px,m->n,k)>=0||bn_cmp(py,m->n,k)>=0)return 0;
    mont_to(m,p->x,px);mont_to(m,p->y,py);for(j=0;j<k;++j)p->z[j]=m->one[j];
    mont_mul(m,l,p->y,p->y);
    mont_mul(m,t,p->x,p->x);mont_mul(m,r,t,p->x);
    mont_add(m,t,p->x,p->x);mont_add(m,t,t,p->x);mont_sub(m,r,r,t);mont_add(m,r,r,c->b);
    return bn_cmp(l,r,k)==0;
}
static void pt_base(const Curve *c,Pt *p){int j;for(j=0;j<c->f.k;++j){p->x[j]=c->gx[j];p->y[j]=c->gy[j];p->z[j]=c->f.one[j];}}
/* ECDHE: produce our public point (04||X||Y) and the shared X coordinate. */
static int ecdh(int curveid,const U8 *peer,unsigned long peerlen,U8 *mypub,U8 *shared)
{
    Curve *c=curve_get(curveid);Pt g,q,r;U8 d[48];BL dn[BN_MAX];U8 xy[96];
    if(!c||peerlen!=1+2*(unsigned long)c->nb||peer[0]!=4)return 0;
    if(!pt_load(c,&q,peer+1,peer+1+c->nb))return 0;
    do{rng_bytes(d,c->nb);bn_set_bytes(dn,c->o.k,d,c->nb);}while(bn_iszero(dn,c->o.k)||bn_cmp(dn,c->o.n,c->o.k)>=0);
    pt_base(c,&g);pt_mul(c,&r,d,c->nb,&g);
    if(!pt_affine(c,&r,xy,xy+c->nb))return 0;
    mypub[0]=4;memcpy(mypub+1,xy,2*c->nb);
    pt_mul(c,&r,d,c->nb,&q);
    if(!pt_affine(c,&r,shared,NULL))return 0;
    memset(d,0,sizeof(d));
    return 1;
}
/* ECDSA verification; pub is an uncompressed point; sig is DER SEQUENCE{INTEGER r,INTEGER s}. */
static int der_int(const U8 **p,const U8 *end,const U8 **v,unsigned long *l)
{
    unsigned long len;
    if(*p+2>end||**p!=0x02)return 0;
    len=(*p)[1];*p+=2;if(len&0x80||*p+len>end||len==0)return 0;
    *v=*p;*l=len;*p+=len;return 1;
}
static int ecdsa_verify(int curveid,const U8 *pub,unsigned long publen,int hash,const U8 *msg,unsigned long ml,const U8 *sig,unsigned long sl)
{
    Curve *c=curve_get(curveid);const Mont *o;Pt g,q,p1,p2;U8 digest[64],ebuf[48],ub1[48],ub2[48],xo[48];
    const U8 *p=sig,*end=sig+sl,*rv,*sv;unsigned long rl,rsl,dl,i;
    BL r[BN_MAX],s[BN_MAX],e[BN_MAX],w[BN_MAX],u1[BN_MAX],u2[BN_MAX],t[BN_MAX],xn[BN_MAX];int k;
    if(!c||publen!=1+2*(unsigned long)c->nb||pub[0]!=4)return 0;
    if(sl<8||sig[0]!=0x30||sig[1]&0x80||(unsigned long)sig[1]+2!=sl)return 0;
    p=sig+2;
    if(!der_int(&p,end,&rv,&rl)||!der_int(&p,end,&sv,&rsl)||p!=end)return 0;
    if((rv[0]&0x80)||(sv[0]&0x80))return 0;
    o=&c->o;k=o->k;
    if(!bn_set_bytes(r,k,rv,rl)||!bn_set_bytes(s,k,sv,rsl))return 0;
    if(bn_iszero(r,k)||bn_iszero(s,k)||bn_cmp(r,o->n,k)>=0||bn_cmp(s,o->n,k)>=0)return 0;
    if(!pt_load(c,&q,pub+1,pub+1+c->nb))return 0;
    dl=hash_data(hash,msg,ml,digest);if(!dl)return 0;
    if(dl>c->nb)dl=c->nb;
    memcpy(ebuf,digest,dl);bn_set_bytes(e,k,ebuf,dl);
    if(bn_cmp(e,o->n,k)>=0)bn_sub(e,o->n,k);
    mont_to(o,s,s);mont_pow(o,w,s,c->nm2,c->nb);
    mont_to(o,t,e);mont_mul(o,u1,t,w);mont_from(o,u1,u1);
    mont_to(o,t,r);mont_mul(o,u2,t,w);mont_from(o,u2,u2);
    bn_get_bytes(u1,k,ub1,c->nb);bn_get_bytes(u2,k,ub2,c->nb);
    pt_base(c,&g);pt_mul(c,&p1,ub1,c->nb,&g);pt_mul(c,&p2,ub2,c->nb,&q);pt_add(c,&p1,&p1,&p2);
    if(!pt_affine(c,&p1,xo,NULL))return 0;
    bn_set_bytes(xn,k,xo,c->nb);if(bn_cmp(xn,o->n,k)>=0)bn_sub(xn,o->n,k);
    (void)i;
    return bn_cmp(xn,r,k)==0;
}

/* ------------------------------- base64 ---------------------------------- */
static int b64_val(int c)
{
    if(c>='A'&&c<='Z')return c-'A';
    if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52;
    if(c=='+')return 62;
    if(c=='/')return 63;
    return -1;
}
static unsigned long b64_decode(const char *s,unsigned long n,U8 *out)
{
    unsigned long i,o=0;U32 acc=0;int bits=0;
    for(i=0;i<n;++i){int v;if(s[i]=='=')break;v=b64_val((unsigned char)s[i]);if(v<0)continue;acc=(acc<<6)|(U32)v;bits+=6;if(bits>=8){bits-=8;out[o++]=(U8)((acc>>bits)&0xff);}}
    return o;
}
static void b64_encode(const U8 *p,unsigned long n,char *out)
{
    static const char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned long i;
    for(i=0;i<n;i+=3){
        U32 v=(U32)p[i]<<16;if(i+1<n)v|=(U32)p[i+1]<<8;if(i+2<n)v|=p[i+2];
        *out++=t[(v>>18)&63];*out++=t[(v>>12)&63];*out++=(i+1<n)?t[(v>>6)&63]:'=';*out++=(i+2<n)?t[v&63]:'=';
    }
    *out=0;
}

/* ------------------------------ X.509 reader ----------------------------- */
typedef struct {
    const U8 *der;unsigned long derlen;const U8 *tbs;unsigned long tbslen;
    const U8 *issuer;unsigned long issuerlen;const U8 *subject;unsigned long subjectlen;
    const U8 *salg;unsigned long salglen;const U8 *sig;unsigned long siglen;
    char nb[16],na[16];
    int keytype;int curve;const U8 *rn;unsigned long rnl;const U8 *re;unsigned long rel;const U8 *ecpt;unsigned long ecptl;
    const U8 *san;unsigned long sanl;int has_bc,is_ca;
} Cert;
static int der_next(const U8 **p,const U8 *end,unsigned int *tag,const U8 **val,unsigned long *len)
{
    const U8 *q=*p;unsigned long l;unsigned int nb;
    if(q+2>end)return 0;
    *tag=q[0];l=q[1];q+=2;
    if(l&0x80){nb=(unsigned int)(l&0x7f);if(nb==0||nb>3||q+nb>end)return 0;l=0;while(nb--)l=(l<<8)|*q++;}
    if(l>(unsigned long)(end-q))return 0;
    *val=q;*len=l;*p=q+l;return 1;
}
static int der_expect(const U8 **p,const U8 *end,unsigned int want,const U8 **val,unsigned long *len)
{
    unsigned int tag;
    if(!der_next(p,end,&tag,val,len)||tag!=want)return 0;
    return 1;
}
static int der_time(unsigned int tag,const U8 *v,unsigned long l,char out[16])
{
    unsigned long i,off;
    if(tag==0x17){if(l!=13)return 0;off=2;}else if(tag==0x18){if(l!=15)return 0;off=4;}else return 0;
    if(v[l-1]!='Z')return 0;
    for(i=0;i+1<l;++i)if(!isdigit(v[i]))return 0;
    if(tag==0x17){out[0]=(v[0]<'5')?'2':'1';out[1]=(v[0]<'5')?'0':'9';memcpy(out+2,v,12);}
    else memcpy(out,v,14);
    out[14]=0;(void)off;return 1;
}
static int cert_parse(Cert *c,const U8 *der,unsigned long len)
{
    const U8 *p=der,*end=der+len,*v,*q,*qe,*t,*te,*a,*ae,*b,*be,*x;unsigned long l,tl,al,bl,xl;unsigned int tag;
    const U8 *start;
    static const U8 oid_rsa[]={0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
    static const U8 oid_ec[]={0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
    static const U8 oid_p256[]={0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
    static const U8 oid_p384[]={0x2b,0x81,0x04,0x00,0x22};
    memset(c,0,sizeof(*c));c->der=der;c->derlen=len;
    if(!der_expect(&p,end,0x30,&v,&l))return 0;
    q=v;qe=v+l;start=q;
    if(!der_expect(&q,qe,0x30,&t,&tl))return 0;
    c->tbs=start;c->tbslen=(unsigned long)(q-start);te=t+tl;
    if(!der_expect(&q,qe,0x30,&a,&al))return 0;
    {const U8 *ap=a;if(!der_expect(&ap,a+al,0x06,&x,&xl))return 0;c->salg=x;c->salglen=xl;}
    if(!der_expect(&q,qe,0x03,&a,&al)||al<2||a[0]!=0)return 0;
    c->sig=a+1;c->siglen=al-1;
    /* TBSCertificate */
    p=t;
    if(p<te&&*p==0xa0){if(!der_next(&p,te,&tag,&a,&al))return 0;}
    if(!der_expect(&p,te,0x02,&a,&al))return 0;
    if(!der_expect(&p,te,0x30,&a,&al))return 0;
    start=p;if(!der_expect(&p,te,0x30,&a,&al))return 0;c->issuer=start;c->issuerlen=(unsigned long)(p-start);
    if(!der_expect(&p,te,0x30,&a,&al))return 0;
    ae=a+al;
    {const U8 *vp=a;if(!der_next(&vp,ae,&tag,&b,&bl)||!der_time(tag,b,bl,c->nb))return 0;if(!der_next(&vp,ae,&tag,&b,&bl)||!der_time(tag,b,bl,c->na))return 0;}
    start=p;if(!der_expect(&p,te,0x30,&a,&al))return 0;c->subject=start;c->subjectlen=(unsigned long)(p-start);
    if(!der_expect(&p,te,0x30,&a,&al))return 0;
    ae=a+al;
    {
        const U8 *sp=a,*algv,*oidv,*bits;unsigned long algl,oidl,bitl;
        if(!der_expect(&sp,ae,0x30,&algv,&algl))return 0;
        if(!der_expect(&sp,ae,0x03,&bits,&bitl)||bitl<2||bits[0]!=0)return 0;
        {
            const U8 *ap=algv;
            if(!der_expect(&ap,algv+algl,0x06,&oidv,&oidl))return 0;
            if(oidl==sizeof(oid_rsa)&&!memcmp(oidv,oid_rsa,oidl)){
                const U8 *kp=bits+1,*kv;unsigned long kl;
                if(!der_expect(&kp,bits+bitl,0x30,&kv,&kl))return 0;
                {const U8 *ip=kv;if(!der_expect(&ip,kv+kl,0x02,&b,&bl))return 0;c->rn=b;c->rnl=bl;if(!der_expect(&ip,kv+kl,0x02,&b,&bl))return 0;c->re=b;c->rel=bl;}
                c->keytype=1;
            }else if(oidl==sizeof(oid_ec)&&!memcmp(oidv,oid_ec,oidl)){
                if(!der_expect(&ap,algv+algl,0x06,&oidv,&oidl))return 0;
                if(oidl==sizeof(oid_p256)&&!memcmp(oidv,oid_p256,oidl))c->curve=0;
                else if(oidl==sizeof(oid_p384)&&!memcmp(oidv,oid_p384,oidl))c->curve=1;
                else return 0;
                c->ecpt=bits+1;c->ecptl=bitl-1;c->keytype=2;
            }else return 0;
        }
    }
    while(p<te){
        if(!der_next(&p,te,&tag,&a,&al))return 0;
        if(tag==0xa3){
            const U8 *ep=a,*ev;unsigned long el;
            if(!der_expect(&ep,a+al,0x30,&ev,&el))return 0;
            ep=ev;
            while(ep<ev+el){
                const U8 *xp,*oid,*oct;unsigned long ol,ctl;unsigned int t2;
                if(!der_expect(&ep,ev+el,0x30,&b,&bl))return 0;
                xp=b;be=b+bl;
                if(!der_expect(&xp,be,0x06,&oid,&ol))return 0;
                if(!der_next(&xp,be,&t2,&oct,&ctl))return 0;
                if(t2==0x01&&!der_next(&xp,be,&t2,&oct,&ctl))return 0;
                if(t2!=0x04)return 0;
                if(ol==3&&oid[0]==0x55&&oid[1]==0x1d&&oid[2]==0x13){
                    const U8 *bp=oct;const U8 *bv;unsigned long bvl;
                    if(der_expect(&bp,oct+ctl,0x30,&bv,&bvl)){
                        const U8 *ip=bv;unsigned int t3;const U8 *iv;unsigned long il;
                        c->has_bc=1;c->is_ca=0;
                        if(ip<bv+bvl&&der_next(&ip,bv+bvl,&t3,&iv,&il)&&t3==0x01&&il==1&&iv[0]!=0)c->is_ca=1;
                    }
                }else if(ol==3&&oid[0]==0x55&&oid[1]==0x1d&&oid[2]==0x11){
                    const U8 *sp=oct;const U8 *sv;unsigned long sl;
                    if(der_expect(&sp,oct+ctl,0x30,&sv,&sl)){c->san=sv;c->sanl=sl;}
                }
            }
        }
    }
    (void)qe;(void)be;
    return 1;
}
static int dns_match(const char *pat,unsigned long pl,const char *host)
{
    unsigned long hl=strlen(host),i;
    if(pl>2&&pat[0]=='*'&&pat[1]=='.'){
        const char *dot=strchr(host,'.');
        if(!dot||dot==host)return 0;
        pat+=1;pl-=1;hl=strlen(dot);host=dot;
    }
    if(pl!=hl)return 0;
    for(i=0;i<pl;++i)if(tolower((unsigned char)pat[i])!=tolower((unsigned char)host[i]))return 0;
    return 1;
}
static int is_ip_literal(const char *h)
{
    int dots=0;if(!*h)return 0;
    for(;*h;++h){if(*h=='.')++dots;else if(!isdigit((unsigned char)*h))return 0;}
    return dots==3;
}
static int cert_match_host(const Cert *c,const char *host)
{
    int ip=is_ip_literal(host),any=0;U8 ipb[4];
    if(ip){unsigned int a,b,cc,d;if(sscanf(host,"%u.%u.%u.%u",&a,&b,&cc,&d)!=4||a>255||b>255||cc>255||d>255)return 0;ipb[0]=(U8)a;ipb[1]=(U8)b;ipb[2]=(U8)cc;ipb[3]=(U8)d;}
    if(c->san){
        const U8 *p=c->san,*end=c->san+c->sanl,*v;unsigned long l;unsigned int tag;
        while(p<end&&der_next(&p,end,&tag,&v,&l)){
            if(tag==0x82){any=1;if(!ip&&dns_match((const char*)v,l,host))return 1;}
            else if(tag==0x87){any=1;if(ip&&l==4&&!memcmp(v,ipb,4))return 1;}
        }
    }
    if(any||ip)return 0;
    {   /* no SAN: fall back to the subject common name */
        const U8 *p,*end,*sv;unsigned long sl;
        p=c->subject;end=c->subject+c->subjectlen;
        if(!der_expect(&p,end,0x30,&sv,&sl))return 0;
        p=sv;end=sv+sl;
        while(p<end){
            const U8 *setv,*seqv,*oid,*val;unsigned long setl,seql,ol,vl;unsigned int vt;const U8 *sp;
            if(!der_expect(&p,end,0x31,&setv,&setl))return 0;
            sp=setv;
            if(!der_expect(&sp,setv+setl,0x30,&seqv,&seql))return 0;
            {const U8 *qp=seqv;if(!der_expect(&qp,seqv+seql,0x06,&oid,&ol)||!der_next(&qp,seqv+seql,&vt,&val,&vl))return 0;
             if(ol==3&&oid[0]==0x55&&oid[1]==4&&oid[2]==3&&dns_match((const char*)val,vl,host))return 1;}
        }
    }
    return 0;
}
static int cert_check_sig(const Cert *c,const Cert *iss)
{
    static const U8 rsa_pre[]={0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01};
    static const U8 ec_pre[]={0x2a,0x86,0x48,0xce,0x3d,0x04,0x03};
    int hash=0,isrsa=0;
    if(c->salglen==9&&!memcmp(c->salg,rsa_pre,8)&&c->salg[8]>=0x0b&&c->salg[8]<=0x0d){isrsa=1;hash=c->salg[8]-0x0b+4;}
    else if(c->salglen==8&&!memcmp(c->salg,ec_pre,7)&&c->salg[7]>=2&&c->salg[7]<=4){hash=c->salg[7]-2+4;}
    else return 0;
    if(isrsa){if(iss->keytype!=1)return 0;return rsa_verify(iss->rn,iss->rnl,iss->re,iss->rel,hash,c->tbs,c->tbslen,c->sig,c->siglen);}
    if(iss->keytype!=2)return 0;
    return ecdsa_verify(iss->curve,iss->ecpt,iss->ecptl,hash,c->tbs,c->tbslen,c->sig,c->siglen);
}

/* ------------------------------ trust store ------------------------------ */
typedef struct { U8 *der;unsigned long len;Cert c; } Anchor;
static Anchor *g_anchors;static unsigned int g_nanchors,g_anchor_cap;static int g_anchors_loaded;
static int trust_add(const U8 *der,unsigned long len)
{
    Anchor *na;U8 *copy;Cert tmp;
    if(!cert_parse(&tmp,der,len))return 0;
    copy=(U8*)malloc(len);if(!copy)return 0;memcpy(copy,der,len);
    if(g_nanchors==g_anchor_cap){
        unsigned int cap=g_anchor_cap?g_anchor_cap*2:64;
        na=(Anchor*)realloc(g_anchors,cap*sizeof(Anchor));if(!na){free(copy);return 0;}
        g_anchors=na;g_anchor_cap=cap;
    }
    g_anchors[g_nanchors].der=copy;g_anchors[g_nanchors].len=len;
    cert_parse(&g_anchors[g_nanchors].c,copy,len);++g_nanchors;return 1;
}
static void trust_load_pem(const Buffer *b)
{
    const char *p=(const char*)b->data,*end=(const char*)b->data+b->len;
    while(p<end){
        const char *s=strstr(p,"-----BEGIN CERTIFICATE-----"),*e;U8 *der;unsigned long n;
        if(!s)break;
        s+=27;e=strstr(s,"-----END CERTIFICATE-----");if(!e)break;
        der=(U8*)malloc((unsigned long)(e-s)/4*3+8);
        if(der){n=b64_decode(s,(unsigned long)(e-s),der);if(n)trust_add(der,n);free(der);}
        p=e+25;
    }
}
static const char *exe_dir(void)
{
    static char dir[MAX_PATH_GIT];static int done;
    if(!done){
        done=1;dir[0]=0;
#if defined(__WATCOMC__)
        {char *s1,*s2,*s;DWORD n=GetModuleFileNameA(NULL,dir,(DWORD)sizeof(dir)-1);dir[n]=0;
         s1=strrchr(dir,'\\');s2=strrchr(dir,'/');s=s1;if(!s||(s2&&s2>s))s=s2;if(s)s[1]=0;else dir[0]=0;}
#endif
    }
    return dir;
}
/* ---- global configuration: git config --global user.name / user.email / user.token ----
 * Stored in a small INI file ("gitconfig") next to git.exe, or in the file named by
 * GIT_CONFIG_GLOBAL. (Win98 has no per-user home directory, so the exe folder is used.) */
static void global_config_path(char *out,size_t cap)
{
    const char *env=getenv("GIT_CONFIG_GLOBAL");
    out[0]=0;
    if(env&&*env){if(strlen(env)<cap)strcpy(out,env);return;}
#if defined(__WATCOMC__)
    if(strlen(exe_dir())+10<cap){strcpy(out,exe_dir());strcat(out,"gitconfig");}
#else
    env=getenv("HOME");
    if(env&&*env&&strlen(env)+20<cap){strcpy(out,env);strcat(out,"/.tinyvcs98-gitconfig");}
    else if(cap>10)strcpy(out,"gitconfig");
#endif
}
/* Is this line "[user]"? */
static int cfg_is_user_header(const char *l,size_t n)
{
    while(n&&(*l==' '||*l=='\t')){++l;--n;}
    while(n&&(l[n-1]=='\r'||l[n-1]==' '||l[n-1]=='\t'))--n;
    return n==6&&!memcmp(l,"[user]",6);
}
/* If the line is "key = value" with the given key, return a pointer to the value start and set *vend. */
static const char *cfg_match_key(const char *l,size_t n,const char *key,const char **vend)
{
    size_t kl=strlen(key);const char *e=l+n;
    while(l<e&&(*l==' '||*l=='\t'))++l;
    if((size_t)(e-l)<kl||STRNICMP(l,key,kl)!=0)return NULL;
    l+=kl;while(l<e&&(*l==' '||*l=='\t'))++l;
    if(l>=e||*l!='=')return NULL;
    ++l;while(l<e&&(*l==' '||*l=='\t'))++l;
    while(e>l&&(e[-1]=='\r'||e[-1]==' '||e[-1]=='\t'))--e;
    *vend=e;return l;
}
static int global_get(const char *key,char *out,size_t cap)
{
    char path[MAX_PATH_GIT];Buffer b;unsigned long p=0;int in_user=0,found=0;const char *k;
    out[0]=0;if(STRNICMP(key,"user.",5)!=0)return 0;k=key+5;
    global_config_path(path,sizeof(path));if(!path[0]||!read_file(path,&b))return 0;
    while(p<b.len){
        unsigned long e=p;const char *v,*ve;
        while(e<b.len&&b.data[e]!='\n')++e;
        if(b.data[p]=='[')in_user=cfg_is_user_header((const char*)b.data+p,e-p);
        else if(in_user&&(v=cfg_match_key((const char*)b.data+p,e-p,k,&ve))!=NULL){
            if((size_t)(ve-v)<cap){memcpy(out,v,(size_t)(ve-v));out[ve-v]=0;found=1;}
        }
        p=e+1;
    }
    free(b.data);return found;
}
/* Set (or, with an empty value, remove) one key in [user]. The rest of the file is preserved. */
static int global_set(const char *key,const char *value)
{
    char path[MAX_PATH_GIT];Buffer b,o;unsigned long p=0;int in_user=0,done=0,had_user=0;const char *k;char line[600];int n;
    if(STRNICMP(key,"user.",5)!=0)return 0;k=key+5;
    if(strlen(k)+strlen(value)+8>=sizeof(line)||strchr(value,'\n')||strchr(value,'\r')){fprintf(stderr,"git: value too long or contains a line break\n");return 0;}
    global_config_path(path,sizeof(path));if(!path[0])return 0;
    if(!read_file(path,&b)){b.data=NULL;b.len=0;}
    o.data=NULL;o.len=0;
    n=sprintf(line,"\t%s = %s\n",k,value);
    while(p<b.len){
        unsigned long e=p,next;const char *ve;
        while(e<b.len&&b.data[e]!='\n')++e;
        next=e<b.len?e+1:e;
        if(b.data[p]=='['){
            if(in_user&&!done&&*value){if(!buf_append(&o,line,(unsigned long)n))goto fail;done=1;}
            in_user=cfg_is_user_header((const char*)b.data+p,e-p);if(in_user)had_user=1;
        }else if(in_user&&cfg_match_key((const char*)b.data+p,e-p,k,&ve)!=NULL){
            if(!done&&*value){if(!buf_append(&o,line,(unsigned long)n))goto fail;done=1;}
            p=next;continue;                                   /* drop old line (or all duplicates) */
        }
        if(!buf_append(&o,(const char*)b.data+p,e-p))goto fail;
        if(!buf_append(&o,"\n",1))goto fail;
        p=next;
    }
    if(!done&&*value){
        if(!had_user&&!buf_append(&o,"[user]\n",7))goto fail;
        if(!buf_append(&o,line,(unsigned long)n))goto fail;
        /* if [user] existed but was last, the line lands at the end of the file, i.e. inside it */
    }
    if(!write_file(path,o.data?o.data:(const U8*)"",o.len)){fprintf(stderr,"git: cannot write %s\n",path);goto fail;}
    if(b.data)free(b.data);if(o.data)free(o.data);return 1;
fail:
    if(b.data)free(b.data);if(o.data)free(o.data);return 0;
}
static int trust_load(void)
{
    char path[MAX_PATH_GIT+32];const char *env;Buffer b;
    if(g_anchors_loaded)return g_nanchors>0;
    g_anchors_loaded=1;
    env=getenv("GIT_SSL_CAINFO");
    if(env&&*env){if(read_file(env,&b)){trust_load_pem(&b);free(b.data);}else fprintf(stderr,"git: cannot read CA bundle %s\n",env);return g_nanchors>0;}
    strcpy(path,exe_dir());strcat(path,"ca-bundle.crt");
    if(read_file(path,&b)){trust_load_pem(&b);free(b.data);}
#if !defined(__WATCOMC__)
    if(!g_nanchors&&read_file("/etc/ssl/certs/ca-certificates.crt",&b)){trust_load_pem(&b);free(b.data);}
#endif
    return g_nanchors>0;
}
static int env_flag(const char *name){const char *v=getenv(name);return v&&*v&&strcmp(v,"0")&&STRICMP(v,"false")&&STRICMP(v,"no");}
static void now_string(char out[24])
{
    time_t t=time(NULL);struct tm *g=gmtime(&t);
    if(!g){strcpy(out,"19700101000000");return;}
    sprintf(out,"%04d%02d%02d%02d%02d%02d",g->tm_year+1900,g->tm_mon+1,g->tm_mday,g->tm_hour,g->tm_min,g->tm_sec);
}
static int cert_date_ok(const Cert *c,const char *now,const char **why)
{
    if(strcmp(now,c->nb)<0){*why="certificate is not yet valid (is the system clock correct?)";return 0;}
    if(strcmp(now,c->na)>0){*why="certificate has expired (is the system clock correct?)";return 0;}
    return 1;
}
static int tls_check_chain(const Cert *chain,int n,const char *host)
{
    char now[24];const char *why=NULL;int cur=0,depth,j;unsigned int i;int nodate=env_flag("GIT_SSL_NO_VERIFY_DATE");
    now_string(now);
    if(!cert_match_host(&chain[0],host)){fprintf(stderr,"git: TLS: certificate is not valid for host '%s'\n",host);return 0;}
    if(!trust_load()){fprintf(stderr,"git: TLS: no CA certificates loaded; place ca-bundle.crt next to git.exe or set GIT_SSL_CAINFO\n");return 0;}
    for(depth=0;depth<10;++depth){
        const Cert *c=&chain[cur];
        if(!nodate&&!cert_date_ok(c,now,&why)){fprintf(stderr,"git: TLS: %s\n",why);return 0;}
        for(i=0;i<g_nanchors;++i)if(g_anchors[i].len==c->derlen&&!memcmp(g_anchors[i].der,c->der,c->derlen))return 1;
        for(i=0;i<g_nanchors;++i){
            const Cert *a=&g_anchors[i].c;
            if(a->subjectlen==c->issuerlen&&!memcmp(a->subject,c->issuer,c->issuerlen)&&cert_check_sig(c,a)){
                if(!nodate&&!cert_date_ok(a,now,&why)){fprintf(stderr,"git: TLS: trusted root: %s\n",why);return 0;}
                return 1;
            }
        }
        {int found=-1;
         for(j=cur+1;j<n;++j){
             const Cert *q=&chain[j];
             if(q->subjectlen==c->issuerlen&&!memcmp(q->subject,c->issuer,c->issuerlen)&&q->has_bc&&q->is_ca&&cert_check_sig(c,q)){found=j;break;}
         }
         if(found<0){fprintf(stderr,"git: TLS: certificate chain does not lead to a trusted CA\n");return 0;}
         cur=found;}
    }
    fprintf(stderr,"git: TLS: certificate chain too long\n");return 0;
}

/* ------------------------- TLS 1.2 record + handshake -------------------- */
typedef struct {
    net_socket_t s;int tls,enc,dec,eof,hello_sent;
    AesGcm cw,sw;U8 civ[4],siv[4];U32 cseq[2],sseq[2];
    U8 rec[18432+16];unsigned long reclen,recpos;
    U8 wbuf[5+8+16384+16+16];
    Buffer hs;unsigned long hspos;Buffer tr;
} Conn;
static int tls_fail(const char *msg){fprintf(stderr,"git: TLS error: %s\n",msg);return 0;}
static int net_read_full(net_socket_t s,U8 *p,unsigned long n)
{
    unsigned long got=0;
    while(got<n){int r=recv(s,(char*)p+got,(int)(n-got),0);if(r<0)return -1;if(r==0)return got?-1:0;got+=(unsigned long)r;}
    return 1;
}
static int net_send_all(net_socket_t s,const U8 *p,unsigned long n)
{
    while(n){int k=(int)(n>32768UL?32768UL:n);int r=send(s,(const char*)p,k,0);if(r<=0)return 0;p+=r;n-=(unsigned long)r;}return 1;
}
static void seq_inc(U32 q[2]){if(++q[1]==0)++q[0];}
static int tls_write_record(Conn *c,int type,const U8 *data,unsigned long n)
{
    U8 *w=c->wbuf;unsigned long bodylen;
    w[0]=(U8)type;w[1]=3;w[2]=(U8)((type==22&&!c->hello_sent)?1:3);
    if(!c->enc){
        if(n>16384)return 0;
        put16be(w+3,(unsigned int)n);memcpy(w+5,data,n);bodylen=n;
    }else{
        U8 aad[13],nonce[12];
        if(n>16384)return 0;
        put32be(aad,c->cseq[0]);put32be(aad+4,c->cseq[1]);aad[8]=(U8)type;aad[9]=3;aad[10]=3;put16be(aad+11,(unsigned int)n);
        memcpy(nonce,c->civ,4);memcpy(nonce+4,aad,8);memcpy(w+5,aad,8);
        gcm_seal(&c->cw,nonce,aad,13,data,n,w+13,w+13+n);
        bodylen=8+n+16;put16be(w+3,(unsigned int)bodylen);seq_inc(c->cseq);
    }
    if(type==22)c->hello_sent=1;
    return net_send_all(c->s,w,5+bodylen);
}
/* returns 1 = record in c->rec, 0 = clean EOF, -1 = error */
static int tls_read_record(Conn *c,int *type)
{
    U8 hdr[5];unsigned long len;int r;
    r=net_read_full(c->s,hdr,5);if(r<=0)return r;
    len=get16be(hdr+3);
    if(hdr[1]!=3||len>18432)return -1;
    if(net_read_full(c->s,c->rec,len)!=1)return -1;
    *type=hdr[0];
    if(c->dec){
        U8 aad[13],nonce[12];unsigned long pl;
        if(len<24)return -1;
        pl=len-24;
        put32be(aad,c->sseq[0]);put32be(aad+4,c->sseq[1]);aad[8]=hdr[0];aad[9]=3;aad[10]=3;put16be(aad+11,(unsigned int)pl);
        memcpy(nonce,c->siv,4);memcpy(nonce+4,c->rec,8);
        if(!gcm_open(&c->sw,nonce,aad,13,c->rec+8,pl,c->rec,c->rec+8+pl))return -1;
        seq_inc(c->sseq);len=pl;
    }
    c->reclen=len;c->recpos=0;return 1;
}
static int tls_hs_next(Conn *c,int *type,const U8 **body,unsigned long *blen)
{
    if(c->hspos==c->hs.len){c->hs.len=0;c->hspos=0;}
    for(;;){
        unsigned long avail=c->hs.len-c->hspos;
        if(avail>=4){
            const U8 *h=c->hs.data+c->hspos;unsigned long l=((unsigned long)h[1]<<16)|((unsigned long)h[2]<<8)|h[3];
            if(l>(1UL<<20))return tls_fail("handshake message too large");
            if(avail>=4+l){
                *type=h[0];*body=h+4;*blen=l;
                if(!buf_append(&c->tr,h,4+l))return 0;
                c->hspos+=4+l;return 1;
            }
        }
        {int rt,r=tls_read_record(c,&rt);
         if(r<=0)return tls_fail("connection closed during handshake");
         if(rt==22){if(!buf_append(&c->hs,c->rec,c->reclen))return 0;}
         else if(rt==21)return tls_fail("server sent an alert during handshake");
         else return tls_fail("unexpected record during handshake");}
    }
}
static int tls_hs_send(Conn *c,int type,const U8 *body,unsigned long n)
{
    U8 *m=(U8*)malloc(n+4);int ok;
    if(!m)return 0;
    m[0]=(U8)type;m[1]=(U8)(n>>16);m[2]=(U8)(n>>8);m[3]=(U8)n;memcpy(m+4,body,n);
    ok=buf_append(&c->tr,m,n+4)&&tls_write_record(c,22,m,n+4);
    free(m);return ok;
}
static void tls_transcript_hash(Conn *c,U8 out[32]){sha256_data(c->tr.data,c->tr.len,out);}
static int tls_connect(Conn *c,const char *host)
{
    U8 cr[32],sr[32],ch[400],*e,*extlen;unsigned long hl=strlen(host),n;
    int mt,suite=0,noverify=env_flag("GIT_SSL_NO_VERIFY"),certreq=0,have_ske=0,i;
    Buffer certmsg,ske;const U8 *b;unsigned long bl;
    Cert chain[12];int nchain=0;unsigned long pklen=0,pmslen=0;
    U8 mypub[97],pms[48],master[48],kb[40],seed[64],vd[12],hash[32],fin[16];
    certmsg.data=NULL;certmsg.len=0;ske.data=NULL;ske.len=0;
    if(hl>200)return tls_fail("host name too long");
    rng_bytes(cr,32);
    /* ClientHello */
    n=0;ch[n++]=3;ch[n++]=3;memcpy(ch+n,cr,32);n+=32;ch[n++]=0;
    ch[n++]=0;ch[n++]=4;ch[n++]=0xc0;ch[n++]=0x2f;ch[n++]=0xc0;ch[n++]=0x2b;
    ch[n++]=1;ch[n++]=0;
    extlen=ch+n;n+=2;e=ch+n;
    if(!is_ip_literal(host)){
        put16be(e,0);put16be(e+2,hl+5);put16be(e+4,hl+3);e[6]=0;put16be(e+7,hl);memcpy(e+9,host,hl);e+=9+hl;
    }
    put16be(e,0x000a);put16be(e+2,6);put16be(e+4,4);put16be(e+6,0x0017);put16be(e+8,0x0018);e+=10;
    put16be(e,0x000b);put16be(e+2,2);e[4]=1;e[5]=0;e+=6;
    put16be(e,0x000d);put16be(e+2,12);put16be(e+4,10);put16be(e+6,0x0401);put16be(e+8,0x0501);put16be(e+10,0x0601);put16be(e+12,0x0403);put16be(e+14,0x0503);e+=16;
    put16be(e,0xff01);put16be(e+2,1);e[4]=0;e+=5;
    put16be(extlen,(unsigned int)(e-extlen-2));n=(unsigned long)(e-ch);
    if(!tls_hs_send(c,1,ch,n))return tls_fail("cannot send ClientHello");
    /* server flight */
    for(;;){
        if(!tls_hs_next(c,&mt,&b,&bl))goto bad;
        if(mt==2){
            unsigned long sid;
            if(bl<38||b[0]!=3||b[1]!=3){tls_fail("server did not negotiate TLS 1.2");goto bad;}
            memcpy(sr,b+2,32);sid=b[34];
            if(bl<35+sid+3||sid>32){tls_fail("bad ServerHello");goto bad;}
            suite=(int)get16be(b+35+sid);
            if((suite!=0xc02f&&suite!=0xc02b)||b[37+sid]!=0){tls_fail("server chose an unsupported cipher suite or compression");goto bad;}
        }else if(mt==11){
            if(!suite||certmsg.len){tls_fail("unexpected Certificate");goto bad;}
            if(!buf_append(&certmsg,b,bl))goto bad;
        }else if(mt==12){
            if(!suite||have_ske){tls_fail("unexpected ServerKeyExchange");goto bad;}
            if(!buf_append(&ske,b,bl))goto bad;
            have_ske=1;
        }else if(mt==13){certreq=1;}
        else if(mt==14)break;
        else{tls_fail("unexpected handshake message");goto bad;}
    }
    if(!certmsg.len||!have_ske){tls_fail("incomplete server handshake");goto bad;}
    /* certificates */
    {
        const U8 *p=certmsg.data,*end=certmsg.data+certmsg.len;unsigned long total;
        if(certmsg.len<3){tls_fail("bad Certificate message");goto bad;}
        total=((unsigned long)p[0]<<16)|((unsigned long)p[1]<<8)|p[2];p+=3;
        if(total!=(unsigned long)(end-p)){tls_fail("bad Certificate message");goto bad;}
        while(p<end&&nchain<12){
            unsigned long cl;
            if(end-p<3){tls_fail("bad Certificate message");goto bad;}
            cl=((unsigned long)p[0]<<16)|((unsigned long)p[1]<<8)|p[2];p+=3;
            if(cl>(unsigned long)(end-p)){tls_fail("bad Certificate message");goto bad;}
            if(!cert_parse(&chain[nchain],p,cl)){if(nchain==0){tls_fail("cannot parse server certificate (unsupported key type?)");goto bad;}}
            else ++nchain;
            p+=cl;
        }
        if(nchain==0){tls_fail("server sent no certificate");goto bad;}
    }
    if(noverify)fprintf(stderr,"git: warning: TLS certificate verification disabled (GIT_SSL_NO_VERIFY)\n");
    else if(!tls_check_chain(chain,nchain,host))goto bad;
    /* ServerKeyExchange: ECDHE parameters signed by the certificate key */
    {
        const U8 *s=ske.data;unsigned long sl=ske.len,siglen,ptl,plen;int hashalg,sigalg,curveid;U8 *signed_data;int ok;
        if(sl<5||s[0]!=3){tls_fail("unsupported ECDHE parameters");goto bad;}
        if(get16be(s+1)==0x0017)curveid=0;else if(get16be(s+1)==0x0018)curveid=1;
        else{tls_fail("unsupported ECDHE curve (only secp256r1 and secp384r1 are supported)");goto bad;}
        ptl=s[3];plen=4+ptl;
        if(ptl!=(curveid?97UL:65UL)||sl<plen+4||s[4]!=4){tls_fail("bad ECDHE public key");goto bad;}
        hashalg=s[plen];sigalg=s[plen+1];siglen=get16be(s+plen+2);
        if(sl!=plen+4+siglen){tls_fail("bad ServerKeyExchange");goto bad;}
        signed_data=(U8*)malloc(64+plen);if(!signed_data)goto bad;
        memcpy(signed_data,cr,32);memcpy(signed_data+32,sr,32);memcpy(signed_data+64,s,plen);
        if(sigalg==1&&chain[0].keytype==1)ok=rsa_verify(chain[0].rn,chain[0].rnl,chain[0].re,chain[0].rel,hashalg,signed_data,64+plen,s+plen+4,siglen);
        else if(sigalg==3&&chain[0].keytype==2)ok=ecdsa_verify(chain[0].curve,chain[0].ecpt,chain[0].ecptl,hashalg,signed_data,64+plen,s+plen+4,siglen);
        else ok=-1;
        free(signed_data);
        if(ok<0){tls_fail("server used an unsupported signature algorithm");goto bad;}
        if(!ok){tls_fail("ServerKeyExchange signature is invalid");goto bad;}
        pklen=1+2*(curveid?48UL:32UL);
        if(!ecdh(curveid,s+4,ptl,mypub,pms)){tls_fail("key exchange failed");goto bad;}
        pmslen=curveid?48UL:32UL;
    }
    /* key schedule */
    memcpy(seed,cr,32);memcpy(seed+32,sr,32);
    tls_prf(pms,pmslen,"master secret",seed,64,master,48);
    memcpy(seed,sr,32);memcpy(seed+32,cr,32);
    tls_prf(master,48,"key expansion",seed,64,kb,40);
    gcm_init(&c->cw,kb);gcm_init(&c->sw,kb+16);memcpy(c->civ,kb+32,4);memcpy(c->siv,kb+36,4);
    memset(pms,0,sizeof(pms));memset(kb,0,sizeof(kb));
    /* client flight */
    if(certreq&&!tls_hs_send(c,11,(const U8*)"\0\0\0",3))goto bad;
    {U8 cke[98];cke[0]=(U8)pklen;memcpy(cke+1,mypub,pklen);if(!tls_hs_send(c,16,cke,pklen+1))goto bad;}
    {U8 one=1;if(!tls_write_record(c,20,&one,1))goto bad;}
    c->enc=1;c->cseq[0]=c->cseq[1]=0;
    tls_transcript_hash(c,hash);tls_prf(master,48,"client finished",hash,32,vd,12);
    if(!tls_hs_send(c,20,vd,12))goto bad;
    /* server Finished */
    {
        int rt,r;
        r=tls_read_record(c,&rt);
        if(r<=0||rt!=20||c->reclen!=1||c->rec[0]!=1){tls_fail("expected ChangeCipherSpec");goto bad;}
        if(c->hspos!=c->hs.len){tls_fail("unexpected data before ChangeCipherSpec");goto bad;}
        c->dec=1;c->sseq[0]=c->sseq[1]=0;
        tls_transcript_hash(c,hash);tls_prf(master,48,"server finished",hash,32,vd,12);
        if(!tls_hs_next(c,&mt,&b,&bl))goto bad;
        if(mt!=20||bl!=12||!ct_equal(b,vd,12)){tls_fail("server Finished verification failed");goto bad;}
    }
    (void)fin;(void)i;
    memset(master,0,sizeof(master));
    if(certmsg.data)free(certmsg.data);
    if(ske.data)free(ske.data);
    if(c->hs.data){free(c->hs.data);c->hs.data=NULL;c->hs.len=0;c->hspos=0;}
    if(c->tr.data){free(c->tr.data);c->tr.data=NULL;c->tr.len=0;}
    c->reclen=c->recpos=0;
    return 1;
bad:
    if(certmsg.data)free(certmsg.data);
    if(ske.data)free(ske.data);
    return 0;
}
static int tls_send(Conn *c,const U8 *p,unsigned long n)
{
    while(n){unsigned long k=n>16384?16384:n;if(!tls_write_record(c,23,p,k))return 0;p+=k;n-=k;}
    return 1;
}
/* returns bytes read, 0 at end of stream, -1 on error */
static int tls_recv(Conn *c,U8 *buf,int n)
{
    for(;;){
        if(c->recpos<c->reclen){unsigned long k=c->reclen-c->recpos;if(k>(unsigned long)n)k=(unsigned long)n;memcpy(buf,c->rec+c->recpos,k);c->recpos+=k;return (int)k;}
        if(c->eof)return 0;
        {
            int rt,r;
            c->reclen=c->recpos=0;
            r=tls_read_record(c,&rt);
            if(r==0){c->eof=1;return 0;}
            if(r<0)return -1;
            if(rt==23)continue;
            if(rt==21){c->eof=1;if(c->reclen>=2&&c->rec[1]==0){c->reclen=0;return 0;}c->reclen=0;return -1;}
            c->reclen=0;
            if(rt==22||rt==20)continue;
            return -1;
        }
    }
}

/* ------------------------------ HTTP(S) client --------------------------- */
typedef struct { char host[256]; char path[1200]; unsigned short port; int https; int has_auth; char user[128]; char pass[256]; } HttpURL;
typedef struct { int status; char location[1400]; int chunked; int has_cl; unsigned long content_length; } HttpResp;
static char g_redir_from[1400],g_redir_to[1400];

static int url_decode(const char *s,size_t n,char *out,size_t cap)
{
    size_t i,o=0;
    for(i=0;i<n;++i){
        int c=(unsigned char)s[i];
        if(c=='%'&&i+2<n+0&&isxdigit((unsigned char)s[i+1])&&isxdigit((unsigned char)s[i+2])){
            char hx[3];hx[0]=s[i+1];hx[1]=s[i+2];hx[2]=0;c=(int)strtol(hx,NULL,16);i+=2;
        }
        if(o+1>=cap)return 0;
        out[o++]=(char)c;
    }
    out[o]=0;return 1;
}
static int parse_http_url(const char *url,HttpURL *u)
{
    const char *p,*slash,*auth_end,*at=NULL,*q,*hp,*colon;size_t n;
    memset(u,0,sizeof(*u));
    if(STRNICMP(url,"https://",8)==0){u->https=1;u->port=443;p=url+8;}
    else if(STRNICMP(url,"http://",7)==0){u->port=80;p=url+7;}
    else{fprintf(stderr,"git: unsupported URL '%s' (only http:// and https:// remotes are supported)\n",url);return 0;}
    slash=strchr(p,'/');auth_end=slash?slash:p+strlen(p);
    for(q=p;q<auth_end;++q)if(*q=='@')at=q;
    hp=p;
    if(at){
        const char *uc=NULL;
        for(q=p;q<at;++q)if(*q==':'){uc=q;break;}
        if(!url_decode(p,uc?(size_t)(uc-p):(size_t)(at-p),u->user,sizeof(u->user)))return 0;
        if(uc&&!url_decode(uc+1,(size_t)(at-uc-1),u->pass,sizeof(u->pass)))return 0;
        u->has_auth=1;hp=at+1;
    }
    if(hp<auth_end&&*hp=='['){fprintf(stderr,"git: IPv6 literal addresses are not supported\n");return 0;}
    colon=NULL;for(q=hp;q<auth_end;++q)if(*q==':'){colon=q;break;}
    n=colon?(size_t)(colon-hp):(size_t)(auth_end-hp);
    if(n==0||n>=sizeof(u->host))return 0;
    memcpy(u->host,hp,n);u->host[n]=0;
    if(colon){long port=strtol(colon+1,NULL,10);if(port<1||port>65535)return 0;u->port=(unsigned short)port;}
    if(slash){if(strlen(slash)>=sizeof(u->path))return 0;strcpy(u->path,slash);}else strcpy(u->path,"/");
    return 1;
}
/* Build "scheme://host[:port]path" without credentials. */
static void url_format(const HttpURL *u,char *out,size_t cap)
{
    char hostport[300];
    if(u->port==(u->https?443:80))strcpy(hostport,u->host);else sprintf(hostport,"%s:%u",u->host,(unsigned int)u->port);
    if(strlen(u->https?"https://":"http://")+strlen(hostport)+strlen(u->path)+1>cap){out[0]=0;return;}
    sprintf(out,"%s%s%s",u->https?"https://":"http://",hostport,u->path);
}
/* Copy of a URL with any password hidden, for display. */
static void url_mask(const char *url,char *out,size_t cap)
{
    const char *p=strstr(url,"://"),*slash,*at,*colon;size_t head;
    if(strlen(url)+4>=cap){strncpy(out,url,cap-1);out[cap-1]=0;return;}
    strcpy(out,url);if(!p)return;
    p+=3;slash=strchr(p,'/');at=strchr(p,'@');
    if(!at||(slash&&at>slash))return;
    colon=memchr(p,':',(size_t)(at-p));
    if(!colon)return;
    head=(size_t)(colon-url);memcpy(out,url,head);strcpy(out+head,":***");strcat(out,at);
}

static Conn *conn_open(const HttpURL *u)
{
    Conn *c;struct hostent *he;struct sockaddr_in sa;
    he=gethostbyname(u->host);
    if(!he||!he->h_addr_list||!he->h_addr_list[0]){fprintf(stderr,"git: cannot resolve %s\n",u->host);return NULL;}
    c=(Conn*)calloc(1,sizeof(Conn));if(!c)return NULL;
    c->s=socket(AF_INET,SOCK_STREAM,0);
    if(c->s==(net_socket_t)-1){fprintf(stderr,"git: socket creation failed\n");free(c);return NULL;}
    memset(&sa,0,sizeof(sa));sa.sin_family=AF_INET;sa.sin_port=htons(u->port);memcpy(&sa.sin_addr,he->h_addr_list[0],(size_t)he->h_length);
    if(connect(c->s,(struct sockaddr*)&sa,sizeof(sa))!=0){fprintf(stderr,"git: cannot connect to %s:%u\n",u->host,(unsigned int)u->port);NET_CLOSE(c->s);free(c);return NULL;}
    {
#if defined(__WATCOMC__)
        int tmo=120000;setsockopt(c->s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&tmo,sizeof(tmo));
#else
        struct timeval tv;tv.tv_sec=120;tv.tv_usec=0;setsockopt(c->s,SOL_SOCKET,SO_RCVTIMEO,(const void*)&tv,sizeof(tv));
#endif
    }
    if(u->https){
        c->tls=1;
        if(!tls_connect(c,u->host)){NET_CLOSE(c->s);if(c->hs.data)free(c->hs.data);if(c->tr.data)free(c->tr.data);free(c);return NULL;}
    }
    return c;
}
static int conn_send(Conn *c,const U8 *p,unsigned long n){return c->tls?tls_send(c,p,n):net_send_all(c->s,p,n);}
static int conn_recv(Conn *c,U8 *p,int n){return c->tls?tls_recv(c,p,n):recv(c->s,(char*)p,n,0);}
static void conn_close(Conn *c)
{
    if(c->tls&&c->enc){U8 al[2];al[0]=1;al[1]=0;tls_write_record(c,21,al,2);}
    NET_CLOSE(c->s);
    if(c->hs.data)free(c->hs.data);
    if(c->tr.data)free(c->tr.data);
    memset(c,0,sizeof(*c));free(c);
}
static int decode_chunked(const U8 *src,unsigned long n,Buffer *out)
{
    unsigned long p=0;out->data=NULL;out->len=0;
    while(p<n){unsigned long z=0;int digits=0;while(p<n&&src[p]!='\r'&&src[p]!='\n'){int c=src[p++];if(c==';'){while(p<n&&src[p]!='\n')++p;break;}if(isxdigit((unsigned char)c)){z=(z<<4)+(unsigned long)(isdigit((unsigned char)c)?c-'0':tolower((unsigned char)c)-'a'+10);++digits;}else if(c!=' '&&c!='\t')goto bad;}while(p<n&&src[p]!='\n')++p;if(p<n)++p;if(!digits)goto bad;if(z==0)return 1;if(z>n-p||!buf_append(out,src+p,z))goto bad;p+=z;if(p+1<n&&src[p]=='\r'&&src[p+1]=='\n')p+=2;else if(p<n&&src[p]=='\n')++p;}
bad:if(out->data)free(out->data);out->data=NULL;out->len=0;return 0;
}
/* One request/response exchange. Returns 1 when a complete HTTP response was parsed. */
static int http_once(const HttpURL *u,const char *method,const char *ctype,const Buffer *body,Buffer *response,HttpResp *info)
{
    Conn *c;char *request,hosthdr[300],accept[96];int hn;U8 *raw=NULL,*blk;unsigned long rawlen=0,rawcap=0,i,bodyoff;int ok=0;
    memset(info,0,sizeof(*info));response->data=NULL;response->len=0;
    c=conn_open(u);if(!c)return 0;
    request=(char*)malloc(4096+400);blk=(U8*)malloc(16384);
    if(!request||!blk)goto done;
    if(u->port==(u->https?443:80))strcpy(hosthdr,u->host);else sprintf(hosthdr,"%s:%u",u->host,(unsigned int)u->port);
    strcpy(accept,"*/*");
    if(ctype&&strstr(ctype,"-request")){size_t k=(size_t)(strstr(ctype,"-request")-ctype);memcpy(accept,ctype,k);strcpy(accept+k,"-result");}
    hn=sprintf(request,"%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: git/1.0 (TinyVCS98)\r\nAccept: %s\r\nPragma: no-cache\r\nConnection: close\r\n",method,u->path,hosthdr,accept);
    if(u->has_auth){
        char cred[400],enc[600];int cl=sprintf(cred,"%s:%s",u->user,u->pass);
        b64_encode((const U8*)cred,(unsigned long)cl,enc);hn+=sprintf(request+hn,"Authorization: Basic %s\r\n",enc);
        memset(cred,0,sizeof(cred));
    }
    if(ctype)hn+=sprintf(request+hn,"Content-Type: %s\r\n",ctype);
    hn+=sprintf(request+hn,"Content-Length: %lu\r\n\r\n",body?body->len:0UL);
    if(!conn_send(c,(U8*)request,(unsigned long)hn)||(body&&body->len&&!conn_send(c,body->data,body->len))){fprintf(stderr,"git: send failed\n");goto done;}
    for(;;){
        int got=conn_recv(c,blk,16384);
        if(got<0){fprintf(stderr,"git: network read error\n");goto done;}
        if(got==0)break;
        if(rawlen+(unsigned long)got>MAX_OBJECT+65536UL){fprintf(stderr,"git: HTTP response too large\n");goto done;}
        if(rawlen+(unsigned long)got+1>rawcap){
            unsigned long nc=rawcap?rawcap*2:65536UL;U8 *nr;
            while(nc<rawlen+(unsigned long)got+1)nc*=2;
            nr=(U8*)realloc(raw,nc);if(!nr){fprintf(stderr,"git: out of memory\n");goto done;}raw=nr;rawcap=nc;
        }
        memcpy(raw+rawlen,blk,(size_t)got);rawlen+=(unsigned long)got;
    }
    if(!raw||rawlen<12){fprintf(stderr,"git: empty or truncated HTTP response\n");goto done;}
    for(i=0;i+3<rawlen;++i)if(raw[i]=='\r'&&raw[i+1]=='\n'&&raw[i+2]=='\r'&&raw[i+3]=='\n')break;
    if(i+3>=rawlen){fprintf(stderr,"git: malformed HTTP response\n");goto done;}
    bodyoff=i+4;
    {
        char *hdr=(char*)malloc(i+3);char *line,*e;
        if(!hdr)goto done;
        memcpy(hdr,raw,i);hdr[i]='\r';hdr[i+1]='\n';hdr[i+2]=0;
        if(sscanf(hdr,"HTTP/%*s %d",&info->status)!=1){free(hdr);fprintf(stderr,"git: malformed HTTP status line\n");goto done;}
        line=strstr(hdr,"\r\n");line=line?line+2:hdr+strlen(hdr);
        while(*line&&(e=strstr(line,"\r\n"))!=NULL){
            *e=0;
            if(STRNICMP(line,"Transfer-Encoding:",18)==0&&strstr(line+18,"chunked"))info->chunked=1;
            else if(STRNICMP(line,"Content-Length:",15)==0){info->has_cl=1;info->content_length=strtoul(line+15,NULL,10);}
            else if(STRNICMP(line,"Location:",9)==0){const char *v=line+9;while(*v==' '||*v=='\t')++v;if(strlen(v)<sizeof(info->location))strcpy(info->location,v);}
            line=e+2;
        }
        free(hdr);
    }
    if(info->chunked){if(!decode_chunked(raw+bodyoff,rawlen-bodyoff,response)){fprintf(stderr,"git: malformed chunked HTTP body\n");goto done;}}
    else{
        unsigned long bl=rawlen-bodyoff;
        if(info->has_cl&&bl<info->content_length){fprintf(stderr,"git: HTTP response was truncated\n");goto done;}
        if(info->has_cl&&bl>info->content_length)bl=info->content_length;
        if(bl&&!buf_append(response,raw+bodyoff,bl)){fprintf(stderr,"git: out of memory\n");goto done;}
    }
    ok=1;
done:
    if(raw)free(raw);
    if(request)free(request);
    if(blk)free(blk);
    conn_close(c);
    return ok;
}
static int http_request_impl(const char *base,const char *suffix,const char *method,const char *ctype,const Buffer *body,Buffer *response)
{
    HttpURL orig,u,nu;Buffer res;HttpResp info;int redirects;size_t a,b;
    if(!parse_http_url(base,&orig))return 0;
    if(!orig.has_auth){   /* no user:token@ in the URL: use user.token from the global config, https only */
        char tok[256],nm[128];
        if(global_get("user.token",tok,sizeof(tok))&&tok[0]){
            if(!orig.https){fprintf(stderr,"git: user.token is not sent over plain http://\n");}
            else{
                if(!global_get("user.name",nm,sizeof(nm))||!nm[0])strcpy(nm,"git");
                strcpy(orig.user,nm);strcpy(orig.pass,tok);orig.has_auth=1;
            }
        }
        memset(tok,0,sizeof(tok));
    }
    u=orig;
    if(g_redir_to[0]&&!strcmp(base,g_redir_from)){
        HttpURL r;
        if(parse_http_url(g_redir_to,&r)){if(!(r.https==orig.https&&r.port==orig.port&&!STRICMP(r.host,orig.host))){r.has_auth=0;}else{r.has_auth=orig.has_auth;strcpy(r.user,orig.user);strcpy(r.pass,orig.pass);}u=r;}
    }
    if(suffix[0]!='/')return 0;
    a=strlen(u.path);b=strlen(suffix);while(a>1&&u.path[a-1]=='/')--a;
    if(a+b>=sizeof(u.path))return 0;
    memcpy(u.path+a,suffix,b+1);
    for(redirects=0;;++redirects){
        if(!http_once(&u,method,ctype,body,&res,&info))return 0;
        if((info.status==301||info.status==302||info.status==303||info.status==307||info.status==308)&&info.location[0]){
            if(res.data)free(res.data);
            if(redirects>=5){fprintf(stderr,"git: too many HTTP redirects\n");return 0;}
            if(!strcmp(method,"POST")&&info.status!=307&&info.status!=308){fprintf(stderr,"git: unexpected redirect (%d) on POST\n",info.status);return 0;}
            nu=u;
            if(STRNICMP(info.location,"http://",7)==0||STRNICMP(info.location,"https://",8)==0){
                if(!parse_http_url(info.location,&nu))return 0;
                if(nu.https==u.https&&nu.port==u.port&&!STRICMP(nu.host,u.host)){nu.has_auth=u.has_auth;strcpy(nu.user,u.user);strcpy(nu.pass,u.pass);}
                else nu.has_auth=0;
                if(u.https&&!nu.https){fprintf(stderr,"git: refusing redirect from https to http\n");return 0;}
            }else if(info.location[0]=='/'){
                if(strlen(info.location)>=sizeof(nu.path))return 0;
                strcpy(nu.path,info.location);
            }else{fprintf(stderr,"git: unsupported relative redirect\n");return 0;}
            if(!strcmp(method,"GET")){
                size_t pl=strlen(nu.path),sl=strlen(suffix);
                if(pl>sl&&!strcmp(nu.path+pl-sl,suffix)){
                    HttpURL nb=nu;char tmp[1400];nb.path[pl-sl]=0;if(!nb.path[0])strcpy(nb.path,"/");
                    url_format(&nb,tmp,sizeof(tmp));
                    if(tmp[0]&&strlen(base)<sizeof(g_redir_from)){strcpy(g_redir_from,base);strcpy(g_redir_to,tmp);}
                }
            }
            u=nu;continue;
        }
        break;
    }
    if(info.status<200||info.status>=300){
        fprintf(stderr,"git: HTTP request failed with status %d\n",info.status);
        if(info.status==401||info.status==403)fprintf(stderr,"git: authentication is required; use a remote URL like https://user:token@host/path.git\n");
        if(res.len){unsigned long show=res.len>300?300:res.len;fprintf(stderr,"%.*s\n",(int)show,res.data);}
        if(res.data)free(res.data);
        return 0;
    }
    *response=res;
    return 1;
}
static int http_request(const char *base,const char *suffix,const char *method,const char *ctype,const Buffer *body,Buffer *response)
{
    int ok;
#if defined(__WATCOMC__)
    WSADATA wd;if(WSAStartup(MAKEWORD(1,1),&wd)!=0){fprintf(stderr,"git: Winsock initialization failed\n");return 0;}
#endif
    ok=http_request_impl(base,suffix,method,ctype,body,response);
#if defined(__WATCOMC__)
    WSACleanup();
#endif
    return ok;
}
static int pkt_read(const Buffer *b,unsigned long *pos,const U8 **data,unsigned long *len,int *special)
{
    unsigned int v=0,i;if(*pos+4>b->len)return 0;for(i=0;i<4;++i){int c=b->data[*pos+i];if(!isxdigit((unsigned char)c))return 0;v=(v<<4)|(unsigned int)(isdigit((unsigned char)c)?c-'0':tolower((unsigned char)c)-'a'+10);}*pos+=4;
    if(v==0){*special=1;*len=0;*data=NULL;return 1;}if(v==1){*special=2;*len=0;*data=NULL;return 1;}if(v==2){*special=3;*len=0;*data=NULL;return 1;}if(v<4||*pos+(v-4)>b->len)return 0;*special=0;*len=v-4;*data=b->data+*pos;*pos+=v-4;return 1;
}
static int pkt_append(Buffer *b,const void *p,unsigned long n)
{
    char h[5];if(n+4>65520UL)return 0;sprintf(h,"%04lx",n+4);return buf_append(b,h,4)&&buf_append(b,p,n);
}
static int parse_advertisement(Buffer *b,RemoteRef *refs,unsigned int *nr,char *symref,size_t symcap)
{
    unsigned long p=0,n;const U8 *d;int sp;*nr=0;symref[0]=0;
    while(p<b->len){if(!pkt_read(b,&p,&d,&n,&sp)){return 0;}if(sp)continue;if(n>=9&&!memcmp(d,"version 2",9)){fprintf(stderr,"git: remote requires Git protocol v2; this client implements protocol v0\n");return 0;}if(n>=19&&!memcmp(d,"# service=git-",14))continue;
        {unsigned long nul=0,lineend=n;char hex[41],name[MAX_PATH_GIT];while(nul<n&&d[nul]!=0)++nul;while(lineend&& (d[lineend-1]=='\n'||d[lineend-1]=='\r'))--lineend;if(nul>=41&&d[40]==' '){memcpy(hex,d,40);hex[40]=0;if(hex_to_oid(hex,refs[*nr].oid)){unsigned long namelen=0;while(41+namelen<lineend&&d[41+namelen]!=0)++namelen;if(namelen>=sizeof(name))return 0;memcpy(name,d+41,namelen);name[namelen]=0;if(!strncmp(name,"refs/heads/",11)||!strncmp(name,"refs/tags/",10)){if(*nr<MAX_REMOTE_REFS){strcpy(refs[*nr].name,name);++*nr;}}}}
            if(nul<n){unsigned long q=nul+1;while(q<lineend){unsigned long e=q;while(e<lineend&&d[e]!=' ')++e;if(e-q>11&&!memcmp(d+q,"symref=HEAD:",12)){size_t z=e-q-12;if(z<symcap){memcpy(symref,d+q+12,z);symref[z]=0;}}q=e+1;}}
        }
    }
    return 1;
}
static int get_advert(const char *url,const char *service,RemoteRef *refs,unsigned int *nr,char *symref,size_t symcap)
{
    char suffix[128];Buffer b;int ok;sprintf(suffix,"/info/refs?service=git-%s",service);b.data=NULL;b.len=0;ok=http_request(url,suffix,"GET",NULL,NULL,&b);if(!ok)return 0;ok=parse_advertisement(&b,refs,nr,symref,symcap);free(b.data);return ok;
}
static int receive_upload_pack(const char *url,RemoteRef *refs,unsigned int nr,Buffer *pack)
{
    Buffer req,res;unsigned int i;char line[256],hex[41];unsigned long p,n;const U8 *d;int sp,first=1,found=0;req.data=NULL;req.len=0;res.data=NULL;res.len=0;pack->data=NULL;pack->len=0;
    for(i=0;i<nr;++i){if(strncmp(refs[i].name,"refs/heads/",11)!=0&&strncmp(refs[i].name,"refs/tags/",10)!=0)continue;oid_to_hex(refs[i].oid,hex);if(first){sprintf(line,"want %s\n",hex);first=0;}else sprintf(line,"want %s\n",hex);if(!pkt_append(&req,line,(unsigned long)strlen(line)))goto bad;}
    if(first){fprintf(stderr,"git: remote has no branch refs to fetch\n");goto bad;}
    if(!buf_append(&req,"0000",4)||!pkt_append(&req,"done\n",5))goto bad;
    if(!http_request(url,"/git-upload-pack","POST","application/x-git-upload-pack-request",&req,&res))goto bad;
    p=0;
    while(p<res.len){if(p+4<=res.len&&!memcmp(res.data+p,"PACK",4)){if(!buf_append(pack,res.data+p,res.len-p))goto bad;found=1;break;}
        if(!pkt_read(&res,&p,&d,&n,&sp))goto bad;
        if(sp){if(sp==1&&p+4<=res.len&&!memcmp(res.data+p,"PACK",4)){if(!buf_append(pack,res.data+p,res.len-p))goto bad;found=1;break;}continue;}
        if(n>=1&&d[0]==1){if(!buf_append(pack,d+1,n-1))goto bad;found=1;}else if(n>=1&&d[0]==3){fprintf(stderr,"git: upload-pack fatal: %.*s\n",(int)(n-1),d+1);goto bad;}
    }
    free(req.data);free(res.data);if(!found||pack->len<32||memcmp(pack->data,"PACK",4)){fprintf(stderr,"git: remote response did not contain a packfile\n");return 0;}return 1;
bad:if(req.data)free(req.data);if(res.data)free(res.data);if(pack->data)free(pack->data);pack->data=NULL;pack->len=0;return 0;
}
static int install_received_pack(Buffer *pack)
{
    char path[MAX_PATH_GIT];U8 check[20];unsigned long p,count,i,*pending=NULL,pn=0,pcap=0;int pass,progress;unsigned int ver;Buffer z;
    if(pack->len<32||memcmp(pack->data,"PACK",4)!=0)return 0;
    ver=get32be(pack->data+4);count=get32be(pack->data+8);if((ver!=2&&ver!=3)||count>MAX_ENTRIES)return 0;sha1_data(pack->data,pack->len-20,check);if(memcmp(check,pack->data+pack->len-20,20)){fprintf(stderr,"git: pack checksum mismatch\n");return 0;}
    if(!path_join(path,sizeof(path),gitdir,"tiny-receive.pack")||!write_file(path,pack->data,pack->len))return 0;
    p=12;
    for(i=0;i<count;++i){unsigned long entryoff=p,size=0,shift=4,used;U8 c;unsigned int ot;if(p>=pack->len-20)goto bad;c=pack->data[p++];ot=(c>>4)&7;size=c&15;while(c&0x80){if(p>=pack->len-20||shift>28)goto bad;c=pack->data[p++];size|=(unsigned long)(c&0x7f)<<shift;shift+=7;}
        if(ot==6){if(p>=pack->len-20)goto bad;c=pack->data[p++];while(c&0x80){if(p>=pack->len-20)goto bad;c=pack->data[p++];}}
        else if(ot==7){if(p+20>pack->len-20)goto bad;p+=20;}
        if(!zlib_inflate_partial(pack->data+p,pack->len-20-p,&z,&used))goto bad;
        p+=used;if(z.len!=size){free(z.data);goto bad;}
        if(ot>=1&&ot<=4){const char *t=ot==1?"commit":ot==2?"tree":ot==3?"blob":"tag";if(!store_object(t,z.data,z.len,check)){free(z.data);goto bad;}}
        else if(ot==6||ot==7){if(pn==pcap){unsigned long nc=pcap?pcap*2:32;unsigned long *q=(unsigned long*)realloc(pending,nc*sizeof(unsigned long));if(!q){free(z.data);goto bad;}pending=q;pcap=nc;}pending[pn++]=entryoff;}
        else {free(z.data);goto bad;}free(z.data);
    }
    if(p!=pack->len-20)goto bad;
    for(pass=0;pass<MAX_DEPTH+2&&pn;++pass){unsigned long j,w=0;progress=0;for(j=0;j<pn;++j){Buffer b;char t[16];if(read_pack_entry(path,pending[j],0,&b,t)){if(!store_object(t,b.data,b.len,check)){free(b.data);goto bad;}free(b.data);progress=1;}else pending[w++]=pending[j];}pn=w;if(!progress)break;}
    if(pn){fprintf(stderr,"git: unable to resolve %lu delta object(s) in received pack\n",pn);goto bad;}
    remove_file(path);if(pending)free(pending);return 1;
bad:remove_file(path);if(pending)free(pending);return 0;
}
static int write_ref(const char *ref,const U8 oid[20])
{
    char path[MAX_PATH_GIT],hex[42];if(!normalize_path(ref,path,sizeof(path)))return 0;{char full[MAX_PATH_GIT];if(!path_join(full,sizeof(full),gitdir,path))return 0;oid_to_hex(oid,hex);strcat(hex,"\n");return write_file(full,(U8*)hex,41);}
}
static int read_remote_url(const char *remote,char *url,size_t cap)
{
    Buffer b;unsigned long p=0;if(!require_repository()||!read_file(".git/config",&b))return 0;
    while(p<b.len){unsigned long e=p;while(e<b.len&&b.data[e]!='\n')++e;if(e-p>10&&b.data[p]=='['&&b.data[e-1]==']'){
            char section[256];size_t n=(size_t)(e-p-2);if(n<sizeof(section)){memcpy(section,b.data+p+1,n);section[n]=0;{char want[300];sprintf(want,"remote \"%s\"",remote);if(!strcmp(section,want)){p=e+1;while(p<b.len&&b.data[p]!='['){unsigned long q0=p;e=p;while(e<b.len&&b.data[e]!='\n')++e;while(q0<e&&(b.data[q0]==' '||b.data[q0]=='\t'))++q0;if(e-q0>=5&&!memcmp(b.data+q0,"url",3)){unsigned long q=q0+3;while(q<e&&(b.data[q]==' '||b.data[q]=='\t'||b.data[q]=='='))++q;while(e>q&&(b.data[e-1]=='\r'||b.data[e-1]==' '||b.data[e-1]=='\t'))--e;if(e-q<cap){memcpy(url,b.data+q,e-q);url[e-q]=0;free(b.data);return 1;}}p=e+1;}break;}}}
        }p=e+1;}
    free(b.data);fprintf(stderr,"git: remote '%s' is not configured\n",remote);return 0;
}
static int remote_add(const char *name,const char *url)
{
    Buffer b;char section[1600];HttpURL parsed;int n;
    if(!require_repository()||!name||!*name||strlen(name)>64||strpbrk(name," /\\\t")||strstr(name,"..")||name[0]=='.'||!parse_http_url(url,&parsed))return 0;
    if(!read_file(".git/config",&b)){b.data=NULL;b.len=0;}
    n=sprintf(section,"\n[remote \"%s\"]\n\turl = %s\n\tfetch = +refs/heads/*:refs/remotes/%s/*\n",name,url,name);
    if(!buf_append(&b,section,(unsigned long)n)||!write_file(".git/config",b.data,b.len)){if(b.data)free(b.data);return 0;}
    free(b.data);printf("Added remote '%s'\n",name);return 1;
}
static int cmd_fetch(const char *remote)
{
    char url[1200],sym[MAX_PATH_GIT];RemoteRef *refs;unsigned int nr=0,i;Buffer pack;int need=0;
    refs=(RemoteRef*)calloc(MAX_REMOTE_REFS,sizeof(RemoteRef));if(!refs)return 1;
    if(!read_remote_url(remote,url,sizeof(url))||!get_advert(url,"upload-pack",refs,&nr,sym,sizeof(sym))){free(refs);return 1;}
    for(i=0;i<nr;++i)if(!strncmp(refs[i].name,"refs/heads/",11)||!strncmp(refs[i].name,"refs/tags/",10))need=1;
    if(!need){printf("No remote branches or tags found.\n");free(refs);return 0;}
    if(!receive_upload_pack(url,refs,nr,&pack)){free(refs);return 1;}
    if(!install_received_pack(&pack)){free(pack.data);free(refs);fprintf(stderr,"git: failed to install received pack\n");return 1;}free(pack.data);
    for(i=0;i<nr;++i){char dst[MAX_PATH_GIT];if(!strncmp(refs[i].name,"refs/heads/",11)){
            if(strlen(remote)+strlen(refs[i].name+11)+20>=sizeof(dst)){free(refs);fprintf(stderr,"git: remote ref path too long\n");return 1;}
            strcpy(dst,"refs/remotes/");strcat(dst,remote);strcat(dst,"/");strcat(dst,refs[i].name+11);
        }else if(!strncmp(refs[i].name,"refs/tags/",10))strcpy(dst,refs[i].name);else continue;
        if(!write_ref(dst,refs[i].oid)){fprintf(stderr,"git: cannot update %s\n",dst);free(refs);return 1;}printf(" * [fetched] %s\n",refs[i].name);}
    free(refs);return 0;
}
static int append_pack_object(Buffer *pack,const PackObject *o)
{
    unsigned long sz=o->body.len;U8 h[8];unsigned int n=0;U8 c;Buffer z;unsigned int t=!strcmp(o->type,"commit")?1:!strcmp(o->type,"tree")?2:!strcmp(o->type,"blob")?3:4;
    c=(U8)((t<<4)|(sz&15));sz>>=4;if(sz)c|=0x80;h[n++]=c;while(sz){c=(U8)(sz&0x7f);sz>>=7;if(sz)c|=0x80;h[n++]=c;}if(!buf_append(pack,h,n)||!zlib_deflate_stored(o->body.data,o->body.len,&z))return 0;if(!buf_append(pack,z.data,z.len)){free(z.data);return 0;}free(z.data);return 1;
}
static int pack_has_oid(PackObject *v,unsigned long n,const U8 oid[20]){unsigned long i;for(i=0;i<n;++i)if(!memcmp(v[i].oid,oid,20))return 1;return 0;}
static int collect_pack_object(PackObject **pv,unsigned long *pn,unsigned long *pc,const U8 oid[20],int depth)
{
    PackObject *v;Buffer b;char type[16];unsigned long p;if(depth>128||*pn>=MAX_ENTRIES||pack_has_oid(*pv,*pn,oid))return depth<=128;
    if(!read_object(oid,&b,type))return 0;
    if(*pn==*pc){unsigned long nc=*pc?*pc*2:64;v=(PackObject*)realloc(*pv,nc*sizeof(PackObject));if(!v){free(b.data);return 0;}*pv=v;*pc=nc;}
    v=&(*pv)[(*pn)++];memcpy(v->oid,oid,20);strcpy(v->type,type);v->body=b;
    if(!strcmp(type,"commit")){
        U8 tree[20],parent[20];if(!parse_commit_tree(b.data,b.len,tree)||!collect_pack_object(pv,pn,pc,tree,depth+1))return 0;
        p=0;while(p<b.len){unsigned long e=p;while(e<b.len&&b.data[e]!='\n')++e;if(e-p>=47&&!memcmp(b.data+p,"parent ",7)){char h[41];memcpy(h,b.data+p+7,40);h[40]=0;if(hex_to_oid(h,parent)&&!collect_pack_object(pv,pn,pc,parent,depth+1))return 0;}p=e+1;}
    }else if(!strcmp(type,"tree")){
        p=0;while(p<b.len){unsigned long e=p;U8 child[20];while(e<b.len&&b.data[e]!=' ')++e;if(e>=b.len)return 0;p=e+1;while(p<b.len&&b.data[p])++p;if(p+21>b.len)return 0;++p;memcpy(child,b.data+p,20);p+=20;if(!collect_pack_object(pv,pn,pc,child,depth+1))return 0;}
    }else if(!strcmp(type,"tag")){
        p=0;while(p<b.len){unsigned long e=p;while(e<b.len&&b.data[e]!='\n')++e;if(e-p>47&&!memcmp(b.data+p,"object ",7)){char h[41];U8 child[20];memcpy(h,b.data+p+7,40);h[40]=0;if(hex_to_oid(h,child)&&!collect_pack_object(pv,pn,pc,child,depth+1))return 0;break;}p=e+1;}
    }
    return 1;
}
static int build_push_pack(const U8 head[20],Buffer *pack)
{
    PackObject *v=NULL;unsigned long n=0,cap=0,i;U8 hdr[12],sum[20];int ok=0;pack->data=NULL;pack->len=0;
    if(!collect_pack_object(&v,&n,&cap,head,0)||n==0)goto done;
    memcpy(hdr,"PACK",4);put32be(hdr+4,2);put32be(hdr+8,(U32)n);if(!buf_append(pack,hdr,12))goto done;
    for(i=0;i<n;++i)if(!append_pack_object(pack,&v[i]))goto done;
    sha1_data(pack->data,pack->len,sum);if(!buf_append(pack,sum,20))goto done;ok=1;
done:for(i=0;i<n;++i)if(v[i].body.data)free(v[i].body.data);if(v)free(v);if(!ok&&pack->data){free(pack->data);pack->data=NULL;pack->len=0;}return ok;
}
static int receive_push(const char *url,const char *ref,const U8 oldoid[20],const U8 newoid[20],Buffer *pack)
{
    Buffer req,res;char oldhex[41],newhex[41];unsigned long p,n;const U8 *d;int sp,ok=0;req.data=NULL;req.len=0;res.data=NULL;res.len=0;oid_to_hex(oldoid,oldhex);oid_to_hex(newoid,newhex);
    /* Append the NUL-separated command/capability packet explicitly. */
    {unsigned long x=(unsigned long)strlen(oldhex)+1+(unsigned long)strlen(newhex)+1+(unsigned long)strlen(ref);Buffer command;command.data=NULL;command.len=0;if(!buf_append(&command,oldhex,40)||!buf_byte(&command,' ')||!buf_append(&command,newhex,40)||!buf_byte(&command,' ')||!buf_append(&command,ref,(unsigned long)strlen(ref))||!buf_byte(&command,0)||!buf_append(&command,"report-status\n",14)||!pkt_append(&req,command.data,command.len)){if(command.data)free(command.data);goto done;}free(command.data);(void)x;}
    if(!buf_append(&req,"0000",4)||!buf_append(&req,pack->data,pack->len))goto done;
    if(!http_request(url,"/git-receive-pack","POST","application/x-git-receive-pack-request",&req,&res))goto done;
    
    {int unpack_ok=0,ref_ok=0;size_t ref_len=strlen(ref);p=0;while(p<res.len){if(!pkt_read(&res,&p,&d,&n,&sp)){if(p+4<=res.len&&!memcmp(res.data+p,"PACK",4))break;goto done;}if(sp)continue;if(n>=9&&!memcmp(d,"unpack ok",9)){unpack_ok=1;continue;}if(n>=3&&!memcmp(d,"ng ",3)){fprintf(stderr,"git: remote rejected push: %.*s\n",(int)n,d);ref_ok=0;}if(n>=3+ref_len&&!memcmp(d,"ok ",3)&&!memcmp(d+3,ref,ref_len)&&(n==3+ref_len||d[3+ref_len]=='\n'))ref_ok=1;}ok=unpack_ok&&ref_ok;}
    if(ok)printf("Push successful.\n");else fprintf(stderr,"git: remote did not confirm the update\n");
done:if(req.data)free(req.data);if(res.data)free(res.data);return ok;
}
static int cmd_push(const char *remote)
{
    char url[1200],target[MAX_PATH_GIT];RemoteRef *refs;unsigned int nr=0,i;char sym[MAX_PATH_GIT];U8 head[20],zero[20];int has,found=0;Buffer pack;
    refs=(RemoteRef*)calloc(MAX_REMOTE_REFS,sizeof(RemoteRef));if(!refs)return 1;
    if(!read_remote_url(remote,url,sizeof(url))||!read_head(head,target,sizeof(target),&has)||!has||strncmp(target,"refs/heads/",11)){fprintf(stderr,"git: push requires a commit on a local branch\n");free(refs);return 1;}
    if(!get_advert(url,"receive-pack",refs,&nr,sym,sizeof(sym))){free(refs);return 1;}
    memset(zero,0,20);
    for(i=0;i<nr;++i)if(!strcmp(refs[i].name,target)){found=1;break;}
    if(!build_push_pack(head,&pack)){fprintf(stderr,"git: cannot build push pack\n");free(refs);return 1;}
    if(!receive_push(url,target,found?refs[i].oid:zero,head,&pack)){free(pack.data);free(refs);return 1;}
    free(pack.data);free(refs);return 0;
}
static int checkout_commit(const U8 commit[20])
{
    Buffer c;char type[16],fs[MAX_PATH_GIT];U8 tree[20];Index idx;unsigned long i;
    if(!read_object(commit,&c,type)||strcmp(type,"commit")||!parse_commit_tree(c.data,c.len,tree)){if(c.data)free(c.data);return 0;}free(c.data);idx.v=NULL;idx.n=idx.cap=0;if(!flatten_tree(tree,"",&idx,0)){index_free(&idx);return 0;}
    for(i=0;i<idx.n;++i){Buffer b;if(!read_object(idx.v[i].oid,&b,type)||strcmp(type,"blob")){index_free(&idx);return 0;}to_fs_path(fs,idx.v[i].path);if(!write_file(fs,b.data,b.len)){free(b.data);index_free(&idx);return 0;}free(b.data);}
    if(!index_write(&idx)){index_free(&idx);return 0;}index_free(&idx);return 1;
}
static int write_clone_config(const char *url,const char *remote)
{
    char cfg[4096];int n=sprintf(cfg,"[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n\tbare = false\n\tlogallrefupdates = true\n[user]\n\tname = TinyVCS98 User\n\temail = tinyvcs98@localhost\n[remote \"%s\"]\n\turl = %s\n\tfetch = +refs/heads/*:refs/remotes/%s/*\n",remote,url,remote);return write_file(".git/config",(U8*)cfg,(unsigned long)n);
}
static int directory_empty(const char *path)
{
#if defined(__WATCOMC__)
    char pattern[MAX_PATH_GIT];struct _finddata_t fd;long h;if(!path_join(pattern,sizeof(pattern),path,"*.*"))return 0;h=_findfirst(pattern,&fd);if(h==-1L)return 1;do{if(strcmp(fd.name,".")&&strcmp(fd.name,"..")){_findclose(h);return 0;}}while(_findnext(h,&fd)==0);_findclose(h);return 1;
#else
    DIR *d=opendir(path);struct dirent *e;if(!d)return 0;while((e=readdir(d))!=NULL)if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){closedir(d);return 0;}closedir(d);return 1;
#endif
}
static int derive_clone_directory(const char *url,char *out,size_t cap)
{
    HttpURL parsed;char *slash,*end;size_t n;
    if(!parse_http_url(url,&parsed))return 0;
    end=strchr(parsed.path,'?');if(end)*end=0;
    while(strlen(parsed.path)>1&&parsed.path[strlen(parsed.path)-1]=='/')parsed.path[strlen(parsed.path)-1]=0;
    slash=strrchr(parsed.path,'/');if(!slash||!slash[1])return 0;
    ++slash;n=strlen(slash);if(n>4&&!strcmp(slash+n-4,".git"))n-=4;
    if(n==0||n>=cap)return 0;
    memcpy(out,slash,n);out[n]=0;return 1;
}
static int cmd_clone(const char *url,const char *directory)
{
    RemoteRef *refs;unsigned int nr=0,i;char sym[MAX_PATH_GIT],branch[MAX_PATH_GIT],localref[MAX_PATH_GIT],dest[MAX_PATH_GIT];Buffer pack;int default_found=0,result=1;U8 default_oid[20];
    refs=(RemoteRef*)calloc(MAX_REMOTE_REFS,sizeof(RemoteRef));if(!refs)return 1;
    if(directory&&*directory){if(strlen(directory)>=sizeof(dest))goto done;strcpy(dest,directory);}else if(!derive_clone_directory(url,dest,sizeof(dest))){fprintf(stderr,"git: cannot determine clone directory; specify one explicitly\n");goto done;}
    if(dir_exists(dest)){if(!directory_empty(dest)){fprintf(stderr,"git: destination '%s' is not empty\n",dest);goto done;}}
    else if(file_exists(dest)){fprintf(stderr,"git: destination '%s' is a file\n",dest);goto done;}
    else if(!make_dir(dest)){fprintf(stderr,"git: cannot create destination\n");goto done;}
    if(CHDIR(dest)!=0){fprintf(stderr,"git: cannot enter destination\n");goto done;}
    if(!get_advert(url,"upload-pack",refs,&nr,sym,sizeof(sym)))goto done;
    if(cmd_init()!=0)goto done;
    if(nr){if(!receive_upload_pack(url,refs,nr,&pack))goto done;if(!install_received_pack(&pack)){free(pack.data);goto done;}free(pack.data);}
    if(!write_clone_config(url,"origin"))goto done;
    branch[0]=0;if(!strncmp(sym,"refs/heads/",11))strcpy(branch,sym+11);
    if(!branch[0])for(i=0;i<nr;++i)if(!strncmp(refs[i].name,"refs/heads/",11)){strcpy(branch,refs[i].name+11);break;}
    for(i=0;i<nr;++i){char dst[MAX_PATH_GIT];if(!strncmp(refs[i].name,"refs/heads/",11)){if(strlen(refs[i].name+11)+20>=sizeof(dst))goto done;strcpy(dst,"refs/remotes/origin/");strcat(dst,refs[i].name+11);if(!write_ref(dst,refs[i].oid))goto done;if(branch[0]&&!strcmp(refs[i].name+11,branch)){memcpy(default_oid,refs[i].oid,20);default_found=1;}}
        else if(!strncmp(refs[i].name,"refs/tags/",10)){if(!write_ref(refs[i].name,refs[i].oid))goto done;}}
    if(default_found){if(strlen(branch)+12>=sizeof(localref))goto done;strcpy(localref,"refs/heads/");strcat(localref,branch);if(!write_ref(localref,default_oid))goto done;{char h[1200];sprintf(h,"ref: %s\n",localref);if(!write_file(".git/HEAD",(U8*)h,(unsigned long)strlen(h)))goto done;}if(!checkout_commit(default_oid))goto done;printf("Checked out branch '%s'.\n",branch);}
    else {if(!write_file(".git/HEAD",(U8*)"ref: refs/heads/master\n",23))goto done;printf("Cloned an empty repository.\n");}
    result=0;
done:free(refs);return result;
}
static int cmd_remote(int argc,char **argv)
{
    Buffer b;unsigned long p=0;char current[128]="";int verbose=(argc==1&&!strcmp(argv[0],"-v"));
    if(argc==3&&!strcmp(argv[0],"add"))return remote_add(argv[1],argv[2])?0:1;
    if(argc!=0&&!verbose){fprintf(stderr,"usage: git remote add <name> <http(s)-url> | git remote [-v]\n");return 1;}
    if(!read_file(".git/config",&b))return 1;
    while(p<b.len){unsigned long e=p;while(e<b.len&&b.data[e]!='\n')++e;
        if(e>p&&b.data[p]=='['&&b.data[e-1]==']'){
            current[0]=0;
            if(e-p>10&&!memcmp(b.data+p,"[remote \"",9)){size_t z=(size_t)(e-p-11);if(z<sizeof(current)){memcpy(current,b.data+p+9,z);current[z]=0;if(!verbose)printf("%s\n",current);}}
        }else if(verbose&&current[0]){
            unsigned long q=p,z;char remoteurl[1200];while(q<e&&(b.data[q]==' '||b.data[q]=='\t'))++q;
            if(e-q>5&&!memcmp(b.data+q,"url",3)){q+=3;while(q<e&&(b.data[q]==' '||b.data[q]=='\t'||b.data[q]=='='))++q;z=e-q;while(z&&b.data[q+z-1]=='\r')--z;if(z<sizeof(remoteurl)){memcpy(remoteurl,b.data+q,z);remoteurl[z]=0;{char shown[1300];url_mask(remoteurl,shown,sizeof(shown));printf("%s\t%s\t(fetch)\n%s\t%s\t(push)\n",current,shown,current,shown);}}}
        }
        p=e+1;
    }
    free(b.data);return 0;
}
static int is_ancestor(const U8 ancestor[20],const U8 descendant[20])
{
    U8 cur[20];int count=0;memcpy(cur,descendant,20);while(count++<4096){Buffer c;char type[16];unsigned long p=0;int found=0;U8 parent[20];if(!memcmp(cur,ancestor,20))return 1;if(!read_object(cur,&c,type)||strcmp(type,"commit")){if(c.data)free(c.data);return 0;}while(p<c.len){unsigned long e=p;while(e<c.len&&c.data[e]!='\n')++e;if(e-p>=47&&!memcmp(c.data+p,"parent ",7)){char h[41];memcpy(h,c.data+p+7,40);h[40]=0;if(hex_to_oid(h,parent)){found=1;break;}}p=e+1;}free(c.data);if(!found)return 0;memcpy(cur,parent,20);}return 0;
}
static int cmd_pull(const char *remote)
{
    U8 local[20],upstream[20];char target[MAX_PATH_GIT],remote_ref[MAX_PATH_GIT];int has;Index oldidx,headidx,newidx;char fs[MAX_PATH_GIT];unsigned long i,j;
    if(!read_head(local,target,sizeof(target),&has)||!has||strncmp(target,"refs/heads/",11)){fprintf(stderr,"git: pull requires a local branch with an existing commit\n");return 1;}
    if(cmd_fetch(remote)!=0)return 1;
    if(strlen(remote)+strlen(target+11)+16>=sizeof(remote_ref)){fprintf(stderr,"git: branch name too long\n");return 1;}
    strcpy(remote_ref,"refs/remotes/");strcat(remote_ref,remote);strcat(remote_ref,"/");strcat(remote_ref,target+11);
    if(!read_ref_oid(remote_ref,upstream)){fprintf(stderr,"git: no matching upstream branch %s\n",remote_ref);return 1;}
    if(!memcmp(local,upstream,20)){printf("Already up to date.\n");return 0;}
    if(!is_ancestor(local,upstream)){fprintf(stderr,"git: pull is not a fast-forward; merge on a modern Git client\n");return 1;}
    oldidx.v=NULL;oldidx.n=oldidx.cap=0;headidx.v=NULL;headidx.n=headidx.cap=0;newidx.v=NULL;newidx.n=newidx.cap=0;
    if(!index_read(&oldidx)||!head_tree(&headidx)){fprintf(stderr,"git: cannot read index or HEAD tree\n");index_free(&oldidx);index_free(&headidx);return 1;}
    if(oldidx.n!=headidx.n){fprintf(stderr,"git: staged changes prevent pull\n");index_free(&oldidx);index_free(&headidx);return 1;}
    for(i=0;i<oldidx.n;++i){U8 workoid[20];for(j=0;j<headidx.n;++j)if(!strcmp(oldidx.v[i].path,headidx.v[j].path))break;
        if(j==headidx.n||memcmp(oldidx.v[i].oid,headidx.v[j].oid,20)){fprintf(stderr,"git: staged changes prevent pull\n");index_free(&oldidx);index_free(&headidx);return 1;}
        to_fs_path(fs,oldidx.v[i].path);if(!file_exists(fs)||!hash_file_blob(fs,workoid)||memcmp(workoid,oldidx.v[i].oid,20)){fprintf(stderr,"git: working tree changes prevent pull\n");index_free(&oldidx);index_free(&headidx);return 1;}}
    if(scan_untracked(&oldidx,"")){fprintf(stderr,"git: untracked files prevent a safe pull\n");index_free(&oldidx);index_free(&headidx);return 1;}
    index_free(&headidx);
    {Buffer c;char type[16];U8 tree[20];if(!read_object(upstream,&c,type)||strcmp(type,"commit")||!parse_commit_tree(c.data,c.len,tree)){if(c.data)free(c.data);index_free(&oldidx);return 1;}free(c.data);if(!flatten_tree(tree,"",&newidx,0)){index_free(&oldidx);return 1;}}
    for(i=0;i<oldidx.n;++i)if(!is_indexed(&newidx,oldidx.v[i].path)){to_fs_path(fs,oldidx.v[i].path);remove_file(fs);}
    for(i=0;i<newidx.n;++i){Buffer b;char type[16];if(!read_object(newidx.v[i].oid,&b,type)||strcmp(type,"blob")){index_free(&oldidx);index_free(&newidx);return 1;}to_fs_path(fs,newidx.v[i].path);if(!write_file(fs,b.data,b.len)){free(b.data);index_free(&oldidx);index_free(&newidx);return 1;}free(b.data);}
    if(!index_write(&newidx)||!write_ref(target,upstream)){index_free(&oldidx);index_free(&newidx);return 1;}
    index_free(&oldidx);index_free(&newidx);printf("Fast-forwarded %s.\n",target+11);return 0;
}

static int cmd_config(int argc,char **argv)
{
    char val[512];const char *key,*newval=NULL;
    if(argc<2||strcmp(argv[0],"--global")!=0){fprintf(stderr,"usage: git config --global user.name|user.email|user.token [value]\n");return 1;}
    key=argv[1];
    {   /* tolerate a missing space before a quoted value: user.name"Bob" arrives as user.nameBob */
        static const char *const keys[]={"user.name","user.email","user.token"};unsigned int ki;
        if(argc==2)for(ki=0;ki<3;++ki){size_t kl=strlen(keys[ki]);
            if(STRNICMP(key,keys[ki],kl)==0&&key[kl]){newval=key+kl;key=keys[ki];break;}}
    }
    if(STRICMP(key,"user.name")&&STRICMP(key,"user.email")&&STRICMP(key,"user.token")){fprintf(stderr,"git: unsupported config key '%s' (use user.name, user.email or user.token)\n",key);return 1;}
    if(argc==3)newval=argv[2];
    else if(argc>3){fprintf(stderr,"usage: git config --global %s \"value\"\n",key);return 1;}
    if(!newval){if(global_get(key,val,sizeof(val))){printf("%s\n",val);return 0;}return 1;}
    if(!global_set(key,newval))return 1;
    return 0;
}
static void usage(void)
{
    printf("TinyVCS98 - Git-compatible Win98 command-line client\n\n"
           "  git config --global user.name \"Your Name\"\n  git config --global user.email \"you@example.com\"\n"
           "  git config --global user.token \"YOUR-TOKEN\"\n"
           "  git init\n  git clone <http(s)-url> [directory]\n"
           "  git remote add <name> <http(s)-url>\n  git remote -v\n");
    printf("  git add <file-or-directory> [more paths...]\n  git add -A\n"
           "  git rm <path>\n  git commit -m \"message\"\n"
           "  git status\n  git log\n  git restore <commit-id> <path>\n");
    printf("  git fetch [remote]\n  git pull [remote]  (fast-forward only)\n  git push [remote]\n\n"
           "Git SHA-1 repository format; smart HTTP protocol v0 over HTTP and HTTPS (TLS 1.2).\n");
    printf("HTTPS: CA bundle ca-bundle.crt next to git.exe (or GIT_SSL_CAINFO); GIT_SSL_NO_VERIFY=1 disables checks.\n"
           "Credentials: https://user:token@host/path.git, or set user.name + user.token once with git config --global\n");
}
int main(int argc,char **argv)
{
    const char *remote="origin";
    if(argc<2){usage();return 1;}
    if(!strcmp(argv[1],"config"))return cmd_config(argc-2,argv+2);
    if(!strcmp(argv[1],"init")&&argc==2)return cmd_init();
    if(!strcmp(argv[1],"clone")&&(argc==3||argc==4))return cmd_clone(argv[2],argc==4?argv[3]:NULL);
    if(!strcmp(argv[1],"remote"))return cmd_remote(argc-2,argv+2);
    if(!strcmp(argv[1],"add"))return cmd_add(argc-2,argv+2);
    if(!strcmp(argv[1],"rm")&&argc==3)return cmd_rm(argv[2]);
    if(!strcmp(argv[1],"commit")&&argc>=4&&!strcmp(argv[2],"-m"))return cmd_commit(argv[3]);
    if(!strcmp(argv[1],"status")&&argc==2)return cmd_status();
    if(!strcmp(argv[1],"log")&&argc==2)return cmd_log();
    if(!strcmp(argv[1],"restore")&&argc==4)return cmd_restore(argv[2],argv[3]);
    if(!strcmp(argv[1],"fetch")&&(argc==2||argc==3))return cmd_fetch(argc==3?argv[2]:remote);
    if(!strcmp(argv[1],"push")&&(argc==2||argc==3))return cmd_push(argc==3?argv[2]:remote);
    if(!strcmp(argv[1],"pull")&&(argc==2||argc==3))return cmd_pull(argc==3?argv[2]:remote);
    usage();return 1;
}
