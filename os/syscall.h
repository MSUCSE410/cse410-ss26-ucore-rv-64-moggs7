#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

void syscall();

//
// Project 2 additions
//
int sys_mmap(void *start, uint64 len, int port, int flag, int fd);   ///< memory map
int sys_munmap(void *start, uint64 len);                             ///< memory unmap

#endif // SYSCALL_H
