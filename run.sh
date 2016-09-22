#!/bin/sh

$PIN_ROOT/pin -t obj-intel64/SBPT.so -- $SB_ROOT/build/kfusion/kfusion-benchmark-cpp -i $SB_ROOT/../living_room_traj2_loop
