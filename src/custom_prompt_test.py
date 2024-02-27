#!/usr/bin/python
#
# custom_prompt_test: tests the custom prompt customization.
# 
# Test the custom prompt. Ensure that hostname, username, and current directory
# are all implemented properly and reflect in the custom prompt.
#
# Written by Ryan Erickson (ryane25) and Chris Qiu (christopherqiumd).
#

import sys, atexit, pexpect, proc_check, signal, time, threading, socket, re
from testutils import *

console = setup_tests()

# Ensures that the custom prompt provides the right format.
sendline("")
expect("[^@]*@[^ ]*\s[^>]*> ", "Prompt gave incorrect format output.")

# Obtains individual components for current host, current directory, and username.
host = socket.gethostname().removesuffix(".rlogin").strip()
dir = re.sub(r"[^/]*/", "", os.getcwd().strip()) # strips getcwd() output down to only current directory.
user = os.getlogin().strip()
out = "" + user + "@" + host + "\s" + dir + "> "

# Asserts that the prompt reflects the current user, host, and directory.
sendline("")
expect(out, "Prompt did not output user, host, and directory properly.")

test_success()