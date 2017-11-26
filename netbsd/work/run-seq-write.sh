#!/usr/pkg/bin/bash

sudo ./setup.sh hdd ffs raw reset seq-write-4k > fio_output/seq-write-4k.txt 
sudo ./setup.sh hdd ffs raw reset seq-write-16k > fio_output/seq-write-16k.txt 
sudo ./setup.sh hdd ffs raw reset seq-write-256k > fio_output/seq-write-256k.txt 
sudo ./setup.sh hdd ffs raw reset seq-write-1m > fio_output/seq-write-1m.txt 
sudo ./setup.sh hdd ffs raw reset seq-write-8m > fio_output/seq-write-8m.txt 
sudo ./setup.sh hdd ffs raw reset seq-write-16m > fio_output/seq-write-16m.txt
