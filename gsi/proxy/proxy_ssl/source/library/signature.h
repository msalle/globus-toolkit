
#ifndef HEADER_SIGNATURE_H
#define HEADER_SIGNATURE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup signature Signature
 *
 * @author Sam Meder
 * @author Sam Lang
 * 
 * The signature set of functions
 * provides an interface
 * to a signature used by proxy certificates
 *
 * @see Internet Draft Document: draft-ietf-pkix-proxy-01.txt
 * for further information
 */

#include <openssl/x509.h>

ASN1_METHOD * X509_SIG_asn1_meth();

X509_SIG * X509_SIG_dup(
    X509_SIG *                          signature);

int X509_SIG_cmp(
    const X509_SIG *                    a,
    const X509_SIG *                    b);

int X509_SIG_print(
    X509_SIG *                          signature);

int X509_SIG_print_fp(
    FILE *                              fp,
    X509_SIG *                          signature);

EVP_MD * X509_SIG_get_algorithm(
    X509_SIG *                          signature);

int X509_SIG_set_signature(
    X509_SIG *                          signature,
    X509 *                              cert);

unsigned char * X509_SIG_get_signature(
    X509_SIG *                          signature,
    long *                              length);

int i2d_X509_SIG(
    X509_SIG *                          signature,
    unsigned char **                    buffer);

X509_SIG * d2i_X509_SIG(
    X509_SIG **                         signature,
    unsigned char **                    buffer,
    long                                length);


#ifdef __cplusplus
}
#endif

#endif // HEADER_SIGNATURE_H
