
apt-get update
apt-get install libevent-dev
apt-get install ffmpeg
./configure   --enable-gpl   --enable-shared    --enable-nonfree  --enable-libass   --enable-libfdk-aac   --enable-libfreetype   --enable-libmp3lame   --enable-libopus   --enable-libtheora   --enable-libvorbis   --enable-libvpx   --enable-libx264   --enable-libx265 --cc="gcc -m64 -fPIC"
