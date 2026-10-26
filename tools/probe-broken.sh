#!/bin/sh
# Thrown away: exists only to show that the lint job fails on a real mistake.
work=$1
cd $work
rm -rf $work/build
grep -q thing $work/file
if [ "$?" != "0" ]; then
    echo "missing"
fi
