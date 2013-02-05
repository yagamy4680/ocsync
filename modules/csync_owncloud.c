/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2011      by Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2012      by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software = NULL, you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation = NULL, either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY = NULL, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program = NULL, if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "config.h"
#ifdef NEON_WITH_LFS /* Switch on LFS in libneon. Never remove the NE_LFS! */
#define NE_LFS
#endif

#include <neon/ne_basic.h>
#include <neon/ne_socket.h>
#include <neon/ne_session.h>
#include <neon/ne_request.h>
#include <neon/ne_props.h>
#include <neon/ne_auth.h>
#include <neon/ne_dates.h>
#include <neon/ne_compress.h>
#include <neon/ne_redirect.h>

#include "c_lib.h"
#include "csync.h"
#include "csync_misc.h"
#include "csync_macros.h"
#include "c_private.h"

#include "vio/csync_vio_module.h"
#include "vio/csync_vio_file_stat.h"
#include "vio/csync_vio.h"

#include "csync_log.h"

#define DEBUG_WEBDAV(...) csync_log( dav_session.csync_ctx, 9, "oc_module", __VA_ARGS__);

#define OC_TIMEDELTA_FAIL (NE_REDIRECT +1)
#define OC_PROPFIND_FAIL  (NE_REDIRECT +2)


enum resource_type {
    resr_normal = 0,
    resr_collection,
    resr_reference,
    resr_error
};

#define DAV_STRTOL strtoll

/* Struct to store data for each resource found during an opendir operation.
 * It represents a single file entry.
 */

typedef struct resource {
    char *uri;           /* The complete uri */
    char *name;          /* The filename only */

    enum resource_type type;
    off_t              size;
    time_t             modtime;
    char*              md5;

    struct resource    *next;
} resource;

/* Struct to hold the context of a WebDAV PropFind operation to fetch
 * a directory listing from the server.
 */
struct listdir_context {
    struct resource *list;           /* The list of result resources */
    struct resource *currResource;   /* A pointer to the current resource */
    char            *target;        /* Request-URI of the PROPFIND */
    unsigned int     result_count;   /* number of elements stored in list */
    int ref; /* reference count, only destroy when it reaches 0 */
};

/*
 * free the fetchCtx
 */
static void free_fetchCtx( struct listdir_context *ctx )
{
    struct resource *newres, *res;
    if( ! ctx ) return;
    newres = ctx->list;
    res = newres;

    ctx->ref--;
    if (ctx->ref > 0) return;

    SAFE_FREE(ctx->target);

    while( res ) {
        SAFE_FREE(res->uri);
        SAFE_FREE(res->name);
        SAFE_FREE(res->md5);

        newres = res->next;
        SAFE_FREE(res);
        res = newres;
    }
    SAFE_FREE(ctx);
}


/*
 * context to store info about a temp file for GET and PUT requests
 * which store the data in a local file to save memory and secure the
 * transmission.
 */
struct transfer_context {
    ne_request *req;            /* the neon request */
    int         fd;             /* file descriptor of the file to read or write from */
    const char  *method;        /* the HTTP method, either PUT or GET  */
    ne_decompress *decompress;  /* the decompress context */
    char        *clean_uri;
};

/* Struct with the WebDAV session */
struct dav_session_s {
    ne_session *ctx;
    char *user;
    char *pwd;

    char *proxy_type;
    char *proxy_host;
    int   proxy_port;
    char *proxy_user;
    char *proxy_pwd;

    char *session_key;

    char *error_string;

    int read_timeout;

    long int prev_delta;
    long int time_delta;     /* The time delta to use.                  */
    long int time_delta_sum; /* What is the time delta average?         */
    long int time_delta_cnt; /* How often was the server time gathered? */

    CSYNC *csync_ctx;
    void *userdata;
};

/* The list of properties that is fetched in PropFind on a collection */
static const ne_propname ls_props[] = {
    { "DAV:", "getlastmodified" },
    { "DAV:", "getcontentlength" },
    { "DAV:", "resourcetype" },
    { "DAV:", "getetag"},
    { NULL, NULL }
};

/*
 * local variables.
 */

struct dav_session_s dav_session; /* The DAV Session, initialised in dav_connect */
int _connected = 0;                   /* flag to indicate if a connection exists, ie.
                                     the dav_session is valid */

csync_auth_callback _authcb;
csync_progress_callback _progresscb;

struct listdir_context *propfind_cache = 0;
csync_vio_file_stat_t _stat_cache;
/* id cache, cache the ETag: header of a GET request */
struct { char *uri; char *id;  } _id_cache = { NULL, NULL };

static void clean_caches() {
    free_fetchCtx(propfind_cache);
    propfind_cache = NULL;

    SAFE_FREE(_stat_cache.name);
    SAFE_FREE(_stat_cache.md5 );

    SAFE_FREE(_id_cache.uri);
    SAFE_FREE(_id_cache.id);
}



#define PUT_BUFFER_SIZE 1024*5

char _buffer[PUT_BUFFER_SIZE];

/* ***************************************************************************** */
static void set_error_message( const char *msg )
{
    SAFE_FREE(dav_session.error_string);
    if( msg )
        dav_session.error_string = c_strdup(msg);
}

static void set_errno_from_http_errcode( int err ) {
    int new_errno = 0;

    switch(err) {
    case 200:           /* OK */
    case 201:           /* Created */
    case 202:           /* Accepted */
    case 203:           /* Non-Authoritative Information */
    case 204:           /* No Content */
    case 205:           /* Reset Content */
    case 207:           /* Multi-Status */
    case 304:           /* Not Modified */
        new_errno = 0;
        break;
    case 401:           /* Unauthorized */
    case 402:           /* Payment Required */
    case 407:           /* Proxy Authentication Required */
    case 405:
        new_errno = EPERM;
        break;
    case 301:           /* Moved Permanently */
    case 303:           /* See Other */
    case 404:           /* Not Found */
    case 410:           /* Gone */
        new_errno = ENOENT;
        break;
    case 408:           /* Request Timeout */
    case 504:           /* Gateway Timeout */
        new_errno = EAGAIN;
        break;
    case 423:           /* Locked */
        new_errno = EACCES;
        break;
    case 400:           /* Bad Request */
    case 403:           /* Forbidden */
    case 409:           /* Conflict */
    case 411:           /* Length Required */
    case 412:           /* Precondition Failed */
    case 414:           /* Request-URI Too Long */
    case 415:           /* Unsupported Media Type */
    case 424:           /* Failed Dependency */
    case 501:           /* Not Implemented */
        new_errno = EINVAL;
        break;
    case 507:           /* Insufficient Storage */
        new_errno = ENOSPC;
        break;
    case 206:           /* Partial Content */
    case 300:           /* Multiple Choices */
    case 302:           /* Found */
    case 305:           /* Use Proxy */
    case 306:           /* (Unused) */
    case 307:           /* Temporary Redirect */
    case 406:           /* Not Acceptable */
    case 416:           /* Requested Range Not Satisfiable */
    case 417:           /* Expectation Failed */
    case 422:           /* Unprocessable Entity */
    case 500:           /* Internal Server Error */
    case 502:           /* Bad Gateway */
    case 505:           /* HTTP Version Not Supported */
        new_errno = EIO;
        break;
    case 503:           /* Service Unavailable */
        new_errno = ERRNO_SERVICE_UNAVAILABLE;
        break;
    case 413:           /* Request Entity too Large */
        new_errno = EFBIG;
        break;
    default:
        new_errno = EIO;
    }

    errno = new_errno;
}

static int http_result_code_from_session() {
    const char *p = ne_get_error( dav_session.ctx );
    char *q;
    int err;

    set_error_message(p); /* remember the error message */

    err = strtol(p, &q, 10);
    if (p == q) {
        err = ERRNO_ERROR_STRING;
    }
    return err;
}

static void set_errno_from_session() {
    int err = http_result_code_from_session();

    if( err == EIO || err == ERRNO_ERROR_STRING) {
        errno = err;
    } else {
        set_errno_from_http_errcode(err);
    }
}

static void set_errno_from_neon_errcode( int neon_code ) {

    if( neon_code != NE_OK ) {
        DEBUG_WEBDAV("Neon error code was %d", neon_code);
    }

    switch(neon_code) {
    case NE_OK:     /* Success, but still the possiblity of problems */
    case NE_ERROR:  /* Generic error; use ne_get_error(session) for message */
        set_errno_from_session(); /* Something wrong with http communication */
        break;
    case NE_LOOKUP:  /* Server or proxy hostname lookup failed */
        errno = ERRNO_LOOKUP_ERROR;
        break;
    case NE_AUTH:     /* User authentication failed on server */
        errno = ERRNO_USER_UNKNOWN_ON_SERVER;
        break;
    case NE_PROXYAUTH:  /* User authentication failed on proxy */
        errno = ERRNO_PROXY_AUTH;
        break;
    case NE_CONNECT:  /* Could not connect to server */
        errno = ERRNO_CONNECT;
        break;
    case NE_TIMEOUT:  /* Connection timed out */
        errno = ERRNO_TIMEOUT;
        break;
    case NE_FAILED:   /* The precondition failed */
        errno = ERRNO_PRECONDITION;
        break;
    case NE_RETRY:    /* Retry request (ne_end_request ONLY) */
        errno = ERRNO_RETRY;
        break;

    case NE_REDIRECT: /* See ne_redirect.h */
        errno = ERRNO_REDIRECT;
        break;
    default:
        errno = ERRNO_GENERAL_ERROR;
    }
}

/* cleanPath to return an escaped path of an uri */
static char *_cleanPath( const char* uri ) {
    int rc = 0;
    char *path = NULL;
    char *re = NULL;

    rc = c_parse_uri( uri, NULL, NULL, NULL, NULL, NULL, &path );
    if( rc  < 0 ) {
        DEBUG_WEBDAV("Unable to cleanPath %s", uri ? uri: "<zero>" );
        re = NULL;
    } else {
        re = ne_path_escape( path );
    }

    SAFE_FREE( path );
    return re;
}

/*
 * helper method to build up a user text for SSL problems, called from the
 * verify_sslcert callback.
 */
static void addSSLWarning( char *ptr, const char *warn, int len )
{
    char *concatHere = ptr;
    int remainingLen = 0;

    if( ! (warn && ptr )) return;
    remainingLen = len - strlen(ptr);
    if( remainingLen <= 0 ) return;
    concatHere = ptr + strlen(ptr);  /* put the write pointer to the end. */
    strncpy( concatHere, warn, remainingLen );
}

/*
 * Callback to verify the SSL certificate, called from libneon.
 * It analyzes the SSL problem, creates a user information text and passes
 * it to the csync callback to ask the user.
 */
#define LEN 4096
static int verify_sslcert(void *userdata, int failures,
                          const ne_ssl_certificate *certificate)
{
    char problem[LEN];
    char buf[MAX(NE_SSL_DIGESTLEN, NE_ABUFSIZ)];
    int ret = -1;
    const ne_ssl_certificate *cert = certificate;

    (void) userdata;
    (void) userdata;
    memset( problem, 0, LEN );

    while( cert ) {

      addSSLWarning( problem, "There are problems with the SSL certificate:\n", LEN );
      if( failures & NE_SSL_NOTYETVALID ) {
        addSSLWarning( problem, " * The certificate is not yet valid.\n", LEN );
      }
      if( failures & NE_SSL_EXPIRED ) {
        addSSLWarning( problem, " * The certificate has expired.\n", LEN );
      }

      if( failures & NE_SSL_UNTRUSTED ) {
        addSSLWarning( problem, " * The certificate is not trusted!\n", LEN );
      }
      if( failures & NE_SSL_IDMISMATCH ) {
        addSSLWarning( problem, " * The hostname for which the certificate was "
                       "issued does not match the hostname of the server\n", LEN );
      }
      if( failures & NE_SSL_BADCHAIN ) {
        addSSLWarning( problem, " * The certificate chain contained a certificate other than the server cert\n", LEN );
      }
      if( failures & NE_SSL_REVOKED ) {
        addSSLWarning( problem, " * The server certificate has been revoked by the issuing authority.\n", LEN );
      }

      if (ne_ssl_cert_digest(cert, buf) == 0) {
        addSSLWarning( problem, "Certificate fingerprint: ", LEN );
        addSSLWarning( problem, buf, LEN );
        addSSLWarning( problem, "\n", LEN );
      }
      cert = ne_ssl_cert_signedby( cert );
    }
    addSSLWarning( problem, "Do you want to accept the certificate chain anyway?\nAnswer yes to do so and take the risk: ", LEN );

    if( _authcb ){
        /* call the csync callback */
        DEBUG_WEBDAV("Call the csync callback for SSL problems");
        memset( buf, 0, NE_ABUFSIZ );
        (*_authcb) ( problem, buf, NE_ABUFSIZ-1, 1, 0, dav_session.userdata );
        if( buf[0] == 'y' || buf[0] == 'Y') {
            ret = 0;
        } else {
            DEBUG_WEBDAV("Authentication callback replied %s", buf );

        }
    }
    DEBUG_WEBDAV("## VERIFY_SSL CERT: %d", ret  );
      return ret;
}

/*
 * Authentication callback. Is set by ne_set_server_auth to be called
 * from the neon lib to authenticate a request.
 */
static int ne_auth( void *userdata, const char *realm, int attempt,
                    char *username, char *password)
{
    char buf[NE_ABUFSIZ];

    (void) userdata;
    (void) realm;

    /* DEBUG_WEBDAV( "Authentication required %s", realm ); */
    if( username && password ) {
        DEBUG_WEBDAV( "Authentication required %s", username );
        if( dav_session.user ) {
            /* allow user without password */
            if( strlen( dav_session.user ) < NE_ABUFSIZ ) {
                strcpy( username, dav_session.user );
            }
            if( dav_session.pwd && strlen( dav_session.pwd ) < NE_ABUFSIZ ) {
                strcpy( password, dav_session.pwd );
            }
        } else if( _authcb != NULL ){
            /* call the csync callback */
            DEBUG_WEBDAV("Call the csync callback for %s", realm );
            memset( buf, 0, NE_ABUFSIZ );
            (*_authcb) ("Enter your username: ", buf, NE_ABUFSIZ-1, 1, 0, dav_session.userdata );
            if( strlen(buf) < NE_ABUFSIZ ) {
                strcpy( username, buf );
            }
            memset( buf, 0, NE_ABUFSIZ );
            (*_authcb) ("Enter your password: ", buf, NE_ABUFSIZ-1, 0, 0, dav_session.userdata );
            if( strlen(buf) < NE_ABUFSIZ) {
                strcpy( password, buf );
            }
        } else {
            DEBUG_WEBDAV("I can not authenticate!");
        }
    }
    return attempt;
}

/*
 * Authentication callback. Is set by ne_set_proxy_auth to be called
 * from the neon lib to authenticate against a proxy. The data to authenticate
 * against comes from mirall throught vio_module_init function.
 */
static int ne_proxy_auth( void *userdata, const char *realm, int attempt,
                          char *username, char *password)
{
    (void) userdata;
    (void) realm;
    if( dav_session.proxy_user && strlen( dav_session.proxy_user ) < NE_ABUFSIZ) {
        strcpy( username, dav_session.proxy_user );
        if( dav_session.proxy_pwd && strlen( dav_session.proxy_pwd ) < NE_ABUFSIZ) {
            strcpy( password, dav_session.proxy_pwd );
        }
    }
    /* NTLM needs several attempts */
    return (attempt < 3) ? 0 : -1;
}

/* Configure the proxy depending on the variables */
static int configureProxy( ne_session *session )
{
    int port = 8080;
    int re = -1;

    if( ! session ) return -1;
    if( ! dav_session.proxy_type ) return 0; /* Go by NoProxy per default */

    if( dav_session.proxy_port > 0 ) {
        port = dav_session.proxy_port;
    }

    if( c_streq(dav_session.proxy_type, "NoProxy" )) {
        DEBUG_WEBDAV("No proxy configured.");
        re = 0;
    } else if( c_streq(dav_session.proxy_type, "DefaultProxy") ||
               c_streq(dav_session.proxy_type, "HttpProxy")    ||
               c_streq(dav_session.proxy_type, "HttpCachingProxy") ) {
        if( dav_session.proxy_host ) {
            DEBUG_WEBDAV("%s at %s:%d", dav_session.proxy_type, dav_session.proxy_host, port );
            ne_session_proxy(session, dav_session.proxy_host, port );
            re = 2;
        } else {
            DEBUG_WEBDAV("%s requested but no proxy host defined.", dav_session.proxy_type );
	    /* we used to try ne_system_session_proxy here, but we should rather err out
	       to behave exactly like the caller. */
        }
    } else if( c_streq(dav_session.proxy_type, "FtpCachingProxy") ||
               c_streq(dav_session.proxy_type, "Socks5Proxy") ) {
        DEBUG_WEBDAV( "Unsupported Proxy: %s", dav_session.proxy_type );
    }

    return re;
}

/*
 * This hook is called for with the response of a request. Here its checked
 * if a Set-Cookie header is there for the PHPSESSID. The key is stored into
 * the webdav session to be added to subsequent requests.
 */
static void post_request_hook(ne_request *req, void *userdata, const ne_status *status)
{
    const char *set_cookie_header = NULL;
    const char *sc  = NULL;
    char *key = NULL;

    (void) userdata;

    if(!(status && req)) return;
    if( status->klass == 2 || status->code == 401 ) {
        /* successful request */
        set_cookie_header =  ne_get_response_header( req, "Set-Cookie" );
        if( set_cookie_header ) {
            DEBUG_WEBDAV(" Set-Cookie found: %s", set_cookie_header);
            /* try to find a ', ' sequence which is the separator of neon if multiple Set-Cookie
             * headers are there.
             * The following code parses a string like this:
             * Set-Cookie: 50ace6bd8a669=p537brtt048jh8srlp2tuep7em95nh9u98mj992fbqc47d1aecp1;
             */
            sc = set_cookie_header;
            while(sc) {
                const char *sc_val = sc;
                const char *sc_end = sc_val;
                int cnt = 0;
                int len = strlen(sc_val); /* The length of the rest of the header string. */

                while( cnt < len && *sc_end != ';' && *sc_end != ',') {
                    cnt++;
                    sc_end++;
                }
                if( cnt == len ) {
                    /* exit: We are at the end. */
                    sc = NULL;
                } else if( *sc_end == ';' ) {
                    /* We are at the end of the session key. */
                    int keylen = sc_end-sc_val;
                    if( key ) SAFE_FREE(key);
                    key = c_malloc(keylen+1);
                    strncpy( key, sc_val, keylen );
                    key[keylen] = '\0';

                    /* now search for a ',' to find a potential other header entry */
                    while(cnt < len && *sc_end != ',') {
                        cnt++;
                        sc_end++;
                    }
                    if( cnt < len )
                        sc = sc_end+2; /* mind the space after the comma */
                    else
                        sc = NULL;
                } else if( *sc_end == ',' ) {
                    /* A new entry is to check. */
                    if( *(sc_end + 1) == ' ') {
                        sc = sc_end+2;
                    } else {
                        /* error condition */
                        sc = NULL;
                    }
                }
            }
        }
    } else {
        DEBUG_WEBDAV("Request failed, don't take session header.");
    }
    if( key ) {
        DEBUG_WEBDAV("----> Session-key: %s", key);
        SAFE_FREE(dav_session.session_key);
        dav_session.session_key = key;
    }
}

/*
 * this hook is called just after a request has been created, before its sent.
 * Here it is used to set the session cookie if available.
 */
static void request_created_hook(ne_request *req, void *userdata,
                                 const char *method, const char *requri)
{
    (void) userdata;
    (void) method;
    (void) requri;

    if( !req ) return;

    if(dav_session.session_key) {
        /* DEBUG_WEBDAV("Setting PHPSESSID to %s", dav_session.session_key); */
        ne_add_request_header(req, "Cookie", dav_session.session_key);
    }

    if(dav_session.proxy_type) {
        /* required for NTLM */
        ne_add_request_header(req, "Proxy-Connection", "Keep-Alive");
    }
}

/* called from neon */
static void ne_notify_status_cb (void *userdata, ne_session_status status,
                                 const ne_session_status_info *info)
{
    struct transfer_context *tc = (struct transfer_context*) userdata;

    if (_progresscb && (status == ne_status_sending || status == ne_status_recving)) {
        if (info->sr.total > 0)
            _progresscb(tc->clean_uri, CSYNC_NOTIFY_PROGRESS, info->sr.progress, info->sr.total, dav_session.userdata);
    }
}


/*
 * Connect to a DAV server
 * This function sets the flag _connected if the connection is established
 * and returns if the flag is set, so calling it frequently is save.
 */
static int dav_connect(const char *base_url) {
    int useSSL = 0;
    int rc;
    char protocol[6] = {'\0'};
    char uaBuf[256];
    char *path = NULL;
    char *scheme = NULL;
    char *host = NULL;
    unsigned int port = 0;
    int proxystate = -1;

    if (_connected) {
        return 0;
    }

    dav_session.time_delta_sum = 0;
    dav_session.time_delta_cnt = 0;
    dav_session.prev_delta     = 0;

    rc = c_parse_uri( base_url, &scheme, &dav_session.user, &dav_session.pwd, &host, &port, &path );
    if( rc < 0 ) {
        DEBUG_WEBDAV("Failed to parse uri %s", base_url );
        goto out;
    }

    DEBUG_WEBDAV("* scheme %s", scheme );
    DEBUG_WEBDAV("* host %s", host );
    DEBUG_WEBDAV("* port %u", port );
    DEBUG_WEBDAV("* path %s", path );

    if( strcmp( scheme, "owncloud" ) == 0 ) {
        strcpy( protocol, "http");
    } else if( strcmp( scheme, "ownclouds" ) == 0 ) {
        strcpy( protocol, "https");
        useSSL = 1;
    } else {
        DEBUG_WEBDAV("Invalid scheme %s, go outa here!", scheme );
        rc = -1;
        goto out;
    }

    DEBUG_WEBDAV("* user %s", dav_session.user ? dav_session.user : "");

    if (port == 0) {
        port = ne_uri_defaultport(protocol);
    }

    rc = ne_sock_init();
    DEBUG_WEBDAV("ne_sock_init: %d", rc );
    if (rc < 0) {
        rc = -1;
        goto out;
    }

    dav_session.ctx = ne_session_create( protocol, host, port);

    if (dav_session.ctx == NULL) {
        DEBUG_WEBDAV("Session create with protocol %s failed", protocol );
        rc = -1;
        goto out;
    }

    if (dav_session.read_timeout == 0)
        dav_session.read_timeout = 30; // Default from old code

    ne_set_read_timeout(dav_session.ctx, dav_session.read_timeout);

    snprintf( uaBuf, sizeof(uaBuf), "csyncoC/%s",CSYNC_STRINGIFY( LIBCSYNC_VERSION ));
    ne_set_useragent( dav_session.ctx, uaBuf);
    ne_set_server_auth(dav_session.ctx, ne_auth, 0 );

    if( useSSL ) {
        if (!ne_has_support(NE_FEATURE_SSL)) {
            DEBUG_WEBDAV("Error: SSL is not enabled.");
            rc = -1;
            goto out;
        }

        ne_ssl_trust_default_ca( dav_session.ctx );
        ne_ssl_set_verify( dav_session.ctx, verify_sslcert, 0 );
    }
    ne_redirect_register( dav_session.ctx );

    /* Hook to get the Session ID */
    ne_hook_post_headers( dav_session.ctx, post_request_hook, NULL );
    /* Hook called when a request is built. It sets the PHPSESSID header */
    ne_hook_create_request( dav_session.ctx, request_created_hook, NULL );

    /* Proxy support */
    proxystate = configureProxy( dav_session.ctx );
    if( proxystate < 0 ) {
        DEBUG_WEBDAV("Error: Proxy-Configuration failed.");
    } else if( proxystate > 0 ) {
        ne_set_proxy_auth( dav_session.ctx, ne_proxy_auth, 0 );
    }

    _connected = 1;
    rc = 0;
out:
    SAFE_FREE(path);
    SAFE_FREE(host);
    SAFE_FREE(scheme);
    return rc;
}

#ifndef HAVE_TIMEGM
#ifdef _WIN32
static int is_leap(unsigned y) {
    y += 1900;
    return (y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0);
}

static time_t timegm(struct tm *tm) {
    static const unsigned ndays[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31} };

    time_t res = 0;
    int i;

    for (i = 70; i < tm->tm_year; ++i)
        res += is_leap(i) ? 366 : 365;

    for (i = 0; i < tm->tm_mon; ++i)
        res += ndays[is_leap(tm->tm_year)][i];
     res += tm->tm_mday - 1;
     res *= 24;
     res += tm->tm_hour;
     res *= 60;
     res += tm->tm_min;
     res *= 60;
     res += tm->tm_sec;
     return res;
}
#else
/* A hopefully portable version of timegm */
static time_t timegm(struct tm *tm ) {
     time_t ret;
     char *tz;

     tz = getenv("TZ");
     setenv("TZ", "", 1);
     tzset();
     ret = mktime(tm);
     if (tz)
         setenv("TZ", tz, 1);
     else
         unsetenv("TZ");
     tzset();
     return ret;
}
#endif /* Platform switch */
#endif /* HAVE_TIMEGM */

#define RFC1123_FORMAT "%3s, %02d %3s %4d %02d:%02d:%02d GMT"
static const char short_months[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
/*
 * This function is borrowed from libneon's ne_httpdate_parse.
 * Unfortunately that one converts to local time but here UTC is
 * needed.
 * This one uses timegm instead, which returns UTC.
 */
static time_t oc_httpdate_parse( const char *date ) {
    struct tm gmt;
    char wkday[4], mon[4];
    int n;
    time_t result = 0;

    memset(&gmt, 0, sizeof(struct tm));

    /*  it goes: Sun, 06 Nov 1994 08:49:37 GMT */
    n = sscanf(date, RFC1123_FORMAT,
               wkday, &gmt.tm_mday, mon, &gmt.tm_year, &gmt.tm_hour,
               &gmt.tm_min, &gmt.tm_sec);
    /* Is it portable to check n==7 here? */
    gmt.tm_year -= 1900;
    for (n=0; n<12; n++)
        if (strcmp(mon, short_months[n]) == 0)
            break;
    /* tm_mon comes out as 12 if the month is corrupt, which is desired,
     * since the mktime will then fail */
    gmt.tm_mon = n;
    gmt.tm_isdst = -1;
    result = timegm(&gmt);
    return result;
}

/*
 * result parsing list.
 * This function is called to parse the result of the propfind request
 * to list directories on the WebDAV server. I takes a single resource
 * and fills a resource struct and stores it to the result list which
 * is stored in the listdir_context.
 */
static void results(void *userdata,
                    const ne_uri *uri,
                    const ne_prop_result_set *set)
{
    struct listdir_context *fetchCtx = userdata;
    struct resource *newres = 0;
    const char *clength, *modtime = NULL;
    const char *resourcetype = NULL;
    const char *md5sum = NULL;
    const ne_status *status = NULL;
    char *path = ne_path_unescape( uri->path );

    (void) status;
    if( ! fetchCtx ) {
        DEBUG_WEBDAV("No valid fetchContext");
        return;
    }

    if( ! fetchCtx->target ) {
        DEBUG_WEBDAV("error: target must not be zero!" );
        return;
    }

    /* Fill the resource structure with the data about the file */
    newres = c_malloc(sizeof(struct resource));
    newres->uri =  path; /* no need to strdup because ne_path_unescape already allocates */
    newres->name = c_basename( path );

    modtime      = ne_propset_value( set, &ls_props[0] );
    clength      = ne_propset_value( set, &ls_props[1] );
    resourcetype = ne_propset_value( set, &ls_props[2] );
    md5sum       = ne_propset_value( set, &ls_props[3] );

    newres->type = resr_normal;
    if( clength == NULL && resourcetype && strncmp( resourcetype, "<DAV:collection>", 16 ) == 0) {
        newres->type = resr_collection;
    }

    if (modtime) {
        newres->modtime = oc_httpdate_parse(modtime);
    }

    /* DEBUG_WEBDAV("Parsing Modtime: %s -> %llu", modtime, (unsigned long long) newres->modtime ); */

    if (clength) {
        char *p;

        newres->size = DAV_STRTOL(clength, &p, 10);
        if (*p) {
            newres->size = 0;
        }
        /* DEBUG_WEBDAV("Parsed File size for %s from %s: %lld", newres->name, clength, (long long)newres->size ); */
    }

    if( md5sum ) {
        int len = strlen(md5sum)-2;
        if( len > 0 ) {
            /* Skip the " around the string coming back from the ne_propset_value call */
            newres->md5 = c_malloc(len+1);
            strncpy( newres->md5, md5sum+1, len );
            newres->md5[len] = '\0';
        }
    }

    /* prepend the new resource to the result list */
    newres->next   = fetchCtx->list;
    fetchCtx->list = newres;
    fetchCtx->result_count = fetchCtx->result_count + 1;
    /* DEBUG_WEBDAV( "results for URI %s: %d %d", newres->name, (int)newres->size, (int)newres->type ); */
}



/*
 * fetches a resource list from the WebDAV server. This is equivalent to list dir.
 */
static struct listdir_context *fetch_resource_list(const char *uri, int depth)
{
    struct listdir_context *fetchCtx;
    int ret = 0;
    ne_propfind_handler *hdl = NULL;
    ne_request *request = NULL;
    const char *date_header = NULL;
    const char *content_type = NULL;
    char *curi = NULL;
    time_t server_time;
    time_t now;
    time_t time_diff;
    time_t time_diff_delta;
    const ne_status *req_status = NULL;

    curi = _cleanPath( uri );

    if (propfind_cache) {
        if (c_streq(curi, propfind_cache->target)) {
            propfind_cache->ref++;
            SAFE_FREE(curi);
            return propfind_cache;
        }
    }

    fetchCtx = c_malloc( sizeof( struct listdir_context ));
    if (!fetchCtx) {
        errno = ENOMEM;
        SAFE_FREE(curi);
        return NULL;
    }
    fetchCtx->list = NULL;
    fetchCtx->target = curi;
    fetchCtx->currResource = NULL;
    fetchCtx->ref = 1;

    /* do a propfind request and parse the results in the results function, set as callback */
    hdl = ne_propfind_create(dav_session.ctx, curi, depth);

    if(hdl) {
        ret = ne_propfind_named(hdl, ls_props, results, fetchCtx);
        request = ne_propfind_get_request( hdl );
        req_status = ne_get_status( request );
    }

    if( ret == NE_OK ) {
        fetchCtx->currResource = fetchCtx->list;
        /* Check the request status. */
        if( req_status->klass != 2 ) {
            set_errno_from_http_errcode(req_status->code);
            DEBUG_WEBDAV("ERROR: Request failed: status %d (%s)", req_status->code,
                         req_status->reason_phrase);
            ret = NE_CONNECT;
            set_error_message(req_status->reason_phrase);
            if (_progresscb) {
                _progresscb(uri, CSYNC_NOTIFY_ERROR,  req_status->code, (long long)(req_status->reason_phrase) ,dav_session.userdata);
            }
        }
        DEBUG_WEBDAV("Simple propfind result code %d.", req_status->code);
    } else {
        if( ret == NE_ERROR && req_status->code == 404) {
            errno = ENOENT;
        } else {
            set_errno_from_neon_errcode(ret);
        }
    }

    if( ret == NE_OK ) {
        /* Check the content type. If the server has a problem, ie. database is gone or such,
         * the content type is not xml but a html error message. Stop on processing if it's
         * not XML.
         * FIXME: Generate user error message from the reply content.
         */
        content_type =  ne_get_response_header( request, "Content-Type" );
        if( !(content_type && c_streq(content_type, "application/xml; charset=utf-8") ) ) {
            DEBUG_WEBDAV("ERROR: Content type of propfind request not XML: %s.",
                         content_type ?  content_type: "<empty>");
            errno = ERRNO_WRONG_CONTENT;
            set_error_message("Server error: PROPFIND reply is not XML formatted!");
            ret = NE_CONNECT;
        }
    }

    if( ret == NE_OK ) {
        date_header =  ne_get_response_header( request, "Date" );
        DEBUG_WEBDAV("Server Date from HTTP header value: %s", date_header);
        server_time = oc_httpdate_parse(date_header);
        if( server_time ) {
            now = time(NULL);
            time_diff = server_time - now;

            dav_session.time_delta_sum += time_diff;
            dav_session.time_delta_cnt++;

            /* Store the previous time delta */
            dav_session.prev_delta = dav_session.time_delta;

            /* check the changing of the time delta */
            time_diff_delta = llabs(dav_session.time_delta - time_diff);
            if( dav_session.time_delta_cnt == 1 ) {
                DEBUG_WEBDAV( "The first time_delta is %llu", (unsigned long long) time_diff );
            } else if( dav_session.time_delta_cnt > 1 ) {
                if( time_diff_delta > 5 ) {
                    DEBUG_WEBDAV("WRN: The time delta changed more than 5 second");
                    // errno = ERRNO_TIMEDELTA;
                    // ret = OC_TIMEDELTA_FAIL;
                } else {
                    DEBUG_WEBDAV("Ok: Time delta remained (almost) the same: %llu.", (unsigned long long) time_diff);
                }
            } else {
                DEBUG_WEBDAV("Difference to last server time delta: %llu", (unsigned long long) time_diff_delta );
            }
            dav_session.time_delta = time_diff;
        } else {
            DEBUG_WEBDAV("ERROR: Unable to parse server time.");
        }
    }

    if( ret != NE_OK ) {
        const char *err = NULL;

        err = ne_get_error( dav_session.ctx );
        DEBUG_WEBDAV("WRN: propfind named failed with %d, request error: %s", ret, err ? err : "<nil>");
    }

    if( hdl )
        ne_propfind_destroy(hdl);

    if( ret == NE_REDIRECT ) {
        const ne_uri *redir_ne_uri = NULL;
        char *redir_uri = NULL;
        redir_ne_uri = ne_redirect_location(dav_session.ctx);
        if( redir_ne_uri ) {
            redir_uri = ne_uri_unparse(redir_ne_uri);
            DEBUG_WEBDAV("Permanently moved to %s", redir_uri);
        }
    }
    if( ret == OC_TIMEDELTA_FAIL ) {
        DEBUG_WEBDAV("WRN: Time delta changed too much!");
        /* FIXME: Reasonable user warning */
    }

    if( ret != NE_OK ) {
        free_fetchCtx(fetchCtx);
        return NULL;
    }

    free_fetchCtx(propfind_cache);
    propfind_cache = fetchCtx;
    propfind_cache->ref++;
    return fetchCtx;
}

/*
 * helper: convert a resource struct to file_stat struct.
 */
static csync_vio_file_stat_t *resourceToFileStat( struct resource *res )
{
    csync_vio_file_stat_t *lfs = NULL;

    if( ! res ) {
        return NULL;
    }

    lfs = c_malloc(sizeof(csync_vio_file_stat_t));
    if (lfs == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    lfs->name = c_strdup( res->name );

    lfs->fields = CSYNC_VIO_FILE_STAT_FIELDS_NONE;
    if( res->type == resr_normal ) {
        lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
        lfs->type = CSYNC_VIO_FILE_TYPE_REGULAR;
    } else if( res->type == resr_collection ) {
        lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
        lfs->type = CSYNC_VIO_FILE_TYPE_DIRECTORY;
    } else {
        DEBUG_WEBDAV("ERROR: Unknown resource type %d", res->type);
    }

    /* Correct the mtime of the file with the server time delta */
    DEBUG_WEBDAV("  :> Subtracting %ld from modtime %llu", dav_session.time_delta,
		 (unsigned long long) res->modtime);
    lfs->mtime = res->modtime - dav_session.time_delta ;
    lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
    lfs->size  = res->size;
    lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;
    if( res->md5 ) {
        lfs->md5   = c_strdup(res->md5);
    }
    lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MD5;
    return lfs;
}

/* WebDAV does not deliver permissions. Set a default here. */
static int _stat_perms( int type ) {
    int ret = 0;

    if( type == CSYNC_VIO_FILE_TYPE_DIRECTORY ) {
        /* DEBUG_WEBDAV("Setting mode in stat (dir)"); */
        /* directory permissions */
        ret = S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR /* directory, rwx for user */
                | S_IRGRP | S_IXGRP                       /* rx for group */
                | S_IROTH | S_IXOTH;                      /* rx for others */
    } else {
        /* regualar file permissions */
        /* DEBUG_WEBDAV("Setting mode in stat (file)"); */
        ret = S_IFREG | S_IRUSR | S_IWUSR /* regular file, user read & write */
                | S_IRGRP                         /* group read perm */
                | S_IROTH;                        /* others read perm */
    }
    return ret;
}

static void fill_stat_cache( csync_vio_file_stat_t *lfs ) {

    if( _stat_cache.name ) SAFE_FREE(_stat_cache.name);
    if( _stat_cache.md5  ) SAFE_FREE(_stat_cache.md5 );

    if( !lfs) return;

    _stat_cache.name   = c_strdup(lfs->name);
    _stat_cache.mtime  = lfs->mtime;
    _stat_cache.fields = lfs->fields;
    _stat_cache.type   = lfs->type;
    _stat_cache.size   = lfs->size;
    if( lfs->md5 ) {
        _stat_cache.md5    = c_strdup(lfs->md5);
    }
}



/*
 * file functions
 */
static int owncloud_stat(const char *uri, csync_vio_file_stat_t *buf) {
    /* get props:
     *   modtime
     *   creattime
     *   size
     */
    csync_vio_file_stat_t *lfs = NULL;
    struct listdir_context  *fetchCtx = NULL;
    char *decodedUri = NULL;
    int len = 0;
    errno = 0;

    DEBUG_WEBDAV("owncloud_stat %s called", uri );

    buf->name = c_basename(uri);

    if (buf->name == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if( _stat_cache.name && strcmp( buf->name, _stat_cache.name ) == 0 ) {
        buf->fields  = CSYNC_VIO_FILE_STAT_FIELDS_NONE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_PERMISSIONS;

        buf->fields = _stat_cache.fields;
        buf->type   = _stat_cache.type;
        buf->mtime  = _stat_cache.mtime;
        buf->size   = _stat_cache.size;
        buf->mode   = _stat_perms( _stat_cache.type );
        buf->md5    = NULL;
        if( _stat_cache.md5 ) {
            buf->md5    = c_strdup( _stat_cache.md5 );
            buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MD5;
        }
        return 0;
    }

    /* fetch data via a propfind call. */
    fetchCtx = fetch_resource_list( uri, NE_DEPTH_ONE);
    DEBUG_WEBDAV("=> Errno after fetch resource list for %s: %d", uri, errno);
    if (!fetchCtx) {
        return -1;
    }

    if( fetchCtx ) {
        struct resource *res = fetchCtx->list;
        while( res ) {
            /* remove trailing slashes */
            len = strlen(res->uri);
            while( len > 0 && res->uri[len-1] == '/' ) --len;
            decodedUri = ne_path_unescape( fetchCtx->target ); /* allocates memory */

            /* Only do the comparaison of the part of the string without the trailing
               slashes, and make sure decodedUri is not too large */
            if( strncmp(res->uri, decodedUri, len ) == 0 && decodedUri[len] == '\0') {
                SAFE_FREE( decodedUri );
                break;
            }
            res = res->next;
            SAFE_FREE( decodedUri );
        }
        if( res ) {
            DEBUG_WEBDAV("Working on file %s", res->name );
        } else {
            DEBUG_WEBDAV("ERROR: Result struct not valid!");
        }

        lfs = resourceToFileStat( res );
        if( lfs ) {
            buf->fields = CSYNC_VIO_FILE_STAT_FIELDS_NONE;
            buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
            buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;
            buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
            buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_PERMISSIONS;
            buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MD5;

            buf->fields = lfs->fields;
            buf->type   = lfs->type;
            buf->mtime  = lfs->mtime;
            buf->size   = lfs->size;
            buf->mode   = _stat_perms( lfs->type );
            buf->md5    = NULL;
            if( lfs->md5 ) {
                buf->md5    = c_strdup( lfs->md5 );
            }

            /* fill the static stat buf as input for the stat function */
            csync_vio_file_stat_destroy( lfs );
        }

        free_fetchCtx( fetchCtx );
    }
    DEBUG_WEBDAV("STAT result from propfind: %s, mtime: %llu", buf->name ? buf->name:"NULL",
                    (unsigned long long) buf->mtime );

    return 0;
}

static ssize_t owncloud_write(csync_vio_method_handle_t *fhandle, const void *buf, size_t count) {
    (void) fhandle;
    (void) buf;
    (void) count;

    return 0;
}

static int content_reader(void *userdata, const char *buf, size_t len)
{
   struct transfer_context *writeCtx = userdata;
   size_t written = 0;

   if( buf && writeCtx->fd ) {
       /* DEBUG_WEBDAV("Writing %scompressed %d bytes", (writeCtx->decompress ? "" : "NON "), len); */
       written = write(writeCtx->fd, buf, len);
       if( len != written ) {
           DEBUG_WEBDAV("WRN: content_reader wrote wrong num of bytes: %zu, %zu", len, written);
       }
       return NE_OK;
   } else {
     errno = EBADF;
   }
   return NE_ERROR;
}

/*
 * This hook is called after the response is here from the server, but before
 * the response body is parsed. It decides if the response is compressed and
 * if it is it installs the compression reader accordingly.
 * If the response is not compressed, the normal response body reader is installed.
 */

static void install_content_reader( ne_request *req, void *userdata, const ne_status *status )
{
    const char *enc = NULL;
    struct transfer_context *writeCtx = userdata;

    (void) status;

    if( !writeCtx ) {
        DEBUG_WEBDAV("Error: install_content_reader called without valid write context!");
        return;
    }

    enc = ne_get_response_header( req, "Content-Encoding" );
    DEBUG_WEBDAV("Content encoding ist <%s> with status %d", enc ? enc : "empty",
                  status ? status->code : -1 );

    if( enc && c_streq( enc, "gzip" )) {
        writeCtx->decompress = ne_decompress_reader( req, ne_accept_2xx,
                                                     content_reader,     /* reader callback */
                                                     (void*) writeCtx );  /* userdata        */
    } else {
        ne_add_response_body_reader( req, ne_accept_2xx,
                                     content_reader,
                                     (void*) writeCtx );
        writeCtx->decompress = NULL;
    }

    enc = ne_get_response_header( req, "ETag" );
    if (enc && *enc) {
        SAFE_FREE(_id_cache.uri);
        SAFE_FREE(_id_cache.id);
        _id_cache.uri = c_strdup(writeCtx->clean_uri);
        _id_cache.id = c_strdup(enc);
    }
}

static char*_lastDir = NULL;

/* capabilities are currently:
 *  bool atomar_copy_support - oC provides atomar copy
 *  bool do_post_copy_stat   - oC does not want the post copy check
 *  bool time_sync_required  - oC does not require the time sync
 *  int  unix_extensions     - oC supports unix extensions.
 *  bool propagate_on_fd     - oC supports the send_file method.
 */
static csync_vio_capabilities_t _owncloud_capabilities = { true, false, false, 0, true };

static csync_vio_capabilities_t *owncloud_capabilities(void)
{
#ifdef _WIN32
  _owncloud_capabilities.unix_extensions = 0;
#endif
  return &_owncloud_capabilities;
}

static const char* owncloud_file_id( const char *path )
{
    ne_request *req    = NULL;
    const char *header = NULL;
    char *uri          = _cleanPath(path);
    char *buf          = NULL;
    const char *cbuf   = NULL;
    csync_vio_file_stat_t *fs = NULL;
    bool doHeadRequest = false;

    if (_id_cache.uri && c_streq(path, _id_cache.uri)) {
        header = _id_cache.id;
    }

    doHeadRequest= false; /* ownCloud server doesn't have good support for HEAD yet */

    if( !header && doHeadRequest ) {
        int neon_stat;
        /* Perform an HEAD request to the resource. HEAD delivers the
         * ETag header back. */
        req = ne_request_create(dav_session.ctx, "HEAD", uri);
        neon_stat = ne_request_dispatch(req);
        set_errno_from_neon_errcode( neon_stat );

        header = ne_get_response_header(req, "etag");
    }
    /* If the request went wrong or the server did not respond correctly
     * (that can happen for collections) a stat call is done which translates
     * into a PROPFIND request.
     */
    if( ! header ) {
        /* ... and do a stat call. */
        fs = csync_vio_file_stat_new();
        if(fs == NULL) {
            DEBUG_WEBDAV( "owncloud_file_id: memory fault.");
            errno = ENOMEM;
            return NULL;
        }
        if( owncloud_stat( path, fs ) == 0 ) {
            header = fs->md5;
        }
    }

    /* In case the result is surrounded by "" cut them away. */
    if( header ) {
        if( header [0] == '"' && header[ strlen(header)-1] == '"') {
            int len = strlen( header )-2;
            buf = c_malloc( len+1 );
            strncpy( buf, header+1, len );
            buf[len] = '\0';
            cbuf = buf;
            /* do not free header here, as it belongs to the request */
        } else {
            cbuf = c_strdup(header);
        }
    }
    DEBUG_WEBDAV("Get file ID for %s: %s", path, cbuf ? cbuf:"<null>");
    if( fs ) csync_vio_file_stat_destroy(fs);
    if( req ) ne_request_destroy(req);
    SAFE_FREE(uri);

    return cbuf;
}

static csync_vio_method_handle_t *owncloud_open(const char *durl,
                                                int flags,
                                                mode_t mode) {
    char *uri = NULL;
    char *dir = NULL;
    int put = 0;
    int rc = NE_OK;

    struct transfer_context *writeCtx = NULL;
    csync_vio_file_stat_t statBuf;
    ZERO_STRUCT(statBuf);

    (void) mode; /* unused on webdav server */
    DEBUG_WEBDAV( "=> open called for %s", durl );

    uri = _cleanPath( durl );
    if( ! uri ) {
        DEBUG_WEBDAV("Failed to clean path for %s", durl );
        errno = EACCES;
        rc = NE_ERROR;
    }

    if( rc == NE_OK )
        dav_connect( durl );

    if (flags & O_WRONLY) {
        put = 1;
    }
    if (flags & O_RDWR) {
        put = 1;
    }
    if (flags & O_CREAT) {
        put = 1;
    }

    if( rc == NE_OK && put ) {
        /* check if the dir name exists. Otherwise return ENOENT */
        dir = c_dirname( durl );
        if (dir == NULL) {
            errno = ENOMEM;
            SAFE_FREE(uri);
            return NULL;
        }
        DEBUG_WEBDAV("Stating directory %s", dir );
        if( c_streq( dir, _lastDir )) {
            DEBUG_WEBDAV("Dir %s is there, we know it already.", dir);
        } else {
            if( owncloud_stat( dir, &statBuf ) == 0 ) {
                SAFE_FREE(statBuf.name);
                SAFE_FREE(statBuf.md5);
                DEBUG_WEBDAV("Directory of file to open exists.");
                SAFE_FREE( _lastDir );
                _lastDir = c_strdup(dir);

            } else {
                DEBUG_WEBDAV("Directory %s of file to open does NOT exist.", dir );
                /* the directory does not exist. That is an ENOENT */
                errno = ENOENT;
                SAFE_FREE(dir);
                SAFE_FREE(uri);
                SAFE_FREE(statBuf.name);
                return NULL;
            }
        }
    }

    writeCtx = c_malloc( sizeof(struct transfer_context) );
    writeCtx->clean_uri = c_strdup(durl);

    if( rc == NE_OK && put) {
        DEBUG_WEBDAV("PUT request on %s!", uri);
        writeCtx->req = ne_request_create(dav_session.ctx, "PUT", uri);
        writeCtx->method = "PUT";
    }

    if( rc == NE_OK && ! put ) {
        writeCtx->req = 0;
        writeCtx->method = "GET";
        DEBUG_WEBDAV("GET request on %s", uri );

        writeCtx->req = ne_request_create( dav_session.ctx, "GET", uri );
        writeCtx->fd = -1;
    }

    if( rc != NE_OK ) {
        SAFE_FREE( writeCtx );
    }

    SAFE_FREE( uri );
    SAFE_FREE( dir );

    return (csync_vio_method_handle_t *) writeCtx;
}

static csync_vio_method_handle_t *owncloud_creat(const char *durl, mode_t mode) {

    csync_vio_method_handle_t *handle = owncloud_open(durl, O_CREAT|O_WRONLY|O_TRUNC, mode);

    /* on create, the file needs to be created empty */
    owncloud_write( handle, NULL, 0 );

    return handle;
}

static int owncloud_sendfile(csync_vio_method_handle_t *src, csync_vio_method_handle_t *hdl ) {
    int rc  = 0;
    int neon_stat;
    const ne_status *status;
    struct transfer_context *write_ctx = (struct transfer_context*) hdl;
    fhandle_t *fh = (fhandle_t *) src;
    int fd;
    int error_code = 0;
    const char *error_string = NULL;

    if( ! write_ctx ) {
        errno = EINVAL;
        return -1;
    }

    if( !fh ) {
        errno = EINVAL;
        return -1;
    }
    fd = fh->fd;

    DEBUG_WEBDAV("Sendfile handling request type %s.", write_ctx->method);

    /* Copy from the file descriptor if method == PUT
     * Copy to the file descriptor if method == GET
     */

    if( c_streq( write_ctx->method, "PUT") ) {
        /* Transmit a file through PUT */
        ne_request *request = write_ctx->req;
        if( request ) {
            /* stat the source-file to get the file size. */
            csync_stat_t sb;
            if( fstat( fd, &sb ) == 0 ) {
                /* Attach the request to the file descriptor */
                ne_set_request_body_fd(request, fd, 0, sb.st_size);
                DEBUG_WEBDAV("Put file size: %lld, variable sizeof: %ld", (long long int) sb.st_size,
                             sizeof(sb.st_size));

                if (_progresscb) {
                    ne_set_notifier(dav_session.ctx, ne_notify_status_cb, write_ctx);
                    _progresscb(write_ctx->clean_uri, CSYNC_NOTIFY_START_UPLOAD, 0 , 0, dav_session.userdata);
                }

                /* Start the request. */
                neon_stat = ne_request_dispatch( write_ctx->req );
                set_errno_from_neon_errcode( neon_stat );

                status = ne_get_status( request );
                if( status->klass != 2 ) {
                    DEBUG_WEBDAV("sendfile request failed with http status %d!", status->code);
                    set_errno_from_http_errcode( status->code );
                    /* decide if soft error or hard error that stops the whole sync. */
                    /* Currently all problems concerning one file are soft errors */
                    if( status->klass == 4 /* Forbidden and stuff, soft error */ ) {
                        rc = 1;
                    } else if( status->klass == 5 /* Server errors and such */ ) {
                        rc = 1; /* No Abort on individual file errors. */
                    } else {
                        rc = 1;
                    }

                    error_code = status->code;
                    error_string = status->reason_phrase;
                } else {
                    DEBUG_WEBDAV("http request all cool, result code %d", status->code);
                }

                if (_progresscb) {
                    ne_set_notifier(dav_session.ctx, 0, 0);
                    _progresscb(write_ctx->clean_uri, rc != NE_OK ?  CSYNC_NOTIFY_ERROR : 
                                CSYNC_NOTIFY_FINISHED_UPLOAD, error_code,
                                (long long)(error_string), dav_session.userdata);
                }
            } else {
                DEBUG_WEBDAV("Could not stat file descriptor");
                rc = 1;
            }
        } else {
            DEBUG_WEBDAV("Did not find a valid request!");
            rc = 1;
        }
    } else if( c_streq( write_ctx->method, "GET") ) {
        /* GET a file to the file descriptor */
        /* actually do the request */
        DEBUG_WEBDAV("  -- GET on %s", write_ctx->clean_uri);
        write_ctx->fd = fd;
        if (_progresscb) {
            ne_set_notifier(dav_session.ctx, ne_notify_status_cb, write_ctx);
            _progresscb(write_ctx->clean_uri, CSYNC_NOTIFY_START_DOWNLOAD, 0 , 0, dav_session.userdata);
        }

        /* Allow compressed content by setting the header */
        ne_add_request_header( write_ctx->req, "Accept-Encoding", "gzip" );

        /* hook called before the content is parsed to set the correct reader,
         * either the compressed- or uncompressed reader.
         */
        ne_hook_post_headers( dav_session.ctx, install_content_reader, write_ctx );

        neon_stat = ne_request_dispatch(write_ctx->req );
        /* possible return codes are:
         *  NE_OK, NE_AUTH, NE_CONNECT, NE_TIMEOUT, NE_ERROR (from ne_request.h)
         */

        if( neon_stat != NE_OK ) {
            set_errno_from_neon_errcode(neon_stat);
            DEBUG_WEBDAV("Error GET: Neon: %d, errno %d", neon_stat, errno);
            rc = -1;
        } else {
            status = ne_get_status( write_ctx->req );
            DEBUG_WEBDAV("GET http result %d (%s)", status->code, status->reason_phrase ? status->reason_phrase : "<empty");
            if( status->klass != 2 ) {
                DEBUG_WEBDAV("sendfile request failed with http status %d!", status->code);
                set_errno_from_http_errcode( status->code );
                /* decide if soft error or hard error that stops the whole sync. */
                /* Currently all problems concerning one file are soft errors */
                if( status->klass == 4 /* Forbidden and stuff, soft error */ ) {
                    rc = 1;
                } else if( status->klass == 5 /* Server errors and such */ ) {
                    rc = 1; /* No Abort on individual file errors. */
                } else {
                    rc = 1;
                }
                error_code = status->code;
                error_string = status->reason_phrase;
            } else {
                DEBUG_WEBDAV("http request all cool, result code %d", status->code);
            }
        }

        /* delete the hook again, otherwise they get chained as they are with the session */
        ne_unhook_post_headers( dav_session.ctx, install_content_reader, write_ctx );

        /* if the compression handle is set through the post_header hook, delete it. */
        if( write_ctx->decompress ) {
            ne_decompress_destroy( write_ctx->decompress );
        }
        if (_progresscb) {
            ne_set_notifier(dav_session.ctx, 0, 0);
            _progresscb(write_ctx->clean_uri, (rc != NE_OK) ? CSYNC_NOTIFY_ERROR :
                        CSYNC_NOTIFY_FINISHED_DOWNLOAD, error_code ,
                        (long long)(error_string), dav_session.userdata);
        }
    } else  {
        DEBUG_WEBDAV("Unknown method!");
        rc = -1;
    }

    return rc;
}

static int owncloud_close(csync_vio_method_handle_t *fhandle) {
    struct transfer_context *writeCtx;

    int ret = 0;

    writeCtx = (struct transfer_context*) fhandle;

    if (fhandle == NULL) {
        DEBUG_WEBDAV("*** Close returns errno EBADF!");
        errno = EBADF;
        return -1;
    }

    ne_request_destroy( writeCtx->req );

    if( ret != -1 && strcmp( writeCtx->method, "PUT" ) == 0 ) {
        // Clear the cache so get_id gets the updates
        clean_caches();
    }

    /* free mem. Note that the request mem is freed by the ne_request_destroy call */
    SAFE_FREE( writeCtx->clean_uri );
    SAFE_FREE( writeCtx );

    return ret;
}

static ssize_t owncloud_read(csync_vio_method_handle_t *fhandle, void *buf, size_t count) {
    size_t len = 0;

    (void) fhandle;
    (void) buf;
    (void) count;

    return len;
}

static off_t owncloud_lseek(csync_vio_method_handle_t *fhandle, off_t offset, int whence) {
    (void) fhandle;
    (void) offset;
    (void) whence;

    return -1;
}

/*
 * directory functions
 */
static csync_vio_method_handle_t *owncloud_opendir(const char *uri) {
    struct listdir_context *fetchCtx = NULL;

    DEBUG_WEBDAV("opendir method called on %s", uri );

    dav_connect( uri );

    fetchCtx = fetch_resource_list( uri, NE_DEPTH_ONE );
    if( !fetchCtx ) {
        /* errno is set properly in fetch_resource_list */
        DEBUG_WEBDAV("Errno set to %d", errno);
        return NULL;
    } else {
        fetchCtx->currResource = fetchCtx->list;
        DEBUG_WEBDAV("opendir returning handle %p", (void*) fetchCtx );
        return fetchCtx;
    }
    /* no freeing of curi because its part of the fetchCtx and gets freed later */
}

static int owncloud_closedir(csync_vio_method_handle_t *dhandle) {

    struct listdir_context *fetchCtx = dhandle;

    DEBUG_WEBDAV("closedir method called %p!", dhandle);

    free_fetchCtx(fetchCtx);

    return 0;
}

static csync_vio_file_stat_t *owncloud_readdir(csync_vio_method_handle_t *dhandle) {

    struct listdir_context *fetchCtx = dhandle;

    if( fetchCtx->currResource ) {
        // DEBUG_WEBDAV("readdir method called for %s", fetchCtx->currResource->uri);
    } else {
        /* DEBUG_WEBDAV("An empty dir or at end"); */
        return NULL;
    }

    while( fetchCtx && fetchCtx->currResource ) {
        resource* currResource = fetchCtx->currResource;
        char *escaped_path = NULL;

        /* set pointer to next element */
        fetchCtx->currResource = fetchCtx->currResource->next;

        /* It seems strange: first uri->path is unescaped to escape it in the next step again.
         * The reason is that uri->path is not completely escaped (ie. it seems only to have
         * spaces escaped), while the fetchCtx->target is fully escaped.
         * See http://bugs.owncloud.org/thebuggenie/owncloud/issues/oc-613
         */
        escaped_path = ne_path_escape( currResource->uri );
        if (ne_path_compare(fetchCtx->target, escaped_path) != 0) {
            csync_vio_file_stat_t* lfs = resourceToFileStat(currResource);
            fill_stat_cache(lfs);
            SAFE_FREE( escaped_path );
            return lfs;
        }

        /* This is the target URI */
        DEBUG_WEBDAV( "Skipping target resource.");
        SAFE_FREE( escaped_path );
    }

    return NULL;
}

static int owncloud_mkdir(const char *uri, mode_t mode) {
    int rc = NE_OK;
    int len = 0;
    ne_request *req = NULL;

    char *path = _cleanPath( uri );
    (void) mode; /* unused */

    if( ! path ) {
        errno = EINVAL;
        rc = -1;
    }
    rc = dav_connect(uri);
    if (rc < 0) {
        errno = EINVAL;
    }

    /* the uri path is required to have a trailing slash */
    if( rc >= 0 ) {
        len = strlen( path );
        if( path[len-1] != '/' ) {
            path = c_realloc(path, len+2);
            path[len]= '/';
            path[len+1] = 0;
        }
        DEBUG_WEBDAV("MKdir on %s", path );

        req = ne_request_create(dav_session.ctx, "MKCOL", path);
        rc = ne_simple_request(dav_session.ctx, req);

        /* Special for mkcol: it returns 405 if the directory already exists.
         * To keep csync vio_mkdirs working errno EEXIST has to be returned. */
        if( ne_get_status(req)->code == 405 ) {
            errno = EEXIST;
        } else {
            set_errno_from_neon_errcode(rc);
        }
    }
    SAFE_FREE( path );

    if( rc < 0 || rc != NE_OK ) {
        return -1;
    }
    return 0;
}

static int owncloud_rmdir(const char *uri) {
    int rc = NE_OK;
    char* curi = _cleanPath( uri );

    rc = dav_connect(uri);
    if (rc < 0) {
        errno = EINVAL;
    }

    if( rc >= 0 ) {
        rc = ne_delete(dav_session.ctx, curi);
        set_errno_from_neon_errcode( rc );
    }
    SAFE_FREE( curi );
    if( rc < 0 || rc != NE_OK ) {
        return -1;
    }

    return 0;
}

static int owncloud_rename(const char *olduri, const char *newuri) {
    char *src = NULL;
    char *target = NULL;
    int rc = NE_OK;


    rc = dav_connect(olduri);
    if (rc < 0) {
        errno = EINVAL;
    }

    src    = _cleanPath( olduri );
    target = _cleanPath( newuri );

    if( rc >= 0 ) {
        DEBUG_WEBDAV("MOVE: %s => %s: %d", src, target, rc );
        rc = ne_move(dav_session.ctx, 1, src, target );

        set_errno_from_neon_errcode(rc);
    }
    SAFE_FREE( src );
    SAFE_FREE( target );

    if( rc != NE_OK )
        return -1;
    return 0;
}

static int owncloud_unlink(const char *uri) {
    int rc = NE_OK;
    char *path = _cleanPath( uri );

    if( ! path ) {
        rc = NE_ERROR;
        errno = EINVAL;
    }
    if( rc == NE_OK ) {
        rc = dav_connect(uri);
        if (rc < 0) {
            errno = EINVAL;
        }
    }
    if( rc == NE_OK ) {
        rc = ne_delete( dav_session.ctx, path );
            set_errno_from_neon_errcode(rc);
    }
    SAFE_FREE( path );

    return 0;
}

static int owncloud_chmod(const char *uri, mode_t mode) {
    (void) uri;
    (void) mode;

    return 0;
}

static int owncloud_chown(const char *uri, uid_t owner, gid_t group) {
    (void) uri;
    (void) owner;
    (void) group;

    return 0;
}

static char *owncloud_error_string()
{
    return dav_session.error_string;
}

static int owncloud_utimes(const char *uri, const struct timeval *times) {

    ne_proppatch_operation ops[2];
    ne_propname pname;
    int rc = NE_OK;
    char val[255];
    char *curi = NULL;
    const struct timeval *modtime = times+1;
    long newmodtime;

    curi = _cleanPath( uri );

    if( ! uri ) {
        errno = ENOENT;
        return -1;
    }
    if( !times ) {
        errno = EACCES;
        return -1; /* FIXME: Find good errno */
    }
    pname.nspace = "DAV:";
    pname.name = "lastmodified";

    newmodtime = modtime->tv_sec;

    DEBUG_WEBDAV("Add a time delta to modtime %lu: %llu",
                 modtime->tv_sec, (unsigned long long) dav_session.time_delta);
    newmodtime += dav_session.time_delta;

    snprintf( val, sizeof(val), "%ld", newmodtime );
    DEBUG_WEBDAV("Setting LastModified of %s to %s", curi, val );

    ops[0].name = &pname;
    ops[0].type = ne_propset;
    ops[0].value = val;

    ops[1].name = NULL;

    rc = ne_proppatch( dav_session.ctx, curi, ops );
    SAFE_FREE(curi);

    if( rc != NE_OK ) {
        set_errno_from_neon_errcode(rc);
        DEBUG_WEBDAV("Error in propatch: %d", rc);
        return -1;
    }

    clean_caches();

    return 0;
}

static int owncloud_set_property(const char *key, void *data) {
#define READ_STRING_PROPERTY(P) \
    if (c_streq(key, #P)) { \
        SAFE_FREE(dav_session.P); \
        dav_session.P = c_strdup((const char*)data); \
        return 0; \
    }
    READ_STRING_PROPERTY(session_key)
    READ_STRING_PROPERTY(proxy_type)
    READ_STRING_PROPERTY(proxy_host)
    READ_STRING_PROPERTY(proxy_user)
    READ_STRING_PROPERTY(proxy_pwd)
#undef READ_STRING_PROPERTY

    if (c_streq(key, "proxy_port")) {
        dav_session.proxy_port = *(int*)(data);
        return 0;
    }
    if (c_streq(key, "progress_callback")) {
        _progresscb = *(csync_progress_callback*)(data);
        return 0;
    }
    if (c_streq(key, "read_timeout")) {
        dav_session.read_timeout = *(int*)(data);
        return 0;
    }
    if( c_streq(key, "csync_context")) {
        dav_session.csync_ctx = data;
        return 0;
    }

    return -1;
}

csync_vio_method_t _method = {
    .method_table_size = sizeof(csync_vio_method_t),
    .get_capabilities = owncloud_capabilities,
    .get_file_id = owncloud_file_id,
    .open = owncloud_open,
    .creat = owncloud_creat,
    .close = owncloud_close,
    .read = owncloud_read,
    .write = owncloud_write,
    .sendfile = owncloud_sendfile,
    .lseek = owncloud_lseek,
    .opendir = owncloud_opendir,
    .closedir = owncloud_closedir,
    .readdir = owncloud_readdir,
    .mkdir = owncloud_mkdir,
    .rmdir = owncloud_rmdir,
    .stat = owncloud_stat,
    .rename = owncloud_rename,
    .unlink = owncloud_unlink,
    .chmod = owncloud_chmod,
    .chown = owncloud_chown,
    .utimes = owncloud_utimes,
    .set_property = owncloud_set_property,
    .get_error_string = owncloud_error_string
};

csync_vio_method_t *vio_module_init(const char *method_name, const char *args,
                                    csync_auth_callback cb, void *userdata) {
    (void) method_name;
    (void) args;

    _authcb = cb;
    _connected = 0;  /* triggers dav_connect to go through the whole neon setup */

    memset(&dav_session, 0, sizeof(dav_session));
    dav_session.userdata = userdata;

    return &_method;
}

void vio_module_shutdown(csync_vio_method_t *method) {
    (void) method;

    SAFE_FREE( dav_session.user );
    SAFE_FREE( dav_session.pwd );

    SAFE_FREE( dav_session.proxy_type );
    SAFE_FREE( dav_session.proxy_host );
    SAFE_FREE( dav_session.proxy_user );
    SAFE_FREE( dav_session.proxy_pwd  );
    SAFE_FREE( dav_session.session_key);
    SAFE_FREE( dav_session.error_string );

    SAFE_FREE( _lastDir );

    clean_caches();

    if( dav_session.ctx )
        ne_session_destroy( dav_session.ctx );
    /* DEBUG_WEBDAV( "********** vio_module_shutdown" ); */

    ne_sock_exit();
}

/* vim: set ts=4 sw=4 et cindent: */
