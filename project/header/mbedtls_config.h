#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#include <limits.h>

/* Entropy */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* Cipher modes */
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CFB
#define MBEDTLS_CIPHER_MODE_CTR

/* Remove weak ciphers */
#define MBEDTLS_REMOVE_ARC4_CIPHERSUITES
#define MBEDTLS_REMOVE_3DES_CIPHERSUITES

/* Elliptic curves */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

/* Key exchange */
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* PKCS */
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

/* TLS extensions */
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET

/* Protocols */
#define MBEDTLS_SSL_PROTO_TLS1_2

/* X.509 */
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE

/* Modules - Ciphers */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C

/* Modules - Parsers */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_PARSE_C

/* Modules - Hashing */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C

/* Modules - Elliptic curves */
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C

/* Modules - RSA */
#define MBEDTLS_RSA_C

/* Modules - Public Key */
#define MBEDTLS_PK_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_PKCS12_C

/* Modules - SSL/TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C

/* Modules - X.509 */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* Requirements */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_WRITE_C

/* Misc */
#define MBEDTLS_ERROR_C
#define MBEDTLS_PLATFORM_C

#endif /* MBEDTLS_CONFIG_H */