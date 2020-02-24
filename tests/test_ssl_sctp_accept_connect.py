#
#  gensio - A library for abstracting stream I/O
#  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
#
#  SPDX-License-Identifier: LGPL-2.1-only
#

from utils import *
import gensio

print("Test ssl over sctp accepter connect")
goterr = False
try:
    TestAcceptConnect(o,
            "ssl(key=%s/key.pem,cert=%s/cert.pem,clientauth),sctp,3023"
                           % (keydir, keydir),
            "ssl(key=%s/key.pem,cert=%s/cert.pem),sctp,3024"
                       % (keydir, keydir),
            "ssl(CA=%s/CA.pem),sctp,localhost,3023" % keydir,
                       do_small_test,
                      expect_remclose = False)
except Exception as E:
    s = str(E)
    # We can race and get either one of these
    if (not (s.endswith("Communication error") or
             s.endswith("Remote end closed connection"))):
        raise
    print("  Success checking no client cert")
    goterr = True
if not goterr:
    raise Exception("Did not get error on no client certificate.")

goterr = False
try:
    TestAcceptConnect(o,
            "ssl(key=%s/key.pem,cert=%s/cert.pem,clientauth),sctp,3023"
                           % (keydir, keydir),
            "ssl(key=%s/key.pem,cert=%s/cert.pem),sctp,3024"
                           % (keydir, keydir),
            "ssl(CA=%s/CA.pem,key=%s/clientkey.pem,cert=%s/clientcert.pem)"
            ",sctp,localhost,3023"
                           % (keydir, keydir, keydir),
                       do_small_test, expect_remclose = False)
except Exception as E:
    s = str(E)
    # We can race and get either one of these
    if (not (s.endswith("Communication error") or
             s.endswith("Remote end closed connection"))):
        raise
    print("  Success checking invalid client cert")
    goterr = True
if not goterr:
    raise Exception("Did not get error on invalid client certificate.")

TestAcceptConnect(o,
            "ssl(key=%s/key.pem,cert=%s/cert.pem,clientauth),sctp,3023"
                           % (keydir, keydir),
            "ssl(key=%s/key.pem,cert=%s/cert.pem),sctp,3024"
                           % (keydir, keydir),
            "ssl(CA=%s/CA.pem,key=%s/clientkey.pem,cert=%s/clientcert.pem)"
            ",sctp,localhost,3023"
                           % (keydir, keydir, keydir),
                       do_small_test, CA="%s/clientcert.pem" % keydir)

