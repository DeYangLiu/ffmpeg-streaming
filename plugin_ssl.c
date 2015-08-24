/*-lssl -lcrypto
*/
#include <openssl/ssl.h>
#include <openssl/err.h> 
#include "plugin_ssl.h"

static SSL_CTX *s_ssl_ctx = NULL;

int ssl_init(char *cert_file, char *private_key)
{
	int ret = 0;
	SSL_library_init();
	SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());/*TLS=SSLv3,TLSv1,TLSv1.1,TLSv1_2*/
	if(!ctx){
		printf("alloc ssl ctx fail\n");
		return -1;
	}

	ret = SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM);
	if(ret != 1){
		printf("check cert file fail %d\n", ret);
		goto end;
	}
	ret = SSL_CTX_use_PrivateKey_file(ctx, private_key, SSL_FILETYPE_PEM);
	if(ret != 1){
		printf("check private key file fail %d\n", ret);
		goto end;
	}
	ret = SSL_CTX_check_private_key(ctx);
	if(ret != 1){
		printf("bad private key file %d\n", ret);
		goto end;
	}

	s_ssl_ctx = ctx;
	return 0;
end:
	if(ctx){
		SSL_CTX_free(ctx);
	}
	return -1;
}

int ssl_destroy(void)
{
	SSL_CTX_free(s_ssl_ctx);
	s_ssl_ctx = NULL;
	return 0;
}


void* ssl_open(int sock)
{
	int n = 0;
	SSL	*ssl = NULL;
	ssl = SSL_new(s_ssl_ctx);
	SSL_set_fd(ssl, sock);
	n = SSL_accept(ssl); 
	if(1 != n){
		n = SSL_get_error(ssl, n);
		if (n != SSL_ERROR_WANT_READ && n != SSL_ERROR_WANT_WRITE){
			SSL_free(ssl);
			ssl = NULL; /*not support renegotiation.*/
		}
	}
	return ssl;
}

int ssl_close(void *sl)
{
	SSL *ssl = (SSL*)sl;
	SSL_shutdown(ssl); //send close notify alert
	SSL_free(ssl);
	return 0;
}

int ssl_read(void *sl, unsigned char *buf, int len)
{
	SSL *ssl = (SSL*)sl;
	int n = -1;
	n = SSL_read(ssl, buf, len);
	if(n < 0){
		if(SSL_get_error(ssl, n) == SSL_ERROR_WANT_READ){
			n = -11; /*EAGAIN*/
		}else{
			n = -1; /*EPERM*/
		}
	}
	return n;
}

int ssl_write(void *sl, unsigned char *buf, int len)
{
	SSL *ssl = (SSL*)sl;
	int n = -1;
	n = SSL_write(ssl, buf, len);
	if(n < 0){
		if(SSL_get_error(ssl, n) == SSL_ERROR_WANT_WRITE){
			n = -11; /*EAGAIN*/
		}else{
			n = -1; /*EPERM*/
		}
	}
	return n;
}

