/*IPC using tcp socket 

test:
 gcc -g -Wall stream_buffer.c -DTEST_MSG_BUFFER
 ./a.out
 ./a.out 127.0.0.1

*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>

#ifndef AV_RB32
#define AV_RB32(x)                                \
    (((uint32_t)((const uint8_t*)(x))[0] << 24) |    \
               (((const uint8_t*)(x))[1] << 16) |    \
               (((const uint8_t*)(x))[2] <<  8) |    \
                ((const uint8_t*)(x))[3])
#endif
#ifndef AV_WB32
#define AV_WB32(p, darg) do {                \
        unsigned d = (darg);                    \
        ((uint8_t*)(p))[3] = (d);               \
        ((uint8_t*)(p))[2] = (d)>>8;            \
        ((uint8_t*)(p))[1] = (d)>>16;           \
        ((uint8_t*)(p))[0] = (d)>>24;           \
    } while(0)
#endif

typedef struct{
	int cmd; /*cmd,nlen,dlen should be non-zero*/
	int nlen, dlen;
	uint8_t *name, *data; /*dynamic allocated, will be freed by caller.*/
}ctrl_msg_t;

typedef struct{
	int rpos, wpos;
	int msize;
	uint8_t *buf;
}StreamBuffer;

typedef int (*ctl_func)(ctrl_msg_t *msg);

int ff_ctl_open(unsigned short port);
int ff_ctl_open2(char *ip, unsigned short port);
int ff_ctl_close(void);
int ff_ctl_send(int cmd, uint8_t *name, int nlen, uint8_t *data, int dlen);
int ff_ctl_send_string(int cmd, char *name, char *data);
int ff_ctl_recv(ctl_func cb);


static int ctrl_fd, serv_fd;
static StreamBuffer *sb_in, *sb_out;

static StreamBuffer* sb_init(int msize)
{
	StreamBuffer *s = NULL;

	if(msize < 1){
		return NULL;
	}
	
	s = malloc(sizeof(*s));
	if(!s){
		return NULL;
	}

	s->rpos = s->wpos = 0;
	s->msize = msize;
	s->buf = malloc(s->msize);
	if(!s->buf){
		free(s);
		return NULL;
	}

	return s;
}

static int sb_destroy(StreamBuffer *s)
{
	if(s && s->buf){
		free(s->buf);
	}
	if(s){
		free(s);
	}
	return 0;
}

static int sb_write(StreamBuffer *s, uint8_t *data, int len)
{/*return < 0 means fail.*/
	int size;
	if(!s || !data || len < 0){
		return -1;
	}

	if(s->msize - s->wpos >= len){
		memcpy(s->buf + s->wpos, data, len);
		s->wpos += len;
	}else if (s->msize - s->wpos + s->rpos >= len){
		size = s->wpos - s->rpos;
		memmove(s->buf, s->buf+s->rpos, size);
		s->rpos = 0;
		s->wpos = size;
		memcpy(s->buf+s->wpos, data, len);
		s->wpos += len;
	}else{
		printf("sb buf full\n");
		return -1;
	}

	return 0;
}

static int sb_read(StreamBuffer *s, uint8_t *data, int len)
{/*read actual read bytes.*/
	int size;
	if(!s || !data || len < 0){
		return 0;
	}

	size = s->wpos - s->rpos;
	if(size > len){
		size = len;
	}
	memcpy(data, s->buf+s->rpos, size);
	s->rpos += size;
	
	return size;
}


static int ctl_msg_open(int server_fd)
{
    struct sockaddr_in from_addr;
    socklen_t len;
    int fd;
	
    len = sizeof(from_addr);
	memset(&from_addr, 0, len);
    fd = accept(server_fd, (struct sockaddr *)&from_addr, &len);
    if (fd < 0) {
        printf("error setup during accept %s\n", strerror(errno));
        return -1;
    }
	printf("ctl chan conn %s:%u\n", inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));
	
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0){
        printf("set non-block failed\n");
    }
	ctrl_fd = fd;
	return fd;
}


static int ctl_msg_recv(void)
{
	int len;
	uint8_t buf[1024];

	while( (len = recv(ctrl_fd, buf, sizeof(buf), 0)) > 0){
		sb_write(sb_in, buf, len);
	}

	return 0;
}

static int ctl_msg_pending(void)
{
	StreamBuffer *sb = sb_out;
	if(!sb){
		return -1;
	}
	return sb->wpos - sb->rpos;	
}

static int ctl_msg_send(void)
{
	int len;
	uint8_t *ptr = NULL;
	StreamBuffer *sb = sb_out;

	if(!sb){
		return -1;
	}
	
	len = sb->wpos - sb->rpos;
	if(len <= 0){
		return 1;
	}
	ptr = sb->buf + sb->rpos;
	
	len = send(ctrl_fd, ptr, len, 0);
	if(len > 0){
		sb->rpos += len;
	}
	
	return 0;
}

int ff_ctl_open(unsigned short port)
{
    int fd, tmp;
	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror ("socket");
        return -1;
    }

    tmp = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof(tmp)))
        printf("setsockopt SO_REUSEADDR failed\n");
	
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        printf("cant bind\n");
        close(fd);
        return -1;
    }

    if(listen(fd, 5) < 0){
        perror ("listen");
        close(fd);
        return -1;
    }

    if(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0){
        printf("set non block failed\n");
    }

	serv_fd = fd;
	sb_in = sb_init(8096);
	sb_out = sb_init(8096);
    return fd;
}

int ff_ctl_open2(char *ip, unsigned short port)
{
    int fd;
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	
	if(inet_aton(ip, &addr.sin_addr) == 0){
        printf("bad ip '%s'\n", ip);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror ("socket");
        return -1;
    }

    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0){
       printf("cant connect to ip '%s'\n", ip);
	   close(fd);
       return -1;
    } 

    if(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0){
        printf("set non block failed\n");
    }

	ctrl_fd = fd;
	sb_in = sb_init(8096);
	sb_out = sb_init(8096);

    return fd;
}

int ff_ctl_close(void)
{
	sb_destroy(sb_in);
	sb_destroy(sb_out);
	ctrl_fd = -1;
	serv_fd = -1;
	return 0;
}

int ff_ctl_recv(ctl_func cb)
{/*unpack sb_in and dispatch messages in it.*/
	StreamBuffer *sb = sb_in;
	uint8_t *ptr, *end;
	static ctrl_msg_t msg = {0};
	
	if(!sb){
		return -1;
	}

	ptr = sb->buf + sb->rpos;
	end = sb->buf + sb->wpos;
	while(ptr + 4 < end){
		if(msg.name)goto data;
		if(msg.dlen)goto name;
		if(msg.nlen)goto dlen;
		if(msg.cmd)goto nlen;
		
		if(memcmp(ptr, "MSG", 3)){
			ptr++;
			continue;
		}
		ptr += 3;
		
		msg.cmd = ptr[0];
		ptr += 1;
		if(ptr >= end)break;

		nlen:
		msg.nlen = AV_RB32(ptr);
		ptr += 4;
		if(ptr >= end)break;

		dlen:
		msg.dlen = AV_RB32(ptr);
		ptr += 4;
		if(ptr >= end)break;

		name:
		if(msg.nlen > 0 && ptr + msg.nlen <= end){
			msg.name = malloc(msg.nlen);
			memcpy(msg.name, ptr, msg.nlen);
			ptr += msg.nlen;
		}
		if(ptr >= end)break;

		data:
		if(msg.dlen > 0 && ptr + msg.dlen <= end){
			msg.data = malloc(msg.dlen);
			memcpy(msg.data, ptr, msg.dlen);
			ptr += msg.dlen;
			
			if(cb){
				cb(&msg);
			}
			free(msg.name);
			free(msg.data);
			memset(&msg, 0, sizeof(msg));
		}
	}

	sb->rpos = ptr - sb->buf;
	return 0;
}

int ff_ctl_send(int cmd, uint8_t *name, int nlen, uint8_t *data, int dlen)
{/*send data to output buffer.*/
	uint8_t *ptr, buf[64];
	StreamBuffer *sb = sb_out;

	if(!sb || !(0 < cmd && cmd <= 255) || !name || nlen < 1 || dlen < 1 || dlen < 1){
		printf("ff ctl bad args\n");
		return -1;
	}

	ptr = buf;
	memcpy(ptr, "MSG", 3);
	ptr += 3;
	*ptr = (uint8_t)cmd;
	ptr += 1;
	AV_WB32(ptr, nlen);
	ptr += 4;
	AV_WB32(ptr, dlen);
	ptr += 4;

	sb_write(sb, buf, ptr-buf);
	sb_write(sb, name, nlen);
	sb_write(sb, data, dlen);
	return 0;
}

int ff_ctl_send_string(int cmd, char *name, char *data)
{
	int nlen, dlen;
	nlen = strlen(name) + 1;
	dlen = strlen(data) + 1;
	return ff_ctl_send(cmd, (uint8_t*)name, nlen, (uint8_t*)data, dlen);
}


#if defined(TEST_MSG_BUFFER)
static int ctl_msg_cb(ctrl_msg_t *msg)
{
	printf("%s: %d '%s' '%s'\n", (serv_fd ? "Server" : "Client"), msg->cmd, msg->name, msg->data);
	return 0;
}

static int strip(char *str)
{
	int n = strlen(str);
	while(n > 0 && (str[n-1] == '\r' || str[n-1] == '\n')){
		str[--n] = 0;
	}
	return n;
}

int main(int ac, char **av)
{
	int ret, fd, is_server = 1;
	int peer_fd = 0;

	if(ac != 2){
		printf("start server\n");
	}else{
		is_server = 0;
	}

	if(is_server){
		fd = ff_ctl_open(5678);
	}else{
		fd = ff_ctl_open2(av[1], 5678);
	}

	struct pollfd *entry, table[8] = {{0}};
	
	for(;;) {
		entry = table;
		if(fd){   
			entry->fd = fd;
	        entry->events = POLLIN|POLLOUT;
	        entry++;
		}
		
		if(peer_fd){
			entry->fd = peer_fd;
        	entry->events = POLLIN|POLLOUT;
        	entry++;
		}
		
		do {
            ret = poll(table, entry - table, 1000);
        } while (ret < 0);

		for(entry = table; entry->fd; ++entry){
			if(entry->revents & POLLIN){
				if(is_server && entry->fd == fd && peer_fd <= 0){
					peer_fd = ctl_msg_open(fd);
				}else{
					ctl_msg_recv();
					ff_ctl_recv(ctl_msg_cb);
				}
			}else if(entry->revents & POLLOUT){
				char line[128] = "";
				
				printf("> ");
	     		fflush(stdout);
				fgets(line, sizeof(line), stdin);
				strip(line);

				ff_ctl_send_string(2, "cmd", line);
				
				ctl_msg_send();
			}
		}
		
	}
}
#endif
