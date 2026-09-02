// malloc.c: the ring-3 heap, ported from kernel/heap.c, which was itself ported
// from the CMSC216 p5 el_malloc (an explicit free-list allocator with boundary
// tags and coalescing).
//
// THIS IS THE SAME CODE AS kernel/heap.c, ON PURPOSE. Porting an allocator is the
// second-cheapest way to get one, and this is the second port of the same one:
// chapter 13 moved p5's el_malloc into the kernel and changed only where it got
// memory from; this file moves it into ring 3 and changes only that again. The
// block layout, the two lists, the first-fit search, the split, the coalesce and
// every function name are unchanged, so that a bug found in one file can be
// looked for in the other by name, and so the two can be diffed. Keep it that way:
// a fix that lands in one copy and not the other is the failure this comment
// exists to prevent. The matching comment is at the top of kernel/heap.c.
//
// What differs from the kernel copy, all forced by running in ring 3 rather than
// in the kernel, and each marked at the site:
//   1. The slab comes from SYS_MMAP, not alloc_frames_contiguous, and lives at
//      whatever address the kernel returns: the bottom of the heap slot for the
//      first slab and upwards from there. Growth checks that the new slab is
//      ADJACENT to the old one, exactly as the kernel copy does, and gives a
//      non-adjacent slab straight back with SYS_MUNMAP rather than splice it in.
//      See el_append_pages_to_heap for why that check is the whole safety of the
//      walk, and the note on multiple slabs.
//   2. There is no interrupt guard. The kernel copy wraps kmalloc/kfree in
//      irq_save/irq_restore because a timer interrupt could call kmalloc in the
//      middle of a relink; ring 3 cannot mask interrupts and does not need to,
//      because preemption switches to another task with its own heap, never back
//      into this program's lists. The hazard that DOES exist is a signal handler
//      calling malloc while malloc is half way through a relink. Nothing here
//      prevents it: malloc is not async-signal-safe, and a handler must not call it.
//   3. No print_string: the one error message goes through sys_print. The stats
//      and debug printers (heap_print_stats, el_print_*) and heap_used_bytes were
//      not ported; they need printf, which lives beside this file and must not be
//      something the allocator depends on (see M6 below).
//   4. free(NULL) is a no-op, as C requires. The p5 el_free treats NULL as an error
//      and the kernel copy kept that; here the public wrapper filters it and the
//      el_free core is unchanged.
//   5. The public malloc rounds each request up to a multiple of 8, so every
//      payload is 8-byte aligned (the slab is page aligned and the block overhead
//      is a multiple of 8). The p5 core and kmalloc round nothing and hand out
//      whatever alignment the previous sizes left. 8, not the 16 that C's
//      max_align_t asks for: enough for every integer and pointer store, not for
//      aligned SSE loads. Recorded in docs/decisions/0024.
//   6. M6, THE FIRST CALL. malloc initialises the heap itself on its first call,
//      from a static `initialised` flag, rather than from an init function a
//      program has to remember to call. The slab path below touches NOTHING that
//      allocates: the first block's header and footer are written directly, and
//      the flag is set before the block is built, not after. See malloc().
//
// Slabs are NEVER returned to the kernel. free puts a block back on the
// available list; nothing ever calls SYS_MUNMAP on a slab, so a program's peak
// heap is held until it exits, when paging_destroy_address_space frees the whole
// slot. See docs/reference/user-memory.md.

#include "../user/userlib.h"   // sys_mmap, sys_munmap, sys_print, and the malloc/free/calloc prototypes
#include "mem.h"               // memset, for calloc

// ============================================================================
// Pointer arithmetic macros (verbatim from the p5 source).
// ============================================================================
// macro to add a byte offset to a pointer, arguments are a pointer
// and a # of bytes (usually size_t)
#define PTR_PLUS_BYTES(ptr,off) ((void *) (((size_t) (ptr)) + ((size_t) (off))))

// macro to subtract a byte offset from a pointer, arguments are a pointer
// and a # of bytes (usually size_t)
#define PTR_MINUS_BYTES(ptr,off) ((void *) (((size_t) (ptr)) - ((size_t) (off))))

// The heap page size is the kernel's mapping granularity: SYS_MMAP rounds every
// request up to it, so a slab is always whole pages.
#define EL_PAGE_BYTES USER_PAGE_SIZE

// Initial slab (16 pages = 64KB, the kernel heap's figure) and the default growth
// chunk. The heap grows on demand when a request does not fit; growth is at least
// this many pages. Each growth is one SYS_MMAP region, and a task may hold at most
// MAX_REGIONS (8) of them, so with this chunk the heap can grow seven times: 512KB
// in all, unless a single request forces a larger chunk. That cap is a consequence
// of the kernel's fixed region array and is recorded in docs/decisions/0024.
#define HEAP_INITIAL_PAGES 16
#define HEAP_GROW_PAGES    16

// What SYS_MMAP returns when it refuses.
#define MMAP_FAILED ((void *)(unsigned long)-1)

// defines to indicate if a block is available or used
#define EL_AVAILABLE     'a'    // block state indicating available
#define EL_USED          'u'    // block state indicating in use
#define EL_BEGIN_BLOCK   'B'    // block state indicating dummy beginning node in a list
#define EL_END_BLOCK     'E'    // block state indicating dummy ending node in a list
#define EL_UNINITIALIZED  0     // indication of uninitialized data

typedef struct block {
  size_t size;                  // number of bytes of memory in this block
  char state;                   // either EL_AVAILABLE or EL_USED
  struct block *next;           // pointer to next block in same list
  struct block *prev;           // pointer to previous block in same list
} el_blockhead_t;

typedef struct {
  size_t size;
} el_blockfoot_t;

#define EL_BLOCK_OVERHEAD (sizeof(el_blockhead_t) + sizeof(el_blockfoot_t))

typedef struct {
  el_blockhead_t beg_actual;    // fixed node at beginning of list; state is EL_BEGIN_BLOCK
  el_blockhead_t end_actual;    // fixed node at end of list; state is EL_END_BLOCK
  el_blockhead_t *beg;          // pointer to beg_actual
  el_blockhead_t *end;          // pointer to end_actual
  size_t length;                // length of the used block list (not counting beg/end)
  size_t bytes;                 // total bytes in list used including overhead;
} el_blocklist_t;

typedef struct {
  void *heap_start;             // pointer to where the heap starts
  void *heap_end;               // pointer to where the heap ends; this memory address is out of bounds
  size_t heap_bytes;            // number of bytes currently in the heap
  el_blocklist_t avail_actual;  // space for the available list data
  el_blocklist_t used_actual;   // space for the used list data
  el_blocklist_t *avail;        // pointer to avail_actual
  el_blocklist_t *used;         // pointer to used_actual
} el_ctl_t;

// The control block is a fixed-size struct: no reason to allocate it, and (M6)
// every reason not to. In the p5 source it was mmap'd at a fixed address; here, as
// in the kernel, it lives in .bss (zero-init) and el_ctl points at it, so all the
// el_ctl-> code below comes over untouched.
static el_ctl_t el_ctl_actual;
static el_ctl_t *el_ctl = &el_ctl_actual;

// Has the first slab been built? Set INSIDE the slab path, before the first block
// is written, and never cleared. See malloc() for why the order matters (M6).
static int initialised = 0;

// ============================================================================
// Boundary-tag address arithmetic (verbatim from the p5 source).
// ============================================================================

el_blockfoot_t *el_get_footer(el_blockhead_t *head){
  size_t size = head->size;
  el_blockfoot_t *foot = PTR_PLUS_BYTES(head, sizeof(el_blockhead_t) + size);
  return foot;
}

el_blockhead_t *el_get_header(el_blockfoot_t *foot){ // move backward in memory from the foot to reach the head.
  el_blockhead_t *head = (el_blockhead_t *) PTR_MINUS_BYTES(foot, sizeof(el_blockhead_t) + (foot->size));
  return head;
}

el_blockhead_t *el_block_above(el_blockhead_t *block){
  el_blockhead_t *higher = PTR_PLUS_BYTES(block, block->size + EL_BLOCK_OVERHEAD);
  if((void *) higher >= (void*) el_ctl->heap_end){
    return NULL;
  }
  else{
    return higher;
  }
}

el_blockhead_t *el_block_below(el_blockhead_t *block){
  if((void *)block <= el_ctl->heap_start){ //check if block at start
    return NULL;
  }
  el_blockfoot_t *footer = (el_blockfoot_t *) PTR_MINUS_BYTES(block, sizeof(el_blockfoot_t)); // gets footer of previous
  el_blockhead_t *lower = el_get_header(footer); //get header from footer
  return lower;
}

// ============================================================================
// Doubly-linked list surgery (verbatim from the p5 source).
// ============================================================================

void el_init_blocklist(el_blocklist_t *list){
  list->beg        = &(list->beg_actual);
  list->beg->state = EL_BEGIN_BLOCK;
  list->beg->size  = EL_UNINITIALIZED;
  list->end        = &(list->end_actual);
  list->end->state = EL_END_BLOCK;
  list->end->size  = EL_UNINITIALIZED;
  list->beg->next  = list->end;
  list->beg->prev  = NULL;
  list->end->next  = NULL;
  list->end->prev  = list->beg;
  list->length     = 0;
  list->bytes      = 0;
}

void el_add_block_front(el_blocklist_t *list, el_blockhead_t *block){
  el_blockhead_t *start = list->beg;
  el_blockhead_t *first = start->next;
  block->next = first; // First, attach the new block to the existing first block
  first->prev = block;
  block->prev = start;   // Then connect new block back to start
  start->next = block;
  list->length += 1; // Update list metadata
  list->bytes  += block->size + EL_BLOCK_OVERHEAD;
}

void el_remove_block(el_blocklist_t *list, el_blockhead_t *block){
  el_blockhead_t *prev = block->prev; // get the blocks it is connected to
  el_blockhead_t *next = block->next;
  prev->next = next; // change the ones next to the block we try to remove
  next->prev = prev;
  list->length-=1;  // update metadata
  list->bytes -= block->size + EL_BLOCK_OVERHEAD;
  block->next = NULL; // get rid of the connections
  block->prev = NULL;
}

// ============================================================================
// Allocation: first-fit search, split, mark used (verbatim core from p5).
// ============================================================================

el_blockhead_t *el_find_first_avail(size_t size){
  el_blockhead_t *f = el_ctl->avail->beg->next;
  while (f->state != EL_END_BLOCK) { // look through the blocks to find the first open one block
    if (f->state == EL_AVAILABLE) { // if it is open, you check size
      if(f->size>=size){ // if size matches, you have found your guy
        return f;
      }
    }
    f = f->next;
  }
  return NULL;
}

el_blockhead_t *el_split_block(el_blockhead_t *block, size_t new_size){
  size_t olds = block->size;
  el_blockfoot_t *oldf = el_get_footer(block);
  if (olds < new_size + EL_BLOCK_OVERHEAD){ //size check to see if we have enough space
    return NULL;
  }
  size_t left = olds - new_size - EL_BLOCK_OVERHEAD;
  block->size = new_size;
  el_blockfoot_t *shrunkf = el_get_footer(block); //creates footer for shrunk block
  shrunkf->size = new_size;
  oldf->size = left; // just make the old footer the one for the new one by updating size for new
  el_blockhead_t *newb = el_get_header(oldf);
  newb->size = left;
  return newb;
}

void *el_malloc(size_t nbytes){
  el_blockhead_t *b = el_find_first_avail(nbytes); // find the first available block that is large enough.
  if (b==NULL){ // check if found
    return NULL;
  }
  el_remove_block(el_ctl->avail, b); // get rid of it from available
  el_blockhead_t *newb = el_split_block(b, nbytes);
  b->state = EL_USED; // change to in use since it will hold parameter
  el_add_block_front(el_ctl->used, b);
  if (newb != NULL) { // the bit of memory not used is created into new block and added to available
    newb->state = EL_AVAILABLE;
    el_add_block_front(el_ctl->avail, newb);
  }
  return PTR_PLUS_BYTES(b, sizeof(el_blockhead_t));
}

// ============================================================================
// Free and coalesce (verbatim core from p5; only the error message is native).
// ============================================================================

void el_merge_block_with_above(el_blockhead_t *lower){
  if(lower == NULL){ // all the lower value checks
    return;
  }
  if(lower->state != EL_AVAILABLE){
    return;
  }
  el_blockhead_t *up = el_block_above(lower); // gets the block above lower
  if(up == NULL){ // all the checks needed for that up one
    return;
  }
  if(up->state != EL_AVAILABLE){
    return;
  }
  el_remove_block(el_ctl->avail, lower); // getting on with the merging
  el_remove_block(el_ctl->avail, up);
  lower->size = lower->size + EL_BLOCK_OVERHEAD + up->size;
  el_blockfoot_t *newf = el_get_footer(lower);
  newf->size = lower->size;
  el_add_block_front(el_ctl->avail, lower);
}

void el_free(void *ptr){
  if (ptr == NULL){ //inavlid pointer check
    sys_print("ERROR: el_free() not called on an EL_USED block\n");
    return;
  }
  el_blockhead_t *block = (el_blockhead_t *) PTR_MINUS_BYTES(ptr, sizeof(el_blockhead_t)); //comvert to block pointer
  if (block->state != EL_USED||block->state=='\0') { //check if it is used
    sys_print("ERROR: el_free() not called on an EL_USED block\n");
    return;
  }
  el_remove_block(el_ctl->used, block);
  block->state = EL_AVAILABLE; // mark as available
  el_add_block_front(el_ctl->avail, block);
  el_merge_block_with_above(block); //try to merge whatever is near it that is available
  el_blockhead_t *lower = el_block_below(block);
  el_merge_block_with_above(lower);
}

// ============================================================================
// Slab construction and growth: alloc_frames_contiguous becomes SYS_MMAP.
// ============================================================================

// Build the initial slab and its single free block. The p5 source mmap'd a page
// for el_ctl and a slab at a fixed address; the kernel copy took a contiguous run
// of frames; here the slab is whatever region SYS_MMAP returns, which for the
// first call of a program is the bottom of the heap slot.
//
// NOTHING ON THIS PATH ALLOCATES, AND NOTHING MAY BE ADDED THAT DOES (M6). This is
// the first malloc of the program, so the lists do not exist yet: a call to malloc,
// calloc, or anything built on them from in here would find `initialised` set (see
// below), search a list that has not been built, and read through a NULL avail
// pointer, or, with the flag set later, recurse into this function forever until
// the stack ran off the bottom of PD[3]. The first block's header and footer are
// therefore written directly, field by field, and the lists are initialised by
// direct assignment.
static int el_init(size_t initial_heap_size){
  void *heap = (void *) sys_mmap(initial_heap_size);
  if(heap == MMAP_FAILED){
    sys_print("malloc: could not get the initial slab from SYS_MMAP\n");
    return 1;
  }

  // Set the flag HERE, before the first block is built, not after. Once the slab
  // exists this is a heap, and every later entry into malloc must see it as one
  // rather than start building a second first slab on top of the first.
  initialised = 1;

  el_ctl->heap_bytes = initial_heap_size;    // make the heap as big as possible to begin with
  el_ctl->heap_start = heap;                 // set addresses of start and end of heap
  el_ctl->heap_end   = PTR_PLUS_BYTES(heap,el_ctl->heap_bytes);

  if(el_ctl->heap_bytes < EL_BLOCK_OVERHEAD){
    sys_print("malloc: heap size too small for a block overhead\n");
    return 1;
  }

  el_init_blocklist(&el_ctl->avail_actual);
  el_init_blocklist(&el_ctl->used_actual);
  el_ctl->avail = &el_ctl->avail_actual;
  el_ctl->used  = &el_ctl->used_actual;

  size_t size = el_ctl->heap_bytes - EL_BLOCK_OVERHEAD;
  el_blockhead_t *ablock = el_ctl->heap_start;
  ablock->size = size;
  ablock->state = EL_AVAILABLE;
  el_blockfoot_t *afoot = el_get_footer(ablock);
  afoot->size = size;

  ablock->prev = el_ctl->avail->beg;
  ablock->next = el_ctl->avail->beg->next;
  ablock->prev->next = ablock;
  ablock->next->prev = ablock;
  el_ctl->avail->length++;
  el_ctl->avail->bytes += (ablock->size + EL_BLOCK_OVERHEAD);

  return 0;
}

int el_append_pages_to_heap(int npages) {
  size_t nbytes = (size_t) npages * EL_PAGE_BYTES;
  void *mapped = (void *) sys_mmap(nbytes); // grow the slab
  if(mapped == MMAP_FAILED){     // Fail if the kernel refused: out of frames, out of region slots, or the 2MB ceiling
    sys_print("malloc: heap grow: SYS_MMAP refused another slab\n");
    return 1;
  }
  // THE PORTED ALLOCATOR ASSUMES ONE CONTIGUOUS SPAN, and this check is what makes
  // that assumption safe on top of SYS_MMAP. el_block_above and el_block_below walk
  // memory linearly and are bounded by a single heap_start/heap_end pair, so
  // heap_end must be the literal next byte of the new slab. The kernel copy could
  // rely on the frame allocator's bottom-up scan to make a growth run adjacent;
  // SYS_MMAP promises no such thing in general, only that a region is placed at the
  // lowest address above every region the program holds. When malloc is the only
  // thing in the program calling SYS_MMAP that address IS heap_end, so growth works
  // and the heap stays one span; if the program has mapped something of its own in
  // between, the new slab lands above it and is NOT adjacent. Splicing a disjoint
  // slab into the single-span walk would let el_block_above step off the end of
  // the old slab into whatever sits in the gap (unmapped pages, or the program's
  // own region) and read garbage as a block header, so the slab is given straight
  // back and the growth fails. malloc then returns NULL. That is the "keep one span
  // and fail" choice from docs/decisions/0024, with the failure made loud.
  if(mapped != el_ctl->heap_end){
    sys_print("malloc: heap grow: non-adjacent slab, refusing to fragment heap\n");
    sys_munmap((unsigned long) mapped, nbytes);
    return 1;
  }
  el_blockhead_t *b = (el_blockhead_t *) el_ctl->heap_end;
  size_t size = nbytes - EL_BLOCK_OVERHEAD;
  el_blockfoot_t *f = (el_blockfoot_t *) PTR_PLUS_BYTES(b, sizeof(el_blockhead_t) + size); // Compute footer address at the end of the block
  b->size  = size;
  f->size = size;
  b->state = EL_AVAILABLE;   // intialize all values
  b->prev  = NULL;
  b->next  = NULL;
  el_ctl->heap_bytes += nbytes;
  el_ctl->heap_end = PTR_PLUS_BYTES(el_ctl->heap_end, nbytes);
  el_add_block_front(el_ctl->avail, b);   // Add the new free block to the available list
  el_blockhead_t *below = el_block_below(b);
  el_merge_block_with_above(below);
  return 0;
}

// ============================================================================
// Public interface: malloc/free/calloc.
// ============================================================================

// malloc: allocate `size` bytes. If no block fits, grow the slab and retry. The
// el_malloc core is unchanged; the first-call initialisation, the rounding, and
// the grow-and-retry policy live here in the wrapper so el_malloc stays the pure
// p5 algorithm, exactly as kmalloc's wrapper does in the kernel.
void *malloc(size_t size){
  if(!initialised){
    // M6. The first malloc of a program builds the heap. el_init sets the flag
    // itself, before it writes the first block, and touches nothing that
    // allocates; see the comment on it. If it fails (SYS_MMAP refused) the flag
    // stays clear, so the next call tries again rather than walking lists that
    // were never built.
    if(el_init((size_t) HEAP_INITIAL_PAGES * EL_PAGE_BYTES) != 0){
      return NULL;
    }
  }

  // Round up to a multiple of 8 so the payload is 8-byte aligned (difference 5
  // above). A request of 0 becomes a zero-sized block, which the core handles.
  size = (size + 7) & ~(size_t)7;

  void *p = el_malloc(size);
  if(p == NULL){
    // No block large enough. Grow by at least HEAP_GROW_PAGES, or more if the
    // request itself needs more than that, then try once more.
    size_t need = size + EL_BLOCK_OVERHEAD;
    int npages = (int)((need + EL_PAGE_BYTES - 1) / EL_PAGE_BYTES);
    if(npages < HEAP_GROW_PAGES){
      npages = HEAP_GROW_PAGES;
    }
    if(el_append_pages_to_heap(npages) == 0){
      p = el_malloc(size);
    }
  }
  return p;
}

void free(void *ptr){
  if(ptr == NULL){
    return;   // C's free(NULL) is a no-op (difference 4 above)
  }
  el_free(ptr);
}

// calloc: `count` objects of `size` bytes each, zeroed. The multiplication is
// checked for overflow first, because a wrapped product is a small request that
// hands back far less memory than the caller then writes into.
void *calloc(size_t count, size_t size){
  if(count != 0 && size > ((size_t)-1) / count){
    return NULL;
  }
  size_t total = count * size;
  void *p = malloc(total);
  if(p != NULL){
    // Always cleared, even though a fresh slab arrives zeroed from the kernel: a
    // block being REUSED after a free holds whatever its last owner wrote.
    memset(p, 0, total);
  }
  return p;
}
