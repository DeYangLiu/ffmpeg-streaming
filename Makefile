#mingw: download dll and dev packages from http://ffmpeg.zeranoe.com/builds,
#suppose merge them to /e/tools/Player/ffmpeg/, and then invoke:
#modify the following variables to suit your needs.
FF_PATH := /e/tools/Player/ffmpeg
T3_PATH := /e/mingw_dgn_lib/third
WIN_LIBS := -lws2_32 -lgdi32

CFLAGS := -Os #-g -O0

FF_LIBS := -lavdevice -lavfilter -lavformat -lavcodec -lpostproc -lswresample -lswscale -lavutil

all: server remuxing

server: ffserver.c compact.c avstring.c
	gcc -o $@ $(CFLAGS) -DFFMPEG_SRC=0   -Werror -Wmissing-prototypes $^ \
 -DPLUGIN_ZLIB=1 plugin_zlib.c -lz \
<<<<<<< HEAD
 -I $(T3_PATH) -L $(T3_PATH) -static

# -DPLUGIN_SSL=1 plugin_ssl.c -lssl -lcrypto  $(WIN_LIBS) \
=======
 -I $(T3_PATH) -L $(T3_PATH) -static $(WIN_LIBS) # -DPLUGIN_SSL=1 plugin_ssl.c -lssl -lcrypto

>>>>>>> d98d3f2f40eb53ba0c8c2a894eda98a62fd7107c

remuxing: remuxing.c
	gcc -o $@ $(CFLAGS) $^ -I $(FF_PATH)/include  -L $(FF_PATH)/lib $(FF_LIBS)

clean:
	rm -f *.o *.exe server remuxing
<<<<<<< HEAD
=======

file_server: ffserver.c compact.c avstring.c
	gcc -o $@ $(CFLAGS) -DFFMPEG_SRC=0   -Werror -Wmissing-prototypes $^  -static $(WIN_LIBS)
>>>>>>> d98d3f2f40eb53ba0c8c2a894eda98a62fd7107c
