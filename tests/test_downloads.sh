#! /usr/bin/env bash

# TODO clean up properly if it fails
set -eou pipefail

mkdir test_downloads_workdir
cd test_downloads_workdir

cp ../../release/server .
mkdir -p base/maps
cp ../../base/maps/carfentanil.bsp.zst base/maps

./server &
sleep 0.25s

curl localhost:44400/base/maps/carfentanil.bsp.zst --silent --output carfentanil.bsp.zst
cmp carfentanil.bsp.zst base/maps/carfentanil.bsp.zst

! curl localhost:44400/base/maps/bad.bsp.zst

kill %1

cd ..
rm -r test_downloads_workdir
