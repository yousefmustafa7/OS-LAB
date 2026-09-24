#include <assert.h>
#include <stdio.h>

#include "mem_alloc_fast_pool.h"
#include "my_mmap.h"
#include "mem_alloc.h"

/*
 * A fast pool is one mmap'ed region cut into equal-sized blocks
 * (block size = max_request_size: 64, 256 or 1024 bytes).
 *
 * Free blocks are chained in a singly linked list. The 'next' pointer is
 * stored in the first 8 bytes of the free block itself, so no extra memory
 * is needed for metadata. Allocated blocks carry no metadata at all: their
 * size is implied by the pool they belong to.
 *
 * Allocation and deallocation follow a LIFO policy: we always pop from and
 * push to the head of the list, so both are O(1).
 */

void init_fast_pool(mem_pool_t *p, size_t size, size_t min_request_size, size_t max_request_size)
{
    size_t block_size = max_request_size;
    size_t nb_blocks;
    size_t i;
    char *base;
    mem_fast_free_block_t *block;

    /* A block must be able to hold the 'next' pointer when it is free */
    assert(block_size >= sizeof(mem_fast_free_block_t));

    base = my_mmap(size);
    if (base == NULL)
    {
        fprintf(stderr, "init_fast_pool: my_mmap failed for %s\n", p->pool_name);
        exit(1);
    }

    p->start_addr = base;
    /* Inclusive bound: find_pool_from_address() tests addr <= end_addr */
    p->end_addr = base + size - 1;

    /* Leftover bytes (if size is not a multiple of block_size) are unused */
    nb_blocks = size / block_size;

    /*
     * Chain the blocks by increasing address, so that the first
     * allocations return offsets 0, block_size, 2*block_size, ...
     */
    for (i = 0; i < nb_blocks; i++)
    {
        block = (mem_fast_free_block_t *)(base + i * block_size);
        if (i + 1 < nb_blocks)
        {
            block->next = (mem_fast_free_block_t *)(base + (i + 1) * block_size);
        }
        else
        {
            block->next = NULL;
        }
    }

    p->first_free = (nb_blocks > 0) ? base : NULL;

    debug_printf("%s: %lu blocks of %lu bytes at %p\n", p->pool_name, nb_blocks, block_size, base);
}

void *mem_alloc_fast_pool(mem_pool_t *pool, size_t size)
{
    mem_fast_free_block_t *block = pool->first_free;

    /* Pool exhausted: the caller (memory_alloc) reports the error */
    if (block == NULL)
    {
        return NULL;
    }

    /* Pop the head of the free list */
    pool->first_free = block->next;
    return block;
}

void mem_free_fast_pool(mem_pool_t *pool, void *b)
{
    mem_fast_free_block_t *block = b;

    /* Push the block back at the head of the free list (LIFO) */
    block->next = pool->first_free;
    pool->first_free = block;
}

size_t mem_get_allocated_block_size_fast_pool(mem_pool_t *pool, void *addr)
{
    size_t res;
    res = pool->max_req_size;
    return res;
}
