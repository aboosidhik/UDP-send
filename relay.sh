nano /etc/apt/sources.list
deb http://deb.debian.org/debian/ stable main contrib non-free
deb-src http://deb.debian.org/debian/ stable main contrib non-free

deb http://deb.debian.org/debian/ stable-updates main contrib non-free
deb-src http://deb.debian.org/debian/ stable-updates main contrib non-free

deb http://deb.debian.org/debian-security stable/updates main
deb-src http://deb.debian.org/debian-security stable/updates main

deb http://ftp.debian.org/debian jessie-backports main
deb-src http://ftp.debian.org/debian jessie-backports main

apt-get update
apt-get install libevent-dev
apt-get install ffmpeg
./configure   --enable-gpl   --enable-shared    --enable-nonfree  --enable-libass   --enable-libfdk-aac   --enable-libfreetype   --enable-libmp3lame   --enable-libopus   --enable-libtheora   --enable-libvorbis   --enable-libvpx   --enable-libx264   --enable-libx265 --cc="gcc -m64 -fPIC"
