// XV6 Shell Implementation
// A minimal Unix-like shell that supports basic command execution,
// pipes, I/O redirection, and background processes.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// Command types supported by the shell
#define EXEC  1    // Simple command execution (e.g., ls, cat)
#define REDIR 2    // I/O redirection (e.g., >, <)
#define PIPE  3    // Pipe between commands (e.g., ls | grep)
#define LIST  4    // Sequential commands (e.g., echo hi; echo bye)
#define BACK  5    // Background execution (e.g., command &)

#define MAXARGS 10 // Maximum number of arguments for a command

// Base command structure - all command types inherit from this
struct cmd {
    int type;
};

// Structure for simple command execution (e.g., ls -l)
struct execcmd {
    int type;
    char *argv[MAXARGS];     // Array of command arguments
    char *eargv[MAXARGS];    // Array of pointers to end of each argument
};

// Structure for I/O redirection (e.g., echo hello > file)
struct redircmd {
    int type;
    struct cmd *cmd;         // The command being redirected
    char *file;             // Target file for redirection
    char *efile;            // Pointer to end of file name
    int mode;               // File open mode (O_RDONLY, O_WRONLY, etc.)
    int fd;                 // File descriptor to redirect
};

// Structure for pipe commands (e.g., ls | grep foo)
struct pipecmd {
    int type;
    struct cmd *left;       // Command before pipe
    struct cmd *right;      // Command after pipe
};

// Structure for command lists (e.g., echo hi; echo bye)
struct listcmd {
    int type;
    struct cmd *left;       // First command
    struct cmd *right;      // Second command
};

// Structure for background processes (e.g., command &)
struct backcmd {
    int type;
    struct cmd *cmd;        // Command to run in background
};

// Function prototypes
int fork1(void);                    // Fork wrapper that handles errors
void panic(char*);                  // Error handling function
struct cmd *parsecmd(char*);        // Parse command line into command structure
void runcmd(struct cmd*) __attribute__((noreturn));  // Execute command

// Execute a command - function never returns
void runcmd(struct cmd *cmd) {
    int p[2];                      // Pipe file descriptors
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0) {
        exit(1);
    }

    switch (cmd->type) {
    case EXEC:
        ecmd = (struct execcmd*)cmd;
        if (ecmd->argv[0] == 0) {
            exit(1);
        }
        exec(ecmd->argv[0], ecmd->argv);
        fprintf(2, "exec %s failed\n", ecmd->argv[0]);
        break;

    case REDIR:
        rcmd = (struct redircmd*)cmd;
        close(rcmd->fd);
        if (open(rcmd->file, rcmd->mode) < 0) {
            fprintf(2, "open %s failed\n", rcmd->file);
            exit(1);
        }
        runcmd(rcmd->cmd);
        break;

    case LIST:
        lcmd = (struct listcmd*)cmd;
        if (fork1() == 0) {
            runcmd(lcmd->left);
        }
        wait(0);
        runcmd(lcmd->right);
        break;

    case PIPE:
        pcmd = (struct pipecmd*)cmd;
        if (pipe(p) < 0) {
            panic("pipe");
        }
        if (fork1() == 0) {
            close(1);           // Close stdout
            dup(p[1]);         // Make stdout point to pipe write end
            close(p[0]);
            close(p[1]);
            runcmd(pcmd->left);
        }
        if (fork1() == 0) {
            close(0);          // Close stdin
            dup(p[0]);         // Make stdin point to pipe read end
            close(p[0]);
            close(p[1]);
            runcmd(pcmd->right);
        }
        close(p[0]);
        close(p[1]);
        wait(0);              // Wait for both children
        wait(0);
        break;

    case BACK:
        bcmd = (struct backcmd*)cmd;
        if (fork1() == 0) {
            runcmd(bcmd->cmd);
        }
        break;

    default:
        panic("runcmd");
    }
    exit(0);
}

// Read a command from stdin
int getcmd(char *buf, int nbuf) {
    write(2, "$ ", 2);                // Print prompt
    memset(buf, 0, nbuf);
    gets(buf, nbuf);
    if (buf[0] == 0) {               // EOF
        return -1;
    }
    return 0;
}

// Main shell loop
int main(void) {
    static char buf[100];
    int fd;

    // Ensure that three file descriptors are open
    // This guarantees stdin, stdout, and stderr
    while ((fd = open("console", O_RDWR)) >= 0) {
        if (fd >= 3) {
            close(fd);
            break;
        }
    }

    // Main command processing loop
    while (getcmd(buf, sizeof(buf)) >= 0) {
        // Handle built-in cd command
        if (buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
            buf[strlen(buf)-1] = 0;   // Remove newline
            if (chdir(buf+3) < 0) {   // Change directory
                fprintf(2, "cannot cd %s\n", buf+3);
            }
            continue;
        }
        
        // Fork and execute command
        if (fork1() == 0) {
            runcmd(parsecmd(buf));
        }
        wait(0);
    }
    exit(0);
}

// Error handling function
void panic(char *s) {
    fprintf(2, "%s\n", s);
    exit(1);
}

// Fork wrapper that handles errors
int fork1(void) {
    int pid;

    pid = fork();
    if (pid == -1) {
        panic("fork");
    }
    return pid;
}

// Command Constructors
// These functions allocate and initialize command structures

// Create a new exec command structure
struct cmd* execcmd(void) {
    struct execcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;
    return (struct cmd*)cmd;
}

// Create a new redirection command structure
struct cmd* redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd) {
    struct redircmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDIR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd = fd;
    return (struct cmd*)cmd;
}

// Create a new pipe command structure
struct cmd* pipecmd(struct cmd *left, struct cmd *right) {
    struct pipecmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd*)cmd;
}

// Create a new list command structure
struct cmd* listcmd(struct cmd *left, struct cmd *right) {
    struct listcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd*)cmd;
}

// Create a new background command structure
struct cmd* backcmd(struct cmd *subcmd) {
    struct backcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;
    return (struct cmd*)cmd;
}

// Parsing
char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

// Get next token from command line
// ps: current position in command string
// es: end of command string
// q: start of token
// eq: end of token
int gettoken(char **ps, char *es, char **q, char **eq) {
    char *s;
    int ret;

    s = *ps;
    while (s < es && strchr(whitespace, *s)) {
        s++;
    }
    if (q) {
        *q = s;
    }
    ret = *s;
    
    switch (*s) {
    case 0:
        break;
    case '|':
    case '(':
    case ')':
    case ';':
    case '&':
    case '<':
        s++;
        break;
    case '>':
        s++;
        if (*s == '>') {
            ret = '+';
            s++;
        }
        break;
    default:
        ret = 'a';
        while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s)) {
            s++;
        }
        break;
    }
    
    if (eq) {
        *eq = s;
    }

    while (s < es && strchr(whitespace, *s)) {
        s++;
    }
    *ps = s;
    return ret;
}

// Look ahead for next token without consuming it
int peek(char **ps, char *es, char *toks) {
    char *s;

    s = *ps;
    while (s < es && strchr(whitespace, *s)) {
        s++;
    }
    *ps = s;
    return *s && strchr(toks, *s);
}

// Forward declarations for recursive parsing
struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *parseblock(char**, char*);
struct cmd *nulterminate(struct cmd*);

// Parse an entire command line
struct cmd* parsecmd(char *s) {
    char *es;
    struct cmd *cmd;

    es = s + strlen(s);
    cmd = parseline(&s, es);
    peek(&s, es, "");
    if (s != es) {
        fprintf(2, "leftovers: %s\n", s);
        panic("syntax");
    }
    nulterminate(cmd);
    return cmd;
}

// Parse a command line sequence (cmd ; cmd)
struct cmd* parseline(char **ps, char *es) {
    struct cmd *cmd;

    cmd = parsepipe(ps, es);
    while (peek(ps, es, "&")) {
        gettoken(ps, es, 0, 0);
        cmd = backcmd(cmd);
    }
    if (peek(ps, es, ";")) {
        gettoken(ps, es, 0, 0);
        cmd = listcmd(cmd, parseline(ps, es));
    }
    return cmd;
}

// Parse a pipe sequence (cmd | cmd)
struct cmd* parsepipe(char **ps, char *es) {
    struct cmd *cmd;

    cmd = parseexec(ps, es);
    if (peek(ps, es, "|")) {
        gettoken(ps, es, 0, 0);
        cmd = pipecmd(cmd, parsepipe(ps, es));
    }
    return cmd;
}

// Parse redirections (< and >)
struct cmd* parseredirs(struct cmd *cmd, char **ps, char *es) {
    int tok;
    char *q, *eq;

    while (peek(ps, es, "<>")) {
        tok = gettoken(ps, es, 0, 0);
        if (gettoken(ps, es, &q, &eq) != 'a') {
            panic("missing file for redirection");
        }
        switch (tok) {
        case '<':
            cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
            break;
        case '>':
            cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
            break;
        case '+':  // >>
            cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
            break;
        }
    }
    return cmd;
}

// Parse a parenthesized command
struct cmd* parseblock(char **ps, char *es) {
    struct cmd *cmd;

    if (!peek(ps, es, "(")) {
        panic("parseblock");
    }
    gettoken(ps, es, 0, 0);
    cmd = parseline(ps, es);
    if (!peek(ps, es, ")")) {
        panic("syntax - missing )");
    }
    gettoken(ps, es, 0, 0);
    cmd = parseredirs(cmd, ps, es);
    return cmd;
}

// Parse the execution of a command
struct cmd* parseexec(char **ps, char *es) {
    char *q, *eq;
    int tok, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    if (peek(ps, es, "(")) {
        return parseblock(ps, es);
    }

    ret = execcmd();
    cmd = (struct execcmd*)ret;

    argc = 0;
    ret = parseredirs(ret, ps, es);
    while (!peek(ps, es, "|)&;")) {
        if ((tok=gettoken(ps, es, &q, &eq)) == 0) {
            break;
        }
        if (tok != 'a') {
            panic("syntax");
        }
        cmd->argv[argc] = q;
        cmd->eargv[argc] = eq;
        argc++;
        if (argc >= MAXARGS) {
            panic("too many args");
        }
        ret = parseredirs(ret, ps, es);
    }
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;
    return ret;
}

// NUL-terminate all the counted strings
struct cmd* nulterminate(struct cmd *cmd) {
    int i;
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0) {
        return 0;
    }

    switch (cmd->type) {
    case EXEC:
        ecmd = (struct execcmd*)cmd;
        for (i = 0; ecmd->argv[i]; i++) {
            *ecmd->eargv[i] = 0;
        }
        break;

    case REDIR:
        rcmd = (struct redircmd*)cmd;
        nulterminate(rcmd->cmd);
        *rcmd->efile = 0;
        break;

    case PIPE:
        pcmd = (struct pipecmd*)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;

    case LIST:
        lcmd = (struct listcmd*)cmd;
        nulterminate(lcmd->left);
        nulterminate(lcmd->right);
        break;

    case BACK:
        bcmd = (struct backcmd*)cmd;
        nulterminate(bcmd->cmd);
        break;
    }
    return cmd;
}