/*
   authmode.c

   This file is part of GNU Anubis.
   Copyright (C) 2003 The Anubis Team.

   GNU Anubis is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GNU Anubis is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GNU Anubis; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   GNU Anubis is released under the GPL with the additional exemption that
   compiling, linking, and/or using OpenSSL is allowed.
*/

#include "headers.h"
#include "extern.h"
#include "lbuf.h"
#include <gsasl.h>

static Gsasl_ctx *ctx;   


/* Basic I/O Functions */

struct anubis_gsasl_stream {
	Gsasl_session_ctx *sess_ctx; /* Context */
	struct _line_buffer *lb;
	void *net_io;
};

static const char *
_gsasl_strerror(int rc)
{
	return gsasl_strerror(rc);
}

int
write_chunk (void *data, char *start, char *end)
{
	struct anubis_gsasl_stream *s = data;
	size_t chunk_size = end - start + 1;
	size_t len;
	size_t wrsize;
	char *buf = NULL;
      
	len = UINT_MAX; /* override the bug in libgsasl */
	gsasl_encode (s->sess_ctx, start, chunk_size, NULL, &len);
	buf = malloc (len);
	if (!buf)
		return ENOMEM;

	gsasl_encode (s->sess_ctx, start, chunk_size, buf, &len);

	wrsize = 0;
	do {
		size_t sz;
		int rc = net_io_write(s->net_io, buf + wrsize, len - wrsize,
				      &sz);
		if (rc) {
			if (rc == EINTR)
				continue;
			free (buf);
			return rc;
		}
		wrsize += sz;
	}
	while (wrsize < len);
  
	free (buf);
	
	return 0;
}


static int
_gsasl_write(void *sd, char *data, size_t size, size_t *nbytes)
{
	struct anubis_gsasl_stream *s = sd;
	int rc = _auth_lb_grow (s->lb, data, size);
	if (rc)
		return rc;

	return _auth_lb_writelines (s->lb, data, size, 
				    write_chunk, s, nbytes);      
}

static int
_gsasl_read(void *sd, char *data, size_t size, size_t *nbytes)
{
	size_t len;
	struct anubis_gsasl_stream *s = sd;
	int rc;
	char *bufp;
	
	do {
		char buf[80];
		size_t sz;
		
		rc = net_io_read(s->net_io, buf, sizeof (buf), &sz);
		if (rc) {
			if (rc == EINTR)
				continue;
			return rc;
		}

		rc = _auth_lb_grow (s->lb, buf, sz);
		if (rc)
			return rc;

		len = UINT_MAX; /* override the bug in libgsasl */
		rc = gsasl_decode(s->sess_ctx,
				  _auth_lb_data(s->lb),
				  _auth_lb_level(s->lb),
				  NULL, &len);
	} while (rc == GSASL_NEEDS_MORE);

	if (rc != GSASL_OK) 
		return rc;

	bufp = malloc (len + 1);
	if (!bufp)
		return ENOMEM;
	rc = gsasl_decode (s->sess_ctx,
			   _auth_lb_data (s->lb),
			   _auth_lb_level (s->lb),
			   bufp, &len);

	if (rc != GSASL_OK) {
		free(bufp);
		return rc;
	}

	if (len > size) {
		memcpy(data, bufp, size);
		_auth_lb_drop (s->lb);
		_auth_lb_grow (s->lb, bufp + size, len - size);
		len = size;
	} else {
		_auth_lb_drop(s->lb);
		memcpy(data, bufp, len);
	}
	if (nbytes)
		*nbytes = len;
	
	free (bufp);
	return 0;
}

static int
_gsasl_close(void *sd)
{
	struct anubis_gsasl_stream *s = sd;

	net_io_close(s->net_io);
	if (s->sess_ctx)
		gsasl_server_finish (s->sess_ctx);
	_auth_lb_destroy (&s->lb);
	return 0;
}

static void
install_gsasl_stream (Gsasl_session_ctx *sess_ctx)
{
	struct anubis_gsasl_stream *s = malloc(sizeof *s);

	s->sess_ctx = sess_ctx;
	_auth_lb_create (&s->lb);

	s->net_io = net_io_get(SERVER, remote_client);
	net_set_io(SERVER, _gsasl_read, _gsasl_write,
		   _gsasl_close, _gsasl_strerror);
		   remote_client = s;
}


/* GSASL Authentication */

#define AUTHBUFSIZE 512

static int
auth_step_base64(Gsasl_session_ctx *sess_ctx, char *input,
		 char **output, size_t *output_len)
{
	int rc;
	
	while (1) {
		rc = gsasl_server_step_base64 (sess_ctx,
					       input,
					       *output, *output_len);

		if (rc == GSASL_TOO_SMALL_BUFFER) {
			*output_len += AUTHBUFSIZE;
			*output = realloc(*output, *output_len);
			if (output)
				continue; 
		}
		break;
	}
	return rc;
}

int
anubis_auth_gsasl (char *auth_type, char *arg, char **username)
{
	char *input = arg;
	size_t input_size = 0;
	char *output;
	size_t output_len;
	int rc;
	Gsasl_session_ctx *sess_ctx = NULL; 

	info(DEBUG, "mech=%s, inp=%s", auth_type, arg);
	rc = gsasl_server_start (ctx, auth_type, &sess_ctx);
	if (rc != GSASL_OK) {
		info(NORMAL, _("SASL gsasl_server_start: %s"),
		     gsasl_strerror(rc));
		asmtp_reply(504, "%s", gsasl_strerror(rc));
		return 1;
	}
	
	gsasl_server_application_data_set (sess_ctx, username);
	
	output_len = AUTHBUFSIZE;
	output = malloc (output_len);
	if (!output)
		anubis_error(HARD, _("Not enough memory"));

	output[0] = '\0';

	/* RFC 2554 4.:
	    Unlike a zero-length client answer to a 334 reply, a zero-
            length initial response is sent as a single equals sign */
	if (input && strcmp(input, "=") == 0)
		input = "";
	
	while ((rc = auth_step_base64 (sess_ctx, input, &output, &output_len))
	       == GSASL_NEEDS_MORE) {
		asmtp_reply (334, "%s", output);
		recvline_ptr(CLIENT, remote_server, &input, &input_size);
		remcrlf(input);
		if (strcmp (input, "*") == 0) {
			asmtp_reply (501, "AUTH aborted");
			return 1;
		}
	}
  
	if (input_size)
		free (input);
	
	if (rc != GSASL_OK) {
		info (NORMAL,
		      _("GSASL error: %s"), gsasl_strerror (rc));
		free (output);
		asmtp_reply(501, "Authentication failed");
		return 1;
	}

	/* Some SASL mechanisms output data when GSASL_OK is returned */
	if (output[0])
		asmtp_reply (334, "%s", output);
  
	free (output);
	info(NORMAL, "Authentication passed. User name %s", *username);
	     
	if (*username == NULL) {
		info (NORMAL,
		      _("GSASL %s: cannot get username"), auth_type);
		asmtp_reply(535, "Authentication failed"); /* FIXME */
		return 1;
	}

	if (sess_ctx) {
		/* FIXME! */
		install_gsasl_stream (sess_ctx);
	}

	asmtp_reply (235, "Authentication successful.");
	return 0;
}


/* Capability list handling */

static void
auth_gsasl_capa_init ()
{
	int rc;
	char *listmech;
	size_t size;

	rc = gsasl_server_listmech(ctx, NULL, &size);
	if (rc != GSASL_OK)
		return;

	listmech = malloc(size);
	if (!listmech)
		return;
  
	rc = gsasl_server_listmech(ctx, listmech, &size);
	if (rc != GSASL_OK)
		return;

	/*FIXME: Configurable subset of auth mechanisms and
	  intersection function*/
	asmtp_capa_add_prefix("AUTH", listmech);
      
	free(listmech);
}


/* Various callback functions */

/* This is for DIGEST-MD5 */
static int
cb_realm (Gsasl_session_ctx *ctx, char *out, size_t *outlen, size_t nth)
{
	char *realm = get_localname ();

	if (nth > 0)
		return GSASL_NO_MORE_REALMS;

	if (out) {
		if (*outlen < strlen (realm))
			return GSASL_TOO_SMALL_BUFFER;
		memcpy (out, realm, strlen (realm));
	}

	*outlen = strlen (realm);
	
	return GSASL_OK;
}

static int
cb_validate (Gsasl_session_ctx *ctx,
	     const char *authorization_id,
	     const char *authentication_id,
	     const char *password)
{
	char **username = gsasl_server_application_data_get (ctx);

	*username = strdup (authentication_id ?
			    authentication_id : authorization_id);
	return GSASL_OK;
}

#define GSSAPI_SERVICE "anubis"

static int
cb_service (Gsasl_session_ctx *ctx, char *srv, size_t *srvlen,
	    char *host, size_t *hostlen)
{
	char *hostname = get_localname ();

	if (srv) {
		if (*srvlen < strlen (GSSAPI_SERVICE))
			return GSASL_TOO_SMALL_BUFFER;
		
		memcpy (srv, GSSAPI_SERVICE, strlen (GSSAPI_SERVICE));
	}

	if (srvlen)
		*srvlen = strlen (GSSAPI_SERVICE);

	if (host) {
		if (*hostlen < strlen (hostname))
			return GSASL_TOO_SMALL_BUFFER;
		
		memcpy (host, hostname, strlen (hostname));
	}
	
	if (hostlen)
		*hostlen = strlen (hostname);
	
	return GSASL_OK;
}

/* This gets called when SASL mechanism EXTERNAL is invoked */
static int
cb_external (Gsasl_session_ctx *ctx)
{
	return GSASL_AUTHENTICATION_ERROR;
}

/* This gets called when SASL mechanism CRAM-MD5 or DIGEST-MD5 is invoked */

static int
cb_retrieve (Gsasl_session_ctx *ctx,
	     const char *authentication_id,
	     const char *authorization_id,
	     const char *realm,
	     char *key,
	     size_t *keylen)
{
	ANUBIS_USER usr;
	char **username = gsasl_server_application_data_get (ctx);

	if (username && authentication_id)
		*username = strdup (authentication_id);

	if (anubis_get_db_record(*username, &usr) == ANUBIS_DB_SUCCESS) {
		if (key) 
			strncpy(key, usr.smtp_passwd, *keylen);
		else
			*keylen = strlen(usr.smtp_passwd);
		return GSASL_OK;
	}
	
	return GSASL_AUTHENTICATION_ERROR;
}


/* Initialization function */
void
auth_gsasl_init ()
{
  int rc;
  
  rc = gsasl_init (&ctx);
  if (rc != GSASL_OK) {
	  info (NORMAL, _("cannot initialize libgsasl: %s"),
		gsasl_strerror (rc));
  }

  gsasl_server_callback_realm_set (ctx, cb_realm);
  gsasl_server_callback_external_set (ctx, cb_external);
  gsasl_server_callback_validate_set (ctx, cb_validate);
  gsasl_server_callback_service_set (ctx, cb_service);
  gsasl_server_callback_retrieve_set (ctx, cb_retrieve);
  
  auth_gsasl_capa_init (0);
}

