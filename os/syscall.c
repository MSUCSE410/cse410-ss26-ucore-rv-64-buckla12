#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"
#include "file.h"
#include "plic.h"

// File types only need to be considered.
#define DIR 0x040000 // directory
#define FILE 0x100000 // ordinary regular file

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
int sys_mmap(uint64 start, unsigned long long len, int port, int flag, int fd){
	if(len > 1073741824 || (port & ~0x7) != 0 || (port & 0x7) == 0 || !PGALIGNED(start)){
		printf("Error: Parameters recieved incorect\n");
		return -1;
	}
	else if(len ==0){
		return 0;
	}
	struct proc *p = curr_proc();
	unsigned long long rounded = PGROUNDUP(len);
	unsigned long long s = (unsigned long long)start;
	for(unsigned long long temp = s; temp < s + rounded; temp+=PGSIZE){
		pte_t *pte = walk(p->pagetable, temp, 0);
		
		if (pte == 0){

		}
		else if(*pte & PTE_V){
			printf("Error: Already allocated\n");
			return -1;
		}
	}
	int flags = PTE_U;

	if (port & 1) flags |= PTE_R;
	if (port & 2) flags |= PTE_W;
	if (port & 4) flags |= PTE_X;
	for(unsigned long long temp = s; temp < s + rounded; temp+=PGSIZE){
		void* pPage = kalloc();
		if(pPage == 0){
			return -1;
		}
		pte_t *pte = walk(p->pagetable, temp, 1);

		*pte = PA2PTE(pPage) | PTE_V | flags;
	}
	return 0;
}

int sys_munmap(uint64 start, unsigned long long len){
	if(!PGALIGNED(start)){
		printf("Error: Not page aligned\n");
		return -1;
	}
	struct proc *p = curr_proc();
	unsigned long long rounded = PGROUNDUP(len);
	unsigned long long s = (unsigned long long)start;
	for(unsigned long long temp = s; temp < s + rounded; temp+=PGSIZE){
		pte_t *pte = walk(p->pagetable, temp, 0);
		
		if (!(*pte & PTE_V)){
			printf("Error: Page %d was not valid\n", temp%PGSIZE);
			return -1;
			
		}
	}
	for(unsigned long long temp = s; temp < s + rounded; temp+=PGSIZE){
		pte_t *pte = walk(p->pagetable, temp, 0);
		uint64 pPage= (uint64)PTE2PA(*pte);
		kfree((void*)pPage);
		*pte = *pte & ~PTE_V;
	}
	return 0;
}
/*
* LAB1: you may need to define sys_task_info here
*/
int sys_task_info(uint64 ti){
	struct proc* process = curr_proc();
	if (ti == 0) 
		return -1;

    uint64 pa = useraddr(process->pagetable, ti);
    if (pa == 0)
        return -1;

    TaskInfo *user_ti = (TaskInfo *)pa;
	user_ti->status = Running;
	user_ti->time = (get_cycle() / (CPU_FREQ / 1000)) - process->taskinfo->time;

    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        user_ti->syscall_times[i] = process->taskinfo->syscall_times[i];
    }
    return 0;
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

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

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

uint64 sys_spawn(uint64 va)
{
	struct proc *np;
    struct proc *p = curr_proc();
    char filename[200];
    
    if (copyinstr(p->pagetable, filename, va, 200) < 0) {
        return -1; 
    }
    
    if ((np = allocproc()) == 0) {
        return -1;
    }
    
    memset(np->trapframe, 0, sizeof(*np->trapframe));
    
    
    np->parent = p;
    
    struct inode *ip;
    if ((ip = namei(filename)) == 0) {
        errorf("invalid file name: %s\n", filename);
        np->state = UNUSED;
        return -1;
    }
    
    if (bin_loader(ip, np) < 0) {
        iput(ip);  // Don't forget to release the inode
        np->state = UNUSED;
        return -1;
    }
    
    iput(ip);  // Release the inode after loading
    
    np->trapframe->a0 = 0;
    
    np->state = RUNNABLE;
    add_task(np);
    
    return (uint64)np->pid;
}

uint64 sys_set_priority(long long prio){
    // TODO: your job is to complete the sys call
    struct proc *process = curr_proc();
    
    if(prio < 2){
        return -1;
    }
    
    process->priority = (int)prio;
    process->pass = BIG_STRIDE / process->priority;
    return (uint64)process->priority;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
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

int sys_fstat(int fd, uint64 stat_addr){
	struct proc *p = curr_proc();
    struct Stat st;
    struct file *f;
	

    if (fd < 0 || fd >= FD_BUFFER_SIZE || p->files[fd] == 0) {
        return -1;
    }
    
    f = p->files[fd];
    
    if (stat_addr == 0 || stat_addr >= 0x80000000) {
        return -1;
    }
    
    if (f->type == FD_INODE) {
        ivalid(f->ip);
        
        st.dev = 0;
        st.ino = f->ip->inum;
        st.nlink = f->ip->nlink;
        
        if (f->ip->type == T_DIR) {
            st.mode = DIR; 
        } else if (f->ip->type == T_FILE) {
            st.mode = FILE;
        } else {
            return -1;
        }
        

        for (int i = 0; i < 7; i++) {
            st.pad[i] = 0;
        }
        
        if (either_copyout(1, stat_addr, (char*)&st, sizeof(st)) < 0) {
            return -1; 
        }
        
        return 0; 
    }
    
    return -1;
}

int sys_linkat(int olddirfd, uint64 oldpath_ptr, int newdirfd, uint64 newpath_ptr, uint64 flags){
	char oldpath[MAXPATH], newpath[MAXPATH];

	if (copyinstr(curr_proc()->pagetable, oldpath, oldpath_ptr, MAXPATH) < 0 || copyinstr(curr_proc()->pagetable, newpath, newpath_ptr, MAXPATH) < 0) {
    	return -1;
	}

    struct inode *ip, *dp;
    ip = namei(oldpath);
    if (ip == 0) return -1;
	ivalid(ip);

    char* last_slash = 0;
    for (char* p = newpath; *p != '\0'; p++) {
        if (*p == '/') {
            last_slash = p;
        }
    }

    char* filename;
    if (last_slash == 0) {
        dp = root_dir();
        filename = newpath;
    } else {
        *last_slash = '\0'; 
        dp = namei(newpath); 
        *last_slash = '/';
        filename = last_slash + 1; 
    }

    if (dp == 0) {
        iput(ip);
        return -1;
    }
	
	ivalid(dp);
	
    if (dp->type != T_DIR) {
        iput(ip);
        iput(dp);
        return -1;
    }

    if (dirlink(dp, filename, ip->inum) < 0) {
        iput(ip);
        iput(dp);
        return -1;
    }

    ip->nlink++;
    iupdate(ip);
    iput(ip);
    iput(dp);

    return 0;
}

int sys_unlinkat(int dirfd, uint64 path_ptr, uint64 flags){
	char path[MAXPATH];
    char name[DIRSIZ];
    struct inode *ip, *dp;

    // copy path from user to kernel 
    if (copyinstr(curr_proc()->pagetable, path, path_ptr, MAXPATH) < 0) {
    	return -1;
	}

    
    char* last_slash = 0;
    for (char* p = path; *p != '\0'; p++) {
        if (*p == '/') last_slash = p;
    }

    if (last_slash == 0) {
        dp = root_dir();
        strncpy(name, path, DIRSIZ);
    } else {
        *last_slash = '\0';
        dp = namei(path);
		*last_slash = '/';
        strncpy(name, last_slash + 1, DIRSIZ);
    }

    if (dp == 0) return -1;
    ivalid(dp);

    if ((ip = dirlookup(dp, name, 0)) == 0) {
        iput(dp);
        return -1;
    }
    ivalid(ip);

    if (ip->type == T_DIR) {
        iput(ip);
        iput(dp);
        return -1;
    }

    if (dirunlink(dp, name, ip->inum) < 0) {
        iput(ip);
        iput(dp);
        return -1;
    }

    ip->nlink--;
    iupdate(ip);

    iput(ip);
    iput(dp);

    return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
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
	case SYS_task_info:
		ret = sys_task_info(args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
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
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
