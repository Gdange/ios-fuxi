/* Private client storage. The traversable parent is writable only by root. */
#pragma once
#include "ClientGate.hpp"
#include <cerrno>
namespace fuxi {
static inline bool privateStorage(int fd,bool directory,uid_t uid) {
 struct stat s{};return fd>=0 && !fstat(fd,&s) && s.st_uid==uid &&
  (directory?S_ISDIR(s.st_mode):S_ISREG(s.st_mode)) &&
  (s.st_mode&07777)==(directory?0700:0600) && (directory || s.st_nlink==1);
}
// Caller owns the returned FD. Nonroot clients cannot create their own parent.
static inline int openClientStorage(const char *client) {
 if(!clientName(client) || (geteuid()!=0 && strcmp(client,"backboardd")))return -1;
 int db=open("/private/var/db",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
 if(db<0)return -1;
 if(geteuid()==0 && mkdirat(db,"fuxi-metal",0755) && errno!=EEXIST){close(db);return -1;}
 int parent=openat(db,"fuxi-metal",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(db);
 struct stat s{};
 if(parent<0 || fstat(parent,&s) || s.st_uid!=0 || !S_ISDIR(s.st_mode) ||
    ((s.st_mode&07777)!=0755 && (s.st_mode&07777)!=0700)) {if(parent>=0)close(parent);return -1;}
 char name[96];snprintf(name,sizeof(name),"%s-%lu",client,(unsigned long)geteuid());
 if(geteuid()==0 && mkdirat(parent,name,0700) && errno!=EEXIST){close(parent);return -1;}
 int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(parent);
 if(!privateStorage(fd,true,geteuid())){if(fd>=0)close(fd);return -1;}return fd;
}
}
