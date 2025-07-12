// Block buffer cache
#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13

struct {
  // 注意：buf数组本身没有变，每个buf内部都有自己的sleeplock
  struct buf buf[NBUF]; 
  
  struct {
    struct spinlock lock; // 桶锁，保护桶的链表结构
    struct buf *head;     // 指向桶的第一个buf
  } buckets[NBUCKETS];

} bcache;

// 哈希函数保持不变
static inline int
hash(uint blockno)
{
  return blockno % NBUCKETS;
}

void
binit(void)
{
  // 将lockname的大小从16增加到32，确保安全
  char lockname[32]; 
  struct buf *b;

  // 1. 初始化每个桶的锁
  for (int i = 0; i < NBUCKETS; i++) {
    // 现在snprintf的输出肯定不会溢出了
    snprintf(lockname, sizeof(lockname), "bcache_bucket_%d", i);
    initlock(&bcache.buckets[i].lock, lockname);
    bcache.buckets[i].head = 0; 
  }

  // 2. 初始化每个buf的sleeplock... (这部分不变)
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    initsleeplock(&b->lock, "bcache_buf");
    b->next = bcache.buckets[0].head;
    bcache.buckets[0].head = b;
  }
}

static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int bucket_idx = hash(blockno);

  // --- 1. 查找现有缓存 (只读操作，在桶锁保护下进行) ---
  acquire(&bcache.buckets[bucket_idx].lock);
  for(b = bcache.buckets[bucket_idx].head; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.buckets[bucket_idx].lock);
      // 获取这个buf自己的sleeplock，因为调用者期望返回一个锁定的buf
      acquiresleep(&b->lock); 
      return b;
    }
  }
  release(&bcache.buckets[bucket_idx].lock);

  // --- 2. 缓存未命中，需要找一个可回收的buf ---
  // 这个过程可能需要修改缓存的结构（移动buf），所以更复杂。
  // 我们从目标桶开始，依次检查所有桶。
  for (int i = 0; i < NBUCKETS; i++) {
    int current_bucket_idx = (bucket_idx + i) % NBUCKETS; // 从目标桶开始轮询
    acquire(&bcache.buckets[current_bucket_idx].lock);

    for (b = bcache.buckets[current_bucket_idx].head; b; b = b->next) {
      if (b->refcnt == 0) {
        // 找到了一个可回收的buf 'b'，它在 'current_bucket_idx' 桶里
        // 注意：在修改b之前，需要先获取它的sleeplock，防止其他进程同时使用它
        // 这是一个潜在的死锁风险：持有桶的spinlock，去获取buf的sleeplock
        // 为了避免死锁，我们采用“尝试锁定”或释放重试策略。
        // 但在这个实验中，一个简化的、可接受的假设是：refcnt==0的buf不会被其他进程持有sleeplock。
        // 所以我们可以直接锁定它。
        
        b->refcnt = 1; // 先占住它

        // 如果这个buf不在它应该在的桶里，就需要移动它
        if (current_bucket_idx != bucket_idx) {
          // a. 从旧桶(current_bucket_idx)的链表中移除
          struct buf **ptr = &bcache.buckets[current_bucket_idx].head;
          while (*ptr != b) {
            ptr = &(*ptr)->next;
          }
          *ptr = b->next;
          release(&bcache.buckets[current_bucket_idx].lock); // 释放旧桶的锁

          // b. 加入到新桶(bucket_idx)的链表中
          acquire(&bcache.buckets[bucket_idx].lock);
          b->next = bcache.buckets[bucket_idx].head;
          bcache.buckets[bucket_idx].head = b;
        }
        
        // 现在buf 'b' 肯定在正确的桶 'bucket_idx' 中了
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0; // 标记为无效，需要从磁盘读取
        release(&bcache.buckets[bucket_idx].lock);

        acquiresleep(&b->lock);
        return b;
      }
    }
    // 如果当前桶没有可回收的，就释放锁，尝试下一个桶
    release(&bcache.buckets[current_bucket_idx].lock);
  }

  panic("bget: no free buffers");
}


// bread函数几乎不变，因为它依赖于bget返回一个锁定的buf
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// bwrite函数不变
void
bwrite(struct buf *b)
{
  // 调用者必须已经持有 b->lock
  virtio_disk_rw(b, 1);
}

// brelse函数需要释放sleeplock，并原子地减少refcnt
void
brelse(struct buf *b)
{
  // 调用者持有 b->lock (sleeplock)
  releasesleep(&b->lock);

  int bucket_idx = hash(b->blockno);
  acquire(&bcache.buckets[bucket_idx].lock);
  b->refcnt--;
  release(&bcache.buckets[bucket_idx].lock);
}

// bpin 和 bunpin 用于临时增加/减少引用计数，防止buf被回收
void
bpin(struct buf *b) {
  int bucket_idx = hash(b->blockno);
  acquire(&bcache.buckets[bucket_idx].lock);
  b->refcnt++;
  release(&bcache.buckets[bucket_idx].lock);
}

void
bunpin(struct buf *b) {
  int bucket_idx = hash(b->blockno);
  acquire(&bcache.buckets[bucket_idx].lock);
  b->refcnt--;
  release(&bcache.buckets[bucket_idx].lock);
}
