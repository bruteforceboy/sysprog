#include "parser.h"
#include "runcommand.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>

#define AND_BOOL 1
#define OR_BOOL 0
#define NO_BOOL -1

#define MAX_CHILD_PROCS 10
#define BUFF_SIZE 1024

static bool contains_pipe(const struct command_line *line) {
    const struct expr *e = line->head;
    while (e != NULL) {
        if (e->type == EXPR_TYPE_PIPE)
            return true;
        e = e->next;
    }

    return false;
}

static int execute_echo_to_shell(const struct expr *e) {
    char **new_args = (char **) malloc((e->cmd.arg_count + 2) * sizeof(char *));

    if (new_args == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    new_args[0] = e->cmd.exe;
    for (int i = 0; i < (int) e->cmd.arg_count; i++)
        new_args[i + 1] = e->cmd.args[i];
    new_args[e->cmd.arg_count + 1] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        execvp(e->cmd.exe, new_args);
        perror("execvp");
        exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    } else {
        free(new_args);
        return pid;
    }
    free(new_args);
}

static void write_to_output(int input_fd, int output_fd) {
    char buffer[BUFF_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(input_fd, buffer, sizeof(buffer))) > 0) {
        write(output_fd, buffer, bytes_read);
    }
}

void execute_commands(const struct command_line *line, int *to_exit, int *exit_code) {
    const struct expr *e = line->head;
    int input_fd = STDIN_FILENO;
    int pipe_fd[2];


    pid_t *child_procs = (pid_t *) malloc(MAX_CHILD_PROCS * sizeof(pid_t));
    int cur_proc = 0;
    int first_expr = 1;
    int exit_set = 0;

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

    bool pipe_expression = contains_pipe(line);
    bool cur_expr_val = true;
    int last_bool = NO_BOOL;
    int false_eval = 0;

    while (e != NULL) {
        if (e->type == EXPR_TYPE_COMMAND) {
            if (pipe(pipe_fd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }

            if (strcmp(e->cmd.exe, "exit") == 0) {
                exit_set = 1;
            }

            if (e->next == NULL && output_fd == STDOUT_FILENO
                    && !pipe_expression && strcmp(e->cmd.exe, "echo") == 0
                    && !(last_bool == OR_BOOL && cur_expr_val == 1)) {
                child_procs[cur_proc++] = execute_echo_to_shell(e);
            } else {
                child_procs[cur_proc++] = execute_command(first_expr,
                                          (e->next == NULL ? 1 : 0),
                                          e->cmd.exe,
                                          e->cmd.arg_count,
                                          e->cmd.args,
                                          input_fd,
                                          pipe_fd[1],
                                          to_exit,
                                          exit_code,
                                          &last_bool,
                                          &cur_expr_val);

            }
            close(pipe_fd[1]);
            input_fd = pipe_fd[0];

        } else if (e->type == EXPR_TYPE_AND) {
            if (cur_expr_val == false) {
                false_eval = true;
                break;
            }
            write_to_output(input_fd, output_fd);
            last_bool = AND_BOOL;
        } else if (e->type == EXPR_TYPE_OR) {
            if (last_bool != OR_BOOL)
                write_to_output(input_fd, output_fd);
            last_bool = OR_BOOL;
        }

        e = e->next;
        first_expr = 0;
    }

    if (line->is_background)
        return;

    bool waited = false;

    for (int i = cur_proc - 1; i >= 0; i--) {
        if (child_procs[i] == 0) continue;
        if (!waited) {
            int status;
            waitpid(child_procs[i], &status, 0);
            waited = true;
            char *cur_exit_status = malloc(10);
            sprintf(cur_exit_status, "%d", WEXITSTATUS(status));

            char *endptr;
            long exit_long_value = strtol(cur_exit_status, &endptr, 10);
            if (*endptr == '\0' && !isspace(*endptr)) {
                int exit_int_value = (int) exit_long_value;
                if (i == cur_proc - 1 && exit_set == 0) {
                    *exit_code = exit_int_value;
                }
            }

            free(cur_exit_status);
        }
        kill(child_procs[i], SIGKILL);
    }

    free(child_procs);

    if (!false_eval && !(last_bool == OR_BOOL && cur_expr_val == 1))
        write_to_output(input_fd, output_fd);
}

int execute_command(int first_expr, int last_expr, char *exe,
                    int args_count, char **args, int input_fd,
                    int output_fd, int *to_exit, int *exit_code, int *last_bool,
                    bool *cur_expr_val) {
    char **new_args = (char **) malloc((args_count + 2) * sizeof(char *));

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
    } else if (strcmp(exe, "false") == 0) {
        if (*last_bool == AND_BOOL || *last_bool == NO_BOOL) {
            *cur_expr_val = false;
        }
    } else {
        if (*last_bool == OR_BOOL || *last_bool == NO_BOOL) {
            *cur_expr_val = true;
        }
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
