#!/bin/bash


rm -rf test_aup
gcc ../aup.c ../aup.h test_aup.c -o test_aup
./test_aup

