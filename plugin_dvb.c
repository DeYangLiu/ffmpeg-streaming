
#include <sys/types.h>
#include<sys/msg.h>

#define MAX_URL_NAME_LENGTH 128
#define FF_MSG_CTRL 1234
typedef struct{
	long type; /*=1*/

	long cmd;
	char info[MAX_URL_NAME_LENGTH];
	char content[512];
}msg_ctl_t;
static int ctl_id = -1;
static int ff_ctl_send(long cmd, char *info, char *para)
{
	int ret = -1;
	msg_ctl_t m = {.type = 1, };
	m.cmd = cmd;
	
	sprintf(m.info, "%s", info);
	if(para){
		memcpy(m.content, para, sizeof(m.content));
	}

	ret = msgsnd(ctl_id, &m, sizeof(msg_ctl_t)-sizeof(long), IPC_NOWAIT);

	if(av_match_ext(info, "flv")){
		hls_close();
	}else if(av_match_ext(info, "m3u8")){
		sff_close();
	}
	
	return ret;
}


static int ignore_request(HTTPContext *c)
{
	static const char *a[] = {
		"js/",
		NULL,
	};
	const char **ptr = a;

	for(; *ptr; ++ptr){
		if(!strcmp(c->url, *ptr))return 1;
	}
	return 0;
}

static int handle_control_request(HTTPContext *c, RequestData *rd)
{
	char msg[128] = "";
	char *q = NULL;

	if(!strcmp(c->url, "digitalDvb/playerPermit")){
		strcpy(msg, "<playerPermit> <status>true</status> <error>E000</error> </playerPermit>");
	}else if(av_match_ext(c->url, "flv")){
		c->keep_alive = 0;
	}

	if(!msg[0]){
		return 0;
	}

	c->buffer[0] = 0;
    av_strlcatf(c->buffer, c->buffer_size, "HTTP/1.1 200 OK\r\n");
    av_strlcatf(c->buffer, c->buffer_size, "Pragma: no-cache\r\n");
    av_strlcatf(c->buffer, c->buffer_size, "Content-Type: %s;charset=UTF-8\r\n", "application/xml");
	av_strlcatf(c->buffer, c->buffer_size, "Content-Length: %d\r\n", strlen(msg));
	if(c->post){
		av_strlcatf(c->buffer, c->buffer_size, "Connection: Close\r\n");
		if(rd->cookie[0])av_strlcatf(c->buffer, c->buffer_size, "Cookie: %s\r\n", rd->cookie);
		c->post = 0; /*redirect post to get*/
	}   
	av_strlcatf(c->buffer, c->buffer_size, "\r\n");

	av_strlcatf(c->buffer, c->buffer_size, "%s", msg);
    q = c->buffer + strlen(c->buffer);
	
    c->http_error = 200;
    c->buffer_ptr = c->buffer;
    c->buffer_end = q;
    c->state = HTTPSTATE_SEND_HEADER;
	
	return 1;
}

static int plugin_dvb(HTTPContext *c, RequestData *rd)
{/*return 0 -- continue, >0 -- ok, stop, <0 -- send 404 error.*/
	int ret = -1;
	ret = ignore_request(c);
	if(ret){
		return -1;
	}

	ret = handle_control_request(c, rd);
	if(ret != 0){
		return ret;
	}

	return 0;
}
