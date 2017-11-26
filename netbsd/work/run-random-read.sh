#!/usr/pkg/bin/bash

sudo ./setup.sh hdd ffs raw reset random-read-4k > fio_output/random-read-4k.txt 
sudo ./setup.sh hdd ffs raw reset random-read-16k > fio_output/random-read-16k.txt 
sudo ./setup.sh hdd ffs raw reset random-read-256k > fio_output/random-read-256k.txt 
sudo ./setup.sh hdd ffs raw reset random-read-1m > fio_output/random-read-1m.txt 
sudo ./setup.sh hdd ffs raw reset random-read-8m > fio_output/random-read-8m.txt 
sudo ./setup.sh hdd ffs raw reset random-read-16m > fio_output/random-read-16m.txt
