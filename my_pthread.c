// File:	my_pthread.c
// Author:	Yujie REN
// Date:	09/23/2017

// name: Joshua B. Kim, Alex LaBrunda and Ronald Kleyzit
// cp.cs.rutgers.edu of iLab: jbk91
// iLab Server: factory.cs.rutgers.edu, kill.cs.rutgers.edu

#include "my_pthread_t.h"

//	define our memory and paging area
static char memory_block[8 * 1024 * 1024] __attribute__ ((aligned (4096 * 32)));	// 8MB
size_t page_size;				//	a variable to hold the system page size
int memory_page_count;	//	the number of pages in memory_block
int swap_page_count;		//	the number of pages in swap file
int library_starting_page_count = 1024;		// Starting number of pages reserved for library use
tid_and_page* memory_map;				//	This is a pointer to an array of owners (tid) of page slots
int start_up_memory[10][2];		//	memory_cell created during startup - need to be added to our memory usage later
int setting_up_memory_management = 0;		//	memory allocation calls need to work differently until we are setup
int start_up_memory_index = 0;		//	the index of start_up_memory array, used until we are setup
int max_threads = 48;							//	the maximum number of threads supported
int interval_cnt = 0;							//	the number of consecutive times the current thread has been dispatched
int swap_file = 0;								//	the file descriptor of our swap file
int stack_size = 64 * 1024;				//	used for stack in context
void* swap_buffer;								//	a buffer to hold a page when we need to swap two
short initialize_called;		//	we need to set stuff up
short debug_level = 1;							//	0 means all, 1 means some
short ignore_timer_interrupt = 1;		//	used to keep some code running (not call schedule)

//	define constants for myallocate/mydeallocate functions
const char malloc_caller[2][10] = {"THREAD", "LIBRARY"};		//	used for printing/debug

const char statuses[5][15] = {"RUNNING", "READY", "JOIN_WAITING", "MUTEX_WAITING", "DONE"};		//	used for printing/debug

//	the structure holds an "array" of memory_cells_headers (one for each thread including the library)
memory_cells_header_array memory_header_array;

//	this structure points to an "array" of threadControlBlocks, 1 per thread, it can grow if it needs more room
tcb_array_header tcb_array;

//	a growable "array" of pointers to mutex pointers.  needed to clean up mutex locks when a thread exits
mutex_pointer_array_header mutex_array;

//	3 growable "arrays" of pointers to threadControlBlocks.  they are used for our priority queues
tcb_pointer_array_header priority_queues[3];		//	high, medium and low

static threadControlBlock* currentThread;		//	a pointer to the threadControlBlock of the current thread
static threadControlBlock* mainThread;			//	a pointer to the threadControlBlock of the main thread
static threadControlBlock* oldThread;				//	a pointer to the threadControlBlock of a saved thread
int priority_of_currentThread = 0;

//	a tid is used by some of the memory management functions
//	the tid of 1 is used for the LIBRARYREQ requests
//	the tid of 2 is for the main program calling thread
static my_pthread_t threadCount = 2;		//	we increment threadCount every time we create a new thread

static struct itimerval interval_timer;
static struct sigaction interval_timer_action;
static struct sigaction seg_fault_action;

//	print who was called
void print_who_was_called (const char* func) {
	if (debug_level == 0) {
		debug_print("---> %s called.\n", func);
	}
};

//	do_not_print (see the bottom of my_pthread_t.h where we set debug_print to printf to print
//	set debug_print to do_not_print to turn printing off
int do_not_print (const char* format, ...) {
};

//	return the smaller value
int min (int i, int j) {
	if (i < j) {
		return i;		
	} else {
		return j;
	}
};

//	return the larger value
int max (int i, int j) {
	if (i > j) {
		return i;		
	} else {
		return j;
	}
};

//	print memory cells
void print_memory_cells_for_tid (my_pthread_t tid) {
	memory_cells_header* cells_header = find_memory_cells_header_for_tid(tid);
	int i;
	for (i=0; i<cells_header->index; i++) {
		memory_cell* cell = cells_header->cells + i;
		debug_print("tid: %d, i: %d, cells_header->cells: %p, cells_header->index: %d, cell: %p, offset: %d, size: %d\n", tid, i, cells_header->cells, cells_header->index, cell, cell->offset, cell->size);
	}
};

//	find memory cells header for tid
memory_cells_header* find_memory_cells_header_for_tid (my_pthread_t tid) {
	print_who_was_called(__FUNCTION__);
	debug_print("memory_header_array.index: %d and tid: %d\n", memory_header_array.index, tid);
	int i;
	for (i=0; i<memory_header_array.index; i++) {
		memory_cells_header* mch_ptr = memory_header_array.cells + i;
		debug_print("tid = %d\n", mch_ptr->tid);
		if (mch_ptr->tid == tid) return mch_ptr;
	}
	return NULL;
};

//	swap out and assign make sure the thread has access to these pages (at the beginning of the memory
int swap_out_and_assign (my_pthread_t tid, int pages) {
	print_who_was_called(__FUNCTION__);
	int i, swapped_ok;
	my_pthread_t assigned_tid;
	for (i=0; i <= pages; i++) {
		assigned_tid = (memory_map + i)->tid;
		if ((assigned_tid != tid) && (assigned_tid == 0)) {
			swapped_ok = swap_page_out(i);		// someone else owns the page, so swap it to another spot or disk
			if (swapped_ok < 0) {
				return swapped_ok;
			}
		}
		if (assigned_tid == 0) {	// the page is available (possibly made so by above), so assign it to the thread
			(memory_map + i)->tid = tid;
			(memory_map + i)->page = i;
		}
	}
	return 0;
};

//	swap page out someone else owns the page, so swap it to another spot or disk
int swap_page_out (int page) {
	print_who_was_called(__FUNCTION__);
	my_pthread_t tid = (memory_map + page)->tid;
	int free_page = find_free_memory_page(page);
	if (free_page > 0) {
		if (free_page < memory_page_count) {	// the free page is in the memory area, so we can move it
			void* from = &memory_block[page * page_size]; 
			void* to = &memory_block[free_page * page_size];
			memmove(to, from, page_size);
		} else {		// the free page is in the disk area, so we must save to disk
			debug_print("swapping page %d to disk\n", free_page);
			lseek(swap_file, ((free_page - memory_page_count) * page_size), SEEK_SET);
			write(swap_file, &memory_block[page * page_size], page_size);
		}
		(memory_map + free_page)->tid = tid;		//	the page is now owned by this thread
		(memory_map + free_page)->page = (memory_map + page)->page;		//	this is the owner's page number
		(memory_map + page)->tid = 0;			//	this says the page is free
		(memory_map + page)->page = 0;		//	un-needed, means the page is nowhere
		return 0;
	} else {
		return -1;
	}
};

//	swap page in switch the faulted page with the needed page
//	needed_page_offset is the offset into memory_map of the page needed to be swapped in
//	page_number is the offset into memory_map and memory of where the page need to go
void swap_page_in (int needed_page_offset, int page_number) {
	print_who_was_called(__FUNCTION__);
	void* fault_page = &memory_block[page_number * page_size];	//	this is the page we are moving out
	if (needed_page_offset < memory_page_count) {	// page is in memory so swap them
		debug_print("page is in memory so swap them");
		void* needed_page = &memory_block[needed_page_offset * page_size];	//	memory address of page to swap in
		memmove(swap_buffer, fault_page, page_size);		//	move the page we are moving out to temp area
		memmove(fault_page, needed_page, page_size);		//	move the page we need to its real spot
		memmove(needed_page, swap_buffer, page_size);		//	move the page in temp area to the now free area in memory
		my_pthread_t saved_tid = memory_map[page_number].tid;		//	save the tid of the current owner
		memory_map[page_number].tid = currentThread->tid;				//	set the owner of the swapped in page
		memory_map[needed_page_offset].tid = saved_tid;					//	set the owner of where we moved the page
	} else {	// page is on the disk, so get it from there
		debug_print("swapping page %d in from disk\n", needed_page_offset);
		lseek(swap_file, ((needed_page_offset - memory_page_count) * page_size), SEEK_SET);		//	memory portion is not on the disk
		read(swap_file, swap_buffer, page_size);
		(memory_map + needed_page_offset)->tid = 0;			//	disk page slot is now free because it is in swap_buffer
		(memory_map + needed_page_offset)->page = 0;		//	page is nowhere
		swap_page_out(page_number);			// the page being swapped out may be moved to a free memory slot or disk
		memmove(fault_page, swap_buffer, page_size);		//	the page that faulted gets the page we saved
	}
};

//	Find a free memory cell big enough to satisfy the request.
cell_pointer_and_slot find_free_memory_cell (struct memory_cells_header* mch_ptr, int size) {
	print_who_was_called(__FUNCTION__);
	debug_print("looking for %d bytes\n", size);
	cell_pointer_and_slot cell_ptr_and_slot;
	int i;
	for (i=0; i<mch_ptr->index; i++) {
		memory_cell* cell = mch_ptr->cells + i;
		debug_print("i: %d, cell->offset: %d, size: %d\n", i, cell->offset, cell->size);
		if ((cell->size < 0) && (abs(cell->size) > size)) {
			cell_ptr_and_slot.cell_ptr = cell;
			cell_ptr_and_slot.slot = i;
			return cell_ptr_and_slot;
		}
	}
	cell_ptr_and_slot.cell_ptr = NULL;
	cell_ptr_and_slot.slot = 0;
	return cell_ptr_and_slot;
};

//	Free memory cell.
void free_memory_cell (struct memory_cells_header* mch_ptr, memory_cell* cell_to_free) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=0; i<mch_ptr->index; i++) {
		memory_cell* cell = mch_ptr->cells + i;
		if (cell == cell_to_free) {
			cell->size = cell->size * -1;
		}
	}
};

//	Add a memory cell at the bottom of the cells
void add_memory_cell (struct memory_cells_header* mch_ptr, int offset, int size) {
	print_who_was_called(__FUNCTION__);
//	Make sure we have room for the new cell
	if ((mch_ptr->index + 1) >= mch_ptr->slot_count) {
		grow_memory_cells(mch_ptr);			//	grow our memory area if we get too close to the end
	}
	memory_cell* cell = mch_ptr->cells + mch_ptr->index;
	debug_print("tid: %d, mch_ptr->cells: %p, mch_ptr->index: %d, cell: %p\n", mch_ptr->tid, mch_ptr->cells, mch_ptr->index, cell);
	debug_print("setting offset: %d and size: %d\n", offset, size);
	cell->offset = offset;
	cell->size = size;
	mch_ptr->index = mch_ptr->index + 1;
	debug_print("mch_ptr->index: %d\n", mch_ptr->index);
};

//	Insert a memory cell at the given spot.  Move cells down to make room.
void insert_memory_cell (struct memory_cells_header* mch_ptr, int slot, int offset, int size) {
	print_who_was_called(__FUNCTION__);
	debug_print("mch_ptr->index: %d, slot: %d, offset: %d, size: %d\n", mch_ptr->index, slot, offset, size);
//	If the cell goes at the end, let add_memory_cell do the work
	if (slot == mch_ptr->index) {
		add_memory_cell(mch_ptr, offset, size);
	} else {
//	Make sure we have room for the new cell
		if ((mch_ptr->index + 1) >= mch_ptr->slot_count) {
			grow_memory_cells(mch_ptr);			//	grow our memory area if we get too close to the end
		}
//	Move things down one slot to make room
		int i, j;
		for (i=mch_ptr->index; (i>slot); i--) {
			j = i - 1;
			*(mch_ptr->cells + i) = *(mch_ptr->cells + j);
		}
		memory_cell* cell = mch_ptr->cells + slot;
		cell->offset = offset;
		cell->size = size;
		mch_ptr->index++;
	}
};

//	remove the memory cell at the given slot.
memory_cell remove_memory_cell (struct memory_cells_header* mch_ptr, int slot) {
	print_who_was_called(__FUNCTION__);
//	Move things up one slot to remove the cell
	memory_cell* removed = mch_ptr->cells + slot;
	int i, j;
	for (i=slot; (i<=mch_ptr->index); i++) {
		j = i + 1;
		*(mch_ptr->cells + i) = *(mch_ptr->cells + j);
	}
	mch_ptr->index = mch_ptr->index - 1;
	return *removed;
};

//	Our malloc function
void* myallocate (int size, const char *filename, const int line_number, int req) {
	ignore_timer_interrupt = 1;
	print_who_was_called(__FUNCTION__);
	if (!initialize_called) {
		initializeLibrary();
	}
	int full_size = ((size + 7) / 8) * 8;
	my_pthread_t tid;
	int offset;
	if (req == LIBRARYREQ) {
		tid = 1;
	} else {
		tid = currentThread->tid;
	}
	debug_print("%d bytes called for by: %s (tid: %d), at line: %d\n", size, malloc_caller[req], tid, line_number);
	if (setting_up_memory_management == 1) {		//	this can only be the case for LIBRARYREQ but we won't check both
		debug_print("start_up_memory_index: %d\n", start_up_memory_index);
		debug_print("start_up_memory offset: %d and size: %d\n", start_up_memory[start_up_memory_index - 1][0], start_up_memory[start_up_memory_index - 1][1]);

		if (start_up_memory_index == 0) {
			offset = (memory_page_count - library_starting_page_count) * page_size;
		} else {
			offset = start_up_memory[start_up_memory_index - 1][0] + start_up_memory[start_up_memory_index - 1][1];
		}

		debug_print("offset: %d\n", offset);

		start_up_memory[start_up_memory_index][0] = offset;
		start_up_memory[start_up_memory_index][1] = full_size;

		void* memory = &memory_block[offset];
		start_up_memory_index++;
		debug_print("%d bytes allocated at offset: %d and address: %p\n", full_size, offset, memory);
		ignore_timer_interrupt = 0;
		return memory;
	} else {
		debug_print("about to call find_memory_cells_header_for_tid: %d\n", tid);
		memory_cells_header* cells_header = find_memory_cells_header_for_tid(tid);
		cell_pointer_and_slot cell_ptr_and_slot = find_free_memory_cell(cells_header, full_size);
		memory_cell* cell = cell_ptr_and_slot.cell_ptr;
		if (cell != NULL) {
			debug_print("free memory cell found - offset: %d, size: %d\n", cell->offset, cell->size);
			int avalible_size = cell->size * -1;
			cell->size = full_size;
			int slot = cell_ptr_and_slot.slot + 1;
			int new_offset = cell->offset + full_size;
			int new_size = (avalible_size - full_size) * -1;
			insert_memory_cell(cells_header, slot, new_offset, new_size);
			void* memory = &memory_block[cell->offset];

			if (tid != 1) {
				memory_cell* last_cell = cells_header->cells + (cells_header->index - 1);
				int pages_allocated = last_cell->offset / page_size;
				currentThread->pages_allocated = pages_allocated;

//	make sure the thread has access to these pages (at the beginning of the memory
				if (swap_out_and_assign(tid, pages_allocated) < 0) {
					ignore_timer_interrupt = 0;
					return NULL;
				}
			}

			debug_print("%d bytes allocated at offset: %d and address: %p\n", full_size, offset, memory);
			ignore_timer_interrupt = 0;
			return memory;
		} else {
			debug_print("free memory cell not found\n");
			ignore_timer_interrupt = 0;
			return NULL;
		}
	}
};

//	shared memory allocate
void* shalloc (size_t size) {
	print_who_was_called(__FUNCTION__);
	ignore_timer_interrupt = 1;
	int full_size = ((size + 7) / 8) * 8;
	memory_cells_header* cells_header = find_memory_cells_header_for_tid(0);
	cell_pointer_and_slot cell_ptr_and_slot = find_free_memory_cell(cells_header, full_size);
	memory_cell* cell = cell_ptr_and_slot.cell_ptr;
	if (cell != NULL) {
		debug_print("free shared memory cell found: %p, for tid: %d\n", cell, currentThread->tid);
		int avalible_size = cell->size * -1;
		cell->size = full_size;
		int slot = cell_ptr_and_slot.slot + 1;
		int new_offset = cell->offset + full_size;
		int new_size = (avalible_size - full_size) * -1;
		insert_memory_cell(cells_header, slot, new_offset, new_size);
		void* memory = &memory_block[cell->offset];
		debug_print("%d bytes allocated at offset: %d and address: %p, for tid: %d\n", full_size, cell->offset, memory, currentThread->tid);
		ignore_timer_interrupt = 0;
		return memory;
	} else {
		debug_print("free shared memory cell not found for tid: %d\n", currentThread->tid);
		ignore_timer_interrupt = 0;
		return NULL;
	}
};

//	Our free function
void mydeallocate (void* pointer, const char *filename, const int line_number, int req) {
	ignore_timer_interrupt = 1;
	print_who_was_called(__FUNCTION__);
	my_pthread_t tid;
	int size;
	if (req == LIBRARYREQ) {tid = 1;} else {tid = currentThread->tid;}
	memory_cells_header* cells_header = find_memory_cells_header_for_tid(tid);
//	free_memory_cell(cells_header, pointer);

	int h, i, j;
	for (i=0; i<cells_header->index; i++) {
		h = i - 1;
		j = i + 1;
		memory_cell* cell = cells_header->cells + i;
		char* memory = &memory_block[cell->offset];
		if (memory == pointer) {
			size = cell->size;
			cell->size = cell->size * -1;
			debug_print("%d bytes freed by: %s (tid: %d), at line: %d", size, malloc_caller[req], tid, line_number);
			debug_print(", bytes from offset: %d and address: %p\n", cell->offset, memory);
			if (h >= 0) {
				memory_cell* cell_h = cells_header->cells + h;
				if (cell_h->size < 0) {
					cell_h->size = cell_h->size + cell->size;
					remove_memory_cell(cells_header, i);
					debug_print("memory cell %d combined with cell above", i);
					cell = cells_header->cells + i;
					if (cell->size < 0) {
						cell_h->size = cell_h->size + cell->size;
						remove_memory_cell(cells_header, i);
						debug_print(" and below.");
					}
				}
			} else {
				memory_cell* cell_j = cells_header->cells + j;
				if (cell_j->size < 0) {
					cell_j->size = cell_j->size + cell->size;
					remove_memory_cell(cells_header, j);
					debug_print("memory cell %d combined with cell below", i);
				}
			}
			break;
		}
	}
	debug_print("\n");
	ignore_timer_interrupt = 0;
};


//	find free memory page
int find_free_memory_page (int page) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=(page + 1); i < (memory_page_count + swap_page_count); i++) {
		if ((memory_map + i)->tid == 0) {
			debug_print("free memory page found: %d\n", i);
			return i;
		}
	}
	debug_print("free memory page not found\n");
	return -1;
};

//	find tids memory page
int find_tids_memory_page (int tid) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=0; i < memory_page_count; i++) {
		if ((memory_map + i)->tid == tid) return i;
	}
	return -1;
};

//	Grow the supplied area that holds the memory_cells
void grow_memory_cells (struct memory_cells_header* mch_ptr) {
	print_who_was_called(__FUNCTION__);
	memory_cell* old = mch_ptr->cells;
	int current_slot_count = mch_ptr->slot_count;
	int new_slot_count = current_slot_count * 2;		//	Make the new area twice as big as the old
	int new_slot_size = new_slot_count * sizeof(memory_cell);
	memory_cell* new = (memory_cell*) myallocate(new_slot_size, __FILE__, __LINE__, LIBRARYREQ);
	int i;
	for (i=0; (i < current_slot_count); i++) {
		*(new + i) = *(old + i);			//	Copy the existing cell data
	}
	mch_ptr->cells = new;
	mch_ptr->slot_count = new_slot_count;
	mydeallocate((char*) old, __FILE__, __LINE__, LIBRARYREQ);
};

//	Create a growable array to hold memory_cells, for the tid, return a memory_cells_header
void create_memory_header (memory_cells_header* mch_ptr, my_pthread_t tid, int slot_count) {
	print_who_was_called(__FUNCTION__);
	mch_ptr->tid = tid;
	int total_cells_size = slot_count * sizeof(memory_cell);
	mch_ptr->cells = (memory_cell*) myallocate(total_cells_size, __FILE__, __LINE__, LIBRARYREQ);
	mch_ptr->index = 0;
	mch_ptr->slot_count = slot_count;
};

//	Create an array to hold memory_cells_header, return a memory_cells_header_array
//	each entry (a memory_cells_header) has an "array" that holds memory_cells that identify each area of memory
//	owned by the thread
memory_cells_header_array create_memory_header_array (int slot_count) {
	print_who_was_called(__FUNCTION__);
	memory_cells_header_array mchh;
	int msize = slot_count * sizeof(memory_cells_header);
	mchh.cells = (memory_cells_header*) myallocate(msize, __FILE__, __LINE__, LIBRARYREQ);
	mchh.index = 0;
	mchh.slot_count = slot_count;
	return mchh;
};

//	Add a memory_cells_header at the bottom of the cells
void add_memory_cells_header (my_pthread_t tid) {
	print_who_was_called(__FUNCTION__);
	int free_page;
	memory_cells_header* mch_ptr = memory_header_array.cells + memory_header_array.index;
	if (setting_up_memory_management == 1) {
		if (tid == 0) {
			free_page = memory_page_count - 4;
		} else {
			free_page = memory_page_count - library_starting_page_count;
		}
	} else {
		free_page = 0;
	}
	create_memory_header(mch_ptr, tid, 128);
	if (tid >= 2) {
//	add a memory cell that shows the whole memory minus the LIBRARY portion as free
		add_memory_cell(mch_ptr, 0, ((memory_page_count - library_starting_page_count) * page_size * -1));
	}

	(memory_map + free_page)->tid = tid;
	memory_header_array.index = memory_header_array.index + 1;
};

//	Create an array to hold threadControlBlocks, return a tcb_array_header
void create_tcb_array (int slot_count) {
	print_who_was_called(__FUNCTION__);
	int array_size = slot_count * sizeof(threadControlBlock);
	tcb_array.tcbs = (threadControlBlock*) myallocate(array_size, __FILE__, __LINE__, LIBRARYREQ);
	debug_print("tcb_array.tcbs: %p\n", tcb_array.tcbs);
	tcb_array.index = 0;
	tcb_array.slot_count = slot_count;
	debug_print("tcb_array.tcbs: %p\n", tcb_array.tcbs);
};

//	Add a tcb at the bottom of the tcbs
threadControlBlock* add_tcb () {
	print_who_was_called(__FUNCTION__);
//	Make sure we have room for the new cell
	if ((tcb_array.index + 1) < tcb_array.slot_count) {
		threadControlBlock* tcb_ptr = tcb_array.tcbs + tcb_array.index;
		debug_print("tcb_array.tcbs: %p, tcb_array.index: %d, tcb_ptr: %p\n", tcb_array.tcbs, tcb_array.index, tcb_ptr);
		tcb_array.index = tcb_array.index + 1;
		return tcb_ptr;
	} else {
		return NULL;
	}
};

//	Create an array to hold threadControlBlock pointers, return a tcb_pointer_array_header
tcb_pointer_array_header create_tcb_pointer_array (int slot_count) {
	print_who_was_called(__FUNCTION__);
	tcb_pointer_array_header tcbph;
	int array_size = slot_count * sizeof(threadControlBlock*);
	tcbph.tcbptrs = (threadControlBlock**) myallocate(array_size, __FILE__, __LINE__, LIBRARYREQ);
	tcbph.index = 0;
	tcbph.slot_count = slot_count;
	return tcbph;
};

//	Add a tcb pointer at the bottom of the tcb pointers
int add_tcb_pointer (struct tcb_pointer_array_header* tcbpah_ptr, threadControlBlock* tcb_ptr) {
	print_who_was_called(__FUNCTION__);
	int slot = tcbpah_ptr->index++;		//	this sets slot to index then adds 1 to index
	*(tcbpah_ptr->tcbptrs + slot) = tcb_ptr;
	return slot;
};

//	Adjust all the tcb pointers to the new tcb buffer
void adjust_tcb_pointers (struct tcb_pointer_array_header* tcbpah_ptr, int adjustment) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=0; (i<tcbpah_ptr->index); i++) {
		*(tcbpah_ptr->tcbptrs + i) = *(tcbpah_ptr->tcbptrs + i) + adjustment;
	}
};

//	Find next READY tcb, answer a tcb_pointer_and_slot
tcb_pointer_and_slot find_next_ready_tcb (struct tcb_pointer_array_header* tcbpah_ptr) {
	debug_print("---> %s called.\n", __FUNCTION__);
	tcb_pointer_and_slot ps;
	threadControlBlock* tcb_ptr;
	int i;
	for (i=0; i<tcbpah_ptr->index; i++) {
		tcb_ptr = *(tcbpah_ptr->tcbptrs + i);
		debug_print("tid: %d, status: %s\n", tcb_ptr->tid, statuses[tcb_ptr->status]);
		if (tcb_ptr->status == READY) {
			ps.tcb_ptr = tcb_ptr;
			ps.slot = i;
			return ps;
		}
	}
	ps.tcb_ptr = NULL;
	ps.slot = 0;
	return ps;
};

//	Find slot of the tcb pointer
int find_slot_of_tcb (struct tcb_pointer_array_header* tcbpah_ptr, threadControlBlock* tcb_ptr) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=0; i<tcbpah_ptr->index; i++) {
		if (*(tcbpah_ptr->tcbptrs + i) == tcb_ptr) {
			return i;
		}
	}
	return -1;
};

//	remove the tcb pointer from the slot
threadControlBlock* remove_tcb_pointer_from_slot (struct tcb_pointer_array_header* tcbpah_ptr, int slot) {
	print_who_was_called(__FUNCTION__);
	if (slot >= tcbpah_ptr->index) {
		return NULL;
	}
//	Move things up one slot to remove the pointer
	threadControlBlock* removed = *(tcbpah_ptr->tcbptrs + slot);
	int i, j;
	for (i=slot; (i<=tcbpah_ptr->index); i++) {
		j = i + 1;
		*(tcbpah_ptr->tcbptrs + i) = *(tcbpah_ptr->tcbptrs + j);
	}
	tcbpah_ptr->index = tcbpah_ptr->index - 1;
	return removed;
};

//	move the tcb pointer from the slot to the bottom
int move_tcb_pointer_from_slot_to_bottom (struct tcb_pointer_array_header* tcbpah_ptr, int slot) {
	print_who_was_called(__FUNCTION__);
	threadControlBlock* removed = remove_tcb_pointer_from_slot(tcbpah_ptr, slot);
	if (removed != NULL) {
		add_tcb_pointer(tcbpah_ptr, removed);
		return 0;
	}
	return -1;
};

//	up priority of currentThread
void up_priority_of_currentThread () {
	print_who_was_called(__FUNCTION__);
	if (currentThread->tid > 1) {
		int slot = find_slot_of_tcb(&priority_queues[priority_of_currentThread], currentThread);
		remove_tcb_pointer_from_slot(&priority_queues[priority_of_currentThread], slot);
		add_tcb_pointer(&priority_queues[0], currentThread);
	}
};

//	Create a growable array to hold my_pthread_mutex_t pointers, return a mutex_pointer_array_header
mutex_pointer_array_header create_mutex_pointer_array (int slot_count) {
	print_who_was_called(__FUNCTION__);
	mutex_pointer_array_header mutexph;
	int array_size = slot_count * sizeof(my_pthread_mutex_t*);
	mutexph.mutexptrs = (my_pthread_mutex_t**) myallocate(array_size, __FILE__, __LINE__, LIBRARYREQ);
	mutexph.index = 0;
	mutexph.slot_count = slot_count;
	return mutexph;
};

//	Grow the supplied area that holds the my_pthread_mutex_t pointers
void grow_mutex_pointer_array (struct mutex_pointer_array_header* mutexpah_ptr) {
	print_who_was_called(__FUNCTION__);
	my_pthread_mutex_t** old = mutexpah_ptr->mutexptrs;
	int current_slot_count = mutexpah_ptr->slot_count;
	int new_slot_count = current_slot_count * 2;		//	Make the new area twice as big as the old
	int array_size = new_slot_count * sizeof(my_pthread_mutex_t*);
	my_pthread_mutex_t** new = (my_pthread_mutex_t**) myallocate(array_size, __FILE__, __LINE__, LIBRARYREQ);
	int i;
	for (i=0; (i < current_slot_count); i++) {
		*(new + i) = *(old + i);			//	Copy the existing cell data
	}
	mutexpah_ptr->mutexptrs = new;
	mutexpah_ptr->slot_count = new_slot_count;
	mydeallocate((char*) old, __FILE__, __LINE__, LIBRARYREQ);
};

//	Add a mutex pointer at the bottom of the mutex pointers
int add_mutex_pointer (struct mutex_pointer_array_header* mutexpah_ptr, my_pthread_mutex_t* mutex_ptr) {
	print_who_was_called(__FUNCTION__);
//	Make sure we have room for the new cell
	if ((mutexpah_ptr->index + 1) >= mutexpah_ptr->slot_count) {
		grow_mutex_pointer_array(mutexpah_ptr);			//	grow our memory area if we get too close to the end
	}
	int slot = mutexpah_ptr->index++;		//	this sets slot to index then adds 1 to index
	*(mutexpah_ptr->mutexptrs + slot) = mutex_ptr;
	return slot;
};

//	Find slot of the mutex pointer
int find_slot_of_mutex (struct mutex_pointer_array_header* mutexpah_ptr, my_pthread_mutex_t* mutex_ptr) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=0; i<mutexpah_ptr->index; i++) {
		if (*(mutexpah_ptr->mutexptrs + i) == mutex_ptr) {
			return i;
		}
	}
	return -1;
};

//	remove the mutex pointer from the slot
my_pthread_mutex_t* remove_mutex_pointer_from_slot (struct mutex_pointer_array_header* mutexpah_ptr, int slot) {
	print_who_was_called(__FUNCTION__);
	if (slot >= mutexpah_ptr->index) {
		return NULL;
	}
//	Move things up one slot to remove the pointer
	my_pthread_mutex_t* removed = *(mutexpah_ptr->mutexptrs + slot);
	int i, j;
	for (i=slot; (i<=mutexpah_ptr->index); i++) {
		j = i + 1;
		*(mutexpah_ptr->mutexptrs + i) = *(mutexpah_ptr->mutexptrs + j);
	}
	mutexpah_ptr->index = mutexpah_ptr->index - 1;
	return removed;
};

//	Create a growable array to hold tids, return a tid_array_header
tid_array_header create_tid_array_header (int slot_count) {
	print_who_was_called(__FUNCTION__);
	tid_array_header tah;
	int array_size = slot_count * sizeof(my_pthread_t);
	tah.tids = (my_pthread_t*) myallocate(array_size, __FILE__, __LINE__, LIBRARYREQ);
	tah.index = 0;
	tah.slot_count = slot_count;
	return tah;
};

//	Grow the supplied area that holds the tids
void grow_tid_array (struct tid_array_header* tidh_ptr) {
	print_who_was_called(__FUNCTION__);
	my_pthread_t* old = tidh_ptr->tids;
	int current_slot_count = tidh_ptr->slot_count;
	int new_slot_count = current_slot_count * 2;		//	Make the new area twice as big as the old
	int array_size = new_slot_count * sizeof(my_pthread_t);
	my_pthread_t* new = (my_pthread_t*) myallocate(array_size, __FILE__, __LINE__, LIBRARYREQ);
	int i;
	for (i=0; (i < current_slot_count); i++) {
		*(new + i) = *(old + i);			//	Copy the existing cell data
	}
	tidh_ptr->tids = new;
	tidh_ptr->slot_count = new_slot_count;
	mydeallocate((char*) old, __FILE__, __LINE__, LIBRARYREQ);
};

//	Add a tid at the bottom of the tids
my_pthread_t add_tid (struct tid_array_header* tidh_ptr, my_pthread_t tid) {
	print_who_was_called(__FUNCTION__);
//	Make sure we have room for the new cell
	if ((tidh_ptr->index + 1) >= tidh_ptr->slot_count) {
		grow_tid_array(tidh_ptr);			//	grow our memory area if we get too close to the end
	}
	*(tidh_ptr->tids + tidh_ptr->index) = tid;
	tidh_ptr->index = tidh_ptr->index + 1;
	return tid;
};

//	find tcb with the given tid 
threadControlBlock* find_tcb_for_tid (my_pthread_t tid) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=0; i<tcb_array.index; i++) {
		threadControlBlock* tcb = tcb_array.tcbs + i;
		if (tcb->tid == tid) {
			return tcb;
		}
	}
	return NULL;
};

//	find tcb with joined_to_tid of the given tid 
threadControlBlock* find_tcb_for_joined_to_tid (my_pthread_t tid) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=0; i<tcb_array.index; i++) {
		threadControlBlock* tcb = tcb_array.tcbs + i;
		if (tcb->joined_to_tid == tid) {
			return tcb;
		}
	}
	return NULL;
};

//	ready the joined tcb
void ready_joined (threadControlBlock* joined_tcb_ptr, void * value_ptr) {
	debug_print("---> %s called.\n", __FUNCTION__);
	if (joined_tcb_ptr->status == JOIN_WAITING) {
		joined_tcb_ptr->status = READY;
		joined_tcb_ptr->return_code = value_ptr;
		joined_tcb_ptr->joined_to_tid = 0;
	}
};

//	ready all the joined tcbs to tcb
void ready_all_tcbs_joined_to_tcb (void * value_ptr) {
	debug_print("---> %s called.\n", __FUNCTION__);
	int i;
	for (i=0; i<tcb_array.index; i++) {
		threadControlBlock* tcb_ptr = tcb_array.tcbs + i;
		if (tcb_ptr->joined_to_tid == currentThread->tid) {
			ready_joined(tcb_ptr, value_ptr);
		}
	}
};

//	find tid slot
int find_tid_slot (struct tid_array_header* tidh_ptr, my_pthread_t tid) {
	print_who_was_called(__FUNCTION__);
	int i;
	for (i=0; i<tidh_ptr->index; i++) {
		if (*(tidh_ptr->tids + i) == tid) {
			return i;
		}
	}
	return -1;
};

//	remove tid at the given slot
my_pthread_t remove_tid (struct tid_array_header* tidh_ptr, int slot) {
	print_who_was_called(__FUNCTION__);
	if (tidh_ptr->index == 0) return 0;
	my_pthread_t removed = *(tidh_ptr->tids + slot);
	debug_print("tidh_ptr->index: %d, removed tid: %d\n", tidh_ptr->index, removed);
	int i, j;
	for (i=slot; (i<=tidh_ptr->index); i++) {
		j = i + 1;
		*(tidh_ptr->tids + i) = *(tidh_ptr->tids + j);
	}
	for (i=slot; (i<=tidh_ptr->index); i++) {
		debug_print("tid: %d\n", *(tidh_ptr->tids + i));
	}
	tidh_ptr->index = tidh_ptr->index - 1;
	debug_print("tidh_ptr->index: %d, removed tid: %d\n", tidh_ptr->index, removed);
	return removed;
};

//	***** initializeLibrary *****
void initializeLibrary () {
	initialize_called = 1;				//	we are now initializing
	print_who_was_called(__FUNCTION__);
	debug_print("setting_up_memory_management: %d, for memory_block at: %p\n", setting_up_memory_management, &memory_block);

//	open our swap file
	swap_file = open("swapfile.bin", (O_RDWR | O_TRUNC | O_CREAT), (S_IRUSR | S_IWUSR));
	if	(swap_file < 0) {
		printf("\n open() Error!!!\n");
		return;
	}
	debug_print("\n File opened successfully through open()\n");

//	before we can fully use our memory management system, we must set things up and do some things differently
	setting_up_memory_management = 1;
	debug_print("setting_up_memory_management: %d\n", setting_up_memory_management);

	page_size = sysconf(_SC_PAGE_SIZE);			//	ask the system its page size
//	calculate the number of pages in our memory and swap areas
	memory_page_count = sizeof(memory_block) / page_size;
	swap_page_count = memory_page_count * 2;

	debug_print("memory_page_count: %d\n", memory_page_count);

//	Setup our memory area controls

//	get a buffer to hold a page when we need to swap two
//	this should be the first entry in start_up_memory[][] of 4096 bytes
	swap_buffer = myallocate(page_size, __FILE__, __LINE__, LIBRARYREQ);

//	Make an array to hold who owns (tid_and_page) each slot of our memory map and the swap file
//	set them all to indicate not used (0) later we will set some aside for LIBRARYREQ (1)
//	this should be the second entry in start_up_memory[][] of 8192 bytes
	int msize = (memory_page_count + swap_page_count) * sizeof(tid_and_page);
	memory_map = (tid_and_page*) myallocate(msize, __FILE__, __LINE__, LIBRARYREQ);
	int i;
	for (i=0; i < (memory_page_count + swap_page_count); i++) {
		(memory_map + i)->tid = 0;		//	0 means the whole page is free
		(memory_map + i)->page = 0;		//	0 means the first page but since the page is free it is meaningless
	}

//	create our array of memory cells headers
//	this should be the third entry in start_up_memory[][] of 3072 bytes
	memory_header_array = create_memory_header_array(max_threads);
//	add an entry for LIBRARYREQ memory but don't set up the free memory area
	add_memory_cells_header(1);

//	set aside library_starting_page_count pages (see above) for LIBRARYREQ (1)
	for (i=(memory_page_count - library_starting_page_count); i < memory_page_count; i++) {
		(memory_map + i)->tid = 1;		//	1 means the LIBRARY owns the page
	}

//	We have allocated some memory for our use, now we need to make it look like all other allocated memory

	debug_print("About to fix up allocated memory\n");
	print_memory_cells_for_tid(1);

	memory_cells_header* cells_header = find_memory_cells_header_for_tid(1);
	debug_print("tid = %d, index = %d, slot count = %d\n", cells_header->tid, cells_header->index, cells_header->slot_count);
	debug_print("start_up_memory_index: %d\n", start_up_memory_index);
//	add memory cells for each piece of memory allocated and identified in start_up_memory[][]
	for (i=0; i<start_up_memory_index; i++) {
		debug_print("i = %d\n", i);
		debug_print("offset = %d\n", start_up_memory[i][0]);
		debug_print("size = %d\n", start_up_memory[i][1]);
		add_memory_cell(cells_header, start_up_memory[i][0], start_up_memory[i][1]);
	}

	print_memory_cells_for_tid(1);

//	add up how much memory we have allocated so far, it is identified in start_up_memory[][]
	int lib_used = 0;
	for (i=0; i<start_up_memory_index; i++) {
		lib_used = lib_used + start_up_memory[i][1];
	}

//	we set aside a block of library_starting_page_count (see above) pages, show the unused part as free
	int free_offset = start_up_memory[start_up_memory_index - 1][0] + start_up_memory[start_up_memory_index - 1][1];
	add_memory_cell(cells_header, free_offset, (((page_size * (library_starting_page_count - 4)) - lib_used) * -1));

	print_memory_cells_for_tid(1);

	setting_up_memory_management = 0;		//	memory management is done setting up
	debug_print("setting_up_memory_management: %d\n", setting_up_memory_management);
	debug_print("sizeof(threadControlBlock): %d\n", sizeof(threadControlBlock));

	create_tcb_array(max_threads);

	mutex_array = create_mutex_pointer_array(25);

//	initialize the multi-level thread queues
	for (i=0; i<3; i++) {
		priority_queues[i] = create_tcb_pointer_array(max_threads);
	}

//	create a threadControlBlock to hold the currently running main program
	mainThread = add_tcb();
	debug_print("mainThread: %p\n", mainThread);
	mainThread->return_code = NULL;
	getcontext(&mainThread->context);
	mainThread->tid = 2;
	mainThread->joined_to_tid = 0;
	mainThread->status = RUNNING;
	currentThread = mainThread;

	debug_print("context size: %d\n", sizeof(mainThread->context));

	add_memory_cells_header(mainThread->tid);
	add_tcb_pointer(&priority_queues[0], mainThread);

//	set up to catch seg faults
	seg_fault_action.sa_flags = SA_SIGINFO;
	seg_fault_action.sa_sigaction = segment_fault_handler;
	sigaction(SIGSEGV, &seg_fault_action, NULL);

//	attach signal handler interval timer
	interval_timer_action.sa_handler = &timer_event_handler;
	sigaction(SIGPROF, &interval_timer_action, NULL);

//	add an entry for shared memory but don't set up the free memory area
	add_memory_cells_header(0);
	cells_header = find_memory_cells_header_for_tid(0);
	free_offset = memory_page_count - 4;
	add_memory_cell(cells_header, free_offset, ((page_size * 4) * -1));
	debug_print("cells_header: %p\n", cells_header);
	print_memory_cells_for_tid(0);

	print_memory_cells_for_tid(2);

//	initialize it_interval struct	
	interval_timer.it_interval.tv_sec = 0;
	interval_timer.it_interval.tv_usec = 25000;
	interval_timer.it_value.tv_sec = 0;
	interval_timer.it_value.tv_usec = 25000;
	setitimer(ITIMER_PROF, &interval_timer, NULL);

};

//	catch the seg fault when a thread accesses its memory that was protected to make sure it is present
static void segment_fault_handler (int signum, siginfo_t *sig_info, void *unused) {
	ignore_timer_interrupt = 1;
	print_who_was_called(__FUNCTION__);
	int address_offset = ((char*)sig_info->si_addr) - &memory_block[0];
	int page_number = address_offset >> 12;
	char* page_addr = &memory_block[page_number * page_size];
	debug_print("segment fault at: %p, page_number: %d, page_addr: %p\n", sig_info->si_addr, page_number, page_addr);
	if ((page_number >= 0) && (page_number <= (memory_page_count + swap_page_count))) {
		tid_and_page* owner = memory_map + page_number;
		mprotect(page_addr, page_size, (PROT_READ | PROT_WRITE));	// remove protection from the memory page
		if (owner->tid != currentThread->tid) {	//	the current thread does not own the page so swap it
			int i;
			for (i=0; i < (memory_page_count + swap_page_count); i++) {
				if (((memory_map + i)->tid == currentThread->tid) && ((memory_map + i)->page == page_number)) {
					swap_page_in(i, page_number);
				}
			}
		}
		ignore_timer_interrupt = 0;
	} else {
		debug_print("segment fault at: %p\n", sig_info->si_addr);
		debug_print("total pages %d\npage_number %d\n", memory_page_count + swap_page_count, page_number);
		fflush(stdout);
		exit(-1);
	}
};

//	timer event handler [signal handler]
void timer_event_handler () {
	print_who_was_called(__FUNCTION__);
	my_pthread_t tid = currentThread->tid;
	if (ignore_timer_interrupt == 0) {		//	can't ignore timer interrupt so far
		if ((priority_of_currentThread == 0) ||
			((priority_of_currentThread == 1) && (interval_cnt >= 1)) || 
			((priority_of_currentThread == 2) && (interval_cnt >= 2))) {
				schedule();
		} else {
			interval_cnt++;
		}
	}
};

//	scheduler
void schedule () {
	ignore_timer_interrupt = 1;
	debug_print("schedule called, currentThread->tid: %d\n", currentThread->tid);
	int i, j;
	tcb_pointer_and_slot ready;
	tcb_pointer_array_header* queue_ptr;

	interval_cnt = 0;
//	look through all the priority_queues to find the first thread that is READY to run and run it
	oldThread = currentThread;
	for (i=0; i<3; i++) {				//	search the priority queues for a thread that is ready to run
		queue_ptr = &priority_queues[i];
		ready = find_next_ready_tcb(queue_ptr);		//	find who is ready in this queue
		if (ready.tcb_ptr != NULL) {							//	we found a ready thread
			remove_tcb_pointer_from_slot(queue_ptr, ready.slot);	//	remove him from this queue
			j = min(i + 1, 2);																		//	and add him to a lower priority queue
			add_tcb_pointer(&priority_queues[j], ready.tcb_ptr);	//	this lowers the priority one notch
			currentThread = ready.tcb_ptr;		//	currentThread is now the new thread
			currentThread->status = RUNNING;
			priority_of_currentThread = i;
			if (oldThread->status == RUNNING) {
				oldThread->status = READY;
			}
			break;
		}
	}
	if (currentThread != oldThread) {			//	this means we found ready thread
		mprotect(&memory_block, (memory_page_count * page_size), (PROT_READ | PROT_WRITE));	// remove protection from all the memory
		if (currentThread->tid > 1) {
			mprotect(&memory_block, ((currentThread->pages_allocated + 1) * page_size), PROT_NONE);
		}
		debug_print("oldThread->tid: %d, currentThread->tid: %d\n", oldThread->tid, currentThread->tid);
		debug_print("oldThread: %p, currentThread: %p\n", oldThread, currentThread);
		ignore_timer_interrupt = 0;
		swapcontext(&(oldThread->context), &(currentThread->context));
	}
};

//	create a new thread
int my_pthread_create (my_pthread_t * thread, pthread_attr_t * attr, void *(*function)(void*), void * arg) {
	ignore_timer_interrupt = 1;
	debug_print("---> %s called.\n", __FUNCTION__);
	if (!initialize_called) {
		initializeLibrary();
	}
	threadControlBlock* threadBlock = add_tcb();
	if (threadBlock) {
// for tid 0, 1 are used by library, tid 2 is assigned to main program, the first requested thread is tid 3
		threadBlock->tid = ++threadCount;
		threadBlock->status = READY;
		add_memory_cells_header(threadBlock->tid);
		oldThread = currentThread;
		currentThread = threadBlock;
		getcontext(&threadBlock->context);
		threadBlock->context.uc_stack.ss_sp = myallocate(stack_size, __FILE__, __LINE__, LIBRARYREQ);
		currentThread = oldThread;
		threadBlock->context.uc_stack.ss_size = stack_size;
//		threadBlock->context.uc_link = &threadBlock->context;
		threadBlock->context.uc_stack.ss_flags = 0;
//		makecontext(&threadBlock->context, (void (*)(void))function, 1, arg);
		makecontext(&threadBlock->context, (void (*)(void))thread_function, 2, function, arg);
		
		if (threadBlock->context.uc_stack.ss_sp == NULL) {
			fprintf(stderr, "error: malloc could not allocate the stack\n");
			ignore_timer_interrupt = 0;
			return -1;	
		}

		add_tcb_pointer(&priority_queues[0], threadBlock);
		*thread = threadBlock->tid;
		ignore_timer_interrupt = 0;
		return 0;
	} else {
		ignore_timer_interrupt = 0;
		return -1;
	}
};

//	give CPU possession to other user level threads voluntarily
int my_pthread_yield () {
	ignore_timer_interrupt = 1;
	debug_print("---> %s called.\n", __FUNCTION__);
	up_priority_of_currentThread();
	schedule();
	debug_print("---> leaving %s\n", __FUNCTION__);
	return 0;
};

//	thread function to call the function from my_pthread_create
void thread_function (void *(*x_function)(void*), void * x_arg) {
	debug_print("function: %p and arg: %d\n", x_function, x_arg);
	void* return_code = (*x_function)(x_arg);
	long rt = (long)return_code;
	debug_print("the thread ended, return_code: %d\n", rt);
	my_pthread_exit(return_code);
};

//	terminate a thread
void my_pthread_exit (void *value_ptr) {
	ignore_timer_interrupt = 1;
	debug_print("---> %s called for tid: %d\n", __FUNCTION__, currentThread->tid);
//	run our array of mutexes and unlock any that this thread has locked
	int i;
	for (i=0; i<mutex_array.index; i++) {
		my_pthread_mutex_t* mutex_ptr = *(mutex_array.mutexptrs + i);
		if (mutex_ptr->current_lock_holder == currentThread->tid) {
			my_pthread_mutex_unlock(mutex_ptr);
		}
	}

//	ready all threads joined to this thread
	ready_all_tcbs_joined_to_tcb(value_ptr);

//	make this thread done
	currentThread->status = DONE;
	currentThread->joined_to_tid = 0;
	currentThread->return_code = value_ptr;
	schedule();
	debug_print("---> leaving %s\n", __FUNCTION__);
};

//	wait for thread termination
int my_pthread_join (my_pthread_t thread, void **value_ptr) {
	ignore_timer_interrupt = 1;
	if (thread == currentThread->tid) {
		ignore_timer_interrupt = 0;
		return -1;
	}

	threadControlBlock* tcb = find_tcb_for_tid(thread);
	if (tcb != NULL) {
		if (tcb->status != DONE) {
			currentThread->status = JOIN_WAITING;
			currentThread->joined_to_tid = thread;
			debug_print("joined tid %d to tid %d\n",currentThread->tid, thread);
			up_priority_of_currentThread();
			schedule();

			debug_print("returned to join\n");
			if (value_ptr) {
				long rt = (long)currentThread->return_code;
				debug_print("rt in join: %d\n", rt);
				memmove(*value_ptr, &rt, 4);
			}
			debug_print("join after trying to set return value\n");

			debug_print("---> leaving %s\n", __FUNCTION__);
			return 0;
		} else {
			debug_print("didn't join tid %d to tid %d because target thread is done\n",currentThread->tid, thread);
			return 0;
		}
	}
	printf("join failed - thread %d does not exist\n", thread);
	ignore_timer_interrupt = 0;
	return -1;
};

//	remove first item waiting for the lock
threadControlBlock* mutex_lock_wait_pop (struct tid_array_header* tidh_ptr) {
	print_who_was_called(__FUNCTION__);
	my_pthread_t next_tid = remove_tid(tidh_ptr, 0);		//	get the tid of the next waiting thread
	if (next_tid == 0) {
		return NULL;
	} else {
		threadControlBlock* tcb = find_tcb_for_tid(next_tid);
//	this shouldn't happen but if it does we skip the thread so it isn't set to READY by our caller
		if (tcb->status == DONE) {
			return mutex_lock_wait_pop(tidh_ptr);
		} else {
			return tcb;
		}
	}
};	

//	initial the mutex lock
int my_pthread_mutex_init (my_pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) {
	ignore_timer_interrupt = 1;
	debug_print("---> %s called.\n", __FUNCTION__);
	add_mutex_pointer(&mutex_array, mutex);
	mutex->my_lock_wait_list = create_tid_array_header(10);
	mutex->current_lock_holder = 0;
	ignore_timer_interrupt = 0;
	return 0;
};

//	acquire the mutex lock
int my_pthread_mutex_lock (my_pthread_mutex_t *mutex) {
	ignore_timer_interrupt = 1;
	print_who_was_called(__FUNCTION__);
//	debug_print("lock-the_lock: %d\n", mutex->the_lock);
//	debug_print("tid: %d owns the lock\n", mutex->current_lock_holder);
	if (__sync_val_compare_and_swap(&mutex->the_lock, 0, 1) == 0) {
		mutex->current_lock_holder = currentThread->tid;
//		debug_print("setting the lock for tid: %d\n", currentThread->tid);
	} else {
		add_tid(&mutex->my_lock_wait_list, currentThread->tid);
		debug_print("setting to wait for lock for tid: %d\n", currentThread->tid);
		currentThread->status = MUTEX_WAITING;
		schedule();
	}
	ignore_timer_interrupt = 0;
	return 0;
};

//	release the mutex lock
int my_pthread_mutex_unlock (my_pthread_mutex_t *mutex) {
	ignore_timer_interrupt = 1;
	print_who_was_called(__FUNCTION__);
//	debug_print("unlock-the_lock: %d\n", mutex->the_lock);
//	get threadControlBlock of next thread waiting to get the lock.
	threadControlBlock* waiting_tcb_ptr = mutex_lock_wait_pop(&(mutex->my_lock_wait_list));
	if (waiting_tcb_ptr != NULL) {
		debug_print("waiting_tcb_ptr->tid: %d\n", waiting_tcb_ptr->tid);
//	we got a thread waiting to get a lock, so it takes over the lock.
		mutex->current_lock_holder = waiting_tcb_ptr->tid;
		waiting_tcb_ptr->status = READY;
	} else {
//	no thread was waiting to get a lock, so we unlock the lock.
		__sync_val_compare_and_swap(&mutex->the_lock, 1, 0);
	}
	ignore_timer_interrupt = 0;
	return 0;
};

//	destroy the mutex
int my_pthread_mutex_destroy (my_pthread_mutex_t *mutex) {
	ignore_timer_interrupt = 1;
	debug_print("---> %s called.\n", __FUNCTION__);
	int waiting_cnt = mutex->my_lock_wait_list.index;
	int i;
	for (i=0; i<waiting_cnt; i++) {		//	unlock everyone
		threadControlBlock* waiting_tcb = mutex_lock_wait_pop(&(mutex->my_lock_wait_list));
		waiting_tcb->status = READY;
	}
	__sync_val_compare_and_swap(&mutex->the_lock, 1, 0);		//	unlock the lock
	mydeallocate((char*) mutex->my_lock_wait_list.tids, __FILE__, __LINE__, LIBRARYREQ);
	int slot = find_slot_of_mutex(&mutex_array, mutex);
	remove_mutex_pointer_from_slot(&mutex_array, slot);
	ignore_timer_interrupt = 0;
	return 0;
};
