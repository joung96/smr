#!/usr/bin/python

import png, sys
import numpy as np
from optparse import OptionParser

usage = "usage: %prog [options] WxH tracefile"

parser = OptionParser(usage=usage)
parser.add_option('-D', '--dir', default='/tmp/',
                  help='destination image file directory')
parser.add_option('-n', '--interval', type='int', default=100,
                  help='number of operations between images')
parser.add_option('-S', '--stop', type='int', default=0,
                  help='number of frames to generate')
parser.add_option('-b', '--Np', type='int', default=64,
                  help='pages per block')

(options, args) = parser.parse_args()
if len(args) != 2:
    parser.error("wrong number of arguments")
Np = options.Np

wxh,infile = args
_w,_h = map(int, wxh.split('x'))

heatmap = np.array(
    [(0, 0, 0), 
     (84, 96, 199), 
     (0, 159, 159), 
     (151, 204, 102),
     (0, 244, 53), 
     (255, 248, 63), 
     (255, 186, 52),
     (255, 141, 45),
     (255, 90, 42), 
     (252, 62, 40)])

hr = heatmap[:,0]
hg = heatmap[:,1]
hb = heatmap[:,2]

N = _w * _h
n = np.zeros(N, dtype=np.int16)
rgb = np.zeros(N*3, dtype=np.int16)
k = 0

w = png.Writer(width=_w, height=_h, planes=3, bitdepth=8)
i = 0

rmap = np.ones(_w*_h) * -1

t0 = 0
with open(infile, 'r') as fp:
    for line in fp:
        a = map(int, line.split())
        blk,pg = a[0:2]
        addr = a[-1]
        if rmap[addr] != -1:
            n[rmap[addr]] = 0
        j = blk*Np + pg
        rmap[addr] = j
        if j >= N:
            print 'bad:', line
            continue
        if pg == 0:
            n[blk*Np:(blk+1)*Np] = 0
        n[j] = 9
        i += 1

        if i < options.interval:
            continue

        j = np.ceil(n).astype(int)
        r = hr[j]
        g = hg[j]
        b = hb[j]
        rgb[0::3] = r
        rgb[1::3] = g
        rgb[2::3] = b
        fname = options.dir + ('/map-%05d.png' % k)
        k += 1
        print fname, i
        i = 0
        
        f = open(fname, 'w')
        z = rgb.reshape(_h, _w*3)
        w.write(f, z)
        f.close()

        n = n * 0.9
        
        if options.stop and k >= options.stop:
            break
        
