#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct{
	struct proc *head;
	struct proc *tail;
}high, middle, low;

struct proc *heap[NPROC+1];
int heapSize = 0;


static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);



//stride
int allocated = MLFQPORTION;
uint MLFQpass = 0;

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
	
}	

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
	
	p->priority = HIGHEST;
	p->curticks = 0;
	p->totalticks = 0;
	
	p->pass = 0;
	p->next = 0;
	p->prev = 0;
	p->time = 0;
	p->retval = 0;
	p->mainT = 0;
	p->sn = 0;
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
	
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  p->state = RUNNABLE;
	insert(p);
  release(&ptable.lock);


}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();
	/*acquire(&ptable.lock);*/
	if(curproc->mainT != 0)
		curproc = curproc->mainT;
	/*release(&ptable.lock);*/
  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
	insert(np);

  release(&ptable.lock);

  return pid;
}

int thread_create(thread_t *thread, void* (*start_routine)(void *), void *arg){
	
	int i;
	uint sn, sz, sp, ustack[2];
  struct proc *np;
  struct proc *curproc = myproc();
	if(curproc->mainT)
		curproc = curproc->mainT;
  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
	*thread = np->pid;
  acquire(&ptable.lock);
	np->pgdir = curproc->pgdir;	
  *np->tf = *curproc->tf;
	//alloc 2page (1 for use, 1 for guard)
	if((sn = getsn(curproc)) < 0){
			np->state = UNUSED;
			return -1;
			}
	/*
	 *if((sn = curproc->mainT == 0? getsn(curproc) : getsn(mainproc)) < 0){
	 *    np->state = UNUSED;
	 *    return -1;
	 *    }
	 */
	np->sn = sn;	 
	if((sz = allocuvm(np->pgdir, curproc->sz + (sn+HEAP_AREA)*2*PGSIZE, curproc->sz+(sn+1+HEAP_AREA)*2*PGSIZE)) == 0){
		np->state = UNUSED;
		return -1;	
	}
	clearpteu(np->pgdir, (char*)(sz - 2*PGSIZE));
	sp = sz;
	
	ustack[0] = 0xffffffff;
	ustack[1] = (uint)arg;

	sp -= 2*4;
	if(copyout(np->pgdir, sp, ustack, 2*4) < 0){
		deallocuvm(np->pgdir, sz, sz-2*PGSIZE );
		np->state = UNUSED;
		return -1;
	}
	curproc->pgdir = np->pgdir;
  np->sz = sz;
	np->tf->eip = (uint)start_routine;
	np->tf->esp = sp;
	np->parent = curproc->parent;
	/*np->parent = curproc;*/
	//store mainThread
	/*
	 *if(curproc->mainT == 0){
   *	np->mainT = curproc;
	 *  curproc->mainT = curproc;
	 *}
	 *else{
	 *  np->mainT = curproc->mainT;
	 *}
	 */
	np->mainT = curproc;
	curproc->mainT = curproc;
	np->priority = curproc->priority;
	np->curticks = curproc->curticks;
	if(np->priority == STRIDE){
		np->stride = curproc->stride;
		np->pass = curproc->pass;
	}
	else{
		np->totalticks = curproc->totalticks;
	}

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);
	
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));



  np->state = RUNNABLE;
	insert(np);
  release(&ptable.lock);

  return 0;


}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p, *cp;
	/*
	 *struct proc *mt;
	 *uint bit;
	 */
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
/*
 *     for(fd = 0; fd < NOFILE; fd++){
 *        if(curproc->ofile[fd]){
 *          fileclose(curproc->ofile[fd]);
 *          curproc->ofile[fd] = 0;
 *        }
 *    
 *      }
 *
 *      begin_op();
 *      iput(curproc->cwd);
 *      end_op();
 *      curproc->cwd = 0;
 */

	for(p = ptable.proc; p<&ptable.proc[NPROC]; p++){
		if(p == curproc || 
				(curproc->mainT && p->mainT == curproc->mainT)){
			for(fd = 0; fd < NOFILE; fd++){
				if(p->ofile[fd]){
					fileclose(p->ofile[fd]);
					p->ofile[fd] = 0;
				}
		
			}

			begin_op();
			iput(p->cwd);
			end_op();
			p->cwd = 0;
		}
	}
	acquire(&ptable.lock);
  // Parent might be sleeping in wait().
	wakeup1(curproc->parent);
  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		//current process is managed by LWP
		if(curproc->mainT && p->mainT == curproc->mainT
				&& p != curproc){
			deallocuvm(p->pgdir, p->sz, p->sz-2*PGSIZE);
			kfree(p->kstack);
			p->sn = 0;
			p->pid = 0;
      p->kstack = 0;
			p->pid = 0;
			p->parent = 0;
			p->name[0] = 0;
			p->killed = 0;
			p->mainT = 0;
			p->state = UNUSED;
			
			for(cp = ptable.proc; cp< &ptable.proc[NPROC]; cp++){
				if(cp->parent == p){
					cp->parent = initproc;
					if(cp->state == ZOMBIE)
						wakeup1(initproc);
				}
			}
		}
		if(p->parent == curproc){
			p->parent = initproc;
			if(p->state == ZOMBIE)
				wakeup1(initproc);
		}
	}

	 /*wakeup1(curproc->parent); */
  // Jump into the scheduler, never to return.
	if(curproc->priority == STRIDE){
		allocated -= curproc->portion;
	}
  curproc->state = ZOMBIE;
	sched();
  panic("zombie exit");
}

void thread_exit(void *retval){
	struct proc *curproc = myproc();
	struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");
	if(curproc == curproc->mainT)
		panic("mainThread can't not exit");
  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in thread_join().
  wakeup1(curproc->mainT);

  // Pass abandoned children to init.
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->parent == curproc){
			if(p->mainT != 0){
				p->parent = curproc->mainT;     
				if(p->state == ZOMBIE)
					wakeup1(p->mainT);
			}
			else{
				p->parent = initproc;
				if(p->state == ZOMBIE)
					wakeup1(initproc);
			}
		}
	}

  // Jump into the scheduler, never to return.
	
	curproc->retval = retval;
  curproc->state = ZOMBIE;
	sched();
  panic("zombie exit");

}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
				p->parent = 0;
				p->name[0] = 0;
				p->killed = 0;
				if(p->priority == STRIDE)	
					allocated -= p->portion;
				p->state = UNUSED;
				release(&ptable.lock);
				return pid;
			}
		}

		// No point waiting if we don't have any children.
		if(!havekids || curproc->killed){
			release(&ptable.lock);
			return -1;
		}

		// Wait for children to exit.  (See wakeup1 call in proc_exit.)
		sleep(curproc, &ptable.lock);  //DOC: wait-sleep
	}
}

int thread_join(thread_t thread, void **retval){
	struct proc *p;
  int havekids;
  struct proc *curproc = myproc();
	uint bit;
 	
	if(curproc->mainT != curproc)
		panic("worker thread can't not join");


  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->mainT != curproc || p->pid != thread)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){

				deallocuvm(p->pgdir, p->sz, p->sz-2*PGSIZE);
				bit = 1 << p->sn;
				curproc->sn = curproc->sn ^bit ;
				kfree(p->kstack);
				p->sn = 0;
				p->pid = 0;
        p->kstack = 0;
				p->pid = 0;
				p->parent = 0;
				p->name[0] = 0;
				p->killed = 0;
				p->mainT = 0;
				*retval = p->retval;
				p->state = UNUSED;
				release(&ptable.lock);
				return 0;
			}
		}

		// No point waiting if we don't have any children.
		if(!havekids || curproc->killed){
			release(&ptable.lock);
			return -1;
		}

		// Wait for children to exit.  (See wakeup1 call in proc_exit.)
		sleep(curproc, &ptable.lock);  //DOC: wait-sleep
	}


}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
MLFQscheduler(void)
{
	struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
	MLFQtick = 0;
	int priority = -1;
  // Enable interrupts on this processor.
	for(;;){
		sti();
    // Loop over process table looking for process to run.
  	acquire(&ptable.lock);
		if(high.head != 0 && high.head->state == RUNNABLE){
			p = high.head;
			priority = HIGHEST;
		}
		else if(middle.head != 0 && middle.head->state == RUNNABLE){
			p = middle.head;
			priority = MIDDLE;
			
		}
		else if(low.head != 0 && low.head->state == RUNNABLE){
			p = low.head;
			priority = LOWEST;
		}
		else{
			priority = -1;
		}
		if(priority == -1){
			release(&ptable.lock);
			continue;
		}

		
			c->proc = p;
			switchuvm(p);
			p->state = RUNNING;
			
			dequeue(priority);
			swtch(&(c->scheduler), p->context);
			switchkvm();
			
			if(MLFQtick !=0 && MLFQtick % PRIORITY_BOOST == 0){
				middle.head = 0;
				low.head = 0;
				for(struct proc *tp =ptable.proc; tp<&ptable.proc[NPROC]; tp++){
					if(tp->state == RUNNABLE && tp->priority != STRIDE){
						tp->next = 0;
						tp->prev = 0;
						tp->priority = HIGHEST;
						tp->totalticks = 0;
						insert(tp);
					}
				}
				
		}
		c->proc = 0;
		release(&ptable.lock);
	}
}

void
STRIDEscheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
	MLFQtick = 0;
  for(;;){
    // Enable interrupts on this processor.
		sti();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
		
		if(heapSize >0){
		
			p = remove(1);
			if(UINT_MAX - p->pass < p->stride){
				for(struct proc *tp = ptable.proc; tp<&ptable.proc[NPROC]; tp++){
					if(tp->priority == STRIDE){
						tp->pass = tp->stride;
					}
				}
			}
		// Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
	  	c->proc = p;
			switchuvm(p);
    	p->state = RUNNING;
    	swtch(&(c->scheduler), p->context);
    	switchkvm();
			c->proc = 0;
		}
		release(&ptable.lock);
	}		
}


void
scheduler1(void)
{
	struct proc *p, *tp;
	struct cpu *c = mycpu();
	c->proc = 0;
	MLFQtick = 0;
	int priority;
	sti();
	for(;;){
		//STRIDE scheduling
		acquire(&ptable.lock);
		if(heapSize > 0 && peek() <= MLFQpass){
			p = remove(1);
				// Switch to chosen process.  It is the process's job
		// to release ptable.lock and then reacquire it
		// before jumping back to us.
			c->proc = p;
			switchuvm(p);
			p->state = RUNNING;
			swtch(&(c->scheduler), p->context);
			switchkvm();
			p->curticks = 0;
			c->proc = 0;
		}

		//MLFQ scheduling
		else{
			if(high.head != 0){
				p = high.head;
				priority = HIGHEST;
			}
			else if(middle.head != 0){
				p = middle.head;
				priority = MIDDLE;
			}
			else if(low.head != 0){
				p = low.head;
				priority = LOWEST;
			}
			else{
				priority = -1;
				release(&ptable.lock);
				continue;
			}
			
			MLFQpass += MLFQSTRIDE;
			c->proc = p;
			switchuvm(p);
			p->state = RUNNING;
			dequeue(priority);
			
			swtch(&(c->scheduler), p->context);
			switchkvm();
			//priority boost	
			if(MLFQtick !=0 && MLFQtick % PRIORITY_BOOST == 0){
				middle.head = 0;
				low.head = 0;
				for(tp =ptable.proc; tp<&ptable.proc[NPROC]; tp++){
					if(tp->state == RUNNABLE && tp->priority != STRIDE){
						tp->next = 0;
						tp->prev = 0;
						tp->priority = HIGHEST;
						tp->totalticks = 0;
						insert(tp);
					}
				}
				
			}
			
			c->proc = 0;
		 }
		release(&ptable.lock);
	}

}

void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}
// Give up the CPU for one scheduling round.
void
yield1(void)
{
	acquire(&ptable.lock);  //DOC: yieldlock
	struct proc *p = myproc();
	/*struct proc *tp, *np;*/
	/*struct cpu *c = mycpu();*/
/*
 *  if(p->mainT != 0){
 *    p->time++;
 *    if(p->mainT == p)
 *      p->curticks++;
 *    else{
 *      p->mainT->curticks++;
 *      p->curticks = p->mainT->curticks;
 *    }
 *  }
 *  else
 *    p->curticks++;
 *
 */
	p->curticks++;
	//MLFQ scheudling
	if(p->priority != STRIDE){
		MLFQtick++;
		//MLFQpass overflow control
		if(UINT_MAX - MLFQpass < MLFQSTRIDE){
			overflow(peek());
		}
		p->totalticks += p->curticks;
		
		//prevent starvation.
		if(MLFQtick % PRIORITY_BOOST == 0)
			goto sched;
		//when totalticks over each level allotment
		else if(p->priority == HIGHEST && 
				p->totalticks >= HIGHEST_ALLOTMENT){
			p->priority = MIDDLE;	
		}
		else if(p->priority == MIDDLE && 
				p->totalticks >= MIDDLE_ALLOTMENT){
			p->priority = LOWEST;		
		}
		//when curticks over each level quantum	
		if(p->priority == HIGHEST &&
			p->curticks >= HIGHEST_QUANTUM){
				p->curticks = 0;
				goto sched;
		}
		else if(p->priority == MIDDLE &&
			p->curticks >= MIDDLE_QUANTUM){
			p->curticks = 0;
			goto sched;
		}
		else if(p->priority == LOWEST && 
			p->curticks >= LOWEST_QUANTUM){
			p->curticks = 0;	
			goto sched;
		}
	}
	//STRIDE scheduling
	else{
		//pass overflow
		if(p->curticks >= STRIDE_QUANTUM){
			
			if(UINT_MAX - p->pass < p->stride){
				overflow(p->pass);
			}
			p->pass += p->stride;
			goto sched;
		}
	}
	//if proc pass above checklist and is lwp, 
	//can swtch diretly
/*
 *  if(p->mainT != 0){
 *    np = p;
 *    //find next proc
 *    for(tp = ptable.proc; tp<&ptable.proc[NPROC]; tp++){
 *      if(tp->state == RUNNABLE && tp->mainT == p->mainT){
 *        np = tp->time <= np->time ? tp : np;
 *      }
 *    }
 *    if(p == np){
 *      return;
 *    }
 *    p->state = RUNNABLE;
 *    insert(p);
 *    
 *    if(np->priority != STRIDE){
 *      if(np->prev != 0){
 *        np->prev->next = np->next;
 *        if(np->next != 0)
 *          np->next->prev = np->prev;
 *      }
 *      else{
 *        dequeue(np->priority);
 *      }
 *    }
 *    else{
 *      remove(np->index);
 *    }	
 *    
 *    np->state = RUNNING;
 *    c->proc = np;
 *    
 *
 *    swtch(&p->context, np->context);
 *      
 *    return;
 *
 *  }
 */
	release(&ptable.lock);
	return;

sched:
		p->state = RUNNABLE;
		insert(p);
		sched();
		release(&ptable.lock);
}
void
yield2(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  struct proc* p = myproc();
	p->state = RUNNABLE;
	
	if(p->priority != STRIDE){
		if(UINT_MAX - MLFQpass < MLFQSTRIDE){
			overflow(peek());
		}
		p->totalticks += p->curticks;
		
		//if process runtime is over priority allotment, then change priority
		if(p->priority == HIGHEST && p->totalticks >= HIGHEST_ALLOTMENT){
			p->priority = MIDDLE;
		}
		else if(p->priority == MIDDLE && p->totalticks >= MIDDLE_ALLOTMENT){
			p->priority = LOWEST;	
		}
		p->curticks = 0;
	}
	else{
		//pass overflow
		if(UINT_MAX - p->pass < p->stride){
			overflow(p->pass);
		}
		p->pass += p->stride;
	}
	insert(p);
  sched();
  release(&ptable.lock);
}
// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
	if(p->state == RUNNABLE){

		if(p->priority == STRIDE)
			remove(p->index);
		else{
			if(p->prev)
				p->prev->next = p->next;
			if(p->next)
				p->next->prev = p->prev;
		}
	}
  p->state = SLEEPING;
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
			insert(p);
		}
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
	struct proc *killedproc = 0;
	int success = 0;	
  acquire(&ptable.lock); 
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->pid == pid){
			success = 1;
			p->killed = 1;
			killedproc = p;
			if(p->state == SLEEPING)
        p->state = RUNNABLE;
			
		}
	}
	if(killedproc && killedproc->mainT){	
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
			if(p->mainT == killedproc->mainT){
				p->killed = 1;
			
				// Wake process from sleep if necessary.
				if(p->state == SLEEPING)
					p->state = RUNNABLE;
			}
		}
	}
  release(&ptable.lock);
	if(success)	
  	return 0;
	else 
		return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
	/*
	 *for(int i = 1; i<heapSize+1; i++){
	 *  cprintf("%d %d %d\n", heap[i]->pid, heap[i]->pass, heap[i]->index);
	 *}
	 */
	cprintf("heapSize : %d\n", heapSize);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s ", p->pid, state, p->name);
		if(p->parent)
			cprintf("%s %d", p->parent->name, p->parent->pid);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
getppid(void)
{
	return myproc()->parent->pid;
}

int
getlev(void)
{
	return myproc()->priority;
}

int
set_cpu_share(int portion)
{
	if(portion + allocated > 100)
		return -1;
	struct proc *p = myproc();
	if(p->priority != STRIDE){
		if(high.head == p)
			high.head = p->next;
		else if(middle.head == p)
			middle.head = p->next;
		else if(low.head == p)
			low.head = p->next;
		else{
			if(p->prev)
				p->prev->next = p->next;
			if(p->next)
				p->next->prev = p->prev;
		}
		p->next = 0;
		p->prev = 0;
	}
	else{
		allocated -= p->portion;
	}
	p->stride = TICKET/portion;
	p->priority = STRIDE;
	p->portion = portion;
	allocated += portion;
	p->pass = peek();

	return 0;
}

int
peek()
{
	return heap[1]->pass;
}	

void
swap(int a, int b){
	struct proc *tmp = heap[a];
	heap[a]->index = b;
	heap[b]->index = a;
	heap[a] = heap[b];
	heap[b] = tmp;
}

void
insert(struct proc *p)
{
	p->next = p->prev = 0;
	switch(p->priority){
		case HIGHEST:
			if(high.head == 0){
				high.head = p;
				high.tail = p;
			}
			else{
				p->prev = high.tail;
				high.tail->next = p;
				high.tail = p;
			}
			break;
		case MIDDLE:
			if(middle.head == 0){
				middle.head = p;
				middle.tail = p;
			}
			else{
				p->prev = middle.tail;
				middle.tail->next = p;
				middle.tail = p;
			}
			break;
		case LOWEST:
			if(low.head == 0){
				low.head = p;
				low.tail = p;
			}	
			else{
				p->prev = low.tail;
				low.tail->next = p;
				low.tail = p;
			}
			break;
		case STRIDE:
			heap[++heapSize] = p;
			p->index = heapSize;

			int child = heapSize;
			int parent = child/2;

			while(child >1 && heap[child]->pass < heap[parent]->pass){
				swap(child, parent);
				child = parent;
				parent /=2;
			}
			break;
		default:
			cprintf("error\n");
	}
}

struct proc*
remove(int index)
{	
	struct proc *p = heap[index];
	swap(index, heapSize--);
	int parent;
	int child;
	p->index = -1;
	//heapify down
	if(index != heapSize){
		parent = index;
		child = index*2;
		if(child+1 <= heapSize){
			child = heap[child]->pass < heap[child+1]->pass ? child : child+1;
		}
		while(child <= heapSize && heap[child]->pass < heap[parent]->pass){
			swap(child, parent);
			parent = child;
			child *=2;

			if(child+1 <= heapSize){
				child = heap[child]->pass < heap[child+1]->pass ? child : child+1;
			}
		}	
	}
	return p;
}


void
dequeue(int priority){
	switch(priority){
		case HIGHEST:
			high.head = high.head->next;
			high.head->prev = 0;
			break;
		case MIDDLE:
			middle.head = middle.head->next;
			middle.head->prev = 0;
			break;
		case LOWEST:
			low.head = low.head->next;
			low.head->prev = 0;
			break;
	}
}

void
overflow(uint pass){
	uint min = pass < MLFQpass ? pass : MLFQpass;
	for(struct proc *tp = ptable.proc; tp<&ptable.proc[NPROC]; tp++){
		if(tp->priority == STRIDE){
			tp->pass -= min;
		}		
	}
	MLFQpass -= min;
}

uint
getsn(struct proc* p)
{
	int bit = 1;
	//if argument is mainThread
	//get where is empty stack number 
	if(p->mainT == p || p->mainT == 0){
		for(int i = 0; i<32; bit = bit<<1){
			/*cprintf("%x\n", bit);*/
			if((p->sn & bit) >> i ){
				i++;
				continue;
			}
			p->sn = p->sn | bit;
			/*cprintf("%d\n\n",i);*/
			return i;
			/*cprint("%d\n",i);*/
		}	
	}
	//if argumnet is not mainThread
	//return my stack number
	else{
		return p->sn;
	}
	return -1;
}

void
exec_thread(struct proc *curproc)
{
	struct proc *p, *cp;
	int fd;


	for(p = ptable.proc; p<&ptable.proc[NPROC]; p++){
		if(curproc->mainT && p->mainT == curproc->mainT
				&& p != curproc){
			for(fd = 0; fd < NOFILE; fd++){
				if(p->ofile[fd]){
					fileclose(p->ofile[fd]);
					p->ofile[fd] = 0;
				}
		
			}

			begin_op();
			iput(p->cwd);
			end_op();
			p->cwd = 0;
		}
	}
	acquire(&ptable.lock);
  // Parent might be sleeping in wait().
	wakeup1(curproc->parent);
  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		//current process is managed by LWP
		if(curproc->mainT && p->mainT == curproc->mainT
				&& p != curproc){
			deallocuvm(p->pgdir, p->sz, p->sz-2*PGSIZE);
			kfree(p->kstack);
			p->sn = 0;
			p->pid = 0;
      p->kstack = 0;
			p->pid = 0;
			p->parent = 0;
			p->name[0] = 0;
			p->killed = 0;
			p->mainT = 0;
			p->state = UNUSED;
			
			for(cp = ptable.proc; cp< &ptable.proc[NPROC]; cp++){
				if(cp->parent == p){
					cp->parent = initproc;
					if(cp->state == ZOMBIE)
						wakeup1(initproc);
				}
			}
		}
		if(p->parent == curproc){
			p->parent = initproc;
			if(p->state == ZOMBIE)
				wakeup1(initproc);
		}
	}

	curproc->mainT = 0;
	/*wakeup1(curproc->parent);*/
	release(&ptable.lock);

}
