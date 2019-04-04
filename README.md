# My-pthread
Instead of using linked lists to hold the various information needed by the library to function, we 
have implemented what we call “growable arrays”.  They consist of two parts, a header and the 
memory.  The headers (acquired form the 8MB array) consist of a pointer to the memory for the 
growable array (also allocated from the 8MB array), an index to the next available slot in the 
array and a maximum number of slots in the array.  When the number of items in the array 
reaches the maximum, a new memory area is obtained at twice the current size and the items are 
copied to the new area.  The old area is freed.  Different arrays are cast to hold different types of 
data (integers, pointers or other structures).  There are functions to create the arrays, add items, 
remove items and grow the array.
We find use of these growable arrays simpler than dealing with linked lists.  Searching the array 
is accomplished with a simple for loop and an index is used as opposed to dealing with the next 
pointer and testing for the end with NULL or whatever.  We also don't have to acquire memory 
for each item in the array, as memory for many items is acquired once or when the array has to 
grow, which can be tuned to not be very often.
In this assignment we implemented a memory abstraction in the form of an 8MB array.  The 
8MB array is mapped by an array (allocated from within the 8MB array) of memory cells.  Each 
cell corresponds to a page in the 8MB memory array or the 16MB disk swap area.  Each cell 
contains the Id of the thread that owns the page and page offset from the beginning of the 8MBs. 
The page offset is where the page needs to be for a thread to use it.  Where the cell is within its 
own array represents where the memory page is within 8MB memory or in the 16MB disk swap 
file.  This scheme manages the memory for the library (which has a dedicated memory area, not 
paged) and the virtual memory (first 4MBs of the 8MBs) of the main program and the created 
threads (from now on both are just called threads) .  The threads virtual memory ALL start at the 
beginning of the 8MBs.  When the main program or a thread is dispatched the shared (virtual) 
memory area is protected and pages must be swapped in if the correct page isn't present.  Pages 
that are not in their needed position are housed in other available memory slots or in the swap 
page file.
There are other growable arrays that manage each threads allocated memory.  To start all 4MBs 
of the virtual memory area is considered free (available).  Entries in these arrays indicate the 
offset from the beginning of the 8MB array and the size of an allocated block.  A negative size 
means the block is now free (available).  As blocks are freed, adjacent free blocks are combined 
into larger free blocks.  When memory is requested, it is supplied from the first free block that 
can satisfy the request.
Our myallocate and mydeallocate functions emulate the malloc and free functions.  When 
myallocate is called, it searches for a free block, updates all metadata, and returns the pointer to 
it. Mydeallocate searches for a block by its pointer and upon finding it, updates all metadata to 
mark as free.  Because memory is allocated from the virtual memory area, myallocate will not 
return NULL (error) unless it has allocated all pages of the virtual memory area.
Finally, in phase D we specifically allocate the last four pages of our array to be “shared 
memory”.  This means that no thread has ownership and the memory is not protected from 
outside threads.  This is used to pass references between threads.  Shalloc was implemented as a 
simplified version of myallocate with less protection.
