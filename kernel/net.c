#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

#define NSOCK 32                // 系统支持的最大套接字（绑定端口）数量
#define MAX_UDP_PACKETS 16      // 每个端口队列中最多存放的数据包数量

// 代表一个UDP套接字（一个被绑定的端口）
struct sock {
  int in_use;                 // 此插槽是否被使用
  short port;                 // 绑定的端口号
  struct spinlock lock;       // 保护此套接字的锁（也可以使用一个全局锁，这里用全局锁更简单）
  
  // 用于存放已到达但未被读取的数据包的队列（环形缓冲区）
  char *packets[MAX_UDP_PACKETS]; 
  uint r_idx;                 // 读指针
  uint w_idx;                 // 写指针
  uint count;                 // 队列中的数据包数量

  struct proc *waiting_proc;  // 正在等待此端口数据的进程 (用于sleep/wakeup)
};

// 全局套接字数组和锁
struct sock sockets[NSOCK];     // 系统中所有套接字的池
static struct spinlock netlock;

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };



void
netinit(void)
{
  initlock(&netlock, "netlock");
  for (int i = 0; i < NSOCK; ++i) {
    sockets[i].in_use = 0; // 标记所有套接字为空闲
    // initlock(&sockets[i].lock, "sock"); // 如果选择每个套接字一个锁，则在此处初始化
  }
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
// 在 kernel/net.c 中添加此新函数

// bind(port) 系统调用
// 为一个UDP端口准备接收数据包
// 在 kernel/net.c 中，这是 sys_bind 的新版本

// bind(port) 系统调用
// 为一个UDP端口准备接收数据包
uint64
sys_bind(void)
{
  struct proc *p = myproc();

  // --- 替换 argshort() 的部分 ---
  // 直接从当前进程的陷阱帧(trapframe)中获取第一个系统调用参数。
  // 在RISC-V中，第一个参数存放在 a0 寄存器中。
  short port = (short)p->trapframe->a0;
  // ------------------------------

  struct sock *s = 0;

  // 使用修改后的锁名 netlock
  acquire(&netlock);

  // 检查端口是否已被其他套接字绑定
  for (struct sock *sp = sockets; sp < &sockets[NSOCK]; sp++) {
    if (sp->in_use && sp->port == port) {
      release(&netlock);
      // 打印一个调试信息可能有助于排查问题
      // printf("sys_bind: error, port %d already in use\n", port);
      return -1; // 错误：端口已被占用
    }
  }

  // 查找一个空闲的套接字插槽
  for (struct sock *sp = sockets; sp < &sockets[NSOCK]; sp++) {
    if (sp->in_use == 0) {
      s = sp;
      break;
    }
  }

  if (s == 0) {
    release(&netlock);
    // printf("sys_bind: error, no free sockets\n");
    return -1; // 错误：没有可用的套接字
  }

  // 初始化这个新找到的套接字
  s->in_use = 1;
  s->port = port;
  s->r_idx = 0;
  s->w_idx = 0;
  s->count = 0;
  s->waiting_proc = 0;

  // 初始化该套接字队列中的所有数据包指针为空
  for (int i = 0; i < MAX_UDP_PACKETS; i++) {
    s->packets[i] = 0;
  }

  release(&netlock);

  return 0; // 成功
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
// 在 kernel/net.c 中添加此新函数

// recv(dport, src_ip, src_port, buf, maxlen) 系统调用
// 从一个绑定的端口读取一个UDP数据包
// 在 kernel/net.c 中，这是 sys_recv 的新版本

// recv(dport, src_ip, src_port, buf, maxlen) 系统调用
// 从一个绑定的端口读取一个UDP数据包
uint64
sys_recv(void)
{
  struct proc *p = myproc();

  // --- 替换 arg...() 系列函数的部分 ---
  // 直接从当前进程的陷阱帧(trapframe)中获取系统调用参数。
  // 在RISC-V中，参数依次存放在 a0, a1, a2, a3, a4 寄存器中。
  
  // 第一个参数: dport (short, 16位)
  // 参数在寄存器中通常是64位宽，我们只需取低16位。
  short dport = (short)p->trapframe->a0;

  // 第二个参数: src_ip_ptr (指向 uint32 的指针)
  // 这是用户空间的虚拟地址
  uint64 src_ip_ptr = p->trapframe->a1;

  // 第三个参数: src_port_ptr (指向 short 的指针)
  // 用户空间的虚拟地址
  uint64 src_port_ptr = p->trapframe->a2;

  // 第四个参数: buf_ptr (指向 char 的缓冲区指针)
  // 用户空间的虚拟地址
  uint64 buf_ptr = p->trapframe->a3;

  // 第五个参数: maxlen (int, 32位)
  int maxlen = (int)p->trapframe->a4;
  
  // 简单的参数验证 (可选，但推荐)
  if (src_ip_ptr == 0 || src_port_ptr == 0 || buf_ptr == 0 || maxlen <= 0) {
    return -1; // 无效的指针或长度
  }
  // ------------------------------------

  struct sock *s = 0;

  // 使用修改后的锁名 netlock
  acquire(&netlock);

  // 查找与目标端口匹配的套接字
  for (struct sock *sp = sockets; sp < &sockets[NSOCK]; sp++) {
    if (sp->in_use && sp->port == dport) {
      s = sp;
      break;
    }
  }

  if (s == 0) {
    // 错误：端口未绑定
    release(&netlock);
    return -1;
  }

  // 等待数据包到达
  while (s->count == 0) {
    // 标记此进程正在等待
    s->waiting_proc = p;
    // 使用套接字地址作为sleep的通道
    // sleep会原子性地释放 netlock 并让进程休眠
    // 当被唤醒时，它会重新获取 netlock
    sleep(s, &netlock);
    // 被唤醒后清除标记
    s->waiting_proc = 0;
  }

  // 从队列中取出一个数据包 (char*)
  char *buf = s->packets[s->r_idx];
  s->packets[s->r_idx] = 0; // 清空指针
  s->r_idx = (s->r_idx + 1) % MAX_UDP_PACKETS;
  s->count--;

  // 在复制数据到用户空间之前释放锁，减少锁的持有时间
  release(&netlock); 

  // 解析数据包
  struct ip *iph = (struct ip *)(buf + sizeof(struct eth));
  struct udp *udph = (struct udp *)((char *)iph + sizeof(struct ip));
  
  // 转换字节序
  uint32 src_ip = ntohl(iph->ip_src);
  uint16 src_port = ntohs(udph->sport);
  uint16 payload_len = ntohs(udph->ulen) - sizeof(struct udp);

  // 计算要复制的长度
  int len_to_copy = (payload_len < maxlen) ? payload_len : maxlen;

  // 将源地址、源端口和数据负载复制到用户空间
  char *payload = (char *)udph + sizeof(struct udp);
  
  // 使用 copyout 将数据从内核空间安全地复制到用户空间
  if(copyout(p->pagetable, src_ip_ptr, (char *)&src_ip, sizeof(src_ip)) < 0 ||
     copyout(p->pagetable, src_port_ptr, (char *)&src_port, sizeof(src_port)) < 0 ||
     copyout(p->pagetable, buf_ptr, payload, len_to_copy) < 0) {
    kfree(buf); // 复制失败也要释放内存
    return -1;
  }
  
  kfree(buf); // 处理完毕，释放数据包缓冲区

  return len_to_copy; // 返回复制的字节数
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0) {
    printf("ip_rx: received an IP packet\n");
    seen_ip = 1;
  }

  // Your code here.

  // 1. 解析报头
  // buf 指向以太网帧的开始，我们需要跳过以太网报头 (14字节) 来获取IP报头。
  // 注意：这个实现假设了没有VLAN标签等额外字段。
  struct ip *iph = (struct ip *)(buf + sizeof(struct eth));
  
  // 检查 IP 版本和报头长度，确保它是一个我们能处理的 IPv4 数据包
  if (iph->ip_vhl != 0x45) { // Version 4, Header Length 5*4=20 bytes
    // 不支持的IP版本或选项，直接丢弃
    kfree(buf); // 释放缓冲区
    return;
  }
  
  // 检查是否为UDP数据包 (IP协议号 17)
  if(iph->ip_p != IPPROTO_UDP) {
    // 不是UDP包，直接丢弃
    // (在真实的协议栈中，这里会分发给ICMP, TCP等处理函数)
    kfree(buf); // 释放缓冲区
    return;
  }

  // 获取UDP报头
  // IP报头长度是20字节 (因为我们检查了ip_vhl == 0x45)
  struct udp *udph = (struct udp *)((char *)iph + sizeof(struct ip));
  
  // 2. 查找绑定的套接字
  // 使用 ntohs (Network To Host Short) 进行字节序转换
  uint16 dport = ntohs(udph->dport);
  struct sock *s = 0;

  acquire(&netlock);

  // 遍历套接字数组，查找与目标端口匹配的已绑定套接字
  for (struct sock *sp = sockets; sp < &sockets[NSOCK]; sp++) {
    if (sp->in_use && sp->port == dport) {
      s = sp;
      break;
    }
  }

  if (s == 0) {
    // 没有进程绑定到此端口，丢弃数据包
    release(&netlock);
    kfree(buf); // 释放缓冲区
    return;
  }

  // 3. 将数据包放入队列
  // 检查队列是否已满
  if (s->count >= MAX_UDP_PACKETS) {
    // 队列已满，丢弃数据包
    release(&netlock);
    kfree(buf); // 释放缓冲区
    return;
  }

  // 我们需要将 char* 缓冲区转换为 mbuf 结构体，
  // 或者直接在 sock 结构体中存储 char*。
  // 为了与 sys_recv 的 mbuf 设计兼容，我们最好在这里创建一个 mbuf。
  // 一个更简单的做法是，如果您的 sock 结构体直接存储 char*，那就直接存。
  // 让我们假设 sock 结构体直接存储 char* 以简化实现。
  // 修改 struct sock { ...; char *packets[MAX_UDP_PACKETS]; ... };
  
  // 将数据包缓冲区指针放入队列
  s->packets[s->w_idx] = buf; // 注意：此时我们将整个以太网帧存入了队列
  s->w_idx = (s->w_idx + 1) % MAX_UDP_PACKETS;
  s->count++;

  // 4. 唤醒等待的进程
  // 如果有进程正在等待数据，唤醒它。
  // 我们使用套接字本身的地址作为唯一的 "通道" (chan)
  if (s->waiting_proc) {
    wakeup(s);
  }
  
  release(&netlock);

  // 注意：此时 buf 的所有权已经转移给了套接字队列，
  // 它将在 sys_recv 中被处理并最终释放。
  // 所以这里不能再调用 kfree(buf)
}
//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
