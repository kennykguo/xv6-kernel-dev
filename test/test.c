#include <stdio.h>   // For printf
#include <stdlib.h>  // For exit
#include <sys/types.h> // For pid_t
#include <sys/wait.h>  // For wait
#include <unistd.h>   // For fork

int main() {
    pid_t pid = fork(); // Create a new process

    if (pid > 0) { // Parent process
        printf("parent: child=%d\n", pid);
        pid = wait(0); // Wait for the child process to finish
        printf("child %d is done\n", pid);
    } else if (pid == 0) { // Child process
        printf("child: exiting\n");
        exit(0); // Exit the child process
    } else { // Fork error
        printf("fork error\n");
    }

    return 0; // Return success
}
