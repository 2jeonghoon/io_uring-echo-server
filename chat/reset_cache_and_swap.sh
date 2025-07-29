#!/bin/bash

sync

# page cache 무효화
echo 3 | sudo tee /proc/sys/vm/drop_caches

# 스왑 재시작
sudo swapoff -a
sudo swapon -a

