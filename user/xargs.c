#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

// This function reads one line from standard input, splits it into arguments by spaces,
// and appends them to the new_argv array.
// It returns the new total number of arguments.
// It returns 0 to signal the end of input (EOF) or if the line was empty.
int readline(char *new_argv[MAXARG], int curr_argc) {
    char buf[1024];
    int n = 0;

    // Read characters one by one until a newline or EOF is encountered.
    while (read(0, buf + n, 1) > 0) {
        if (buf[n] == '\n') {
            break;
        }
        n++;
        if (n >= 1023) {
            fprintf(2, "xargs: input line too long\n");
            exit(1);
        }
    }
    // If read returned 0 (EOF) and we have no characters, it's the end of input.
    if (n == 0) {
        return 0;
    }
    buf[n] = 0; // Null-terminate the line.

    int offset = 0;
    while (offset < n) {
        // Skip leading/multiple spaces
        while (offset < n && buf[offset] == ' ') {
            offset++;
        }
        // If we are at the end of the buffer (e.g., trailing spaces), stop.
        if (offset >= n) {
            break;
        }

        int start = offset;
        // Find the end of the current argument
        while (offset < n && buf[offset] != ' ') {
            offset++;
        }
        
        if (curr_argc >= MAXARG - 1) {
            fprintf(2, "xargs: too many arguments\n");
            exit(1);
        }

        // Allocate memory for the new argument and copy it.
        int len = offset - start;
        char *arg = malloc(len + 1);
        if (arg == 0) {
            fprintf(2, "xargs: malloc failed\n");
            exit(1);
        }
        memmove(arg, buf + start, len);
        arg[len] = 0;
        new_argv[curr_argc++] = arg;
    }
    return curr_argc;
}

int main(int argc, char *argv[]) {
    // xargs needs at least a command to execute, e.g., "xargs echo"
    if (argc < 2) {
        fprintf(2, "Usage: xargs command (arg ...)\n");
        exit(1);
    }
    
    // The command to be executed is the first argument after "xargs"
    char *command = argv[1];

    // The base arguments are the ones provided on the command line.
    // For "xargs echo hello", the base arguments are "echo" and "hello".
    // We will use these for every command execution.
    char *base_argv[MAXARG];
    int base_argc = 0;
    for (int i = 1; i < argc; i++) {
        base_argv[base_argc++] = argv[i];
    }
    
    // Main loop: process one line of input at a time.
    while (1) {
        // For each line, we will build a new full argument list for exec.
        char *exec_argv[MAXARG];
        int exec_argc = 0;

        // 1. Start with the base arguments from the command line.
        for (int i = 0; i < base_argc; i++) {
            exec_argv[exec_argc++] = base_argv[i];
        }

        // 2. Read one line from standard input and append its contents as new arguments.
        int final_argc = readline(exec_argv, exec_argc);

        // 3. Check for termination condition.
        // If readline returns 0, it means EOF was reached.
        // If final_argc is the same as exec_argc, it means an empty line was read,
        // so we don't execute anything and just continue to the next line.
        if (final_argc == 0) {
            break; // End of input
        }
        if (final_argc == exec_argc) {
            continue; // Empty line, do nothing
        }
        
        // 4. Null-terminate the argument list for exec.
        exec_argv[final_argc] = 0;

        // 5. Fork a child process to execute the command.
        if (fork() == 0) {
            // Child process
            exec(command, exec_argv);
            // exec only returns if it fails.
            fprintf(2, "xargs: exec %s failed\n", command);
            exit(1);
        } else {
            // Parent process
            // Wait for the child to finish before processing the next line of input.
            wait(0);
        }
    }

    exit(0);
}