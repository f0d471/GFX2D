#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif
#include "platform.h"
#include "core/record.h"
#include "core/crc32.h"
#define STORE_BYTES 4096u
/* The SDK has one task owner. Calls are synchronous and not ISR-safe. */
static uint8_t s_record[2][STORE_BYTES+RECORD_HEADER_BYTES];
static int path(char *dst,size_t cap,const char *name,int bank)
{
    const char *dir=getenv("GFX2D_STORE_DIR");size_t n;
    if(!dir || !*dir)return PLATFORM_NOT_SUPPORTED;
    if(!name || !(n=strlen(name)) || n>32)return PLATFORM_INVALID;
    for(size_t i=0;i<n;i++)if(!((name[i]>='a'&&name[i]<='z') ||
        (name[i]>='A'&&name[i]<='Z') || (name[i]>='0'&&name[i]<='9') || name[i]=='_' || name[i]=='-'))return PLATFORM_INVALID;
    int count=snprintf(dst,cap,"%s/%s.%d",dir,name,bank);
    return count<0 || (size_t)count>=cap?PLATFORM_RANGE:0;
}
static int load(const char *p,uint8_t *out,uint32_t *bytes)
{
    FILE *f=fopen(p,"rb");size_t n;int extra,bad;
    *bytes=0;
    if(!f)return errno==ENOENT?0:PLATFORM_IO;
    n=fread(out,1,STORE_BYTES+RECORD_HEADER_BYTES,f);extra=fgetc(f);bad=ferror(f);fclose(f);
    if(bad)return PLATFORM_IO;
    if(extra==EOF)*bytes=(uint32_t)n;
    return 0;
}
static int current(const char *name,char paths[2][1024],uint32_t n[2])
{
    int rc=path(paths[0],1024,name,0);if(rc)return rc;
    rc=path(paths[1],1024,name,1);if(rc)return rc;
    rc=load(paths[0],s_record[0],&n[0]);if(rc)return rc;
    rc=load(paths[1],s_record[1],&n[1]);if(rc)return rc;
    return record_select(s_record[0],n[0],s_record[1],n[1]);
}
int platform_store_read(const char *name,void *dst,uint32_t cap,uint32_t *bytes)
{
    char paths[2][1024];uint32_t n[2],length;int bank;
    if(!bytes || (!dst&&cap))return PLATFORM_INVALID;
    crc32_init();bank=current(name,paths,n);
    if(bank<0)return bank==-1?PLATFORM_CORRUPT:bank;
    if(record_validate(s_record[bank],n[bank],NULL,&length))return PLATFORM_CORRUPT;
    if(length>cap)return PLATFORM_RANGE;
    if(length)memcpy(dst,s_record[bank]+RECORD_HEADER_BYTES,length);
    *bytes=length;return 0;
}
int platform_store_write(const char *name,const void *data,uint32_t bytes)
{
    char paths[2][1024];uint32_t n[2],sequence=0;int old,bank,bad=0;FILE *f;
    if((!data&&bytes) || bytes>STORE_BYTES)return PLATFORM_RANGE;
    crc32_init();old=current(name,paths,n);
    if(old<0 && old!=-1)return old;
    if(old>=0)(void)record_validate(s_record[old],n[old],&sequence,NULL);
    bank=old==0?1:0;
    if(record_encode(s_record[bank],sizeof s_record[bank],data,bytes,sequence+1u))return PLATFORM_RANGE;
    f=fopen(paths[bank],"wb");if(!f)return PLATFORM_IO;
    size_t total=RECORD_HEADER_BYTES+bytes,to_write=total;
    /* PC fault injection deliberately stops the real inactive-slot write. */
    const char *cut=getenv("GFX2D_STORE_FAIL_AFTER");
    if(cut) { char *end;unsigned long v=strtoul(cut,&end,10);if(*end==0 && v<to_write)to_write=(size_t)v; }
    if(fwrite(s_record[bank],1,to_write,f)!=to_write || fflush(f)!=0)bad=1;
#ifdef _WIN32
    if(_commit(_fileno(f))!=0)bad=1;
#else
    if(fsync(fileno(f))!=0)bad=1;
#endif
    if(fclose(f)!=0)bad=1;
#ifndef _WIN32
    /* The directory entry needs its own durability barrier. */
    int directory=open(getenv("GFX2D_STORE_DIR"),O_RDONLY|O_DIRECTORY);
    if(directory<0)bad=1;
    else { if(fsync(directory)!=0)bad=1;if(close(directory)!=0)bad=1; }
#endif
    return bad || to_write!=total?PLATFORM_IO:0;
}
