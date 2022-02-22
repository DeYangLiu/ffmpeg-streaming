#mingw: download dll and dev packages from http://ffmpeg.zeranoe.com/builds,
#suppose merge them to /e/tools/Player/ffmpeg/, and then invoke:
#modify the following variables to suit your needs.
FF_PATH := /e/tools/Player/ffmpeg
T3_PATH := e:/mingw_dgn_lib/third
WIN_LIBS := -lssl -lz -ldl #-lws2_32 -lgdi32 -lcrypt32

CFLAGS := -g -O0 -std=c99 # -Os -std=c99 #

FF_LIBS := -lavdevice -lavfilter -lavformat -lavcodec -lpostproc -lswresample -lswscale -lavutil

all: server remuxing

# -DPLUGIN_ZLIB=1 plugin_zlib.c   -Werror -Wmissing-prototypes
server: ffserver.c compact.c avstring.c
	gcc -o $@ $(CFLAGS) -DFFMPEG_SRC=0   $^ \
  -lz -Id:/MinGW/include -Ld:/MinGW/lib \
 -I$(T3_PATH) -L$(T3_PATH) -static \
 -DPLUGIN_SSL=1 plugin_ssl.c -lssl -lcrypto  $(WIN_LIBS) 


remuxing: remuxing.c
	gcc -o $@ $(CFLAGS) $^ -I $(FF_PATH)/include  -L $(FF_PATH)/lib $(FF_LIBS)

clean:
	rm -f *.o *.exe server remuxing


file_server: ffserver.c compact.c avstring.c
	gcc -o $@ $(CFLAGS) -DFFMPEG_SRC=0   -Werror -Wmissing-prototypes $^  -static $(WIN_LIBS)


file_server.linux: ffserver.c compact.c avstring.c
	gcc -o $@ $(CFLAGS) -DPLUGIN_SSL=1 -DFFMPEG_SRC=0  plugin_ssl.c -lssl -lcrypto  $^  # -static

test_client: http_client.c
	gcc -o $@ $(CFLAGS) -DFFMPEG_SRC=0 -Werror -Wmissing-prototypes $^ -static $(WIN_LIBS)

test_client.linux: http_client.c
	gcc -o $@ $(CFLAGS) -DFFMPEG_SRC=0 -Werror -Wmissing-prototypes $^ -static