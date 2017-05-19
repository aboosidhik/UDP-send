# UDP-send
http://qiita.com/dandelion1124/items/fb12461e41e70ccc161e
g++ rtcp_rtp_audio.cpp -I/usr/local/include -L/usr/local/lib -lavcodec -lavformat -lswscale -lavfilter -lavutil -lx264 -lz -lm -lopencv_imgproc -lopencv_highgui -lopencv_core `pkg-config --cflags --libs opencv` `pkg-config --cflags --libs glib-2.0` -lm -lvpx -ljansson -std=gnu++11
