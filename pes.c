
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/crc.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/avassert.h"
#include "avformat.h"
#include "internal.h"
#include "avio_internal.h"

#define MAX_PES_HEADER_SIZE 19 
#define MAX_PES_PAYLOAD_SIZE (512*1024)
typedef struct{
	AVClass *class; /*for priv_class.option, must be first*/
	enum AVCodecID codec_id; /*AV_CODEC_ID_XX*/
}PesContext;

static int is_pes_start_code(int32_t code)
{
	if((code & 0xfffffff0) == (0x1e0 & 0xfffffff0) ){
		return 1; /*video*/
	}else if((code & 0xffffffe0) == (0x1c0 & 0xffffffe0) ){
		return 2; /*audio*/
	}
	return 0;
}

static inline int64_t parse_pes_pts(const uint8_t *buf) {
    return (int64_t)(*buf & 0x0e) << 29 |
            (AV_RB16(buf+1) >> 1) << 15 |
             AV_RB16(buf+3) >> 1;
}

static int pes_read_probe(AVProbeData *p)
{
    const uint8_t *ptr, *end;
	uint32_t code;
	int found  = 0;

    end = p->buf + p->buf_size - sizeof(uint32_t);
    for(ptr = p->buf; ptr < end; ++ptr) {
		code = AV_RB32(ptr);	
		if(found=is_pes_start_code(code))break;

    }
	return found ? AVPROBE_SCORE_EXTENSION + 1 : 0;
}

static int pes_read_header(AVFormatContext *s)
{
    PesContext *pes = s->priv_data;
    AVStream *st = NULL;

	if(!s->nb_streams && pes->codec_id != AV_CODEC_ID_NONE){
	    st = avformat_new_stream(s, NULL);
	    if (!st)
	        return AVERROR(ENOMEM);
	    st->codec->codec_type = pes->codec_id >= AV_CODEC_ID_MP2 ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
		st->codec->codec_id = pes->codec_id;
	}

	if(1 == s->nb_streams){
		s->streams[0]->need_parsing  = AVSTREAM_PARSE_HEADERS; 
	}else{
		av_log(NULL, AV_LOG_ERROR, "nb_streams %d\n", s->nb_streams);
		return AVERROR_INVALIDDATA;
	}
	
    return 0;
}

static int read_to_next_sync(AVIOContext *pb, uint8_t *buf, int *size)
{/*byte-to-byte manner*/
	uint8_t *ptr = buf, *ptr_end = buf + *size;
	int found = 0;

	for(ptr = buf; !avio_feof(pb) && (ptr < ptr_end); ){
		*ptr = avio_r8(pb);	
		if(*ptr++ != 0x00)continue;
		*ptr = avio_r8(pb);	
		if(*ptr++ != 0x00)continue;
		*ptr = avio_r8(pb);	
		if(*ptr++ != 0x01)continue;
		*ptr = avio_r8(pb);	
		if(is_pes_start_code((0x000001<<8)|(*ptr++) )){
			found = 1;
			break;
		}
	}
	if(found){
		avio_seek(pb, -4, SEEK_CUR);
		*size = ptr - buf - 4;
	}else{
		*size = ptr - buf;
	}

	return 0;
}

static int sync_to_startcode(AVIOContext *pb)
{
	uint8_t tmp;
	int found = 0;

	for(; !avio_feof(pb); ){
		tmp = avio_r8(pb);	
		if(tmp != 0x00)continue;
		tmp = avio_r8(pb);	
		if(tmp != 0x00)continue;
		tmp = avio_r8(pb);	
		if(tmp != 0x01)continue;
		tmp = avio_r8(pb);	
		if(is_pes_start_code((0x000001<<8)|tmp)){
			found = 1;
			break;
		}
	}
	if(found){
		avio_seek(pb, -4, SEEK_CUR);
	}else{
		av_log(NULL, AV_LOG_ERROR, "sync to start code fail\n");
	}
	return 0;
}
static int pes_read_packet(AVFormatContext *s, AVPacket *pkt)
{
	PesContext *pes = s->priv_data;
	AVIOContext *pb = s->pb;
	int64_t delta, pos = 0,  size = MAX_PES_PAYLOAD_SIZE;
	int64_t pts = AV_NOPTS_VALUE, dts = AV_NOPTS_VALUE;
	int32_t code = 0;
	int ret = 0,  header_size = 0, pes_header_size = 0, remains = 0, tmp = 0;
	int total_size = 0;
	unsigned int flags = 0, pes_ext = 0, skip = 0;
	uint8_t *r = NULL, buf[MAX_PES_HEADER_SIZE ] = "";
	AVStream *st = s->streams[0];

	pos = avio_tell(pb);
	header_size = avio_read(pb, buf, sizeof(buf));
	if(header_size < 9 ){
		return AVERROR_EOF;
	}

	code = is_pes_start_code(AV_RB32(buf));
	if(!code){/*dont waste time on corrupted data due to cbuf overwrite.*/
		sync_to_startcode(pb);
		return AVERROR(EAGAIN);
	}

	total_size = AV_RB24(buf+4); /*ludi extended:use three bytes*/
	flags = AV_RB8(buf+7); 
	pes_header_size = AV_RB8(buf+8) + 9; 
	r = buf + 9;
	if ((flags & 0xc0) == 0x80) {
		dts = pts = parse_pes_pts(r);
		r += 5;
	} else if ((flags & 0xc0) == 0xc0) {
		pts = parse_pes_pts(r);
		r += 5;
		dts = parse_pes_pts(r);
		r += 5;
	}

	pos += pes_header_size ; 
_end:
	if(total_size && size > total_size)
		size = total_size + 6 - pes_header_size;
	if(av_new_packet(pkt, size) < 0){
		return AVERROR(ENOMEM);
	}
	
	//printf("pes%d dts %lld header %d %d size %u %u\n", code, dts, header_size, pes_header_size, total_size, pkt->size);
	
	avio_seek(pb, pos, SEEK_SET);
	if(total_size){
		avio_read(pb, pkt->data, pkt->size);
	}else{
		av_log(NULL, AV_LOG_ERROR, "pes internal error\n");
	}
	
	if(AVSTREAM_PARSE_NONE == st->need_parsing)
		pkt->flags = AV_PKT_FLAG_KEY; /*let hlsenc can_split*/
	pkt->stream_index = 0;
	pkt->dts = dts;
	pkt->pts = pts;

	return ret;
}


static const AVOption options[] = {
    { "codec_id", "AV_CODEC_ID_XX", offsetof(PesContext, codec_id), AV_OPT_TYPE_INT, {.i64 = AV_CODEC_ID_NONE}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM},
    { NULL },
};

static const AVClass demuxer_class = {
    .class_name = "pes class",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

AVInputFormat ff_pes_demuxer = {
    .name           = "pes",
    .long_name      = NULL_IF_CONFIG_SMALL("Packeted Elementary Stream"),
    .read_probe     = pes_read_probe,
    .read_header    = pes_read_header,
    .read_packet    = pes_read_packet,
    .priv_data_size = sizeof(PesContext),
    .flags          = 0,
    .extensions     = "pes", 
    .priv_class     = &demuxer_class,
};
