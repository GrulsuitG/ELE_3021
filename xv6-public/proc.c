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

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

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

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
	
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
scheduler(void)
{
  struct proc *p;
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
				for(struct proc *tp =ptable.proc; tp<&ptable.proc[NPROC]; tp++){
					if(tp->state == RUNNABLE && tp->priority != STRIDE){
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

// Give up the CPU for one scheduling round.
void
yield(void)
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

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
			else{
				if(p->priority == STRIDE ){
					remove(p->index);
				}
				else{
					if(p->prev)
						p->prev->next = p->next;
					if(p->next)
						p->next->prev = p->prev;
				}
			}
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
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
    cprintf("%d %s %s %d %x", p->pid, state, p->name, p->stride, p->pass);
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
