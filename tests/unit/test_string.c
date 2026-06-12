#include <stdio.h>
#include <assert.h>
#include "../../include/types.h"
#include "../../lib/str/string.c"
int main(void){
    u8 buf[16]; memset(buf,0xAB,16);
    for(int i=0;i<16;i++) assert(buf[i]==0xAB);
    const u8 src[8]={1,2,3,4,5,6,7,8}; u8 dst[8];
    memcpy(dst,src,8); assert(memcmp(dst,src,8)==0);
    assert(strlen("hello")==5 && strlen("")==0);
    assert(strcmp("abc","abc")==0 && strcmp("abc","abd")<0);
    char d[8]; strncpy(d,"hi",8);
    assert(d[0]=='h'&&d[1]=='i'&&d[2]==0);
    printf("All string tests passed.\n"); return 0;
}
