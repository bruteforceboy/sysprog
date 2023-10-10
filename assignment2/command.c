#include "parser.h"
#include "command.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

void execute_pipeline(const struct command_line *line) {
    assert(line != NULL);

    const struct expr *e = line->head;
    int input_fd = STDIN_FILENO; // Default input
    int pipe_fd[2];

    while (e != NULL) {
        if (pipe(pipe_fd) == -1) {
            perror("pipe");
            exit(EXIT_FAILURE);
        }

        execute_command((e->next == NULL ? 1 : 0), e->cmd.exe, e->cmd.arg_count, e->cmd.args, input_fd, pipe_fd[1]);

        // Close write end of the pipe (after executing the current command)
        close(pipe_fd[1]);

        input_fd = pipe_fd[0];

        if (e->next == NULL)
            break;
        e = e->next->next; // skip next pipe
    }
    char buffer[1024];
    ssize_t bytes_read;

    while ((bytes_read = read(input_fd, buffer, sizeof(buffer))) > 0) {
        write(STDOUT_FILENO, buffer, bytes_read);
    }
}

int execute_command(int last_expr, char* exe, int args_count, char** args, int input_fd, int output_fd) {
    char** new_args = (char**) malloc((args_count + 2) * sizeof(char*));

    if (new_args == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    new_args[0] = exe;
    for (int i = 0; i < args_count; i++)
        new_args[i + 1] = args[i];
    new_args[args_count + 1] = '\0';

    if (strcmp(exe, "cd") == 0) {
        if (args_count >= 1) {
            if (chdir(args[0]) != 0) {
                perror("chdir");
            }
        }
    } else if (strcmp(exe, "exit") == 0) {
        if (last_expr == 1) {
            free(new_args);
            return 1;
        }
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            if (input_fd != STDIN_FILENO) {
                dup2(input_fd, STDIN_FILENO);
                close(input_fd);
            }

            if (output_fd != STDOUT_FILENO) {
                dup2(output_fd, STDOUT_FILENO);
                close(output_fd);
            }

            execvp(exe, new_args);
            perror("execvp");
            exit(EXIT_FAILURE);
        } else if (pid < 0) {
            // Fork failed
            perror("fork");
            exit(EXIT_FAILURE);
        } else {
            // Parent process
            int status;
            wait(&status);
        }
    }

    free(new_args);
    return 0;
}