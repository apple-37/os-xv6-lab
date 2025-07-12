// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers.
// Allocates memory in page-sized chunks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// We need a structure to hold the lock and freelist for each CPU.
struct kmem_cpu {
  struct spinlock lock;
  struct run *freelist;
};

// Create an array of these structures, one for each possible CPU.
struct {
  struct kmem_cpu cpus[NCPU];
} kmem;


void
kinit()
{
  // Initialize one lock for each CPU's freelist.
  // We use snprintf to give each lock a unique name for easier debugging.
  // For example, on a 4-core machine, this will create locks named
  // "kmem0", "kmem1", "kmem2", and "kmem3".
  char lockname[16];
  for (int i = 0; i < NCPU; i++) {
    snprintf(lockname, sizeof(lockname), "kmem%d", i);
    initlock(&kmem.cpus[i].lock, lockname);
  }
  
  // This function will now add all the initial free memory to the
  // freelist of the CPU that is currently running this code (the bootstrap CPU).
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // To safely use cpuid(), we must disable interrupts.
  // push_off/pop_off handle this for us.
  push_off();
  int id = cpuid(); // Get the current CPU's ID.
  
  // Acquire the lock for this specific CPU's freelist.
  acquire(&kmem.cpus[id].lock);
  
  // Add the memory to this CPU's freelist.
  r->next = kmem.cpus[id].freelist;
  kmem.cpus[id].freelist = r;
  
  // Release the lock.
  release(&kmem.cpus[id].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void*
kalloc(void)
{
  struct run *r;

  // Disable interrupts to get the current CPU ID safely.
  push_off();
  int id = cpuid();

  // --- Fast Path: Allocate from local CPU's freelist ---
  // Acquire the lock for the local CPU.
  acquire(&kmem.cpus[id].lock);
  r = kmem.cpus[id].freelist;
  if(r){
    // If we found a free page, update the list head and we are done.
    kmem.cpus[id].freelist = r->next;
  }
  // Release the local lock. We are either done, or we are about to steal.
  // In either case, we don't need this lock anymore.
  release(&kmem.cpus[id].lock);

  // --- Slow Path: Steal from another CPU's freelist ---
  if(!r){
    // The local freelist was empty. We must try to steal from other CPUs.
    for(int i = 0; i < NCPU; i++){
      if(i == id){
        // Don't steal from ourselves.
        continue;
      }
      
      // Acquire the lock of the "victim" CPU.
      acquire(&kmem.cpus[i].lock);
      struct run* victim_list = kmem.cpus[i].freelist;

      if(victim_list){
        // The victim has memory. Let's steal *half* of it.
        // This is a common heuristic: it's more efficient than stealing one
        // page at a time, but less aggressive than stealing everything.
        struct run* slow = victim_list;
        struct run* fast = victim_list;
        while(fast && fast->next){
          slow = slow->next;
          fast = fast->next->next;
        }

        // 'victim_list' is the head of the list we will steal.
        r = victim_list;
        
        // The victim's new freelist starts after the midpoint ('slow').
        kmem.cpus[i].freelist = slow->next;

        // Terminate the list we just stole.
        slow->next = 0;

        // Release the victim's lock. We are done with their data structures.
        release(&kmem.cpus[i].lock);
        
        // Now, add the stolen pages (except for the one we will return)
        // to our own local freelist.
        acquire(&kmem.cpus[id].lock);
        kmem.cpus[id].freelist = r->next; // Our list is the tail of the stolen pages.
        r->next = 0; // The page we return must be severed from the list.
        release(&kmem.cpus[id].lock);
        
        // We have successfully stolen memory, so we can stop looking.
        break;
      } else {
        // This victim had no memory. Release their lock and try the next one.
        release(&kmem.cpus[i].lock);
      }
    }
  }

  // Re-enable interrupts.
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}