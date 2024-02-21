/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <spawn.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

static void handle_child_status(pid_t pid, int status);
void execute_command_line(struct ast_command_line*);
struct job* find_job_of_pid(pid_t pid);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
};

struct job {
    struct list /*<pid_t>*/ pids;
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */

    /* Add additional fields here if needed. */
};

struct pid {
    pid_t pid;             /* PID stored within wrapper struct. */
    struct list_elem elem; /* Link element for pids list. */
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

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
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_init(&job->pids);
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
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
static void add_pid_to_job(pid_t pid, struct job* job) {
    struct pid* pid_str = malloc(sizeof(struct pid));
    pid_str->pid = pid;
    list_push_back(&job->pids, &pid_str->elem);
    job->num_processes_alive++;
}

/* Iterates through the current job list and each job list's pid list to find the job
   belonging to the given pid. */
struct job* find_job_of_pid(pid_t g_pid) {

    // Iterates through every job in the list
    for (struct list_elem* job_elem = list_begin(&job_list); job_elem != list_end(&job_list); job_elem = list_next(job_elem)) {

        // Iterates through every pid in the pid list of the job
        struct job* job = list_entry(job_elem, struct job, elem);
        for (struct list_elem* pid_elem = list_begin(&job->pids); pid_elem != list_end(&job->pids); pid_elem = list_next(pid_elem)) {
            if (list_entry(pid_elem, struct pid, elem)->pid == g_pid) {
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
    // Frees internal job PID list.
    for (struct list_elem* e = list_begin(&job->pids); e != list_end(&job->pids); e = list_next(e)) {
        free(list_entry(e, struct pid, elem));
    }

    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status) {
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
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
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

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
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

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
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
}

static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    struct job* job = find_job_of_pid(pid);
    if (job == NULL) {
        printf("ERROR: given PID is not associated with a job.\n");
        return;
    }

    if (WIFEXITED(status)) {
        job->status = STOPPED;
        job->num_processes_alive--;
        termstate_sample();
        termstate_give_terminal_back_to_shell();
    }

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */

}

/**
 * Main's helper iterative function that iterates through all pipelines,
 * their respective commands, and executes their commands. Adds each 
 * pipeline to the job list, maintains records of which PIDs belong
 * to which jobs via struct operations, and handles signaling via
 * wait_for_job() and handle_child_status().
*/
void execute_command_line(struct ast_command_line* cline) {
    // Iterates through the list of pipelines.
    for (struct list_elem* pList = list_begin(&cline->pipes); pList != list_end(&cline->pipes); pList = list_next(pList)) { 
        // Adds a job for each pipeline.
        signal_block(SIGCHLD);
        struct ast_pipeline* pipe = list_entry(pList, struct ast_pipeline, elem);
        struct job* job = add_job(pipe);

        // Iterates through the list of commands within a pipeline.
        for (struct list_elem* cList = list_begin(&pipe->commands); cList != list_end(&pipe->commands); cList = list_next(cList)) {

            // Spawns a child process for each command within the pipeline.
            struct ast_command* cmd = list_entry(cList, struct ast_command, elem);
            posix_spawn_file_actions_t child_file_attr;
            posix_spawnattr_t child_spawn_attr;
            posix_spawnattr_init(&child_spawn_attr);
            posix_spawn_file_actions_init(&child_file_attr);

            // Obtains the pid of the spawned process and adds it to the pid list of the current job.
            pid_t cpid;
            posix_spawnp(&cpid, cmd->argv[0], &child_file_attr, &child_spawn_attr, &cmd->argv[1], environ);
            add_pid_to_job(cpid, job);
        }
        // Waits for the current pipeline to finish. Calls handle_child_status().
        wait_for_job(job);
        signal_unblock(SIGCHLD);
    }

    
}

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;) {

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
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        struct ast_command_line * cline = ast_parse_command_line(cmdline);
        free (cmdline);
        if (cline == NULL)                  /* Error in command line */
            continue;

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }
        execute_command_line(cline);

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        ast_command_line_free(cline);
    }
    return 0;
}
