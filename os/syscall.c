#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

// Chapter 3 Addition - START
#include "proc.h"	// need to define TaskInfo
#include "vm.h"
// Chapter 3 Addition - END
// Chapter 4 Addition - START
#include "riscv.h"
#include "kalloc.h"
// Chapter 4 Addition - END

// Chapter 6 Additions - START
#include "fcntl.h"
// Chapter 6 Additions - END

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

// Chapter 3 Addition -- START
uint64 sys_gettimeofday(uint64 val, int _tz)
{
	if (val == 0) return -1;

	struct proc *p = curr_proc();
	TimeVal tv = {0};
	uint64 cycle = get_cycle();

	tv.sec = cycle / CPU_FREQ;
	tv.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	// write via pagetable helper
	if (copyout(p->pagetable, (uint64)val, (char *)&tv, sizeof(tv)) < 0) return -1;

	return 0;
}

int sys_task_info(TaskInfo *ti) 
{
    if (!ti) return -1;	// ensure user did not enter null/0
    struct proc *p = curr_proc();
	TaskInfo kti;
	// set task status to running
	kti.status = Running;
	// copy syscall user stats
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        kti.syscall_times[i] = p->syscall_times[i];
    }
	// uses same time logic of sys_gettimeofday
    uint64 now = get_cycle();
    kti.time = (now - p->start_time) * 1000 / CPU_FREQ;
	if (copyout(p->pagetable, (uint64)ti, (char*)&kti, sizeof(kti)) < 0) return -1;
    return 0;
}
// Chapter 3 Addition -- END


// Chapter 4 Addition - START
int sys_mmap(void *start, uint64 len, int port, int flag, int fd)
{
	// quit if len is 0
	if (len == 0) return 0;
	// upper limit 1 GiB
	if (len > (1ULL << 30)) return -1;
	uint64 va0 = (uint64)start;
    // start must be page aligned
    if (!PGALIGNED(va0)) return -1;
    // port must only use bits [0..2]
    if ((port & ~0x7) != 0) return -1;
    // port must not be 0 (meaningless mapping)
    if ((port & 0x7) == 0) return -1;
    struct proc *p = curr_proc();
    // round length up to whole pages
    uint64 sz = PGROUNDUP(len);
    // translate port to PTE flags
    int perm = PTE_U;
    if (port & 0x1) perm |= PTE_R;
    if (port & 0x2) perm |= PTE_W;
    if (port & 0x4) perm |= PTE_X;
    // error if any page alrea.dy mapped in [va0, va0+sz)
    for (uint64 va = va0; va < va0 + sz; va += PGSIZE) 
	{
        if (walkaddr(p->pagetable, va) != 0) 
		{
            return -1;
        }
    }
	// allocate + map each page
	for (uint64 va = va0; va < va0 + sz; va += PGSIZE)
	{
		void *pa = kalloc();
		if (pa == 0)
		{
			// don't reclaim on failure
			return -1;
		}
		memset(pa, 0, PGSIZE);
		if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) != 0)
		{
			// don't reclaim on failure
			return -1;
		}
	}
	return 0;
}
int sys_munmap(void *start, uint64 len)
{
	// quit if len is 0
	if (len == 0) return 0;
	// upper limit is 1 GiB
	if (len > (1ULL << 30)) return -1;
	uint64 va0 = (uint64)start;
	// page align start
	if (!PGALIGNED(va0)) return -1;
	struct proc *p = curr_proc();
	// round page length up to whole pages
	uint64 sz = PGROUNDUP(len);
	// if any page is unmapped in [va0, va0 + sz), error
	for (uint64 va = va0; va < va0 + sz; va += PGSIZE)
	{
		if (walkaddr(p->pagetable, va) == 0)
		{
			return -1;
		}
	}
	// pass number of pages
	uvmunmap(p->pagetable, va0, sz / PGSIZE, 1);
	return 0;
}
// Chapter 4 Addition - END


uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

// Chapter 5 Additions - START
uint64 sys_spawn(uint64 va)
{
	/**
	Sys call handler for spawn jobs
		va: user virtual address pointing to string
	 */
	char name[64];	// kernel side buffer (zero-trust)
	// fetch filename from user process
	if (copyinstr(curr_proc()->pagetable, name, va, sizeof(name)) < 0)
	{
		return -1;
	}
	// actually spawn the process
	return spawn(name);
}

uint64 sys_set_priority(long long prio)
{
	/**
	Sys call handler for priority assignments
	prio: higher the priority, the more often the process will run
	 */
    return setpriority(prio);
}
// Chapter 5 Additions - END

// Chapter 6 Additions - START
uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[MAXPATH];

	if (copyinstr(p->pagetable, path, va, MAXPATH) < 0)
	{
		errorf("openat: copyinstr failed\n");
		return -1;		
	}

	infof("openat: path=%s omode=%p\n", path, omode);
	return fileopen(path, omode);
}
// Chapter 6 Additions - END

uint64 sys_close(int fd)
{
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}


// Chapter 6 Additions - START

int sys_fstat(int fd,uint64 stat)
{
	struct proc *p = curr_proc();
	struct file *f;
	struct Stat st;

	// verify file descriptor is valid
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;

	// get file!!
	f = p->files[fd];

	// verify file is valid (inode backed)
	if (f == 0 || f->type != FD_INODE)
		return -1;

	// load inode metadata (if needed)
	ivalid(f->ip);

	// fill Stat with metadata
	st.dev = 0;
	st.ino = f->ip->inum;
	st.mode = (f->ip->type == T_DIR) ? DIR : FILE;
	st.nlink = f->ip->nlink;
	memset(st.pad, 0, sizeof(st.pad));

	// copy result back to user memory
	if (copyout(p->pagetable, stat, (char *)&st, sizeof(st)) < 0)
		return -1;

	// success!
	return 0;
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags)
{
	char old[MAXPATH];	// kernel buffer for old file name
	char new[MAXPATH];	// kernel buffer for new file name
	struct inode *ip, *dp;	// ip = inode of file, dp = inode of directory
	struct proc *p = curr_proc();

	// copy strings from user space
	if (copyinstr(p->pagetable, old, oldpath, MAXPATH) < 0)
		return -1;
	if (copyinstr(p->pagetable, new, newpath, MAXPATH) < 0)
		return -1;

	// find existing file
	ip = namei(old);	// finds inode
	if (ip == 0)
		return -1;

	// load inode metadata as necessary
	ivalid(ip);

	// root directory
	dp = root_dir();
	ivalid(dp);

	// increment link count
	ip->nlink++;	// tracks how many names point to a file 
	iupdate(ip);

	// add new directory entry
	if (dirlink(dp, new, ip->inum) < 0) 
	{
		// rollback if failed!!!
		ip->nlink--;
		iupdate(ip);
		iput(dp);
		iput(ip);
		return -1;
	}

	// release references
	iput(dp);
	iput(ip);

	return 0;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
	char kname[MAXPATH];	// kernel buffer for file name
	struct proc *p = curr_proc();
	struct inode *ip, *dp;	// ip = inode of file, dp = inode of directory

	// copy pathname from user space into kernel buffer
	if (copyinstr(p->pagetable, kname, name, MAXPATH) < 0)
		return -1;

	infof("unlink: kname = %s\n", kname);


	// find the inode for this filename
	ip = namei(kname);
	if (ip == 0)
	{
		errorf("unlink: namei failed for %s\n", kname);
		return -1;
	}
	ivalid(ip);	// load file inode metadata as necessary
	infof("unlink: found inode %d nlink=%d\n", ip->inum, ip->nlink);


	// get the root directory inode
	dp = root_dir();
	ivalid(dp);	

	// remove the name from the directory
	if (dirunlink(dp, kname) < 0) 
	{
		errorf("unlink: dirunlink failed for %s\n", kname);
		iput(dp);
		iput(ip);
		return -1;
	}

	// prevent negative link counts
	if (ip->nlink < 1) 
	{
		errorf("unlink: bad nlink %d for %s\n", ip->nlink, kname);
		iput(dp);
		iput(ip);
		return -1;
	}

	// update link count for this file
	ip->nlink--;
	iupdate(ip);

	// release references
	iput(dp);
	iput(ip);

	// success!!!
	return 0;
}

// Chapter 6 Additions - END


extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);

	// Chapter 3 Addition -- START
	// update syscall counter
	if (id >= 0 && id < MAX_SYSCALL_NUM) 
	{
		curr_proc()->syscall_times[id]++;
	}
	// Chapter 3 Addition -- END

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;

	// Chapter 3 Addition - START
	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;
	// Chapter 3 Addition - END
	// Chapter 4 Addition - START
	case SYS_mmap:
		ret = sys_mmap((void*)args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap((void*)args[0], args[1]);
		break;
	// Chapter 4 Addition - END

	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;

	// Chapter 6 Additions - START
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	// Chapter 6 Additions - END

	// Chapter 5 Additions - START	
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	// Chapter 5 Additions - END
	
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
