#include <stdio.h>
#include "ff_utils.h"

int ff_ts_init(TimestampContext *ts)
{
	memset(ts, 0, sizeof(*ts));
	ts->dts = ts->pts = ts->next_dts = ts->next_pts = 
		ts->last_ts = ts->ts_offset = AV_NOPTS_VALUE;

	return 0;
}

int ff_ts_wrap(AVFormatContext *ctx, AVStream *st, TimestampContext *ts, AVPacket *pkt)
{/*handle timestamp overflow and discontuity.*/
	int ret = 0;
	int64_t delta, stime, stime2;
	const int64_t dts_delta_threshold = st->time_base.den/st->time_base.num * 10ULL;
	const int64_t trick_delta_min = st->time_base.den/st->time_base.num * 0.1;
	
	if(ctx->start_time != AV_NOPTS_VALUE && st->pts_wrap_bits < 64){
		if (ts->next_dts == AV_NOPTS_VALUE && ts->ts_offset == AV_NOPTS_VALUE ) {
			ts->ts_offset = -ctx->start_time; 
		}

		stime = ctx->start_time;
		stime2= stime + (1ULL<<st->pts_wrap_bits);

		if(stime2 > stime && pkt->dts != AV_NOPTS_VALUE && pkt->dts > stime + (1LL<<(st->pts_wrap_bits-1))) {
			pkt->dts -= 1ULL<<st->pts_wrap_bits;
			printf("wrap dts %lld\n", pkt->dts);
		}
		if(stime2 > stime && pkt->pts != AV_NOPTS_VALUE && pkt->pts > stime + (1LL<<(st->pts_wrap_bits-1))) {
			pkt->pts -= 1ULL<<st->pts_wrap_bits;
			printf("wrap pts %lld\n", pkt->pts);
		}
	}
	
	if (pkt->dts != AV_NOPTS_VALUE)
		pkt->dts += ts->ts_offset;
	if (pkt->pts != AV_NOPTS_VALUE)
		pkt->pts += ts->ts_offset;
			
	if (pkt->dts != AV_NOPTS_VALUE && ts->next_dts != AV_NOPTS_VALUE ){
		delta   = pkt->dts - ts->next_dts;
		if (FFABS(delta) >  dts_delta_threshold ||
			pkt->dts + trick_delta_min < FFMAX(ts->pts, ts->dts)) {
				printf("wrap delta %lld before offset %lld dts %lld pts %lld\n", delta, ts->ts_offset, pkt->dts, pkt->pts);
				ts->ts_offset -= delta;
				pkt->dts -= delta;
				if (pkt->pts != AV_NOPTS_VALUE)
						pkt->pts -= delta;
				
				ret = 1;
			}
	}
	
	if (pkt->dts != AV_NOPTS_VALUE)
		ts->last_ts = pkt->dts;
				
	if (!ts->saw_first_ts) {
		delta = st->avg_frame_rate.num ? - st->codec->has_b_frames * (st->time_base.den/st->time_base.num) / av_q2d(st->avg_frame_rate) : 0;
		ts->dts = delta;
		ts->pts = 0;
		if (pkt->pts != AV_NOPTS_VALUE) {
			ts->dts += pkt->pts;
			ts->pts = ts->dts; 
		}
		ts->saw_first_ts = 1;
		printf("saw_first_ts %lld delta %lld\n", ts->dts, delta);
	}

	if (ts->next_dts == AV_NOPTS_VALUE)
		ts->next_dts = ts->dts;
	if (ts->next_pts == AV_NOPTS_VALUE)
		ts->next_pts = ts->pts;
	if (pkt->dts != AV_NOPTS_VALUE) {
		ts->next_dts = ts->dts = pkt->dts;
		ts->next_pts = ts->pts = ts->dts;
	}
	
	ts->dts = ts->next_dts;
	ts->next_dts += pkt->duration;
	ts->pts = ts->dts;
	ts->next_pts = ts->next_dts;
		
	return ret;
}


