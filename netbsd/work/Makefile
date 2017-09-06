all:
	cc -O2   -o stl_sample stl_sample.c -lutil
	cc -O2   -o stl_sample2 stl_sample2.c -lutil
	cc -O2   -o stl_sample3 stl_sample3.c -lutil
	cc -O2   -o stl_sample4 stl_sample4.c -lutil
	cc -O2   -o cgd_sample cgd_sample.c -lutil
run:
	sudo /usr/src/sbin/atactl/obj/atactl /dev/rwd0a reset_all_write_pointers
	# XXX this is non-seq...
	sudo disklabel -R -r /dev/rwd0d tests/smr_lfs_disklabel
	sudo ./smr-stl/format --map-bands 7 --group-bands 247 --over-provisioning 1.05 /dev/rwd0a
	sudo ./stl_sample
	#sudo ./stl_sample2
	sudo /usr/src/sbin/newfs/obj/newfs -O2 -s97440512 /dev/rstl0d
	sudo /usr/src/sbin/mount_ffs/obj/mount_ffs /dev/stl0d /mnt/test

bench:
	./setup.sh hdd ffs raw noreset random-read

fsck:
	sudo /usr/src/sbin/fsck_ffs/obj/fsck_ffs -f /dev/rstl0d
