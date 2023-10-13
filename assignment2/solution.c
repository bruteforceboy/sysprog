#include "parser.h"
#include "command.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>  
#include <sys/types.h>
#include <sys/wait.h>

static void execute_command_line(int* to_exit, const struct command_line *line, int* exit_code) {
    assert(line != NULL);
    execute_commands(line, to_exit, exit_code);
}

int main(void) {
    // freopen("output.txt", "w", stdout);
    const size_t buf_size = 1;
    char buf[buf_size];
    int32_t rc;
    struct parser *p = parser_new();
    int to_exit = 0;
    int exit_code = 0;
    while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
        parser_feed(p, buf, rc);
        if (buf[0] == EOF)
            break;
        struct command_line *line = NULL;
        while (true) {
            enum parser_error err = parser_pop_next(p, &line);
            if (err == PARSER_ERR_NONE && line == NULL)
                break;
            if (err != PARSER_ERR_NONE) {
                printf("Error: %d\n", (int)err);
                continue;
            }
            execute_command_line(&to_exit, line, &exit_code);
            command_line_delete(line);
            if (to_exit) {
                parser_delete(p);
                exit(exit_code);
            }
        }
    }
    parser_delete(p);
    return exit_code;
}
