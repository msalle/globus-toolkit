
#ifndef HEADER_PROXYCERTINFO_H
#define HEADER_PROXYCERTINFO_H

#include "signature.h"
#include "proxyrestriction.h"
#include "proxygroup.h"

#include <openssl/asn1.h>
#include <openssl/x509.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup proxycertinfo ProxyCertInfo
 * 
 * @author Sam Meder
 * @author Sam Lang
 * 
 * The proxycertinfo.h file defines a method of
 * maintaining information about proxy certificates
 * as defined in Internet Draft Document: 
 * draft-ietf-pkix-proxy-01.txt
 */

/**
 * Used for error checking
 */
#define ASN1_F_PROXYCERTINFO_NEW                         430
#define ASN1_F_D2I_PROXYCERTINFO                         431

/* data structures */

/**
 * @ingroup proxycertinfo
 * @typedef PROXYCERTINFO
 *
 * This typedef maintains information about a proxy
 * certificate.
 *
 * @note NOTE: The API provides functions to manipulate
 * the fields of a PROXYCERTINFO.  Accessing the fields
 * directly will not work.
 *
 * 
 * @param pC a boolean value, it indicates whether or not the 
 * certificate is a proxy certificate: if the certificate is a proxy 
 * certificate, the pC field MUST be TRUE; otherwise, the pC field MUST 
 * be FALSE.
 * @param path_length an optional field in the ANS.1 DER encoding, 
 * it specifies the maximum depth of the path of Proxy Certificates 
 * that can be signed by this End Entity Certificate or Proxy Certificate.
 * @param restriction an optional field in the ANS.1 DER encoding,
 * specifies restrictions on the use of this certificate.  If the 
 * certificate is not a Proxy Certificate (i.e, if the pC field is 
 * FALSE), then the restriction field will not be present.
 * @param group defines which group the proxy is to be in
 * @param issuer_signature holds the signature of the certificate
 * that signed this proxy certificate (issued it)
 */
#ifndef _PROXY_SSL_INTERNAL_
typedef void PROXYCERTINFO;
#endif

/* functions */
    
ASN1_METHOD * PROXYCERTINFO_asn1_meth();

PROXYCERTINFO * PROXYCERTINFO_new();

void PROXYCERTINFO_free(
    PROXYCERTINFO *                     cert_info);

PROXYCERTINFO * PROXYCERTINFO_dup(
    PROXYCERTINFO *                     cert_info);

int PROXYCERTINFO_cmp(
    const PROXYCERTINFO *               a,
    const PROXYCERTINFO *               b);

int PROXYCERTINFO_print(
    PROXYCERTINFO *                     cert_info);

int PROXYCERTINFO_print_fp(
    FILE *                              fp,
    PROXYCERTINFO *                     cert_info);

int PROXYCERTINFO_set_group(
    PROXYCERTINFO *                     cert_info,
    PROXYGROUP *                        group);

PROXYGROUP * PROXYCERTINFO_get_group(
    PROXYCERTINFO *                     cert_info);

int PROXYCERTINFO_set_restriction(
    PROXYCERTINFO *                     cert_info,
    PROXYRESTRICTION *                  restriction);

PROXYRESTRICTION * PROXYCERTINFO_get_restriction(
    PROXYCERTINFO *                     cert_info);

int PROXYCERTINFO_set_path_length(
    PROXYCERTINFO *                     cert_info,
    ASN1_INTEGER                        path_length);

ASN1_INTEGER * PROXYCERTINFO_get_path_length(
    PROXYCERTINFO *                     cert_info);

int PROXYCERTINFO_set_issuer_cert_digest(
    PROXYCERTINFO *                     cert_info,
    X509_SIG *                          cert_digest);

X509_SIG * PROXYCERTINFO_get_issuer_cert_digest(
    PROXYCERTINFO *                     cert_info);

int i2d_PROXYCERTINFO(
    PROXYCERTINFO *                     cert_info,
    unsigned char **                    buffer);

PROXYCERTINFO * d2i_PROXYCERTINFO(
    PROXYCERTINFO **                    cert_info,
    unsigned char **                    buffer,
    long                                length);

#ifdef __cplusplus
}
#endif

#endif // HEADER_PROXYCERTINFO_H
