/* netssl.c - Interface to OpenSSL for upsd

   Copyright (C)
	2002	Russell Kroll <rkroll@exploits.org>
	2008	Arjen de Korte <adkorte-guest@alioth.debian.org>

   based on the original implementation:

   Copyright (C) 2002  Technorama Ltd. <oss-list-ups@technorama.net>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "config.h" /* must be the first header */
#include "common.h" /* for upsdebugx() etc */

#include <sys/types.h>
#ifndef WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#else	/* WIN32 */
#include "wincompat.h"
#endif	/* WIN32 */

#include "upsd.h"
#include "neterr.h"
#include "netssl.h"
#include "nut_stdint.h"

#ifdef WITH_NSS
#	include <pk11pub.h>
#	include <prinit.h>
#	include <private/pprio.h>
#if defined(NSS_VMAJOR) && (NSS_VMAJOR > 3 || (NSS_VMAJOR == 3 && defined(NSS_VMINOR) && NSS_VMINOR >= 39))
#	include <keyhi.h>
#	include <keythi.h>
#else
#	include <key.h>
#	include <keyt.h>
#endif /* NSS before 3.39 */
#	include <secerr.h>
#	include <sslerr.h>
#	include <sslproto.h>
#endif /* WITH_NSS */

char	*certfile = NULL;
char	*certname = NULL;
char	*certpasswd = NULL;

/* Warning: in this release of NUT, this feature is disabled by default
 * in order to retain compatibility with "least surprise" for earlier
 * existing deployments. Over time it can become enabled by default.
 * See upsd.conf option DISABLE_WEAK_SSL to toggle this in-vivo.
 */
int	disable_weak_ssl = 0;

#ifdef WITH_CLIENT_CERTIFICATE_VALIDATION
int certrequest = 0;
#endif /* WITH_CLIENT_CERTIFICATE_VALIDATION */

static int	ssl_initialized = 0;

#ifndef WITH_SSL

/* stubs for non-ssl compiles */
void net_starttls(nut_ctype_t *client, size_t numarg, const char **arg)
{
	NUT_UNUSED_VARIABLE(client);
	NUT_UNUSED_VARIABLE(numarg);
	NUT_UNUSED_VARIABLE(arg);

	send_err(client, NUT_ERR_FEATURE_NOT_SUPPORTED);
	return;
}

ssize_t ssl_write(nut_ctype_t *client, const char *buf, size_t buflen)
{
	NUT_UNUSED_VARIABLE(client);
	NUT_UNUSED_VARIABLE(buf);
	NUT_UNUSED_VARIABLE(buflen);

	upslogx(LOG_ERR, "ssl_write called but SSL wasn't compiled in");
	return -1;
}

ssize_t ssl_read(nut_ctype_t *client, char *buf, size_t buflen)
{
	NUT_UNUSED_VARIABLE(client);
	NUT_UNUSED_VARIABLE(buf);
	NUT_UNUSED_VARIABLE(buflen);

	upslogx(LOG_ERR, "ssl_read called but SSL wasn't compiled in");
	return -1;
}

void ssl_init(void)
{
	ssl_initialized = 0;	/* keep gcc quiet */
}

void ssl_finish(nut_ctype_t *client)
{
	if (client->ssl) {
		upslogx(LOG_ERR, "ssl_finish found active SSL connection but SSL wasn't compiled in");
	}
}

void ssl_cleanup(void)
{
}

#else

#ifdef WITH_OPENSSL

static SSL_CTX	*ssl_ctx = NULL;

static void ssl_debug(void)
{
	unsigned long	e;
	char	errmsg[SMALLBUF];

	while ((e = ERR_get_error()) != 0) {
		ERR_error_string_n(e, errmsg, sizeof(errmsg));
		upsdebugx(1, "ssl_debug: %s", errmsg);
	}
}

static int ssl_error(SSL *ssl, ssize_t ret)
{
	int	e;

	if (ret >= INT_MAX) {
		upslogx(LOG_ERR, "ssl_error() ret=%" PRIiSIZE " would not fit in an int", ret);
		return -1;
	}
	e = SSL_get_error(ssl, (int)ret);

	switch (e)
	{
	case SSL_ERROR_WANT_READ:
		upsdebugx(1, "ssl_error() ret=%" PRIiSIZE " SSL_ERROR_WANT_READ", ret);
		break;

	case SSL_ERROR_WANT_WRITE:
		upsdebugx(1, "ssl_error() ret=%" PRIiSIZE " SSL_ERROR_WANT_WRITE", ret);
		break;

	case SSL_ERROR_SYSCALL:
		if (ret == 0 && ERR_peek_error() == 0) {
			upsdebugx(1, "ssl_error() EOF from client");
		} else {
			upsdebugx(1, "ssl_error() ret=%" PRIiSIZE " SSL_ERROR_SYSCALL", ret);
		}
		break;

	default:
		upsdebugx(1, "ssl_error() ret=%" PRIiSIZE " SSL_ERROR %d", ret, e);
		ssl_debug();
	}

	return -1;
}

#elif defined(WITH_NSS) /* WITH_OPENSSL */

static CERTCertificate *cert;
static SECKEYPrivateKey *privKey;

static char *nss_password_callback(PK11SlotInfo *slot, PRBool retry,
		void *arg)
{
	NUT_UNUSED_VARIABLE(arg);

	if (retry) {
		/* Force not inted to retrieve password many times. */
		return NULL;
	}
	upslogx(LOG_INFO, "Intend to retrieve password for %s / %s: password %sconfigured",
		PK11_GetSlotName(slot), PK11_GetTokenName(slot), certpasswd?"":"not ");
	return certpasswd ? PL_strdup(certpasswd) : NULL;
}

static void nss_error(const char* text)
{
	char buffer[SMALLBUF];
	PRErrorCode err_num = PR_GetError();
	PRInt32 err_len = PR_GetErrorTextLength();

	if (err_len > 0) {
		if (err_len < SMALLBUF) {
			PR_GetErrorText(buffer);
			upsdebugx(1, "nss_error %ld in %s : %s", (long)err_num, text, buffer);
		}else{
			upsdebugx(1, "nss_error %ld in %s : Internal error buffer too small, needs %ld bytes", (long)err_num, text, (long)err_len);
		}
	}else{
		upsdebugx(1, "nss_error %ld in %s", (long)PR_GetError(), text);
	}
}

static int ssl_error(PRFileDesc *ssl, ssize_t ret)
{
	char buffer[256];
	PRErrorCode err_num = PR_GetError();
	PRInt32 err_len = PR_GetErrorTextLength();
	PRInt32 length;
	NUT_UNUSED_VARIABLE(ssl);
	NUT_UNUSED_VARIABLE(ret);

	if (err_len > 0) {
		if (err_len < SMALLBUF) {
			length = PR_GetErrorText(buffer);
			upsdebugx(1, "ssl_error %ld : %*s", (long)err_num, length, buffer);
		}else{
			upsdebugx(1, "ssl_error %ld : Internal error buffer too small, needs %ld bytes", (long)err_num, (long)err_len);
		}
	}else{
		upsdebugx(1, "ssl_error %ld", (long)err_num);
	}

	return -1;
}

static SECStatus AuthCertificate(CERTCertDBHandle *arg, PRFileDesc *fd,
	PRBool checksig, PRBool isServer)
{
	nut_ctype_t *client  = (nut_ctype_t *)SSL_RevealPinArg(fd);
	SECStatus status = SSL_AuthCertificate(arg, fd, checksig, isServer);
	upslogx(LOG_INFO, "Intend to authenticate client %s : %s.",
		client?client->addr:"(unnamed)",
		status==SECSuccess?"SUCCESS":"FAILED");
	return status;
}

static SECStatus BadCertHandler(nut_ctype_t *arg, PRFileDesc *fd)
{
	NUT_UNUSED_VARIABLE(fd);

	upslogx(LOG_WARNING, "Certificate validation failed for %s",
		(arg&&arg->addr)?arg->addr:"<unnamed>");
#ifdef WITH_CLIENT_CERTIFICATE_VALIDATION
	/* BadCertHandler is called when the NSS certificate validation is failed.
	 * If the certificate verification (user conf) is mandatory, reject authentication
	 * else accept it.
	 */
	return certrequest==NETSSL_CERTREQ_REQUIRE?SECFailure:SECSuccess;
#else /* WITH_CLIENT_CERTIFICATE_VALIDATION */
	/* Always accept clients. */
	return SECSuccess;
#endif /* WITH_CLIENT_CERTIFICATE_VALIDATION */
}

static void HandshakeCallback(PRFileDesc *fd, nut_ctype_t *client_data)
{
	NUT_UNUSED_VARIABLE(fd);

	upslogx(LOG_INFO, "SSL handshake done successfully with client %s",
		client_data->addr);
}


#endif /* WITH_OPENSSL | WITH_NSS */

void net_starttls(nut_ctype_t *client, size_t numarg, const char **arg)
{
#ifdef WITH_OPENSSL
	int ret;
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	SECStatus	status;
	PRFileDesc	*socket;
#endif /* WITH_OPENSSL | WITH_NSS */

	NUT_UNUSED_VARIABLE(numarg);
	NUT_UNUSED_VARIABLE(arg);

	if (client->ssl) {
		send_err(client, NUT_ERR_ALREADY_SSL_MODE);
		return;
	}

	client->ssl_connected = 0;

	if ((!certfile) || (!ssl_initialized)) {
		send_err(client, NUT_ERR_FEATURE_NOT_CONFIGURED);
		return;
	}

#ifdef WITH_OPENSSL
	if (!ssl_ctx)
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	if (!NSS_IsInitialized())
#endif /* WITH_OPENSSL | WITH_NSS */
	{
		send_err(client, NUT_ERR_FEATURE_NOT_CONFIGURED);
		ssl_initialized = 0;
		return;
	}

	if (!sendback(client, "OK STARTTLS\n")) {
		return;
	}

#ifdef WITH_OPENSSL

	client->ssl = SSL_new(ssl_ctx);

	if (!client->ssl) {
		upslog_with_errno(LOG_ERR, "SSL_new failed\n");
		ssl_debug();
		return;
	}

	if (SSL_set_fd(client->ssl, client->sock_fd) != 1) {
		upslog_with_errno(LOG_ERR, "SSL_set_fd failed\n");
		ssl_debug();
		return;
	}

	ret = SSL_accept(client->ssl);
	switch (ret)
	{
	case 1:
		client->ssl_connected = 1;
		upsdebugx(3, "SSL connected (%s)", SSL_get_version(client->ssl));
		break;

	case 0:
		upslog_with_errno(LOG_ERR, "SSL_accept do not accept handshake.");
		ssl_error(client->ssl, ret);
		break;

	case -1:
		upslog_with_errno(LOG_ERR, "Unknown return value from SSL_accept");
		ssl_error(client->ssl, ret);
		break;
	default:
		break;
	}

#elif defined(WITH_NSS) /* WITH_OPENSSL */

	socket = PR_ImportTCPSocket(client->sock_fd);
	if (socket == NULL) {
		upslogx(LOG_ERR, "Can not initialize SSL connection");
		nss_error("net_starttls / PR_ImportTCPSocket");
		return;
	}

	client->ssl = SSL_ImportFD(NULL, socket);
	if (client->ssl == NULL) {
		upslogx(LOG_ERR, "Can not initialize SSL connection");
		nss_error("net_starttls / SSL_ImportFD");
		return;
	}

	if (SSL_SetPKCS11PinArg(client->ssl, client) == -1) {
		upslogx(LOG_ERR, "Can not initialize SSL connection");
		nss_error("net_starttls / SSL_SetPKCS11PinArg");
		return;
	}

#if (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_PUSH_POP) && (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_CAST_FUNCTION_TYPE_STRICT)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type-strict"
#endif
	/* Note cast to SSLAuthCertificate to prevent warning due to
	 * bad function prototype in NSS.
	 */
	status = SSL_AuthCertificateHook(client->ssl, (SSLAuthCertificate)AuthCertificate, CERT_GetDefaultCertDB());
	if (status != SECSuccess) {
		upslogx(LOG_ERR, "Can not initialize SSL connection");
		nss_error("net_starttls / SSL_AuthCertificateHook");
		return;
	}

	status = SSL_BadCertHook(client->ssl, (SSLBadCertHandler)BadCertHandler, client);
	if (status != SECSuccess) {
		upslogx(LOG_ERR, "Can not initialize SSL connection");
		nss_error("net_starttls / SSL_BadCertHook");
		return;
	}

	status = SSL_HandshakeCallback(client->ssl, (SSLHandshakeCallback)HandshakeCallback, client);
	if (status != SECSuccess) {
		upslogx(LOG_ERR, "Can not initialize SSL connection");
		nss_error("net_starttls / SSL_HandshakeCallback");
		return;
	}

	status = SSL_ConfigSecureServer(client->ssl, cert, privKey, NSS_FindCertKEAType(cert));
	if (status != SECSuccess) {
		upslogx(LOG_ERR, "Can not initialize SSL connection");
		nss_error("net_starttls / SSL_ConfigSecureServer");
		return;
	}
#if (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_PUSH_POP) && (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_CAST_FUNCTION_TYPE_STRICT)
#pragma GCC diagnostic pop
#endif

	status = SSL_ResetHandshake(client->ssl, PR_TRUE);
	if (status != SECSuccess) {
		upslogx(LOG_ERR, "Can not initialize SSL connection");
		nss_error("net_starttls / SSL_ResetHandshake");
		return;
	}

	/* Note: this call can generate memory leaks not resolvable
	 * by any release function.
	 * Probably SSL session key object allocation. */
	status = SSL_ForceHandshake(client->ssl);
	if (status != SECSuccess) {
		PRErrorCode code = PR_GetError();
		if (code==SSL_ERROR_NO_CERTIFICATE) {
			upslogx(LOG_WARNING, "Client %s do not provide certificate.",
				client->addr);
		} else {
			nss_error("net_starttls / SSL_ForceHandshake");
			/* TODO : Close the connection. */
			return;
		}
	}
	client->ssl_connected = 1;
#endif /* WITH_OPENSSL | WITH_NSS */
}

void ssl_init(void)
{
#ifdef WITH_NSS
	SECStatus status;
#if defined(NSS_VMAJOR) && (NSS_VMAJOR > 3 || (NSS_VMAJOR == 3 && defined(NSS_VMINOR) && NSS_VMINOR >= 14))
	SSLVersionRange range;
#endif
#endif /* WITH_NSS */

	if (!certfile) {
		return;
	}

	check_perms(certfile);
	if (!disable_weak_ssl)
		upslogx(LOG_WARNING, "Warning: DISABLE_WEAK_SSL is not enabled. Please consider enabling to improve network security.");

#ifdef WITH_OPENSSL

#if OPENSSL_VERSION_NUMBER < 0x10100000L
	SSL_load_error_strings();
	SSL_library_init();

	ssl_ctx = SSL_CTX_new(SSLv23_server_method());
#else
	ssl_ctx = SSL_CTX_new(TLS_server_method());
#endif

	if (!ssl_ctx) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_new failed");
	}

	SSL_CTX_set_options(ssl_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	/* set minimum protocol TLSv1 */
	SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
	if (disable_weak_ssl) {
#if defined(SSL_OP_NO_TLSv1_2)
		SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
#elif defined(SSL_OP_NO_TLSv1_1)
		SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_TLSv1);
#endif
	}
#else
	if (SSL_CTX_set_min_proto_version(ssl_ctx, disable_weak_ssl ? TLS1_2_VERSION : TLS1_VERSION) != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_set_min_proto_version(TLS1_VERSION)");
	}
#endif

	if (SSL_CTX_use_certificate_chain_file(ssl_ctx, certfile) != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_use_certificate_chain_file(%s) failed", certfile);
	}

	if (SSL_CTX_use_PrivateKey_file(ssl_ctx, certfile, SSL_FILETYPE_PEM) != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_use_PrivateKey_file(%s) failed", certfile);
	}

	if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_check_private_key(%s) failed", certfile);
	}

	if (SSL_CTX_set_cipher_list(ssl_ctx, "HIGH:@STRENGTH") != 1) {
		ssl_debug();
		fatalx(EXIT_FAILURE, "SSL_CTX_set_cipher_list failed");
	}

	SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);

	ssl_initialized = 1;

#elif defined(WITH_NSS) /* WITH_OPENSSL */

	if (!certname || certname[0]==0 ) {
		upslogx(LOG_ERR, "The SSL certificate name is not specified.");
		return;
	}

	PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 0);

	PK11_SetPasswordFunc(nss_password_callback);

	/* Note: this call can generate memory leaks not resolvable
	 * by any release function.
	 * Probably NSS key module object allocation and
	 * probably NSS key db object allocation too. */
	status = NSS_Init(certfile);
	if (status != SECSuccess) {
		upslogx(LOG_ERR, "Can not initialize SSL context");
		nss_error("ssl_init / NSS_Init");
		return;
	}

	status = NSS_SetDomesticPolicy();
	if (status != SECSuccess) {
		upslogx(LOG_ERR, "Can not initialize SSL policy");
		nss_error("ssl_init / NSS_SetDomesticPolicy");
		return;
	}

	/* Default server cache config */
	status = SSL_ConfigServerSessionIDCache(0, 0, 0, NULL);
	if (status != SECSuccess) {
		upslogx(LOG_ERR, "Can not initialize SSL server cache");
		nss_error("ssl_init / SSL_ConfigServerSessionIDCache");
		return;
	}

	if (!disable_weak_ssl) {
		status = SSL_OptionSetDefault(SSL_ENABLE_SSL3, PR_TRUE);
		if (status != SECSuccess) {
			upslogx(LOG_ERR, "Can not enable SSLv3");
			nss_error("ssl_init / SSL_OptionSetDefault(SSL_ENABLE_SSL3)");
			return;
		}
		status = SSL_OptionSetDefault(SSL_ENABLE_TLS, PR_TRUE);
		if (status != SECSuccess) {
			upslogx(LOG_ERR, "Can not enable TLSv1");
			nss_error("ssl_init / SSL_OptionSetDefault(SSL_ENABLE_TLS)");
			return;
		}
	} else {
#if defined(NSS_VMAJOR) && (NSS_VMAJOR > 3 || (NSS_VMAJOR == 3 && defined(NSS_VMINOR) && NSS_VMINOR >= 14))
		status = SSL_VersionRangeGetSupported(ssl_variant_stream, &range);
		if (status != SECSuccess) {
			upslogx(LOG_ERR, "Can not get versions supported");
			nss_error("ssl_init / SSL_VersionRangeGetSupported");
			return;
		}
		range.min = SSL_LIBRARY_VERSION_TLS_1_1;
#ifdef SSL_LIBRARY_VERSION_TLS_1_2
		range.min = SSL_LIBRARY_VERSION_TLS_1_2;
#endif
		status = SSL_VersionRangeSetDefault(ssl_variant_stream, &range);
		if (status != SECSuccess) {
			upslogx(LOG_ERR, "Can not set versions supported");
			nss_error("ssl_init / SSL_VersionRangeSetDefault");
			return;
		}
		/* Disable old/weak ciphers */
		SSL_CipherPrefSetDefault(TLS_DHE_RSA_WITH_3DES_EDE_CBC_SHA, PR_FALSE);
		SSL_CipherPrefSetDefault(TLS_RSA_WITH_3DES_EDE_CBC_SHA, PR_FALSE);
		SSL_CipherPrefSetDefault(TLS_RSA_WITH_RC4_128_SHA, PR_FALSE);
		SSL_CipherPrefSetDefault(TLS_RSA_WITH_RC4_128_MD5, PR_FALSE);
#else
		status = SSL_OptionSetDefault(SSL_ENABLE_SSL3, PR_FALSE);
		if (status != SECSuccess) {
			upslogx(LOG_ERR, "Can not disable SSLv3");
			nss_error("ssl_init / SSL_OptionSetDefault(SSL_DISABLE_SSL3)");
			return;
		}
		status = SSL_OptionSetDefault(SSL_ENABLE_TLS, PR_TRUE);
		if (status != SECSuccess) {
			upslogx(LOG_ERR, "Can not enable TLSv1");
			nss_error("ssl_init / SSL_OptionSetDefault(SSL_ENABLE_TLS)");
			return;
		}
#endif
	}

#ifdef WITH_CLIENT_CERTIFICATE_VALIDATION
	if (certrequest < NETSSL_CERTREQ_NO &&
		certrequest > NETSSL_CERTREQ_REQUEST) {
		upslogx(LOG_ERR, "Invalid certificate requirement");
		return;
	}

	if (certrequest == NETSSL_CERTREQ_REQUEST ||
		certrequest == NETSSL_CERTREQ_REQUIRE ) {
		status = SSL_OptionSetDefault(SSL_REQUEST_CERTIFICATE, PR_TRUE);
		if (status != SECSuccess) {
			upslogx(LOG_ERR, "Can not enable certificate request");
			nss_error("ssl_init / SSL_OptionSetDefault(SSL_REQUEST_CERTIFICATE)");
			return;
		}
	}

	if (certrequest == NETSSL_CERTREQ_REQUIRE ) {
		status = SSL_OptionSetDefault(SSL_REQUIRE_CERTIFICATE, PR_TRUE);
		if (status != SECSuccess) {
			upslogx(LOG_ERR, "Can not enable certificate requirement");
			nss_error("ssl_init / SSL_OptionSetDefault(SSL_REQUIRE_CERTIFICATE)");
			return;
		}
	}
#endif /* WITH_CLIENT_CERTIFICATE_VALIDATION */

	cert = PK11_FindCertFromNickname(certname, NULL);
	if(cert==NULL)	{
		upslogx(LOG_ERR, "Can not find server certificate");
		nss_error("ssl_init / PK11_FindCertFromNickname");
		return;
	}

	privKey = PK11_FindKeyByAnyCert(cert, NULL);
	if(privKey==NULL){
		upslogx(LOG_ERR, "Can not find private key associate to server certificate");
		nss_error("ssl_init / PK11_FindKeyByAnyCert");
		return;
	}

	ssl_initialized = 1;
#else /* WITH_OPENSSL | WITH_NSS */
	upslogx(LOG_ERR, "ssl_init called but SSL wasn't compiled in");
#endif /* WITH_OPENSSL | WITH_NSS */
}

#if (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_PUSH_POP_BESIDEFUNC) && ( (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TYPE_LIMITS_BESIDEFUNC) || (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TAUTOLOGICAL_CONSTANT_OUT_OF_RANGE_COMPARE_BESIDEFUNC) )
# pragma GCC diagnostic push
#endif
#ifdef HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TYPE_LIMITS_BESIDEFUNC
# pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#ifdef HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TAUTOLOGICAL_CONSTANT_OUT_OF_RANGE_COMPARE_BESIDEFUNC
# pragma GCC diagnostic ignored "-Wtautological-constant-out-of-range-compare"
#endif
ssize_t ssl_read(nut_ctype_t *client, char *buf, size_t buflen)
{
	ssize_t	ret = -1;
#ifdef WITH_OPENSSL
	int	iret;
#endif

	if (!client->ssl_connected) {
		return -1;
	}

#ifdef WITH_OPENSSL
	/* SSL_* routines deal with int type for return and buflen
	 * We might need to window our I/O if we exceed 2GB (in
	 * 32-bit builds)... Not likely to exceed in 64-bit builds,
	 * but smaller systems with 16-bits might be endangered :)
	 */
	assert(buflen <= INT_MAX);
	iret = SSL_read(client->ssl, buf, (int)buflen);
	assert(iret <= SSIZE_MAX);
	ret = (ssize_t)iret;
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	/* PR_* routines deal in PRInt32 type
	 * We might need to window our I/O if we exceed 2GB :) */
	assert(buflen <= PR_INT32_MAX);
	ret = PR_Read(client->ssl, buf, (PRInt32)buflen);
#endif /* WITH_OPENSSL | WITH_NSS */

	if (ret < 1) {
		ssl_error(client->ssl, ret);
		return -1;
	}

	return ret;
}

ssize_t ssl_write(nut_ctype_t *client, const char *buf, size_t buflen)
{
	ssize_t	ret = -1;
#ifdef WITH_OPENSSL
	int	iret;
#endif

	if (!client->ssl_connected) {
		return -1;
	}

#ifdef WITH_OPENSSL
	/* SSL_* routines deal with int type for return and buflen
	 * We might need to window our I/O if we exceed 2GB (in
	 * 32-bit builds)... Not likely to exceed in 64-bit builds,
	 * but smaller systems with 16-bits might be endangered :)
	 */
	assert(buflen <= INT_MAX);
	iret = SSL_write(client->ssl, buf, (int)buflen);
	assert(iret <= SSIZE_MAX);
	ret = (ssize_t)iret;
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	/* PR_* routines deal in PRInt32 type
	 * We might need to window our I/O if we exceed 2GB :) */
	assert(buflen <= PR_INT32_MAX);
	ret = PR_Write(client->ssl, buf, (PRInt32)buflen);
#endif /* WITH_OPENSSL | WITH_NSS */

	upsdebugx(5, "ssl_write ret=%" PRIiSIZE, ret);

	return ret;
}
#if (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_PUSH_POP_BESIDEFUNC) && ( (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TYPE_LIMITS_BESIDEFUNC) || (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TAUTOLOGICAL_CONSTANT_OUT_OF_RANGE_COMPARE_BESIDEFUNC) )
# pragma GCC diagnostic pop
#endif

void ssl_finish(nut_ctype_t *client)
{
	if (client->ssl) {
#ifdef WITH_OPENSSL
		SSL_free(client->ssl);
#elif defined(WITH_NSS)
		PR_Shutdown(client->ssl, PR_SHUTDOWN_BOTH);
		PR_Close(client->ssl);
#endif /* WITH_OPENSSL | WITH_NSS */
		client->ssl_connected = 0;
		client->ssl = NULL;
	}
}

void ssl_cleanup(void)
{
#ifdef WITH_OPENSSL
	if (ssl_ctx) {
		SSL_CTX_free(ssl_ctx);
		ssl_ctx = NULL;
	}
#elif defined(WITH_NSS) /* WITH_OPENSSL */
	CERT_DestroyCertificate(cert);
    SECKEY_DestroyPrivateKey(privKey);
	NSS_Shutdown();
	PR_Cleanup();
	/* Called to release memory arena used by NSS/NSPR.
	 * Prevent to show all PL_ArenaAllocate mem alloc as leaks.
	 * https://developer.mozilla.org/en/NSS_Memory_allocation
	 */
	PL_ArenaFinish();
#endif /* WITH_OPENSSL | WITH_NSS */
	ssl_initialized = 0;
}

#endif /* WITH_SSL */
