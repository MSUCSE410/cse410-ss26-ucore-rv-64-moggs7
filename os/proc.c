#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"
// Chapter 3 Addition - START
#include "timer.h"	// need to def for get_cycle()
// Chapter 4 Addition - END

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;
struct queue task_queue;

int threadid()
{
	return curr_proc()->pid;
}

int cpuid()
{
	return 0;
}

struct proc *curr_proc()
{
	return current_proc;
}

// initialize the proc table at boot time.
void proc_init()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = UNUSED;
		p->kstack = (uint64)kstack[p - pool];
		p->trapframe = (struct trapframe *)trapframe[p - pool];

		// Chapter 3 Addition - START
		// initialize start time and syscall times 
		p->start_time = 0;
		memset(p->syscall_times, 0, sizeof(p->syscall_times));
		// Chapter 3 Addition - END		

	}
	idle.kstack = (uint64)boot_stack_top;
	idle.pid = IDLE_PID;
	current_proc = &idle;
	init_queue(&task_queue);
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

struct proc *fetch_task()
{
	int index = pop_queue(&task_queue);
	if (index < 0) {
		debugf("No task to fetch\n");
		return NULL;
	}
	debugf("fetch task %d(pid=%d) from task queue\n", index,
	       pool[index].pid);
	return pool + index;
}

void add_task(struct proc *p)
{
	push_queue(&task_queue, p - pool);
	debugf("add task %d(pid=%d) to task queue\n", p - pool, p->pid);
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc *allocproc()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED) {
			goto found;
		}
	}
	return 0;

found:
	// init proc
	p->pid = allocpid();
	p->state = USED;
	p->ustack = 0;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->pagetable = uvmcreate((uint64)p->trapframe);

	// Chapter 5 Additions - START
	// init priority and stride fields
	p->priority = 16;
	p->stride = 0;
	// Chapter 5 Additions - END

	memset(&p->context, 0, sizeof(p->context));
	memset((void *)p->kstack, 0, KSTACK_SIZE);
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
	memset((void *)p->files, 0, sizeof(struct file *) * FD_BUFFER_SIZE);

	// Chapter 5 Additions - START
	// reset process slot times
	p->start_time = 0;
	memset(p->syscall_times, 0, sizeof(p->syscall_times));
	// Chapter 5 Additions - END

	p->context.ra = (uint64)usertrapret;
	p->context.sp = p->kstack + KSTACK_SIZE;
	return p;
}

int init_stdio(struct proc *p)
{
	for (int i = 0; i < 3; i++) {
		if (p->files[i] != NULL) {
			return -1;
		}
		p->files[i] = stdio_init(i);
	}
	return 0;
}

// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler()
{
	struct proc *p;
	struct proc *best;

	// Chapter 6 Additions - START
	for (;;) {
		best = 0;	// reset best process
		// check all processes in pool for the best one
		for (p = pool; p < &pool[NPROC]; p++)
		{
			if (p->state == RUNNABLE)
			{
				// choose new best if this is better than current best
				if (best == 0 || p->stride < best->stride) // prioritize lower stride
				{
					best = p;
				}
			}
		}
		if (best == 0)	// no runnable processes found
		{
			panic("all app are over!\n");
		}

		// check if process has ever been run before
		if (best->start_time == 0)
		{
			best->start_time = get_cycle();
		}

		// increase stride after prcoess is selected 
		best->stride += BIG_STRIDE / best->priority;

		tracef("swtich to proc %d", best - pool);
		best->state = RUNNING;
		current_proc = best;
		swtch(&idle.context, &best->context);

		// Chapter 6 Additions - END
	}
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched()
{
	struct proc *p = curr_proc();
	if (p->state == RUNNING)
		panic("sched running");
	swtch(&p->context, &idle.context);
}

// Give up the CPU for one scheduling round.
void yield()
{
	current_proc->state = RUNNABLE;
	sched();
}

// Free a process's page table, and free the
// physical memory it refers to.
void freepagetable(pagetable_t pagetable, uint64 max_page)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmunmap(pagetable, TRAPFRAME, 1, 0);
	uvmfree(pagetable, max_page);
}

void freeproc(struct proc *p)
{
	if (p->pagetable)
		freepagetable(p->pagetable, p->max_page);
	p->pagetable = 0;
	for (int i = 0; i < FD_BUFFER_SIZE; i++) {
		if (p->files[i] != NULL) {
			fileclose(p->files[i]);
		}
	}
	p->state = UNUSED;
}

int fork()
{
	struct proc *np;
	struct proc *p = curr_proc();
	int i;
	// Allocate process.
	if ((np = allocproc()) == 0) {
		panic("allocproc\n");
	}
	// Copy user memory from parent to child.
	if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
		panic("uvmcopy\n");
	}
	np->max_page = p->max_page;
	// Copy file table to new proc
	for (i = 0; i < FD_BUFFER_SIZE; i++) {
		if (p->files[i] != NULL) {
			// TODO: f->type == STDIO ?
			p->files[i]->ref++;
			np->files[i] = p->files[i];
		}
	}
	// copy saved user registers.
	*(np->trapframe) = *(p->trapframe);
	// Cause fork to return 0 in the child.
	np->trapframe->a0 = 0;
	np->parent = p;
	np->state = RUNNABLE;
	return np->pid;
}

// Chapter 5 Additions - START
int spawn(char *name)
{
	/**
	Build the child as a fresh process running requested program from the start
	*/
	struct proc *np;
	struct proc *p = curr_proc();
    struct inode *ip;

	ip = namei(name);
    if (ip == 0) {
        return -1;
    }
    ivalid(ip);

	// allocate child process
	np = allocproc();
    if (np == 0) {
        iput(ip);
        return -1;
    }

	// Chapter 6 Additions - START
	// perform stdio setup to install fd 0,1, & 2
	if (init_stdio(np) < 0) {
        iput(ip);
        freeproc(np);
        return -1;
    }
	// Chapter 6 Additions - END


    bin_loader(ip, np);
    iput(ip);

	// child metadata
	np->parent = p;			// record relationship
	np->state = RUNNABLE;	// mark as runnable

	// mirror child-side convention from fork & set child's return register to 0
    np->trapframe->a0 = 0;

	// queue task and return
    return np->pid;
}
// Chapter 5 Additions - END


int push_argv(struct proc *p, char **argv)
{
	uint64 argc, ustack[MAX_ARG_NUM + 1];
	uint64 sp = p->ustack + USTACK_SIZE, spb = p->ustack;
	// Push argument strings, prepare rest of stack in ustack.
	for (argc = 0; argv[argc]; argc++) {
		if (argc >= MAX_ARG_NUM)
			panic("...");
		sp -= strlen(argv[argc]) + 1;
		sp -= sp % 16; // riscv sp must be 16-byte aligned
		if (sp < spb) {
			panic("...");
		}
		if (copyout(p->pagetable, sp, argv[argc],
			    strlen(argv[argc]) + 1) < 0) {
			panic("...");
		}
		ustack[argc] = sp;
	}
	ustack[argc] = 0;
	// push the array of argv[] pointers.
	sp -= (argc + 1) * sizeof(uint64);
	sp -= sp % 16;
	if (sp < spb) {
		panic("...");
	}
	if (copyout(p->pagetable, sp, (char *)ustack,
		    (argc + 1) * sizeof(uint64)) < 0) {
		panic("...");
	}
	p->trapframe->a1 = sp;
	p->trapframe->sp = sp;
	// clear files ?
	return argc; // this ends up in a0, the first argument to main(argc, argv)
}

int exec(char *path, char **argv)
{
	infof("exec : %s\n", path);
	struct inode *ip;
	struct proc *p = curr_proc();
	if ((ip = namei(path)) == 0) {
		errorf("invalid file name %s\n", path);
		return -1;
	}
	uvmunmap(p->pagetable, 0, p->max_page, 1);
	bin_loader(ip, p);
	iput(ip);
	return push_argv(p, argv);
}

int wait(int pid, int *code)
{
	struct proc *np;
	int havekids;
	struct proc *p = curr_proc();

	for (;;) {
		// Scan through table looking for exited children.
		havekids = 0;
		for (np = pool; np < &pool[NPROC]; np++) {
			if (np->state != UNUSED && np->parent == p &&
			    (pid <= 0 || np->pid == pid)) {
				havekids = 1;
				if (np->state == ZOMBIE) {

					// Chapter 5 Additions - START
					// if status code is 0, do not store exit code
					if (code != 0)
						*code = np->exit_code;
					// Chapter 5 Additions - END

					// Found one.
					pid = np->pid;
					np->state = UNUSED;

					return pid;
				}
			}
		}
		if (!havekids) {
			return -1;
		}
		p->state = RUNNABLE;
		// add_task(p); // skipped because scheduler no longer uses queue
		sched();
	}
}

// Exit the current process.
void exit(int code)
{
	struct proc *p = curr_proc();
	// Chapter 5 Additions - START
	struct proc *np;

	p->exit_code = code;
	debugf("proc %d exit with %d", p->pid, code);

	// Set the `parent` of all children to NULL
	for (np = pool; np < &pool[NPROC]; np++) {
		if (np->parent == p) {
			np->parent = NULL;
		}
	}

	if (p->parent != NULL) {
		// Parent should `wait`
		// free user memory but keep PCB for reaping
		if (p->pagetable) {
			freepagetable(p->pagetable, p->max_page);
			p->pagetable = 0;
		}
		p->max_page = 0;
		p->ustack = 0;
		p->state = ZOMBIE;
	}
	else {
		// no parent will reap this process
		freeproc(p);
	}

	sched();
}

int setpriority(long long prio)
{
	/**
	Helper function to assign process priority.
	prio: higher the number, the more often this process will run
	*/
    if (prio < 2) return -1;
    curr_proc()->priority = prio;
    return prio;
}
// Chapter 5 Additions - END


int fdalloc(struct file *f)
{
	debugf("debugf f = %p, type = %d", f, f->type);
	struct proc *p = curr_proc();
	for (int i = 0; i < FD_BUFFER_SIZE; ++i) {
		if (p->files[i] == NULL) {
			p->files[i] = f;
			debugf("debugf fd = %d, f = %p", i, p->files[i]);
			return i;
		}
	}
	return -1;
}