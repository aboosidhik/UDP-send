
. ./myscript.sh

nano /etc/apt/sources.list
deb http://deb.debian.org/debian/ stable main contrib non-free
deb-src http://deb.debian.org/debian/ stable main contrib non-free

deb http://deb.debian.org/debian/ stable-updates main contrib non-free
deb-src http://deb.debian.org/debian/ stable-updates main contrib non-free

deb http://deb.debian.org/debian-security stable/updates main
deb-src http://deb.debian.org/debian-security stable/updates main

deb http://ftp.debian.org/debian jessie-backports main
deb-src http://ftp.debian.org/debian jessie-backports main
#!/bin/sh
apt-get update
apt-get install libevent-dev -y
apt-get install yasm -y
apt-get install libass-dev -y
apt-get install libfdk-aac-dev -y
apt-get install libmp3lame-dev -y
apt-get install libtheora-dev -y
apt-get install libvorbis-dev -y
apt-get install libvpx-dev -y
apt-get install libx264-dev -y
apt-get install libx265-dev -y
git clone https://github.com/FFmpeg/FFmpeg.git
 #etc.

./configure   --enable-gpl   --enable-shared    --enable-nonfree  --enable-libass   --enable-libfdk-aac   --enable-libfreetype   --enable-libmp3lame   --enable-libopus   --enable-libtheora   --enable-libvorbis   --enable-libvpx   --enable-libx264   --enable-libx265 --cc="gcc -m64 -fPIC"
