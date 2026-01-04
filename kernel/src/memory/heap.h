#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>


typedef struct BlockHeader {
    struct BlockHeader* next; 
    struct BlockHeader* prev; 
    size_t size;              
    bool is_free;             
} BlockHeader;


void init_heap();


void* kmalloc(size_t size);


void kfree(void* ptr);


void kheap_print_stats();

#endif