#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
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

uint64 sys_gettimeofday(uint64 va, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	struct proc *p = curr_proc();

	uint64 pa= useraddr(p->pagetable,va);
	uint64 cycle = get_cycle();
	uint64 sec = cycle / CPU_FREQ;
	uint64 usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

	if(pa != 0){
		TimeVal *t = (TimeVal *)pa;
		t->sec = sec;
		t->usec = usec;
		
	}
	return 0;
}
// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
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

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	if(id < MAX_SYSCALL_NUM){
		curr_proc()->taskinfo->syscall_times[id]++;
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
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
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
