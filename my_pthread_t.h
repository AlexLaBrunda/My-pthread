// File:	my_pthread_t.h
// Author:	Yujie REN
// Date:	09/23/2017

// name: Joshua B. Kim, Alex LaBrunda and Ronald Kleyzit
// cp.cs.rutgers.edu of iLab: jbk91
// iLab Server: factory.cs.rutgers.edu
#ifndef MY_PTHREAD_T_H
#define MY_PTHREAD_T_H

#define _GNU_SOURCE
#define THREAD_SIZE (1024 * 1024)

#define THREADREQ 0
#define LIBRARYREQ 1

// include lib header files that you need here:
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>

typedef uint my_pthread_t;

//	this structure identifies each area of memory that that has been allocated or freed.
//	adjacent free cells will be merged and the second cell will be removed.
typedef struct memory_cell {
	int offset;			//	offset in bytes from beginning of memory area
	int size;				//	size of memory, negative means it is free (available)
} memory_cell;

//	one of these structures exists for each thread.  it contains an "array" of memory cells (see memory_cell)
//	one for each each area of memory that that has been allocated or freed.
typedef struct memory_cells_header {
	struct memory_cell* cells;		//	pointer to cells list area, see memory_cell definition above
	my_pthread_t tid;							//	Id of the thread that owns the memory
	int index;		//	index of next available slot - zero (0) based - we will try to keep it less than size - 1
	int slot_count;			//	Total number of slots
} memory_cells_header;

//	the structure holds an "array" of memory_cells_headers (one for each thread including the library)
typedef struct memory_cells_header_array {
	struct memory_cells_header* cells;		//	pointer to cells list area, see memory_cells_header definition above
	int index;		//	index of next available slot - zero (0) based - we will try to keep it less than size - 1
	int slot_count;			//	Total number of slots
} memory_cells_header_array;

//	identifies the state/status of a thread
typedef enum state {
	RUNNING, 
	READY,
	JOIN_WAITING,
	MUTEX_WAITING,
	DONE
} State;

//	information about a thread.  there is one for each thread in 
typedef struct threadControlBlock {
	void* return_code;
	my_pthread_t tid; 
	my_pthread_t joined_to_tid;
	ucontext_t context;
	int pages_allocated;
	State status;
} threadControlBlock; 

//	structure pointing to an "array" of tids (thread Ids), it can grow if it need more room.
//	we create more than 1 of these
typedef struct tid_array_header {
	my_pthread_t* tids;		//	pointer to memory area
	int index;		//	index of next available slot - zero (0) based - we will try to keep it less than size - 1
	int slot_count;			//	Total number of slots
} tid_array_header;

//	mutex struct definition.  1 for each mutex.  it holds the lock, the tid of the current lock holder
//	and an "array" of tids (threads) waiting on the lock
typedef struct my_pthread_mutex_t {
	int the_lock;
	my_pthread_t current_lock_holder;			//	we identify the current_lock_holder by his tid
	tid_array_header my_lock_wait_list;		//	a list of tids who are waiting on the lock
} my_pthread_mutex_t;

//	this structure points to an "array" of threadControlBlocks, 1 per thread, it can grow if it needs more room
typedef struct tcb_array_header {
	struct threadControlBlock* tcbs;		//	pointer to memory area
	int index;		//	index of next available slot - zero (0) based - we will try to keep it less than size - 1
	int slot_count;			//	Total number of slots
} tcb_array_header;

//	a growable "array" of pointers to threadControlBlocks.  it is used for our priority queues
typedef struct tcb_pointer_array_header {
	threadControlBlock** tcbptrs;		//	pointer to memory area
	int index;		//	index of next available slot - zero (0) based - we will try to keep it less than size - 1
	int slot_count;			//	Total number of slots
} tcb_pointer_array_header;

//	a growable "array" of pointers to mutex pointers.  needed to clean up mutex locks when a thread exits
typedef struct mutex_pointer_array_header {
	my_pthread_mutex_t** mutexptrs;		//	pointer to memory area
	int index;		//	index of next available slot - zero (0) based - we will try to keep it less than size - 1
	int slot_count;			//	Total number of slots
} mutex_pointer_array_header;

//	a structure used to return a pointer to a threadControlBlock and its position in an "array"
typedef struct tcb_pointer_and_slot {
	threadControlBlock* tcb_ptr;
	int slot;
} tcb_pointer_and_slot;

//	used to return both a cell pointer and the slot it occupies in the memory_cells array
typedef struct cell_pointer_and_slot {
	memory_cell* cell_ptr;
	int slot;
} cell_pointer_and_slot;

//	used to return the tid and number of the page from the beginning of the thread's allocated memory
typedef struct tid_and_page {
	my_pthread_t tid;				//	thread id
	int page;								//	number of the page from the beginning of the thread's allocated memory
} tid_and_page;

// Function Declarations:

//	print_who_was_called
void print_who_was_called (const char* func);

//	do_not_print (see the bottom of my_pthread_t.h where we set debug_print to printf to print
//	set debug_print to do_not_print to turn printing off
int do_not_print (const char* format, ...);

//	return the smaller value
int min (int i, int j);

//	return the larger value
int max (int i, int j);

//	print memory cells
void print_memory_cells_for_tid (my_pthread_t tid);

//	find memory cells header for tid
memory_cells_header* find_memory_cells_header_for_tid (my_pthread_t tid);

//	Find a free memory cell big enough to satisfy the request.
cell_pointer_and_slot find_free_memory_cell (struct memory_cells_header* mch_ptr, int size);

//	swap_out_and_assign make sure the thread has access to these pages (at the beginning of the memory
int swap_out_and_assign (my_pthread_t tid, int pages);

//	swap_page_out someone else owns the page, so swap it to another spot or disk
int swap_page_out(int page);

//	swap_page_in switch the faulted  page with the needed page
void	swap_page_in (int needed_page_offset, int page_number);

//	Free memory cell.
void free_memory_cell (struct memory_cells_header* mch_ptr, memory_cell* cell_to_free);

//	Add a memory cell at the bottom of the cells
void add_memory_cell (struct memory_cells_header* mch_ptr, int offset, int size);

//	Insert a memory cell at the given spot.  Move cells down to make room.
void insert_memory_cell (struct memory_cells_header* mch_ptr, int slot, int offset, int size);

//	remove the memory cell at the given slot. 
memory_cell remove_memory_cell (struct memory_cells_header* mch_ptr, int slot);

//	find_free_memory_page
int find_free_memory_page (int page);

//	find_tids_memory_page
int find_tids_memory_page (int tid);

//	Grow the supplied area that hold the memory_cells
void grow_memory_cells (struct memory_cells_header* mch_ptr);

//	Create a growable array to hold memory_cells, for the tid, return a memory_cells_header
void create_memory_header (memory_cells_header* mch_ptr, my_pthread_t tid, int slot_count);

//	Create a growable array to hold memory_cells_header, return a memory_cells_header_array
memory_cells_header_array create_memory_header_array (int slot_count);

//	Add a memory_cells_header at the bottom of the cells
void add_memory_cells_header (my_pthread_t tid);

//	Create a growable array to hold threadControlBlocks, return a tcb_array_header
void create_tcb_array (int slot_count);

//	Add a tcb at the bottom of the tcbs
threadControlBlock* add_tcb ();

//	Create a growable array to hold threadControlBlock pointers, return a tcb_pointer_array_header
tcb_pointer_array_header create_tcb_pointer_array (int slot_count);

//	Add a tcb pointer at the bottom of the tcb pointers
int add_tcb_pointer (struct tcb_pointer_array_header* tcbpah_ptr, threadControlBlock* tcb_ptr);

//	Adjust all the tcb pointers to the new tcb buffer
void adjust_tcb_pointers (struct tcb_pointer_array_header* tcbpah_ptr, int adjustment);

//	Find next READY tcb, answer a tcb_pointer_and_slot
tcb_pointer_and_slot find_next_ready_tcb (struct tcb_pointer_array_header* tcbpah_ptr);

//	Find slot of the tcb pointer
int find_slot_of_tcb (struct tcb_pointer_array_header* tcbpah_ptr, threadControlBlock* tcb_ptr);

//	remove the tcb pointer from the slot
threadControlBlock* remove_tcb_pointer_from_slot (struct tcb_pointer_array_header* tcbpah_ptr, int slot);

//	move the tcb pointer from the slot to the bottom
int move_tcb_pointer_from_slot_to_bottom (struct tcb_pointer_array_header* tcbpah_ptr, int slot);

//	up_priority_of_currentThread
void up_priority_of_currentThread ();

//	Create a growable array to hold my_pthread_mutex_t pointers, return a mutex_pointer_array_header
mutex_pointer_array_header create_mutex_pointer_array (int slot_count);

//	Grow the supplied area that holds the my_pthread_mutex_t pointers
void grow_mutex_pointer_array (struct mutex_pointer_array_header* mutexpah_ptr);

//	Add a mutex pointer at the bottom of the mutex pointers
int add_mutex_pointer (struct mutex_pointer_array_header* mutexpah_ptr, my_pthread_mutex_t* mutex_ptr);

//	Find slot of the mutex pointer
int find_slot_of_mutex (struct mutex_pointer_array_header* mutexpah_ptr, my_pthread_mutex_t* mutex_ptr);

//	remove the mutex pointer from the slot
my_pthread_mutex_t* remove_mutex_pointer_from_slot (struct mutex_pointer_array_header* mutexpah_ptr, int slot);

//	Create a growable array to hold tids, return a tid_array_header
tid_array_header create_tid_array_header (int slot_count);

//	Grow the supplied area that holds the tids
void grow_tid_array (struct tid_array_header* tidh_ptr);

//	Add a tid at the bottom of the tids
my_pthread_t add_tid (struct tid_array_header* tidh_ptr, my_pthread_t tid);

//	find tcb 
threadControlBlock* find_tcb_for_tid (my_pthread_t tid);

//	find tcb with joined_to_tid of the given tid 
threadControlBlock* find_tcb_for_joined_to_tid (my_pthread_t tid);

//	ready the joined tcb
void ready_joined (threadControlBlock* joined_tcb, void * value_ptr);

//	ready all the joined tcbs to tcb
void ready_all_tcbs_joined_to_tcb (void * value_ptr);

//	fine tid slot
int find_tid_slot (struct tid_array_header* tidh_ptr, my_pthread_t tid);

//	remove tid
my_pthread_t remove_tid (struct tid_array_header* tidh_ptr, int slot);

//	initializes the my_pthread library and appropriate signal handlers;
//	must be called at the beginning of main function, prior to creating threads
void initializeLibrary ();

//	catch the seg fault when a thread accesses its memory that was protected to make sure it is present
static void segment_fault_handler (int signum, siginfo_t *sig_info, void *unused);

//	timer event handler [signal handler]
void timer_event_handler ();

// signal handler that deals with scheduling
void schedule();

// create a new thread
int my_pthread_create (my_pthread_t * thread, pthread_attr_t * attr, void *(*function)(void*), void * arg);

//	thread function to call the my_pthread_create function
void thread_function (void *(*x_function)(void*), void * x_arg);

// give CPU possession to other user level threads voluntarily
int my_pthread_yield ();

// terminate a thread
void my_pthread_exit (void *value_ptr);

// wait for thread termination
int my_pthread_join (my_pthread_t thread, void **value_ptr);

// remove first item waiting for the lock
threadControlBlock* mutex_lock_wait_pop (struct tid_array_header* tidh_ptr);

// initial the mutex lock
int my_pthread_mutex_init (my_pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr);

// acquire the mutex lock
int my_pthread_mutex_lock (my_pthread_mutex_t *mutex);

// release the mutex lock
int my_pthread_mutex_unlock (my_pthread_mutex_t *mutex);

// destroy the mutex
int my_pthread_mutex_destroy (my_pthread_mutex_t *mutex);

//	Our malloc function
void* myallocate (int size, const char *filename, const int line_number, int req);

//	shared memory allocate
void* shalloc (size_t size);

//	Our free function
void mydeallocate (void* pointer, const char *filename, const int line_number, int req);

#endif



//	#define debug_print printf
#define debug_print do_not_print

#define USE_MY_PTHREAD

#ifdef USE_MY_PTHREAD
#define pthread_t my_pthread_t
#define pthread_mutex_t my_pthread_mutex_t
#define pthread_create my_pthread_create
#define pthread_exit my_pthread_exit
#define pthread_join my_pthread_join
#define pthread_mutex_init my_pthread_mutex_init
#define pthread_mutex_lock my_pthread_mutex_lock
#define pthread_mutex_unlock my_pthread_mutex_unlock
#define pthread_mutex_destroy my_pthread_mutex_destroy
#define malloc(x) myallocate(x, __FILE__, __LINE__, THREADREQ)
#define free(x) mydeallocate(x, __FILE__, __LINE__, THREADREQ)
#endif