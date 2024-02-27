#!/usr/bin/python
#
# history_builtin_test: tests the "history" custom cush shell built-in command.
# 
# Test the history command. This file tests for arrow key capability for command
# recall, the first 6 event designators, and the history built-in list itself.
#
# Written by Ryan Erickson (ryane25) and Chris Qiu (christopherqiumd).
#

import sys, atexit, pexpect, proc_check, signal, time, threading
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

# test initial history behavior with first prompt
sendline("history")
expect("1  history")

# send in multiple test lines to provide previous history
sendline("sleep 2 &")   # 1
sendline("jobs")        # 2
sendline("echo hello")  # 3
sendline("clear")       # 4

# tests arrow key command functionality
console.send(b"[A") # up
sendline("")    # runs last command (clear)
expect_exact("", "Failed at running previous command.")

console.send(b"[A") # up
console.send(b"[A") # up
console.send(b"[A") # up
sendline("")    # runs second-to-last command (echo hello)
expect_exact("hello", "Failed at scrolling more than one command behind (up key).")       
# asserts that the up key scrolls previous commands

console.send(b"[A") # up
console.send(b"[B") # down
sendline("")    # returns to blank prompt line and enters
expect_prompt("Failed at returning to blank command line after scrolling (down key).")       
# asserts that the down key returned to the blank command input

# tests the built-in history functionality
run_builtin("history")
expect("    1  history\r\n" + 
       "    2  sleep 2 &\r\n" +
       "    3  jobs\r\n" +
       "    4  echo hello\r\n" +
       "    5  clear\r\n" +
       "    6  clear\r\n" +
       "    7  echo hello\r\n"
       "    8  \r\n" +
       "    9  history\r\n")

# tests event designators
# tests error functionality of the sub character
sendline("!")
expect_exact("!: No such file or directory", "Failed '!' error test.")    

# tests substitution of a specified command line
sendline("!4")
expect_exact("hello", "Command line 4 was not substituted as intended.")

# tests substitution of a relative command line location
sendline("!-3")
expect_exact("", "Relative command line failed to substitute.")

# tests previous command substitution
sendline("echo hello")
sendline("!!")
expect_exact("hello", "Previous command was not substituted as intended.")

# tests string substitution
sendline("!his")
expect_exact("    1  history\r\n" + 
             "    2  sleep 2 &\r\n", "Most recent string was not substituted as intended.")
# tests containing string substitution
sendline("!?ell")
expect_exact("hello", "Containing string was not substituted as intended.")

# tests out of bounds for relative commands + absolute command line substitution
sendline("!100")
expect_exact("!100: event not found", "Absolute line substitution was not handled properly.")
sendline("!-100")
expect_exact("!-100: event not found", "Relative line substitution was not handled properly.")

test_success()