/*
 * Pound - the reverse-proxy load-balancer
 * Copyright (C) 2002-2006 Apsis GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * Additionaly compiling, linking, and/or using OpenSSL is expressly allowed.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA  02111-1307, USA.
 *
 * Contact information:
 * Apsis GmbH
 * P.O.Box
 * 8707 Uetikon am See
 * Switzerland
 * Tel: +41-44-920 4904
 * EMail: roseg@apsis.ch
 */

#include    "pound.h"

/* HTTP error replies */
static char *h500 = "500 Internal Server Error",
            *h501 = "501 Not Implemented",
            *h503 = "503 Service Unavailable",
            *h414 = "414 Request URI too long";

static char *err_response = "HTTP/1.0 %s\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n%s";

/*
 * Reply with an error
 */
static void
err_reply(BIO *const c, const char *head, const char *txt)
{
    BIO_printf(c, err_response, head, strlen(txt), txt);
    BIO_flush(c);
    return;
}

/*
 * Reply with a redirect
 */
static void
redirect_reply(BIO *const c, const char *url)
{
    char    rep[MAXBUF], cont[MAXBUF];

    snprintf(cont, sizeof(cont),
        "<html><head><title>Redirect</title></head><body><h1>Redirect</h1><p>You should go to <a href=\"%s\">%s</a></p></body></html>",
        url, url);
    /*
     * This really should be 307, but some HTTP/1.0 clients do not understand that, so we use 302

    snprintf(rep, sizeof(rep),
        "HTTP/1.0 307 Temporary Redirect\r\nLocation: %s\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n",
        url, strlen(cont));
    */
    snprintf(rep, sizeof(rep),
        "HTTP/1.0 302 Found\r\nLocation: %s\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n",
        url, strlen(cont));
    BIO_write(c, rep, strlen(rep));
    BIO_write(c, cont, strlen(cont));
    BIO_flush(c);
    return;
}

/*
 * Read and write some binary data
 */
static int
copy_bin(BIO *const cl, BIO *const be, long cont, long *res_bytes, const int no_write)
{
    char        buf[MAXBUF];
    int         res;

    while(cont > 0L) {
        if((res = BIO_read(cl, buf, cont > MAXBUF? MAXBUF: cont)) < 0)
            return -1;
        else if(res == 0)
            return -2;
        if(!no_write)
            if(BIO_write(be, buf, res) != res)
                return -3;
        if(BIO_flush(be) != 1)
            return -4;
        cont -= res;
        if(res_bytes)
            *res_bytes += res;
    }
    return 0;
}

/*
 * Strip trailing CRLF
 */
static void
strip_eol(char *lin)
{
    while(*lin)
        if(*lin == '\n' || (*lin == '\r' && *(lin + 1) == '\n')) {
            *lin = '\0';
            break;
        } else
            lin++;
    return;
}

/*
 * Copy chunked
 */
static int
copy_chunks(BIO *const cl, BIO *const be, long *res_bytes, const int no_write, const long max_size)
{
    char        buf[MAXBUF];
    long        cont, tot_size;
    regmatch_t  matches[2];

    for(tot_size = 0L;;) {
        if(BIO_gets(cl, buf, MAXBUF) <= 0) {
            if(errno)
                logmsg(LOG_NOTICE, "unexpected chunked EOF: %s", strerror(errno));
            return -1;
        }
        strip_eol(buf);
        if(!regexec(&CHUNK_HEAD, buf, 2, matches, 0))
            cont = strtol(buf, NULL, 16);
        else {
            /* not chunk header */
            logmsg(LOG_NOTICE, "bad chunk header <%s>: %s", buf, strerror(errno));
            return -2;
        }
        if(!no_write)
            if(BIO_printf(be, "%s\r\n", buf) <= 0) {
                logmsg(LOG_NOTICE, "error write chunked: %s", strerror(errno));
                return -3;
            }

        tot_size += cont;
        if(max_size > 0L && tot_size > max_size) {
            logmsg(LOG_WARNING, "chunk content too large");
                return -4;
        }

        if(cont > 0L) {
            if(copy_bin(cl, be, cont, res_bytes, no_write)) {
                if(errno)
                    logmsg(LOG_NOTICE, "error copy chunk cont: %s", strerror(errno));
                return -4;
            }
        } else
            break;
        /* final CRLF */
        if(BIO_gets(cl, buf, MAXBUF) <= 0) {
            if(errno)
                logmsg(LOG_NOTICE, "unexpected after chunk EOF: %s", strerror(errno));
            return -5;
        }
        strip_eol(buf);
        if(buf[0])
            logmsg(LOG_NOTICE, "unexpected after chunk \"%s\"", buf);
        if(!no_write)
            if(BIO_printf(be, "%s\r\n", buf) <= 0) {
                logmsg(LOG_NOTICE, "error after chunk write: %s", strerror(errno));
                return -6;
            }
    }
    /* possibly trailing headers */
    for(;;) {
        if(BIO_gets(cl, buf, MAXBUF) <= 0) {
            if(errno)
                logmsg(LOG_NOTICE, "unexpected post-chunk EOF: %s", strerror(errno));
            return -7;
        }
        if(!no_write) {
            if(BIO_puts(be, buf) <= 0) {
                logmsg(LOG_NOTICE, "error post-chunk write: %s", strerror(errno));
                return -8;
            }
            if(BIO_flush(be) != 1) {
                logmsg(LOG_NOTICE, "copy_chunks flush error: %s", strerror(errno));
                return -4;
            }
        }
        strip_eol(buf);
        if(!buf[0])
            break;
    }
    return 0;
}

static int  err_to = -1;

/*
 * Time-out for client read/gets
 * the SSL manual says not to do it, but it works well enough anyway...
 */
static long
bio_callback(BIO *const bio, const int cmd, const char *argp, int argi, long argl, long ret)
{
    struct pollfd   p;
    int             to;

    if(cmd != BIO_CB_READ && cmd != BIO_CB_WRITE)
        return ret;

    /* a time-out already occured */
    if((to = *((int *)BIO_get_callback_arg(bio)) * 1000) < 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    for(;;) {
        memset(&p, 0, sizeof(p));
        BIO_get_fd(bio, &p.fd);
        p.events = (cmd == BIO_CB_READ)? (POLLIN | POLLPRI): POLLOUT;
        switch(poll(&p, 1, to)) {
        case 1:
            if(cmd == BIO_CB_READ) {
                if((p.revents & POLLIN) || (p.revents & POLLPRI))
                    /* there is readable data */
                    return ret;
                else
                    errno = EIO;
            } else {
                if(p.revents & POLLOUT)
                    /* data can be written */
                    return ret;
                else
                    errno = ECONNRESET;
            }
            return -1;
        case 0:
            /* timeout - mark the BIO as unusable for the future */
            BIO_set_callback_arg(bio, (char *)&err_to);
            errno = ETIMEDOUT;
            return 0;
        default:
            /* error */
            if(errno != EINTR) {
                logmsg(LOG_WARNING, "callback poll: %s", strerror(errno));
                return -2;
            }
        }
    }
}

/*
 * Check if the file underlying a BIO is readable
 */
static int
is_readable(BIO *const bio, const int to_wait)
{
    struct pollfd   p;

    if(BIO_pending(bio) > 0)
        return 1;
    memset(&p, 0, sizeof(p));
    BIO_get_fd(bio, &p.fd);
    p.events = POLLIN | POLLPRI;
    return (poll(&p, 1, to_wait * 1000) > 0);
}

static void
free_headers(char **headers)
{
    int     i;

    for(i = 0; i < MAXHEADERS; i++)
        if(headers[i])
            free(headers[i]);
    free(headers);
    return;
}

static char **
get_headers(BIO *const in, BIO *const cl, const LISTENER *lstn)
{
    char    **headers, buf[MAXBUF];
    int     res, n;

    /* HTTP/1.1 allows leading CRLF */
    while((res = BIO_gets(in, buf, MAXBUF)) > 0) {
        strip_eol(buf);
        if(buf[0])
            break;
    }

    if(res <= 0) {
        /* this is expected to occur only on client reads */
        /* logmsg(LOG_NOTICE, "headers: bad starting read"); */
        return NULL;
    } else if(res >= (MAXBUF - 1)) {
        /* check for request length limit */
        logmsg(LOG_WARNING, "headers: request URI too long");
        err_reply(cl, h414, lstn->err414);
        return NULL;
    }

    if((headers = (char **)calloc(MAXHEADERS, sizeof(char *))) == NULL) {
        logmsg(LOG_WARNING, "headers: out of memory");
        err_reply(cl, h500, lstn->err500);
        return NULL;
    }

    for(n = 0; n < MAXHEADERS; n++) {
        if((headers[n] = (char *)malloc(MAXBUF)) == NULL) {
            free_headers(headers);
            logmsg(LOG_WARNING, "header: out of memory");
            err_reply(cl, h500, lstn->err500);
            return NULL;
        }
        strncpy(headers[n], buf, MAXBUF - 1);
        if((res = BIO_gets(in, buf, MAXBUF)) <= 0) {
            free_headers(headers);
            logmsg(LOG_WARNING, "can't read header");
            err_reply(cl, h500, lstn->err500);
            return NULL;
        }
        strip_eol(buf);
        if(!buf[0])
            return headers;
    }

    free_headers(headers);
    logmsg(LOG_NOTICE, "too many headers");
    err_reply(cl, h500, lstn->err500);
    return NULL;
}

#define LOG_TIME_SIZE   32
/*
 * Apache log-file-style time format
 */
static void
log_time(char *res)
{
    time_t  now;
    struct tm   *t_now, t_res;

    now = time(NULL);
#ifdef  HAVE_LOCALTIME_R
    t_now = localtime_r(&now, &t_res);
#else
    t_now = localtime(&now);
#endif
    strftime(res, LOG_TIME_SIZE - 1, "%d/%b/%Y:%H:%M:%S %z", t_now);
    return;
}

static double
cur_time(void)
{
#ifdef  HAVE_GETTIMEOFDAY
    struct timeval  tv;
    struct timezone tz;

    gettimeofday(&tv, &tz);
    return tv.tv_sec * 1000000.0 + tv.tv_usec;
#else
    return time(NULL) * 1000000.0;
#endif
}

#define LOG_BYTES_SIZE  16
/*
 * Apache log-file-style number format
 */
static void
log_bytes(char *res, const long cnt)
{
    if(cnt > 0L)
        snprintf(res, LOG_BYTES_SIZE - 1, "%ld", cnt);
    else
        strcpy(res, "-");
    return;
}

/* Cleanup code. This should really be in the pthread_cleanup_push, except for bugs in some implementations */
#define clean_all() {   \
    if(ssl != NULL) { BIO_ssl_shutdown(cl); BIO_ssl_shutdown(cl); BIO_ssl_shutdown(cl); } \
    if(be != NULL) { BIO_flush(be); BIO_reset(be); BIO_free_all(be); be = NULL; } \
    if(cl != NULL) { BIO_flush(cl); BIO_reset(cl); BIO_free_all(cl); cl = NULL; } \
    if(x509 != NULL) { X509_free(x509); x509 = NULL; } \
    if(ssl != NULL) { ERR_clear_error(); ERR_remove_state(0); } \
}

/*
 * handle an HTTP request
 */
void *
thr_http(void *arg)
{
    int                 cl_11, be_11, res, chunked, n, sock, no_cont, skip, conn_closed, redir,
                        force_10;
    LISTENER            *lstn;
    SERVICE             *svc;
    BACKEND             *backend, *cur_backend;
    struct in_addr      from_host;
    BIO                 *cl, *be, *bb, *b64;
    X509                *x509;
    char                request[MAXBUF], response[MAXBUF], buf[MAXBUF], url[MAXBUF], loc_path[MAXBUF], **headers,
                        headers_ok[MAXHEADERS], v_host[MAXBUF], referer[MAXBUF], u_agent[MAXBUF], u_name[MAXBUF],
                        caddr[MAXBUF], req_time[LOG_TIME_SIZE], s_res_bytes[LOG_BYTES_SIZE], *mh;
    SSL                 *ssl;
    long                cont, res_bytes;
    struct sockaddr_in  *srv;
    regmatch_t          matches[4];
    struct linger       l;
    double              start_req, end_req;

    from_host = ((thr_arg *)arg)->from_host;
    lstn = ((thr_arg *)arg)->lstn;
    sock = ((thr_arg *)arg)->sock;
    free(arg);

    n = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&n, sizeof(n));
    l.l_onoff = 1;
    l.l_linger = 10;
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *)&l, sizeof(l));

#ifdef  TCP_LINGER2
    n = 5;
    setsockopt(sock, SOL_TCP, TCP_LINGER2, (void *)&n, sizeof(n));
#endif

    cl = NULL;
    be = NULL;
    ssl = NULL;
    x509 = NULL;

    if((cl = BIO_new_socket(sock, 1)) == NULL) {
        logmsg(LOG_WARNING, "BIO_new_socket failed");
        shutdown(sock, 2);
        close(sock);
        pthread_exit(NULL);
    }
    if(lstn->to > 0) {
        BIO_set_callback_arg(cl, (char *)&lstn->to);
        BIO_set_callback(cl, bio_callback);
    }

    if(lstn->ctx != NULL) {
        if((ssl = SSL_new(lstn->ctx)) == NULL) {
            logmsg(LOG_WARNING, "SSL_new: failed");
            BIO_reset(cl);
            BIO_free_all(cl);
            pthread_exit(NULL);
        }
        SSL_set_bio(ssl, cl, cl);
        if((bb = BIO_new(BIO_f_ssl())) == NULL) {
            logmsg(LOG_WARNING, "BIO_new(Bio_f_ssl()) failed");
            BIO_reset(cl);
            BIO_free_all(cl);
            pthread_exit(NULL);
        }
        BIO_set_ssl(bb, ssl, BIO_CLOSE);
        BIO_set_ssl_mode(bb, 0);
        cl = bb;
        if(BIO_do_handshake(cl) <= 0) {
            /* no need to log every client without a certificate...
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_NOTICE, "BIO_do_handshake with %s failed: %s", caddr,
                ERR_error_string(ERR_get_error(), NULL));
            x509 = NULL;
            */
            BIO_reset(cl);
            BIO_free_all(cl);
            pthread_exit(NULL);
        } else {
            if((x509 = SSL_get_peer_certificate(ssl)) != NULL && lstn->clnt_check < 3
            && SSL_get_verify_result(ssl) != X509_V_OK) {
                addr2str(caddr, MAXBUF - 1, &from_host);
                logmsg(LOG_NOTICE, "Bad certificate from %s", caddr);
                BIO_reset(cl);
                BIO_free_all(cl);
                pthread_exit(NULL);
            }
        }
    } else {
        x509 = NULL;
    }
    cur_backend = NULL;

    if((bb = BIO_new(BIO_f_buffer())) == NULL) {
        logmsg(LOG_WARNING, "BIO_new(buffer) failed");
        BIO_reset(cl);
        BIO_free_all(cl);
        pthread_exit(NULL);
    }
    BIO_set_close(cl, BIO_CLOSE);
    BIO_set_buffer_size(cl, MAXBUF);
    cl = BIO_push(bb, cl);

    for(cl_11 = be_11 = 0;;) {
        res_bytes = 0L;
        v_host[0] = referer[0] = u_agent[0] = u_name[0] = '\0';
        conn_closed = 0;
        for(n = 0; n < MAXHEADERS; n++)
            headers_ok[n] = 1;
        if((headers = get_headers(cl, cl, lstn)) == NULL) {
            if(!cl_11) {
                if(errno) {
                    addr2str(caddr, MAXBUF - 1, &from_host);
                    logmsg(LOG_NOTICE, "error read from %s: %s", caddr, strerror(errno));
                    /* err_reply(cl, h500, lstn->err500); */
                }
            }
            clean_all();
            pthread_exit(NULL);
        }
        memset(req_time, 0, LOG_TIME_SIZE);
        start_req = cur_time();
        log_time(req_time);

        /* check for correct request */
        strncpy(request, headers[0], MAXBUF);
        if(!regexec(&lstn->verb, request, 3, matches, 0)) {
            no_cont = !strncasecmp(request + matches[1].rm_so, "HEAD", matches[1].rm_eo - matches[1].rm_so);
        } else {
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_WARNING, "bad request \"%s\" from %s", request, caddr);
            err_reply(cl, h501, lstn->err501);
            free_headers(headers);
            clean_all();
            pthread_exit(NULL);
        }
        cl_11 = (request[strlen(request) - 1] == '1');
        strncpy(url, request + matches[2].rm_so, matches[2].rm_eo - matches[2].rm_so);
        url[matches[2].rm_eo - matches[2].rm_so] = '\0';
        if(regexec(&lstn->url_pat,  url, 0, NULL, 0)) {
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_NOTICE, "bad URL \"%s\" from %s", url, caddr);
            err_reply(cl, h501, lstn->err501);
            free_headers(headers);
            clean_all();
            pthread_exit(NULL);
        }

        /* check other headers */
        for(chunked = 0, cont = 0L, n = 1; n < MAXHEADERS && headers[n]; n++) {
            /* no overflow - see check_header for details */
            switch(check_header(headers[n], buf)) {
            case HEADER_HOST:
                strcpy(v_host, buf);
                break;
            case HEADER_REFERER:
                strcpy(referer, buf);
                break;
            case HEADER_USER_AGENT:
                strcpy(u_agent, buf);
                break;
            case HEADER_CONNECTION:
                if(!strcasecmp("close", buf))
                    conn_closed = 1;
                break;
            case HEADER_TRANSFER_ENCODING:
                if(cont != 0L)
                    headers_ok[n] = 0;
                else if(!strcasecmp("chunked", buf))
                    if(chunked)
                        headers_ok[n] = 0;
                    else
                        chunked = 1;
                break;
            case HEADER_CONTENT_LENGTH:
                if(chunked)
                    headers_ok[n] = 0;
                else
                    cont = atol(buf);
                break;
            case HEADER_ILLEGAL:
                if(log_level > 0) {
                    addr2str(caddr, MAXBUF - 1, &from_host);
                    logmsg(LOG_NOTICE, "bad header from %s (%s)", caddr, headers[n]);
                }
                headers_ok[n] = 0;
                break;
            }
            if(headers_ok[n] && lstn->head_off) {
                /* maybe header to be removed */
                MATCHER *m;

                for(m = lstn->head_off; m; m = m->next)
                    headers_ok[n] = regexec(&m->pat, headers[n], 0, NULL, 0);
            }
            /* get User name */
            if(!regexec(&AUTHORIZATION, headers[n], 2, matches, 0)) {
                int inlen;

                if((bb = BIO_new(BIO_s_mem())) == NULL) {
                    logmsg(LOG_WARNING, "Can't alloc BIO_s_mem");
                    continue;
                }
                if((b64 = BIO_new(BIO_f_base64())) == NULL) {
                    logmsg(LOG_WARNING, "Can't alloc BIO_f_base64");
                    BIO_free(bb);
                    continue;
                }
                b64 = BIO_push(b64, bb);
                BIO_write(bb, headers[n] + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
                BIO_write(bb, "\n", 1);
                if((inlen = BIO_read(b64, buf, MAXBUF - 1)) <= 0) {
                    logmsg(LOG_WARNING, "Can't read BIO_f_base64");
                    BIO_free_all(b64);
                    continue;
                }
                BIO_free_all(b64);
                if((mh = strchr(buf, ':')) == NULL) {
                    logmsg(LOG_WARNING, "Unknown authentication");
                    continue;
                }
                *mh = '\0';
                strcpy(u_name, buf);
            }
        }

        /* possibly limited request size */
        if(lstn->max_req > 0L && cont > 0L && cont > lstn->max_req) {
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_NOTICE, "request too large (%ld) from %s", cont, caddr);
            err_reply(cl, h501, lstn->err501);
            free_headers(headers);
            clean_all();
            pthread_exit(NULL);
        }

        if(be != NULL) {
            if(is_readable(be, 0)) {
                /* The only way it's readable is if it's at EOF, so close it! */
                BIO_reset(be);
                BIO_free_all(be);
                be = NULL;
            }
        }

        /* check that the requested URL still fits the old back-end (if any) */
        if((svc = get_service(lstn, url, &headers[1])) == NULL) {
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_NOTICE, "no service \"%s\" from %s", request, caddr);
            err_reply(cl, h503, lstn->err503);
            free_headers(headers);
            clean_all();
            pthread_exit(NULL);
        }
        if((backend = get_backend(svc, &from_host, url, &headers[1])) == NULL) {
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_NOTICE, "no back-end \"%s\" from %s", request, caddr);
            err_reply(cl, h503, lstn->err503);
            free_headers(headers);
            clean_all();
            pthread_exit(NULL);
        }

        if(be != NULL && backend != cur_backend) {
            BIO_reset(be);
            BIO_free_all(be);
            be = NULL;
        }
        while(be == NULL && backend->be_type == BACK_END) {
            if(backend->domain == PF_UNIX) {
                if((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
                    logmsg(LOG_WARNING, "backend %s create: %s", backend->addr.un.sun_path, strerror(errno));
                    err_reply(cl, h503, lstn->err503);
                    free_headers(headers);
                    clean_all();
                    pthread_exit(NULL);
                }
                if(connect_nb(sock, (struct sockaddr *)&backend->addr.un, (socklen_t)sizeof(backend->addr.un), backend->to) < 0) {
                    logmsg(LOG_WARNING, "backend %s connect: %s", backend->addr.un.sun_path, strerror(errno));
                    close(sock);
                    kill_be(svc, backend);
                    if((backend = get_backend(svc, &from_host, url, &headers[1])) == NULL) {
                        addr2str(caddr, MAXBUF - 1, &from_host);
                        logmsg(LOG_NOTICE, "no back-end \"%s\" from %s", request, caddr);
                        err_reply(cl, h503, lstn->err503);
                        free_headers(headers);
                        clean_all();
                        pthread_exit(NULL);
                    }
                    continue;
                }
            } else {
                if((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
                    addr2str(caddr, MAXBUF - 1, &backend->addr.in.sin_addr);
                    logmsg(LOG_WARNING, "backend %s:%hd create: %s",
                        caddr, ntohs(backend->addr.in.sin_port), strerror(errno));
                    err_reply(cl, h503, lstn->err503);
                    free_headers(headers);
                    clean_all();
                    pthread_exit(NULL);
                }
                if(connect_nb(sock, (struct sockaddr *)&backend->addr.in, (socklen_t)sizeof(backend->addr.in), backend->to) < 0) {
                    addr2str(caddr, MAXBUF - 1, &backend->addr.in.sin_addr);
                    logmsg(LOG_WARNING, "backend %s:%hd connect: %s",
                        caddr, ntohs(backend->addr.in.sin_port), strerror(errno));
                    close(sock);
                    kill_be(svc, backend);
                    if((backend = get_backend(svc, &from_host, url, &headers[1])) == NULL) {
                        addr2str(caddr, MAXBUF - 1, &from_host);
                        logmsg(LOG_NOTICE, "no back-end \"%s\" from %s", request, caddr);
                        err_reply(cl, h503, lstn->err503);
                        free_headers(headers);
                        clean_all();
                        pthread_exit(NULL);
                    }
                    continue;
                }
                n = 1;
                setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&n, sizeof(n));
                l.l_onoff = 1;
                l.l_linger = 10;
                setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *)&l, sizeof(l));
            }
            if((be = BIO_new_socket(sock, 1)) == NULL) {
                logmsg(LOG_WARNING, "BIO_new_socket server failed");
                shutdown(sock, 2);
                close(sock);
                err_reply(cl, h503, lstn->err503);
                free_headers(headers);
                clean_all();
                pthread_exit(NULL);
            }
            BIO_set_close(be, BIO_CLOSE);
            if(backend->to > 0) {
                BIO_set_callback_arg(be, (char *)&backend->to);
                BIO_set_callback(be, bio_callback);
            }
            if((bb = BIO_new(BIO_f_buffer())) == NULL) {
                logmsg(LOG_WARNING, "BIO_new(buffer) server failed");
                err_reply(cl, h503, lstn->err503);
                free_headers(headers);
                clean_all();
                pthread_exit(NULL);
            }
            BIO_set_buffer_size(bb, MAXBUF);
            BIO_set_close(bb, BIO_CLOSE);
            be = BIO_push(bb, be);
        }
        cur_backend = backend;

        /* if we have anything but a BACK_END we close the channel */
        if(be != NULL && cur_backend->be_type != BACK_END) {
            BIO_reset(be);
            BIO_free_all(be);
            be = NULL;
        }

        /* send the request */
        if(cur_backend->be_type == BACK_END)
            for(n = 0; n < MAXHEADERS && headers[n]; n++) {
                if(!headers_ok[n])
                    continue;
                /* this is the earliest we can check for Destination - we had no back-end before */
                if(lstn->rewr_dest && check_header(headers[n], buf) == HEADER_DESTINATION) {
                    if(regexec(&LOCATION, buf, 4, matches, 0)) {
                        logmsg(LOG_NOTICE, "Can't parse Destination %s", buf);
                        break;
                    }
                    str_be(caddr, MAXBUF - 1, cur_backend);
                    strcpy(loc_path, buf + matches[3].rm_so);
                    snprintf(buf, MAXBUF, "Destination: http://%s/%s", caddr, loc_path);
                    free(headers[n]);
                    if((headers[n] = strdup(buf)) == NULL) {
                        logmsg(LOG_WARNING, "rewrite Destination - out of memory: %s", strerror(errno));
                        free_headers(headers);
                        clean_all();
                        pthread_exit(NULL);
                    }
                }
                if(BIO_printf(be, "%s\r\n", headers[n]) <= 0) {
                    str_be(buf, MAXBUF - 1, cur_backend);
                    logmsg(LOG_WARNING, "error write to %s: %s", buf, strerror(errno));
                    err_reply(cl, h500, lstn->err500);
                    free_headers(headers);
                    clean_all();
                    pthread_exit(NULL);
                }
            }
        free_headers(headers);

        /* if SSL put additional headers for client certificate */
        if(cur_backend->be_type == BACK_END && ssl != NULL) {
            SSL_CIPHER  *cipher;

            if(lstn->ssl_head != NULL)
                if(BIO_printf(be, "%s\r\n", lstn->ssl_head) <= 0) {
                    str_be(buf, MAXBUF - 1, cur_backend);
                    logmsg(LOG_WARNING, "error write HTTPSHeader to %s: %s", buf, strerror(errno));
                    err_reply(cl, h500, lstn->err500);
                    clean_all();
                    pthread_exit(NULL);
                }
            if(lstn->clnt_check > 0 && x509 != NULL && (bb = BIO_new(BIO_s_mem())) != NULL) {
                X509_NAME_print_ex(bb, X509_get_subject_name(x509), 8, XN_FLAG_ONELINE & ~ASN1_STRFLGS_ESC_MSB);
                BIO_gets(bb, buf, MAXBUF);
                if(BIO_printf(be, "X-SSL-Subject: %s\r\n", buf) <= 0) {
                    str_be(buf, MAXBUF - 1, cur_backend);
                    logmsg(LOG_WARNING, "error write X-SSL-Subject to %s: %s", buf, strerror(errno));
                    err_reply(cl, h500, lstn->err500);
                    BIO_free_all(bb);
                    clean_all();
                    pthread_exit(NULL);
                }

                X509_NAME_print_ex(bb, X509_get_issuer_name(x509), 8, XN_FLAG_ONELINE & ~ASN1_STRFLGS_ESC_MSB);
                BIO_gets(bb, buf, MAXBUF);
                if(BIO_printf(be, "X-SSL-Issuer: %s\r\n", buf) <= 0) {
                    str_be(buf, MAXBUF - 1, cur_backend);
                    logmsg(LOG_WARNING, "error write X-SSL-Issuer to %s: %s", buf, strerror(errno));
                    err_reply(cl, h500, lstn->err500);
                    BIO_free_all(bb);
                    clean_all();
                    pthread_exit(NULL);
                }

                ASN1_TIME_print(bb, X509_get_notBefore(x509));
                BIO_gets(bb, buf, MAXBUF);
                if(BIO_printf(be, "X-SSL-notBefore: %s\r\n", buf) <= 0) {
                    str_be(buf, MAXBUF - 1, cur_backend);
                    logmsg(LOG_WARNING, "error write X-SSL-notBefore to %s: %s", buf, strerror(errno));
                    err_reply(cl, h500, lstn->err500);
                    BIO_free_all(bb);
                    clean_all();
                    pthread_exit(NULL);
                }

                ASN1_TIME_print(bb, X509_get_notAfter(x509));
                BIO_gets(bb, buf, MAXBUF);
                if(BIO_printf(be, "X-SSL-notAfter: %s\r\n", buf) <= 0) {
                    str_be(buf, MAXBUF - 1, cur_backend);
                    logmsg(LOG_WARNING, "error write X-SSL-notAfter to %s: %s", buf, strerror(errno));
                    err_reply(cl, h500, lstn->err500);
                    BIO_free_all(bb);
                    clean_all();
                    pthread_exit(NULL);
                }
                if(BIO_printf(be, "X-SSL-serial: %ld\r\n", ASN1_INTEGER_get(X509_get_serialNumber(x509))) <= 0) {
                    str_be(buf, MAXBUF - 1, cur_backend);
                    logmsg(LOG_WARNING, "error write X-SSL-serial to %s: %s", buf, strerror(errno));
                    err_reply(cl, h500, lstn->err500);
                    BIO_free_all(bb);
                    clean_all();
                    pthread_exit(NULL);
                }
                PEM_write_bio_X509(bb, x509);
                BIO_gets(bb, buf, MAXBUF);
                strip_eol(buf);
                if(BIO_printf(be, "X-SSL-certificate: %s\r\n", buf) <= 0) {
                    str_be(buf, MAXBUF - 1, cur_backend);
                    logmsg(LOG_WARNING, "error write X-SSL-certificate to %s: %s", buf, strerror(errno));
                    err_reply(cl, h500, lstn->err500);
                    BIO_free_all(bb);
                    clean_all();
                    pthread_exit(NULL);
                }
                while(BIO_gets(bb, buf, MAXBUF) > 0) {
                    strip_eol(buf);
                    if(BIO_printf(be, "\t%s\r\n", buf) <= 0) {
                        str_be(buf, MAXBUF - 1, cur_backend);
                        logmsg(LOG_WARNING, "error write X-SSL-certificate to %s: %s", buf, strerror(errno));
                        err_reply(cl, h500, lstn->err500);
                        BIO_free_all(bb);
                        clean_all();
                        pthread_exit(NULL);
                    }
                }
                if((cipher = SSL_get_current_cipher(ssl)) != NULL) {
                    SSL_CIPHER_description(cipher, buf, MAXBUF);
                    strip_eol(buf);
                    if(BIO_printf(be, "X-SSL-cipher: %s\r\n", buf) <= 0) {
                        str_be(buf, MAXBUF - 1, cur_backend);
                        logmsg(LOG_WARNING, "error write X-SSL-cipher to %s: %s", buf, strerror(errno));
                        err_reply(cl, h500, lstn->err500);
                        clean_all();
                        pthread_exit(NULL);
                    }
                }
                BIO_free_all(bb);
            }
        }
        /* put additional client IP header */
        if(cur_backend->be_type == BACK_END) {
            addr2str(caddr, MAXBUF - 1, &from_host);
            BIO_printf(be, "X-Forwarded-For: %s\r\n", caddr);

            /* final CRLF */
            BIO_puts(be, "\r\n");
        }

        if(cl_11 && chunked) {
            /* had Transfer-encoding: chunked so read/write all the chunks (HTTP/1.1 only) */
            if(copy_chunks(cl, be, NULL, cur_backend->be_type != BACK_END, lstn->max_req)) {
                err_reply(cl, h500, lstn->err500);
                clean_all();
                pthread_exit(NULL);
            }
        } else if(cont > 0L) {
            /* had Content-length, so do raw reads/writes for the length */
            if(copy_bin(cl, be, cont, NULL, cur_backend->be_type != BACK_END)) {
                logmsg(LOG_NOTICE, "error copy client cont: %s", strerror(errno));
                err_reply(cl, h500, lstn->err500);
                clean_all();
                pthread_exit(NULL);
            }
        }

        /* flush to the back-end */
        if(cur_backend->be_type == BACK_END && BIO_flush(be) != 1) {
            str_be(buf, MAXBUF - 1, cur_backend);
            logmsg(LOG_NOTICE, "error flush to %s: %s", buf, strerror(errno));
            err_reply(cl, h500, lstn->err500);
            clean_all();
            pthread_exit(NULL);
        }

        /*
         * check on no_https_11:
         *  - if 0 ignore
         *  - if 1 and SSL force HTTP/1.0
         *  - if 2 and SSL and MSIE force HTTP/1.0
         */
        switch(lstn->noHTTPS11) {
        case 1:
            force_10 = (ssl != NULL);
            break;
        case 2:
            force_10 = (ssl != NULL && strstr(u_agent, "MSIE") != NULL);
            break;
        default:
            force_10 = 0;
            break;
        }

        /* if we have a redirector */
        if(cur_backend->be_type == REDIRECTOR) {
            memset(buf, 0, sizeof(buf));
            if(cur_backend->redir_req)
                snprintf(buf, sizeof(buf) - 1, "%s%s", cur_backend->url, url);
            else 
                strncpy(buf, cur_backend->url, sizeof(buf) - 1);
            redirect_reply(cl, buf);
            switch(log_level) {
            case 0:
                break;
            case 1:
            case 2:
                addr2str(caddr, MAXBUF - 1, &from_host);
                logmsg(LOG_INFO, "%s %s - REDIRECT %s", caddr, request, buf);
                break;
            case 3:
                addr2str(caddr, MAXBUF - 1, &from_host);
                if(v_host[0])
                    logmsg(LOG_INFO, "%s %s - %s [%s] \"%s\" 302 0 \"%s\" \"%s\"", v_host, caddr,
                        u_name[0]? u_name: "-", req_time, request, referer, u_agent);
                else
                    logmsg(LOG_INFO, "%s - %s [%s] \"%s\" 302 0 \"%s\" \"%s\"", caddr,
                        u_name[0]? u_name: "-", req_time, request, referer, u_agent);
                break;
            case 4:
                addr2str(caddr, MAXBUF - 1, &from_host);
                logmsg(LOG_INFO, "%s - %s [%s] \"%s\" 302 0 \"%s\" \"%s\"", caddr,
                    u_name[0]? u_name: "-", req_time, request, referer, u_agent);
                break;
            }
            if(!cl_11 || conn_closed || force_10)
                break;
            continue;
        }

        /* get the response */
        for(skip = 1; skip;) {
            if((headers = get_headers(be, cl, lstn)) == NULL) {
                str_be(buf, MAXBUF - 1, cur_backend);
                logmsg(LOG_NOTICE, "response error read from %s: %s", buf, strerror(errno));
                err_reply(cl, h500, lstn->err500);
                clean_all();
                pthread_exit(NULL);
            }

            strncpy(response, headers[0], MAXBUF);
            be_11 = (response[7] == '1');
            /* responses with code 100 are never passed back to the client */
            skip = !regexec(&RESP_SKIP, response, 0, NULL, 0);
            /* some response codes (1xx, 204, 304) have no content */
            if(!no_cont && !regexec(&RESP_IGN, response, 0, NULL, 0))
                no_cont = 1;
            /* check for redirection */
            /* redir = !regexec(&RESP_REDIR, response, 0, NULL, 0); */

            for(chunked = 0, cont = -1L, n = 1; n < MAXHEADERS && headers[n]; n++) {
                switch(check_header(headers[n], buf)) {
                case HEADER_CONNECTION:
                    if(!strcasecmp("close", buf))
                        conn_closed = 1;
                    break;
                case HEADER_TRANSFER_ENCODING:
                    if(!strcasecmp("chunked", buf)) {
                        chunked = 1;
                        no_cont = 0;
                    }
                    break;
                case HEADER_CONTENT_LENGTH:
                    cont = atol(buf);
                    break;
                case HEADER_LOCATION:
                    if(v_host[0] && need_rewrite(lstn->rewr_loc, buf, loc_path, lstn, cur_backend)) {
                        snprintf(buf, MAXBUF, "Location: %s://%s/%s",
                            (ssl == NULL? "http": "https"), v_host, loc_path);
                        free(headers[n]);
                        if((headers[n] = strdup(buf)) == NULL) {
                            logmsg(LOG_WARNING, "rewrite Location - out of memory: %s", strerror(errno));
                            free_headers(headers);
                            clean_all();
                            pthread_exit(NULL);
                        }
                    }
                    break;
                case HEADER_CONTLOCATION:
                    if(v_host[0] && need_rewrite(lstn->rewr_loc, buf, loc_path, lstn, cur_backend)) {
                        snprintf(buf, MAXBUF, "Content-location: %s://%s/%s",
                            (ssl == NULL? "http": "https"), v_host, loc_path);
                        free(headers[n]);
                        if((headers[n] = strdup(buf)) == NULL) {
                            logmsg(LOG_WARNING, "rewrite Content-location - out of memory: %s", strerror(errno));
                            free_headers(headers);
                            clean_all();
                            pthread_exit(NULL);
                        }
                    }
                    break;
                }
            }

            /* possibly record session information (only for cookies/header) */
            upd_session(svc, &headers[1], cur_backend);

            /* send the response */
            if(!skip)
                for(n = 0; n < MAXHEADERS && headers[n]; n++) {
                    if(BIO_printf(cl, "%s\r\n", headers[n]) <= 0) {
                        if(errno) {
                            addr2str(caddr, MAXBUF - 1, &from_host);
                            logmsg(LOG_NOTICE, "error write to %s: %s", caddr, strerror(errno));
                        }
                        free_headers(headers);
                        clean_all();
                        pthread_exit(NULL);
                    }
                }
            free_headers(headers);

            /* final CRLF */
            if(!skip)
                BIO_puts(cl, "\r\n");
            if(BIO_flush(cl) != 1) {
                if(errno) {
                    addr2str(caddr, MAXBUF - 1, &from_host);
                    logmsg(LOG_NOTICE, "error flush headers to %s: %s", caddr, strerror(errno));
                }
                clean_all();
                pthread_exit(NULL);
            }

            if(!no_cont) {
                /* ignore this if request was HEAD or similar */
                if(be_11 && chunked) {
                    /* had Transfer-encoding: chunked so read/write all the chunks (HTTP/1.1 only) */
                    if(copy_chunks(be, cl, &res_bytes, skip, 0L)) {
                        /* copy_chunks() has its own error messages */
                        clean_all();
                        pthread_exit(NULL);
                    }
                } else if(cont >= 0L) {
                    /* may have had Content-length, so do raw reads/writes for the length */
                    if(copy_bin(be, cl, cont, &res_bytes, skip)) {
                        if(errno)
                            logmsg(LOG_NOTICE, "error copy server cont: %s", strerror(errno));
                        clean_all();
                        pthread_exit(NULL);
                    }
                } else if(!skip) {
                    if(is_readable(be, cur_backend->to)) {
                        char    one;
                        BIO     *be_unbuf;
                        /*
                         * old-style response - content until EOF
                         * also implies the client may not use HTTP/1.1
                         */
                        cl_11 = be_11 = 0;

                        /*
                         * first read whatever is already in the input buffer
                         */
                        while(BIO_pending(be)) {
                            if(BIO_read(be, &one, 1) != 1) {
                                logmsg(LOG_NOTICE, "error read response pending: %s", strerror(errno));
                                clean_all();
                                pthread_exit(NULL);
                            }
                            if(BIO_write(cl, &one, 1) != 1) {
                                if(errno)
                                    logmsg(LOG_NOTICE, "error write response pending: %s", strerror(errno));
                                clean_all();
                                pthread_exit(NULL);
                            }
                            res_bytes++;
                        }
                        BIO_flush(cl);

                        /*
                         * find the socket BIO in the chain
                         */
                        if((be_unbuf = BIO_find_type(be, BIO_TYPE_SOCKET)) == NULL) {
                            logmsg(LOG_WARNING, "error get unbuffered: %s", strerror(errno));
                            clean_all();
                            pthread_exit(NULL);
                        }

                        /*
                         * copy till EOF
                         */
                        while((res = BIO_read(be_unbuf, buf, MAXBUF)) > 0) {
                            if(BIO_write(cl, buf, res) != res) {
                                if(errno)
                                    logmsg(LOG_NOTICE, "error copy response body: %s", strerror(errno));
                                clean_all();
                                pthread_exit(NULL);
                            } else {
                                res_bytes += res;
                                BIO_flush(cl);
                            }
                        }
                    }
                }
                if(BIO_flush(cl) != 1) {
                    if(errno) {
                        addr2str(caddr, MAXBUF - 1, &from_host);
                        logmsg(LOG_NOTICE, "error final flush to %s: %s", caddr, strerror(errno));
                    }
                    clean_all();
                    pthread_exit(NULL);
                }
            }
        }
        end_req = cur_time();
        upd_be(cur_backend, end_req - start_req);

        /* log what happened */
        strip_eol(request);
        strip_eol(response);
        memset(s_res_bytes, 0, LOG_BYTES_SIZE);
        log_bytes(s_res_bytes, res_bytes);
        switch(log_level) {
        case 0:
            break;
        case 1:
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_INFO, "%s %s - %s", caddr, request, response);
            break;
        case 2:
            str_be(buf, MAXBUF - 1, cur_backend);
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_INFO, "%s %s - %s (%s) %.3f sec", caddr, request, response, buf,
                (end_req - start_req) / 1000000.0);
            break;
        case 3:
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_INFO, "%s %s - %s [%s] \"%s\" %c%c%c %s \"%s\" \"%s\"", v_host[0]? v_host: "-",
                caddr, u_name[0]? u_name: "-", req_time, request, response[9],
                response[10], response[11], s_res_bytes, referer, u_agent);
            break;
        case 4:
            addr2str(caddr, MAXBUF - 1, &from_host);
            logmsg(LOG_INFO, "%s - %s [%s] \"%s\" %c%c%c %s \"%s\" \"%s\"", caddr,
                u_name[0]? u_name: "-", req_time, request, response[9], response[10], response[11],
                s_res_bytes, referer, u_agent);
            break;
        }

        if(!be_11) {
            BIO_reset(be);
            BIO_free_all(be);
            be = NULL;
        }
        /*
         * Stop processing if:
         *  - client is not HTTP/1.1
         *      or
         *  - we had a "Connection: closed" header
         *      or
         *  - this is an SSL connection and we had a NoHTTPS11 directive
         */
        if(!cl_11 || conn_closed || force_10)
            break;
    }

    /*
     * This may help with some versions of IE with a broken channel shutdown
     */
    if(ssl != NULL)
        SSL_set_shutdown(ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);

    clean_all();
    pthread_exit(NULL);
}
