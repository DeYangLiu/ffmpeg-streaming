/*
 multiple format streaming server based on the FFmpeg libraries
 */

#include "config.h"
#if !HAVE_CLOSESOCKET
#define closesocket close
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "libavformat/avformat.h"
#include "libavformat/network.h"
#include "libavformat/os_support.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/time.h"

#include <stdarg.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/ioctl.h>
#if HAVE_POLL_H
#include <poll.h>
#endif
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/inotify.h>

#if 1
#include <sys/types.h>
#include<sys/msg.h>
#define FF_MSG_CTRL 1234
typedef struct{
	long type; /*=1*/

	long cmd;
	long para[4];
	char info[32];
}msg_ctl_t;
static int ctl_id = -1;
static int ff_ctl_send(long cmd, char *info)
{
	int ret = -1;
	msg_ctl_t m = {.type = 1, };
	m.cmd = cmd;
	sscanf(info, "%ld", &m.para[0]);
	sprintf(m.info, "%s", info);

	ret = msgsnd(ctl_id, &m, sizeof(msg_ctl_t)-sizeof(long), IPC_NOWAIT);
	return ret;
}
#endif

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


#define N 32
typedef struct{
	int type;
	int size;
	uint8_t *data;
	int wpos;
	int wflag; /*1--is writing, cant be read*/
}SFF;

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

    char url[64];
	int post;
	int http_error;
	int keep_alive;
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
} HTTPContext;

typedef struct{/*extra data not in HTTPContext*/
	char domain[64];
	char cookie[512];
}RequestData;

static struct sockaddr_in my_http_addr;
static HTTPContext *first_http_ctx;

static void log_connection(HTTPContext *c);

static void new_connection(int server_fd, int is_rtsp);
static void close_connection(HTTPContext *c);

static int handle_connection(HTTPContext *c);
static int http_parse_request(HTTPContext *c);
static int http_send_data(HTTPContext *c);
static int http_receive_data(HTTPContext *c);

/* maximum number of simultaneous HTTP connections */
static unsigned int nb_max_http_connections = 2000;
static unsigned int nb_max_connections = 5;
static unsigned int nb_connections;
static uint64_t max_bandwidth = 1000;
static int64_t cur_time; 

static FILE *logfile = NULL;
static void http_log(const char *fmt, ...);

#if 1

static int is_static_file(char *name)
{/*static file is defined as whose content-length can be directly set by ffserver.*/
	static char* a[] = {
		"ProgList.htm",
	};
	int i;

	if(av_match_ext(name, "m3u8") || av_match_ext(name, "ts")){
		return 0;
	}
	
	for(i = 0; i < sizeof(a)/sizeof(a[0]); ++i){
		if(!av_strcasecmp(name, a[i]))return 0;
	}

	return 1;
}

static char* get_mine_type(char *name)
{
	int i, n;
	
	static char* mm[][2] = {
		".htm", "text/html",
		".m3u8", "text/plain",
		"", "application/octet-stream",
	};

	n = sizeof(mm)/sizeof(mm[0]);
	for(i = 0; i < n; ++i){
		if(av_stristr(name, mm[i][0]))break;
	}
	if(i >= n)i = n -1;

	return mm[i][1];
}


static int prepare_local_file(HTTPContext *c)
{/*return 1 if local file exist and can be read to buffer.*/
    int len;
	unsigned char tmp[64] = "";
	char prefix[32] = ".";
	int fd = -1;
	struct stat st = {0};

	snprintf(tmp, sizeof(tmp)-1, "%s/%s",  prefix, c->url);
	fd = open(tmp, O_RDONLY);
	if(fd < 0){
		return 0;
	}
	if((fstat(fd, &st) < 0) || (st.st_size > SIZE_MAX)){
		return 0;
	}
	
	c->pb_buffer = av_malloc(1024 + st.st_size);
	if(!c->pb_buffer){
        c->buffer_ptr = c->buffer;
        c->buffer_end = c->buffer;
		close(fd);
		return 0;
	}
	
	len = sprintf(c->pb_buffer, "HTTP/1.0 200 OK\r\n"
			"Content-type: %s\r\n"
			"Content-Length: %u\r\n"
			"Connection: %s\r\n"
			"\r\n", 
			get_mine_type(c->url),
			(unsigned int)st.st_size, 
			(c->keep_alive ? "keep-alive" : "close") );

	len += read(fd, c->pb_buffer + len, st.st_size);
	close(fd);

    c->buffer_ptr = c->pb_buffer;
    c->buffer_end = c->pb_buffer + len;
	return 1;
}

#endif 

#if 1 //ludi add

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
		
		http_log("hls post c %p name %s:%d data %p size %d:%d\n", c, name, idx, s->data, s->msize, s->csize);

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
		fp = fopen("dump.flv", "wb");
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
		fwrite(sff->data + 28, sff->size - 28, 1, fp);
		cnt += sff->size - 28;
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

	if(c->sff_r == f->sff_w){
		return NULL;
	}
	
	sff = f->sff_pkts[c->sff_r];
	c->sff_r = (c->sff_r + 1)%N;
	return sff;
}

static int sff_parse(SFF *sff, AVPacket *pkt)
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
		len = recv(c->fd, c->buffer, c->buffer_size, 0);
		if(len > 0){
			hls_write(c, c->buffer, len);	
		}
		goto check;
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
		if(size > 1E6)continue;
		
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
	if(len <= 0 && ret != AVERROR(EAGAIN) && ret != AVERROR(EINTR)){
		//http_log("conn end len %d ret %s\n", len, av_err2str(ret));
		if(s){
			s->flag = 2;
			//printf("hls get seg %d:%d:%d data %02x %02x %02x %02x\n", c->hls_idx, s->msize, s->csize, s->data[0], s->data[1], s->data[2], s->data[3]);
		}
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

#endif

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

static void http_av_log(void *ptr, int level, const char *fmt, va_list vargs)
{
    static int print_prefix = 1;
    AVClass *avc = ptr ? *(AVClass**)ptr : NULL;
    if (level > av_log_get_level())
        return;
    if (print_prefix && avc)
        http_log("[%s @ %p]", avc->item_name(ptr), ptr);
    print_prefix = strstr(fmt, "\n") != NULL;
    http_vlog(fmt, vargs);
}

static void log_connection(HTTPContext *c)
{
	//if(av_match_ext(c->url, "m3u8"))
    	http_log("%s:%u %d \"%s\" %lld %d %d\n",
             inet_ntoa(c->from_addr.sin_addr), ntohs(c->from_addr.sin_port), 
			 c->post, c->url,
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
        av_log(NULL, AV_LOG_WARNING, "setsockopt SO_REUSEADDR failed\n");

    my_addr->sin_family = AF_INET;
    if (bind (server_fd, (struct sockaddr *) my_addr, sizeof (*my_addr)) < 0) {
        char bindmsg[32];
        snprintf(bindmsg, sizeof(bindmsg), "bind(port %d)", ntohs(my_addr->sin_port));
        perror (bindmsg);
        closesocket(server_fd);
        return -1;
    }

    if (listen (server_fd, 5) < 0) {
        perror ("listen");
        closesocket(server_fd);
        return -1;
    }

    if (ff_socket_nonblock(server_fd, 1) < 0)
        av_log(NULL, AV_LOG_WARNING, "ff_socket_nonblock failed\n");

    return server_fd;
}

static int http_server(void)
{
    int server_fd = 0;
    int ret, delay;
    struct pollfd *poll_table, *poll_entry;
    HTTPContext *c, *c_next;

    if(!(poll_table = av_mallocz_array(nb_max_http_connections + 1, sizeof(*poll_table)))) {
        http_log("Impossible to allocate a poll table handling %d connections.\n", nb_max_http_connections);
        return -1;
    }

    if (my_http_addr.sin_port) {
        server_fd = socket_open_listen(&my_http_addr);
        if (server_fd < 0) {
            av_free(poll_table);
            return -1;
        }
    }

    if ( !server_fd) {
        http_log("HTTP and RTSP disabled.\n");
        av_free(poll_table);
        return -1;
    }

	
	ctl_id = msgget(FF_MSG_CTRL, IPC_CREAT|0666);
	if(ctl_id < 0){
		http_log("cant create msg que\n");
		av_free(poll_table);
        return -1;
	}
	
    http_log("FFserver started.\n");

    for(;;) {
        poll_entry = poll_table;
        if (server_fd) {
            poll_entry->fd = server_fd;
            poll_entry->events = POLLIN;
            poll_entry++;
        }

        /* wait for events on each HTTP handle */
        c = first_http_ctx;
        delay = 2000;
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
        if (server_fd) {
            if (poll_entry->revents & POLLIN)
                new_connection(server_fd, 0);
            poll_entry++;
        }
	
    }
}

static void http_send_too_busy_reply(int fd)
{
    char buffer[400];
    int len = snprintf(buffer, sizeof(buffer),
                       "HTTP/1.0 503 Server too busy\r\n"
                       "Content-type: text/html\r\n"
                       "\r\n"
                       "<html><head><title>Too busy</title></head><body>\r\n"
                       "<p>The server is too busy to serve your request at this time.</p>\r\n"
                       "<p>The number of current connections is %u, and this exceeds the limit of %u.</p>\r\n"
                       "</body></html>\r\n",
                       nb_connections, nb_max_connections);
    av_assert0(len < sizeof(buffer));
    if (send(fd, buffer, len, 0) < len)
        av_log(NULL, AV_LOG_WARNING, "Could not send too-busy reply, send() failed\n");
}


static void new_connection(int server_fd, int is_rtsp)
{
    struct sockaddr_in from_addr;
    socklen_t len;
    int fd;
    HTTPContext *c = NULL;

    len = sizeof(from_addr);
    fd = accept(server_fd, (struct sockaddr *)&from_addr,
                &len);
    if (fd < 0) {
        http_log("error during accept %s\n", strerror(errno));
        return;
    }
    if (ff_socket_nonblock(fd, 1) < 0)
        av_log(NULL, AV_LOG_WARNING, "ff_socket_nonblock failed\n");

    if (nb_connections >= nb_max_connections) {
        http_send_too_busy_reply(fd);
        goto fail;
    }

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

    c->next = first_http_ctx;
    first_http_ctx = c;
    nb_connections++;

	c->buffer_ptr = c->buffer;
    c->buffer_end = c->buffer + c->buffer_size - 1; /* leave room for '\0' */
	c->timeout = cur_time + HTTP_REQUEST_TIMEOUT;
    c->state = HTTPSTATE_WAIT_REQUEST;
	c->hls_idx = -1;

    return;
 fail:
    if (c) {
        av_free(c->buffer);
        av_free(c);
    }
    closesocket(fd);
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
    if (c->fd >= 0)
        closesocket(c->fd);

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
            c->buffer_ptr += len;
            c->data_count += len;
            if (c->buffer_ptr >= c->buffer_end) {
                av_freep(&c->pb_buffer);
				if(c->keep_alive){
					c->buffer_ptr = c->buffer;
				    c->buffer_end = c->buffer + c->buffer_size - 1; 
					c->timeout = cur_time + HTTP_REQUEST_TIMEOUT;
				    c->state = HTTPSTATE_WAIT_REQUEST;
					c->hls_idx = -1;
					http_log("%u alive %s\n", ntohs(c->from_addr.sin_port), c->url);
					return 0;
				}
                /* if error, exit */
                if (c->http_error)
                    return -1;
                /* all the buffer was sent : synchronize to the incoming*/
                c->state = HTTPSTATE_SEND_DATA_HEADER;
                c->buffer_ptr = c->buffer_end = c->buffer;
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
        if (c->poll_entry->revents & (POLLIN | POLLERR | POLLHUP))
            return -1;
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
        if ((q - buf) < buf_size - 1)
            *q++ = *p;
        p++;
    }
    if (buf_size > 0)
        *q = '\0';
    *pp = p;
	return q - buf;
}

static int get_line(char *buf, int buf_size, const char **pp)
{
	const char *p = *pp;
	char *q = buf;

	while(*p && (*p == '\r' || *p == '\n')){
		p++;
	}
	while(*p && !(*p == '\r' || *p == '\n')){
		if(q - buf < buf_size - 1)*q++ = *p++;	
	}
	*q = 0;
	*pp = p;
	return q - buf;
}

static int handle_line(HTTPContext *c, char *line, int line_size, RequestData *rd)
{
	char *p1, tmp[32], info[32];
	const char *p = line;
	int len;
	
	get_word(tmp, sizeof(tmp), &p);
	if(!strcmp(tmp, "GET") || !strcmp(tmp, "POST")){
		if (tmp[0]== 'G')
			c->post = 0;
		else if (tmp[0] == 'P')
			c->post = 1;
		else
			return -1;

		get_word(c->url, sizeof(c->url), &p);
		if(c->url[0] == '/'){
			len = strlen(c->url)-1;
			memmove(c->url, c->url+1, len); 
			c->url[len] = 0;
		}
		if(!c->url[0]){
			av_strlcpy(c->url, "index.html", sizeof(c->url));
		}

		get_word(tmp, sizeof(tmp), &p);
		if (strcmp(tmp, "HTTP/1.0") && strcmp(tmp, "HTTP/1.1"))
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
			c->keep_alive = is_static_file(c->url);
		}
	}
	return 0;
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
	is_first = !av_stristr(rd.cookie, first_tag);

    //http_log("New conn: %s:%u %d %s cookie:%s\n", inet_ntoa(c->from_addr.sin_addr), ntohs(c->from_addr.sin_port), c->post, c->url, rd.cookie);

	/*handle m3u8/ts request solely*/
	if(av_match_ext(c->url, "m3u8") 
			|| av_match_ext(c->url, "ts")){
		ret = hls_parse_request(c, c->url, is_first);
		if(ret < 0)goto send_error;
		else if(ret == 1){
			long chid = atoi(c->url);
			if(!(0 <= chid && chid <= 10000)){
				sprintf(msg, "bad request: %s-->%ld", c->url, chid);
				http_log("%s\n", msg);
				goto send_error;
			}
			ff_ctl_send(1, c->url);
			http_log("wait get %s\n", c->url);
		}
		if(c->state == HTTPSTATE_SEND_HEADER)
			goto send_header;
		return 0; /*end here*/
	}

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
		if(prepare_local_file(c) > 0){
			c->http_error = 200;
			c->state = HTTPSTATE_SEND_HEADER;
			return 0; /*no need feed, send local files directly.*/
		}
		
		ctx = find_feed(c->url);
		if(!ctx){
			sprintf(msg, "wait to get %s", c->url);
			http_log("%s\n", msg);
			ff_ctl_send(2, c->url); 
		}else{
			ctx->sff_ref_cnt++;
		}
		c->feed_ctx = ctx; 
	}

send_header:
    /* prepare HTTP header */
    c->buffer[0] = 0;
    av_strlcatf(c->buffer, c->buffer_size, "HTTP/1.0 200 OK\r\n");
	mime_type =  get_mine_type(c->url);
    av_strlcatf(c->buffer, c->buffer_size, "Pragma: no-cache\r\n");
    av_strlcatf(c->buffer, c->buffer_size, "Content-Type: %s\r\n", mime_type);
	av_strlcatf(c->buffer, c->buffer_size, "Set-Cookie: %s; Path=/; Domain=%s\r\n", first_tag, rd.domain);
    av_strlcatf(c->buffer, c->buffer_size, "\r\n");

    q = c->buffer + strlen(c->buffer);

    /* prepare output buffer */
    c->http_error = 0;
    c->buffer_ptr = c->buffer;
    c->buffer_end = q;
    c->state = HTTPSTATE_SEND_HEADER;
	
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
    return 0;
 send_error:
    c->http_error = 404;
    q = c->buffer;
    htmlstrip(msg);
    snprintf(q, c->buffer_size,
                  "HTTP/1.0 404 Not Found\r\n"
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
	AVPacket pkt = {0};

    switch(c->state) {
    case HTTPSTATE_SEND_DATA_HEADER:
		sff = sff_read(c, 1);
		if(!sff){
			printf("prepare no sff\n");
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

/* send data starting at c->buffer_ptr to the output connection(either UDP or TCP)*/
static int http_send_data(HTTPContext *c)
{
    int len, ret;

    for(;;) {
        if (c->buffer_ptr >= c->buffer_end) {
            ret = c->hls_idx >= 0 ? hls_read(c) : sff_prepare_data(c);
            if (ret < 0)
                return -1;
            else if (ret != 0)
                /* state change requested */
                break;
		} else {
			/* TCP data output */
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


static int parse_config(void)
{
	my_http_addr.sin_port = htons(8090);
	nb_max_http_connections = 1000;
	nb_max_connections = 1000;
	max_bandwidth = 80000;
    logfile = stdout;
    av_log_set_callback(http_av_log);
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
    unsetenv("http_proxy");  /* Kill the http_proxy */

	parse_config();
    signal(SIGPIPE, SIG_IGN);

    if (http_server() < 0) {
        http_log("Could not start server\n");
        exit(1);
    }

    return 0;
}

