#include "parser.h"
#include "command.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

void execute_commands(const struct command_line *line,
                      int *to_exit, int *exit_code) {
    assert(line != NULL);

    const struct expr *e = line->head;
    int input_fd = STDIN_FILENO;
    int pipe_fd[2];

    pid_t *child_procs = (pid_t *) malloc(10 * sizeof(pid_t));
    int cur_proc = 0;
    int first_expr = 1;
    int exit_set = 0;

    while (e != NULL) {
        if (e->type == EXPR_TYPE_COMMAND) {
            if (pipe(pipe_fd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }

            if (strcmp(e->cmd.exe, "exit") == 0) {
                exit_set = 1;
            }
            child_procs[cur_proc++] = execute_command(first_expr, (e->next == NULL ? 1 : 0),
                                      e->cmd.exe, e->cmd.arg_count, e->cmd.args, input_fd,
                                      pipe_fd[1], to_exit, exit_code);
            close(pipe_fd[1]);
            input_fd = pipe_fd[0];

            e = e->next;
        } else {
            e = e->next;
        }
        first_expr = 0;
    }

    bool waited = false;

    for (int i = cur_proc - 1; i >= 0; i--) {
        if (child_procs[i] == 0) continue;
        if (!waited) {
            int status;
            waitpid(child_procs[i], &status, 0);
            waited = true;
            char *cur_exit_status = malloc(10);
            sprintf(cur_exit_status, "%d", WEXITSTATUS(status));
            int value = atoi(cur_exit_status);
            if (i == cur_proc - 1 && exit_set == 0) {
                *exit_code = value;
            }
            free(cur_exit_status);
        }
        kill(child_procs[i], SIGKILL);
    }

    int output_fd = STDOUT_FILENO; // Default output

    if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
        output_fd = open(line->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (output_fd == -1) {
            perror("open");
            exit(EXIT_FAILURE);
        }
    } else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
        output_fd = open(line->out_file, O_WRONLY | O_APPEND, 0666);
        if (output_fd == -1) {
            perror("open");
            exit(EXIT_FAILURE);
        }
    }

    char buffer[1024];
    ssize_t bytes_read;

    while ((bytes_read = read(input_fd, buffer, sizeof(buffer))) > 0) {
        write(output_fd, buffer, bytes_read);
    }
}

int execute_command(int first_expr, int last_expr, char *exe,
                    int args_count, char **args, int input_fd,
                    int output_fd, int *to_exit, int *exit_code) {
    char **new_args = (char **)malloc((args_count + 2) * sizeof(char *));

    if (new_args == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    new_args[0] = exe;
    for (int i = 0; i < args_count; i++)
        new_args[i + 1] = args[i];
    new_args[args_count + 1] = NULL;

    if (strcmp(exe, "cd") == 0) {
        if (args_count >= 1) {
            if (chdir(args[0]) != 0) {
                perror("chdir");
            }
        }
    } else if (strcmp(exe, "exit") == 0) {
        if (last_expr == 1 && args_count == 0) {
            free(new_args);
            *to_exit = 1;
            return 0;
        } else if (first_expr == 1 && last_expr == 1) {
            *to_exit = 1;
            *exit_code = atoi(args[0]);
            return 0;
        } else {
            if (args_count > 0) {
                *exit_code = atoi(args[0]);
            }
            return 0;
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
            perror("fork");
            exit(EXIT_FAILURE);
        } else {
            free(new_args);
            return pid;
        }
    }

    free(new_args);
    return 0;
}
