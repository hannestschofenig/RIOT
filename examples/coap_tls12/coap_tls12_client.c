/*
 * Copyright (C) 2019 Gabriele Restuccia
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     examples
 * @{
 *
 * @file
 * @brief       Example application of ATLS 1.2 over CoAP
 *
 * @author      Gabriele Restuccia <restuccia.1548310@studenti.uniroma1.it>
 *
 * @}
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wolfssl/ssl.h>
#include "log.h"
#include "net/gcoap.h"
#include "mutex.h"

#define VERBOSE 0

#ifdef MODULE_WOLFSSL_PSK

#define PAYLOAD_TLS_SIZE 256

#else

#define PAYLOAD_TLS_SIZE 1024

#endif

/* identity is OpenSSL testing default for openssl s_client, keep same */
static const char* kIdentityStr = "Client_identity";

static int config_index = 2;
static char *config[] = {"PSK-AES128-CCM", "PSK-AES128-GCM-SHA256", "PSK-AES256-GCM-SHA384", "ECDHE-ECDSA-AES128-CCM-8", "ECDHE-ECDSA-AES128-GCM-SHA256", "ECDHE-ECDSA-AES256-GCM-SHA384"};

extern size_t _send(uint8_t *buf, size_t len, char *addr_str, char *port_str);

extern char payload_tls[];
extern int size_payload;

extern const unsigned char client_cert[];
extern const int client_cert_len;

extern const unsigned char client_key[];
extern const int client_key_len;

extern const unsigned char ca_cert[];
extern const int ca_cert_len;

char *addr_str;

int count_read = 0;
int count_send = 0;
static int offset = 0;
static int get_flag = 0;

extern mutex_t client_lock;
extern mutex_t client_send_lock;

#ifdef MODULE_WOLFSSL_PSK

static inline unsigned int my_psk_client_cb(WOLFSSL* ssl, const char* hint,
        char* identity, unsigned int id_max_len, unsigned char* key,
        unsigned int key_max_len)
{
    (void)ssl;
    (void)hint;
    (void)key_max_len;

    /* see internal.h MAX_PSK_ID_LEN for PSK identity limit */
    strncpy(identity, kIdentityStr, id_max_len);

    int i;
    int b = 0x01;

    for (i = 0; i < 64; i++, b += 0x22) {
        if (b >= 0x100)
            b = 0x01;
        key[i] = b;
    }

    return 64;   /* length of key in octets or 0 for error */
}
#endif

static void usage(const char *cmd_name)
{
    LOG(LOG_ERROR, "Usage: %s <server-address>\n", cmd_name);
}

int coap_post(void)
{
    /*
        For initializing a COAP packet we need a buffer which can contain all of the header options for
        a PDU and the eventual payload.
    */

    // The GCOAP macro is 128B because it is typically enough to hold all the header options
    // But we have to be sure it is enoguh to hold also the payload!!!
    // We solve that by redefining it in the Makefile.
    uint8_t buf_pdu[GCOAP_PDU_BUF_SIZE];
    coap_pkt_t pdu;
    size_t len;
    size_t paylen;

    //Using strlen here is stupid. It will understand zeroes as end of a string
    paylen = size_payload;

    // Code '2' is POST
    gcoap_req_init(&pdu, &buf_pdu[0], GCOAP_PDU_BUF_SIZE, 2, "/.well-known/atls");

    coap_opt_add_format(&pdu, COAP_FORMAT_TEXT);
    len = coap_opt_finish(&pdu, COAP_OPT_FINISH_PAYLOAD);

    // The payload len tells how many bytes are free for the payload. If we have
    // enough space we can copy our message inside it.
    if (pdu.payload_len >= paylen) {
                memcpy(pdu.payload, payload_tls, paylen);
                len += paylen;
    } else {
                puts("gcoap_cli: msg buffer too small");
                return -1;
    }

    if (!_send(&buf_pdu[0], len, addr_str, "5683")){
        puts("gcoap_cli: msg send failed");
        return -1;
    }

    return 0;
}

int coap_get(void)
{
    uint8_t buf_pdu[GCOAP_PDU_BUF_SIZE];
    coap_pkt_t pdu;
    size_t len;

    // Code '1' is GET
    gcoap_req_init(&pdu, &buf_pdu[0], GCOAP_PDU_BUF_SIZE, 1, "/.well-known/atls");
    len = coap_opt_finish(&pdu, COAP_OPT_FINISH_NONE);

    if (!_send(&buf_pdu[0], len, addr_str, "5683")){
        puts("gcoap_cli: msg send failed");
        return -1;
    }

    return 0;
}

int client_send(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    (void) ssl;
    (void) sz;
    (void) ctx;

    //printf("SEND %d\n",count_send);

    /*
        Why 3 and 4? They are the client's messages seq IDs in which the server needs to do more
        reads without doing any writes between them. We need someone in charge to restore
        COAP - TLS mutexes synchronization.

        TODO: it's not good practice AT ALL to have local counters. It will be a good idea to parse the seq
        numbers directly from the packets and handle eventual packet loss.
    */
    #ifndef MODULE_WOLFSSL_PSK
        if(count_send == 2 || count_send == 3 || count_send == 4 || count_send == 5) mutex_lock(&client_send_lock);
    #else
        if(count_send == 2 || count_send == 3) mutex_lock(&client_send_lock);
    #endif

    memcpy(payload_tls,buf,sz);
    size_payload = sz;

    if(VERBOSE){
        int i;

        printf("/*-------------------- CLIENT SEND -----------------*/\n");
        for (i = 0; i < size_payload; i++) {
            printf("%02x ", (unsigned char) payload_tls[i]);
            if (i > 0 && (i % 16) == 0)
                printf("\n");
        }
        printf("\n/*-------------------- END SEND -----------------*/\n");
    }

    coap_post();

    count_send += 1;

    return sz;
}

int client_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    (void) ssl;
    (void) buf;
    (void) sz;
    (void) ctx;
    int i;

    //printf("RECV %d\n", count_read);

    /*  
        Why 2, 3 and 5? They are the server's messages seq IDs in which the client needs to do more
        reads without doing any writes between them. Without the the writes we can't have
        any call to COAP's 'send' function because we don't actually have anything to send.
        In order to have a request -> response mechanism (and in order to let the server know
        the client's ip address when replying) we adopt this cheap trick calling a 'get'.

        TODO: it's not good practice AT ALL to have local counters. It will be a good idea to parse the seq
        numbers directly from the packets and handle eventual packet loss.
    */

    #ifndef MODULE_WOLFSSL_PSK
        if(count_read == 1 || count_read == 2 || count_read == 3 || count_read == 4 || count_read == 6){
            if(!get_flag) coap_get();
            get_flag = 1;
        }
    #else
        if(count_read == 1 || count_read == 2 || count_read == 4){
            if(!get_flag) coap_get();
            get_flag = 1;
        }
    #endif

    if(!offset) mutex_lock(&client_lock);

    memcpy(buf, payload_tls+offset, sz);

    offset += sz;

    if(VERBOSE){
        printf("/*-------------------- CLIENT RECV -----------------*/\n");
        for (i = 0; i < sz; i++) {
            printf("%02x ", (unsigned char) buf[i]);
            if (i > 0 && (i % 16) == 0)
                printf("\n");
        }
        printf("\n/*-------------------- END RECV -----------------*/\n");
    }

    if(offset == size_payload){
        offset = 0;
        get_flag = 0;
    }

    if(!offset) count_read += 1;
    
    return sz;
}

WOLFSSL* Client(WOLFSSL_CTX* ctx, char* suite, int setSuite, int doVerify)
{
    WOLFSSL* ssl = NULL;
    int ret;

    if ((ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method())) == NULL) {
        printf("Error in setting client ctx\n");
        return NULL;
    }

#ifndef MODULE_WOLFSSL_PSK
    /* Disable certificate validation from the client side */
    wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_PEER |
                     SSL_VERIFY_FAIL_IF_NO_PEER_CERT, 0);

    /* Load certificate file for the TLS CA */
    if (wolfSSL_CTX_load_verify_buffer(ctx, ca_cert,
                ca_cert_len, SSL_FILETYPE_PEM ) != SSL_SUCCESS)
    {
        LOG(LOG_ERROR, "Error loading CA cert buffer\n");
        return NULL;
    }

    /* Load certificate file for the TLS server */
    if (wolfSSL_CTX_use_certificate_buffer(ctx, client_cert,
                client_cert_len, SSL_FILETYPE_PEM ) != SSL_SUCCESS)
    {
        LOG(LOG_ERROR, "Failed to load certificate from memory.\r\n");
        return NULL;
    }

    /* Load the private key */
    if (wolfSSL_CTX_use_PrivateKey_buffer(ctx, client_key,
                client_key_len, SSL_FILETYPE_PEM ) != SSL_SUCCESS)
    {
        LOG(LOG_ERROR, "Failed to load private key from memory.\r\n");
        return NULL;
    }

    ret = wolfSSL_CTX_UseSNI(ctx, WOLFSSL_SNI_HOST_NAME, "www.prova.com",
    strlen("www.prova.com"));
    if (ret != SSL_SUCCESS) {
        printf("ret = %d\n", ret);
        printf("Error :can't set SNI\n");
        wolfSSL_CTX_free(ctx);
        return NULL;
    }

#else /* !def MODULE_WOLFSSL_PSK */
    wolfSSL_CTX_set_psk_client_callback(ctx, my_psk_client_cb);
#endif

    if (( ret = wolfSSL_CTX_set_cipher_list(ctx, config[config_index])) != SSL_SUCCESS) {
            printf("ret = %d\n", ret);
            printf("Error :can't set cipher\n");
            wolfSSL_CTX_free(ctx);
            return NULL;
    }

    wolfSSL_SetIORecv(ctx, client_recv);
    wolfSSL_SetIOSend(ctx, client_send);

    if ((ssl = wolfSSL_new(ctx)) == NULL) {
        printf("issue when creating ssl\n");
        wolfSSL_CTX_free(ctx);
        return NULL;
    }

    return ssl;
}

void client_cleanup(WOLFSSL* ssl, WOLFSSL_CTX* ctx)
{
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
}

int start_tls_client(int argc, char **argv)
{
    WOLFSSL* sslCli;
    WOLFSSL_CTX* ctxCli = NULL;

    (void) argc;
    (void) argv;

    if (argc != 2) {
        usage(argv[0]);
        return -1;
    }

    addr_str = argv[1];

    int ret = SSL_FAILURE;

    char buf[PAYLOAD_TLS_SIZE];

    //wolfSSL_Debugging_ON();

    wolfSSL_Init();

    //  Example usage (not implemented)
    //  sslServ = Server(ctxServ, "ECDHE-RSA-AES128-SHA", 1);
    sslCli  = Client(ctxCli, NULL, 0, 0);

    if (sslCli == NULL) {
        printf("Failed to start client\n");
        client_cleanup(sslCli,ctxCli);
        return -1;
    }

    printf("Starting client\n");
    while (ret != SSL_SUCCESS) {
        int error;

        /* client connect */
        ret |= wolfSSL_connect(sslCli);
        error = wolfSSL_get_error(sslCli, 0);
        if (ret != SSL_SUCCESS) {
            if (error != SSL_ERROR_WANT_READ &&
                error != SSL_ERROR_WANT_WRITE) {
                printf("client ssl connect failed\n");
                client_cleanup(sslCli,ctxCli);
                return -1;
            }
        }
    }

    printf("CLIENT CONNECTED SUCCESSFULLY!\n");
    printf("TLS version is %s\n", wolfSSL_get_version(sslCli));
    printf("Cipher Suite is %s\n",
           wolfSSL_CIPHER_get_name(wolfSSL_get_current_cipher(sslCli)));
/*
    char send_msg[] = "This is ATLS client!";

    printf("Sending hello message...\n");
    wolfSSL_write(sslCli, send_msg, strlen(send_msg));

    wolfSSL_read(sslCli, buf, PAYLOAD_TLS_SIZE);
    buf[size_payload] = (char)0;

    //  TODO: probably the string isn't terminated correctly and sometimes
    //  can print random chars
    
    LOG(LOG_INFO, "Received: '%s'\r\n", buf);

     Clean up and exit. */
    LOG(LOG_INFO, "Closing connection.\r\n");

    client_cleanup(sslCli,ctxCli);

    return 0;
}