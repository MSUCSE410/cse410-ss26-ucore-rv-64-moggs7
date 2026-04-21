#ifndef FCNTL_H
#define FCNTL_H

#include "types.h"  // for uint64 and uint32

#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR 0x002
#define O_CREATE 0x200
#define O_TRUNC 0x400

// Chapter 6 Additions - START
#define DIR  0x040000
#define FILE 0x100000

// The data that fstat(fd, &st) fills in so a user program can get metadata on a file descriptor
// Matches required fstat structure and mode values
struct Stat {
    uint64 dev; // device number
    uint64 ino; // inode number
    uint32 mode;    // file type (directory or file)
    uint32 nlink;   // number of hardlinks pointing to inode
    uint64 pad[7];
};
// Chapter 6 Additions - END


#endif // FCNIL_H