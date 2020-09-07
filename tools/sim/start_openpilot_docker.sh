#!/bin/bash

# expose X to the container
xhost +local:root
#docker pull commaai/openpilot-sim:latest

docker run --net=host\
  --rm \
  -it \
  --gpus all \
  --device=/dev/dri/  \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  --shm-size 1G \
  -e DISPLAY=$DISPLAY \
  commaai/openpilot-sim:latest \
  /bin/bash -c 'cd /openpilot && tmux'
  #/bin/bash \

#docker run --shm-size 1G --rm --net=host -e PASSIVE=0 -e NOBOARD=1 -e NOSENSOR=1 --volume="$HOME/.Xauthority:/root/.Xauthority:rw" --gpus all -e DISPLAY=$DISPLAY -it commaai/openpilot-sim:latest /bin/bash
