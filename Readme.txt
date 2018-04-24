Background
====
I tested that the original ffserver cant streaming flv with h264+aac[1].
Also no direct support for HLS.

For my own project, it reads pes data from set top box. I observed that audio pes is always with start code,
whereas video not, so I need assemble a complete pes before sending it to ffmpeg library interfaces.
I had also tried read raw ts, but due to limited cpu speed, my box cant handle it in real-time. 

HLS streaming, in my original understanding, is just a file server, response file get request[2]. 
This thinking is good enough in previous days, but on shotest responce time, it's not enough.
So here comes http output, when partial data of a segment ts flow in server, the server put it to guest, no longer need to
waiting a complete segment.


Compilation
====
1. You need a decent ffmpeg source, then overwrite them with files in this repo.

2. add a line to 
allformats.c: REGISTER_DEMUXER (PES,              pes);
libavformat/Makefile: OBJS-$(CONFIG_PES_DEMUXER)            	 += pes.o

3. ../ffmpeg/configure --cross-prefix=arm-hisiv200-linux- --enable-cross-compile --target-os=linux --arch=arm --cpu=armv7-a --disable-asm --prefix=$HOME/porting/target/ffv300 --disable-shared  --enable-gpl --disable-optimizations

4.
make -j16 && make install



How to test
====
Server
./ffserver &
./remuxing -i input.mp4 -codec copy http://localhost:80/out.m3u8

PC client
ffplay.exe http://server_ip:80/out.m3u8

test case
====
wget -d http://localhost:8080/stream/xx.jpg
./remuxing.exe test.jpg http://localhost:8080/stream/xx.jpg  data

ffplay http://localhost:8080/stream/out.avi
./remuxing.exe music.ts http://localhost:8080/stream/out.avi avi

ffplay http://localhost:8080/stream/out.m3u8
./remuxing.exe music.ts http://localhost:8080/stream/out.m3u8 hls


file_server
====
target: cd /data/ && mkdir upload && file_server -http_port 80
pc: curl --upload-file your-file --url http://192.168.1.102/upload/


[1] http://blog.csdn.net/deyangliu/article/details/39898169
[2] http://blog.csdn.net/deyangliu/article/details/39187623
