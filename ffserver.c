/*
 multiple format streaming server based on the FFmpeg libraries
 */

//#define PLUGIN_DVB
//#define PLUGIN_SSDP
#define PLUGIN_DIR 1
#define FFMPEG_SRC 0

#define DIR_UPLOAD_MAX_SIZE ((int64_t)2*1024*1024*1024*1024)
#define FILE_BUF_SIZE (2*1024*1024)

#define _BSD_SOURCE  /*for struct ip_mreq, must be precede all header files.*/
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h> //PRId64
#include <ctype.h>

#if FFMPEG_SRC == 0
#include "compact.h"
#else
 #if HAVE_CLOSESOCKET  == 1
  #define close_socket closesocket
 #else
  #define close_socket close
 #endif

#include "libavformat/avformat.h"
#include "libavformat/network.h"
#include "libavformat/os_support.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/time.h"
#endif

#include <stdarg.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
//#include <sys/ioctl.h>
#include <sys/stat.h>
#if HAVE_POLL_H
#include <poll.h>
#endif
#include <errno.h>
#include <time.h>
//#include <sys/wait.h>
#include <signal.h>
//#include <netinet/tcp.h>

#if HAVE_ICONV_H
#include <iconv.h>
#endif

#define MIN_SIZE_OF_LOCAL_FILE_FOR_CHUNKED (8*1024)
#define CHUNK_HEAD_LEN 8
#define IOBUFFER_INIT_SIZE 8192
/* timeouts are in ms */
#define HTTP_REQUEST_TIMEOUT (15 * 1000)

#define S 10
#define SEG_INC_SIZE (4*1024)

typedef struct{
	uint8_t *data; /*seg data*/
	int msize; /*memory allocated size*/
	int csize; /*current size*/
	int flag; /*1 -- writting, 2 -- completed*/
}HLS;


#define N (128)
typedef struct{
	int type;
	int size;
	uint8_t *data;
	int wpos;
	int wflag; /*1--is writing, cant be read*/
}SFF;

typedef struct{
	uint8_t *data;
	int size;
	uint32_t stream_index;
}packet_t;

typedef enum {
    HTTPSTATE_WAIT_REQUEST = 1,
    HTTPSTATE_SEND_HEADER,
    HTTPSTATE_SEND_DATA_HEADER,
    HTTPSTATE_SEND_DATA,  /* sending TCP or UDP data */
    HTTPSTATE_SEND_DATA_TRAILER,
    HTTPSTATE_RECEIVE_DATA,
    HTTPSTATE_WAIT_FEED,  /* wait for data from the feed */
    HTTPSTATE_CLOSE, /*msg of close this conection*/

}HTTPState ;

/* context associated with one connection */
typedef struct HTTPContext {
    HTTPState state;
    int fd; /* socket file descriptor */
    struct sockaddr_in from_addr; /* origin */
    struct pollfd *poll_entry; /* used when polling */
    int64_t timeout;
    struct HTTPContext *next;

    char *url;
	int post;
	int http_error;
	int keep_alive; /*whether response has Content-Length.*/
	int64_t content_length;
	int64_t data_count;
    int last_packet_sent; /* true if last data packet was sent */
	
    int buffer_size;
    uint8_t *buffer; /*for parsing http request*/
    uint8_t *pb_buffer; /*dynamic buffer, currently for m3u8-ts files*/
	uint8_t *buffer_ptr, *buffer_end;
	
	int hls_idx; /*array index, S -- m3u8*/
	int hls_wpos; /*write pos*/
	int hls_rpos; /*read pos*/

	/*writer specific*/
	SFF *sff; /*current incomple pkt*/
	SFF *sff_pkts[N+1]; 
	int sff_w;
	int sff_ref_cnt; /*being quotied count*/
	/*reader specific*/
	struct HTTPContext *feed_ctx; /*data source*/
	int sff_r;

	int local_fd; /*local file handle*/
	int64_t total_count; /*http header + file size*/
	int only_header; /*HEAD request*/
	#if PLUGIN_SSL
	void* ssl;
	#endif
	#if PLUGIN_ZLIB
	void *z_st;
	#endif
	int http_version; /*0 -- 1.0, 1 -- 1.1, 2 -- 2.0*/
	int tr_encoding; /*transfer-encoding method: 1 -- chunked.*/
	int chunk_size; /*post chunked size*/
	#if PLUGIN_DIR
	char inm[32]; /*If-None-Match*/
	char ims[32]; /*If-Modified-Since*/
	#endif
	char xToken[64];
} HTTPContext;

typedef struct{/*extra data not in HTTPContext*/
	char domain[64];
	char cookie[512];
	uint8_t content[512];
	char range[32];
	char expect[32];
}RequestData;

static struct sockaddr_in my_http_addr;
static struct sockaddr_in my_https_addr;
static HTTPContext *first_http_ctx;

static void log_connection(HTTPContext *c);

static void new_connection(int server_fd, int flag);
static void close_connection(HTTPContext *c);

static int handle_connection(HTTPContext *c);
static int http_parse_request(HTTPContext *c);
static int http_send_data(HTTPContext *c);
static int http_receive_data(HTTPContext *c);

static int mkdirRecursive(char *path, unsigned int mode);

/* maximum number of simultaneous HTTP connections */
static unsigned int nb_max_http_connections = 2000;
static unsigned int nb_max_connections = 5;
static unsigned int nb_connections;
static uint64_t max_bandwidth = 1000;
static int64_t cur_time; 

static char sUploadToken[64];

static FILE *logfile = NULL;
static void http_log(const char *fmt, ...);
static int hls_close(void);
static int sff_close(void);

#if defined(PLUGIN_DVB)
#include "stream_buffer.c"
#include "plugin_dvb.c"

static int ctl_msg_cb(ctrl_msg_t *msg)
{
	http_log("ctl msg: %d '%s' '%s'\n", msg->cmd, msg->name, msg->data);
	if(5 == msg->cmd){
		if(av_match_ext(msg->data, "m3u8")){
			hls_close();
		}
		ff_ctl_send_string(5, msg->name, msg->data);
	}
	return 0;
}
#endif

#if defined(PLUGIN_SSDP)
#include "plugin_ssdp.c"
#endif

#if PLUGIN_DIR
#include "plugin_dir.c"
#endif

#if PLUGIN_SSL
#include "plugin_ssl.h"
#define recv(fd, buf, len, mode) (c->ssl ? ssl_read(c->ssl, buf, len) : recv(fd, buf, len, mode))
#define send(fd, buf, len, mode) (c->ssl ? ssl_write(c->ssl, buf, len) : send(fd, buf, len, mode))
#endif

#if PLUGIN_ZLIB
#include "plugin_zlib.h"
#endif

int checkToken(struct HTTPContext *c)
{
	return strncmp(c->xToken, sUploadToken, sizeof(c->xToken)-1);
}

int mkdirRecursive(char *path, unsigned int mode)
{
	char *strPath = strdup(path);
	if (!strPath) {
		return -1;
	}

	int ret = 0;
	char *ptr = strPath;
	for (; *ptr; ++ptr) {
		if (ptr > strPath && *ptr == '/') {
			*ptr = 0;
			if (access(strPath, F_OK)) {
				#if HAVE_WINDOWS_H
				ret = mkdir(strPath);
				#else
				ret = mkdir(strPath, mode);
				#endif
				if (ret) {
					http_log("cant mkdir '%s'", strPath);
					ret = -2;
					break;
				}
				else {
					//printf("ok mkdir '%s'", strPath));
				}
			}
			else {
				//printf("exist '%s'", strPath));
			}
			*ptr = '/';
			
			if (*(ptr+1) == '/') { //skip ...//...
				++ptr;
			}
		}
	}
	
	free(strPath);
	return ret;
}

static int preparePathOK(char *path)
{
	if (!path) {
		return 0;
	}

	struct stat stBuf;
	int ret = -1;

	ret = stat(path, &stBuf);
	if (0 == ret) {
		return 1;
	}

	if (mkdirRecursive(path, 0777)) {
		http_log("cant prepare dir '%s'", path);
	}
	
	ret = stat(path, &stBuf);
	
	return ret == 0 ? 1 : 0;
}


static struct in_addr get_host_ip(void)
{
	struct in_addr retAddr;
	memset(&retAddr, 0, sizeof(retAddr));
	
	char ac[256] = "";
	if (gethostname(ac, sizeof(ac))) {
		return retAddr;
	}
	
	struct hostent *phe = gethostbyname(ac);
	if (!phe) {
		return retAddr;
	}
	http_log("host: '%s' ip ", ac);
	
	int i;
	for (int i = 0; phe->h_addr_list[i] != 0; ++i) {
        struct in_addr addr;
        memcpy(&addr, phe->h_addr_list[i], sizeof(struct in_addr));
        http_log("%s ", inet_ntoa(addr));
		memcpy(&retAddr, &addr, sizeof addr);
    }
	http_log("\n");
	
	return retAddr;
}

static int prepare_response(HTTPContext *c, int code, char *reason)
{
	int len = snprintf(c->buffer, c->buffer_size, 
			"HTTP/1.1 %d %s\r\n"
			"\r\n\r\n", 
			code, reason);

	c->buffer_ptr = c->buffer; 
	c->buffer_end = c->buffer + len;
	c->state = HTTPSTATE_SEND_HEADER;
	return 0;
}

static unsigned hex2int(char c)
{
	c = toupper(c);
	return isdigit(c) ? c - '0' : c - 'A' + 10;
}

static int url_decode(unsigned char *src)
{
	int i, j;
	for(i = 0, j = 0; src[i]; ){
		if('%' == src[i]){
			if(isxdigit(src[i+1]) && isxdigit(src[i+2])){
				src[j++] = (hex2int(src[i+1])<<4)|hex2int(src[i+2]); 
				i+=3;
			}else{
				src[j++] = src[i++];
			}
		}else{
			src[j++] = src[i++];
		}
	}
	src[j] = 0;
	return j;
}

static int is_utf8(unsigned char *s)
{
	int cnt = 0, err = 0;

	for(; *s; ++s){
		if((s[0]>>7) == 0){//1 Byte
			++cnt;
		}
		else if((s[0]>>5) == 0x6){// 2 Bytes
			if((s[1]>>6) == 0x2){
				s += 1;
				++cnt;
			}else{
				err++;
			}
		}
		else if((s[0]>>4) == 0xE){// 3 Bytes
			if((s[1] >> 6) == 0x2 && (s[2] >> 6) == 0x2){
				s += 2;
				++cnt;
			}else{
				err++;
			}
		}
		else if((s[0]>>3) == 0x1E){// 4 Bytes
			if((s[1] >> 6) == 0x2 && (s[2] >> 6) == 0x2 && (s[3] >> 6) == 0x2){
				s += 3;
				++cnt;
			}else{
				err++;
			}
		}
		
		if(err){
			return 0;
		}
	}
	//printf("cnt %d err %d\n", cnt, err);
	
	return !err;
}

static int url_local(unsigned char *utf8, int len)
{
	if(!utf8 || len < 1){
		http_log("bad arg len %d\n", len);
		return 0;
	}
	
	#if defined(_WIN32)
	if(!is_utf8(utf8)){
		return 0;
	}

	int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	if(wn <= 0){
		return 0;
	}

	wchar_t *wc = (wchar_t *)av_malloc(wn * sizeof(wchar_t));
	if(!wc){
		http_log("malloc for local fail\n");
		return 0;
	}
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wc, wn);

	int mn = WideCharToMultiByte(CP_ACP, 0, wc, -1, NULL, 0, NULL, NULL);
	unsigned char *mb = (unsigned char*)av_malloc(mn+1);
	WideCharToMultiByte(CP_ACP, 0, wc, -1, mb, mn, NULL, NULL);
	printf("mb '%s' mn %d\n", mb, mn);

	if(mn < len){
		memcpy(utf8, mb, mn);
		utf8[mn] = 0;
	}

	av_freep(&wc);
	av_freep(&mb);
	#elif HAVE_ICONV_H
	if(is_utf8(utf8)){
		return 0;
	}
	static iconv_t ic_g2u = (iconv_t)-1;
	iconv_t ic;
	size_t inlen0, inlen, outlen0, outlen;
	char *outbuf, *pin, *pout;
	int ret;
	
	if((iconv_t)-1 == ic_g2u){
		ic_g2u = iconv_open("utf-8", "gbk");
	}
	ic = ic_g2u;
	
	inlen0 = inlen = strlen(utf8);
	outlen0 = outlen = inlen*3;
	pin = (char*)utf8;	
	pout = outbuf = av_malloc(outlen);
	if(!pout){
		http_log("malloc for local fail\n");
		return 0;
	}
	
	ret = iconv(ic, &pin, &inlen, &pout, &outlen);	
	if(!ret && !inlen){
		strncpy(utf8, outbuf, len-1);
		utf8[len-1] = 0;
	}else{
		http_log("conv fail ret %d left in %d\n", ret, (int)inlen);
	}
	av_freep(&outbuf);
	#endif
	return 1;
}

static const char* get_mine_type(char *name)
{
	int i, n;

	//more types: see https://www.iana.org/assignments/media-types/media-types.xhtml
	//order: longer match first
	static const char* mm[][2] = {
		".htm", "text/html",
		".m3u8", "text/plain",
		".ts", "video/MP2T",
		".flv", "video/MP2T",
		".xml", "text/xml",
		".css", "text/css",
		".h", "text/plain",
		".c", "text/plain",
		".txt", "text/plain",
		"", "application/octet-stream",
	};
	if(!name){
		return "";
	}

	n = sizeof(mm)/sizeof(mm[0]);
	for(i = 0; i < n; ++i){
		if(av_stristr(name, mm[i][0]))break;
	}
	if(i >= n)i = n -1;

	return mm[i][1];
}

static int prepare_local_file(HTTPContext *c, RequestData *rd)
{/*return 1 if local file exist and can be read to buffer.*/
    int64_t len = 0, len0 = 0;
	unsigned char *tmp = NULL;
	int tlen = 0;
	int ret = 0;
	char prefix[32] = ".";
	int fd = -1;
	struct stat st = {0};
	int64_t off_start = 0, off_end = 0;
	int64_t size = 0, wanted = 0, tried = 0;
	int status = 200;
	char *msg = "OK";
	
	if(!c->url){
		return 0;
	}
	tlen = strlen(c->url)+16;
	tmp = av_malloc(tlen);
	if(!tmp){
		http_log("malloc fail for len %d\n", tlen);
		return 0;
	}

	snprintf(tmp, tlen, "%s/%s",  prefix, c->url);

	http_log("begin open local file %s\n", c->url);
	
	fd = open(tmp, O_RDONLY);
	if(fd < 0){
		goto end;
	}
	if((fstat(fd, &st) < 0) || (st.st_size > SIZE_MAX)){
		goto end;
	}

	char dt_lm_etag[256] = "";
	#if PLUGIN_DIR
	char lm[64] = "", etag[64] = "";
	int modified = dir_is_modifed(c, &st, NULL, lm, etag, 64);
	snprintf(dt_lm_etag, sizeof(dt_lm_etag), 
			"Cache-Control:max-age=0, must-revalidate\r\nLast-Modified: %s\r\nEtag: %s\r\n", lm, etag);
	if(!modified){
		close(fd);
		c->http_error = 304;
		c->local_fd = -1;
		c->pb_buffer = av_malloc(512);
		if(!c->pb_buffer){
			http_log("cant alloc for zlib\n");
			goto end;
		}

		len0 = sprintf(c->pb_buffer, "HTTP/1.1 %d Not Modified\r\n"
				"%s"
				"Connection: %s\r\n"
				"\r\n",
				c->http_error,
			    dt_lm_etag,	
				(c->keep_alive ? "keep-alive" : "close") );
		len = 0;

		goto tail;
	}
	#endif

	
	if( (st.st_size >= MIN_SIZE_OF_LOCAL_FILE_FOR_CHUNKED)
		&& (c->keep_alive
		#if PLUGIN_ZLIB
		|| c->z_st
		#endif
		)){
		char *content_encoding = "";
		char *transfer_encoding = "";

		c->tr_encoding = c->http_version >= 1 ? 1 : 0;
		c->local_fd = fd;
		c->http_error = 0;
		c->pb_buffer = av_malloc(512);
		if(!c->pb_buffer){
			http_log("cant alloc for zlib\n");
			goto end;
		}

		if(1 == c->tr_encoding){
			transfer_encoding = "Transfer-Encoding: chunked\r\n";
		}

		if(0 == c->tr_encoding){
			c->keep_alive = 0; /*close as boundary*/
		}

		#if PLUGIN_ZLIB
		if(c->z_st){
			content_encoding = "Content-Encoding: gzip\r\n";
		}
		#endif

		len0 = sprintf(c->pb_buffer, "HTTP/1.1 200 OK\r\n"
				"%s%s"
				"Content-type: %s\r\n"
				"%s"
				"Connection: %s\r\n"
				"\r\n", 
				content_encoding, transfer_encoding,
				get_mine_type(c->url),
				dt_lm_etag,
				(c->keep_alive ? "keep-alive" : "close"));
		len = 0;

		goto tail;
	}
	
	//Range:bytes=start-end
	if(!strncmp(rd->range, "bytes=", 6)){
		char *ptr = rd->range + 6;
		off_start = strtoll(ptr, NULL, 10);
		ptr = strchr(ptr, '-');
		if(ptr && strlen(ptr) > 1){
			off_end = strtoll(ptr+1, NULL, 10);
		}
		status = 206;
		msg = "Partial Content";
		if(off_end <= 0 || off_end >= st.st_size){
			off_end = st.st_size - 1;
		}
		dt_lm_etag[0] = 0;
	}
	wanted = (206 == status ? off_end-off_start+1 : st.st_size);
	size = FFMIN(wanted + 1024, FILE_BUF_SIZE);

	c->pb_buffer = av_malloc(size);
	if(!c->pb_buffer){
        c->buffer_ptr = c->buffer;
        c->buffer_end = c->buffer;
		close(fd);
		goto end;
	}
	
	len0 = sprintf(c->pb_buffer, "HTTP/1.1 %d %s\r\n"
			"Content-type: %s\r\n"
			"Accept-Ranges: bytes\r\n"
			"Content-Range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n"
			"Content-Length: %" PRId64 "\r\n"
			"%s"
			"Connection: %s\r\n"
			"\r\n", 
			status, msg, //200, "OK", 
			get_mine_type(c->url),
			(int64_t)off_start, 
			(int64_t)(206 == status ? off_end : st.st_size-1), 
			(int64_t)st.st_size,
			(int64_t)(206 == status ? off_end-off_start+1 : st.st_size), 
			dt_lm_etag,
			(c->keep_alive ? "keep-alive" : "close") );

	tried = FFMIN(size-len0, wanted);
	lseek(fd, (off_t)off_start, SEEK_SET);
	len = read(fd, c->pb_buffer + len0, tried);
	if(len < 0){
		http_log("local file read err %d\n", len);
		av_freep(&c->pb_buffer);
		goto end;
	}

	if(len == wanted){
		close(fd);
		c->http_error = 200;
		c->local_fd = -1;
	}else{
		c->local_fd = fd;
		c->http_error = 0;
		c->total_count = len0 + wanted;
	}
tail:
	http_log("local file %s size %" PRId64 " head %" PRId64 " range '%s'\n", 
			c->url, (int64_t)st.st_size, len0, rd->range);

    c->buffer_ptr = c->pb_buffer;
    c->buffer_end = c->pb_buffer + (c->only_header ? 0 : len) + len0;
	ret = 1;
end:
	av_freep(&tmp);
	return ret;
}

static int local_prepare_data(HTTPContext *c)
{/*prepare: 0 -- ok, <0 -- err, >0 -- state change*/
	int rlen = 0;

	if(c->keep_alive && ((c->total_count && c->data_count >= c->total_count)
		|| c->local_fd < 0)){
		#if PLUGIN_ZLIB
		if(c->z_st){
			zlib_destroy(c->z_st);
			c->z_st = NULL;
		}
		#endif
		c->timeout = cur_time + HTTP_REQUEST_TIMEOUT;
		c->state = HTTPSTATE_WAIT_REQUEST;
		http_log("%u local-alive %s\n", ntohs(c->from_addr.sin_port), c->url);
		return 1;
	}else if(c->local_fd < 0){
		return -1;
	}
	#if PLUGIN_ZLIB
	if(c->z_st){
		rlen = zlib_read_compress(read, c->local_fd, c->z_st, c->buffer, c->buffer_size);
	}else
	#endif
		rlen = read(c->local_fd, c->buffer, c->buffer_size);

	if(-11 == rlen){
		return 2; /*not really change state, jump to next big loop.*/
	}else if(rlen <= 0){
		close(c->local_fd);
		c->local_fd = -2;
		if(rlen < 0)return -1;
	}

	c->buffer_ptr = c->buffer;
	c->buffer_end = c->buffer_ptr + rlen;
	return 0;
}


static HLS s_hls[S+1]; 
static char s_hls_name[32];

static int hls_close(void)
{
	int i;
	
	memset(s_hls_name, 0, sizeof(s_hls_name));
	for(i = 0; i <= S; ++i){
		s_hls[i].csize = 0;
		s_hls[i].flag = 0;
	}
	
	return 0;		
}

static int hls_parse_request(HTTPContext *c, char *name, int is_first)
{
	int idx = -1, ret = 0;
	char *ext = NULL;
	HLS *s = NULL;

    ext = strrchr(name, '.');
	if(!ext){
		http_log("bad name %s\n", name);
		return -1;
	}	
	if(ext - name > sizeof(s_hls_name)-1){
		http_log("name %s too long\n", name);
		return -1;
	}
				
	if(!strcmp(ext, ".m3u8")){
		idx = S;
		
	}else if(!strcmp(ext, ".ts")){
		idx = ext[-1] - '0';//todo: S > 10 without get basename.
		if(!(0 <= idx && idx < S)){
			http_log("too large index %d\n", idx);
			return -1;
		}
	}
	
	if(-1 == idx){
		http_log("unkown name %s\n", name);
		return -1;
	}

	c->hls_idx = idx;
	s = &s_hls[idx];
	if(c->post){/*writer*/
		//todo: close http conn with same name
		
		//http_log("hls post c %p name %s:%d data %p size %d:%d\n", c, name, idx, s->data, s->msize, s->csize);

		if(!s->data){
			s->data = av_malloc(SEG_INC_SIZE);
			s->msize = SEG_INC_SIZE;
		}else{
			/*intended not to free*/
		}

		//todo: if someone is reading
		s->flag = 1;
		s->csize = 0;
		
		c->hls_wpos = 0; 
		c->http_error = 0;
		c->state = HTTPSTATE_RECEIVE_DATA;
	
		ret = 0;
	}else{/*reader*/
		if(is_first && (S == idx) && strncmp(s_hls_name, name, ext-name) ){
			hls_close();
			strncpy(s_hls_name, name, ext - name);
			s_hls_name[ext - name] = 0; 
			
			ret = 1; /*request switch*/ 	
		}

		c->hls_rpos = 0;
		c->http_error = 0;
		c->state = HTTPSTATE_SEND_HEADER;
	}
	
	return ret; 
}

static int hls_write(HTTPContext *c, uint8_t *data, int size)
{
	int inc = 0;
	uint8_t *ptr = NULL;
	HLS *s = &s_hls[c->hls_idx];

	if(!s->data){
		http_log("internal error i %d\n", c->hls_idx);
		return -1;
	}
	
	if(s->csize + size > s->msize){
		inc = 	(s->csize + size - s->msize + SEG_INC_SIZE)/SEG_INC_SIZE;
		ptr = av_realloc(s->data, inc*SEG_INC_SIZE + s->msize);
		if(ptr){
			s->data = ptr;
			s->msize += inc*SEG_INC_SIZE;
		}else{
			http_log("not enough mem  %d\n", inc);
			return -2;
		}
	}
	
	memcpy(s->data + s->csize, data, size);
	s->csize += size;
	c->hls_wpos += size;

	return 0;
}

static int hls_read(HTTPContext *c)
{
	HLS *s = &s_hls[c->hls_idx];
	int left = s->csize - c->hls_rpos;

	if( !s->data || left <= 0){
		c->state = s->flag == 2 ? HTTPSTATE_SEND_DATA_TRAILER : HTTPSTATE_WAIT_FEED;
		c->buffer_ptr = c->buffer_end = c->buffer;
		return 1;
	}

	c->buffer_ptr = s->data + c->hls_rpos;
	c->buffer_end = c->buffer_ptr + left;
	c->hls_rpos += left; 
	return 0;
}

static int hls_reset(HTTPContext *c)
{
	if(!c){
		return -1;
	}

	c->hls_wpos = 0;
	c->hls_rpos = 0;
	c->hls_idx = -1;

	return 0;
}

static int sff_close(void)
{
	HTTPContext *c = NULL, *c_next = NULL;
	
	for(c = first_http_ctx; c != NULL; c = c_next){
		c_next = c->next;
		if(c->post && av_match_ext(c->url, "flv")){
			printf("sff close %s\n", c->url);
			close_connection(c);
		}
	}
	return 0;
}

static void sff_free(SFF **s)
{
	SFF *sff = NULL;
	if(!s || !*s){
		return;
	}
	
	sff = *s;
	sff->wflag = 1;
	if(sff->data){
		av_free(sff->data);
	}
	av_free(sff);
	*s = NULL;
}

static void sff_reset(HTTPContext *c)
{
	int i;
	if(!c){
		printf("bad arg in reset\n");
		return;
	}
	if(!c->post){
		if(c->feed_ctx)c->feed_ctx->sff_ref_cnt--;
		return;
	}

	for(i = 0; i < N+1; ++i){
		sff_free(&c->sff_pkts[i]);
	}
}

static void sff_dump(SFF *sff)
{
	static int seq = 0;

	if(!sff || !sff->data || sff->size < 28){
		return;
	}
	
	#if 0
	static FILE *fp = NULL;
	static int cnt = 0;
	if(!fp){
		fp = fopen("/var/dump.flv", "wb");
	}
	
	if(!fp){
		printf("cant open dump\n");
		return;
	}
	if(1 == sff->type){
		fwrite(sff->data, sff->size, 1, fp);
		cnt += sff->size;
	}
	if(2 == sff->type){
		fwrite(sff->data + 4, sff->size - 4, 1, fp);
		cnt += sff->size - 4;
	}
	#endif
	seq++;
	if(sff->size < 30) printf("sff %d:%d %d ", seq, sff->type, sff->size);
}

static int sff_write(HTTPContext *c, SFF *sff)
{

	if(1 == sff->type){
		sff_free(&c->sff_pkts[N]);
		c->sff_pkts[N] = sff; //todo:exist?	
	}else if(2 == sff->type){
		sff_free(&c->sff_pkts[c->sff_w]);
		c->sff_pkts[c->sff_w] = sff;
		c->sff_w = (c->sff_w + 1)%N;
	}
	sff_dump(sff);
	sff->wflag = 2; /*now reader can read*/
	return 0;
}

static SFF* sff_read(HTTPContext *c, int type)
{
	HTTPContext *f = c->feed_ctx;
	SFF *sff = NULL;
	int idx = 1 == type ? N : c->sff_r;

	if(!f){
		return NULL;
	}
	
	sff = f->sff_pkts[idx];
	if(!(sff && sff->wflag == 2)){
		return NULL;
	}
	
	if(1 == type){
		return f->sff_pkts[N];
	}

	if(FFABS(c->sff_r - f->sff_w) <= 0){
		return NULL;
	}
	
	sff = f->sff_pkts[c->sff_r];
	c->sff_r = (c->sff_r + 1)%N;
	return sff;
}

static int sff_parse(SFF *sff, packet_t *pkt)
{
	uint8_t *ptr = NULL;
	if(!pkt || !sff || 2 != sff->type){
		printf("bad arg\n");
		return -1;
	}

	if(!sff->data || sff->size < 4){
		printf("invalid data\n");
		return -2;
	}
	ptr = sff->data;
	pkt->stream_index = AV_RB32(ptr); ptr += 4;
	pkt->data = ptr;
	pkt->size = sff->size - 4;

	return 0;	
}


static int wake_others(HTTPContext *c, int to)
{
	HTTPContext *c2 = NULL;

	for(c2 = first_http_ctx; c2 != NULL; c2 = c2->next) {
		if (c2->state == HTTPSTATE_WAIT_FEED){
			if(!c2->feed_ctx && !c2->post && !strcmp(c2->url, c->url)){
				c2->feed_ctx = c;
				c->sff_ref_cnt++;
			}
			if(c2->feed_ctx == c)
				c2->state = to;
			if(c->hls_idx >= 0 && c->hls_idx == c2->hls_idx)
				c2->state = to;
		}
	}
	return 0;
}

static int recv_chunked(HTTPContext *c)
{/*tcp or ssl recv --> 
   removed chunk header and tail -->
   cooked data is in c->buffer+0..len-1.
   return len = 0 -- eof, >0 -- ok, <0 -- error.
   */
	int len = -1;
	c->buffer_ptr = c->buffer;
	while(!c->chunk_size && c->buffer_ptr < c->buffer+32){
		len = recv(c->fd, c->buffer_ptr, 1, 0);
		if(len <= 0){return -1;}
		else if(c->buffer_ptr - c->buffer >= 2 &&
				!memcmp(c->buffer_ptr-1, "\r\n", 2)){
			c->chunk_size = (int)strtol(c->buffer, NULL, 16);
			break;
		}
		else c->buffer_ptr++;	
	}

	if(c->buffer_ptr >= c->buffer+32){
		http_log("missing chunk header\n");
		return -1;
	}

	if(c->chunk_size == 0){
		return 0; /*eof*/
	}
	
	len = recv(c->fd, c->buffer, FFMIN(c->chunk_size, c->buffer_size), 0);
	if(len > 0){
		c->chunk_size -= len;
	}

	return len;
}

static int http_receive_data(HTTPContext *c)
{
	int len = 0, len0 = 0, trans = 0, ret = 0;
	uint8_t *ptr = NULL, buf[8] = "";
	uint32_t type = 0, size = 0;
	SFF *sff = c->sff;
	HLS *s = NULL;
	
	if(c->hls_idx >= 0){
		s = &s_hls[c->hls_idx];
	}

	if(s){
		len = c->tr_encoding == 1 ? recv_chunked(c) : recv(c->fd, c->buffer, c->buffer_size, 0);
		if(len > 0){
			hls_write(c, c->buffer, len);	
		}
		goto check;
	}
	else if(c->local_fd >= 0){
		//drain read buffer
		for (;;) {
			len = c->tr_encoding == 1 ? recv_chunked(c) : recv(c->fd, c->buffer, c->buffer_size, 0);
			if (len > 0) {
				c->data_count += write(c->local_fd, c->buffer, len);
			}
			else {
				goto check;
			}
		}
	}

	/*get remains*/	
	if(sff && sff->size && sff->wpos < sff->size){
		len0 = sff->size - sff->wpos;
		len = recv(c->fd, sff->data + sff->wpos, len0, 0);
		if(len > 0)sff->wpos += len;
		if(len == len0){
			sff_write(c, sff);
			c->sff = NULL;	
		}
		goto check;
	}
	if(c->sff){
		printf("sff internal error\n");
		return -1;	
	}

	ptr = buf;	
	/*sync to current pkt, drop any data between last pkt and current pkt*/
	while( (len = recv(c->fd, ptr, 1, 0)) > 0){
		if(*ptr != 'S')continue; 
		recv(c->fd, ptr, 1, 0);
		if(*ptr != 'F')continue;
		recv(c->fd, ptr, 1, 0);
		if(*ptr != 'F')continue;		
		recv(c->fd, ptr, 1, 0);
		if(!('0' <= *ptr && *ptr <= '9'))continue;
		type = *ptr - '0';
		
		recv(c->fd, ptr, 4, 0);
		size = AV_RB32(ptr); 
		if(!size || size > 1E6){
			http_log("sff size error %u\n", size);
			continue;
		}
		
		c->sff = sff = av_mallocz(sizeof(* c->sff));
		sff->type = type;
		sff->size = size;
		sff->data = av_malloc(sff->size);
		len = recv(c->fd, sff->data, sff->size, 0);
		if(len > 0)sff->wpos = len;	
		if(sff->size && sff->wpos == sff->size){
			sff_write(c, sff);
			c->sff = NULL;	
		}
		break;
	}

check:
	ret = ff_neterrno();
	//printf("tr_encoding %dlen %d ret 0x%x due %lld recv %lld\n", c->tr_encoding, len, ret, c->content_length, c->data_count);

	
	if((len == 0) 
		|| (len < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR(EINTR))
		|| (0 < c->content_length && c->data_count >= c->content_length)
		){
		//printf("local_fd %d hls %p\n", c->local_fd, s);
		
		//http_log("conn end len %d ret %s re_cnt %d\n", len, av_err2str(ret), c->sff_ref_cnt);
		if(s){
			s->flag = 2;
			//printf("hls get seg %d:%d:%d data %02x %02x %02x %02x\n", c->hls_idx, s->msize, s->csize, s->data[0], s->data[1], s->data[2], s->data[3]);
		}
		#if PLUGIN_DIR
		else if(c->local_fd >= 0){
			close(c->local_fd);
			c->local_fd = -1;
			c->http_error = 200;
			//printf("prepare_response\n");
			
			return prepare_response(c, c->http_error, "OK");
		}
		#endif

		wake_others(c, HTTPSTATE_SEND_DATA_TRAILER); 
		if(s || (c->sff_ref_cnt <= 0))return -1;
	}

	if(s && s->csize > 0){/*get partial data, also send notify.*/
		trans = HTTPSTATE_SEND_DATA; 
	}
	/*get full pkt, add it to cbuf*/
	else if(sff && sff->size && sff->wpos == sff->size){
		if(1 == sff->type){
			trans = HTTPSTATE_SEND_DATA_HEADER;
		}
		else if(2 == sff->type){
			trans = HTTPSTATE_SEND_DATA; 
		}
	}
	
	/* wake up any waiting connections */
	if(trans)wake_others(c, trans);
	return 0;
}

static void htmlstrip(char *s) {
    while (s && *s) {
        s += strspn(s, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ,. ");
        if (*s)
            *s++ = '?';
    }
}

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

__attribute__ ((format (printf, 1, 2)))
static void http_log(const char *fmt, ...)
{
    va_list vargs;
    va_start(vargs, fmt);
    http_vlog(fmt, vargs);
    va_end(vargs);
}

static void http_av_log(void *ptr, int level, const char *fmt, va_list vargs)
{
    static int print_prefix = 1;
	#if FFMPEG_SRC
    AVClass *avc = ptr ? *(AVClass**)ptr : NULL;
    if (level > av_log_get_level())
        return;
    if (print_prefix && avc)
        http_log("[%s @ %p]", avc->item_name(ptr), ptr);
    print_prefix = strstr(fmt, "\n") != NULL;
	#endif
    http_vlog(fmt, vargs);
}

static int get_socket_error(int fd)
{
	int error = 0;
	socklen_t errlen = sizeof(error);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen);
	return error;
}

static void log_connection(HTTPContext *c)
{
	//if(av_match_ext(c->url, "m3u8"))
    	http_log("%s:%u %d '%s' %" PRId64 " %d %d\n",
             inet_ntoa(c->from_addr.sin_addr), 
			 ntohs(c->from_addr.sin_port), 
			 c->post, (c->url ? c->url : "null"),
			 c->data_count, c->http_error, nb_connections);
}

static int socket_open_listen(struct sockaddr_in *my_addr)
{
    int server_fd, tmp;

    server_fd = socket(AF_INET,SOCK_STREAM,0);
    if (server_fd < 0) {
        perror ("socket");
        return -1;
    }

    tmp = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof(tmp)))
        http_log("setsockopt SO_REUSEADDR failed\n");

    my_addr->sin_family = AF_INET;
    if (bind (server_fd, (struct sockaddr *) my_addr, sizeof (*my_addr)) < 0) {
        char bindmsg[32];
        snprintf(bindmsg, sizeof(bindmsg), "bind(port %d)", ntohs(my_addr->sin_port));
        perror (bindmsg);
        close_socket(server_fd);
        return -1;
    }

    if (listen (server_fd, 50) < 0) {
        perror ("listen");
        close_socket(server_fd);
        return -1;
    }

    if (ff_socket_nonblock(server_fd, 1) < 0)
        http_log("ff socket set nonblock failed\n");

    return server_fd;
}

static int http_server(void)
{
	int ssl_fd = 0;
    int server_fd = 0;
	int ctrl_fd = 0, ctrl_fd2 = 0;
    int ret, delay;
    struct pollfd *poll_table, *poll_entry;
    HTTPContext *c, *c_next;
    if(!(poll_table = av_mallocz_array(nb_max_http_connections + 1, sizeof(*poll_table)))) {
        http_log("Impossible to allocate a poll table handling %d connections.\n", nb_max_http_connections);
        return -1;
    }

	#if defined(PLUGIN_DVB)
	ctrl_fd = ff_ctl_open(1234);
    if (ctrl_fd < 0) {
        av_free(poll_table);
        return -1;
    }
	#endif
	
    if (my_http_addr.sin_port) {
        server_fd = socket_open_listen(&my_http_addr);
        if (server_fd < 0) {
            av_free(poll_table);
            return -1;
        }
    }
	
	#if PLUGIN_SSL
	if (my_https_addr.sin_port) {
        ssl_fd = socket_open_listen(&my_https_addr);
        if (ssl_fd < 0) {
            av_free(poll_table);
            return -1;
        }
    }
	#endif

    if (!server_fd && !ssl_fd) {
        http_log("HTTP disabled.\n");
        av_free(poll_table);
        return -1;
    }
	
	#if defined(PLUGIN_SSDP)
	ssdp_fd = mcast_open(ssdp_ip, ssdp_port);
	if(ssdp_fd <= 0){
		http_log("ssdp disabled\n");
	}
	ssdp_notify(ssdp_fd, ssdp_ip, ssdp_port, "ssdp:alive");
	#endif
	
    http_log("FFserver started.\n");

    for(;;) {
        poll_entry = poll_table;
		
		#if defined(PLUGIN_DVB)
		if(ctrl_fd){
			poll_entry->fd = ctrl_fd;
            poll_entry->events = POLLIN;
            poll_entry++;
		}
		if(ctrl_fd2){
			poll_entry->fd = ctrl_fd2;
			poll_entry->events = POLLIN;
			if(ctl_msg_pending() > 0){
				poll_entry->events |= POLLOUT;
			}
            poll_entry++;
		}
		#endif
		
        if (server_fd) {
            poll_entry->fd = server_fd;
            poll_entry->events = POLLIN;
            poll_entry++;
        }
		
		#if PLUGIN_SSL
	    if (ssl_fd) {
            poll_entry->fd = ssl_fd;
            poll_entry->events = POLLIN;
            poll_entry++;
        }
		#endif
		
		#if defined(PLUGIN_SSDP)
		if(ssdp_fd){
			poll_entry->fd = ssdp_fd;
            poll_entry->events = POLLIN;
            poll_entry++;
		}
		#endif

        /* wait for events on each HTTP handle */
        c = first_http_ctx;
        delay = 1500;
        while (c != NULL) {
            int fd;
            fd = c->fd;
            switch(c->state) {
            case HTTPSTATE_SEND_HEADER:
                c->poll_entry = poll_entry;
                poll_entry->fd = fd;
                poll_entry->events = POLLOUT;
                poll_entry++;
                break;
            case HTTPSTATE_SEND_DATA_HEADER:
            case HTTPSTATE_SEND_DATA:
            case HTTPSTATE_SEND_DATA_TRAILER:
                /*for TCP, we output as much as we can*/
                c->poll_entry = poll_entry;
                poll_entry->fd = fd;
                poll_entry->events = POLLOUT;
                poll_entry++;
                break;
            case HTTPSTATE_WAIT_REQUEST:
            case HTTPSTATE_RECEIVE_DATA:
            case HTTPSTATE_WAIT_FEED:
                /* need to catch errors */
                c->poll_entry = poll_entry;
                poll_entry->fd = fd;
                poll_entry->events = POLLIN;/* Maybe this will work */
                poll_entry++;
                break;
            default:
                c->poll_entry = NULL;
                break;
            }
            c = c->next;
        }

        /* wait for an event on one connection. We poll at least every second to handle timeouts */
        do {
            ret = poll(poll_table, poll_entry - poll_table, delay);
            if (ret < 0 && ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR)) {
                av_free(poll_table);
                return -1;
            }
        } while (ret < 0);

        cur_time = av_gettime() / 1000;

        /* now handle the events */
        for(c = first_http_ctx; c != NULL; c = c_next) {
            c_next = c->next;
            if (handle_connection(c) < 0) {
                log_connection(c);
                close_connection(c);
            }
        }

        poll_entry = poll_table;

		#if defined(PLUGIN_DVB)
		if(ctrl_fd){
			if(poll_entry->revents & POLLIN){
				ctrl_fd2 = ctl_msg_open(ctrl_fd);
			}
			poll_entry++;
		}
		if(ctrl_fd2 && poll_entry->fd == ctrl_fd2){
			if(poll_entry->revents & POLLIN){
				ctl_msg_recv();
				ff_ctl_recv(ctl_msg_cb);
			}else if(poll_entry->revents & POLLOUT){
				ctl_msg_send();
			}
			poll_entry++;
		}
		#endif
		if(server_fd && poll_entry->fd != server_fd){
			printf("bad  entry\n");
			continue;
		}
		
        if (server_fd) {
            if (poll_entry->revents & POLLIN)
                new_connection(server_fd, 0);
            poll_entry++;
        }

		#if PLUGIN_SSL
		if (ssl_fd) {
            if (poll_entry->revents & POLLIN)
                new_connection(ssl_fd, 1);
            poll_entry++;
        }
		#endif

		#if defined(PLUGIN_SSDP)
		if (ssdp_fd) {
            if (poll_entry->revents & POLLIN)
                ssdp_response(ssdp_fd);
            poll_entry++;
        }
		#endif
	
    }
}

static void http_send_too_busy_reply(HTTPContext *c)
{
    char buffer[400];
    int len = snprintf(buffer, sizeof(buffer),
                       "HTTP/1.1 503 Server too busy\r\n"
                       "Content-type: text/html\r\n"
                       "\r\n"
                       "<html><head><title>Too busy</title></head><body>\r\n"
                       "<p>The server is too busy to serve your request at this time.</p>\r\n"
                       "<p>The number of current connections is %u, and this exceeds the limit of %u.</p>\r\n"
                       "</body></html>\r\n",
                       nb_connections, nb_max_connections);
    av_assert0(len < sizeof(buffer));
    if (send(c->fd, buffer, len, 0) < len)
        http_log("Could not send too-busy reply, send() failed\n");
}


static void new_connection(int server_fd, int flag)
{
    struct sockaddr_in from_addr;
    socklen_t len;
    int fd;
    HTTPContext *c = NULL;
	int val;
	val = 0;

    len = sizeof(from_addr);
	memset(&from_addr, 0, len);
    fd = accept(server_fd, (struct sockaddr *)&from_addr,
                &len);
    if (fd < 0) {
        http_log("error during accept %s\n", strerror(errno));
        return;
    }
	#if PLUGIN_SSL
	void *ssl = NULL;
	if(1 == flag){
		ssl = ssl_open(fd);
		if(!ssl){
			http_log("ssl open err %s\n", strerror(errno));
			goto fail;
		}
	}
	#endif
    if (ff_socket_nonblock(fd, 1) < 0)
        http_log("ff socket set nonblock failed\n");

	#if 0
	int tcp_nodelay = 1;
	if (setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay, sizeof (tcp_nodelay))) {
        http_log("setsockopt(TCP_NODELAY) fail");
    }
	#endif

	int recv_buffer_size = 500*1024;
	if (setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &recv_buffer_size, sizeof(recv_buffer_size))) {
    	http_log("setsockopt(SO_RCVBUF) fail");
    }

	int send_buffer_size = 500*1024;
	if (setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size))) {
    	http_log("setsockopt(SO_RCVBUF) fail");
    }
	
	#if	0 /*prevent myself close_wait*/
	val = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
	val = 5;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
	val = 3;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val));
	val = 2;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val));
	#endif

    /* add a new connection */
    c = av_mallocz(sizeof(HTTPContext));
    if (!c)
        goto fail;
    c->fd = fd;
    c->poll_entry = NULL;
    c->from_addr = from_addr;
    c->buffer_size = IOBUFFER_INIT_SIZE;
    c->buffer = av_malloc(c->buffer_size);
    if (!c->buffer)
        goto fail;

	if (nb_connections >= nb_max_connections) {
        http_send_too_busy_reply(c);
        goto fail;
    }

    c->next = first_http_ctx;
    first_http_ctx = c;
    nb_connections++;

	c->buffer_ptr = c->buffer;
    c->buffer_end = c->buffer + c->buffer_size - 1; /* leave room for '\0' */
	c->timeout = cur_time + HTTP_REQUEST_TIMEOUT;
    c->state = HTTPSTATE_WAIT_REQUEST;
	c->hls_idx = -1;
	c->local_fd = -1;
	#if PLUGIN_SSL
	c->ssl = ssl;
	#endif
    return;
 fail:
    if (c) {
        av_free(c->buffer);
        av_free(c);
    }
    close_socket(fd);
}

static void close_connection(HTTPContext *c)
{
    HTTPContext **cp, *c2;

    /* remove connection from list */
    cp = &first_http_ctx;
    while ((*cp) != NULL) {
        c2 = *cp;
        if (c2 == c)
            *cp = c->next;
        else
            cp = &c2->next;
    }

    /* remove connection associated resources */
	#if PLUGIN_ZLIB
	if(c->z_st){
		zlib_destroy(c->z_st);
	}
	#endif
	#if PLUGIN_SSL
	if(c->ssl){
		ssl_close(c->ssl);
		c->ssl = NULL;
	}
	#endif
    if (c->fd >= 0)
        close_socket(c->fd);
	
	if(c->local_fd >= 0){
		close(c->local_fd);
	}
	
	av_freep(&c->url);
    av_freep(&c->pb_buffer);
    av_free(c->buffer);
	sff_reset(c);
	hls_reset(c);
    av_free(c);
    nb_connections--;
}

static int handle_connection(HTTPContext *c)
{
    int len, ret;

    switch(c->state) {
    case HTTPSTATE_WAIT_REQUEST:
        /* timeout ? */
        if ((c->timeout - cur_time) < 0)
            return -1;
        if (c->poll_entry->revents & (POLLERR | POLLHUP))
            return -1;

        /* no need to read if no events */
        if (!(c->poll_entry->revents & POLLIN))
            return 0;
        /* read the data */
    read_loop:
        len = recv(c->fd, c->buffer_ptr, 1, 0);
        if (len < 0) {
            if (ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR))
                return -1;
        } else if (len == 0) {
            if(!c->keep_alive)return -1;
        } else {
            /* search for end of request. */
            uint8_t *ptr;
            c->buffer_ptr += len;
            ptr = c->buffer_ptr;
            if ((ptr >= c->buffer + 2 && !memcmp(ptr-2, "\n\n", 2)) ||
                (ptr >= c->buffer + 4 && !memcmp(ptr-4, "\r\n\r\n", 4))) {
                /* request found : parse it and reply */
                ret = http_parse_request(c);
                if (ret < 0)
                    return -1;
            } else if (ptr >= c->buffer_end) {
                /* request too long: cannot do anything */
                return -1;
            } else goto read_loop;
        }
        break;

    case HTTPSTATE_SEND_HEADER:
        if (c->poll_entry->revents & (POLLERR | POLLHUP))
            return -1;

        /* no need to write if no events */
        if (!(c->poll_entry->revents & POLLOUT))
            return 0;
        len = send(c->fd, c->buffer_ptr, c->buffer_end - c->buffer_ptr, 0);
        if (len < 0) {
            if (ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR)) {
                goto close_conn;
            }
        } else {
        	http_log("send len %d\n", len);
            c->buffer_ptr += len;
            c->data_count += len;
            if (c->buffer_ptr >= c->buffer_end) {
                av_freep(&c->pb_buffer);
				
				if(100 == c->http_error){
					c->state = HTTPSTATE_RECEIVE_DATA;
					c->buffer_ptr = c->buffer_end = c->buffer;
				} else if(c->keep_alive && c->total_count && c->data_count >= c->total_count){
					memset(c->buffer, 0, c->buffer_size);
					c->buffer_ptr = c->buffer;
				    c->buffer_end = c->buffer + c->buffer_size - 1; 
					c->timeout = cur_time + HTTP_REQUEST_TIMEOUT;
				    c->state = HTTPSTATE_WAIT_REQUEST;
					c->hls_idx = -1;
					http_log("%u alive %s\n", ntohs(c->from_addr.sin_port), c->url);
				} else if (c->http_error){/* if error, exit */
                    return -1;
				} else { /* all the buffer was sent : synchronize to the incoming*/
					c->state = HTTPSTATE_SEND_DATA_HEADER;
					c->buffer_ptr = c->buffer_end = c->buffer;
				}
            }
        }
        break;

    case HTTPSTATE_SEND_DATA:
    case HTTPSTATE_SEND_DATA_HEADER:
    case HTTPSTATE_SEND_DATA_TRAILER:
        if (c->poll_entry->revents & (POLLERR | POLLHUP))
			return -1;
        /* no need to read if no events */
        if (!(c->poll_entry->revents & POLLOUT))
            return 0;
        if (http_send_data(c) < 0)
			return -1;
        /* close connection if trailer sent */
        if (c->state == HTTPSTATE_SEND_DATA_TRAILER)
            return -1;
        break;
    case HTTPSTATE_RECEIVE_DATA:
        /* no need to read if no events */
        if (c->poll_entry->revents & (POLLERR | POLLHUP)){
			HLS *s = NULL;
			if(c->hls_idx >= 0){
				s = &s_hls[c->hls_idx];
				s->flag = 2;
			}
				
			wake_others(c, HTTPSTATE_SEND_DATA_TRAILER); 
            return -1;
        }
        if (!(c->poll_entry->revents & POLLIN))
            return 0;
        if (http_receive_data(c) < 0)
            return -1;
        break;
    case HTTPSTATE_WAIT_FEED:
        /* no need to read if no events */
        if (c->poll_entry->revents & (POLLIN | POLLERR | POLLHUP)) /*19*/
            {printf("line %d: %x:%d\n", __LINE__, c->poll_entry->revents, get_socket_error(c->fd)); return -1;} 
        /* nothing to do, we'll be waken up by incoming feed packets */
        break;

    default:
        return -1;
    }
    return 0;

close_conn:
    av_freep(&c->pb_buffer);
    return -1;
}

static HTTPContext* find_feed(char *name)
{
	HTTPContext *ctx;

	for(ctx = first_http_ctx; ctx; ctx = ctx->next) {
		if(ctx->post && !strcmp(ctx->url, name)){
			return ctx;
		}
	}	
	return NULL;
}

/* XXX: factorize in utils.c ? */
/* XXX: take care with different space meaning */
static void skip_spaces(const char **pp)
{
    const char *p;
    p = *pp;
    while (*p == ' ' || *p == '\t')
        p++;
    *pp = p;
}

static int get_word(char *buf, int buf_size, const char **pp)
{
    const char *p;
    char *q;

    p = *pp;
    skip_spaces(&p);
    q = buf;
    while (!av_isspace(*p) && *p != '\0') {
		if(!buf){
			q++;
		}
		else if ((q - buf) < buf_size - 1)
            *q++ = *p;
        p++;
    }

	if(buf){
		if (buf_size > 0)
			*q = '\0';
		*pp = p;
	}
	return q - buf;
}

static int get_line(char *buf, int buf_size, const char **pp)
{
	const char *p = *pp;
	char *q = buf;

	while(*p && !(*p == '\r' || *p == '\n')){
		if(q - buf < buf_size - 1)*q++ = *p++; 
	}
	if(!*p){
		return 0;
	}
	/*drop "rn", "r", or "n"*/
	if(*p == '\r'){
		p++;
	}
	if(*p == '\n'){
		p++;
	}


	*q = 0;
	*pp = p;
	return q - buf;

}
static int handle_line(HTTPContext *c, char *line, int line_size, RequestData *rd)
{
	char *p1, tmp[32], info[64];
	const char *p = line;
	int len0, len;
	unsigned char *uri = NULL;
	
	get_word(tmp, sizeof(tmp), &p);
	if(!strcmp(tmp, "GET") || !strcmp(tmp, "POST") || !strcmp(tmp, "PUT") || !strcmp(tmp, "DELETE") || !strcmp(tmp, "HEAD")){
		if (tmp[0]== 'G')
			c->post = 0;
		else if (tmp[0] == 'P'){
			c->post = tmp[1] == 'O' ? 1 : 2;
		}else if(tmp[0] == 'D'){
			c->post = 3;
		}else if(tmp[0] == 'H'){
			c->post = 0;
			c->only_header = 1;
		}else
			return -1;
		
		len0 = get_word(NULL, 0, &p)+16;
		if(len0 > 8192){
			http_log("too long: '%s'\n", p);
			return -1;	
		}
		uri = av_malloc(len0);
		if(!uri){
			http_log("malloc fail for uri len %d\n", len0);
			return -1;
		}
		get_word(uri, len0, &p);
		http_log("%s '%s' len %d\n", tmp, uri, len0);
		url_decode(uri);
		url_local(uri, len0);

		if(uri[0] == '/'){
			len = strlen(uri)-1;
			memmove(uri, uri+1, len); 
			uri[len] = 0;
			if(len > 0 && uri[len-1] == '/'){
				uri[len-1] = 0;
			}
		}
		c->url = uri;

		if(!c->url[0]){
			av_strlcpy(c->url, "index.html", len0);
		}

		get_word(tmp, sizeof(tmp), &p);
		if(!strcmp(tmp, "HTTP/1.0"))
			c->http_version = 0;
		else if(!strcmp(tmp, "HTTP/1.1"))
			c->http_version = 1;
		else 
			return -1;

		p1 = strchr(c->url, '?');
		if (p1) {
			av_strlcpy(info, p1, sizeof(info));
			*p1 = '\0';
		} else
			info[0] = '\0';
	}
	else if(!strcmp(tmp, "Host:")){
		get_word(rd->domain, sizeof(rd->domain), &p);	
	}
	else if(!strcmp(tmp, "Cookie:")){
		get_word(rd->cookie, sizeof(rd->cookie), &p);
	}
	else if(!strcmp(tmp, "Connection:")){
		get_word(info, sizeof(info), &p);
		if(!av_strcasecmp(info, "keep-alive")){
			c->keep_alive = 1;
		}
	}
	else if(!av_strcasecmp(tmp, "Content-Length:")){
		get_word(info, sizeof(info), &p);
		c->content_length = strtoll(info, NULL, 10);
	}
	else if(!av_strcasecmp(tmp, "Range:")){
		get_word(rd->range, sizeof(rd->range), &p);
	}
	else if(!av_strcasecmp(tmp, "Expect:")){
		get_word(rd->expect, sizeof(rd->expect), &p);
	}
	#if PLUGIN_ZLIB
	else if(!av_strcasecmp(tmp, "Accept-Encoding:")){
		while((len = get_word(info, sizeof(info), &p)) > 0){
			if(',' == info[len-1]){
				info[len-1] = 0;
			}

			if(!c->z_st && !strcmp(info, "gzip")){
				c->z_st = zlib_init();
				break;
			}
		}
	}
	#endif
	#if PLUGIN_DIR
	else if(!av_strcasecmp(tmp, "If-None-Match:")){
		get_word(c->inm, sizeof(c->inm), &p);
	}
	else if(!av_strcasecmp(tmp, "If-Modified-Since:")){
		char *ptr = c->ims;
		while((len = get_word(info, sizeof(info), &p)) > 0){
			ptr += sprintf(ptr, "%s ", info);
		}
		if(ptr[-1] == ' '){
			ptr[-1] = 0;
		}
	}
	#endif
	else if(!av_strcasecmp(tmp, "Transfer-Encoding:")){
		get_word(info, sizeof(info), &p);
		if(!av_strcasecmp(info, "chunked")){
			c->tr_encoding = 1;
		}
	}
	
	else if(!av_strcasecmp(tmp, "X-TOKEN:")){
		get_word(info, sizeof(info), &p);
		strncpy(c->xToken, info, sizeof(c->xToken)-1);
		c->xToken[sizeof(c->xToken)-1] = 0;
	}

	return 0;
}

static int read_request_content(HTTPContext *c, uint8_t *buf, int bufsize)
{
	int ret, rsize = FFMIN(bufsize-2, c->content_length);
	ret =  recv(c->fd, buf, rsize, 0);
	printf("read content size %d-->%d\n", rsize, ret);
	if(ret <= 0){/*only try one time.*/
		ret = 0;
	}

	return ret;
}

/* parse HTTP request and prepare header */
static int http_parse_request(HTTPContext *c)
{
    char *q, msg[1024];
    const char *mime_type, *p;
	HTTPContext *ctx;
	int ret = 0, is_first = 0;
	const char *first_tag = "First-Request=0";
	RequestData rd = {{0}};

    p = c->buffer;
	while(get_line(msg, sizeof(msg), &p) > 0){
		ret = handle_line(c, msg, sizeof(msg), &rd);
		if(ret < 0)return ret;
	}
	if(!c->url){
		return -1;
	}
	is_first = !av_stristr(rd.cookie, first_tag);
	
	#if PLUGIN_DIR
	if(0 == c->post && dir_list_local(c) > 0){
		c->state = HTTPSTATE_SEND_HEADER;
		return 0;
	}
	else if(3 == c->post){
		if(strncmp(c->url, "upload/", 7) || checkToken(c)) {
			c->http_error = 403;
		}else{
			c->http_error = dir_delete_file(c);
		}
		return prepare_response(c, c->http_error, "");
	}
	else if((2 == c->post) && !strncmp(c->url, "upload/", 7)){
		char *reason = "";
		c->http_error = 0;
		if(!av_strcasecmp(rd.expect, "100-continue")){
			c->http_error = 100;
			reason = "continue";
		}

		if(c->content_length > DIR_UPLOAD_MAX_SIZE){
			c->http_error = 417;
			reason = "upload too large file";
		}
		else if (checkToken(c)) {
			c->http_error = 417;
			reason = "bad token";
		}
		else{
			snprintf(msg, sizeof(msg), "./upload/%s", c->url+7);	

			#if 0 //disable create dir.
			char *basePath = strrchr(msg, '/');
			if (!basePath) {
				http_log("not full path '%s'\n", basePath);
				return -1;
			}

			if (!preparePathOK(basePath)) {
				http_log("cant prepare base path '%s'\n", basePath);
				return -1;
			}
			#endif
			
			c->local_fd = open(msg, O_CREAT|O_WRONLY|O_TRUNC, 0644);
			if(c->local_fd < 0){
				http_log("write file '%s' err %d\n", msg, ff_neterrno()); 
				snprintf(msg, sizeof(msg), "bad file path error %d", ff_neterrno());
				c->http_error = 417;
				reason = msg;
			}
		}

		if(c->http_error){
			prepare_response(c, c->http_error, reason);
		}else{
			c->state = HTTPSTATE_RECEIVE_DATA;
		}
		return 0;
	}
	else if ((1 == c->post) && !strncmp(c->url, "sign_apk", 8)) {
		c->content_length = read_request_content(c, rd.content, sizeof(rd.content));
		if (c->content_length <= 0) {
			printf("sign apk no content\n");
			c->http_error = 403;
			return prepare_response(c, c->http_error, "no content");
		}
		else if (checkToken(c)) {
			printf("sign apk bad token\n");
			c->http_error = 403;
			return prepare_response(c, c->http_error, "bad token");
		}
		
		rd.content[c->content_length] = 0;
		char* path = (char*)rd.content;
		struct stat stBuf;
		if (stat(path, &stBuf))
		{
			printf("sign apk path %s not exist\n", path);
			c->http_error = 404;
			return prepare_response(c, c->http_error, "");
		}
		else {
			char cmd[512] = "";
			snprintf(cmd, sizeof(cmd)-1, "../sign_apk.sh %s", path);
			printf("cmd %s\n", cmd);
			
			system(cmd);
			
			c->http_error = 200;
			return prepare_response(c, c->http_error, "");
		}
		
		return 0;
	}
	
	#endif
	
	if(c->post && c->content_length 
		&& strncmp(c->url, "stream/", 7)
		&& !av_match_ext(c->url, "m3u8")
		&& !av_match_ext(c->url, "ts")
		&& !av_match_ext(c->url, "flv")){
		c->post = 0;
		c->content_length = read_request_content(c, rd.content, sizeof(rd.content));
	}
	#if defined(PLUGIN_DVB)
	if(!c->post && !strcmp(c->url, "digitalDvb/allServiceType/getClientInfo")){
		uint32_t *ptr = (uint32_t*)rd.content, *ptr_end = (uint32_t*)(rd.content+sizeof(rd.content)-8);
		for(ctx = first_http_ctx; ctx; ctx = ctx->next) 
			if(!ctx->post && av_match_ext(ctx->url, "flv") )
			{/*todo: record hls*/
				if(ptr < ptr_end){
					int chid = -1;
					sscanf(ctx->url, "%d", &chid);
		
					*ptr++ = inet_addr(inet_ntoa(ctx->from_addr.sin_addr));
					*ptr++ = chid;

					printf("ip %s id %u %s\t", inet_ntoa(ctx->from_addr.sin_addr), chid, ctx->url);
				}
			}
	}
	#endif

    //http_log("New conn: %s:%u %d %s cookie:%s\n", inet_ntoa(c->from_addr.sin_addr), ntohs(c->from_addr.sin_port), c->post, c->url, rd.cookie);

	/*handle m3u8/ts request solely*/
	if(strncmp(c->url, "stream/", 7)
		&& (av_match_ext(c->url, "m3u8") 
			|| av_match_ext(c->url, "ts"))){
		c->keep_alive = 0; 
		ret = hls_parse_request(c, c->url, is_first);
		if(ret < 0)goto send_error;
		else if(ret == 1){
			long chid = atoi(c->url);
			if(!(0 <= chid && chid <= 10000)){
				sprintf(msg, "bad request: %s-->%ld", c->url, chid);
				http_log("%s\n", msg);
				goto send_error;
			}
			#if defined(PLUGIN_DVB)
			ff_ctl_send_string(1, c->url, rd.content);
			#endif
			http_log("wait get %s\n", c->url);
		}
		if(c->state == HTTPSTATE_SEND_HEADER)
			goto send_header;
		return 0; /*end here*/
	}

	#if defined(PLUGIN_DVB)
	ret = plugin_dvb(c, &rd);
	if(ret < 0){
		goto send_error;
	}else if(ret > 0){
		return 0;
	}
	#endif

    /*handle feed request*/
    if (c->post) {
		ctx = find_feed(c->url);
		if(ctx && ctx != c){
			sprintf(msg, "file %s has been feeded", c->url);
			http_log("%s\n", msg);
			goto send_error;
		}
        c->http_error = 0;
        c->state = HTTPSTATE_RECEIVE_DATA;
        return 0; /*end here*/
	}else{
		if(prepare_local_file(c, &rd) > 0){
			c->state = HTTPSTATE_SEND_HEADER;
			return 0; /*no need feed, send local files directly.*/
		}
		else if(strncmp(c->url, "stream/", 7)){
			snprintf(msg, sizeof(msg), "%s", "404 Not Found");
			goto send_error;
		}
		
		ctx = find_feed(c->url);
		if(!ctx){
			c->keep_alive = 0; 
			sprintf(msg, "wait to get %s", c->url);
			http_log("%s\n", msg);
			#if defined(PLUGIN_DVB)
			ff_ctl_send(2, c->url, strlen(c->url)+1, rd.content, sizeof(rd.content)); 
			#endif
		}else{
			ctx->sff_ref_cnt++;
		}
		c->feed_ctx = ctx; 
	}

send_header:
    /* prepare HTTP header */
    c->buffer[0] = 0;
    av_strlcatf(c->buffer, c->buffer_size, "HTTP/1.1 200 OK\r\n");
	mime_type =  get_mine_type(c->url);
    av_strlcatf(c->buffer, c->buffer_size, "Pragma: no-cache\r\n");
    av_strlcatf(c->buffer, c->buffer_size, "Content-Type: %s\r\n", mime_type);
	av_strlcatf(c->buffer, c->buffer_size, "Connection: %s\r\n", (c->keep_alive ? "keep-alive" : "close"));
	av_strlcatf(c->buffer, c->buffer_size, "Set-Cookie: %s; Path=/; Domain=%s\r\n", first_tag, rd.domain);
    av_strlcatf(c->buffer, c->buffer_size, "\r\n");

    q = c->buffer + strlen(c->buffer);

    /* prepare output buffer */
    c->http_error = 0;
    c->buffer_ptr = c->buffer;
    c->buffer_end = q;
    c->state = HTTPSTATE_SEND_HEADER;

	#if 0
	if(S == c->hls_idx){
		HLS *s = &s_hls[c->hls_idx];
		char *ext = strrchr(c->url, '.');
		if(!(2 == s->flag && s->data && s->csize > 0)){/*not exist yet, fake one*/
			c->http_error = 200;
			c->buffer_end += sprintf(c->buffer_end, 
				"#EXTM3U\n"
				"#EXT-X-VERSION:3\n"
				"#EXT-X-TARGETDURATION:2\n"
				"#EXT-X-MEDIA-SEQUENCE:0\n"
				"#EXTINF:1.283989,\n"
				"%.*s0.ts\n", ext - c->url, c->url);
		}
	}
	#endif
    return 0;
 send_error:
	c->keep_alive = 0;
    c->http_error = 404;
    q = c->buffer;
    htmlstrip(msg);
    snprintf(q, c->buffer_size,
                  "HTTP/1.1 404 Not Found\r\n"
                  "Content-type: text/html\r\n"
                  "\r\n"
                  "<html>\n"
                  "<head><title>404 Not Found</title></head>\n"
                  "<body>%s</body>\n"
                  "</html>\n", msg);
    q += strlen(q);
    /* prepare output buffer */
    c->buffer_ptr = c->buffer;
    c->buffer_end = q;
    c->state = HTTPSTATE_SEND_HEADER;
    return 0;
}


static int sff_prepare_data(HTTPContext *c)
{
	SFF *sff = NULL;  
	packet_t pkt = {0};

    switch(c->state) {
    case HTTPSTATE_SEND_DATA_HEADER:
		sff = sff_read(c, 1);
		if(!sff){
			printf("prepare no sff for %s\n", c->url);
			c->state = HTTPSTATE_WAIT_FEED;
			return 1;
		}
		c->buffer_ptr = sff->data;
        c->buffer_end = sff->data + sff->size;

        c->state = HTTPSTATE_SEND_DATA;
        c->last_packet_sent = 0;
        break;
    case HTTPSTATE_SEND_DATA:
		sff = sff_read(c, 2);
		if(!sff){
			//printf("2prepare no sff\n");
			c->state = HTTPSTATE_WAIT_FEED;
			return 1;
		}
		
		if(sff_parse(sff, &pkt)){
			printf("parse erro\n");
			return AVERROR(EAGAIN);
		}

		c->buffer_ptr = pkt.data;
		c->buffer_end = pkt.data + pkt.size;
		if(-1 == pkt.stream_index){
			c->state = HTTPSTATE_SEND_DATA_TRAILER;
		}

        break;
    default:
    case HTTPSTATE_SEND_DATA_TRAILER:
        c->last_packet_sent = 1;
		http_log("send trailer status\n");
		return -2;
        break;
    }
    return 0;
}

static int http_send_data(HTTPContext *c)
{
    int len, ret;

    for(;;) {
        if (c->buffer_ptr >= c->buffer_end) {
			c->buffer += CHUNK_HEAD_LEN;
			c->buffer_size -= (CHUNK_HEAD_LEN+2);
            ret = c->hls_idx >= 0 ? hls_read(c) : (c->local_fd == -1 ? sff_prepare_data(c) : local_prepare_data(c));
			c->buffer -= CHUNK_HEAD_LEN;
			c->buffer_size += (CHUNK_HEAD_LEN+2);

			if(1 == c->tr_encoding && 0 == ret){/*transfer: add chunked header and tail*/
				len = snprintf(c->buffer, CHUNK_HEAD_LEN, "%lx\r\n", c->buffer_end - c->buffer_ptr);
				memmove(c->buffer_ptr-len, c->buffer, len);
				c->buffer_ptr -= len;
				memcpy(c->buffer_end, "\r\n", 2);
				c->buffer_end += 2;
			}

            if (ret < 0){ /*error occured*/
                return -1;
			}else if (ret > 0){ /*state change requested */
				if(c->state == HTTPSTATE_WAIT_REQUEST){
					memset(c->buffer, 0, c->buffer_size);
					c->buffer_ptr = c->buffer;
					c->buffer_end = c->buffer + c->buffer_size - 1;
				}
				break;
			}else if(ret == 0){/*prepare ok*/
				/*fall through*/
			}
		} else {
			/*send: using tcp or ssl*/
			len = send(c->fd, c->buffer_ptr, c->buffer_end - c->buffer_ptr, 0);
			if (len < 0) {
				if (ff_neterrno() != AVERROR(EAGAIN) &&
						ff_neterrno() != AVERROR(EINTR))
					/* error : close connection */
					return -1;
				else
					return 0;
			} else
				c->buffer_ptr += len;

			c->data_count += len;
			break;
		}
    } /* for(;;) */
    return 0;
}


static int parse_config(int ac, char **av)
{
	int i, tmp;
	if(1 == ac || (ac > 1 && !strcmp(av[1]+1, "h"))){
		fprintf(stderr, "usage: -http_port 8080 -https_port 8081 -https_cert server.pem\n"
				"set port=0 means disable.\n");
		exit(0);
	}

	for(i = 1; i < ac; ++i){
		if('-' == av[i][0] && i < ac-1){
			if(!strcmp(av[i]+1, "http_port")){
				tmp = atoi(av[i+1]);
				my_http_addr.sin_port = htons(tmp);
			}
			#if PLUGIN_SSL
			else if(!strcmp(av[i]+1, "https_port")){
				tmp = atoi(av[i+1]);
				my_https_addr.sin_port = htons(tmp);
			}
			else if(!strcmp(av[i]+1, "https_cert")){
				if(ssl_init(av[i+1], av[i+1]) < 0){
					exit(1);
				}
			}
			#endif
			else if (!strcmp(av[i]+1, "upload_token")){
				strncpy(sUploadToken, av[i+1], sizeof(sUploadToken)-1);
			}
			
			else{
				fprintf(stderr, "unkown option '%s %s'\n", av[i], av[i+1]);
				exit(1);
			}
			++i;
		}
		else{
			fprintf(stderr, "unkown option %s\n", av[i]);
			exit(1);
		}
	}
	
	nb_max_http_connections = 1000;
	nb_max_connections = 1000;
	max_bandwidth = 80000;
    logfile = stdout;
	#if FFMPEG_SRC
    av_log_set_callback(http_av_log);
	#endif
	return 0;
}

char program_name[16] = "ffserver";
int program_birth_year = 2014;
void show_help_default(const char *opt, const char *arg);
void show_help_default(const char *opt, const char *arg)
{
	http_log("dumpy default help\n");
}

int main(int argc, char **argv)
{
    //av_register_all();
    //avformat_network_init();
    putenv("http_proxy=");  /* Kill the http_proxy */

	parse_config(argc, argv);
	#if HAVE_WINSOCK2_H
	WSADATA wsa;
	WSAStartup(MAKEWORD(1,1), &wsa);
	_fmode = _O_BINARY;  //extern extern int _fmode in crt0.o
	#else
    signal(SIGPIPE, SIG_IGN);
	#endif
	get_host_ip();

    if (http_server() < 0) {
        http_log("Could not start server\n");
        exit(1);
    }
	#if PLUGIN_SSL
	ssl_destroy();
	#endif

    return 0;
}

