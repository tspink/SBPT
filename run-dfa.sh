#!/bin/sh

make && $PIN_ROOT/pin -t obj-intel64/SBPT-DFA.so $* -- $SB_ROOT/kfusion/kfusion-benchmark-cpp -i $SB_ROOT/../living_room_traj2_loop.raw  -s 4.8 -p 0.34,0.5,0.24 -z 4 -c 2 -r 1 -k 481.2,480,320,240
