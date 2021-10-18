#
#  gensio - A library for abstracting stream I/O
#  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
#
#  SPDX-License-Identifier: GPL-2.0-only
#

from utils import *
import gensio

print("Test echo gensio")
io = alloc_io(o, "echo")
check_raddr(io, "echo", "echo")
test_dataxfer(io, io, "This is a test string!")
io_close(io)
del io
del o
test_shutdown()
print("  Success!")

