#include <libavformat/avformat.h>

#include "libavutil/opt.h"
#include "libavutil/buffer.h"
#include "libavutil/log.h"
#include "libavutil/intreadwrite.h"


#define WB64(buf, x) AV_WB64(buf, x); buf += 8;
#define WB32(buf, x) AV_WB32(buf, x); buf += 4;
#define WB16(buf, x) AV_WB16(buf, x); buf += 2;
#define W8(buf, x)  *buf = x; buf += 1;

#define RB64(buf) ({uint64_t tmp = AV_RB64(buf); buf += 8; tmp;})
#define RB32(buf) ({uint32_t tmp = AV_RB32(buf); buf += 4; tmp;})
#define RB16(buf) ({uint16_t tmp = AV_RB16(buf); buf += 2; tmp;})
#define R8(buf) ({uint8_t tmp = *buf; buf += 1; tmp;})

#define HEADER_SIZE (8*1024)


static int header_write(AVFormatContext *s, uint8_t *pb, uint32_t size)
{
    AVDictionaryEntry *t;
    AVStream *st;
    AVCodecContext *codec;
    int bit_rate, i;
	uint8_t *pb0 = pb;

	WB32(pb, s->nb_streams);
    bit_rate = 0;
    for(i=0;i<s->nb_streams;i++) {
        st = s->streams[i];
        bit_rate += st->codec->bit_rate;
    }
    WB32(pb, bit_rate);

    /* list of streams */
    for(i=0;i<s->nb_streams;i++) {
        st = s->streams[i];

		WB32(pb, st->pts_wrap_bits);
		WB32(pb, st->time_base.num);
		WB32(pb, st->time_base.den);

        codec = st->codec;
		if(AV_CODEC_ID_NONE == codec->codec_id){
			printf("no code id\n");
			return 0;
		}
        /* generic info */
		printf("header codec_id %x flags %x extra size %d gop %d\n",  codec->codec_id, codec->flags, codec->extradata_size, codec->gop_size);
		if(codec->extradata_size > 0){
			codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
		}
		
        WB32(pb, codec->codec_id);
        W8(pb, codec->codec_type);
        WB32(pb, codec->bit_rate);
        WB32(pb, codec->flags);
        WB32(pb, codec->flags2);
        WB32(pb, codec->debug);
		
        if (codec->flags & CODEC_FLAG_GLOBAL_HEADER) {
			if(pb + codec->extradata_size - pb0 > size - 57*4){
				printf("write too large size %u\n", codec->extradata_size);
				return 0;
			}
            WB32(pb, codec->extradata_size);
            memcpy(pb, codec->extradata, codec->extradata_size);
			pb += codec->extradata_size;
        }
        /* specific info */
        switch(codec->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            WB32(pb, codec->time_base.num);
            WB32(pb, codec->time_base.den);
            WB16(pb, codec->width);
            WB16(pb, codec->height);
            WB16(pb, codec->gop_size);
            WB32(pb, codec->pix_fmt);
            W8(pb, codec->qmin);
            W8(pb, codec->qmax);
            W8(pb, codec->max_qdiff);
            WB16(pb, (int) (codec->qcompress*1000));
            WB16(pb, (int) (codec->qblur*1000));
            WB32(pb, codec->bit_rate_tolerance);
            WB32(pb, codec->rc_max_rate);
            WB32(pb, codec->rc_min_rate);
            WB32(pb, codec->rc_buffer_size);
            WB64(pb, av_double2int(codec->i_quant_factor));
            WB64(pb, av_double2int(codec->b_quant_factor));
            WB64(pb, av_double2int(codec->i_quant_offset));
            WB64(pb, av_double2int(codec->b_quant_offset));
            WB32(pb, codec->dct_algo);
            WB32(pb, codec->strict_std_compliance);
            WB32(pb, codec->max_b_frames);
            WB32(pb, codec->mpeg_quant);
            WB32(pb, codec->intra_dc_precision);
            WB32(pb, codec->me_method);
            WB32(pb, codec->mb_decision);
            WB32(pb, codec->nsse_weight);
            WB32(pb, codec->frame_skip_cmp);
            WB64(pb, av_double2int(codec->rc_buffer_aggressivity));
            WB32(pb, codec->codec_tag);
            W8(pb, codec->thread_count);
            WB32(pb, codec->coder_type);
            WB32(pb, codec->me_cmp);
            WB32(pb, codec->me_subpel_quality);
            WB32(pb, codec->me_range);
            WB32(pb, codec->keyint_min);
            WB32(pb, codec->scenechange_threshold);
            WB32(pb, codec->b_frame_strategy);
            WB64(pb, av_double2int(codec->qcompress));
            WB64(pb, av_double2int(codec->qblur));
            WB32(pb, codec->max_qdiff);
            WB32(pb, codec->refs);
            break;
        case AVMEDIA_TYPE_AUDIO:
            WB32(pb, codec->sample_rate);
            WB16(pb, codec->channels);
            WB16(pb, codec->frame_size);
            break;
        default:
            return -1;
        }
    }
  

    return pb - pb0;
}

static int header_read(AVFormatContext *s, uint8_t *pb, uint32_t size)
{
    AVStream *st;
    AVCodecContext *codec;
    int i,ret, tmp32, nb_streams;
	uint64_t tmp64;
	uint8_t *pb0 = pb;

    nb_streams = RB32(pb);
    RB32(pb); /* total bitrate */
    /* read each stream */
    for(i=0;i<nb_streams;i++) {
        st = avformat_new_stream(s, NULL);
        if (!st){
            goto fail;
        }
		
		st->pts_wrap_bits = RB32(pb);
		st->time_base.num = RB32(pb);
		st->time_base.den = RB32(pb);

        codec = st->codec;
        /* generic info */
        codec->codec_id = RB32(pb);
        codec->codec_type = R8(pb); /* codec_type */
        codec->bit_rate = RB32(pb);
        codec->flags = RB32(pb);
        codec->flags2 = RB32(pb);
        codec->debug = RB32(pb);

		if(AV_CODEC_ID_NONE == codec->codec_id){
			printf("read no code id\n");
			goto fail;
		}
		
		if (codec->flags & CODEC_FLAG_GLOBAL_HEADER) {
			tmp32 = RB32(pb);
			if(pb + tmp32 - pb0 > size - 57*4){
				printf("read too large size %u\n", tmp32);
				goto fail;
			}
		    ret = ff_alloc_extradata(codec, tmp32);
			memcpy(codec->extradata, pb, tmp32);
			pb += tmp32;
			printf("header codec_id %x flags %x extra size %d\n",  codec->codec_id, codec->flags,  codec->extradata_size);
        }
        /* specific info */
        switch(codec->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            codec->time_base.num = RB32(pb);
            codec->time_base.den = RB32(pb);
            codec->width = RB16(pb);
            codec->height = RB16(pb);
            codec->gop_size = RB16(pb);
            codec->pix_fmt = RB32(pb);
            codec->qmin = R8(pb);
            codec->qmax = R8(pb);
            codec->max_qdiff = R8(pb);
            codec->qcompress = RB16(pb)/1000.0;
            codec->qblur = RB16(pb)/1000.0;
            codec->bit_rate_tolerance = RB32(pb);

            codec->rc_max_rate = RB32(pb);
            codec->rc_min_rate = RB32(pb);
            codec->rc_buffer_size = RB32(pb);
			tmp64 = RB64(pb);
            codec->i_quant_factor = av_int2double(tmp64);
			tmp64 = RB64(pb);
            codec->b_quant_factor = av_int2double(tmp64);
			tmp64 = RB64(pb);
            codec->i_quant_offset = av_int2double(tmp64);
			tmp64 = RB64(pb);
            codec->b_quant_offset = av_int2double(tmp64);
            codec->dct_algo = RB32(pb);
            codec->strict_std_compliance = RB32(pb);
            codec->max_b_frames = RB32(pb);
            codec->mpeg_quant = RB32(pb);
            codec->intra_dc_precision = RB32(pb);
            codec->me_method = RB32(pb);
            codec->mb_decision = RB32(pb);
            codec->nsse_weight = RB32(pb);
            codec->frame_skip_cmp = RB32(pb);
			tmp64 = RB64(pb);
            codec->rc_buffer_aggressivity = av_int2double(tmp64);
            codec->codec_tag = RB32(pb);
            codec->thread_count = R8(pb);
            codec->coder_type = RB32(pb);
            codec->me_cmp = RB32(pb);
            codec->me_subpel_quality = RB32(pb);
            codec->me_range = RB32(pb);
            codec->keyint_min = RB32(pb);
            codec->scenechange_threshold = RB32(pb);
            codec->b_frame_strategy = RB32(pb);
			tmp64 = RB64(pb);
            codec->qcompress = av_int2double(tmp64);
			tmp64 = RB64(pb);
            codec->qblur = av_int2double(tmp64);
            codec->max_qdiff = RB32(pb);
            codec->refs = RB32(pb);
            break;
        case AVMEDIA_TYPE_AUDIO:
            codec->sample_rate = RB32(pb);
            codec->channels = RB16(pb);
            codec->frame_size = RB16(pb);
            break;
        default:
			goto fail;
            break;
        }
    }

    return pb - pb0;
fail:
	for(i = s->nb_streams - 1; i >= 0; --i){
		ff_free_stream(s, s->streams[i]);
	}
	return 0;
}

int ff_header_write(AVFormatContext *s, const char *name, int postfix)
{
	unsigned char *buf = NULL;
	char full_name[128] = "";
	int len = 0;
	FILE *fp = NULL;

	if(s->nb_streams < 1){
		printf("no streams\n");
		return 0;
	}

	buf = malloc(HEADER_SIZE);
	if(!buf){
		printf("cant malloc\n");
		return 0;
	}
	memset(buf, 0, HEADER_SIZE);
	
	len = header_write(s, buf, HEADER_SIZE);
	if(len <= 0){
		len = 0;
		goto fail;
	}

	sprintf(full_name, "/var/ffh.%s.%d", name, postfix);
	fp = fopen(full_name, "wb");
	if(!fp){
		len = 0;
		goto fail;
	}

	fwrite(buf, 1, len, fp);
	fclose(fp);
fail:
	free(buf);
	return len;
}

int ff_header_read(AVFormatContext *s, const char *name, int postfix)
{
	unsigned char *buf = NULL;
	char full_name[128] = "";
	int len = 0;
	FILE *fp = NULL;

	if(s->nb_streams > 0){
		printf("already nb_streams %d\n", s->nb_streams);
		return 0;
	}

	buf = malloc(HEADER_SIZE);
	if(!buf){
		printf("cant malloc\n");
		return 0;
	}
	memset(buf, 0, HEADER_SIZE);

	sprintf(full_name, "/var/ffh.%s.%d", name, postfix);
	fp = fopen(full_name, "rb");
	if(!fp){
		free(buf);
		return 0;
	}
	len = fread(buf, 1, HEADER_SIZE, fp);
	fclose(fp);
	
	len = header_read(s, buf, HEADER_SIZE);

	free(buf);
	return len;	
}
