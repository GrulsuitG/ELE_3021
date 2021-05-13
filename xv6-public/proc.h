// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
	pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  
	
	uint priority;               // MLFQ priority
  uint curticks;               // current running time
  uint totalticks;             // total running time
	struct proc *next;					 // for mlfq
	struct proc *prev;					 // for mlfq
	
	
	uint stride;                 // portion of stride
	uint pass;                   // pass of stride schedule
	uint index;									 // index for stride Queue
	int portion;								 // portion of CPU
	
	struct proc *mainT;						//MainThread for lwp
	int time; 										//how many schedule time by cpu for lwp
	void *retval;									//return value for lwp exit
	int yield;
	uint sn;											//stack number
																//away from mainthread stack 
																
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap


//priority
#define HIGHEST 0
#define MIDDLE 1
#define LOWEST 2
#define STRIDE 3

//time quantum
#define HIGHEST_QUANTUM 5
#define MIDDLE_QUANTUM 10
#define LOWEST_QUANTUM 20
#define STRIDE_QUANTUM 5

//time allotment
#define HIGHEST_ALLOTMENT 20
#define MIDDLE_ALLOTMENT (40 +HIGHEST_ALLOTMENT) 

#define PRIORITY_BOOST 200

//stride
#define TICKET 10000
#define MLFQPORTION 20
#define MLFQSTRIDE (TICKET/MLFQPORTION)

uint MLFQtick;
