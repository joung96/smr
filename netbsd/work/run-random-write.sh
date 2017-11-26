#!/usr/pkg/bin/bash

sudo ./setup.sh hdd ffs raw reset random-write-4k > fio_output/random-write-4k.txt 
sudo ./setup.sh hdd ffs raw reset random-write-16k > fio_output/random-write-16k.txt 
sudo ./setup.sh hdd ffs raw reset random-write-256k > fio_output/random-write-256k.txt 
sudo ./setup.sh hdd ffs raw reset random-write-1m > fio_output/random-write-1m.txt 
sudo ./setup.sh hdd ffs raw reset random-write-8m > fio_output/random-write-8m.txt 
sudo ./setup.sh hdd ffs raw reset random-write-16m > fio_output/random-write-16m.txt
