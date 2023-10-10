#pragma once

#include <stdio.h>

int execute_command(int last_expr, char* exe, int args_count, char** args, int input_fd, int output_fd);
void execute_pipeline(const struct command_line *line);