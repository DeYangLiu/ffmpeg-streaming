#ifndef _PLUGIN_SSL_H_
#define _PLUGIN_SSL_H_
int ssl_init(char *cert_file, char *private_key);
int ssl_destroy(void);
void* ssl_open(int sock);
int ssl_close(void *sl);
int ssl_read(void *sl, unsigned char *buf, int len);
int ssl_write(void *sl, unsigned char *buf, int len);

#endif //_PLUGIN_SSL_H_
