
static int dir2html(char *path, char *uri, char *buf, int len)
{
	int ret = 0;
	struct dirent *dp = NULL;
	char file[FILENAME_MAX], size[64], mod[64];
	struct stat	st;
	const char *slash = "";

	DIR *dirp = opendir(path);
	if(NULL == dirp){
		return 0;
	}
	
	ret += snprintf((!buf ? NULL : buf + ret), (!len ? 0 : len - ret),	
		    "<html><head><title>Index of %s</title>"
		    "<style>th {text-align: left;}</style></head>"
		    "<body><h1>Index of %s</h1><pre><table cellpadding=\"0\">\n"
		    "<tr><th>Name</th><th>Modified</th><th>Size</th></tr>"
		    "<tr><td colspan=\"3\"><hr></td></tr>\n",
		    uri, uri);

	while((dp = readdir(dirp))){
		if(!strcmp(dp->d_name, ".")){continue;}
		snprintf(file, sizeof(file), "%s/%s", path, dp->d_name);
		stat(file, &st);
		strftime(mod, sizeof(mod), "%d-%b-%Y %H:%M", localtime(&st.st_mtime));
		if (S_ISDIR(st.st_mode)) {
			snprintf(size,sizeof(size),"%s","&lt;DIR&gt;");
		} else {
			if (st.st_size < 1024)
				snprintf(size, sizeof(size), "%lu", (unsigned long) st.st_size);
			else if (st.st_size < 1024 * 1024)
				snprintf(size, sizeof(size), "%luk", (unsigned long) (st.st_size >> 10)  + 1);
			else
				snprintf(size, sizeof(size), "%.1fM", (float) st.st_size / 1048576);
		}
		
		ret += snprintf((!buf ? NULL : buf + ret), (!len ? 0 : len - ret),
		    "<tr><td><a href=\"%s%s%s\">%s%s</a></td>"
		    "<td>&nbsp;%s</td><td>&nbsp;&nbsp;%s</td></tr>\n",
		    uri, slash, dp->d_name, dp->d_name,
		    S_ISDIR(st.st_mode) ? "/" : "", mod, size);
	}
	ret += snprintf((!buf ? NULL : buf + ret), (!len ? 0 : len - ret),
			"</table></body></html>\n");
	closedir(dirp);

	return ret;
}

static int dir_list_local(HTTPContext *c)
{
	struct stat st;
	int ret = stat(c->url, &st);
	char *path = NULL, uri[FILENAME_MAX] = "";

	if(ret < 0 && !strcmp(c->url, "index.html")){
		path = "."; snprintf(uri, sizeof(uri), "/");
	}else if(ret == 0 && S_ISDIR(st.st_mode)){
		path = c->url; snprintf(uri, sizeof(uri), "/%s/", c->url);
	}

	if(!path){
		return 0;
	}

	char head[512], *charset = "utf-8";
	#if defined(_WIN32)
	charset = "gbk"; /*todo: detect from content.*/
	#endif
   
	int hlen;
	int len = dir2html(path, uri, NULL, 0);
	hlen = snprintf(head, sizeof(head), 
			"HTTP/1.1 200 OK\r\n"
		    "Connection: close\r\n"
		    "Content-Type: text/html; charset=%s\r\n"
			"Content-Length: %d\r\n\r\n",
			charset, len);

	c->pb_buffer = av_malloc(hlen+len);
	if(!c->pb_buffer){
        c->buffer_ptr = c->buffer;
        c->buffer_end = c->buffer;
		return 0;
	}
	memcpy(c->pb_buffer, head, hlen);
	dir2html(path, uri, c->pb_buffer+hlen, len);

	c->buffer_ptr = c->pb_buffer;
    c->buffer_end = c->pb_buffer + hlen + len;
	c->http_error = 200;
	c->local_fd = -1;
	return 1;
}

static int dir_delete_file(HTTPContext *c)
{
	struct stat st;
	int code = 200;
	int ret = stat(c->url, &st);
	if(ret < 0){
		code = 404;
	}else{
		int r2 = remove(c->url);
		code = r2 ? 400 : 200;
	}

	return code;
}

