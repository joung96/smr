#!/usr/pkg/bin/bash

sudo ./hdd_setup.sh hdd ffs raw reset random-read-4k > fio_output/random-read-4k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-read-16k > fio_output/random-read-16k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-read-256k > fio_output/random-read-256k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-read-1m > fio_output/random-read-1m.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-read-8m > fio_output/random-read-8m.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-read-16m > fio_output/random-read-16m.txt

sudo ./hdd_setup.sh hdd ffs raw reset random-write-4k > fio_output/random-write-4k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-write-16k > fio_output/random-write-16k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-write-256k > fio_output/random-write-256k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-write-1m > fio_output/random-write-1m.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-write-8m > fio_output/random-write-8m.txt 
sudo ./hdd_setup.sh hdd ffs raw reset random-write-16m > fio_output/random-write-16m.txt

sudo ./hdd_setup.sh hdd ffs raw reset seq-read-4k > fio_output/seq-read-4k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-read-16k > fio_output/seq-read-16k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-read-256k > fio_output/seq-read-256k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-read-1m > fio_output/seq-read-1m.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-read-8m > fio_output/seq-read-8m.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-read-16m > fio_output/seq-read-16m.txt

sudo ./hdd_setup.sh hdd ffs raw reset seq-write-4k > fio_output/seq-write-4k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-write-16k > fio_output/seq-write-16k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-write-256k > fio_output/seq-write-256k.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-write-1m > fio_output/seq-write-1m.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-write-8m > fio_output/seq-write-8m.txt 
sudo ./hdd_setup.sh hdd ffs raw reset seq-write-16m > fio_output/seq-write-16m.txt
