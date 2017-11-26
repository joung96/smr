#!/usr/pkg/bin/bash

sudo ./setup.sh hdd ffs raw reset seq-read-4k > fio_output/seq-read-4k.txt 
sudo ./setup.sh hdd ffs raw reset seq-read-16k > fio_output/seq-read-16k.txt 
sudo ./setup.sh hdd ffs raw reset seq-read-256k > fio_output/seq-read-256k.txt 
sudo ./setup.sh hdd ffs raw reset seq-read-1m > fio_output/seq-read-1m.txt 
sudo ./setup.sh hdd ffs raw reset seq-read-8m > fio_output/seq-read-8m.txt 
sudo ./setup.sh hdd ffs raw reset seq-read-16m > fio_output/seq-read-16m.txt
