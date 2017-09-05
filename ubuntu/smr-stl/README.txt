Files in this directory:

rb.c, rbtree.h - this is Apple's version of the BSD red/black tree code, which makes a good interval map. (for a kernel version we probably want to use a different structure, maybe the in-kernel RB or AVL tree or maybe a B+-tree for lower memory usage and better cache performance)

-- note that all the files in this directory do things in terms of 4KB sectors. 

stl_fakesmr.[c,h] - pretends to be an SMR drive on top of a conventional drive or image file. It puts a header in the last 4KB sector indicating how many bands there are and what the band size is, and then the last sectors before that hold the band write pointers. I'm using mmap to map the write pointers, so that they can be kept mostly up to date without a lot of writes to the disk.

-- the fakesmr.c code is using O_DIRECT to write to the device, which tells the OS not to use the buffer cache but to read/write directly to disk. The problem is that for historical reasons any address passed to read()/write() on an O_DIRECT file descriptor has to be aligned to a multiple of 512 bytes. Note that valloc() allocates aligned 4KB pages, so its values are always safe.

mkfakesmr.c - this uses smr_init() from stl_fakesmr.c to set up a device as a fake SMR drive. You tell it the band size and it calculates the number of bands. (which may be one or two fewer than you expect, since it needs to store write pointers in the top few sectors)

stl_map.c - this holds the forward and reverse maps. In order to do reads you need to be able to map logical addresses to physical addresses. (forward map) In order to do cleaning it's helpful to be able to map physical addresses to logical ones. (reverse map) The RB tree code lets you insert elements, search for them, and iterate up ("right") or down ("left") in order by key from any location in the list.

stl_base.c - this is the main body of the code. Most of the logic right now is in persisting the map and recovering the most recent map on power up. There's also a simple greedy cleaner.

An SMR disk is divided into *bands*, each of which can be written sequentially and must be reset before they can be re-written. For each band there is a *write pointer*, identifying the next location to write. Reads within a band are only valid if they are below the write pointer - i.e. they are for data which has been written since the last reset of that band. Writes are only valid if they are *at* the current write pointer for a band. (well, with "host-aware" drives you can write to other locations, but those writes will go through the drive translation layer)

Although the specification seems to allow bands to be of different sizes, I think we can count on real drives having equal-sized bands. (that's what the Seagate drive does.) (except for the last band - that one may be smaller)

The drive maintains the write pointers - you can query for an individual write pointer or all write pointers, and reset a specific write pointer or all write pointers. (resetting all write pointers erases the entire disk, and isn't very useful in practice although we'll probably end up using it a lot.)

In stl.h we define a physical block address (pba_t) as a structure holding a band number and an offset within the band:
    typedef struct pba {
	int32_t band;
	int32_t offset;
    } pba_t;
For logical addresses we use an int64_t. Note again that all the addresses in this code base are in units of 4KB sectors. For now we have to multiply by 4096 to get offsets for read()/write(). In the kernel we'll need to use LBAs in units of 512-byte sectors, so we'll multiply by 8.

The translation layer in stl_base.c divides the drive into three sections:

superblock (band 1) - this holds a single sector with various parameters, defined as 'struct superblock' in stl.h. Note that we can probably avoid this by copying the superblock to the first sector of each of the map bands. (or at least the first and second ones. Note that we need to deal with the case where we reset the write pointer on the first band, and we could crash before we re-write the first sector on that band)

map bands - these are the next N bands, and are used to persist map and band information. They are used in round-robin fashion, so on startup you find the band with the most recent information.

groups - data bands are divided into equal-sized groups, and the LBA space is divided into equal-sized sections corresponding to those groups. Each group runs a separate copy of the translation algorith - in the current implementation it has a write frontier and is cleaned separately from other groups. The main reason for groups is to preserve physical locality - on devices like the SMR drives we've seen, short seeks are significantly faster than a rotation, while the longest seeks are about 2 or 3 rotations. I'm going to guess that a good group size is one where the max seek within a zone is somewhere between 1/2 and 1 rotation. (or maybe mean seek = 1/2 rotation?)

"magic numbers" - in file and disk formats it's common to have an identifier called a "magic number" which is checked when interpreting a data structure - if it's wrong, then we're trying to interpret garbage and should signal an error instead. If we need to be able to tell reliably whether an arbitrary block is a valid structure or not (we probably do during crash recovery) then we can add a checksum of the block. If we want to be *really* sure we can put a nonce (https://en.wikipedia.org/wiki/Cryptographic_nonce) in the superblock and include that nonce in the checksum - that way if we re-format the disk, metadata blocks from before the reformat won't pass the test, since the reformat will write a new nonce. Right now the code just uses a magic number.

Everything written to the disk is of the form header/contents/trailer, where the header and trailer are single sectors containing a 'struct header' and possibly additional contents following that header.
