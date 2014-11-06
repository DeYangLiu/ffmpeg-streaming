#ifndef _FF_UTILS_H_
#define _FF_UTILS_H_

#include <stdint.h>
#include <libavformat/avformat.h>

#include "libavutil/opt.h"
#include "libavutil/buffer.h"
#include "libavutil/log.h"
#include "libavutil/intreadwrite.h"

typedef struct{
	int64_t dts, next_dts, pts, next_pts;

	int64_t last_ts, ts_offset;
	int saw_first_ts;
}TimestampContext;

int ff_ts_init(TimestampContext *ist);
int ff_ts_wrap(AVFormatContext *ctx, AVStream *st, TimestampContext *ts, AVPacket *pkt);

#endif


