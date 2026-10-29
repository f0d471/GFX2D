#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
int main(int argc,char **argv)
{
    char data[64]={0};uint32_t n=0;
    if(argc!=3)return 2;
    if(strcmp(argv[1],"write")==0)return platform_store_write("save",argv[2],(uint32_t)strlen(argv[2]))==0?0:1;
    if(strcmp(argv[1],"read")==0) {
        int rc=platform_store_read("save",data,sizeof data,&n);
        if(rc || n!=strlen(argv[2]) || memcmp(data,argv[2],n))return 1;
        puts("persistent record PASS");return 0;
    }
    return platform_store_write("../escape",data,1)==PLATFORM_INVALID?0:1;
}
