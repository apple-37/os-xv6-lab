#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static char *tx_bufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static char *rx_bufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_tx_lock;
struct spinlock e1000_rx_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_tx_lock, "e1000_tx");
  initlock(&e1000_rx_lock, "e1000_rx");
  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_bufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_bufs[i] = kalloc();
    if (!rx_bufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_bufs[i];
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}


uint32 e1000_tx_next_tail = 0;
int
e1000_transmit(char *buf, int len)
{

  acquire(&e1000_tx_lock);

  // 获取下一个可用 TX 描述符的索引。
  uint32 tdt = e1000_tx_next_tail;

  // 检查环是否已满。
  // 我们检查当前槽位的 '描述符完成' 位 (E1000_TXD_STAT_DD) 是否已设置。
  // 如果 DD 未设置，则环已满或该描述符尚未完成（仍归硬件所有）。
  if (!(tx_ring[tdt].status & E1000_TXD_STAT_DD)) {
    // 无法发送。释放锁并返回失败。
    release(&e1000_tx_lock);
    return -1;
  }

  // 确保数据包长度符合 E1000 要求（如果硬件没有填充，以太网帧最小 60 字节）。
  // E1000_TCTL_PSP（填充短数据包）在 e1000_init 中已设置，所以此处严格来说不需要最小长度检查，
  // 但这是良好的实践。

  // 1. 存储缓冲区指针。我们需要它以便稍后释放缓冲区。
  // 描述符环 `tx_ring` 存储物理地址，`tx_bufs` 存储内核虚拟地址。
  if (tx_bufs[tdt] != 0 && tx_bufs[tdt] != (char*)0x1) {
    // 这只会在之前的某个数据包未被释放时发生，意味着存在错误或上述描述符状态检查失败。
    kfree(tx_bufs[tdt]);
  }
  tx_bufs[tdt] = buf; 

  // 2. 配置描述符。
  // 假设 buf 已经是内核物理地址或标识映射的虚拟地址。
  tx_ring[tdt].addr = (uint64)buf; 
  tx_ring[tdt].length = len;

  // 设置控制标志：
  // EOP (数据包结束) - 这是一个单一数据包
  // RS (报告状态) - 要求硬件在完成后设置 DD 位
  tx_ring[tdt].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  
  // 设置状态为 0 以表示驱动程序拥有它（可选，通常 DD 在初始化或上次使用时已经是 0）

  // 3. 更新尾指针并通知硬件。
  e1000_tx_next_tail = (tdt + 1) % TX_RING_SIZE;
  regs[E1000_TDT] = e1000_tx_next_tail;

  release(&e1000_tx_lock);

  return 0;
}

static void
e1000_recv(void)
{
  acquire(&e1000_rx_lock);

  // e1000_init 中的接收环管理将 RDT 设置为 SIZE-1，RDH 设置为 0。
  // 硬件从索引 0 开始填充。我们检查 (RDT + 1) % RX_RING_SIZE 处的描述符以查看接收到的数据包。

  // RDT 是驱动程序最后提供给硬件的描述符索引。
  uint32 rdt = regs[E1000_RDT]; 
  // 我们要检查的下一个索引是 (当前 RDT + 1) % RX_RING_SIZE。
  uint32 next_rdt = (rdt + 1) % RX_RING_SIZE;

  // 遍历硬件已完成写入的所有描述符。
  // 如果设置了 E1000_RXD_STAT_DD 位，则描述符“已完成”（已接收到数据包）。
  while (rx_ring[next_rdt].status & E1000_RXD_STAT_DD) {
    
    // 检查数据包结束 (EOP) 状态，这是完整数据包所必需的。
    // 如果是数据包的最后一个缓冲区，通常会设置 E1000_RXD_STAT_EOP。
    if (!(rx_ring[next_rdt].status & E1000_RXD_STAT_EOP)) {
        // 此处未处理多描述符数据包。对于 xv6 中的标准以太网帧，这应该是 EOP。
        panic("e1000_recv: Multi-descriptor packet detected");
    }

    // 1. 检索接收到的数据包信息。
    uint32 len = rx_ring[next_rdt].length;
    char *buf = rx_bufs[next_rdt];

    // 2. 将数据包交付给网络协议栈。net_rx 接受缓冲区和长度。
    net_rx(buf, len);

    // 3. 为下一个传入数据包准备描述符。

    // 为描述符分配一个新的缓冲区。
    char *new_buf = kalloc();
    if (!new_buf) {
      // 如果无法分配新的缓冲区，则 panic。在 xv6 中，对于关键资源故障，通常会 panic。
      panic("e1000_recv: kalloc failed for new RX buffer");
    }

    // 将 rx_bufs 指针更新为新缓冲区
    rx_bufs[next_rdt] = new_buf;
    
    // 使用新缓冲区的物理地址更新描述符。
    // 之前的缓冲区 (`buf`) 现在归网络协议栈 (`net_rx`) 所有。
    rx_ring[next_rdt].addr = (uint64)new_buf;

    // 清除状态位以指示描述符已准备好供硬件使用。
    // 关键是，清除 E1000_RXD_STAT_DD。
    rx_ring[next_rdt].status = 0; 

    // 将 RDT 索引前进到这个新可用的描述符。
    rdt = next_rdt;
    next_rdt = (rdt + 1) % RX_RING_SIZE;
  }

  // 4. 更新硬件 RDT 寄存器。
  // 这告诉 E1000 驱动程序在何处提供了可用的缓冲区。
  // 注意：我们将 RDT 更新为我们处理的 *最后* 一个描述符的索引。
  regs[E1000_RDT] = rdt;

  release(&e1000_rx_lock);
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
