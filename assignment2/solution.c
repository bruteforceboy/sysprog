#include "parser.h"
#include "command.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static bool contains_pipe(const struct command_line *line) {
    const struct expr *e = line->head;

    while (e != NULL) {
        if (e->type == EXPR_TYPE_PIPE) {
            return true;
        }
        e = e->next;
    }

    return false;
}

static void execute_command_line(int* to_exit, const struct command_line *line) {
    assert(line != NULL);

    if (contains_pipe(line)) {
        execute_pipeline(line);
    } else {
        const struct expr *e = line->head;
        int input_fd = STDIN_FILENO; // Default input

        while (e != NULL) {
            if (e->type == EXPR_TYPE_COMMAND) {
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

                // Execute the current command with appropriate input and output
                *to_exit = execute_command((e->next == NULL ? 1 : 0), e->cmd.exe, e->cmd.arg_count, e->cmd.args, input_fd, output_fd);

                if (to_exit) {
                    return;
                }

                // Close the output file descriptor if it was opened
                if (output_fd != STDOUT_FILENO) {
                    close(output_fd);
                }
            } else if (e->type == EXPR_TYPE_PIPE) {
                assert(false);
            } else if (e->type == EXPR_TYPE_AND) {
                printf("\tAND\n");
            } else if (e->type == EXPR_TYPE_OR) {
                printf("\tOR\n");
            } else {
                assert(false);
            }

            e = e->next;
        }
    }
}

int main(void) {
    const size_t buf_size = 1024;
    char buf[buf_size];
    int rc;
    struct parser *p = parser_new();
    int to_exit = 0;
    while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
        parser_feed(p, buf, rc);
        struct command_line *line = NULL;
        while (true) {
            enum parser_error err = parser_pop_next(p, &line);
            if (err == PARSER_ERR_NONE && line == NULL)
                break;
            if (err != PARSER_ERR_NONE) {
                printf("Error: %d\n", (int)err);
                continue;
            }
            execute_command_line(&to_exit, line);
            command_line_delete(line);
            if (to_exit)
                goto do_parse_del;
        }
    }
do_parse_del:
    parser_delete(p);
    return 0;
}
