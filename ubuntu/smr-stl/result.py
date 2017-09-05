#!/usr/bin/python

import sys

from optparse import OptionParser

usage = "usage: %prog [options] > file.out"
parser = OptionParser(usage=usage)
parser.add_option('-l', '--maxlba', type='int', default=236000,
                  help='LBA range')
(options, args) = parser.parse_args()
if args:
    fp = open(args[0], 'r')
else:
    fp = sys.stdin
    
lbas = options.maxlba
vals = [-1] * lbas

for line in fp:
    cmd = line.split()
    if cmd[0] != 'write':
        continue
    lba,blks,val = map(int, cmd[1:])
    for i in range(lba,lba+blks):
        vals[i] = val

for i in range(lbas):
    if vals[i] != -1:
        print "verify", i, 1, vals[i]
