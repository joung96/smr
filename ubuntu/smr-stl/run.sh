# create the img file
dd of=image.img conv=notrunc if=/dev/zero bs=4k count=1024k

./mkfakesmr --bandsize=16m image.img
./format  --group-bands=122 --map-bands=10 --over-provisioning=1.05 image.img


sudo nbdkit -f -v ./stl-plugin.so device=image.img
sudo nbd-client localhost 10809 /dev/nbd0 -b 4096

sudo mkdir /mnt/nbd
sudo mount /dev/nbd0 /mnt/nbd

