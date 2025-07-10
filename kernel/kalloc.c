// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define SUPERPAGE_SIZE (2 * 1024 * 1024)  // 2MB
#define NSUPERPAGES 8  // 预留8个超级页面

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

struct {
  struct spinlock lock;
  char *freelist;
  char superpages[NSUPERPAGES * SUPERPAGE_SIZE] __attribute__((aligned(SUPERPAGE_SIZE)));
  int free_superpages[NSUPERPAGES];
} super_kmem;

void
superinit(void)
{
  initlock(&super_kmem.lock, "super_kmem");
  // 初始化所有超级页面为可用
  for(int i = 0; i < NSUPERPAGES; i++) {
    super_kmem.free_superpages[i] = 1;
  }
}

// 分配一个2MB超级页面
void*
superalloc(void)
{
  acquire(&super_kmem.lock);
  for(int i = 0; i < NSUPERPAGES; i++) {
    if(super_kmem.free_superpages[i]) {
      super_kmem.free_superpages[i] = 0;
      void *pa = &super_kmem.superpages[i * SUPERPAGE_SIZE];
      release(&super_kmem.lock);
      memset(pa, 0, SUPERPAGE_SIZE);  // 清零
      return pa;
    }
  }
  release(&super_kmem.lock);
  return 0;  // 分配失败
}

// 释放一个2MB超级页面
void
superfree(void *pa)
{
  if(((uint64)pa % SUPERPAGE_SIZE) != 0 || (char*)pa < super_kmem.superpages || 
     (char*)pa >= super_kmem.superpages + NSUPERPAGES * SUPERPAGE_SIZE)
    panic("superfree");

  acquire(&super_kmem.lock);
  int index = ((char*)pa - super_kmem.superpages) / SUPERPAGE_SIZE;
  super_kmem.free_superpages[index] = 1;
  release(&super_kmem.lock);
}

// 检查地址是否是超级页面
int
is_superpage(uint64 pa)
{
  return (char*)pa >= super_kmem.superpages && 
         (char*)pa < super_kmem.superpages + NSUPERPAGES * SUPERPAGE_SIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
  superinit();
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

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
