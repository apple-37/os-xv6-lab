#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

int readline(char *new_argv[32], int curr_argc){
	char buf[1024];
	int n = 0;
	while(read(0, buf+n, 1)){
		if (n == 1023){
			fprintf(2, "argument is too long\n");
			exit(1);
		}
		if (buf[n] == '\n'){
			break;
		}
		n++;
	}
	buf[n] = 0;
	if (n == 0) return 0;
	int offset = 0;
	while(offset < n){
		// 跳过前导空格
		while(buf[offset] == ' ' && offset < n){
			offset++;
		}
		if(offset >= n) break;
		int start = offset;
		while(buf[offset] != ' ' && buf[offset] != 0 && offset < n){
			offset++;
		}
		// 分配新内存并拷贝参数
		int len = offset - start;
		char *arg = malloc(len + 1);
		memmove(arg, buf + start, len);
		arg[len] = 0;
		new_argv[curr_argc++] = arg;
		// 跳过空格
		while(buf[offset] == ' ' && offset < n){
			offset++;
		}
	}
	return curr_argc;
}

int main(int argc, char const *argv[])
{
    int n = 0;
    int cmd_idx = 1;
    // 解析 -n 参数
    if(argc > 2 && strcmp(argv[1], "-n") == 0){
        n = atoi(argv[2]);
        cmd_idx = 3;
    }
    if(n == 0) n = 32; // 默认每次最多32个参数

    if(argc <= cmd_idx){
        fprintf(2, "Usage: xargs [-n N] command (arg ...)\n");
        exit(1);
    }

    char *command = malloc(strlen(argv[cmd_idx]) + 1);
    strcpy(command, argv[cmd_idx]);

    char *fixed_argv[MAXARG];
    int fixed_argc = 0;
    for(int i = cmd_idx; i < argc; ++i){
        fixed_argv[fixed_argc] = malloc(strlen(argv[i]) + 1);
        strcpy(fixed_argv[fixed_argc++], argv[i]);
    }

    char buf[1024];
    int len = 0;
    while(read(0, buf+len, 1) == 1){
        if(buf[len] == '\n'){
            buf[len] = ' ';
        }
        len++;
        if(len >= 1023) break;
    }
    buf[len] = 0;

    // 按空格分割参数，每 n 个参数执行一次
    int offset = 0;
    while(offset < len){
        char *new_argv[MAXARG];
        int curr_argc = 0;
        // 固定参数
        for(int i = 0; i < fixed_argc; i++)
            new_argv[curr_argc++] = fixed_argv[i];

        // 追加 n 个参数
        for(int i = 0; i < n && offset < len; i++){
            while(buf[offset] == ' ' && offset < len) offset++;
            if(offset >= len) break;
            int start = offset;
            while(buf[offset] != ' ' && buf[offset] != 0 && offset < len) offset++;
            int arglen = offset - start;
            if(arglen > 0){
                char *arg = malloc(arglen + 1);
                memmove(arg, buf + start, arglen);
                arg[arglen] = 0;
                new_argv[curr_argc++] = arg;
            }
        }
        if(curr_argc == fixed_argc) break; // 没有新参数了
        new_argv[curr_argc] = 0;
        if(fork() == 0){
            exec(command, new_argv);
            fprintf(2, "exec failed\n");
            exit(1);
        }
        wait(0);
    }
    exit(0);
}