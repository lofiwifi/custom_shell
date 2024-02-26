Student Information
-------------------
Christopher Qiu
Ryan Erickson

How to execute the shell
------------------------
<describe how to execute from the command line>

Run cush from its directory or add its directory to PATH.

Important Notes
---------------
<Any important notes about your system>



Description of Base Functionality
---------------------------------
<describe your IMPLEMENTATION of the following commands:
jobs, fg, bg, kill, stop, \ˆC, \ˆZ >

stop
sends SIGSTOP to the given pgid

kill
sends SIGTERM to the given pgid

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

List of Additional Builtins Implemented
---------------------------------------
(Written by Your Team)
<builtin name>
<description>
