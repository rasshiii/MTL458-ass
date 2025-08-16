/*
  mini_shell.c
  A compact interactive shell with:
    - Prompt: msh$ 
    - External commands via fork() + execvp()
    - One pipeline segment (cmd1 | cmd2)
    - Basic I/O redirection: <, >, >> (not combined with pipes)
    - Command separators: ; and && (&& executes next only on success)
    - Globbing using glob()
    - Built-ins: cd, history, exit
    - Command history up to 2048 entries (supports: history and history n)
    - Simple Tab-based filename completion
  Target: POSIX/Linux
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
        int in_quotes = 0;
        char token_buf[MAXLINE];
        int tlen = 0;
        if (*p == '"') { in_quotes = 1; p++; }
        while (*p) {
            if (in_quotes) {
                if (*p == '"') { p++; break; }
            } else {
                if (isspace((unsigned char)*p)) break;
            }
            if (tlen < MAXLINE - 1) token_buf[tlen++] = *p;
            p++;
        }
        token_buf[tlen] = '\0';
        argv[argc++] = strdup(token_buf);
        if (argc >= MAXARGS) break;
        while (*p && isspace((unsigned char)*p)) p++;
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
        const char *arg = argv[i];
        if (strpbrk(arg, "*?[") != NULL) {
            glob_t results;
            memset(&results, 0, sizeof(results));
            int g = glob(arg, GLOB_NOCHECK, NULL, &results);
            if (g == 0) {
                for (size_t j = 0; j < results.gl_pathc && outc < MAXARGS; ++j) {
                    out[outc++] = strdup(results.gl_pathv[j]);
                }
                globfree(&results);
            } else {
                out[outc++] = strdup(arg);
            }
        } else {
            out[outc++] = strdup(arg);
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
            char *endptr = NULL;
            long n = strtol(argv[1], &endptr, 10);
            if (endptr == argv[1]) n = 0;
            do_history((int)n);
        }
        return 0;
    } else if (strcmp(argv[0], "exit") == 0) {
        for (int i = 0; i < hist_count; ++i) free(history[i]);
        exit(0);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("Invalid Command");
        return 1;
    } else if (pid == 0) {
        if (redirect_in_fd >= 0) {
            dup2(redirect_in_fd, STDIN_FILENO);
            close(redirect_in_fd);
        }
        if (redirect_out_fd >= 0) {
            dup2(redirect_out_fd, STDOUT_FILENO);
            close(redirect_out_fd);
        }
        execvp(argv[0], argv);
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

    pid_t right_pid = fork();
    if (right_pid < 0) {
        perror("Invalid Command");
        close(pipefd[0]); close(pipefd[1]);
        return 1;
    }
    if (right_pid == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(right_argv[0], right_argv);
        fprintf(stderr, "Invalid Command\n");
        _exit(127);
    }

    pid_t left_pid = fork();
    if (left_pid < 0) {
        perror("Invalid Command");
        close(pipefd[0]); close(pipefd[1]);
        return 1;
    }
    if (left_pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(left_argv[0], left_argv);
        fprintf(stderr, "Invalid Command\n");
        _exit(127);
    }

    close(pipefd[0]); close(pipefd[1]);
    int status_left, status_right;
    waitpid(left_pid, &status_left, 0);
    waitpid(right_pid, &status_right, 0);
    if (WIFEXITED(status_right)) return WEXITSTATUS(status_right);
    return 1;
}

/* Parse a single simple command (no pipes) for redirection. Returns 0 on success, fills argv_out & out_argc */
int parse_redirection_and_build_args(char *cmd, char ***argv_out, int *out_argc,
                                     int *in_fd, int *out_fd, int *append_flag) {
    char *copy = strdup(cmd);
    char *argv_tmp[MAXARGS];
    int argc_tmp = tokenize_args(copy, argv_tmp);

    *in_fd = -1; *out_fd = -1; *append_flag = 0;

    char **final_args = malloc(sizeof(char*)*(argc_tmp+1));
    int final_count = 0;

    for (int i = 0; i < argc_tmp; ++i) {
        char *tok = argv_tmp[i];
        if (tok[0] == '<' && tok[1] == '\0') {
            if (i+1 >= argc_tmp) {
                free(copy);
                free_argv(argv_tmp, argc_tmp);
                free(final_args);
                return -1;
            }
            int fd = open(argv_tmp[i+1], O_RDONLY);
            if (fd < 0) {
                free(copy);
                free_argv(argv_tmp, argc_tmp);
                free(final_args);
                return -2;
            }
            *in_fd = fd;
            i++;
        } else if ((tok[0] == '>' && tok[1] == '\0') || (tok[0] == '>' && tok[1] == '>' && tok[2] == '\0')) {
            int isappend = (tok[1] == '>');
            if (i+1 >= argc_tmp) {
                free(copy);
                free_argv(argv_tmp, argc_tmp);
                free(final_args);
                return -1;
            }
            int fd = open(argv_tmp[i+1], O_WRONLY | O_CREAT | (isappend ? O_APPEND : O_TRUNC), 0644);
            if (fd < 0) {
                free(copy);
                free_argv(argv_tmp, argc_tmp);
                free(final_args);
                return -2;
            }
            *out_fd = fd;
            *append_flag = isappend;
            i++;
        } else {
            final_args[final_count++] = strdup(tok);
        }
    }
    final_args[final_count] = NULL;

    int expanded_count;
    char **expanded = expand_wildcards(final_args, final_count, &expanded_count);

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
    raw_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio);

    char buf[MAXLINE];
    int len = 0;
    memset(buf, 0, sizeof(buf));
    printf("msh$ ");
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
        } else if (c == 127 || c == 8) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c == '\t') {
            int i = len - 1;
            while (i >= 0 && !isspace((unsigned char)buf[i])) i--;
            int start = i + 1;
            int plen = len - start;
            if (plen <= 0) continue;
            char prefix[512];
            if (plen >= (int)sizeof(prefix)) continue;
            memcpy(prefix, buf + start, plen);
            prefix[plen] = '\0';

            DIR *dir = opendir(".");
            if (!dir) continue;
            int matches = 0;
            char single[1024];
            single[0] = '\0';
            struct dirent *de;
            while ((de = readdir(dir)) != NULL) {
                if (strncmp(de->d_name, prefix, plen) == 0) {
                    matches++;
                    if (matches == 1) {
                        strncpy(single, de->d_name, sizeof(single)-1);
                        single[sizeof(single)-1] = '\0';
                    } else {
                        // more than one match -> do nothing
                    }
                    if (matches > 1) {
                        // already more than one, no need to keep scanning strictly
                    }
                }
            }
            closedir(dir);
            if (matches == 1) {
                int addlen = (int)strlen(single) - plen;
                if (addlen > 0) {
                    if (len + addlen >= MAXLINE - 1) addlen = MAXLINE - 1 - len;
                    memcpy(buf + len, single + plen, addlen);
                    len += addlen;
                    buf[len] = '\0';
                    printf("%s", single + plen);
                    fflush(stdout);
                }
            }
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
    int cap = 16;
    char **cmds = malloc(sizeof(char*)*cap);
    int *types = malloc(sizeof(int)*cap);
    int c = 0;

    int in_quotes = 0;
    char *segment_start = line;

    for (char *p = line; *p; ++p) {
        if (*p == '"') in_quotes = !in_quotes;
        if (!in_quotes) {
            if (*p == ';') {
                int len = p - segment_start;
                char *piece = malloc(len + 1);
                strncpy(piece, segment_start, len);
                piece[len] = '\0';
                cmds[c] = trim(piece);
                types[c] = 0;
                c++;
                if (c >= cap) { cap *= 2; cmds = realloc(cmds, sizeof(char*)*cap); types = realloc(types, sizeof(int)*cap); }
                segment_start = p + 1;
                while (*segment_start && isspace((unsigned char)*segment_start)) segment_start++;
            } else if (*p == '&' && *(p+1) == '&') {
                int len = p - segment_start;
                char *piece = malloc(len + 1);
                strncpy(piece, segment_start, len);
                piece[len] = '\0';
                cmds[c] = trim(piece);
                types[c] = 1;
                c++;
                if (c >= cap) { cap *= 2; cmds = realloc(cmds, sizeof(char*)*cap); types = realloc(types, sizeof(int)*cap); }
                p++; // skip second '&'
                segment_start = p + 1;
                while (*segment_start && isspace((unsigned char)*segment_start)) segment_start++;
            }
        }
    }
    if (*segment_start) {
        char *piece = strdup(segment_start);
        cmds[c] = trim(piece);
        types[c] = 0;
        c++;
    } else {
        // trailing separator -> add empty piece so loop logic remains consistent
        char *piece = strdup("");
        cmds[c] = piece;
        types[c] = 0;
        c++;
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
    // find '|' outside quotes
    int in_quotes = 0;
    char *p = piece;
    char *pipe_pos = NULL;
    for (; *p; ++p) {
        if (*p == '"') in_quotes = !in_quotes;
        if (!in_quotes && *p == '|') { pipe_pos = p; break; }
    }

    if (pipe_pos) {
        char *left = strndup(piece, pipe_pos - piece);
        char *right = strdup(pipe_pos + 1);
        left = trim(left); right = trim(right);

        char **left_argv; int left_argc;
        int in_fd_left, out_fd_left, append_left;
        if (parse_redirection_and_build_args(left, &left_argv, &left_argc, &in_fd_left, &out_fd_left, &append_left) != 0) {
            free(left); free(right);
            return 1;
        }
        char **right_argv; int right_argc;
        int in_fd_right, out_fd_right, append_right;
        if (parse_redirection_and_build_args(right, &right_argv, &right_argc, &in_fd_right, &out_fd_right, &append_right) != 0) {
            free(left); free(right);
            free_expanded(left_argv, left_argc);
            return 1;
        }

        if (in_fd_left >= 0 || out_fd_left >= 0 || in_fd_right >= 0 || out_fd_right >= 0) {
            fprintf(stderr, "Invalid Command\n");
            free(left); free(right);
            free_expanded(left_argv, left_argc);
            free_expanded(right_argv, right_argc);
            return 1;
        }

        int status = execute_pipe(left_argv, left_argc, right_argv, right_argc);

        free(left); free(right);
        free_expanded(left_argv, left_argc);
        free_expanded(right_argv, right_argc);
        return status;
    } else {
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
