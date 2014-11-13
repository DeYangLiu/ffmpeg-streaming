#ifndef _FF_UTILS_H_
#define _FF_UTILS_H_

#include <stdint.h>
#include <signal.h>
#include <libavformat/avformat.h>

#include "libavutil/opt.h"
#include "libavutil/buffer.h"
#include "libavutil/log.h"
#include "libavutil/intreadwrite.h"

#define MAX_URL_NAME_LENGTH 128
#define FF_MSG_CTRL 1234
typedef struct{
	long type; 

	long cmd;
	long para[4];
	char info[MAX_URL_NAME_LENGTH];
}msg_ctl_t;

typedef struct{
	int64_t dts, next_dts, pts, next_pts;

	int64_t last_ts, ts_offset;
	int saw_first_ts;
}TimestampContext;

int ff_ts_init(TimestampContext *ist);
int ff_ts_wrap(AVFormatContext *ctx, AVStream *st, TimestampContext *ts, AVPacket *pkt);

int sff_write_block(AVIOContext *pb, int type, uint8_t *data, uint32_t size);
int ff_wrapper_write_header(AVFormatContext *ofmt_ctx);
int ff_wrapper_write_packet(AVFormatContext *ofmt_ctx, AVPacket *pkt);
int ff_wrapper_write_trailer(AVFormatContext *ofmt_ctx);

int ff_filter_init(int type);
int ff_filter_close(int type);
int ff_filter_data(AVFormatContext *fmt_ctx, AVPacket *pkt);

#endif


