/*
  +----------------------------------------------------------------------+
  | PHP Version 4                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2006 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.02 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available at through the world-wide-web at                           |
  | http://www.php.net/license/2_02.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Sara Golemon <pollita@php.net>                               |
  +----------------------------------------------------------------------+

  $Id$ 
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ssh2.h"

/* **********************
   * channel_stream_ops *
   ********************** */

static size_t php_ssh2_channel_stream_write(php_stream *stream, const char *buf, size_t count TSRMLS_DC)
{
	php_ssh2_channel_data *abstract = (php_ssh2_channel_data*)stream->abstract;
	size_t writestate;
	LIBSSH2_SESSION *session;

	libssh2_channel_set_blocking(abstract->channel, abstract->is_blocking);
	session = (LIBSSH2_SESSION *)zend_fetch_resource(NULL TSRMLS_CC, abstract->session_rsrc, PHP_SSH2_SESSION_RES_NAME, NULL, 1, le_ssh2_session);

#ifdef PHP_SSH2_SESSION_TIMEOUT
	if (abstract->is_blocking) {
		libssh2_session_set_timeout(session, abstract->timeout);
	}
#endif

	writestate = libssh2_channel_write_ex(abstract->channel, abstract->streamid, buf, count);

#ifdef PHP_SSH2_SESSION_TIMEOUT
	if (abstract->is_blocking) {
		libssh2_session_set_timeout(session, 0);
	}
#endif
	if (writestate == LIBSSH2_ERROR_EAGAIN) {
		writestate = 0;
	}

	if (writestate < 0) {
		char *error_msg = NULL;
		if (libssh2_session_last_error(session, &error_msg, NULL, 0) == writestate) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failure '%s' (%ld)", error_msg, writestate);
		}

		stream->eof = 1;
		writestate = 0;
	}

	return writestate;
}

static size_t php_ssh2_channel_stream_read(php_stream *stream, char *buf, size_t count TSRMLS_DC)
{
	php_ssh2_channel_data *abstract = (php_ssh2_channel_data*)stream->abstract;
	ssize_t readstate;
	LIBSSH2_SESSION *session;

	stream->eof = libssh2_channel_eof(abstract->channel);
	libssh2_channel_set_blocking(abstract->channel, abstract->is_blocking);
	session = (LIBSSH2_SESSION *)zend_fetch_resource(NULL TSRMLS_CC, abstract->session_rsrc, PHP_SSH2_SESSION_RES_NAME, NULL, 1, le_ssh2_session);

#ifdef PHP_SSH2_SESSION_TIMEOUT
	if (abstract->is_blocking) {
		libssh2_session_set_timeout(session, abstract->timeout);
	}
#endif

	readstate = libssh2_channel_read_ex(abstract->channel, abstract->streamid, buf, count);

#ifdef PHP_SSH2_SESSION_TIMEOUT
	if (abstract->is_blocking) {
		libssh2_session_set_timeout(session, 0);
	}
#endif
	if (readstate == LIBSSH2_ERROR_EAGAIN) {
		readstate = 0;
	}

	if (readstate < 0) {
		char *error_msg = NULL;
		if (libssh2_session_last_error(session, &error_msg, NULL, 0) == readstate) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failure '%s' (%ld)", error_msg, readstate);
		}

		stream->eof = 1;
		readstate = 0;
	}
	return readstate;
}

static int php_ssh2_channel_stream_close(php_stream *stream, int close_handle TSRMLS_DC)
{
	php_ssh2_channel_data *abstract = (php_ssh2_channel_data*)stream->abstract;

	if (!abstract->refcount || (--(*(abstract->refcount)) == 0)) {
		/* Last one out, turn off the lights */
		if (abstract->refcount) {
			efree(abstract->refcount);
		}
		libssh2_channel_eof(abstract->channel);
		libssh2_channel_free(abstract->channel);
		zend_list_delete(abstract->session_rsrc);
	}
	efree(abstract);

	return 0;
}

static int php_ssh2_channel_stream_flush(php_stream *stream TSRMLS_DC)
{
	php_ssh2_channel_data *abstract = (php_ssh2_channel_data*)stream->abstract;

	return libssh2_channel_flush_ex(abstract->channel, abstract->streamid);
}

static int php_ssh2_channel_stream_set_option(php_stream *stream, int option, int value, void *ptrparam TSRMLS_DC)
{
	php_ssh2_channel_data *abstract = (php_ssh2_channel_data*)stream->abstract;
	int ret;

	switch (option) {
		case PHP_STREAM_OPTION_BLOCKING:
			ret = abstract->is_blocking;
			abstract->is_blocking = value;
			return ret;
			break;

		case PHP_STREAM_OPTION_META_DATA_API:
			add_assoc_long((zval*)ptrparam, "exit_status", libssh2_channel_get_exit_status(abstract->channel));
			break;

		case PHP_STREAM_OPTION_READ_TIMEOUT:
			ret = abstract->timeout;
#ifdef PHP_SSH2_SESSION_TIMEOUT
			struct timeval tv = *(struct timeval*)ptrparam;
			abstract->timeout = tv.tv_sec * 1000 + (tv.tv_usec / 1000);
#else
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "No support for ssh2 stream timeout. Please recompile with libssh2 >= 1.2.9");
#endif
			return ret;
			break;

#if PHP_MAJOR_VERSION >= 5
		case PHP_STREAM_OPTION_CHECK_LIVENESS:
			return stream->eof = libssh2_channel_eof(abstract->channel);
			break;
#endif
	}

	return -1;
}

php_stream_ops php_ssh2_channel_stream_ops = {
	php_ssh2_channel_stream_write,
	php_ssh2_channel_stream_read,
	php_ssh2_channel_stream_close,
	php_ssh2_channel_stream_flush,
	PHP_SSH2_CHANNEL_STREAM_NAME,
	NULL, /* seek */
	NULL, /* cast */
	NULL, /* stat */
	php_ssh2_channel_stream_set_option,
};

/* *********************
   * Magic Path Helper *
   ********************* */

/* {{{ php_ssh2_fopen_wraper_parse_path
 * Parse an ssh2.*:// path
 */
php_url *php_ssh2_fopen_wraper_parse_path(	char *path, char *type, php_stream_context *context,
											LIBSSH2_SESSION **psession, int *presource_id, 
											LIBSSH2_SFTP **psftp, int *psftp_rsrcid
											TSRMLS_DC)
{
	php_ssh2_sftp_data *sftp_data = NULL;
	LIBSSH2_SESSION *session;
	php_url *resource;
	zval *methods = NULL, *callbacks = NULL, zsession, **tmpzval;
	long resource_id;
	char *s, *username = NULL, *password = NULL, *pubkey_file = NULL, *privkey_file = NULL;
	int username_len = 0, password_len = 0;

	resource = php_url_parse(path);
	if (!resource || !resource->path) {
		return NULL;
	}

	if (strncmp(resource->scheme, "ssh2.", sizeof("ssh2.") - 1)) {
		/* Not an ssh wrapper */
		php_url_free(resource);
		return NULL;
	}

	if (strcmp(resource->scheme + sizeof("ssh2.") - 1, type)) {
		/* Wrong ssh2. wrapper type */
		php_url_free(resource);
		return NULL;
	}

	if (!resource->host) {
		return NULL;
	}

	/*
		Find resource->path in the path string, then copy the entire string from the original path.
		This includes ?query#fragment in the path string
	*/
	s = resource->path;
	resource->path = estrdup(strstr(path, resource->path));
	efree(s);

	/* Look for a resource ID to reuse a session */
	s = resource->host;
	if (strncmp(resource->host, "Resource id #", sizeof("Resource id #") - 1) == 0) {
		s = resource->host + sizeof("Resource id #") - 1;
	}
	if (is_numeric_string(s, strlen(s), &resource_id, NULL, 0) == IS_LONG) {
		php_ssh2_sftp_data *sftp_data;

		if (psftp) {
			sftp_data = (php_ssh2_sftp_data*)zend_fetch_resource(NULL TSRMLS_CC, resource_id, PHP_SSH2_SFTP_RES_NAME, NULL, 1, le_ssh2_sftp);
			if (sftp_data) {
				/* Want the sftp layer */
				zend_list_addref(resource_id);
				*psftp_rsrcid = resource_id;
				*psftp = sftp_data->sftp;
				*presource_id = sftp_data->session_rsrcid;
				*psession = sftp_data->session;
				return resource;
			}
		}
		session = (LIBSSH2_SESSION *)zend_fetch_resource(NULL TSRMLS_CC, resource_id, PHP_SSH2_SESSION_RES_NAME, NULL, 1, le_ssh2_session);
		if (session) {
			if (psftp) {
				/* We need an sftp layer too */
				LIBSSH2_SFTP *sftp = libssh2_sftp_init(session);

				if (!sftp) {
					php_url_free(resource);
					return NULL;
				}
				sftp_data = emalloc(sizeof(php_ssh2_sftp_data));
				sftp_data->sftp = sftp;
				sftp_data->session = session;
				sftp_data->session_rsrcid = resource_id;
				zend_list_addref(resource_id);
				*psftp_rsrcid = ZEND_REGISTER_RESOURCE(NULL, sftp_data, le_ssh2_sftp);
				*psftp = sftp;
				*presource_id = resource_id;
				*psession = session;
				return resource;
			}
			zend_list_addref(resource_id);
			*presource_id = resource_id;
			*psession = session;
			return resource;
		}
	}

	/* Fallback on finding it in the context */
	if (resource->host[0] == 0 && context && psftp &&
		php_stream_context_get_option(context, "ssh2", "sftp", &tmpzval) == SUCCESS &&
		Z_TYPE_PP(tmpzval) == IS_RESOURCE) {
		php_ssh2_sftp_data *sftp_data;
		sftp_data = (php_ssh2_sftp_data *)zend_fetch_resource(tmpzval TSRMLS_CC, -1, PHP_SSH2_SFTP_RES_NAME, NULL, 1, le_ssh2_sftp);
		if (sftp_data) {
			zend_list_addref(Z_LVAL_PP(tmpzval));
			*psftp_rsrcid = Z_LVAL_PP(tmpzval);
			*psftp = sftp_data->sftp;
			*presource_id = sftp_data->session_rsrcid;
			*psession = sftp_data->session;
			return resource;
		}
	}
	if (resource->host[0] == 0 && context &&
		php_stream_context_get_option(context, "ssh2", "session", &tmpzval) == SUCCESS &&
		Z_TYPE_PP(tmpzval) == IS_RESOURCE) {
		session = (LIBSSH2_SESSION *)zend_fetch_resource(tmpzval TSRMLS_CC, -1, PHP_SSH2_SESSION_RES_NAME, NULL, 1, le_ssh2_session);
		if (session) {
			if (psftp) {
				/* We need an SFTP layer too! */
				LIBSSH2_SFTP *sftp = libssh2_sftp_init(session);
				php_ssh2_sftp_data *sftp_data;

				if (!sftp) {
					php_url_free(resource);
					return NULL;
				}
				sftp_data = emalloc(sizeof(php_ssh2_sftp_data));
				sftp_data->sftp = sftp;
				sftp_data->session = session;
				sftp_data->session_rsrcid = Z_LVAL_PP(tmpzval);
				zend_list_addref(Z_LVAL_PP(tmpzval));
				*psftp_rsrcid = ZEND_REGISTER_RESOURCE(NULL, sftp_data, le_ssh2_sftp);
				*psftp = sftp;
				*presource_id = Z_LVAL_PP(tmpzval);
				*psession = session;
				return resource;
			}
			zend_list_addref(Z_LVAL_PP(tmpzval));
			*psession = session;
			*presource_id = Z_LVAL_PP(tmpzval);
			return resource;
		}
	}

	/* Make our own connection then */
	if (!resource->port) {
		resource->port = 22;
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "methods", &tmpzval) == SUCCESS &&
		Z_TYPE_PP(tmpzval) == IS_ARRAY) {
		methods = *tmpzval;
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "callbacks", &tmpzval) == SUCCESS &&
		Z_TYPE_PP(tmpzval) == IS_ARRAY) {
		callbacks = *tmpzval;
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "username", &tmpzval) == SUCCESS &&
		Z_TYPE_PP(tmpzval) == IS_STRING) {
		username = Z_STRVAL_PP(tmpzval);
		username_len = Z_STRLEN_PP(tmpzval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "password", &tmpzval) == SUCCESS &&
		Z_TYPE_PP(tmpzval) == IS_STRING) {
		password = Z_STRVAL_PP(tmpzval);
		password_len = Z_STRLEN_PP(tmpzval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "pubkey_file", &tmpzval) == SUCCESS &&
		Z_TYPE_PP(tmpzval) == IS_STRING) {
		pubkey_file = Z_STRVAL_PP(tmpzval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "privkey_file", &tmpzval) == SUCCESS &&
		Z_TYPE_PP(tmpzval) == IS_STRING) {
		privkey_file = Z_STRVAL_PP(tmpzval);
	}

	if (resource->user) {
		int len = strlen(resource->user);

		if (len) {
			username = resource->user;
			username_len = len;
		}
	}

	if (resource->pass) {
		int len = strlen(resource->pass);

		if (len) {
			password = resource->pass;
			password_len = len;
		}
	}

	if (!username) {
		/* username is a minimum */
		php_url_free(resource);
		return NULL;
	}

	session = php_ssh2_session_connect(resource->host, resource->port, methods, callbacks TSRMLS_CC);
	if (!session) {
		/* Unable to connect! */
		php_url_free(resource);
		return NULL;
	}

	/* Authenticate */
	if (pubkey_file && privkey_file) {
		if (SSH2_OPENBASEDIR_CHECKPATH(pubkey_file) || SSH2_OPENBASEDIR_CHECKPATH(privkey_file)) {
			php_url_free(resource);
			return NULL;
		}

		/* Attempt pubkey authentication */
		if (!libssh2_userauth_publickey_fromfile(session, username, pubkey_file, privkey_file, password)) {
			goto session_authed;
		}
	}

	if (password) {
		/* Attempt password authentication */
		if (libssh2_userauth_password_ex(session, username, username_len, password, password_len, NULL) == 0) {
			goto session_authed;
		}
	}

	/* Auth failure */
	php_url_free(resource);
	zend_list_delete(Z_LVAL(zsession));
	return NULL;

session_authed:
	ZEND_REGISTER_RESOURCE(&zsession, session, le_ssh2_session);

	if (psftp) {
		LIBSSH2_SFTP *sftp;
		zval zsftp;

		sftp = libssh2_sftp_init(session);
		if (!sftp) {
			php_url_free(resource);
			zend_list_delete(Z_LVAL(zsession));
			return NULL;
		}

		sftp_data = emalloc(sizeof(php_ssh2_sftp_data));
		sftp_data->session = session;
		sftp_data->sftp = sftp;
		sftp_data->session_rsrcid = Z_LVAL(zsession);

		ZEND_REGISTER_RESOURCE(&zsftp, sftp_data, le_ssh2_sftp);
		*psftp_rsrcid = Z_LVAL(zsftp);
		*psftp = sftp;
	}

	*presource_id = Z_LVAL(zsession);
	*psession = session;

	return resource;
}
/* }}} */

/* *****************
   * Shell Wrapper *
   ***************** */

/* {{{ php_ssh2_shell_open
 * Make a stream from a session
 */
static php_stream *php_ssh2_shell_open(LIBSSH2_SESSION *session, int resource_id, char *term, int term_len, zval *environment, long width, long height, long type TSRMLS_DC)
{
	LIBSSH2_CHANNEL *channel;
	php_ssh2_channel_data *channel_data;
	php_stream *stream;

	libssh2_session_set_blocking(session, 1);

	channel = libssh2_channel_open_session(session);
	if (!channel) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to request a channel from remote host");
		return NULL;
	}

	if (environment) {
		char *key;
		int key_type, key_len;
		long idx;

		for(zend_hash_internal_pointer_reset(HASH_OF(environment));
			(key_type = zend_hash_get_current_key_ex(HASH_OF(environment), &key, &key_len, &idx, 0, NULL)) != HASH_KEY_NON_EXISTANT;
			zend_hash_move_forward(HASH_OF(environment))) {
			if (key_type == HASH_KEY_IS_STRING) {
				zval **value;

				if (zend_hash_get_current_data(HASH_OF(environment), (void**)&value) == SUCCESS) {
					zval copyval = **value;

					zval_copy_ctor(&copyval);
					convert_to_string(&copyval);
					if (libssh2_channel_setenv_ex(channel, key, key_len, Z_STRVAL(copyval), Z_STRLEN(copyval))) {
						php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed setting %s=%s on remote end", key, Z_STRVAL(copyval));
					}
					zval_dtor(&copyval);
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Skipping numeric index in environment array");
			}
		}
	}

	if (type == PHP_SSH2_TERM_UNIT_CHARS) {
		if (libssh2_channel_request_pty_ex(channel, term, term_len, NULL, 0, width, height, 0, 0)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed allocating %s pty at %ldx%ld characters", term, width, height);
			libssh2_channel_free(channel);
			return NULL;
		}
	} else {
		if (libssh2_channel_request_pty_ex(channel, term, term_len, NULL, 0, 0, 0, width, height)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed allocating %s pty at %ldx%ld pixels", term, width, height);
			libssh2_channel_free(channel);
			return NULL;
		}
	}

	if (libssh2_channel_shell(channel)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to request shell from remote host");
		libssh2_channel_free(channel);
		return NULL;
	}

	/* Turn it into a stream */
	channel_data = emalloc(sizeof(php_ssh2_channel_data));
	channel_data->channel = channel;
	channel_data->streamid = 0;
	channel_data->is_blocking = 0;
	channel_data->timeout = 0;
	channel_data->session_rsrc = resource_id;
	channel_data->refcount = NULL;

	stream = php_stream_alloc(&php_ssh2_channel_stream_ops, channel_data, 0, "r+");

	return stream;
}
/* }}} */

/* {{{ php_ssh2_fopen_wrapper_shell
 * ssh2.shell:// fopen wrapper
 */
static php_stream *php_ssh2_fopen_wrapper_shell(php_stream_wrapper *wrapper, char *path, char *mode, int options, char **opened_path, php_stream_context *context STREAMS_DC TSRMLS_DC)
{
	LIBSSH2_SESSION *session = NULL;
	php_stream *stream;
	zval **tmpzval, *environment = NULL;
	char *terminal = PHP_SSH2_DEFAULT_TERMINAL;
	long width = PHP_SSH2_DEFAULT_TERM_WIDTH;
	long height = PHP_SSH2_DEFAULT_TERM_HEIGHT;
	long type = PHP_SSH2_DEFAULT_TERM_UNIT;
	int resource_id = 0, terminal_len = sizeof(PHP_SSH2_DEFAULT_TERMINAL) - 1;
	php_url *resource;
	char *s, *e;

	resource = php_ssh2_fopen_wraper_parse_path(path, "shell", context, &session, &resource_id, NULL, NULL TSRMLS_CC);
	if (!resource || !session) {
		return NULL;
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "env", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval && Z_TYPE_PP(tmpzval) == IS_ARRAY) {
		environment = *tmpzval;
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "term", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval && Z_TYPE_PP(tmpzval) == IS_STRING) {
		terminal = Z_STRVAL_PP(tmpzval);
		terminal_len = Z_STRLEN_PP(tmpzval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "term_width", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval) {
		zval *copyval;
		ALLOC_INIT_ZVAL(copyval);
		*copyval = **tmpzval;
		convert_to_long(copyval);
		width = Z_LVAL_P(copyval);
		zval_ptr_dtor(&copyval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "term_height", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval) {
		zval *copyval;
		ALLOC_INIT_ZVAL(copyval);
		*copyval = **tmpzval;
		convert_to_long(copyval);
		height = Z_LVAL_P(copyval);
		zval_ptr_dtor(&copyval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "term_units", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval) {
		zval *copyval;
		ALLOC_INIT_ZVAL(copyval);
		*copyval = **tmpzval;
		convert_to_long(copyval);
		type = Z_LVAL_P(copyval);
		zval_ptr_dtor(&copyval);
	}

	s = resource->path ? resource->path : NULL;
	e = s ? s + strlen(s) : NULL;

	if (s && s[0] == '/') {
		/* Terminal type encoded into URL overrides context terminal type */
		char *p;

		s++;
		p = strchr(s, '/');
		if (p) {
			if (p - s) {
				terminal = s;
				terminal_len = p - terminal;
				s += terminal_len + 1;
			} else {
				/* "null" terminal given, skip it */
				s++;
			}
		} else {
			int len;

			if ((len = strlen(path + 1))) {
				terminal = s;
				terminal_len = len;
				s += len;
			}
		}
	}

	/* TODO: Accept resolution and environment vars as URL style parameters
	 * ssh2.shell://hostorresource/terminal/99x99c?envvar=envval&envvar=envval....
	 */
	stream = php_ssh2_shell_open(session, resource_id, terminal, terminal_len, environment, width, height, type TSRMLS_CC);
	if (!stream) {
		zend_list_delete(resource_id);
	}
	php_url_free(resource);

	return stream;
}
/* }}} */

static php_stream_wrapper_ops php_ssh2_shell_stream_wops = {
	php_ssh2_fopen_wrapper_shell,
	NULL, /* stream_close */
	NULL, /* stat */
	NULL, /* stat_url */
	NULL, /* opendir */
	"ssh2.shell"
};

php_stream_wrapper php_ssh2_stream_wrapper_shell =    {
	&php_ssh2_shell_stream_wops,
	NULL,
	0
};

/* {{{ proto stream ssh2_shell(resource session[, string term_type[, array env[, int width, int height[, int width_height_type]]]])
 * Open a shell at the remote end and allocate a channel for it
 */
PHP_FUNCTION(ssh2_shell)
{
	LIBSSH2_SESSION *session;
	php_stream *stream;
	zval *zsession;
	zval *environment = NULL;
	char *term = PHP_SSH2_DEFAULT_TERMINAL;
	int term_len = sizeof(PHP_SSH2_DEFAULT_TERMINAL) - 1;
	long width = PHP_SSH2_DEFAULT_TERM_WIDTH;
	long height = PHP_SSH2_DEFAULT_TERM_HEIGHT;
	long type = PHP_SSH2_DEFAULT_TERM_UNIT;
	int argc = ZEND_NUM_ARGS();

	if (argc == 5) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "width specified without height parameter");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(argc TSRMLS_CC, "r|sa!lll", &zsession, &term, &term_len, &environment, &width, &height, &type) == FAILURE) {
		return;
	}

	SSH2_FETCH_AUTHENTICATED_SESSION(session, zsession);

	stream = php_ssh2_shell_open(session, Z_LVAL_P(zsession), term, term_len, environment, width, height, type TSRMLS_CC);
	if (!stream) {
		RETURN_FALSE;
	}

	/* Ensure that channels are freed BEFORE the sessions they belong to */
	zend_list_addref(Z_LVAL_P(zsession));

	php_stream_to_zval(stream, return_value);
}
/* }}} */

/* ****************
   * Exec Wrapper *
   **************** */

/* {{{ php_ssh2_exec_command
 * Make a stream from a session
 */
static php_stream *php_ssh2_exec_command(LIBSSH2_SESSION *session, int resource_id, char *command, char *term, int term_len, zval *environment, long width, long height, long type TSRMLS_DC)
{
	LIBSSH2_CHANNEL *channel;
	php_ssh2_channel_data *channel_data;
	php_stream *stream;

	libssh2_session_set_blocking(session, 1);

	channel = libssh2_channel_open_session(session);
	if (!channel) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to request a channel from remote host");
		return NULL;
	}

	if (environment) {
		char *key;
		int key_type, key_len;
		long idx;

		for(zend_hash_internal_pointer_reset(HASH_OF(environment));
			(key_type = zend_hash_get_current_key_ex(HASH_OF(environment), &key, &key_len, &idx, 0, NULL)) != HASH_KEY_NON_EXISTANT;
			zend_hash_move_forward(HASH_OF(environment))) {
			if (key_type == HASH_KEY_IS_STRING) {
				zval **value;

				if (zend_hash_get_current_data(HASH_OF(environment), (void**)&value) == SUCCESS) {
					zval copyval = **value;

					zval_copy_ctor(&copyval);
					convert_to_string(&copyval);
					if (libssh2_channel_setenv_ex(channel, key, key_len, Z_STRVAL(copyval), Z_STRLEN(copyval))) {
						php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed setting %s=%s on remote end", key, Z_STRVAL(copyval));
					}
					zval_dtor(&copyval);
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Skipping numeric index in environment array");
			}
		}
	}

	if (term) {
		if (type == PHP_SSH2_TERM_UNIT_CHARS) {
			if (libssh2_channel_request_pty_ex(channel, term, term_len, NULL, 0, width, height, 0, 0)) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed allocating %s pty at %ldx%ld characters", term, width, height);
				libssh2_channel_free(channel);
				return NULL;
			}
		} else {
			if (libssh2_channel_request_pty_ex(channel, term, term_len, NULL, 0, 0, 0, width, height)) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed allocating %s pty at %ldx%ld pixels", term, width, height);
				libssh2_channel_free(channel);
				return NULL;
			}
		}
	}

	if (libssh2_channel_exec(channel, command)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to request command execution on remote host");
		libssh2_channel_free(channel);
		return NULL;
	}

	/* Turn it into a stream */
	channel_data = emalloc(sizeof(php_ssh2_channel_data));
	channel_data->channel = channel;
	channel_data->streamid = 0;
	channel_data->is_blocking = 0;
	channel_data->timeout = 0;
	channel_data->session_rsrc = resource_id;
	channel_data->refcount = NULL;

	stream = php_stream_alloc(&php_ssh2_channel_stream_ops, channel_data, 0, "r+");

	return stream;
}
/* }}} */

/* {{{ php_ssh2_fopen_wrapper_exec
 * ssh2.exec:// fopen wrapper
 */
static php_stream *php_ssh2_fopen_wrapper_exec(php_stream_wrapper *wrapper, char *path, char *mode, int options, char **opened_path, php_stream_context *context STREAMS_DC TSRMLS_DC)
{
	LIBSSH2_SESSION *session = NULL;
	php_stream *stream;
	zval **tmpzval, *environment = NULL;
	int resource_id = 0;
	php_url *resource;
	char *terminal = NULL;
	int terminal_len = 0;
	long width = PHP_SSH2_DEFAULT_TERM_WIDTH;
	long height = PHP_SSH2_DEFAULT_TERM_HEIGHT;
	long type = PHP_SSH2_DEFAULT_TERM_UNIT;

	resource = php_ssh2_fopen_wraper_parse_path(path, "exec", context, &session, &resource_id, NULL, NULL TSRMLS_CC);
	if (!resource || !session) {
		return NULL;
	}
	if (!resource->path) {
		php_url_free(resource);
		zend_list_delete(resource_id);
		return NULL;
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "env", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval && Z_TYPE_PP(tmpzval) == IS_ARRAY) {
		environment = *tmpzval;
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "term", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval && Z_TYPE_PP(tmpzval) == IS_STRING) {
		terminal = Z_STRVAL_PP(tmpzval);
		terminal_len = Z_STRLEN_PP(tmpzval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "term_width", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval) {
		zval *copyval;
		ALLOC_INIT_ZVAL(copyval);
		*copyval = **tmpzval;
		convert_to_long(copyval);
		width = Z_LVAL_P(copyval);
		zval_ptr_dtor(&copyval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "term_height", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval) {
		zval *copyval;
		ALLOC_INIT_ZVAL(copyval);
		*copyval = **tmpzval;
		convert_to_long(copyval);
		height = Z_LVAL_P(copyval);
		zval_ptr_dtor(&copyval);
	}

	if (context &&
		php_stream_context_get_option(context, "ssh2", "term_units", &tmpzval) == SUCCESS &&
		tmpzval && *tmpzval) {
		zval *copyval;
		ALLOC_INIT_ZVAL(copyval);
		*copyval = **tmpzval;
		convert_to_long(copyval);
		type = Z_LVAL_P(copyval);
		zval_ptr_dtor(&copyval);
	}

	stream = php_ssh2_exec_command(session, resource_id, resource->path + 1, terminal, terminal_len, environment, width, height, type TSRMLS_CC);
	if (!stream) {
		zend_list_delete(resource_id);
	}
	php_url_free(resource);

	return stream;
}
/* }}} */

static php_stream_wrapper_ops php_ssh2_exec_stream_wops = {
	php_ssh2_fopen_wrapper_exec,
	NULL, /* stream_close */
	NULL, /* stat */
	NULL, /* stat_url */
	NULL, /* opendir */
	"ssh2.exec"
};

php_stream_wrapper php_ssh2_stream_wrapper_exec = {
	&php_ssh2_exec_stream_wops,
	NULL,
	0
};

/* {{{ proto stream ssh2_exec(resource session, string command[, string pty[, array env[, int width[, int heightp[, int width_height_type]]]]])
 * Execute a command at the remote end and allocate a channel for it
 *
 * This function has a dirty little secret.... pty and env can be in either order.... shhhh... don't tell anyone
 */
PHP_FUNCTION(ssh2_exec)
{
	LIBSSH2_SESSION *session;
	php_stream *stream;
	zval *zsession;
	zval *environment = NULL;
	zval *zpty = NULL;
	char *command;
	int command_len;
	long width = PHP_SSH2_DEFAULT_TERM_WIDTH;
	long height = PHP_SSH2_DEFAULT_TERM_HEIGHT;
	long type = PHP_SSH2_DEFAULT_TERM_UNIT;
	char *term = NULL;
	int term_len = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs|z!z!lll", &zsession, &command, &command_len, &zpty, &environment, &width, &height, &type) == FAILURE) {
		return;
	}

	if (zpty && Z_TYPE_P(zpty) == IS_ARRAY) {
		/* Swap pty and environment -- old call style */
		zval *tmp = zpty;
		zpty = environment;
		environment = tmp;
	}

	if (environment && Z_TYPE_P(environment) != IS_ARRAY) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "ssh2_exec() expects arg 4 to be of type array");
		RETURN_FALSE;
	}

	if (zpty) {
		convert_to_string(zpty);
		term = Z_STRVAL_P(zpty);
		term_len = Z_STRLEN_P(zpty);
	}

	SSH2_FETCH_AUTHENTICATED_SESSION(session, zsession);

	stream = php_ssh2_exec_command(session, Z_LVAL_P(zsession), command, term, term_len, environment, width, height, type TSRMLS_CC);
	if (!stream) {
		RETURN_FALSE;
	}

	/* Ensure that channels are freed BEFORE the sessions they belong to */
	zend_list_addref(Z_LVAL_P(zsession));

	php_stream_to_zval(stream, return_value);
}
/* }}} */

/* ***************
   * SCP Wrapper *
   *************** */

/* {{{ php_ssh2_scp_xfer
 * Make a stream from a session
 */
static php_stream *php_ssh2_scp_xfer(LIBSSH2_SESSION *session, int resource_id, char *filename TSRMLS_DC)
{
	LIBSSH2_CHANNEL *channel;
	php_ssh2_channel_data *channel_data;
	php_stream *stream;

	channel = libssh2_scp_recv(session, filename, NULL);
	if (!channel) {
		char *error = "";
		libssh2_session_last_error(session, &error, NULL, 0);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to request a channel from remote host: %s", error);
		return NULL;
	}

	/* Turn it into a stream */
	channel_data = emalloc(sizeof(php_ssh2_channel_data));
	channel_data->channel = channel;
	channel_data->streamid = 0;
	channel_data->is_blocking = 0;
	channel_data->timeout = 0;
	channel_data->session_rsrc = resource_id;
	channel_data->refcount = NULL;

	stream = php_stream_alloc(&php_ssh2_channel_stream_ops, channel_data, 0, "r");

	return stream;
}
/* }}} */

/* {{{ php_ssh2_fopen_wrapper_scp
 * ssh2.scp:// fopen wrapper (Read mode only, if you want to know why write mode isn't supported as a stream, take a look at the SCP protocol)
 */
static php_stream *php_ssh2_fopen_wrapper_scp(php_stream_wrapper *wrapper, char *path, char *mode, int options, char **opened_path, php_stream_context *context STREAMS_DC TSRMLS_DC)
{
	LIBSSH2_SESSION *session = NULL;
	php_stream *stream;
	int resource_id = 0;
	php_url *resource;

	if (strchr(mode, '+') || strchr(mode, 'a') || strchr(mode, 'w')) {
		return NULL;
	}

	resource = php_ssh2_fopen_wraper_parse_path(path, "scp", context, &session, &resource_id, NULL, NULL TSRMLS_CC);
	if (!resource || !session) {
		return NULL;
	}
	if (!resource->path) {
		php_url_free(resource);
		zend_list_delete(resource_id);
		return NULL;
	}

	stream = php_ssh2_scp_xfer(session, resource_id, resource->path TSRMLS_CC);
	if (!stream) {
		zend_list_delete(resource_id);
	}
	php_url_free(resource);

	return stream;
}
/* }}} */

static php_stream_wrapper_ops php_ssh2_scp_stream_wops = {
	php_ssh2_fopen_wrapper_scp,
	NULL, /* stream_close */
	NULL, /* stat */
	NULL, /* stat_url */
	NULL, /* opendir */
	"ssh2.scp"
};

php_stream_wrapper php_ssh2_stream_wrapper_scp = {
	&php_ssh2_scp_stream_wops,
	NULL,
	0
};

/* {{{ proto bool ssh2_scp_recv(resource session, string remote_file, string local_file)
 * Request a file via SCP
 */
PHP_FUNCTION(ssh2_scp_recv)
{
	LIBSSH2_SESSION *session;
	LIBSSH2_CHANNEL *remote_file;
	struct stat sb;
	php_stream *local_file;
	zval *zsession;
	char *remote_filename, *local_filename;
	int remote_filename_len, local_filename_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rss", &zsession,  &remote_filename, &remote_filename_len, 
																			&local_filename, &local_filename_len) == FAILURE) {
		return;
	}

	SSH2_FETCH_AUTHENTICATED_SESSION(session, zsession);

	remote_file = libssh2_scp_recv(session, remote_filename, &sb);
	if (!remote_file) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to receive remote file");
		RETURN_FALSE;
	}
	libssh2_channel_set_blocking(remote_file, 1);

	local_file = php_stream_open_wrapper(local_filename, "wb", ENFORCE_SAFE_MODE | REPORT_ERRORS, NULL);
	if (!local_file) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to write to local file");
		libssh2_channel_free(remote_file);
		RETURN_FALSE;
	}

	while (sb.st_size) {
		char buffer[8192];
		int bytes_read;

		bytes_read = libssh2_channel_read(remote_file, buffer, sb.st_size > 8192 ? 8192 : sb.st_size);
		if (bytes_read < 0) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Error reading from remote file");
			libssh2_channel_free(remote_file);
			php_stream_close(local_file);
			RETURN_FALSE;
		}
		php_stream_write(local_file, buffer, bytes_read);
		sb.st_size -= bytes_read;
	}

	libssh2_channel_free(remote_file);
	php_stream_close(local_file);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto stream ssh2_scp_send(resource session, string local_file, string remote_file[, int create_mode = 0644])
 * Send a file via SCP
 */
PHP_FUNCTION(ssh2_scp_send)
{
	LIBSSH2_SESSION *session;
	LIBSSH2_CHANNEL *remote_file;
	php_stream *local_file;
	zval *zsession;
	char *local_filename, *remote_filename;
	int local_filename_len, remote_filename_len;
	long create_mode = 0644;
	php_stream_statbuf ssb;
	int argc = ZEND_NUM_ARGS();

	if (zend_parse_parameters(argc TSRMLS_CC, "rss|l", &zsession, &local_filename, &local_filename_len, 
													   &remote_filename, &remote_filename_len, &create_mode) == FAILURE) {
		return;
	}

	SSH2_FETCH_AUTHENTICATED_SESSION(session, zsession);

	local_file = php_stream_open_wrapper(local_filename, "rb", ENFORCE_SAFE_MODE | REPORT_ERRORS, NULL);
	if (!local_file) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to read source file");
		RETURN_FALSE;
	}

	if (php_stream_stat(local_file, &ssb)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed statting local file");
		php_stream_close(local_file);
		RETURN_FALSE;
	}

	if (argc < 4) {
		create_mode = ssb.sb.st_mode & 0777;
	}

	remote_file = libssh2_scp_send_ex(session, remote_filename, create_mode, ssb.sb.st_size, ssb.sb.st_atime, ssb.sb.st_mtime);
	if (!remote_file) {
		int last_error = 0;
		char *error_msg = NULL;

		last_error = libssh2_session_last_error(session, &error_msg, NULL, 0);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failure creating remote file: %s", error_msg);
		php_stream_close(local_file);
		RETURN_FALSE;
	}
	libssh2_channel_set_blocking(remote_file, 1);

	while (ssb.sb.st_size) {
		char buffer[8192];
		size_t toread = MIN(8192, ssb.sb.st_size);
		size_t bytesread = php_stream_read(local_file, buffer, toread);
		size_t sent = 0;
		size_t justsent = 0;

		if (bytesread <= 0 || bytesread > toread) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed copying file 2");
			php_stream_close(local_file);
			libssh2_channel_free(remote_file);
			RETURN_FALSE;
		}


		while (bytesread - sent > 0) {
			if ((justsent = libssh2_channel_write(remote_file, (buffer + sent), bytesread - sent)) < 0) {

				switch (justsent) {
					case LIBSSH2_ERROR_EAGAIN:
						php_error_docref(NULL TSRMLS_CC, E_WARNING, "Operation would block");
						break;

					case LIBSSH2_ERROR_ALLOC:
						php_error_docref(NULL TSRMLS_CC,E_WARNING, "An internal memory allocation call failed");
						break;

					case LIBSSH2_ERROR_SOCKET_SEND:
						php_error_docref(NULL TSRMLS_CC,E_WARNING, "Unable to send data on socket");
						break;

					case LIBSSH2_ERROR_CHANNEL_CLOSED:
						php_error_docref(NULL TSRMLS_CC,E_WARNING, "The channel has been closed");
						break;

					case LIBSSH2_ERROR_CHANNEL_EOF_SENT:
						php_error_docref(NULL TSRMLS_CC,E_WARNING, "The channel has been requested to be closed");
						break;
				}

				php_stream_close(local_file);
				libssh2_channel_free(remote_file);
				RETURN_FALSE;
			}
			sent = sent + justsent;
		}
		ssb.sb.st_size -= bytesread;
	}

	libssh2_channel_flush_ex(remote_file, LIBSSH2_CHANNEL_FLUSH_ALL);
	php_stream_close(local_file);
	libssh2_channel_free(remote_file);
	RETURN_TRUE;
}
/* }}} */

/* ***************************
   * Direct TCP/IP Transport *
   *************************** */

/* {{{ php_ssh2_direct_tcpip
 * Make a stream from a session
 */
static php_stream *php_ssh2_direct_tcpip(LIBSSH2_SESSION *session, int resource_id, char *host, int port TSRMLS_DC)
{
	LIBSSH2_CHANNEL *channel;
	php_ssh2_channel_data *channel_data;
	php_stream *stream;

	channel = libssh2_channel_direct_tcpip(session, host, port);
	if (!channel) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to request a channel from remote host");
		return NULL;
	}

	/* Turn it into a stream */
	channel_data = emalloc(sizeof(php_ssh2_channel_data));
	channel_data->channel = channel;
	channel_data->streamid = 0;
	channel_data->is_blocking = 0;
	channel_data->timeout = 0;
	channel_data->session_rsrc = resource_id;
	channel_data->refcount = NULL;

	stream = php_stream_alloc(&php_ssh2_channel_stream_ops, channel_data, 0, "r+");

	return stream;
}
/* }}} */

/* {{{ php_ssh2_fopen_wrapper_tunnel
 * ssh2.tunnel:// fopen wrapper
 */
static php_stream *php_ssh2_fopen_wrapper_tunnel(php_stream_wrapper *wrapper, char *path, char *mode, int options, char **opened_path, php_stream_context *context STREAMS_DC TSRMLS_DC)
{
	LIBSSH2_SESSION *session = NULL;
	php_stream *stream = NULL;
	php_url *resource;
	char *host = NULL;
	int port = 0;
	int resource_id = 0;

	resource = php_ssh2_fopen_wraper_parse_path(path, "tunnel", context, &session, &resource_id, NULL, NULL TSRMLS_CC);
	if (!resource || !session) {
		return NULL;
	}

	if (resource->path && resource->path[0] == '/') {
		char *colon;

		host = resource->path + 1;
		if (*host == '[') {
			/* IPv6 Encapsulated Format */
			host++;
			colon = strstr(host, "]:");
			if (colon) {
				*colon = 0;
				colon += 2;
			}
		} else {
			colon = strchr(host, ':');
			if (colon) {
				*(colon++) = 0;
			}
		}
		if (colon) {
			port = atoi(colon);
		}
	}

	if ((port <= 0) || (port > 65535) || !host || (strlen(host) == 0)) {
		/* Invalid connection criteria */
		php_url_free(resource);
		zend_list_delete(resource_id);
		return NULL;
	}
		 
	stream = php_ssh2_direct_tcpip(session, resource_id, host, port TSRMLS_CC);
	if (!stream) {
		zend_list_delete(resource_id);
	}
	php_url_free(resource);

	return stream;
}
/* }}} */

static php_stream_wrapper_ops php_ssh2_tunnel_stream_wops = {
	php_ssh2_fopen_wrapper_tunnel,
	NULL, /* stream_close */
	NULL, /* stat */
	NULL, /* stat_url */
	NULL, /* opendir */
	"ssh2.tunnel"
};

php_stream_wrapper php_ssh2_stream_wrapper_tunnel =    {
	&php_ssh2_tunnel_stream_wops,
	NULL,
	0
};

/* {{{ proto stream ssh2_tunnel(resource session, string host, int port)
 * Tunnel to remote TCP/IP host/port
 */
PHP_FUNCTION(ssh2_tunnel)
{
	LIBSSH2_SESSION *session;
	php_stream *stream;
	zval *zsession;
	char *host;
	int host_len;
	long port;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rsl", &zsession, &host, &host_len, &port) == FAILURE) {
		return;
	}

	SSH2_FETCH_AUTHENTICATED_SESSION(session, zsession);

	stream = php_ssh2_direct_tcpip(session, Z_LVAL_P(zsession), host, port TSRMLS_CC);
	if (!stream) {
		RETURN_FALSE;
	}

	/* Ensure that channels are freed BEFORE the sessions they belong to */
	zend_list_addref(Z_LVAL_P(zsession));

	php_stream_to_zval(stream, return_value);
}
/* }}} */

/* ******************
   * Generic Helper *
   ****************** */

/* {{{ proto stream ssh2_fetch_stream(stream channel, int streamid)
 * Fetch an extended data stream
 */
PHP_FUNCTION(ssh2_fetch_stream)
{
	php_ssh2_channel_data *data, *stream_data;
	php_stream *parent, *stream;
	zval *zparent;
	long streamid;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl", &zparent, &streamid) == FAILURE) {
		return;
	}

	if (streamid < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid stream ID requested");
		RETURN_FALSE;
	}

	php_stream_from_zval(parent, &zparent);

	if (parent->ops != &php_ssh2_channel_stream_ops) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Provided stream is not of type " PHP_SSH2_CHANNEL_STREAM_NAME);
		RETURN_FALSE;
	}

	data = (php_ssh2_channel_data*)parent->abstract;

	if (!data->refcount) {
		data->refcount = emalloc(sizeof(unsigned char));
		*(data->refcount) = 1;
	}

	if (*(data->refcount) == 255) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Too many streams associated to a single channel");
		RETURN_FALSE;
	}

	(*(data->refcount))++;

	stream_data = emalloc(sizeof(php_ssh2_channel_data));
	memcpy(stream_data, data, sizeof(php_ssh2_channel_data));
	stream_data->streamid = streamid;

	stream = php_stream_alloc(&php_ssh2_channel_stream_ops, stream_data, 0, "r+");
	if (!stream) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Error opening substream");
		efree(stream_data);
		*(data->refcount)--;
		RETURN_FALSE;
	}

	php_stream_to_zval(stream, return_value);
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
