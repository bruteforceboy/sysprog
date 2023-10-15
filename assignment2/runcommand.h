#pragma once

#include <stdio.h>
#include <sys/types.h> 

int execute_command(int first_expr, int last_expr, char *exe, int args_count, char **args, int input_fd, int output_fd, int *to_exit,
    int *exit_code, int *last_bool, bool *cur_expr_val);
void execute_commands(const struct command_line *line, int *to_exit, int *exit_code);