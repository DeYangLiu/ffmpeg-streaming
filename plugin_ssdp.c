#ifndef _BSD_SOURCE
#define _BSD_SOURCE  /*for struct ip_mreq, must be precede all header files.*/
#endif
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <net/if.h> 

static const char *SSDP_ST = "urn:schemas-upnp-org:device:Basic:1";
static const char *SSDP_USN = "uuid:37d0a56f-985d-4df1-77cd-ca49a836881b::urn:schemas-upnp-org:device:Basic:1"; 
 
const char ssdp_ip[16] = "239.255.255.250";
const int  ssdp_port = 1900;
int  ssdp_fd = -1;

static char* get_hostip(void)   
{   
    int fd;   
    struct sockaddr_in *sin;   
    struct ifreq ifr_ip;
	static char ipbuf[32] = "";
   
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0){  
        goto end;   
    }   
  
    memset(&ifr_ip, 0, sizeof(ifr_ip));      
    strncpy(ifr_ip.ifr_name, "eth0", sizeof(ifr_ip.ifr_name)-1);      
    
    if(ioctl(fd, SIOCGIFADDR, &ifr_ip) < 0){      
         memset(&ifr_ip, 0, sizeof(ifr_ip));      
		 strncpy(ifr_ip.ifr_name, "wlan0", sizeof(ifr_ip.ifr_name)-1);  
		 if(ioctl(fd, SIOCGIFADDR, &ifr_ip) < 0)
		     goto end;     
    }        
    sin = (struct sockaddr_in *)&ifr_ip.ifr_addr;      
    strcpy(ipbuf, inet_ntoa(sin->sin_addr));          

end:
    close(fd);   
    return ipbuf;   
}   

static int mcast_open(const char *ip, int port)
{
	int sock, ret;
	struct sockaddr_in saddr;
	struct in_addr iaddr;
	unsigned char val;
	struct ip_mreq imreq;

	memset(&saddr, 0, sizeof(struct sockaddr_in));
	memset(&iaddr, 0, sizeof(struct in_addr));

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0){
		perror("Error creating socket");
		return -1;
	}

	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(port); 
	saddr.sin_addr.s_addr = INADDR_ANY; 
	ret = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0){
		perror("Error binding socket to interface");
		return -2;
	}

	if (ff_socket_nonblock(sock, 1) < 0)
        http_log("ff_socket_nonblock failed\n");

	val = 0;
	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val));

	iaddr.s_addr = INADDR_ANY; 
	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr, sizeof(struct in_addr));

	imreq.imr_multiaddr.s_addr = inet_addr(ip);
	imreq.imr_interface.s_addr = INADDR_ANY; 
	setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)&imreq, sizeof(struct ip_mreq));

	return sock;
}

static int ssdp_notify(int fd, const char *dstip, int dstport, const char *msg)
{
	int ret, socklen;
	struct sockaddr_in saddr;
	uint8_t buffer[512] = "", *ptr = buffer;

	ptr += sprintf(ptr, 
	"NOTIFY * HTTP/1.1\r\n"
	"NTS: %s\r\n"
	"HOST: %s:%d\r\n"
	"NT: %s\r\n"
	"USN: %s\r\n"
	"Location: http://%s/DeviceDescription.xml\r\n"
	"\r\n",
	msg, dstip, dstport, SSDP_ST, SSDP_USN, get_hostip());

	memset(&saddr, 0, sizeof(struct sockaddr_in));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = inet_addr(dstip);
	saddr.sin_port = htons(dstport);
	socklen = sizeof(struct sockaddr_in);
	ret = sendto(fd, buffer, ptr - buffer, 0, (struct sockaddr *)&saddr, socklen);

	return ret;
}

static int ssdp_response(int fd)
{
	int ret, socklen, buflen; 
	struct sockaddr_in saddr;
	uint8_t buffer[1024] = ""; /*suppose msg frame size is less than this.*/
	uint8_t *ptr = NULL, ans[512] = "", *ap = NULL;
	static int state = 0;
	int found = 0;

	socklen = sizeof(saddr);
	memset(&saddr, 0, socklen);

	buflen = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&saddr, &socklen);
	if(buflen <= 0){
		return -1;
	}
	
	if(!state && !strncmp(buffer, "M-SEARCH", strlen("M-SEARCH"))){
		state = 1;
	}else{
		state = 0;
	}
	
	if(1 == state){
		ptr = strstr(buffer, "ST:");
		if(ptr){
			ptr += 3;
			while(*ptr == ' ')++ptr;
			if(!strncmp(ptr, SSDP_ST, strlen(SSDP_ST))){
				state = 2;
			}
		}
	}
	if(2 == state){
		found = 1;
		if(strstr(buffer, "\r\n\r\n")){
			state = 0;
		}
	}

	if(!found){
		return 1;
	}

	ap = ans;
	ap += sprintf(ap, 
		"HTTP/1.1 200 OK\r\n"
		"Cache-Control: max-age=1800\r\n"
		"ST: %s\r\n"
		"USN: %s\r\n"
		"Location: http://%s:80/DeviceDescription.xml\r\n"
		"\r\n", SSDP_ST, SSDP_USN, get_hostip());	

	ret = sendto(fd, ans, ap - ans, 0, (struct sockaddr *)&saddr, socklen);
	return 0;
}

