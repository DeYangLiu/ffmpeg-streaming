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

int sff_write_block(AVIOContext *pb, int type, uint8_t *data, uint32_t size)
{
	uint8_t tmp[8];
	//printf("size %u\t", size);

	type = type % 10;
	sprintf(tmp, "SFF%u", type);
	avio_write(pb, tmp, 4);
	avio_wb32(pb, size);
	avio_write(pb, data, size);
	return 0;
}


static int sff_write_header(AVFormatContext *ctx)
{
	uint32_t size = 0; 
	uint8_t *buf = NULL;
	int ret = 0;
	AVIOContext *pb = NULL, *old = ctx->pb;
	if(!ctx ){
		return -1;
	}	

	if(avio_open_dyn_buf(&pb) < 0){
		return -1;
	}
	ctx->pb = pb;
	if(avformat_write_header(ctx, NULL) < 0){
		ctx->pb = old;
		return -1;
	}
		
	size = avio_close_dyn_buf(ctx->pb, &buf);
	ctx->pb = old;
	if(size > 0)
		ret = sff_write_block(ctx->pb, 1, buf, size);
	av_freep(&buf);	
	return ret;
}

static int sff_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
	AVIOContext *pb = NULL, *old = NULL;
	uint8_t *buf = NULL;
	int size = 0, frame_size = -1, ret = -1;
	static int seq = 0;
	++seq;

	if(!ctx || !ctx->pb)return 0;
	if(avio_open_dyn_buf(&pb) < 0){
		return -1;
	}
	pb->seekable = 0;
	old = ctx->pb;
	ctx->pb = pb;
	
	/*add more pkt info if needed, here stream_index only for illustration.*/
	if(pkt){
		avio_wb32(pb, pkt->stream_index);
		ret = av_interleaved_write_frame(ctx, pkt);
	}else{
		avio_wb32(pb, (uint32_t)-1);
		ret = av_write_trailer(ctx);
	}
	size = avio_close_dyn_buf(pb, &buf);
	frame_size = size - 4;
	ctx->pb = old;

	if(frame_size <= 0){
		//printf("write frame fail err %x %d:%d\n", ret, seq, frame_size);
		goto fail;/*its ok*/
	}
	if(ret < 0){
		printf("write frame fail err %x %d:%d\n", ret, seq, frame_size);
		goto fail;
	}
	ret = sff_write_block(ctx->pb, 2, buf, size);
fail:
	av_freep(&buf);	
	return ret;
}


static AVBitStreamFilterContext *aacbsf = NULL;
int ff_filter_init(int type)
{
	aacbsf = av_bitstream_filter_init("aac_adtstoasc");
	return 0;
}

int ff_filter_close(int type)
{
	av_bitstream_filter_close(aacbsf);
	aacbsf = NULL;
	return 0;
}

int ff_filter_data(AVFormatContext *fmt_ctx, AVPacket *pkt)
{
	int i, ret = -1;
	AVCodecContext *codec = NULL;

	if(!pkt->data || (pkt->size < 2)){
		return ret;
	}

	if(fmt_ctx->oformat && !strcmp(fmt_ctx->oformat->name, "flv")){
		for(i = 0; i < fmt_ctx->nb_streams; ++i){
			codec = fmt_ctx->streams[i]->codec;
			if(AV_CODEC_ID_AAC == codec->codec_id){				
				ret = av_bitstream_filter_filter(aacbsf, codec, NULL, 
						&pkt->data, &pkt->size, pkt->data, pkt->size, 0);
			}
		}
	}
	
	return ret;
}

int ff_wrapper_write_header(AVFormatContext *ofmt_ctx)
{
	if(ofmt_ctx->oformat && !strcmp(ofmt_ctx->oformat->name, "flv")){
		return sff_write_header(ofmt_ctx);
	}

	return avformat_write_header(ofmt_ctx, NULL);
}

int ff_wrapper_write_packet(AVFormatContext *ofmt_ctx, AVPacket *pkt)
{
	if(ofmt_ctx->oformat && !strcmp(ofmt_ctx->oformat->name, "flv")){
		return sff_write_packet(ofmt_ctx, pkt);
	}

	return av_interleaved_write_frame(ofmt_ctx, pkt); 	
}

int ff_wrapper_write_trailer(AVFormatContext *ofmt_ctx)
{
	if(ofmt_ctx->oformat && !strcmp(ofmt_ctx->oformat->name, "flv")){
		return sff_write_packet(ofmt_ctx, NULL);
	}
	return av_write_trailer(ofmt_ctx);
}

