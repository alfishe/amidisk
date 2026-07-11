#!/bin/bash
set -e

echo "Creating 10GB blank Amiga drive with single default partition..."
mkdir -p scratch
python3 amidisk_python/src/amidisk create scratch/test_10gb.hdf --size 10G --layout default --force

echo "Streaming 12GB tar archive natively into the image using SCP syntax..."
time python3 amidisk_python/src/amidisk put scratch/test_10gb.hdf:DH0/ /Volumes/172.16.17.101-3/Amiga/WHDLoad-Games-and-Demos-Unpacked.tar

echo "Done!"
