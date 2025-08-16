/*
  entrynumber_a1.c
  MTL458 Assignment 1 - Basic Shell
  Features:
    - Prompt: MTL458 >
    - Execute commands via fork() + execvp()
    - Single pipe support (cmd1 | cmd2)
    - Single I/O redirection: <, >, >> (no pipes combined with redirection)
    - Command separators: ; and && (&& executes next only on success)
    - Wildcard expansion using glob()
    - Built-ins: cd, history, exit
    - Command history up to 2048 entries (history and history n)
    - Filename auto-completion via Tab (simple)
    - Error message on invalid commands: "Invalid Command"
  Notes:
    - Does NOT use readline.
    - Designed for POSIX (Linux). Use WSL / Cygwin / Linux VM to run on Windows.
*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <glob.h>
#include <errno.h>
#include <sys/types.h>
#include <termios.h>
#include <dirent.h>
#include <ctype.h>

#define MAXLINE 2048
#define MAXARGS 100
#define HISTORY_MAX 2048

/* History storage */
static char *history[HISTORY_MAX];
static int hist_count = 0;

/* Save command to history (store the raw line) */
void add_history(const char *line) {
    if (!line || line[0] == '\0') return;
    if (hist_count == HISTORY_MAX) {
        free(history[0]);
        memmove(history, history+1, sizeof(char*)*(HISTORY_MAX-1));
        hist_count--;
    }
    history[hist_count++] = strdup(line);
}

/* Print history: 'history' prints all; 'history n' prints last n (oldest->newest per spec) */
void do_history(int n) {
    if (n <= 0 || n > hist_count) {
        // print all
        for (int i = 0; i < hist_count; ++i) {
            printf("%s\n", history[i]);
        }
    } else {
        // print last n, but oldest->latest among those
        int start = hist_count - n;
        if (start < 0) start = 0;
        for (int i = start; i < hist_count; ++i) {
            printf("%s\n", history[i]);
        }
    }
}

/* Trim leading/trailing whitespace */
char *trim(char *s) {
    if (!s) return s;
    while(isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while(end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

/* Split string into tokens by whitespace respecting quoted strings (double quotes) */
int tokenize_args(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        if (*p == '"') {
            p++;
            char *start = p;
            while (*p && *p != '"') p++;
            int len = p - start;
            argv[argc] = malloc(len + 1);
            strncpy(argv[argc], start, len);
            argv[argc][len] = '\0';
            argc++;
            if (*p == '"') p++;
        } else {
            char *start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            int len = p - start;
            argv[argc] = malloc(len + 1);
            strncpy(argv[argc], start, len);
            argv[argc][len] = '\0';
            argc++;
        }
        if (argc >= MAXARGS) break;
    }
    argv[argc] = NULL;
    return argc;
}

/* Free argv allocated by tokenize_args */
void free_argv(char **argv, int argc) {
    for (int i = 0; i < argc; ++i) free(argv[i]);
}

/* Expand wildcards in argv using glob; returns new argv allocated via malloc; new_argc set */
char **expand_wildcards(char **argv, int argc, int *new_argc) {
    char **out = malloc(sizeof(char*)*(MAXARGS+1));
    int outc = 0;
    for (int i = 0; i < argc; ++i) {
        if (strchr(argv[i], '*') || strchr(argv[i], '?') || strchr(argv[i], '[')) {
            glob_t results;
            int g = glob(argv[i], 0, NULL, &results);
            if (g == 0) {
                for (size_t j = 0; j < results.gl_pathc; ++j) {
                    out[outc++] = strdup(results.gl_pathv[j]);
                }
                globfree(&results);
            } else {
                // No matches: keep pattern as-is
                out[outc++] = strdup(argv[i]);
            }
        } else {
            out[outc++] = strdup(argv[i]);
        }
        if (outc >= MAXARGS) break;
    }
    out[outc] = NULL;
    *new_argc = outc;
    return out;
}

/* Free array returned by expand_wildcards */
void free_expanded(char **arr, int cnt) {
    for (int i = 0; i < cnt; ++i) free(arr[i]);
    free(arr);
}

/* Execute non-piped command with optional redirection. Returns exit status (0 on success) */
int execute_command(char **argv, int argc, int redirect_in_fd, int redirect_out_fd, int append_out) {
    if (argc == 0) return 0;

    // Built-ins: cd, history, exit
    if (strcmp(argv[0], "cd") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Invalid Command\n");
            return 1;
        }
        if (chdir(argv[1]) != 0) {
            perror("Invalid Command");
            return 1;
        }
        return 0;
    } else if (strcmp(argv[0], "history") == 0) {
        if (argc == 1) {
            do_history(0);
        } else {
            // parse integer
            int n = atoi(argv[1]);
            do_history(n);
        }
        return 0;
    } else if (strcmp(argv[0], "exit") == 0) {
        // free history
        for (int i = 0; i < hist_count; ++i) free(history[i]);
        exit(0);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("Invalid Command");
        return 1;
    } else if (pid == 0) {
        // child
        if (redirect_in_fd >= 0) {
            dup2(redirect_in_fd, STDIN_FILENO);
            close(redirect_in_fd);
        }
        if (redirect_out_fd >= 0) {
            dup2(redirect_out_fd, STDOUT_FILENO);
            close(redirect_out_fd);
        }
        execvp(argv[0], argv);
        // If exec fails:
        fprintf(stderr, "Invalid Command\n");
        _exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return 1;
    }
}

/* Execute pipeline of two commands: left | right. Both cmds are argv arrays. */
int execute_pipe(char **left_argv, int left_argc, char **right_argv, int right_argc) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("Invalid Command");
        return 1;
    }

    pid_t p1 = fork();
    if (p1 < 0) {
        perror("Invalid Command");
        return 1;
    }
    if (p1 == 0) {
        // left child: write end -> stdout
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(left_argv[0], left_argv);
        fprintf(stderr, "Invalid Command\n");
        _exit(127);
    }

    pid_t p2 = fork();
    if (p2 < 0) {
        perror("Invalid Command");
        return 1;
    }
    if (p2 == 0) {
        // right child: read end -> stdin
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(right_argv[0], right_argv);
        fprintf(stderr, "Invalid Command\n");
        _exit(127);
    }

    // parent
    close(pipefd[0]); close(pipefd[1]);
    int status1, status2;
    waitpid(p1, &status1, 0);
    waitpid(p2, &status2, 0);
    if (WIFEXITED(status2)) return WEXITSTATUS(status2);
    return 1;
}

/* Parse a single simple command (no pipes) for redirection. Returns 0 on success, fills argv_out & out_argc */
int parse_redirection_and_build_args(char *cmd, char ***argv_out, int *out_argc,
                                     int *in_fd, int *out_fd, int *append_flag) {
    // We'll tokenise, then detect <, >, >>
    char *copy = strdup(cmd);
    char *tokens[MAXARGS];
    int tcount = 0;
    char *p = copy;
    // naive split by whitespace but keep quoted handled by tokenize_args
    char *argv_tmp[MAXARGS];
    int argc_tmp = tokenize_args(copy, argv_tmp);

    // prepare default fds
    *in_fd = -1; *out_fd = -1; *append_flag = 0;

    // Scan for redirection tokens
    char **final_args = malloc(sizeof(char*)*(argc_tmp+1));
    int final_count = 0;
    int i = 0;
    while (i < argc_tmp) {
        if (strcmp(argv_tmp[i], "<") == 0) {
            if (i+1 >= argc_tmp) {
                free(copy);
                free_argv(argv_tmp, argc_tmp);
                free(final_args);
                return -1;
            }
            int fd = open(argv_tmp[i+1], O_RDONLY);
            if (fd < 0) {
                // file open error
                free(copy);
                free_argv(argv_tmp, argc_tmp);
                free(final_args);
                return -2;
            }
            *in_fd = fd;
            i += 2;
        } else if (strcmp(argv_tmp[i], ">") == 0 || strcmp(argv_tmp[i], ">>") == 0) {
            int isappend = (strcmp(argv_tmp[i], ">>") == 0);
            if (i+1 >= argc_tmp) {
                free(copy);
                free_argv(argv_tmp, argc_tmp);
                free(final_args);
                return -1;
            }
            int fd;
            if (isappend) {
                fd = open(argv_tmp[i+1], O_WRONLY | O_CREAT | O_APPEND, 0644);
            } else {
                fd = open(argv_tmp[i+1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            }
            if (fd < 0) {
                free(copy);
                free_argv(argv_tmp, argc_tmp);
                free(final_args);
                return -2;
            }
            *out_fd = fd;
            *append_flag = isappend;
            i += 2;
        } else {
            final_args[final_count++] = strdup(argv_tmp[i]);
            i++;
        }
    }
    final_args[final_count] = NULL;

    // expand wildcards
    int expanded_count;
    char **expanded = expand_wildcards(final_args, final_count, &expanded_count);

    // cleanup
    free(copy);
    free_argv(argv_tmp, argc_tmp);
    for (int k = 0; k < final_count; ++k) free(final_args[k]);
    free(final_args);

    *argv_out = expanded;
    *out_argc = expanded_count;
    return 0;
}

/* Read a line with basic line-editing and Tab completion.
   Tab completion: completes the current token if exactly one match exists.
*/
char *read_line_with_tab() {
    struct termios orig_tio, raw_tio;
    tcgetattr(STDIN_FILENO, &orig_tio);
    raw_tio = orig_tio;
    raw_tio.c_lflag &= ~(ICANON | ECHO); // disable canonical mode and echo
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio);

    char buf[MAXLINE];
    int len = 0;
    memset(buf, 0, sizeof(buf));
    printf("MTL458 > ");
    fflush(stdout);

    while (1) {
        int c = getchar();
        if (c == EOF) {
            buf[len] = '\0';
            putchar('\n');
            break;
        } else if (c == '\n') {
            buf[len] = '\0';
            putchar('\n');
            break;
        } else if (c == 127 || c == 8) { // backspace
            if (len > 0) {
                len--;
                // erase char from terminal
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c == '\t') {
            // perform tab completion: find current token start
            int i = len - 1;
            while (i >= 0 && !isspace((unsigned char)buf[i])) i--;
            int start = i + 1;
            char prefix[512];
            int plen = len - start;
            if (plen <= 0) continue;
            strncpy(prefix, buf + start, plen);
            prefix[plen] = '\0';

            // Use glob to find matches for prefix*
            char pattern[1024];
            snprintf(pattern, sizeof(pattern), "%s*", prefix);
            glob_t results;
            int g = glob(pattern, 0, NULL, &results);
            if (g == 0 && results.gl_pathc == 1) {
                // single match -> complete
                char *match = results.gl_pathv[0];
                int addlen = strlen(match) - plen;
                if (addlen > 0) {
                    // append to buffer and display
                    if (len + addlen >= MAXLINE - 1) addlen = MAXLINE - 1 - len;
                    memcpy(buf + len, match + plen, addlen);
                    len += addlen;
                    buf[len] = '\0';
                    printf("%s", match + plen);
                    fflush(stdout);
                }
            } else {
                // multiple or zero matches: do nothing (could show list, but assignment not require)
            }
            globfree(&results);
        } else {
            if (len < MAXLINE - 1) {
                buf[len++] = (char)c;
                putchar(c);
                fflush(stdout);
            }
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
    return strdup(buf);
}

/* Split a line by separators ; and && while keeping their types.
   Returns arrays: commands[] and separators[] where separators[i] is:
      0 => ';' or end
      1 => '&&'
   The number of commands returned is stored in *count.
*/
char **split_by_separators(char *line, int *count, int **sep_types) {
    // We'll scan and split
    char *s = line;
    int cap = 16;
    char **cmds = malloc(sizeof(char*)*cap);
    int *types = malloc(sizeof(int)*cap);
    int c = 0;

    while (*s) {
        // find next separator ; or &&
        char *p = s;
        char *next_sep = NULL;
        int sep_type = 0;
        while (*p) {
            if (p[0] == ';') { next_sep = p; sep_type = 0; break; }
            if (p[0] == '&' && p[1] == '&') { next_sep = p; sep_type = 1; break; }
            p++;
        }
        if (next_sep) {
            int len = next_sep - s;
            char *piece = malloc(len+1);
            strncpy(piece, s, len);
            piece[len] = '\0';
            cmds[c] = trim(piece);
            types[c] = sep_type;
            c++;
            if (c >= cap) {
                cap *= 2;
                cmds = realloc(cmds, sizeof(char*)*cap);
                types = realloc(types, sizeof(int)*cap);
            }
            if (sep_type == 1) s = next_sep + 2;
            else s = next_sep + 1;
            while (*s && isspace((unsigned char)*s)) s++;
        } else {
            // last piece
            char *piece = strdup(s);
            cmds[c] = trim(piece);
            types[c] = 0;
            c++;
            break;
        }
    }
    *count = c;
    *sep_types = types;
    return cmds;
}

/* Free results from split_by_separators */
void free_split(char **cmds, int count, int *types) {
    for (int i = 0; i < count; ++i) free(cmds[i]);
    free(cmds);
    free(types);
}

/* Process a single command piece (may contain a pipe) and execute. Returns exit status. */
int process_piece(char *piece) {
    // Check for pipe '|'. Only single pipe supported.
    char *pipe_pos = strchr(piece, '|');
    if (pipe_pos) {
        // left and right
        char *left = strndup(piece, pipe_pos - piece);
        char *right = strdup(pipe_pos + 1);
        left = trim(left); right = trim(right);

        // parse redirection and build args for left and right (redirection not allowed with pipes per assumptions of assignment)
        char **left_argv; int left_argc;
        int in_fd_left, out_fd_left, append_left;
        if (parse_redirection_and_build_args(left, &left_argv, &left_argc, &in_fd_left, &out_fd_left, &append_left) != 0) {
            // error in parsing
            free(left); free(right);
            return 1;
        }
        char **right_argv; int right_argc;
        int in_fd_right, out_fd_right, append_right;
        if (parse_redirection_and_build_args(right, &right_argv, &right_argc, &in_fd_right, &out_fd_right, &append_right) != 0) {
            // error
            free(left); free(right);
            free_expanded(left_argv, left_argc);
            return 1;
        }

        // We won't support redirection combined with pipe to simplify: if any redirection fds present, error
        if (in_fd_left >= 0 || out_fd_left >= 0 || in_fd_right >= 0 || out_fd_right >= 0) {
            fprintf(stderr, "Invalid Command\n");
            free(left); free(right);
            free_expanded(left_argv, left_argc);
            free_expanded(right_argv, right_argc);
            return 1;
        }

        // execute pipe
        int status = execute_pipe(left_argv, left_argc, right_argv, right_argc);

        free(left); free(right);
        free_expanded(left_argv, left_argc);
        free_expanded(right_argv, right_argc);
        return status;
    } else {
        // no pipe -> possibly redirection
        char **argv; int argc;
        int in_fd, out_fd, append_flag;
        int pr = parse_redirection_and_build_args(piece, &argv, &argc, &in_fd, &out_fd, &append_flag);
        if (pr == -1) {
            fprintf(stderr, "Invalid Command\n");
            return 1;
        } else if (pr == -2) {
            fprintf(stderr, "Invalid Command\n");
            return 1;
        }
        int status = execute_command(argv, argc, in_fd, out_fd, append_flag);

        if (in_fd >= 0) close(in_fd);
        if (out_fd >= 0) close(out_fd);
        free_expanded(argv, argc);
        return status;
    }
}

int main(int argc, char **argv) {
    while (1) {
        char *line = read_line_with_tab();
        if (!line) break;
        char *trimline = trim(line);
        if (strlen(trimline) == 0) { free(line); continue; }

        // Add to history (save original)
        add_history(trimline);

        // Split by separators ; and &&
        int piece_count;
        int *sep_types;
        char **pieces = split_by_separators(trimline, &piece_count, &sep_types);

        int last_status = 0;
        for (int i = 0; i < piece_count; ++i) {
            char *piece = pieces[i];
            if (strlen(piece) == 0) {
                last_status = 0;
                continue;
            }

            // process piece
            last_status = process_piece(piece);

            // if next separator is && and last_status non-zero -> skip until next separator
            if (i+1 < piece_count && sep_types[i+1] == 1) {
                // sep_types index i corresponds to this piece's separator? Our split placed types[i] for this piece.
                // We used types[i] as the separator following the ith piece. So check types[i] (not i+1).
            }

            // Implement semantics: if the separator after this piece is '&&' and last_status != 0 then skip next piece(s) until after that chain
            if (i < piece_count-1 && sep_types[i] == 1 && last_status != 0) {
                // skip next piece
                i++;
                // continue skipping chained && sequences where previous failed
                while (i < piece_count-1 && sep_types[i] == 1) {
                    i++;
                }
            }
        }

        free_split(pieces, piece_count, sep_types);
        free(line);
    }
    return 0;
}
