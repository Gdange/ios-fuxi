/* Original fail-closed client policy; no GPU or Objective-C dependencies. */
#pragma once
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace fuxi {
enum class ClientMode { deny, allow, census };
static inline bool clientName(const char *s) {
 size_t n=strlen(s);if(!n || n>63)return false;
 for(size_t i=0;i<n;++i)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||
     (s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'||s[i]=='.'))return false;
 return true;
}
static inline ClientMode parseClients(char *data,size_t length,const char *process) {
 if(!strcmp(process,"backboardd") || !strcmp(process,"SpringBoard"))return ClientMode::deny;
 if(!clientName(process)||memchr(data,0,length))return ClientMode::deny;
 data[length]=0;ClientMode result=ClientMode::deny;
 for(char *line=data;line;) {
  char *next=strchr(line,'\n');if(next)*next++=0;
  size_t n=strlen(line);while(n && (line[n-1]==' '||line[n-1]=='\t'||line[n-1]=='\r'))line[--n]=0;
  while(*line==' '||*line=='\t')++line;
  if(*line && *line!='#') {
   ClientMode mode=ClientMode::allow;
   if(!strncmp(line,"census ",7)) {mode=ClientMode::census;line+=7;while(*line==' '||*line=='\t')++line;}
   if(!clientName(line))return ClientMode::deny;
   if(!strcmp(line,process)) {
    if(result!=ClientMode::deny && result!=mode)return ClientMode::deny;
    result=mode;
   }
  }
  line=next;
 }
 return result;
}
static inline ClientMode readClients(const char *path,const char *process) {
 int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return ClientMode::deny;
 struct stat st={};char data[8193];size_t used=0;bool ok=false;
 if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&!(st.st_mode&0022)&&st.st_size<=8192) {
  ssize_t n;while(used<sizeof(data)-1 && (n=read(fd,data+used,sizeof(data)-1-used))>0)used+=(size_t)n;
  char tail;ok=read(fd,&tail,1)==0 && used==(size_t)st.st_size;
 }
 close(fd);return ok?parseClients(data,used,process):ClientMode::deny;
}
}
