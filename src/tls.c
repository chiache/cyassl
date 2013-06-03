/* tls.c
 *
 * Copyright (C) 2006-2013 wolfSSL Inc.
 *
 * This file is part of CyaSSL.
 *
 * CyaSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * CyaSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <cyassl/ctaocrypt/settings.h>

#include <cyassl/ssl.h>
#include <cyassl/internal.h>
#include <cyassl/error.h>
#include <cyassl/ctaocrypt/hmac.h>



#ifndef NO_TLS


#ifndef min

    static INLINE word32 min(word32 a, word32 b)
    {
        return a > b ? b : a;
    }

#endif /* min */


#ifdef CYASSL_SHA384
    #define PHASH_MAX_DIGEST_SIZE SHA384_DIGEST_SIZE
#else
    #define PHASH_MAX_DIGEST_SIZE SHA256_DIGEST_SIZE
#endif

/* compute p_hash for MD5, SHA-1, SHA-256, or SHA-384 for TLSv1 PRF */
static void p_hash(byte* result, word32 resLen, const byte* secret,
                   word32 secLen, const byte* seed, word32 seedLen, int hash)
{
    word32   len = PHASH_MAX_DIGEST_SIZE;
    word32   times;
    word32   lastLen;
    word32   lastTime;
    word32   i;
    word32   idx = 0;
    byte     previous[PHASH_MAX_DIGEST_SIZE];  /* max size */
    byte     current[PHASH_MAX_DIGEST_SIZE];   /* max size */

    Hmac hmac;

    switch (hash) {
        #ifndef NO_MD5
        case md5_mac:
        {
            len = MD5_DIGEST_SIZE;
            hash = MD5;
        }
        break;
        #endif
        #ifndef NO_SHA256
        case sha256_mac:
        {
            len = SHA256_DIGEST_SIZE;
            hash = SHA256;
        }
        break;
        #endif
        #ifdef CYASSL_SHA384
        case sha384_mac:
        {
            len = SHA384_DIGEST_SIZE;
            hash = SHA384;
        }
        break;
        #endif
#ifndef NO_SHA
        case sha_mac:
        default:
        {
            len = SHA_DIGEST_SIZE;
            hash = SHA;
        }
        break;
#endif
    }

    times = resLen / len;
    lastLen = resLen % len;
    if (lastLen) times += 1;
    lastTime = times - 1;

    HmacSetKey(&hmac, hash, secret, secLen);
    HmacUpdate(&hmac, seed, seedLen);       /* A0 = seed */
    HmacFinal(&hmac, previous);             /* A1 */

    for (i = 0; i < times; i++) {
        HmacUpdate(&hmac, previous, len);
        HmacUpdate(&hmac, seed, seedLen);
        HmacFinal(&hmac, current);

        if ( (i == lastTime) && lastLen)
            XMEMCPY(&result[idx], current, min(lastLen, sizeof(current)));
        else {
            XMEMCPY(&result[idx], current, len);
            idx += len;
            HmacUpdate(&hmac, previous, len);
            HmacFinal(&hmac, previous);
        }
    }
    XMEMSET(previous, 0, sizeof previous);
    XMEMSET(current, 0, sizeof current);
    XMEMSET(&hmac, 0, sizeof hmac);
}



#ifndef NO_OLD_TLS

/* calculate XOR for TLSv1 PRF */
static INLINE void get_xor(byte *digest, word32 digLen, byte* md5, byte* sha)
{
    word32 i;

    for (i = 0; i < digLen; i++) 
        digest[i] = md5[i] ^ sha[i];
}


/* compute TLSv1 PRF (pseudo random function using HMAC) */
static void doPRF(byte* digest, word32 digLen, const byte* secret,word32 secLen,
            const byte* label, word32 labLen, const byte* seed, word32 seedLen)
{
    word32 half = (secLen + 1) / 2;

    byte md5_half[MAX_PRF_HALF];        /* half is real size */
    byte sha_half[MAX_PRF_HALF];        /* half is real size */
    byte labelSeed[MAX_PRF_LABSEED];    /* labLen + seedLen is real size */
    byte md5_result[MAX_PRF_DIG];       /* digLen is real size */
    byte sha_result[MAX_PRF_DIG];       /* digLen is real size */

    if (half > MAX_PRF_HALF)
        return;
    if (labLen + seedLen > MAX_PRF_LABSEED)
        return;
    if (digLen > MAX_PRF_DIG)
        return;

    XMEMSET(md5_result, 0, digLen);
    XMEMSET(sha_result, 0, digLen);
    
    XMEMCPY(md5_half, secret, half);
    XMEMCPY(sha_half, secret + half - secLen % 2, half);

    XMEMCPY(labelSeed, label, labLen);
    XMEMCPY(labelSeed + labLen, seed, seedLen);

    p_hash(md5_result, digLen, md5_half, half, labelSeed, labLen + seedLen,
           md5_mac);
    p_hash(sha_result, digLen, sha_half, half, labelSeed, labLen + seedLen,
           sha_mac);
    get_xor(digest, digLen, md5_result, sha_result);
}

#endif


/* Wrapper to call straight thru to p_hash in TSL 1.2 cases to remove stack
   use */
static void PRF(byte* digest, word32 digLen, const byte* secret, word32 secLen,
            const byte* label, word32 labLen, const byte* seed, word32 seedLen,
            int useAtLeastSha256, int hash_type)
{
    if (useAtLeastSha256) {
        byte labelSeed[MAX_PRF_LABSEED];    /* labLen + seedLen is real size */

        if (labLen + seedLen > MAX_PRF_LABSEED)
            return;

        XMEMCPY(labelSeed, label, labLen);
        XMEMCPY(labelSeed + labLen, seed, seedLen);

        /* If a cipher suite wants an algorithm better than sha256, it
         * should use better. */
        if (hash_type < sha256_mac)
            hash_type = sha256_mac;
        p_hash(digest, digLen, secret, secLen, labelSeed, labLen + seedLen,
               hash_type);
    }
#ifndef NO_OLD_TLS
    else
        doPRF(digest, digLen, secret, secLen, label, labLen, seed, seedLen);
#endif
}


#ifdef CYASSL_SHA384
    #define HSHASH_SZ SHA384_DIGEST_SIZE
#else
    #define HSHASH_SZ FINISHED_SZ
#endif


void BuildTlsFinished(CYASSL* ssl, Hashes* hashes, const byte* sender)
{
    const byte* side;
    byte        handshake_hash[HSHASH_SZ];
    word32      hashSz = FINISHED_SZ;

#ifndef NO_OLD_TLS
    Md5Final(&ssl->hashMd5, handshake_hash);
    ShaFinal(&ssl->hashSha, &handshake_hash[MD5_DIGEST_SIZE]);
#endif
    
    if (IsAtLeastTLSv1_2(ssl)) {
#ifndef NO_SHA256
        if (ssl->specs.mac_algorithm <= sha256_mac) {
            Sha256Final(&ssl->hashSha256, handshake_hash);
            hashSz = SHA256_DIGEST_SIZE;
        }
#endif
#ifdef CYASSL_SHA384
        if (ssl->specs.mac_algorithm == sha384_mac) {
            Sha384Final(&ssl->hashSha384, handshake_hash);
            hashSz = SHA384_DIGEST_SIZE;
        }
#endif
    }
   
    if ( XSTRNCMP((const char*)sender, (const char*)client, SIZEOF_SENDER) == 0)
        side = tls_client;
    else
        side = tls_server;

    PRF((byte*)hashes, TLS_FINISHED_SZ, ssl->arrays->masterSecret, SECRET_LEN,
        side, FINISHED_LABEL_SZ, handshake_hash, hashSz, IsAtLeastTLSv1_2(ssl),
        ssl->specs.mac_algorithm);
}


#ifndef NO_OLD_TLS

ProtocolVersion MakeTLSv1(void)
{
    ProtocolVersion pv;
    pv.major = SSLv3_MAJOR;
    pv.minor = TLSv1_MINOR;

    return pv;
}


ProtocolVersion MakeTLSv1_1(void)
{
    ProtocolVersion pv;
    pv.major = SSLv3_MAJOR;
    pv.minor = TLSv1_1_MINOR;

    return pv;
}

#endif


ProtocolVersion MakeTLSv1_2(void)
{
    ProtocolVersion pv;
    pv.major = SSLv3_MAJOR;
    pv.minor = TLSv1_2_MINOR;

    return pv;
}


static const byte master_label[MASTER_LABEL_SZ + 1] = "master secret";
static const byte key_label   [KEY_LABEL_SZ + 1]    = "key expansion";


int DeriveTlsKeys(CYASSL* ssl)
{
    int length = 2 * ssl->specs.hash_size + 
                 2 * ssl->specs.key_size  +
                 2 * ssl->specs.iv_size;
    byte         seed[SEED_LEN];
    byte         key_data[MAX_PRF_DIG];

    XMEMCPY(seed, ssl->arrays->serverRandom, RAN_LEN);
    XMEMCPY(&seed[RAN_LEN], ssl->arrays->clientRandom, RAN_LEN);

    PRF(key_data, length, ssl->arrays->masterSecret, SECRET_LEN, key_label,
        KEY_LABEL_SZ, seed, SEED_LEN, IsAtLeastTLSv1_2(ssl),
        ssl->specs.mac_algorithm);

    return StoreKeys(ssl, key_data);
}


int MakeTlsMasterSecret(CYASSL* ssl)
{
    byte seed[SEED_LEN];
    
    XMEMCPY(seed, ssl->arrays->clientRandom, RAN_LEN);
    XMEMCPY(&seed[RAN_LEN], ssl->arrays->serverRandom, RAN_LEN);

    PRF(ssl->arrays->masterSecret, SECRET_LEN,
        ssl->arrays->preMasterSecret, ssl->arrays->preMasterSz,
        master_label, MASTER_LABEL_SZ, 
        seed, SEED_LEN, IsAtLeastTLSv1_2(ssl), ssl->specs.mac_algorithm);

#ifdef SHOW_SECRETS
    {
        int i;
        printf("master secret: ");
        for (i = 0; i < SECRET_LEN; i++)
            printf("%02x", ssl->arrays->masterSecret[i]);
        printf("\n");
    }
#endif

    return DeriveTlsKeys(ssl);
}


/* Used by EAP-TLS and EAP-TTLS to derive keying material from
 * the master_secret. */
int CyaSSL_make_eap_keys(CYASSL* ssl, void* msk, unsigned int len,
                                                              const char* label)
{
    byte seed[SEED_LEN];

    /*
     * As per RFC-5281, the order of the client and server randoms is reversed
     * from that used by the TLS protocol to derive keys.
     */
    XMEMCPY(seed, ssl->arrays->clientRandom, RAN_LEN);
    XMEMCPY(&seed[RAN_LEN], ssl->arrays->serverRandom, RAN_LEN);

    PRF((byte*)msk, len,
        ssl->arrays->masterSecret, SECRET_LEN,
        (const byte *)label, (word32)strlen(label),
        seed, SEED_LEN, IsAtLeastTLSv1_2(ssl), ssl->specs.mac_algorithm);

    return 0;
}


/*** next for static INLINE s copied from cyassl_int.c ***/

/* convert 16 bit integer to opaque */
static INLINE void c16toa(word16 u16, byte* c)
{
    c[0] = (u16 >> 8) & 0xff;
    c[1] =  u16 & 0xff;
}

/* convert opaque to 16 bit integer */
static INLINE void ato16(const byte* c, word16* u16)
{
    *u16 = (c[0] << 8) | (c[1]);
}


/* convert 32 bit integer to opaque */
static INLINE void c32toa(word32 u32, byte* c)
{
    c[0] = (u32 >> 24) & 0xff;
    c[1] = (u32 >> 16) & 0xff;
    c[2] = (u32 >>  8) & 0xff;
    c[3] =  u32 & 0xff;
}


static INLINE word32 GetSEQIncrement(CYASSL* ssl, int verify)
{
#ifdef CYASSL_DTLS
    if (ssl->options.dtls) {
        if (verify)
            return ssl->keys.dtls_peer_sequence_number; /* explicit from peer */
        else
            return ssl->keys.dtls_sequence_number - 1; /* already incremented */
    }
#endif
    if (verify)
        return ssl->keys.peer_sequence_number++; 
    else
        return ssl->keys.sequence_number++; 
}


#ifdef CYASSL_DTLS

static INLINE word32 GetEpoch(CYASSL* ssl, int verify)
{
    if (verify)
        return ssl->keys.dtls_peer_epoch; 
    else
        return ssl->keys.dtls_epoch; 
}

#endif /* CYASSL_DTLS */


static INLINE const byte* GetMacSecret(CYASSL* ssl, int verify)
{
    if ( (ssl->options.side == CLIENT_END && !verify) ||
         (ssl->options.side == SERVER_END &&  verify) )
        return ssl->keys.client_write_MAC_secret;
    else
        return ssl->keys.server_write_MAC_secret;
}

/*** end copy ***/


/* TLS type HMAC */
void TLS_hmac(CYASSL* ssl, byte* digest, const byte* in, word32 sz,
              int content, int verify)
{
    Hmac hmac;
    byte seq[SEQ_SZ];
    byte length[LENGTH_SZ];
    byte inner[ENUM_LEN + VERSION_SZ + LENGTH_SZ]; /* type + version +len */
    int  type;

    XMEMSET(seq, 0, SEQ_SZ);
    c16toa((word16)sz, length);
#ifdef CYASSL_DTLS
    if (ssl->options.dtls)
        c16toa((word16)GetEpoch(ssl, verify), seq);
#endif
    c32toa(GetSEQIncrement(ssl, verify), &seq[sizeof(word32)]);
    
    switch (ssl->specs.mac_algorithm) {
        #ifndef NO_MD5
        case md5_mac:
        {
            type = MD5;
        }
        break;
        #endif
        #ifndef NO_SHA256
        case sha256_mac:
        {
            type = SHA256;
        }
        break;
        #endif
        #ifdef CYASSL_SHA384
        case sha384_mac:
        {
            type = SHA384;
        }
        break;
        #endif
#ifndef NO_SHA
        case sha_mac:
        default:
        {
            type = SHA;
        }
        break;
#endif
    }
    HmacSetKey(&hmac, type, GetMacSecret(ssl, verify), ssl->specs.hash_size);
    
    HmacUpdate(&hmac, seq, SEQ_SZ);                               /* seq_num */
    inner[0] = (byte)content;                                     /* type */
    inner[ENUM_LEN] = ssl->version.major;
    inner[ENUM_LEN + ENUM_LEN] = ssl->version.minor;              /* version */
    XMEMCPY(&inner[ENUM_LEN + VERSION_SZ], length, LENGTH_SZ);     /* length */
    HmacUpdate(&hmac, inner, sizeof(inner));
    HmacUpdate(&hmac, in, sz);                                /* content */
    HmacFinal(&hmac, digest);
}

#ifdef HAVE_TLS_EXTENSIONS

static int TLSX_Append(TLSX** list, TLSX_Type type)
{
    TLSX* extension;

    if (list == NULL) /* won't check type since this function is static */
        return BAD_FUNC_ARG;

    if ((extension = XMALLOC(sizeof(TLSX), 0, DYNAMIC_TYPE_TLSX)) == NULL)
        return MEMORY_E;

    extension->type = type;
    extension->data = NULL;
    extension->resp = 0;
    extension->next = *list;
    *list = extension;

    return 0;
}

#ifndef NO_CYASSL_SERVER

static void TLSX_SetResponse(CYASSL* ssl, TLSX_Type type)
{
    TLSX *ext = TLSX_Find(ssl->extensions, type);

    if (ext)
        ext->resp = 1;
}

#endif

/* SNI - Server Name Indication */

#ifdef HAVE_SNI

static void TLSX_SNI_Free(SNI* sni)
{
    if (sni) {
        switch (sni->type) {
            case CYASSL_SNI_HOST_NAME:
                XFREE(sni->data.host_name, 0, DYNAMIC_TYPE_TLSX);
            break;
        }

        XFREE(sni, 0, DYNAMIC_TYPE_TLSX);
    }
}

static void TLSX_SNI_FreeAll(SNI* list)
{
    SNI* sni;

    while ((sni = list)) {
        list = sni->next;
        TLSX_SNI_Free(sni);
    }
}

static int TLSX_SNI_Append(SNI** list, byte type, const void* data, word16 size)
{
    SNI* sni;

    if (list == NULL)
        return BAD_FUNC_ARG;

    if ((sni = XMALLOC(sizeof(SNI), 0, DYNAMIC_TYPE_TLSX)) == NULL)
        return MEMORY_E;

    switch (type) {
        case CYASSL_SNI_HOST_NAME: {
            sni->data.host_name = XMALLOC(size + 1, 0, DYNAMIC_TYPE_TLSX);

            if (sni->data.host_name) {
                XSTRNCPY(sni->data.host_name, (const char*) data, size);
                sni->data.host_name[size] = 0;
            } else {
                XFREE(sni, 0, DYNAMIC_TYPE_TLSX);
                return MEMORY_E;
            }
        }
        break;

        default: /* invalid type */
            XFREE(sni, 0, DYNAMIC_TYPE_TLSX);
            return BAD_FUNC_ARG;
        break;
    }

    sni->type = type;
    sni->next = *list;
    sni->options = 0;
    *list = sni;

    return 0;
}

static word16 TLSX_SNI_GetSize(SNI* list)
{
    SNI* sni;
    word16 length = OPAQUE16_LEN; /* list length */

    while ((sni = list)) {
        list = sni->next;

        length += ENUM_LEN + OPAQUE16_LEN; /* sni type + sni length */

        switch (sni->type) {
            case CYASSL_SNI_HOST_NAME:
                length += XSTRLEN((char*) sni->data.host_name);
            break;
        }
    }

    return length;
}

static word16 TLSX_SNI_Write(SNI* list, byte* output)
{
    SNI* sni;
    word16 length = 0;
    word16 offset = OPAQUE16_LEN; /* list length offset */

    while ((sni = list)) {
        list = sni->next;

        output[offset++] = sni->type; /* sni type */

        switch (sni->type) {
            case CYASSL_SNI_HOST_NAME:
                length = XSTRLEN((char*) sni->data.host_name);

                c16toa(length, output + offset); /* sni length */
                offset += OPAQUE16_LEN;

                XMEMCPY(output + offset, sni->data.host_name, length);

                offset += length;
            break;
        }
    }

    c16toa(offset - OPAQUE16_LEN, output); /* writing list length */

    return offset;
}

static SNI* TLSX_SNI_Find(SNI *list, byte type)
{
    SNI *sni = list;

    while (sni && sni->type != type)
        sni = sni->next;

    return sni;
}

static int TLSX_SNI_Parse(CYASSL* ssl, byte* input, word16 length,
                                                                 byte isRequest)
{
#ifndef NO_CYASSL_SERVER
    word16 size = 0;
    word16 offset = 0;
#endif

    TLSX *extension = TLSX_Find(ssl->extensions, SERVER_NAME_INDICATION);

    if (!extension)
        extension = TLSX_Find(ssl->ctx->extensions, SERVER_NAME_INDICATION);

    if (!extension || !extension->data) {
        if (!isRequest) {
            CYASSL_MSG("Unexpected SNI response from server");
        }

        return 0; /* not using SNI */
    }

    if (!isRequest) {
        if (length) {
            CYASSL_MSG("SNI response should be empty!");
        }

        return 0; /* nothing to do */
    }

#ifndef NO_CYASSL_SERVER

    if (OPAQUE16_LEN > length)
        return INCOMPLETE_DATA;

    ato16(input, &size);
    offset += OPAQUE16_LEN;

    /* validating sni list length */
    if (length != OPAQUE16_LEN + size)
        return INCOMPLETE_DATA;

    for (size = 0; offset < length; offset += size) {
        SNI *sni;
        byte type = input[offset++];

        if (offset + OPAQUE16_LEN > length)
            return INCOMPLETE_DATA;

        ato16(input + offset, &size);
        offset += OPAQUE16_LEN;

        if (offset + size > length)
            return INCOMPLETE_DATA;

        if (!(sni = TLSX_SNI_Find((SNI *) extension->data, type))) {
            continue; /* not using this SNI type */
        }

        switch(type) {
            case CYASSL_SNI_HOST_NAME:
                if (XSTRLEN(sni->data.host_name) != size
                    || XSTRNCMP(sni->data.host_name,
                                         (const char *) input + offset, size)) {
                    if (sni->options & CYASSL_SNI_CONTINUE_ON_MISMATCH)
                        break;
                        /**
                         * Better client thinks the server is not using SNI,
                         * instead of thinking that the host_name matched.
                         * No empty SNI response in this case.
                         */

                    SendAlert(ssl, alert_fatal, unrecognized_name);

                    return UNKNOWN_SNI_HOST_NAME_E;
                } else {
                    int r = TLSX_UseSNI(&ssl->extensions, type, (byte *) "", 0);

                    if (r) return r; /* throw error */
                }
                break;
        }

        TLSX_SetResponse(ssl, SERVER_NAME_INDICATION);
    }

#endif

    return 0;
}

int TLSX_UseSNI(TLSX** extensions, byte type, const void* data, word16 size)
{
    TLSX* extension = NULL;
    SNI*  sni       = NULL;
    int   ret       = 0;

    if (extensions == NULL || data == NULL)
        return BAD_FUNC_ARG;

    if ((ret = TLSX_SNI_Append(&sni, type, data, size)) != 0)
        return ret;

    extension = *extensions;

    /* find SNI extension if it already exists. */
    while (extension && extension->type != SERVER_NAME_INDICATION)
        extension = extension->next;

    /* push new SNI extension if it doesn't exists. */
    if (!extension) {
        if ((ret = TLSX_Append(extensions, SERVER_NAME_INDICATION)) != 0) {
            TLSX_SNI_Free(sni);
            return ret;
        }

        extension = *extensions;
    }

    /* push new SNI object to extension data. */
    sni->next = (SNI*) extension->data;
    extension->data = (void*) sni;

    /* look for another server name of the same type to remove (replacement) */
    while ((sni = sni->next)) {
        if (sni->next && sni->next->type == type) {
            SNI *next = sni->next;

            sni->next = next->next;
            TLSX_SNI_Free(next);

            break;
        }
    }

    return 0;
}

#ifndef NO_CYASSL_SERVER
void TLSX_SNI_SetOptions(TLSX* extensions, byte type, byte options)
{
  TLSX* extension = TLSX_Find(extensions, SERVER_NAME_INDICATION);
  SNI* sni = TLSX_SNI_Find(extension ? extension->data : NULL, type);

  if (sni)
      sni->options = options;
}
#endif

#define SNI_FREE_ALL TLSX_SNI_FreeAll
#define SNI_GET_SIZE TLSX_SNI_GetSize
#define SNI_WRITE    TLSX_SNI_Write
#define SNI_PARSE    TLSX_SNI_Parse

#else

#define SNI_FREE_ALL(x)
#define SNI_GET_SIZE(x) 0
#define SNI_WRITE(x) 0
#define SNI_PARSE(x) 0

#endif /* HAVE_SNI */

TLSX* TLSX_Find(TLSX* list, TLSX_Type type)
{
    TLSX* extension = list;

    while (extension && extension->type != type)
        extension = extension->next;

    return extension;
}

void TLSX_FreeAll(TLSX* list)
{
    TLSX* extension;

    while ((extension = list)) {
        list = extension->next;

        switch (extension->type) {
            case SERVER_NAME_INDICATION:
                SNI_FREE_ALL((SNI *) extension->data);
                break;
        }

        XFREE(extension, 0, DYNAMIC_TYPE_TLSX);
    }
}

#define IS_OFF(semaphore, light) \
    ((semaphore)[(light) / 8] ^  (byte) (0x01 << ((light) % 8)))

#define TURN_ON(semaphore, light) \
    ((semaphore)[(light) / 8] |= (byte) (0x01 << ((light) % 8)))

static word16 TLSX_GetSize(TLSX* list, byte* semaphore, byte isRequest)
{
    TLSX* extension;
    word16 length = 0;

    while ((extension = list)) {
        list = extension->next;

        if (!isRequest && !extension->resp)
            continue; /* skip! */

        if (IS_OFF(semaphore, extension->type)) {
            /* type + data length */
            length += HELLO_EXT_TYPE_SZ + OPAQUE16_LEN;

            switch (extension->type) {
                case SERVER_NAME_INDICATION:
                    if (isRequest)
                        length += SNI_GET_SIZE((SNI *) extension->data);
                    break;
            }

            TURN_ON(semaphore, extension->type);
        }
    }

    return length;
}

static word16 TLSX_Write(TLSX* list, byte* output, byte* semaphore,
                                                                 byte isRequest)
{
    TLSX* extension;
    word16 offset = 0;
    word16 length_offset = 0;

    while ((extension = list)) {
        list = extension->next;

        if (!isRequest && !extension->resp)
            continue; /* skip! */

        if (IS_OFF(semaphore, extension->type)) {
            /* extension type */
            c16toa(extension->type, output + offset);
            offset += HELLO_EXT_TYPE_SZ + OPAQUE16_LEN;
            length_offset = offset;

            /* extension data should be written internally */
            switch (extension->type) {
                case SERVER_NAME_INDICATION:
                    if (isRequest)
                        offset += SNI_WRITE((SNI *) extension->data,
                                                               output + offset);
                    break;
            }

            /* writing extension data length */
            c16toa(offset - length_offset,
                                         output + length_offset - OPAQUE16_LEN);

            TURN_ON(semaphore, extension->type);
        }
    }

    return offset;
}

#ifndef NO_CYASSL_CLIENT

word16 TLSX_GetRequestSize(CYASSL* ssl)
{
    word16 length = 0;

    if (ssl && IsTLS(ssl)) {
        byte semaphore[16] = {0};

        if (ssl->extensions)
            length += TLSX_GetSize(ssl->extensions, semaphore, 1);

        if (ssl->ctx && ssl->ctx->extensions)
            length += TLSX_GetSize(ssl->ctx->extensions, semaphore, 1);

        if (IsAtLeastTLSv1_2(ssl) && ssl->suites->hashSigAlgoSz)
            length += ssl->suites->hashSigAlgoSz + HELLO_EXT_LEN;
    }

    if (length)
        length += OPAQUE16_LEN; /* for total length storage */

    return length;
}

word16 TLSX_WriteRequest(CYASSL* ssl, byte* output)
{
    word16 offset = 0;

    if (ssl && IsTLS(ssl) && output) {
        byte semaphore[16] = {0};

        offset += OPAQUE16_LEN; /* extensions length */

        if (ssl->extensions)
            offset += TLSX_Write(ssl->extensions, output + offset,
                                                                  semaphore, 1);

        if (ssl->ctx && ssl->ctx->extensions)
            offset += TLSX_Write(ssl->ctx->extensions, output + offset,
                                                                  semaphore, 1);

        if (IsAtLeastTLSv1_2(ssl) && ssl->suites->hashSigAlgoSz)
        {
            int i;
            /* extension type */
            c16toa(HELLO_EXT_SIG_ALGO, output + offset);
            offset += HELLO_EXT_TYPE_SZ;

            /* extension data length */
            c16toa(OPAQUE16_LEN + ssl->suites->hashSigAlgoSz, output + offset);
            offset += OPAQUE16_LEN;

            /* sig algos length */
            c16toa(ssl->suites->hashSigAlgoSz, output + offset);
            offset += OPAQUE16_LEN;

            /* sig algos */
            for (i = 0; i < ssl->suites->hashSigAlgoSz; i++, offset++)
                output[offset] = ssl->suites->hashSigAlgo[i];
        }

        if (offset > OPAQUE16_LEN)
            c16toa(offset - OPAQUE16_LEN, output); /* extensions length */
    }

    return offset;
}

#endif /* NO_CYASSL_CLIENT */

#ifndef NO_CYASSL_SERVER

word16 TLSX_GetResponseSize(CYASSL* ssl)
{
    word16 length = 0;
    byte semaphore[16] = {0};

    if (ssl && IsTLS(ssl))
        length += TLSX_GetSize(ssl->extensions, semaphore, 0);

    /* All the response data is set at the ssl object only, so no ctx here. */

    if (length)
        length += OPAQUE16_LEN; /* for total length storage */

    return length;
}

word16 TLSX_WriteResponse(CYASSL *ssl, byte* output)
{
    word16 offset = 0;

    if (ssl && IsTLS(ssl) && output) {
        byte semaphore[16] = {0};

        offset += OPAQUE16_LEN; /* extensions length */

        offset += TLSX_Write(ssl->extensions, output + offset, semaphore, 0);

        if (offset > OPAQUE16_LEN)
            c16toa(offset - OPAQUE16_LEN, output); /* extensions length */
    }

    return offset;
}

#endif /* NO_CYASSL_SERVER */

int TLSX_Parse(CYASSL* ssl, byte* input, word16 length, byte isRequest,
                                                                 Suites *suites)
{
    int ret = 0;
    word16 offset = 0;

    if (!ssl || !input || !suites)
        return BAD_FUNC_ARG;

    while (ret == 0 && offset < length) {
        word16 type;
        word16 size;

        if (length - offset < HELLO_EXT_TYPE_SZ + OPAQUE16_LEN)
            return INCOMPLETE_DATA;

        ato16(input + offset, &type);
        offset += HELLO_EXT_TYPE_SZ;

        ato16(input + offset, &size);
        offset += OPAQUE16_LEN;

        if (offset + size > length)
            return INCOMPLETE_DATA;

        switch (type) {
            case SERVER_NAME_INDICATION:
                ret = SNI_PARSE(ssl, input + offset, size, isRequest);
                break;

            case HELLO_EXT_SIG_ALGO:
                if (isRequest) {
                    /* do not mess with offset inside the switch! */
                    if (IsAtLeastTLSv1_2(ssl)) {
                        ato16(input + offset, &suites->hashSigAlgoSz);

                        if (suites->hashSigAlgoSz > size - OPAQUE16_LEN)
                            return INCOMPLETE_DATA;

                        XMEMCPY(suites->hashSigAlgo,
                                input + offset + OPAQUE16_LEN,
                                min(suites->hashSigAlgoSz,
                                                        HELLO_EXT_SIGALGO_MAX));
                    }
                } else {
                    CYASSL_MSG("Servers MUST NOT send SIG ALGO extension.");
                }

                break;
        }

        /* offset should be updated here! */
        offset += size;
    }

    return ret;
}

/* undefining semaphore macros */
#undef IS_OFF
#undef TURN_ON

#endif /* HAVE_TLS_EXTENSIONS */


#ifndef NO_CYASSL_CLIENT

#ifndef NO_OLD_TLS

    CYASSL_METHOD* CyaTLSv1_client_method(void)
    {
        CYASSL_METHOD* method =
                             (CYASSL_METHOD*) XMALLOC(sizeof(CYASSL_METHOD), 0,
                                                      DYNAMIC_TYPE_METHOD);
        if (method)
            InitSSL_Method(method, MakeTLSv1());
        return method;
    }


    CYASSL_METHOD* CyaTLSv1_1_client_method(void)
    {
        CYASSL_METHOD* method =
                              (CYASSL_METHOD*) XMALLOC(sizeof(CYASSL_METHOD), 0,
                                                       DYNAMIC_TYPE_METHOD);
        if (method)
            InitSSL_Method(method, MakeTLSv1_1());
        return method;
    }

#endif /* !NO_OLD_TLS */

#ifndef NO_SHA256   /* can't use without SHA256 */

    CYASSL_METHOD* CyaTLSv1_2_client_method(void)
    {
        CYASSL_METHOD* method =
                              (CYASSL_METHOD*) XMALLOC(sizeof(CYASSL_METHOD), 0,
                                                       DYNAMIC_TYPE_METHOD);
        if (method)
            InitSSL_Method(method, MakeTLSv1_2());
        return method;
    }

#endif


    CYASSL_METHOD* CyaSSLv23_client_method(void)
    {
        CYASSL_METHOD* method =
                              (CYASSL_METHOD*) XMALLOC(sizeof(CYASSL_METHOD), 0,
                                                       DYNAMIC_TYPE_METHOD);
        if (method) {
#ifndef NO_SHA256         /* 1.2 requires SHA256 */
            InitSSL_Method(method, MakeTLSv1_2());
#else
            InitSSL_Method(method, MakeTLSv1_1());
#endif
#ifndef NO_OLD_TLS
            method->downgrade = 1;
#endif 
        }
        return method;
    }


#endif /* NO_CYASSL_CLIENT */



#ifndef NO_CYASSL_SERVER

#ifndef NO_OLD_TLS

    CYASSL_METHOD* CyaTLSv1_server_method(void)
    {
        CYASSL_METHOD* method =
                              (CYASSL_METHOD*) XMALLOC(sizeof(CYASSL_METHOD), 0,
                                                       DYNAMIC_TYPE_METHOD);
        if (method) {
            InitSSL_Method(method, MakeTLSv1());
            method->side = SERVER_END;
        }
        return method;
    }


    CYASSL_METHOD* CyaTLSv1_1_server_method(void)
    {
        CYASSL_METHOD* method =
                              (CYASSL_METHOD*) XMALLOC(sizeof(CYASSL_METHOD), 0,
                                                       DYNAMIC_TYPE_METHOD);
        if (method) {
            InitSSL_Method(method, MakeTLSv1_1());
            method->side = SERVER_END;
        }
        return method;
    }

#endif /* !NO_OLD_TLS */

#ifndef NO_SHA256   /* can't use without SHA256 */

    CYASSL_METHOD* CyaTLSv1_2_server_method(void)
    {
        CYASSL_METHOD* method =
                              (CYASSL_METHOD*) XMALLOC(sizeof(CYASSL_METHOD), 0,
                                                       DYNAMIC_TYPE_METHOD);
        if (method) {
            InitSSL_Method(method, MakeTLSv1_2());
            method->side = SERVER_END;
        }
        return method;
    }

#endif


    CYASSL_METHOD* CyaSSLv23_server_method(void)
    {
        CYASSL_METHOD* method =
                              (CYASSL_METHOD*) XMALLOC(sizeof(CYASSL_METHOD), 0,
                                                       DYNAMIC_TYPE_METHOD);
        if (method) {
#ifndef NO_SHA256         /* 1.2 requires SHA256 */
            InitSSL_Method(method, MakeTLSv1_2());
#else
            InitSSL_Method(method, MakeTLSv1_1());
#endif
            method->side      = SERVER_END;
#ifndef NO_OLD_TLS
            method->downgrade = 1;
#endif /* !NO_OLD_TLS */
        }
        return method;
    }



#endif /* NO_CYASSL_SERVER */
#endif /* NO_TLS */

