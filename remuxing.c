/*
 Remux streams from one container format to another.
 */

#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>

int sff_write_block(AVIOContext *pb, int type, uint8_t *data, uint32_t size);
int sff_write_header(AVFormatContext *ctx);
int sff_write_packet(AVFormatContext *ctx, AVPacket *pkt);

#if 1
static char *ctime1(char *buf2, int buf_size)
{
	int64_t t = av_gettime_relative()/1000;
	uint32_t h, m, s, ss;
	ss = t%1000; t = t/1000;
	s = t%60;  t = t/60;
	m = t%60;  t = t/60;
	h = t;	
	snprintf(buf2, buf_size, "[%02u:%02u:%02u.%03u]", h, m, s, ss);
	return buf2;
}

static void http_vlog(const char *fmt, va_list vargs)
{
    static int print_prefix = 1;
	FILE *logfile = stdout;
	
    if (logfile) {
        if (print_prefix) {
            char buf[32];
            ctime1(buf, sizeof(buf));
            fprintf(logfile, "%s ", buf);
        }
        print_prefix = strstr(fmt, "\n") != NULL;
        vfprintf(logfile, fmt, vargs);
        fflush(logfile);
    }
}

#ifdef __GNUC__
__attribute__ ((format (printf, 1, 2)))
#endif
static void http_log(const char *fmt, ...)
{
    va_list vargs;
    va_start(vargs, fmt);
    http_vlog(fmt, vargs);
    va_end(vargs);
}
#endif

int sff_write_block(AVIOContext *pb, int type, uint8_t *data, uint32_t size)
{
	uint8_t tmp[8];
	//printf("size %u\t", size);

	type = type % 10;
	sprintf(tmp, "SFF%d", type);
	avio_write(pb, tmp, 4);
	avio_wb32(pb, size);
	avio_write(pb, data, size);
	return 0;
}


int sff_write_header(AVFormatContext *ctx)
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
		return -1;
	}
		
	size = avio_close_dyn_buf(ctx->pb, &buf);
	ctx->pb = old;
	if(size > 0)
		ret = sff_write_block(ctx->pb, 1, buf, size);
	av_freep(&buf);	
	return ret;
}

int sff_write_packet(AVFormatContext *ctx, AVPacket *pkt)
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

#define M 4
static int64_t start_time[M], curr_time[M];
static int64_t first_dts[M] = {0};
static int64_t curr_dts[M] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE};

int main(int argc, char **argv)
{
    AVOutputFormat *ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
	AVDictionary *out_opts = NULL;
    AVPacket pkt;
    const char *fmt, *in_filename, *out_filename;
    int ret, i, cmp;
	int cnt = 0;

    if (argc < 3) {
        printf("usage: remuxing input output fmt\n");
        return 1;
    }

    in_filename  = argv[1];
    out_filename = argv[2];
	fmt = argv[3];

	av_log_set_level(AV_LOG_DEBUG |AV_LOG_SKIP_REPEATED); // 

    av_register_all();
    avformat_network_init();

	http_log("before open input\n");
    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
        fprintf(stderr, "Could not open input file '%s'", in_filename);
        goto end;
    }
	http_log("before find stream\n");
    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        fprintf(stderr, "Failed to retrieve input stream information");
        goto end;
    }
	http_log("before dump\n");
    av_dump_format(ifmt_ctx, 0, in_filename, 0);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, fmt, out_filename);
    if (!ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    ofmt = ofmt_ctx->oformat;

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
        if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy context from input to output stream codec context\n");
            goto end;
        }
        out_stream->codec->codec_tag = 0;
        if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }
    av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if (!(ofmt->flags & AVFMT_NOFILE)) {
		av_dict_set(&out_opts, "chunked_post", "0", 0);	
        ret = avio_open2(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE, NULL, &out_opts);

        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            goto end;
        }
    }

    ret = sff_write_header(ofmt_ctx); //avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        goto end;
    }
	avio_flush(ofmt_ctx->pb);

    while (1) {
        AVStream *in_stream, *out_stream;
		ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0)
            break;

        in_stream  = ifmt_ctx->streams[pkt.stream_index];
        out_stream = ofmt_ctx->streams[pkt.stream_index];
		
		i = pkt.stream_index;
		if(curr_dts[i] == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE){
			first_dts[i] = pkt.dts;
			start_time[i] = av_gettime_relative();
		}
		if(pkt.dts != AV_NOPTS_VALUE){	
			curr_dts[i] = pkt.dts; //us
		}

        /* copy packet */
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;

        ret = sff_write_packet(ofmt_ctx, &pkt); //av_interleaved_write_frame(ofmt_ctx, &pkt);
		avio_flush(ofmt_ctx->pb);
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
        av_free_packet(&pkt);
		++cnt;
		//printf("cnt %d\t", cnt);
		
		do{
			curr_time[i] = av_gettime_relative();
			cmp = av_compare_ts(curr_dts[i] - first_dts[i], in_stream->time_base, 
					curr_time[i] - start_time[i], AV_TIME_BASE_Q); 
			if(cmp <= 0)break;
			
			av_usleep(10000);
		}while(cmp > 0);
		
    }

    sff_write_packet(ofmt_ctx, NULL); //av_write_trailer(ofmt_ctx);
end:

    avformat_close_input(&ifmt_ctx);

    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE) && ofmt_ctx->pb){
        avio_close(ofmt_ctx->pb);
		av_dict_free(&out_opts);
	}
    avformat_free_context(ofmt_ctx);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return 1;
    }
	
	printf("end of remux\n");
    return 0;
}
