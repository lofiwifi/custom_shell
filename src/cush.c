/*
 * cush - the customizable shell.
 *
 * Christopher Qiu and Ryan Erickson
 *
 * Developed by Godmar Back for CS 3214 Summer 2020
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <spawn.h>
#include <stdio.h>
#include <fcntl.h>
#include <readline/history.h>
#include <linux/limits.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

static void handle_child_status(pid_t pid, int status);
static void execute_command_line(struct ast_command_line *);
static struct job *find_job_of_pid(pid_t pid);
static void delete_dead_jobs(void);

// Built-in function prototypes
static int call_builtin(char **argv, struct job *job);
static void jobs_builtin(void);
static void exit_builtin(void);
static void stop_builtin(int jid, struct job *job);
static void fg_builtin(char *arg);
static void bg_builtin(char *arg);
static void kill_builtin(int jid, struct job *job);
static void history_builtin(char *arg);
static int check_expansion(char **argv);

char* get_machine(void);
char* get_only_current_dir(void);


static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
           " -h            print this help\n",
           progname);

    exit(EXIT_SUCCESS);
}

/* Returns a dynamically-allocated string containing the rlogin machine
   that the shell is running on. If an error occurs, it will return it
   through the dynamically-allocated string. The string returned must be
   freed after use. */
char* get_machine() {
    char buf[PATH_MAX];
    /* Checks for errors with obtaining hostname. */
    if (gethostname(buf, sizeof(buf)) != 0) {
        return strdup("Error: hostname could not be obtained.");
    }

    // Find how many characters are in the machine name.
    int machine_chars = -1;
    for (int i = 0; (i < sizeof(buf) && buf[i] != '\0'); i++) {
        if (buf[i] == '.') {
            machine_chars = i;
            break;
        }
    }

    /* Checks for errors with finding the '.' in the hostname. */
    if (machine_chars == -1) {
        return strdup("Error: hostname could not be parsed.");
    }

    // Returns truncated hostname containing only the name of the machine.
    char* name = calloc(machine_chars + 1, sizeof(char));
    snprintf(name, machine_chars + 1, "%s", buf);
    return name; 
}

/* Obtains only the current directory, not the entire cwd path. Returns it
   in a dynamically-allocated string that needs to be freed after use by
   the caller. It will return an error message through a dynamically-
   allocated string if an error occurs. */
char* get_only_current_dir() {

    char* cwd; 
    if ((cwd = getcwd(NULL, 0)) == NULL) { // Checks for any errors with obtaining cwd.
        return strdup("Error: current working directory could not be obtained.");
    }

    // Obtains the index within cwd where the last directory of the cwd is contained.
    int dir_index = -1;
    for (int i = strlen(cwd) - 1; i >= 0; i--) {
        if (cwd[i] == '/') {
            dir_index = i + 1;
            break;
        }
    }

    if (dir_index == -1) { // Checks for parsing errors.
        return strdup("Error: current working directory could not be parsed.");
    }

    // Allocates a null-terminated string for the directory and returns it. 
    char* dir = calloc(strlen(cwd) - dir_index + 1, sizeof(char));
    snprintf(dir, strlen(cwd) - dir_index + 1, "%s", &cwd[dir_index]);
    free(cwd);
    return dir;
}

/* Build a prompt */
static char *
build_prompt(void)
{
    char prompt[PATH_MAX];
    
    char* machine = get_machine();
    char* directory = get_only_current_dir();
    char* login = getlogin();
    snprintf(prompt, sizeof(prompt), "%s@%s %s> ", login, machine, directory);

    free(machine);
    free(directory);
    return strdup(prompt);
}

enum job_status
{
    FOREGROUND,    /* job is running in foreground.  Only one job can be
                      in the foreground state. */
    BACKGROUND,    /* job is running in background */
    STOPPED,       /* job is stopped via SIGSTOP */
    NEEDSTERMINAL, /* job is stopped because it was a background job
                      and requires exclusive terminal access */
};

struct job
{
    struct list pids;               /* List of pids for job. */
    pid_t pgid;                     /* gpid for job. */
    struct list_elem elem;          /* Link element for jobs list. */
    struct ast_pipeline *pipe;      /* The pipeline of commands this job represents */
    int jid;                        /* Job id. */
    enum job_status status;         /* Job status. */
    int num_processes_alive;        /* The number of processes that we know to be alive */
    struct termios saved_tty_state; /* The state of the terminal when this job was
                                       stopped after having been in foreground */
    bool termstate_saved;           /* Tracks whether or not the terminal state has been saved before. */

    /* Add additional fields here if needed. */
};

struct pid
{
    pid_t pid;             /* PID stored within wrapper struct. */
    struct list_elem elem; /* Link element for pids list. */
};

/* Utility functions for job list management.
 * We use 2 data structures:
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1 << 16)
static struct list job_list;

static struct job *jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job *
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job *job = malloc(sizeof *job);
    job->pgid = 0;
    job->pipe = pipe;
    job->num_processes_alive = 0;
    job->termstate_saved = false;
    list_init(&job->pids);
    list_push_back(&job_list, &job->elem);

    if (pipe->bg_job)
    {
        job->status = BACKGROUND;
    }
    else
    {
        job->status = FOREGROUND;
    }

    for (int i = 1; i < MAXJOBS; i++)
    {
        if (jid2job[i] == NULL)
        {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Adds a pid to the end of the pid list of the given job. Initializes this list if necessary.
   Assumes that the given PID is active, so it increases the num_processes_alive field. */
static void
add_pid_to_job(pid_t pid, struct job *job)
{
    struct pid *pid_str = malloc(sizeof(struct pid));
    pid_str->pid = pid;
    list_push_back(&job->pids, &pid_str->elem);
    job->num_processes_alive++;
}

/* Iterates through the current job list and each job list's pid list to find the job
   belonging to the given pid. */
static struct job *
find_job_of_pid(pid_t g_pid)
{

    // Iterates through every job in the list
    for (struct list_elem *job_elem = list_begin(&job_list); job_elem != list_end(&job_list); job_elem = list_next(job_elem))
    {

        // Iterates through every pid in the pid list of the job
        struct job *job = list_entry(job_elem, struct job, elem);
        for (struct list_elem *pid_elem = list_begin(&job->pids); pid_elem != list_end(&job->pids); pid_elem = list_next(pid_elem))
        {
            if (list_entry(pid_elem, struct pid, elem)->pid == g_pid)
            {
                return job;
            }
        }
    }

    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    list_remove(&job->elem);

    if (job->pipe->bg_job)
    {
        printf("[%d]\tDone\n", job->jid);
    }

    // Frees internal job PID list.
    while (!list_empty(&job->pids))
    {
        struct list_elem *e = list_pop_front(&job->pids);
        struct pid *to_free = list_entry(e, struct pid, elem);
        free(to_free);
    }

    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

/* Deletes all jobs with no live processes remaining. Removes each dead
   job from the job list. */
static void
delete_dead_jobs(void)
{
    for (int i = list_size(&job_list); i > 0; i--)
    {
        struct list_elem *e = list_pop_front(&job_list);
        struct job *curr_job = list_entry(e, struct job, elem);

        if (curr_job->num_processes_alive <= 0)
        {
            delete_job(curr_job);
        }
        else
        {
            list_push_back(&job_list, e);
        }
    }
}

static const char *
get_status(enum job_status status)
{
    switch (status)
    {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem *e = list_begin(&pipeline->commands);
    for (; e != list_end(&pipeline->commands); e = list_next(e))
    {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD
 * signal may be delivered for multiple children that have
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0)
    {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 *
 * Implement handle_child_status such that it records the
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0)
    {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
    delete_dead_jobs();
}

static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    struct job *job = find_job_of_pid(pid);
    if (job == NULL)
    {
        char *out = "ERROR: given PID is not associated with a job.\n";
        write(1, out, strlen(out));
        return;
    }

    if (WIFEXITED(status))
    {
        job->num_processes_alive--;

        // Only sample the terminal if the process exited correctly
        if (WEXITSTATUS(status) == 0 && job->status == FOREGROUND)
        {
            termstate_sample();
        }
        termstate_give_terminal_back_to_shell();
    }
    else if (WIFSTOPPED(status))
    {
        switch (WSTOPSIG(status))
        {

        /* If user stopped foreground process with Ctrl + Z */
        case SIGTSTP:
            job->status = STOPPED;
            termstate_save(&job->saved_tty_state);
            job->termstate_saved = true;
            if (job->pgid == pid)
            {
                print_job(job);
            }
            termstate_give_terminal_back_to_shell();
            break;

        /* If user stopped background process with stop command */
        case SIGSTOP:
            job->status = STOPPED;
            termstate_give_terminal_back_to_shell();
            break;

        /* If non-foreground process wants terminal access */
        case (SIGTTOU || SIGTTIN):
            job->status = FOREGROUND;
            termstate_give_terminal_to(&job->saved_tty_state, job->pgid);
        }
    }
    else if (WIFSIGNALED(status))
    {
        /* If the process was killed at all, decrement live processes and return terminal control to shell. */
        job->num_processes_alive--;
        termstate_give_terminal_back_to_shell();
        char *buf;
        switch (WTERMSIG(status))
        {

        /* User terminates process with CTRL+C */
        case SIGINT:
            printf("\n");
            break;

        /* User terminates process with kill command OR user terminates process with kill -9 */
        case SIGTERM || SIGKILL:
            buf = strsignal(WTERMSIG(status));
            write(1, buf, strlen(buf));
            break;

        /* General case, process has been terminated */
        default:
            buf = strsignal(WTERMSIG(status));
            write(1, buf, strlen(buf));
        }
    }
}

/* Jobs built-in shell function. Outputs the current information about logged, live jobs to the current "standard" output.
   Deletes dead jobs as it iterates to prevent redundant, inaccurate information due to finished background processes. */
static void
jobs_builtin(void)
{
    struct list_elem *e = list_begin(&job_list);
    while (e != list_end(&job_list))
    {
        struct job *j = list_entry(e, struct job, elem);
        if (j->pgid != 0)
        { // Does not print the "jobs" job
            print_job(j);
        }
        e = list_next(e);
    }
}

/* Exit built-in shell function. Exits cush and returns you to the bash shell.*/
static void
exit_builtin(void)
{
    exit(0);
}

/* Stop built-in shell function. Stops the job specified by arg. */
static void
stop_builtin(int jid, struct job *job)
{
    struct job *to_stop = get_job_from_jid(jid);

    if (jid2job[jid] == NULL || job->jid == jid)
    {
        printf("stop %d: No such job\n", jid);
        return;
    }

    killpg(to_stop->pgid, SIGSTOP);
}

/* Foreground fg built-in shell function. Places the job of jid arg in
   the foreground. */
static void
fg_builtin(char *arg)
{
    struct job *job = get_job_from_jid(atoi(arg));
    job->status = FOREGROUND;

    /* Output fg command line message. */
    print_cmdline(job->pipe);
    printf("\n");

    if (job->termstate_saved == false)
    { // Ensures it calls NULL for unsaved termstates
        termstate_give_terminal_to(NULL, job->pgid);
    }
    else
    {
        termstate_give_terminal_to(&job->saved_tty_state, job->pgid);
    }
    killpg(job->pgid, SIGCONT);
    wait_for_job(job);
}

/* Background bg built-in shell function. Places the job of jid arg in
   the background and returns terminal control. */
static void
bg_builtin(char *arg)
{
    struct job *job = get_job_from_jid(atoi(arg));
    job->status = BACKGROUND;
    printf("[%d] %d\n", job->jid, job->pgid);
    termstate_give_terminal_back_to_shell();
    killpg(job->pgid, SIGCONT);
}

/* Kill built-in shell function. Kills the job specified by jid. */
static void
kill_builtin(int jid, struct job *job)
{
    struct job *to_kill = get_job_from_jid(jid);

    if (jid2job[jid] == NULL || job->jid == jid)
    {
        printf("kill %d: No such job\n", jid);
        return;
    }

    killpg(to_kill->pgid, SIGTERM);
}

/* History built-in shell function. Displays past command history and allows
   for querying of commands. */
static void history_builtin(char *arg)
{
    HIST_ENTRY **list = history_list();
    for (int i = (history_base - 1); i < (history_base + history_length - 1); i++)
    {
        printf("%5d  %s\n", (i + 1), list[i]->line);
    }
}

/* Checks for a command-line history expansion. If an expansion is successful, the command
   given in argv is replaced with the expansion. Returns 0 if the expansion was successful
   and the command can be executed. Returns 1 if there was an issue with expansion or
   the command does not need to be executed. Handles output of the expansion function. */
static int check_expansion(char **cmd)
{
    switch (history_expand(*cmd, cmd))
    {
    case -1:
        fprintf(stderr, "%s\n", *cmd);
        return 1;
    case 2:
        printf("%s\n", *cmd);
        return 1;
    default:
        return 0;
    }
}

/*
 * Calls the builtin function specified by the cmd parameter. If the command matches a supported builtin,
 * it calls the builtin function and returns 0 to indicate a successful builtin call. If the command
 * does not match a supported builtin function, it returns 1 to indicate a non-matching command that needs
 * to be spawned.
 */
static int
call_builtin(char **argv, struct job *job)
{
    char *cmd = argv[0];
    if (strcmp(cmd, "kill") == 0)
    {
        kill_builtin(atoi(argv[1]), job);
        return 0;
    }
    else if (strcmp(cmd, "fg") == 0)
    {
        fg_builtin(argv[1]);
        return 0;
    }
    else if (strcmp(cmd, "bg") == 0)
    {
        bg_builtin(argv[1]);
        return 0;
    }
    else if (strcmp(cmd, "jobs") == 0)
    {
        jobs_builtin();
        return 0;
    }
    else if (strcmp(cmd, "stop") == 0)
    {
        stop_builtin(atoi(argv[1]), job);
        return 0;
    }
    else if (strcmp(cmd, "exit") == 0)
    {
        exit_builtin();
    }
    else if (strcmp(cmd, "history") == 0)
    {
        history_builtin(argv[1]);
        return 0;
    }
    return 1;
}

/**
 * Main's helper iterative function that iterates through all pipelines,
 * their respective commands, and executes their commands. Adds each
 * pipeline to the job list, maintains records of which PIDs belong
 * to which jobs via struct operations, and handles signaling via
 * wait_for_job() and handle_child_status().
 */
static void
execute_command_line(struct ast_command_line *cline)
{
    // Iterates through the list of pipelines.
    while (!list_empty(&cline->pipes))
    {
        struct list_elem *pList = list_pop_front(&cline->pipes);

        // Blocks the child signal, then adds a job for each pipeline.
        signal_block(SIGCHLD);
        struct ast_pipeline *pipe = list_entry(pList, struct ast_pipeline, elem);
        struct job *job = add_job(pipe);

        // Create matrix of 2*(n-1) pipe fds.
        // Matrix is of size 2*n to make logic simpler.
        int num_pipes = list_size(&pipe->commands) - 1;
        int pipefds[num_pipes + 1][2];

        for (int i = 0; i < num_pipes; i++)
        {
            int currfds[2];
            pipe2(currfds, O_CLOEXEC);

            pipefds[i][0] = currfds[0];
            pipefds[i][1] = currfds[1];
        }

        int cmd_index = 0;

        // Iterates through the list of commands within a pipeline.
        for (struct list_elem *cList = list_begin(&pipe->commands); cList != list_end(&pipe->commands); cList = list_next(cList))
        {

            // Spawns a child process for each command within the pipeline.
            struct ast_command *cmd = list_entry(cList, struct ast_command, elem);

            // If the command does not match a supported builtin, follow process spawning procedures.
            if (call_builtin(cmd->argv, job) != 0)
            {
                posix_spawn_file_actions_t child_file_attr;
                posix_spawnattr_t child_spawn_attr;
                posix_spawnattr_init(&child_spawn_attr);
                posix_spawn_file_actions_init(&child_file_attr);

                // Spawn the process as part of a process group. If the PGID of the job is 0, create a new group.
                posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP);
                posix_spawnattr_setpgroup(&child_spawn_attr, job->pgid);

                // Redirect input.
                if (pipe->iored_input != NULL && cList == list_begin(&pipe->commands))
                {
                    posix_spawn_file_actions_addopen(&child_file_attr, 0, pipe->iored_input, O_RDONLY, 0666);
                }

                // Redirect output.
                if (pipe->iored_output != NULL && cList == list_end(&pipe->commands)->prev)
                {
                    // Append output.
                    if (pipe->append_to_output)
                    {
                        posix_spawn_file_actions_addopen(&child_file_attr, 1, pipe->iored_output, O_WRONLY | O_CREAT | O_APPEND, 0666);
                    }
                    // Overwrite output.
                    else
                    {
                        posix_spawn_file_actions_addopen(&child_file_attr, 1, pipe->iored_output, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                    }

                    // >& implementation.
                    if (cmd->dup_stderr_to_stdout)
                    {
                        posix_spawn_file_actions_adddup2(&child_file_attr, 1, 2);
                    }
                }

                // Linking pipe output.
                if (num_pipes > 0 && cList != list_end(&pipe->commands)->prev)
                {
                    posix_spawn_file_actions_adddup2(&child_file_attr, pipefds[cmd_index][1], 1);

                    if (cmd->dup_stderr_to_stdout)
                    {
                        posix_spawn_file_actions_adddup2(&child_file_attr, pipefds[cmd_index][1], 2);
                    }
                }

                // Linking pipe input.
                if (num_pipes > 0 && cList != list_begin(&pipe->commands))
                {
                    posix_spawn_file_actions_adddup2(&child_file_attr, pipefds[cmd_index - 1][0], 0);
                }

                /* Spawn process and add the process to the job PID list if the spawn is successful. Otherwise, output command not found error. */
                pid_t cpid;
                extern char **environ;
                if (posix_spawnp(&cpid, cmd->argv[0], &child_file_attr, &child_spawn_attr, &cmd->argv[0], environ) == 0)
                {
                    /* If this spawn created a new process group, store the PGID in the job's PGID field.
                       Give new foreground jobs terminal access. Output job message if it's a background job. */
                    if (job->pgid == 0)
                    {
                        job->pgid = getpgid(cpid);
                        if (pipe->bg_job)
                        {
                            printf("[%d] %d\n", job->jid, job->pgid);
                        }
                        else
                        {
                            tcsetpgrp(termstate_get_tty_fd(), job->pgid);
                        }
                    }
                    add_pid_to_job(cpid, job);
                }
                else
                {
                    utils_error("%s: ", cmd->argv[0]); /* Outputs suitable error message when a process doesn't spawn */
                }

                posix_spawnattr_destroy(&child_spawn_attr);
                posix_spawn_file_actions_destroy(&child_file_attr);
            }

            cmd_index++;
        }

        // Close pipes.
        for (int i = 0; i < num_pipes; i++)
        {
            close(pipefds[i][0]);
            close(pipefds[i][1]);
        }

        // After all processes have been spawned, wait for the job if it is foreground.
        wait_for_job(job);
        signal_unblock(SIGCHLD);
    }
}

int main(int ac, char *av[])
{
    int opt;
    using_history(); /* Initializes history's variables. */

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0)
    {
        switch (opt)
        {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;)
    {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        free(prompt);

        if (cmdline == NULL)
        { /* User typed EOF */
            break;
        }

        // Ensures any history expansion errors will not be ran
        bool execute = (check_expansion(&cmdline) == 0) ? true : false;

        struct ast_command_line *cline = ast_parse_command_line(cmdline);

        if (cline == NULL)
        { /* Error in command line */
            continue;
        }

        if (list_empty(&cline->pipes))
        { /* User hit enter */
            add_history(cmdline);
            ast_command_line_free(cline);
            continue;
        }

        if (execute)
        {
            add_history(cmdline);
            execute_command_line(cline);
        }
        free(cmdline);
        ast_command_line_free(cline);
    }
    return 0;
}
