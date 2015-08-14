/*generate thumbnail frome key packet.

usage:
 av_copy_packet(&pkt1, &pkt);
 ff_thumbnail_generate(in_stream->codec, &pkt1, "dump2.jpg", 300, 200);

Notes:
 1. av_frame_alloc need "refcounted_frames=1".
 2. when decoding, avcodec_open2.avctx need be set.

author: ludi 2014.11
ref:
http://dranger.com/ffmpeg/tutorial01.html
doc/examples/transcoding
*/

#include "ff_utils.h"

#include <stdio.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>



typedef struct OutputStream {/*a wrapper around a single output AVStream*/
    AVStream *st;
    AVFrame *frame; /*re-usable frame*/
    struct SwsContext *sws_ctx;
} OutputStream;

static int frame_write(AVFormatContext *oc, OutputStream *ost, AVFrame *in_frame)
{
	int ret = -1;
	AVCodecContext *c = NULL;
	AVFrame *frame = NULL;
	int got_packet = 0;
	AVPacket pkt = { 0 };

	/*get_video_frame*/
	c = ost->st->codec;
	if (!ost->sws_ctx) {
		//printf("fmt %d --> %d\n", in_frame->format, c->pix_fmt);
		ost->sws_ctx = sws_getContext(in_frame->width, in_frame->height, in_frame->format,
				c->width, c->height, c->pix_fmt,
				SWS_BICUBIC, NULL, NULL, NULL);
		if (!ost->sws_ctx) {
			fprintf(stderr,
					"Could not initialize the conversion context\n");
			exit(1);
		}
	}
	sws_scale(ost->sws_ctx,
			(const uint8_t * const *)in_frame->data, in_frame->linesize,
			0, in_frame->height, ost->frame->data, ost->frame->linesize);
	frame = ost->frame;
	

	/* encode the image */
	av_init_packet(&pkt);
	ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
	if (ret < 0) {
		fprintf(stderr, "Error encoding video frame: %s\n", av_err2str(ret));
		exit(1);
	}

	if (got_packet) {
		av_packet_rescale_ts(&pkt, c->time_base, ost->st->time_base);
	    pkt.stream_index = ost->st->index;
	    ret = av_interleaved_write_frame(oc, &pkt);
	} 

	return (frame || got_packet) ? 0 : 1;
}


static int encode_frame(AVFrame *frame, const char *filename, int w, int h)
{
    OutputStream video_st = { 0 };
    AVOutputFormat *fmt;
    AVFormatContext *oc;
    AVCodec *video_codec;
    int ret;

	enum AVCodecID codec_id = -1;
	AVCodecContext *c;

	AVFrame *picture;

    ret = avformat_alloc_output_context2(&oc, NULL, NULL, filename);
    if (!oc){
        return -1;
    }
    fmt = oc->oformat;

	/*open_stream*/
	codec_id = fmt->video_codec;
    video_codec = avcodec_find_encoder(codec_id);
    if (!video_codec) {
        fprintf(stderr, "Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
    }

    video_st.st = avformat_new_stream(oc, video_codec);
    if (!video_st.st) {
        fprintf(stderr, "Could not allocate stream\n");
		return -2;
    }
    video_st.st->id = oc->nb_streams-1;
    c = video_st.st->codec;
	
    c->codec_id = codec_id;
    c->bit_rate = 400000;
    c->width    = (w>>1)<<1; /* Resolution must be a multiple of two. */
    c->height   = (h>>1)<<1;
    c->time_base.den = 25;
    c->time_base.num = 1;
    c->gop_size      = 1; 
    c->pix_fmt       = video_codec->pix_fmts[0]; 
    if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        /* just for testing, we also add B frames */
        c->max_b_frames = 2;
    }
    if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        /* Needed to avoid using macroblocks in which some coeffs overflow.
         * This does not happen with normal video, it just happens here as
         * the motion of the chroma plane does not match the luma plane. */
        c->mb_decision = 2;
    }

    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    /*open_video*/
    c = video_st.st->codec;
    ret = avcodec_open2(c, video_codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
    }

	/*alloc_picture*/
    picture = av_frame_alloc();
    if (!picture){
		fprintf(stderr, "Could not alloc frame\n");
        return -3;
    }
    picture->format = c->pix_fmt;
    picture->width  = c->width;
    picture->height = c->height;
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        return -4;
    }
	video_st.frame = picture;

    //av_dump_format(oc, 0, filename, 1);
    ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
    ret = avformat_write_header(oc, NULL);
	
	
	ret = frame_write(oc, &video_st, frame);
   
    av_write_trailer(oc);

	avcodec_close(video_st.st->codec);
    av_frame_free(&video_st.frame);
    sws_freeContext(video_st.sws_ctx);
	
    avio_close(oc->pb);
    avformat_free_context(oc);
    return ret;
}


static AVFrame* decode_frame(AVCodecContext *rctx, AVPacket *pkt)
{
    AVFrame *frame = NULL;
	int ret = 0, got_frame = 0;
	AVCodec *in_codec = NULL;
	AVCodecContext *in_codec_ctx = NULL;
	AVDictionary *opts = NULL;
	AVPacket pkt1 = *pkt;

	in_codec = avcodec_find_decoder(rctx->codec_id);
	if(!in_codec){
		printf("cant find codec_id %x\n", rctx->codec_id);
		goto end;
	}
	in_codec_ctx = avcodec_alloc_context3(in_codec);
	ret = avcodec_copy_context(in_codec_ctx, rctx);
    if (ret < 0) {
        printf("Failed to copy context\n");
        goto end;
    }
    in_codec_ctx->codec_tag = 0;
    in_codec_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;

	av_dict_set(&opts, "refcounted_frames", "1", 0);
	ret = avcodec_open2(in_codec_ctx, in_codec, &opts);
	if(ret < 0){
		printf("cant open decode id %x\n", rctx->codec_id);
		goto end;
	}

	frame = av_frame_alloc();
	ret = avcodec_decode_video2(in_codec_ctx, frame, &got_frame, pkt);
	if(ret < 0){
		printf("cant decode:%s\n", av_err2str(ret));
		goto end;
	}
	if(got_frame){
		goto end;
	}
	
	pkt1.data = NULL;
	pkt1.size = 0;
	ret = avcodec_decode_video2(in_codec_ctx, frame, &got_frame, &pkt1);
	if(ret < 0){
		printf("cant flush decode:%s\n", av_err2str(ret));
		goto end;
	}
	
end:
	avcodec_close(in_codec_ctx);
	av_free(in_codec_ctx);
	av_free_packet(pkt);
	if(ret < 0){
		av_frame_free(&frame);
		frame = NULL;
	}
	return frame;
}


int ff_thumbnail_generate(AVCodecContext *rctx, AVPacket *pkt, const char *filename, int width, int height)
{
	AVFrame *frame = NULL;
	int ret = -1;
	
	frame = decode_frame(rctx, pkt);
	if(!frame)return ret;

	if((AV_PIX_FMT_NONE == frame->format) || !frame->width || !frame->height){
		return ret;
	}
	
	ret = encode_frame(frame, filename, width, height);
	return ret;
}

