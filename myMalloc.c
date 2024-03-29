#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "myMalloc.h"
#include "printing.h"

/* Due to the way assert() prints error messges we use out own assert function
 * for deteminism when testing assertions
 */
#ifdef TEST_ASSERT
  inline static void assert(int e) {
    if (!e) {
      const char * msg = "Assertion Failed!\n";
      write(2, msg, strlen(msg));
      exit(1);
    }
  }
#else
  #include <assert.h>
#endif

/*
 * Mutex to ensure thread safety for the freelist
 */
static pthread_mutex_t mutex;

/*
 * Array of sentinel nodes for the freelists
 */
header freelistSentinels[N_LISTS];

/*
 * Pointer to the second fencepost in the most recently allocated chunk from
 * the OS. Used for coalescing chunks
 */
header * lastFencePost;

/*
 * Pointer to maintian the base of the heap to allow printing based on the
 * distance from the base of the heap
 */ 
void * base;

/*
 * List of chunks allocated by  the OS for printing boundary tags
 */
header * osChunkList [MAX_OS_CHUNKS];
size_t numOsChunks = 0;

/*
 * direct the compiler to run the init function before running main
 * this allows initialization of required globals
 */
static void init (void) __attribute__ ((constructor));

// Helper functions for manipulating pointers to headers
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off);
static inline header * get_left_header(header * h);
static inline header * ptr_to_header(void * p);

// Helper functions for allocating more memory from the OS
static inline void initialize_fencepost(header * fp, size_t left_size);
static inline void insert_os_chunk(header * hdr);
static inline void insert_fenceposts(void * raw_mem, size_t size);
static header * allocate_chunk(size_t size);

// Helper functions for freeing a block
static inline void deallocate_object(void * p);

// Helper functions for allocating a block
static inline header * allocate_object(size_t raw_size);

// Helper functions for verifying that the data structures are structurally 
// valid
static inline header * detect_cycles();
static inline header * verify_pointers();
static inline bool verify_freelist();
static inline header * verify_chunk(header * chunk);
static inline bool verify_tags();

static void init();

static bool isMallocInitialized;

/**
 * @brief Helper function to retrieve a header pointer from a pointer and an 
 *        offset
 *
 * @param ptr base pointer
 * @param off number of bytes from base pointer where header is located
 *
 * @return a pointer to a header offset bytes from pointer
 */
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off) {
	return (header *)((char *) ptr + off);
}

/**
 * @brief Helper function to get the header to the :spring20:startright of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
header * get_right_header(header * h) {
	return get_header_from_offset(h, get_block_size(h));
}

/**
 * @brief Helper function to get the header to the left of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
inline static header * get_left_header(header * h) {
  return get_header_from_offset(h, -h->left_size);
}

/**
 * @brief Fenceposts are marked as always allocated and may need to have
 * a left object size to ensure coalescing happens properly
 *
 * @param fp a pointer to the header being used as a fencepost
 * @param left_size the size of the object to the left of the fencepost
 */
inline static void initialize_fencepost(header * fp, size_t left_size) {
	set_block_state(fp,FENCEPOST);
	set_block_size(fp, ALLOC_HEADER_SIZE);
	fp->left_size = left_size;
}

/**
 * @brief Helper function to maintain list of chunks from the OS for debugging
 *
 * @param hdr the first fencepost in the chunk allocated by the OS
 */
inline static void insert_os_chunk(header * hdr) {
  if (numOsChunks < MAX_OS_CHUNKS) {
    osChunkList[numOsChunks++] = hdr;
  }
}

/**
 * @brief given a chunk of memory insert fenceposts at the left and 
 * right boundaries of the block to prevent coalescing outside of the
 * block
 *
 * @param raw_mem a void pointer to the memory chunk to initialize
 * @param size the size of the allocated chunk
 */
inline static void insert_fenceposts(void * raw_mem, size_t size) {
  // Convert to char * before performing operations
  char * mem = (char *) raw_mem;

  // Insert a fencepost at the left edge of the block
  header * leftFencePost = (header *) mem;
  initialize_fencepost(leftFencePost, ALLOC_HEADER_SIZE);

  // Insert a fencepost at the right edge of the block
  header * rightFencePost = get_header_from_offset(mem, size - ALLOC_HEADER_SIZE);
  initialize_fencepost(rightFencePost, size - 2 * ALLOC_HEADER_SIZE);
}

/**
 * @brief Allocate another chunk from the OS and prepare to insert it
 * into the free list
 *
 * @param size The size to allocate from the OS
 *
 * @return A pointer to the allocable block in the chunk (just after the 
 * first fencpost)
 */
static header * allocate_chunk(size_t size) {
  void * mem = sbrk(size);

  insert_fenceposts(mem, size);
  header * hdr = (header *) ((char *)mem + ALLOC_HEADER_SIZE);
  set_block_state(hdr, UNALLOCATED);
  set_block_size(hdr, size - 2 * ALLOC_HEADER_SIZE);
  hdr->left_size = ALLOC_HEADER_SIZE;
  return hdr;
}

/**
 * @brief Helper allocate an object given a raw request size from the user
 *
 * @param raw_size number of bytes the user needs
 *
 * @return A block satisfying the user's request
 */
static inline header * allocate_object(size_t raw_size) {
  // TODO implement allocation
  if (raw_size <= 0) {
    return NULL;
  }
  if (raw_size <= 8) {
    raw_size = 16;
  }
  //converting raw size to block size
  int size = raw_size;
  if (size % 8 != 0) {
    size = size + (8 - raw_size%8);
  }
  size = size + ALLOC_HEADER_SIZE;
  //fprintf(stderr, "Size: %d\n", size);

  header * hdr = NULL;
  header * oldNext = NULL;
  header * oldPrev = NULL;


  //finding location for smallest possible in freelist
  int smallest = (size - ALLOC_HEADER_SIZE)/8 - 1;
  if (smallest >= N_LISTS) {
    smallest = N_LISTS - 1;
  }

  //iterate through free list to find open location
  for (int i = smallest; i < N_LISTS; i++) {
    if (freelistSentinels[i].prev != &freelistSentinels[i]) {
      hdr = freelistSentinels[i].next;
      if (i == N_LISTS - 1) {
        //Need to find a block large enough if in last list
        while (get_block_size(hdr) < size) {
          hdr = hdr->next;
          if (hdr == &freelistSentinels[i]) {
            hdr = NULL;
            break;
          }
        }
      }
      //setting pointers to remove from free list
      hdr->next->prev = hdr->prev;
      hdr->prev->next = hdr->next;

      //keep track of old pointers incase needs to be added back in same list
      oldPrev = hdr->prev;
      oldNext = hdr->next;
      hdr->prev = NULL;
      hdr->next = NULL;
      break;
    }
  }

  //when there is no block big enough
  if (hdr == NULL) {
    //allocating a new chunk
    hdr = allocate_chunk(ARENA_SIZE);
    header * rightPost = get_right_header(hdr);
    header * leftPost = get_left_header(hdr);

    //check if the blocks are next to each other
    if ((header *)((char *) leftPost - ALLOC_HEADER_SIZE) == lastFencePost) {
      header * temp = lastFencePost;
      header * lastBlock = get_left_header(temp);

      //if last block in old chunk is free
      if (get_block_state(lastBlock) == (enum state) 0) {
        set_block_size(lastBlock, get_block_size(lastBlock) + 2 * ALLOC_HEADER_SIZE + get_block_size(hdr));
        lastBlock->next->prev = lastBlock->prev;
        lastBlock->prev->next = lastBlock->next;
        hdr = lastBlock;
      } else { //otherwise just remove the fences
        set_block_state(temp, (enum state) 0);
        set_block_size(temp, get_block_size(hdr) + 2 * ALLOC_HEADER_SIZE);
        hdr = temp;
      }
      rightPost->left_size = get_block_size(hdr);
    } else { //if not next to each other, add to osChunkList
      osChunkList[numOsChunks] = leftPost;
      numOsChunks++;
    }
    //add new chunk to freelist
    freelistSentinels[N_LISTS - 1].next->prev = hdr;
    hdr->next = freelistSentinels[N_LISTS - 1].next;
    freelistSentinels[N_LISTS - 1].next = hdr;
    hdr->prev = &freelistSentinels[N_LISTS - 1];
    lastFencePost = rightPost;
    //call allocate again
    return allocate_object(raw_size);
  }
  //finding how much extra space is in the block
  int extra = get_block_size(hdr) - size;

  //check if the block should be split and added to a free list
  if (extra >= sizeof(header)) {
    header * tempHdr = hdr;
    hdr =(header *)((char *)hdr + extra);

    set_block_size(tempHdr, extra);
    set_block_state(tempHdr, (enum state) 0);
    int loc = (extra - ALLOC_HEADER_SIZE)/sizeof(header *) - 1;
    if (loc >= N_LISTS) {
      //readd to old location
      loc = N_LISTS - 1;
      tempHdr->next = oldNext;
      tempHdr->prev = oldPrev;
      oldNext->prev = tempHdr;
      oldPrev->next = tempHdr;
    } else {
      //add to front of list
      tempHdr->next = freelistSentinels[loc].next;
      freelistSentinels[loc].next = tempHdr;
      tempHdr->next->prev = tempHdr;
      tempHdr->prev = &freelistSentinels[loc];
    }

    set_block_size(hdr, size);
    hdr->left_size = extra;
  }

  //preparing hdr to be returned
  header * right = get_right_header(hdr);
  right->left_size = get_block_size(hdr);
  set_block_state(hdr, (enum state) 1);
  return (header *)((char *)hdr + ALLOC_HEADER_SIZE);

}

/**
 * @brief Helper to get the header from a pointer allocated with malloc
 *
 * @param p pointer to the data region of the block
 *
 * @return A pointer to the header of the block
 */
static inline header * ptr_to_header(void * p) {
  return (header *)((char *) p - ALLOC_HEADER_SIZE); //sizeof(header));
}

/**
 * @brief Helper to manage deallocation of a pointer returned by the user
 *
 * @param p The pointer returned to the user by a call to malloc
 */
static inline void deallocate_object(void * p) {
  // TODO implement deallocation
  if (!p) {
    return;
  }
  //converting p to header pointer and getting ptr to left and right
  header * hdr = ptr_to_header(p);

  //making sure only an allocated block is deallocated
  if (get_block_state(hdr) == (enum state) 0) {
    printf("Double Free Detected\n");
    assert(0);
    return;
  }
  if (get_block_state(hdr) == (enum state) 2) {
    return;
  }

  set_block_state(hdr, (enum state) 0);
  header * right = get_right_header(hdr);
  header * left = get_left_header(hdr);

  //to keep track of if they need to be re-inserted in same location in list
  bool coalR = false;
  bool coalL = false;

  //check and coalace if right block is empty
  if (get_block_state(right) == (enum state) 0) {
    //coalacing blocks
    set_block_size(hdr, get_block_size(hdr) + get_block_size(right));
    //removing right from free list
    right->next->prev = right->prev;
    right->prev->next = right->next;
    header * temp = get_right_header(right);
    temp->left_size = get_block_size(hdr);

    //checking if right was in the last list
    if (get_block_size(right) > ((N_LISTS - 1) * sizeof(header *)) + ALLOC_HEADER_SIZE) {
      coalR = true;
    }
  }

  //check and coallace if left block is empty
  if (get_block_state(left) == (enum state) 0) {
    //removing left from the list
    left->next->prev = left->prev;
    left->prev->next = left->next;

    //checking if left was in the last list
    if (get_block_size(left) > ((N_LISTS - 1) * sizeof(header *)) + ALLOC_HEADER_SIZE) {
      coalL = true;
    }

    //coalacing blocks
    set_block_size(left, get_block_size(left) + get_block_size(hdr));
    hdr = left;
    right = get_right_header(hdr);
    right->left_size = get_block_size(hdr);
  }

  //adding the block to the appropriate list and location
  if (coalL) {
    left->next->prev = hdr;
    left->prev->next = hdr;
    hdr->next = left->next;
    hdr->prev = left->prev;
  } else if (coalR) {
    right->next->prev = hdr;
    right->prev->next = hdr;
    hdr->next = right->next;
    hdr->prev = right->prev;
  } else {
    //find location to add it to a free list
    int listLoc = (get_block_size(hdr)-ALLOC_HEADER_SIZE)/8 - 1;
    if (listLoc >= N_LISTS) {
      listLoc = N_LISTS - 1;
    }
    //set pointers for the free list
    hdr->prev = &(freelistSentinels[listLoc]);
    hdr->next = freelistSentinels[listLoc].next;
    freelistSentinels[listLoc].next = hdr;
    hdr->next->prev = hdr;
  }
}
/**
 * @brief Helper to detect cycles in the free list
 * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
 *
 * @return One of the nodes in the cycle or NULL if no cycle is present
 */
static inline header * detect_cycles() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * slow = freelist->next, * fast = freelist->next->next; 
         fast != freelist; 
         slow = slow->next, fast = fast->next->next) {
      if (slow == fast) {
        return slow;
      }
    }
  }
  return NULL;
}

/**
 * @brief Helper to verify that there are no unlinked previous or next pointers
 *        in the free list
 *
 * @return A node whose previous and next pointers are incorrect or NULL if no
 *         such node exists
 */
static inline header * verify_pointers() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * cur = freelist->next; cur != freelist; cur = cur->next) {
      if (cur->next->prev != cur || cur->prev->next != cur) {
        return cur;
      }
    }
  }
  return NULL;
}

/**
 * @brief Verify the structure of the free list is correct by checkin for 
 *        cycles and misdirected pointers
 *
 * @return true if the list is valid
 */
static inline bool verify_freelist() {
  header * cycle = detect_cycles();
  if (cycle != NULL) {
    fprintf(stderr, "Cycle Detected\n");
    print_sublist(print_object, cycle->next, cycle);
    return false;
  }

  header * invalid = verify_pointers();
  if (invalid != NULL) {
    fprintf(stderr, "Invalid pointers\n");
    print_object(invalid);
    return false;
  }

  return true;
}

/**
 * @brief Helper to verify that the sizes in a chunk from the OS are correct
 *        and that allocated node's canary values are correct
 *
 * @param chunk AREA_SIZE chunk allocated from the OS
 *
 * @return a pointer to an invalid header or NULL if all header's are valid
 */
static inline header * verify_chunk(header * chunk) {
	if (get_block_state(chunk) != FENCEPOST) {
		fprintf(stderr, "Invalid fencepost\n");
		print_object(chunk);
		return chunk;
	}
	
	for (; get_block_state(chunk) != FENCEPOST; chunk = get_right_header(chunk)) {
		if (get_block_size(chunk)  != get_right_header(chunk)->left_size) {
			fprintf(stderr, "Invalid sizes\n");
			print_object(chunk);
			return chunk;
		}
	}
	
	return NULL;
}

/**
 * @brief For each chunk allocated by the OS verify that the boundary tags
 *        are consistent
 *
 * @return true if the boundary tags are valid
 */
static inline bool verify_tags() {
  for (size_t i = 0; i < numOsChunks; i++) {
    header * invalid = verify_chunk(osChunkList[i]);
    if (invalid != NULL) {
      return invalid;
    }
  }

  return NULL;
}

/**
 * @brief Initialize mutex lock and prepare an initial chunk of memory for allocation
 */
static void init() {
  // Initialize mutex for thread safety
  pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
  // Manually set printf buffer so it won't call malloc when debugging the allocator
  setvbuf(stdout, NULL, _IONBF, 0);
#endif // DEBUG

  // Allocate the first chunk from the OS
  header * block = allocate_chunk(ARENA_SIZE);

  header * prevFencePost = get_header_from_offset(block, -ALLOC_HEADER_SIZE);
  insert_os_chunk(prevFencePost);

  lastFencePost = get_header_from_offset(block, get_block_size(block));

  // Set the base pointer to the beginning of the first fencepost in the first
  // chunk from the OS
  base = ((char *) block) - ALLOC_HEADER_SIZE; //sizeof(header);

  // Initialize freelist sentinels
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    freelist->next = freelist;
    freelist->prev = freelist;
  }

  // Insert first chunk into the free list
  header * freelist = &freelistSentinels[N_LISTS - 1];
  freelist->next = block;
  freelist->prev = block;
  block->next = freelist;
  block->prev = freelist;
}

/* 
 * External interface
 */
void * my_malloc(size_t size) {
  pthread_mutex_lock(&mutex);
  header * hdr = allocate_object(size); 
  pthread_mutex_unlock(&mutex);
  return hdr;
}

void * my_calloc(size_t nmemb, size_t size) {
  return memset(my_malloc(size * nmemb), 0, size * nmemb);
}

void * my_realloc(void * ptr, size_t size) {
  void * mem = my_malloc(size);
  memcpy(mem, ptr, size);
  my_free(ptr);
  return mem; 
}

void my_free(void * p) {
  pthread_mutex_lock(&mutex);
  deallocate_object(p);
  pthread_mutex_unlock(&mutex);
}

bool verify() {
  return verify_freelist() && verify_tags();
}
