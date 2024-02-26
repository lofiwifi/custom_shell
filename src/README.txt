Student Information
-------------------
Christopher Qiu
Ryan Erickson

How to execute the shell
------------------------
<describe how to execute from the command line>

Run cush from its directory or add its directory to PATH.
Execute cush with "./cush" from the command line.

Important Notes
---------------
Includes the built-in command history and a 
custom prompt implementation displaying username, hostname
and current directory.


Description of Base Functionality
---------------------------------
<describe your IMPLEMENTATION of the following commands:
jobs, fg, bg, kill, stop, \ˆC, \ˆZ >

jobs
uses the given job list to iterate through all active jobs
and print their information. If a job being iterated has a 
pgid that matches the current process's pgid, it is not printed.
This is to avoid printing the current process running the jobs
builtin. 

fg
sets a job's status to FOREGROUND and performs bookkeeping. Outputs
the job being moved to the foreground. We implemented a "termstate_saved"
boolean field in the jobs struct to make sure we could track whether
a process has had its state saved before. If it hasn't and the user wants
the job in the foreground, termstate_give_terminal_to is called with a NULL
termstate to prevent errors. If it has had its state saved before, the state
is given to the function. The job is then told to continue and the job is waited
for.

bg
The job's status is changed to BACKGROUND, a message containing the job's JID and
PGID is sent to the terminal, the job is told to continue and the shell regains
control of the terminal.

stop
sends SIGSTOP to the given pgid

kill
sends SIGTERM to the given pgid

\^C
WIFSIGNALED in the status handler catches the status change. Our switch statement
identifies it as a SIGINT Ctrl + C signal, we print the new line character for 
readability, decrement the live processes of the jobs, return terminal control
to the shell, and return. 

\^Z
WIFSTOPPED identifies the status change in the signal handler. Our switch statement
identifies it as a SIGSTP signal. We then change job status to STOPPED, save the terminal
state, set our termstate_saved field in the job to TRUE, print the job to the terminal
and return terminal control to the shell.

>

Description of Extended Functionality
-------------------------------------
<describe your IMPLEMENTATION of the following functionality:
I/O, Pipes, Exclusive Access >

I/O
Makes a call to addopen and sets stdin to iored_input
Makes a call to addopen and sets stdout to iored_output,
if >, includes O_TRUNC, if >>, includes O_APPEND

Pipes
Creates an matrix of 2*(n-1) pipe fds,
calls adddup2 to link pipe fds,
if dup_stderr_to_stdout, link stderr as well,
afterwards closes all fds in the matrix

Exclusive Access

>

List of Additional Builtins Implemented
---------------------------------------
history
 - history builtin implemented. Maintains a list of
   submitted commands via the GNU History library.
   Whenever a command is submitted through the command
   line, the command is checked for an expansion by
   history library functions, added to the history list, 
   and executed if valid. History implementation also includes
   both the standard event designators via GNU History Library, and
   scrolling through past entered commands via the arrow keys
   on the command line.

custom prompt
 - custom prompt implemented. Obtains strings containing the 
   current user's username, the current truncated rlogin hostname, 
   and the current directory via getlogin(), gethostname(), getcwd(), 
   and snprintf() functions. Changes with user, hostname and current
   directory.
