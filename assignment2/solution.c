#include "parser.h"
#include "runcommand.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#define BUFF_SIZE 1024

static void execute_command_line(int* to_exit, const struct command_line *line, int* exit_code) {
    assert(line != NULL);
    execute_commands(line, to_exit, exit_code);
}

int main(void) {
    size_t buf_size = BUFF_SIZE;
    char *buf = (char *) malloc(buf_size);
    size_t rc;
    struct parser *p = parser_new();
    int to_exit = 0;
    int exit_code = 0;
    while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
        parser_feed(p, buf, rc);
        struct command_line *line = NULL;
        while (true) {
            enum parser_error err = parser_pop_next(p, &line);
            if (err == PARSER_ERR_NONE && line == NULL) {
                break;
            }
            if (err != PARSER_ERR_NONE) {
                printf("Error: %d\n", (int)err);
                continue;
            }
            execute_command_line(&to_exit, line, &exit_code);
            command_line_delete(line);
            if (to_exit) {
                free(buf);
                parser_delete(p);
                exit(exit_code);
            }
        }
    }
    free(buf);
    parser_delete(p);
    return exit_code;
}