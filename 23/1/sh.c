#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

// Simplifed xv6 shell.

#define MAXARGS 10

// All commands have at least a type. Have looked at the type, the code
// typically casts the *cmd to some specific cmd type.
struct cmd {
    int type;          //  ' ' (exec), | (pipe), '<' or '>' for redirection
    int isBackground;
};

struct execcmd {
    int type;              // ' '
    int isBackground;
    char *argv[MAXARGS];   // arguments to the command to be exec-ed
};

struct redircmd {
    int type;          // < or >
    int isBackground;
    struct cmd *cmd;   // the command to be run (e.g., an execcmd)
    char *file;        // the input/output file
    int mode;          // the mode to open the file with
    int fd;            // the file descriptor number to use for the file
};

struct pipecmd {
    int type;          // |
    int isBackground;
    struct cmd *left;  // left side of pipe
    struct cmd *right; // right side of pipe
};

int fork_or_exit(void);  // Fork but exits on failure
struct cmd *parsecmd(char *);

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd) {
    struct execcmd *ecmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0) {
        exit(0);
    }

    switch (cmd->type) {
        default: {
            fprintf(stderr, "Unknown runcmd\n");
            exit(-1);
        }

        case ' ': {
            ecmd = (struct execcmd *) cmd;
            if (execvp(ecmd->argv[0], ecmd->argv) == -1) {
                fprintf(stderr, "Error while invoking exec command: %s\n", strerror(errno));
            }
            break;
        }

        case '>':
        case '<':
            rcmd = (struct redircmd *) cmd;
            // Create with 00700 permissions
            int fd = open(rcmd->file, rcmd->mode, S_IRWXU);
            if (fd == -1) {
                fprintf(stderr, "Failed to open file: %s\n", strerror(errno));
            }
            // Replace in/out with opened file
            dup2(fd, rcmd->fd);
            // Close unused file
            close(fd);
            runcmd(rcmd->cmd);
            break;


        case '|': {
            /*
             * For a | b | c
             * run 'a' in current process and 'b | c' in forked
             * with in/out redirection
             */
            pcmd = (struct pipecmd *) cmd;

            // [0] for reading, [1] for writing
            int pipe_fds[2];
            // Creates kind a in/out files in pipefs
            if (pipe(pipe_fds) == -1) {
                fprintf(stderr, "Error executing pipe system call: %s\n", strerror(errno));
            }

            if (fork_or_exit()) {
                // Close unused writing fd in child
                close(pipe_fds[1]);
                // New process will read from stdin,
                // but should read from pipe_fds[0] => use dup2
                if (dup2(pipe_fds[0], STDIN_FILENO) == -1) {
                    fprintf(stderr, "Error in dup2 process: %s\n", strerror(errno));
                    exit(-1);
                }
                // Now it's unused
                close(pipe_fds[0]);
                runcmd(pcmd->right);
            } else {
                // Close unused reading fd in parent
                close(pipe_fds[0]);
                // Process will write to stdout,
                // but should write to pipe_fds[1] => use dup2
                if (dup2(pipe_fds[1], STDOUT_FILENO) == -1) {
                    fprintf(stderr, "Error in dup2 process: %s\n", strerror(errno));
                    exit(-1);
                }
                // Now it's unused
                close(pipe_fds[1]);
                runcmd(pcmd->left);
            }

            break;
        }
    }
    exit(0);
}

int
getcmd(char *buf, int nbuf) {

    if (isatty(fileno(stdin)))
        fprintf(stdout, "$ ");
    memset(buf, 0, nbuf);
    fgets(buf, nbuf, stdin);
    if (buf[0] == 0) // EOF
        return -1;
    return 0;
}

int main(void) {
    static char buf[100];
    int status;

    // Read and run input commands.
    while (getcmd(buf, sizeof(buf)) >= 0) {
        if (buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
            // Clumsy but will have to do for now.
            // Chdir has no effect on the parent if run in the child.
            buf[strlen(buf) - 1] = 0;  // chop \n
            if (chdir(buf + 3) < 0) {
                fprintf(stderr, "cannot cd %s\n", buf + 3);
            }
            continue;
        }

        struct cmd *cmd = parsecmd(buf);
        int pid;
        if ((pid = fork_or_exit()) == 0) {
            runcmd(cmd);
        }

        if (!cmd->isBackground) {
            // Write result to status, but nobody cares
            waitpid(pid, &status, 0);
        } else {
            fprintf(stdout, "[1] %d\n", pid);
        }
    }
    exit(0);
}

int fork_or_exit(void) {
    int pid;

    pid = fork();
    if (pid == -1) {
        fprintf(stderr, "Error forking process: %s\n", strerror(errno));
        exit(-1);
    }
    return pid;
}

struct cmd *
execcmd(void) {
    struct execcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = ' ';
    return (struct cmd *) cmd;
}

struct cmd *
redircmd(struct cmd *subcmd, char *file, int type) {
    struct redircmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->mode = (type == '<') ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
    cmd->fd = (type == '<') ? 0 : 1;
    return (struct cmd *) cmd;
}

struct cmd *
pipecmd(struct cmd *left, struct cmd *right) {
    struct pipecmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = '|';
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *) cmd;
}

// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>";

int
gettoken(char **ps, char *es, char **q, char **eq) {
    char *s;
    int ret;

    s = *ps;
    while (s < es && strchr(whitespace, *s))
        s++;
    if (q)
        *q = s;
    ret = *s;
    switch (*s) {
        case 0:
            break;
        case '|':
        case '<':
            s++;
            break;
        case '>':
            s++;
            break;
        default:
            ret = 'a';
            while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
                s++;
            break;
    }
    if (eq)
        *eq = s;

    while (s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return ret;
}

int
peek(char **ps, char *es, char *toks) {
    char *s;

    s = *ps;
    while (s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);

struct cmd *parsepipe(char **, char *);

struct cmd *parseexec(char **, char *);

// make a copy of the characters in the input buffer, starting from s through es.
// null-terminate the copy to make it a string.
char
*mkcopy(char *s, char *es) {
    int n = es - s;
    char *c = malloc(n + 1);
    assert(c);
    strncpy(c, s, n);
    c[n] = 0;
    return c;
}

struct cmd *parsecmd(char *string) {
    char *end_of_string;
    struct cmd *cmd;

    size_t length = strlen(string);
    end_of_string = string + length;

    /*
     * Check that command line ends
     * with '&' and whitespaces.
     * If so, treat cmd as background one
     * and trim string to '&' position
     * (so it will be not parsed), else
     * just ignore presence of '&'
     */
    int isBackground = 0;
    char *iterator = end_of_string;
    while (iterator >= string) {
        if (strchr(whitespace, *iterator)) {
            --iterator;
        } else {
            if (*iterator == '&') {
                isBackground = 1;
                // Make it disappear
                *iterator = ' ';
            }
            break;
        }
    }

    cmd = parseline(&string, end_of_string);
    peek(&string, end_of_string, "");
    if (string != end_of_string) {
        fprintf(stderr, "leftovers: %s\n", string);
        exit(-1);
    }

    cmd->isBackground = isBackground;
    return cmd;
}

struct cmd *
parseline(char **ps, char *es) {
    struct cmd *cmd;
    cmd = parsepipe(ps, es);
    return cmd;
}

struct cmd *
parsepipe(char **ps, char *es) {
    struct cmd *cmd;

    cmd = parseexec(ps, es);
    if (peek(ps, es, "|")) {
        gettoken(ps, es, 0, 0);
        cmd = pipecmd(cmd, parsepipe(ps, es));
    }
    return cmd;
}

struct cmd *
parseredirs(struct cmd *cmd, char **ps, char *es) {
    int tok;
    char *q, *eq;

    while (peek(ps, es, "<>")) {
        tok = gettoken(ps, es, 0, 0);
        if (gettoken(ps, es, &q, &eq) != 'a') {
            fprintf(stderr, "missing file for redirection\n");
            exit(-1);
        }
        switch (tok) {
            case '<':
                cmd = redircmd(cmd, mkcopy(q, eq), '<');
                break;
            case '>':
                cmd = redircmd(cmd, mkcopy(q, eq), '>');
                break;
        }
    }
    return cmd;
}

struct cmd *
parseexec(char **ps, char *es) {
    char *q, *eq;
    int tok, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    ret = execcmd();
    cmd = (struct execcmd *) ret;

    argc = 0;
    ret = parseredirs(ret, ps, es);
    while (!peek(ps, es, "|")) {
        if ((tok = gettoken(ps, es, &q, &eq)) == 0)
            break;
        if (tok != 'a') {
            fprintf(stderr, "syntax error\n");
            exit(-1);
        }
        cmd->argv[argc] = mkcopy(q, eq);
        argc++;
        if (argc >= MAXARGS) {
            fprintf(stderr, "too many args\n");
            exit(-1);
        }
        ret = parseredirs(ret, ps, es);
    }
    cmd->argv[argc] = 0;
    return ret;
}
