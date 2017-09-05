#!/usr/bin/python

import sys
import random

from optparse import OptionParser

usage = "usage: %prog [options] > file.out"
parser = OptionParser(usage=usage)
parser.add_option('-r', '--read', type='float', default=0.25,
                  help='fraction of read commands (0.0 .. 1.0)')
parser.add_option('-t', '--trim', type='float', default=0.25,
                  help='fraction of TRIM commands (0.0 .. 1.0)')
parser.add_option('-o', '--output', help='output file')
parser.add_option('-l', '--maxlba', type='int', default=236000,
                  help='LBA range')

(options, args) = parser.parse_args()
if len(args) != 1:
    parser.error("unrecognized argument")

maxlba = options.maxlba - 1
p_read = options.read
p_trim = options.trim

n = int(args[0])
for j in range(n):
    _len = random.randint(1, 200)
    lba = random.randint(0, maxlba-_len)
    if random.random() < p_read:
        print "read", lba, _len
    elif random.random() < (1-p_read)*p_trim:
        print "trim", lba, _len
    else:
        val = random.randint(0, 254)
        if (val >= 17):
            val += 1
        print "write", lba, _len, val
    

