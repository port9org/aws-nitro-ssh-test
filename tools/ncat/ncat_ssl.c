/***************************************************************************
 * ncat_ssl.c -- SSL support functions.                                    *
 ***********************IMPORTANT NMAP LICENSE TERMS************************
 *                                                                         *
 * The Nmap Security Scanner is (C) 1996-2022 Nmap Software LLC ("The Nmap *
 * Project"). Nmap is also a registered trademark of the Nmap Project.     *
 *                                                                         *
 * This program is distributed under the terms of the Nmap Public Source   *
 * License (NPSL). The exact license text applying to a particular Nmap    *
 * release or source code control revision is contained in the LICENSE     *
 * file distributed with that version of Nmap or source code control       *
 * revision. More Nmap copyright/legal information is available from       *
 * https://nmap.org/book/man-legal.html, and further information on the    *
 * NPSL license itself can be found at https://nmap.org/npsl/ . This       *
 * header summarizes some key points from the Nmap license, but is no      *
 * substitute for the actual license text.                                 *
 *                                                                         *
 * Nmap is generally free for end users to download and use themselves,    *
 * including commercial use. It is available from https://nmap.org.        *
 *                                                                         *
 * The Nmap license generally prohibits companies from using and           *
 * redistributing Nmap in commercial products, but we sell a special Nmap  *
 * OEM Edition with a more permissive license and special features for     *
 * this purpose. See https://nmap.org/oem/                                 *
 *                                                                         *
 * If you have received a written Nmap license agreement or contract       *
 * stating terms other than these (such as an Nmap OEM license), you may   *
 * choose to use and redistribute Nmap under those terms instead.          *
 *                                                                         *
 * The official Nmap Windows builds include the Npcap software             *
 * (https://npcap.com) for packet capture and transmission. It is under    *
 * separate license terms which forbid redistribution without special      *
 * permission. So the official Nmap Windows builds may not be              *
 * redistributed without special permission (such as an Nmap OEM           *
 * license).                                                               *
 *                                                                         *
 * Source is provided to this software because we believe users have a     *
 * right to know exactly what a program is going to do before they run it. *
 * This also allows you to audit the software for security holes.          *
 *                                                                         *
 * Source code also allows you to port Nmap to new platforms, fix bugs,    *
 * and add new features.  You are highly encouraged to submit your         *
 * changes as a Github PR or by email to the dev@nmap.org mailing list     *
 * for possible incorporation into the main distribution. Unless you       *
 * specify otherwise, it is understood that you are offering us very       *
 * broad rights to use your submissions as described in the Nmap Public    *
 * Source License Contributor Agreement. This is important because we      *
 * fund the project by selling licenses with various terms, and also       *
 * because the inability to relicense code has caused devastating          *
 * problems for other Free Software projects (such as KDE and NASM).       *
 *                                                                         *
 * The free version of Nmap is distributed in the hope that it will be     *
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of  *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. Warranties,        *
 * indemnification and commercial support are all available through the    *
 * Npcap OEM program--see https://nmap.org/oem/                            *
 *                                                                         *
 ***************************************************************************/

/* $Id: ncat_ssl.c 38416 2022-08-29 17:09:47Z dmiller $ */

#include "nbase.h"
#include "ncat_config.h"
#ifdef HAVE_OPENSSL
#include "nsock.h"
#include "ncat.h"

#include <stdio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined LIBRESSL_VERSION_NUMBER
#define HAVE_OPAQUE_STRUCTS 1
#define FUNC_ASN1_STRING_data ASN1_STRING_get0_data
#else
#define FUNC_ASN1_STRING_data ASN1_STRING_data
#endif

#if OPENSSL_API_LEVEL >= 30000
#include <openssl/provider.h>
/* Deprecated in OpenSSL 3.0 */
#define SSL_get_peer_certificate SSL_get1_peer_certificate
#else
#include <openssl/bn.h>
#endif

/* Required for windows compilation to Eliminate APPLINK errors.
   See http://www.openssl.org/support/faq.html#PROG2 */
#ifdef WIN32
#include <openssl/applink.c>
#endif

static SSL_CTX *sslctx;

static int ssl_gen_cert(X509 **cert, EVP_PKEY **key);

/* Parameters for automatic key and certificate generation. */
enum {
    DEFAULT_KEY_BITS = 2048,
    DEFAULT_CERT_DURATION = 60 * 60 * 24 * 365,
};
#define CERTIFICATE_COMMENT "Automatically generated by Ncat. See https://nmap.org/ncat/."

SSL_CTX *setup_ssl_listen(void)
{
    const SSL_METHOD *method;

    if (sslctx)
        goto done;

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined LIBRESSL_VERSION_NUMBER
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();
    SSL_load_error_strings();
#elif OPENSSL_API_LEVEL >= 30000
  if (NULL == OSSL_PROVIDER_load(NULL, "legacy"))
  {
    loguser("OpenSSL legacy provider failed to load.\n");
  }
  if (NULL == OSSL_PROVIDER_load(NULL, "default"))
  {
    loguser("OpenSSL default provider failed to load.\n");
  }
#endif

    /* RAND_status initializes the random number generator through a variety of
       platform-dependent methods, then returns 1 if there is enough entropy or
       0 otherwise. This seems to be a good platform-independent way of seeding
       the generator, as well as of refusing to continue without enough
       entropy. */
    if (!RAND_status())
        bye("Failed to seed OpenSSL PRNG (RAND_status returned false).");

    if (!(method = SSLv23_server_method()))
        bye("SSLv23_server_method(): %s.", ERR_error_string(ERR_get_error(), NULL));
    if (!(sslctx = SSL_CTX_new(method)))
        bye("SSL_CTX_new(): %s.", ERR_error_string(ERR_get_error(), NULL));

    SSL_CTX_set_options(sslctx, SSL_OP_ALL | SSL_OP_NO_SSLv2);

    /* Secure ciphers list taken from Nsock. */
    if (o.sslciphers == NULL) {
      if (!SSL_CTX_set_cipher_list(sslctx, "ALL:!aNULL:!eNULL:!LOW:!EXP:!RC4:!MD5:@STRENGTH"))
        bye("Unable to set OpenSSL cipher list: %s", ERR_error_string(ERR_get_error(), NULL));
    }
    else {
      if (!SSL_CTX_set_cipher_list(sslctx, o.sslciphers))
        bye("Unable to set OpenSSL cipher list: %s", ERR_error_string(ERR_get_error(), NULL));
    }

    if (o.sslcert == NULL && o.sslkey == NULL) {
        X509 *cert;
        EVP_PKEY *key;
        char digest_buf[SHA1_STRING_LENGTH + 1];

        if (o.verbose)
            loguser("Generating a temporary %d-bit RSA key. Use --ssl-key and --ssl-cert to use a permanent one.\n", DEFAULT_KEY_BITS);
        if (ssl_gen_cert(&cert, &key) == 0)
            bye("ssl_gen_cert(): %s.", ERR_error_string(ERR_get_error(), NULL));
        if (o.verbose) {
            char *fp;
            fp = ssl_cert_fp_str_sha1(cert, digest_buf, sizeof(digest_buf));
            ncat_assert(fp == digest_buf);
            loguser("SHA-1 fingerprint: %s\n", digest_buf);
        }
        if (SSL_CTX_use_certificate(sslctx, cert) != 1)
            bye("SSL_CTX_use_certificate(): %s.", ERR_error_string(ERR_get_error(), NULL));
        if (SSL_CTX_use_PrivateKey(sslctx, key) != 1)
            bye("SSL_CTX_use_PrivateKey(): %s.", ERR_error_string(ERR_get_error(), NULL));
        X509_free(cert);
        EVP_PKEY_free(key);
    } else {
        if (o.sslcert == NULL || o.sslkey == NULL)
            bye("The --ssl-key and --ssl-cert options must be used together.");
        if (SSL_CTX_use_certificate_chain_file(sslctx, o.sslcert) != 1)
            bye("SSL_CTX_use_certificate_chain_file(): %s.", ERR_error_string(ERR_get_error(), NULL));
        if (SSL_CTX_use_PrivateKey_file(sslctx, o.sslkey, SSL_FILETYPE_PEM) != 1)
            bye("SSL_CTX_use_Privatekey_file(): %s.", ERR_error_string(ERR_get_error(), NULL));
    }

done:
    return sslctx;
}

SSL *new_ssl(int fd)
{
    SSL *ssl;

    if (!(ssl = SSL_new(sslctx)))
        bye("SSL_new(): %s.", ERR_error_string(ERR_get_error(), NULL));
    if (!SSL_set_fd(ssl, fd))
        bye("SSL_set_fd(): %s.", ERR_error_string(ERR_get_error(), NULL));

    return ssl;
}

/* Match a (user-supplied) hostname against a (certificate-supplied) name, which
   may be a wildcard pattern. A wildcard pattern may contain only one '*', it
   must be the entire leftmost component, and there must be at least two
   components following it. len is the length of pattern; pattern may contain
   null bytes so that len != strlen(pattern); pattern may also not be null terminated.
   hostname *must* be null-terminated. */
static int wildcard_match(const char *pattern, const char *hostname, int len)
{
    const char *p = pattern;
    const char *h = hostname;
    int remaining = len;
    if (len > 1 && pattern[0] == '*' && pattern[1] == '.') {
        /* A wildcard pattern. */
        const char *dot;

        /* Skip the wildcard component. */
        p += 2;
        remaining -= 2;

        /* Ensure there are no more wildcard characters. */
        if (memchr(p, '*', remaining) != NULL)
            return 0;

        /* Ensure there's at least one more dot, not counting a dot at the
           end. */
        dot = (const char *) memchr(p, '.', remaining);
        if (dot == NULL /* not found */
          || dot - p == remaining /* dot in last position */
          || *(dot + 1) == '\0') /* dot immediately before null terminator */
          {
            if (o.debug > 1) {
                logdebug("Wildcard name \"%.*s\" doesn't have at least two"
                    " components after the wildcard; rejecting.\n", len, pattern);
            }
            return 0;
        }

        /* Skip the leftmost hostname component. */
        h = strchr(hostname, '.');
        if (h == NULL)
            return 0;
        h++;

    }
    /* Compare what remains of the pattern and hostname. */
    /* Normal string comparison. Check the name length because I'm concerned
       about someone somehow embedding a '\0' in the subject and matching
       against a shorter name. */
    return remaining == strlen(h) && strncmp(p, h, remaining) == 0;
}

/* Match a hostname against the contents of a dNSName field of the
   subjectAltName extension, if present. This is the preferred place for a
   certificate to store its domain name, as opposed to in the commonName field.
   It has the advantage that multiple names can be stored, so that one
   certificate can match both "example.com" and "www.example.com".

   If num_checked is not NULL, the number of dNSName fields that were checked
   before returning will be stored in it. This is so you can distinguish between
   the check failing because there were names but none matched, or because there
   were no names to match. */
static int cert_match_dnsname(X509 *cert, const char *hostname,
    unsigned int *num_checked)
{
    X509_EXTENSION *ext;
    STACK_OF(GENERAL_NAME) *gen_names;
    const X509V3_EXT_METHOD *method;
    unsigned char *data;
    int i;

    if (num_checked != NULL)
        *num_checked = 0;

    i = X509_get_ext_by_NID(cert, NID_subject_alt_name, -1);
    if (i < 0)
        return 0;
    /* If there's more than one subjectAltName extension, forget it. */
    if (X509_get_ext_by_NID(cert, NID_subject_alt_name, i) >= 0)
        return 0;
    ext = X509_get_ext(cert, i);

    /* See the function X509V3_EXT_print in the OpenSSL source for this method
       of getting a string value from an extension. */
    method = X509V3_EXT_get(ext);
    if (method == NULL)
        return 0;

    /* We must copy this address into a temporary variable because ASN1_item_d2i
       increments it. We don't want it to corrupt ext->value->data. */
    ASN1_OCTET_STRING* asn1_str = X509_EXTENSION_get_data(ext);
    data = asn1_str->data;
    /* Here we rely on the fact that the internal representation (the "i" in
       "i2d") for NID_subject_alt_name is STACK_OF(GENERAL_NAME). Converting it
       to a stack of CONF_VALUE with a i2v method is not satisfactory, because a
       CONF_VALUE doesn't contain the length of the value so you can't know the
       presence of null bytes. */
#if (OPENSSL_VERSION_NUMBER > 0x00907000L)
    if (method->it != NULL) {
        ASN1_OCTET_STRING* asn1_str_a = X509_EXTENSION_get_data(ext);
        gen_names = (STACK_OF(GENERAL_NAME) *) ASN1_item_d2i(NULL,
            (const unsigned char **) &data,
            asn1_str_a->length, ASN1_ITEM_ptr(method->it));
    } else {
        ASN1_OCTET_STRING* asn1_str_b = X509_EXTENSION_get_data(ext);
        gen_names = (STACK_OF(GENERAL_NAME) *) method->d2i(NULL,
            (const unsigned char **) &data,
            asn1_str_b->length);
    }
#else
    gen_names = (STACK_OF(GENERAL_NAME) *) method->d2i(NULL,
        (const unsigned char **) &data,
        ext->value->length);
#endif
    if (gen_names == NULL)
        return 0;

    /* Look for a dNSName field with a matching hostname. There may be more than
       one dNSName field. */
    for (i = 0; i < sk_GENERAL_NAME_num(gen_names); i++) {
        GENERAL_NAME *gen_name;

        gen_name = sk_GENERAL_NAME_value(gen_names, i);
        if (gen_name->type == GEN_DNS) {
            const char *dnsname = (const char *) FUNC_ASN1_STRING_data(gen_name->d.dNSName);
            int dnslen = ASN1_STRING_length(gen_name->d.dNSName);
            if (o.debug > 1)
                logdebug("Checking certificate DNS name \"%.*s\" against \"%s\".\n", dnslen, dnsname, hostname);
            if (num_checked != NULL)
                (*num_checked)++;
            if (wildcard_match(dnsname, hostname, dnslen))
                return 1;
        }
    }

    return 0;
}

/* Returns the number of contiguous blocks of bytes in pattern that do not
   contain the '.' byte. */
static unsigned int num_components(const unsigned char *pattern, size_t len)
{
    const unsigned char *p;
    unsigned int count;

    count = 0;
    p = pattern;
    for (;;) {
        while (p - pattern < len && *p == '.')
            p++;
        if (p - pattern >= len)
            break;
        while (p - pattern < len && *p != '.')
            p++;
        count++;
    }

    return count;
}

/* Returns true if the a pattern is strictly less specific than the b
   pattern. */
static int less_specific(const unsigned char *a, size_t a_len,
    const unsigned char *b, size_t b_len)
{
    /* Wildcard patterns are always less specific than non-wildcard patterns. */
    if (memchr(a, '*', a_len) != NULL && memchr(b, '*', b_len) == NULL)
        return 1;
    if (memchr(a, '*', a_len) == NULL && memchr(b, '*', b_len) != NULL)
        return 0;

    return num_components(a, a_len) < num_components(b, b_len);
}

static int most_specific_commonname(X509_NAME *subject, const char **result)
{
    ASN1_STRING *best, *cur;
    int i;

    i = -1;
    best = NULL;
    while ((i = X509_NAME_get_index_by_NID(subject, NID_commonName, i)) != -1) {
        cur = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, i));
        /* We use "not less specific" instead of "more specific" to allow later
           entries to supersede earlier ones. */
        if (best == NULL
            || !less_specific(FUNC_ASN1_STRING_data(cur), ASN1_STRING_length(cur),
                              FUNC_ASN1_STRING_data(best), ASN1_STRING_length(best))) {
            best = cur;
        }
    }

    if (best == NULL) {
        *result = NULL;
        return -1;
    } else {
        *result = (char *) FUNC_ASN1_STRING_data(best);
        return ASN1_STRING_length(best);
    }
}

/* Match a hostname against the contents of the "most specific" commonName field
   of a certificate. The "most specific" term is used in RFC 2818 but is not
   defined anywhere that I (David Fifield) can find. This is what it means in
   Ncat: wildcard patterns are always less specific than non-wildcard patterns.
   If both patterns are wildcard or both are non-wildcard, the one with more
   name components is more specific. If two names have the same number of
   components, the one that comes later in the certificate is more specific. */
static int cert_match_commonname(X509 *cert, const char *hostname)
{
    X509_NAME *subject;
    const char *commonname;
    int n;

    subject = X509_get_subject_name(cert);
    if (subject == NULL)
        return 0;

    n = most_specific_commonname(subject, &commonname);
    if (n < 0 || commonname == NULL)
        /* No commonName found. */
        return 0;
    if (wildcard_match(commonname, hostname, n))
        return 1;

    if (o.verbose)
        loguser("Certificate verification error: Connected to \"%s\", but certificate is for \"%s\".\n", hostname, commonname);

    return 0;
}

/* Verify a host's name against the name in its certificate after connection.
   If the verify mode is SSL_VERIFY_NONE, always returns true. Returns nonzero
   on success. */
int ssl_post_connect_check(SSL *ssl, const char *hostname)
{
    X509 *cert = NULL;
    unsigned int num_checked;

    if (SSL_get_verify_mode(ssl) == SSL_VERIFY_NONE)
        return 1;

    if (hostname == NULL)
        return 0;

    cert = SSL_get_peer_certificate(ssl);
    if (cert == NULL)
        return 0;

    /* RFC 2818 (HTTP Over TLS): If a subjectAltName extension of type dNSName
       is present, that MUST be used as the identity. Otherwise, the (most
       specific) Common Name field in the Subject field of the certificate MUST
       be used. Although the use of the Common Name is existing practice, it is
       deprecated and Certification Authorities are encouraged to use the
       dNSName instead. */
    if (!cert_match_dnsname(cert, hostname, &num_checked)) {
        /* If there were dNSNames, we're done. If not, try the commonNames. */
        if (num_checked > 0 || !cert_match_commonname(cert, hostname)) {
            X509_free(cert);
            return 0;
        }
    }

    X509_free(cert);

    return SSL_get_verify_result(ssl) == X509_V_OK;
}

/* Generate a self-signed certificate and matching RSA keypair. References for
   this code are the book Network Programming with OpenSSL, chapter 10, section
   "Making Certificates"; and apps/req.c in the OpenSSL source. */
static int ssl_gen_cert(X509 **cert, EVP_PKEY **key)
{
    X509_NAME *subj;
    X509_EXTENSION *ext;
    X509V3_CTX ctx;
    const char *commonName = "localhost";
    char dNSName[128];
    int rc;
#if OPENSSL_API_LEVEL < 30000
    int ret = 0;
    RSA *rsa = NULL;
    BIGNUM *bne = NULL;

    *cert = NULL;
    *key = NULL;

    /* Generate a private key. */
    *key = EVP_PKEY_new();
    if (*key == NULL)
        goto err;
    do {
        /* Generate RSA key. */
        bne = BN_new();
        ret = BN_set_word(bne, RSA_F4);
        if (ret != 1)
            goto err;

        rsa = RSA_new();
        ret = RSA_generate_key_ex(rsa, DEFAULT_KEY_BITS, bne, NULL);
        if (ret != 1)
            goto err;

        rc = RSA_check_key(rsa);
    } while (rc == 0);
    if (rc == -1)
        bye("Error generating RSA key: %s", ERR_error_string(ERR_get_error(), NULL));
    if (EVP_PKEY_assign_RSA(*key, rsa) == 0) {
        RSA_free(rsa);
        goto err;
    }
#else
    *cert = NULL;
    *key = EVP_RSA_gen(DEFAULT_KEY_BITS);
    if (*key == NULL)
        goto err;
#endif

    /* Generate a certificate. */
    *cert = X509_new();
    if (*cert == NULL)
        goto err;
    if (X509_set_version(*cert, 2) == 0) /* Version 3. */
        goto err;
    ASN1_INTEGER_set(X509_get_serialNumber(*cert), get_random_u32() & 0x7FFFFFFF);

    /* Set the commonName. */
    subj = X509_get_subject_name(*cert);
    if (o.target != NULL)
        commonName = o.target;
    if (X509_NAME_add_entry_by_txt(subj, "commonName", MBSTRING_ASC,
        (unsigned char *) commonName, -1, -1, 0) == 0) {
        goto err;
    }

    /* Set the dNSName. */
    rc = Snprintf(dNSName, sizeof(dNSName), "DNS:%s", commonName);
    if (rc < 0 || rc >= sizeof(dNSName))
        goto err;
    X509V3_set_ctx(&ctx, *cert, *cert, NULL, NULL, 0);
    ext = X509V3_EXT_conf(NULL, &ctx, "subjectAltName", dNSName);
    if (ext == NULL)
        goto err;
    if (X509_add_ext(*cert, ext, -1) == 0)
        goto err;

    /* Set a comment. */
    ext = X509V3_EXT_conf(NULL, &ctx, "nsComment", CERTIFICATE_COMMENT);
    if (ext == NULL)
        goto err;
    if (X509_add_ext(*cert, ext, -1) == 0)
        goto err;

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined LIBRESSL_VERSION_NUMBER
    {
        ASN1_TIME *tb, *ta;
        tb = NULL;
        ta = NULL;

        if (X509_set_issuer_name(*cert, X509_get_subject_name(*cert)) == 0
            || (tb = ASN1_STRING_dup(X509_get0_notBefore(*cert))) == 0
            || X509_gmtime_adj(tb, 0) == 0
            || X509_set1_notBefore(*cert, tb) == 0
            || (ta = ASN1_STRING_dup(X509_get0_notAfter(*cert))) == 0
            || X509_gmtime_adj(ta, DEFAULT_CERT_DURATION) == 0
            || X509_set1_notAfter(*cert, ta) == 0
            || X509_set_pubkey(*cert, *key) == 0) {
            ASN1_STRING_free(tb);
            ASN1_STRING_free(ta);
            goto err;
        }
        ASN1_STRING_free(tb);
        ASN1_STRING_free(ta);
    }
#else
    if (X509_set_issuer_name(*cert, X509_get_subject_name(*cert)) == 0
        || X509_gmtime_adj(X509_get_notBefore(*cert), 0) == 0
        || X509_gmtime_adj(X509_get_notAfter(*cert), DEFAULT_CERT_DURATION) == 0
        || X509_set_pubkey(*cert, *key) == 0) {
        goto err;
    }
#endif

    /* Sign it. */
    if (X509_sign(*cert, *key, EVP_sha1()) == 0)
        goto err;

    return 1;

err:
    if (*cert != NULL)
        X509_free(*cert);
    if (*key != NULL)
        EVP_PKEY_free(*key);

    return 0;
}

/* Calculate a SHA-1 fingerprint of a certificate and format it as a
   human-readable string. Returns strbuf or NULL on error. */
char *ssl_cert_fp_str_sha1(const X509 *cert, char *strbuf, size_t len)
{
    unsigned char binbuf[SHA1_BYTES];
    unsigned int n;
    char *p;
    unsigned int i;

    if (len < SHA1_STRING_LENGTH + 1)
        return NULL;
    n = sizeof(binbuf);
    if (X509_digest(cert, EVP_sha1(), binbuf, &n) != 1)
        return NULL;

    p = strbuf;
    for (i = 0; i < n; i++) {
        if (i > 0 && i % 2 == 0)
            *p++ = ' ';
        Snprintf(p, 3, "%02X", binbuf[i]);
        p += 2;
    }
    ncat_assert(p - strbuf <= len);
    *p = '\0';

    return strbuf;
}

/* Tries to complete an ssl handshake on the socket received by fdinfo struct
   if ssl is enabled on that socket. */

int ssl_handshake(struct fdinfo *sinfo)
{
    int ret = 0;
    int sslerr = 0;

    if (sinfo == NULL) {
        if (o.debug)
           logdebug("ncat_ssl.c: Invoking ssl_handshake() with a NULL parameter "
                    "is a serious bug. Please fix it.\n");
        return -1;
    }

    if (!o.ssl)
        return -1;

    /* Initialize the socket too if it isn't.  */
    if (!sinfo->ssl)
        sinfo->ssl = new_ssl(sinfo->fd);

    ret = SSL_accept(sinfo->ssl);

    if (ret == 1)
        return NCAT_SSL_HANDSHAKE_COMPLETED;

    sslerr = SSL_get_error(sinfo->ssl, ret);

    if (ret == -1) {
        if (sslerr == SSL_ERROR_WANT_READ)
            return NCAT_SSL_HANDSHAKE_PENDING_READ;
        if (sslerr == SSL_ERROR_WANT_WRITE)
            return NCAT_SSL_HANDSHAKE_PENDING_WRITE;
    }

    if (o.verbose) {
        loguser("Failed SSL connection from %s: %s\n",
        inet_socktop(&sinfo->remoteaddr),
                     ERR_error_string(ERR_get_error(), NULL));
    }
    return NCAT_SSL_HANDSHAKE_FAILED;
}

#endif
