band.sectors 1024
map.bands 20
group.bands 132
group.span 118000
group.num 2

create /tmp/new.img
#open /tmp/new.img
superblock

#     start count ptr type (0=free,1=full,2=frontier)
bands 0	    21	  0   0

#     start ptr type (0=free,1=full,2/130=frontier)
#band  21    0   130
band  21    0   2

#     start count ptr type (0=free,1=full,2=frontier)
#bands 22    127   0   0
bands 22    131   0   0

#     start ptr type (0=free,1=full,2=frontier)
#band  149   0   130
#band  149   0   2
band  153   0   2

#     start count ptr type (0=free,1=full,2=frontier)
#bands 150   127   0   0
bands 154   131   0   0

# RECORD_BAND 1
# RECORD_MAP 2
# RECORD_DATA 3

zero 1.0 15

#        loc  seq   type   n_recs   prev   next   base
header 	 1.0  0	    1	   277	    1.0	   1.2	  1.0
dump bands 1.1
header 	 1.2  1	    1      0        1.0    1.3    1.0

# #   lba      pba       len
# map 0 	     22.1      100
# map 10000    22.103    50

# #        loc     seq   type   n_recs   prev     next     base
# header   1.3  	 2     2      2        1.2 	1.5 	 1.0
# dump map 1.4
# header   1.5  	 3     2      0        1.3 	1.6 	 1.0

zero 21.0 200

# note that LBAs for group 1 are 000000-117999
#      	    	     	   2     118000-235999
# write 100 sectors of data to beginning of band 21

#   lba      pba       len
#map 10000    21.0      100

#         loc     seq   type   prev     next     base
#mapheader 21.0 	  4     3      21.0   	21.101 	 1.0
#mapheader 21.101  5     3      21.0	21.102	 1.0

#   lba        pba       len
#map 20000      21.103    50

# and another 50 sectors...
#         loc     seq   type   prev     next     base
#mapheader 21.102  6     3      21.101	22.153	 1.0
#mapheader 21.153  7     3      21.102	22.154	 1.0

zero 22.0 200
zero 149.0 200

# and finally some sectors in the 2nd group
#   lba        pba       len
#map 150000     149.1     4

#        loc     seq   type   prev     next     base
#mapheader 149.0   8     3     149.0    149.5 	1.0
#mapheader 149.5   9     3     149.0    149.6	1.0

#map 150010     149.7     4
#mapheader 149.6   10    3     149.5    149.11	1.0
#mapheader 149.11  11    3     149.6    149.12	1.0
