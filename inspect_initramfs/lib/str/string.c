#include "string.h"
void *memset(void *d,int c,size_t n){
    u8*p=(u8*)d;while(n--)*p++=(u8)c;return d;}
void *memcpy(void *d,const void *s,size_t n){
    u8*dd=(u8*)d;const u8*ss=(const u8*)s;while(n--)*dd++=*ss++;return d;}
int memcmp(const void *a,const void *b,size_t n){
    const u8*p=(const u8*)a,*q=(const u8*)b;
    while(n--){if(*p!=*q)return(int)*p-(int)*q;p++;q++;}return 0;}
size_t strlen(const char *s){const char*p=s;while(*p)p++;return(size_t)(p-s);}
int strcmp(const char *a,const char *b){
    while(*a&&*a==*b){a++;b++;}return(int)(unsigned char)*a-(int)(unsigned char)*b;}
char *strncpy(char *d,const char *s,size_t n){
    char*r=d;while(n&&*s){*d++=*s++;n--;}while(n--)*d++=0;return r;}
