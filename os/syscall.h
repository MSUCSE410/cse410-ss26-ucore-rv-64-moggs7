#ifndef SYSCALL_H
#define SYSCALL_H

// Chapter 4 Addition - START
#include "types.h"
// Chapter 4 Addition - END

void syscall();

// Chapter 4 Addition - START
int sys_mmap(void *start, uint64 len, int port, int flag, int fd);   ///< memory map
int sys_munmap(void *start, uint64 len);                             ///< memory unmap
// Chapter 4 Addition - END


#endif // SYSCALL_H
