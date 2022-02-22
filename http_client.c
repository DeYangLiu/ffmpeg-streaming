#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h> //PRId64
#include <ctype.h>


#include "compact.h"
#include <stdarg.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#include "time_log.h"

int main(int ac, char** av) {
	int sockfd;
	int ret = 0;
	int len = 0;
	if (ac < 4) {
		return fprintf(stderr, "usage: http_client server_ip server_port get_file_name\n");
	}

	#if HAVE_WINSOCK2_H
	WSADATA wsa;
	WSAStartup(MAKEWORD(1,1), &wsa);
	_fmode = _O_BINARY;  //extern extern int _fmode in crt0.o
	#else
    signal(SIGPIPE, SIG_IGN);
	#endif

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		perror("can not create socket\n");
		return -1;
	}
	
	int tcp_nodelay = 1;
	if (setsockopt (sockfd, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay, sizeof (tcp_nodelay))) {
    	time_log("setsockopt(TCP_NODELAY) fail");
    }

	int recv_buffer_size = 500*1024;
	if (setsockopt (sockfd, SOL_SOCKET, SO_RCVBUF, &recv_buffer_size, sizeof(recv_buffer_size))) {
    	time_log("setsockopt(SO_RCVBUF) fail");
    }

	int send_buffer_size = 500*1024;
	if (setsockopt (sockfd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size))) {
    	time_log("setsockopt(SO_RCVBUF) fail");
    }

	int dataSize = 1*1024*1024;
	unsigned char *dataBuf = malloc(dataSize);
	memset(dataBuf, 0, dataSize);

	time_log("begin connect \n");

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(atoi(av[2]));
	address.sin_addr.s_addr = inet_addr(av[1]);

	ret = connect(sockfd, (struct sockaddr*)&address, sizeof(struct sockaddr));
	if (ret < 0) {
		time_log("connect error");
		goto end;
	}

	unsigned char buf[1024];
	snprintf(buf, sizeof(buf), "GET /%s HTTP/1.1\r\n\r\n", av[3]);

	int verbose = av[4] ? av[4][0] == 'v' : 0;

	time_log("begin send\n");
	len = send(sockfd, buf, strlen(buf), 0);
	if (len < 0) {
		time_log("send data error\n");
		goto end;;
	}

	time_log("begin recv\n");
	
	int dataLen = 0;
	int tmpLen = 0;
	do {
		tmpLen = recv(sockfd, dataBuf+dataLen, dataSize-dataLen, 0);
		if (tmpLen > 0)
			dataLen += tmpLen;	
		if (verbose)time_log("recvLen %d space %d\n", tmpLen, dataSize-dataLen);
	}
	while((tmpLen > 0) && (dataLen <= dataSize));
	
	time_log("end recv dataLen %d\n", dataLen);

	unsigned char *ptr = dataBuf;
	unsigned char *ptr_end = dataBuf + dataLen;
	while(ptr < ptr_end) {
		if (!memcmp(ptr, "\r\n\r\n", 4)) {
			ptr += 4;
			break;
		}
		ptr++;
	}
	
	time_log("data head:0x%02x %02x %02x %02x tail:0x%02x %02x %02x %02x\n", 
		ptr[0], ptr[1], ptr[2], ptr[3], ptr_end[-4], ptr_end[-3], ptr_end[-2], ptr_end[-1]);

end:
	if (sockfd >=0)close_socket(sockfd);
	if (dataBuf) free(dataBuf);
	return 0;
}
