#include <stdio.h>
#include <string.h>

//#include "mbedtls/net.h"
#include "mbedtls/config.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"

#include "mutex.h"
#include "thread.h"

#include "log.h"

#include "certs.h"

#ifdef MBEDTLS_PLATFORM_MEMORY
#include "mbedtls/platform.h"
#endif

#define mbedtls_fprintf    fprintf
#define mbedtls_printf     printf

#define VERBOSE 0

#define RESPONSE "This is ATLS server!\n"

#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
    // !!!CAREFUL!!! ONLY FOR TESTING PURPOSES!
    #define DFL_PSK                 "a66d258de75987d31a4537ecd1ff7a34517bf92f2c07abb20fa0fb517f2491f1"
    #define DFL_PSK_IDENTITY        "Client_identity"

    static unsigned char psk[MBEDTLS_PSK_MAX_LEN];
    static size_t psk_len = 0;
#endif

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;
static mbedtls_ssl_context ssl;
static mbedtls_ssl_config conf;

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    static mbedtls_x509_crt srvcert;
    static mbedtls_pk_context pkey;

    static mbedtls_x509_crt cacert;

    typedef struct _sni_entry sni_entry;

    struct _sni_entry {
    const char *name;
        mbedtls_x509_crt *cert;
        mbedtls_pk_context *key;
        mbedtls_x509_crt* ca;
        mbedtls_x509_crl* crl;
        int authmode;
        sni_entry *next;
    };
#endif

extern char payload_tls[];
extern int size_payload;

extern mutex_t server_lock;
extern mutex_t server_req_lock;

extern kernel_pid_t main_pid;

#ifdef MBEDTLS_PLATFORM_MEMORY
extern unsigned int mem_max;

extern void* MyCalloc(size_t n, size_t size);
extern void MyFree(void* ptr);
#endif

static int offset = 0;
static int wake_flag = 0;

static int tls_version = MBEDTLS_SSL_MINOR_VERSION_3;

static int cipher[2];

static int recv_count;

static void usage(const char *cmd_name)
{
    LOG(LOG_ERROR, "\nUsage: %s [optional: <key_exchange_mode> <tls_version>]\n\n<key_exchange_mode: psk (default), psk_dhe, psk_all, ecdhe_ecdsa, all>\n<tls_version: tls1_2, tls1_3 (default)>\n", cmd_name);
}

static void my_debug( void *ctx, int level,
                      const char *file, int line,
                      const char *str )
{
    ((void) level);

    mbedtls_fprintf( (FILE *) ctx, "%s:%04d: %s", file, line, str );
    fflush(  (FILE *) ctx  );
}

#if defined(MBEDTLS_X509_CRT_PARSE_C)

int sni_callback( void *p_info, mbedtls_ssl_context *ssl,
                  const unsigned char *name, size_t name_len )
{
    int ret;

    if( ( ret = mbedtls_ssl_conf_own_cert( &conf, &srvcert, &pkey ) ) != 0 )
        {
            mbedtls_printf( " failed\n  ! mbedtls_ssl_conf_own_cert returned %d\n\n", ret );
        }

    return ret;

}

#endif

static int mbedtls_ssl_send(void *ctx, const unsigned char *buf, size_t len)
{
    unsigned int i;

    //printf("Server SEND... %d\n",len);
    //printf("SEND ssl state %d\n",ssl.state);

    mutex_lock(&server_req_lock);

    if(VERBOSE){
        printf("/*-------------------- SERVER SENDING -----------------*/\n");
        for (i = 0; i < len; i++) {
            printf("%02x ", (unsigned char) buf[i]);
            if (i > 0 && (i % 16) == 0)
                printf("\n");
        }
        printf("\n/*-------------------- END SENDING -----------------*/\n");
    }

    memcpy(payload_tls, buf, len);
    size_payload = len;

    thread_wakeup(main_pid);

    return len;
}

static int mbedtls_ssl_recv(void *ctx, unsigned char *buf, size_t len)
{
    unsigned int i;

    //printf("Server RECV... %d\n",recv_count);

    //printf("RECV ssl state %d\n",ssl.state);

    if(!offset){
        mutex_lock(&server_lock);
    }

    memcpy(buf, payload_tls+offset, len);

    offset += len;

    if(VERBOSE){
        printf("/*-------------------- SERVER RECV -----------------*/\n");
        for (i = 0; i < len; i++) {
            printf("%02x ", (unsigned char) buf[i]);
            if (i > 0 && (i % 16) == 0)
                printf("\n");
        }
        printf("\n/*-------------------- END RECV -----------------*/\n");
    }

#if defined(MBEDTLS_CERTS_C)
    if(recv_count == 1 || recv_count == 2 || recv_count == 3 || recv_count == 4){
        if(wake_flag){
            size_payload = 0;
            offset = 0;
            thread_wakeup(main_pid);
            wake_flag = 0;
        } else {
            wake_flag = 1;
        }
    }
#else
    if(recv_count == 1 || recv_count == 2){
        if(wake_flag){
            size_payload = 0;
            offset = 0;
            thread_wakeup(main_pid);
            wake_flag = 0;
        } else {
            wake_flag = 1;
        }
    }
#endif

    if(offset == size_payload){
        offset = 0;
        recv_count += 1;
    }

    return len;
}

int mbedtls_server_init(void)
{
    int ret;

    const char *pers = "ssl_server";

    mbedtls_ssl_init( &ssl );
    mbedtls_ssl_config_init( &conf );
    mbedtls_ctr_drbg_init( &ctr_drbg );

    #if defined(MBEDTLS_X509_CRT_PARSE_C)
        sni_entry *sni_info = NULL;
    #endif

    mbedtls_entropy_init( &entropy );

    if( ( ret = mbedtls_ctr_drbg_seed( &ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *) pers,
                               strlen( pers ) ) ) != 0 )
    {
        mbedtls_printf( " failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret );
        return ret;
    }

    #if defined(MBEDTLS_X509_CRT_PARSE_C)
        mbedtls_x509_crt_init( &srvcert );
        mbedtls_x509_crt_init( &cacert );
        mbedtls_pk_init( &pkey );

        // !!!CAREFUL!!! ONLY FOR TESTING PURPOSES!
        ret = mbedtls_x509_crt_parse( &srvcert, (const unsigned char *) server_cert,
                              server_cert_len );
        if( ret != 0 )
        {
            mbedtls_printf( " failed\n  !  server mbedtls_x509_crt_parse returned %d\n\n", ret );
            return ret;
        }

        ret = mbedtls_x509_crt_parse( &cacert, (const unsigned char *) ca_cert,
                              ca_cert_len );
        if( ret != 0 )
        {
            mbedtls_printf( " failed\n  !  ca mbedtls_x509_crt_parse returned %d\n\n", ret );
            return ret;
        }

        ret =  mbedtls_pk_parse_key( &pkey, (const unsigned char *) server_key,
                             server_key_len, NULL, 0 );
        if( ret != 0 )
        {
            mbedtls_printf( " failed\n  !  mbedtls_pk_parse_key returned %d\n\n", ret );
            return ret;
        }
    #endif

    if( ( ret = mbedtls_ssl_config_defaults( &conf,
                    MBEDTLS_SSL_IS_SERVER,
                    MBEDTLS_SSL_TRANSPORT_STREAM,
                    MBEDTLS_SSL_PRESET_DEFAULT ) ) != 0 )
    {
        mbedtls_printf( " failed\n  ! mbedtls_ssl_config_defaults returned %d\n\n", ret );
        return ret;
    }

    /** TLS 1.2
    mbedtls_ssl_conf_min_version( &conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version( &conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    **/

    /** TLS 1.3
    mbedtls_ssl_conf_min_version( &conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_4);
    mbedtls_ssl_conf_max_version( &conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_4);
    **/

    mbedtls_ssl_conf_min_version( &conf, MBEDTLS_SSL_MAJOR_VERSION_3, tls_version);
    mbedtls_ssl_conf_max_version( &conf, MBEDTLS_SSL_MAJOR_VERSION_3, tls_version);

    mbedtls_ssl_conf_rng( &conf, mbedtls_ctr_drbg_random, &ctr_drbg );
    mbedtls_ssl_conf_dbg( &conf, my_debug, stdout );

#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
    /*
     * Unhexify the pre-shared key if any is given
     */

    if( strlen( DFL_PSK ) )
    {
        unsigned char c;
        size_t j;

        if( strlen( DFL_PSK ) % 2 != 0 )
        {
            printf("pre-shared key not valid hex\n");
            return -1;
        }

        psk_len = strlen( DFL_PSK ) / 2;

        for( j = 0; j < strlen( DFL_PSK ); j += 2 )
        {
            c = DFL_PSK[j];
            if( c >= '0' && c <= '9' )
                c -= '0';
            else if( c >= 'a' && c <= 'f' )
                c -= 'a' - 10;
            else if( c >= 'A' && c <= 'F' )
                c -= 'A' - 10;
            else
            {
                printf("pre-shared key not valid hex\n");
                return -1;
            }
            psk[ j / 2 ] = c << 4;

            c = DFL_PSK[j + 1];
            if( c >= '0' && c <= '9' )
                c -= '0';
            else if( c >= 'a' && c <= 'f' )
                c -= 'a' - 10;
            else if( c >= 'A' && c <= 'F' )
                c -= 'A' - 10;
            else
            {
                printf("pre-shared key not valid hex\n");
                return -1;
            }
            psk[ j / 2 ] |= c;
        }
    }

    if( ( ret = mbedtls_ssl_conf_psk( &conf, psk, psk_len,
                             (const unsigned char *) DFL_PSK_IDENTITY,
                             strlen( DFL_PSK_IDENTITY ) ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_conf_psk returned %d\n\n", ret );
        return ret;
    }

#endif /* MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED */    

/**
    PSK:    TLS-PSK-WITH-AES-128-CCM
            TLS-PSK-WITH-AES-128-GCM-SHA256 
            TLS-PSK-WITH-AES-256-GCM-SHA384

            TLS-ECDHE-ECDSA-WITH-AES-128-CCM
            TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256
            TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384
**/

    cipher[0] = mbedtls_ssl_get_ciphersuite_id("TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384");
    cipher[1] = 0;

    if (cipher[0] == 0)
            {
                mbedtls_printf("forced ciphersuite not found\n");
                ret = 2;
                return ret;
    }
/*
    const mbedtls_ssl_ciphersuite_t *ciphersuite_info;
    ciphersuite_info = mbedtls_ssl_ciphersuite_from_id( cipher[0] );
*/
    mbedtls_ssl_conf_ciphersuites( &conf, cipher );

    #if defined(MBEDTLS_X509_CRT_PARSE_C)

        mbedtls_ssl_conf_ca_chain( &conf, &cacert, NULL );

        mbedtls_ssl_conf_sni( &conf, sni_callback, sni_info );

        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);

    #endif

    if( ( ret = mbedtls_ssl_setup( &ssl, &conf ) ) != 0 )
    {
        mbedtls_printf( " failed\n  ! mbedtls_ssl_setup returned %d\n\n", ret );
        return ret;
    }

    mbedtls_ssl_session_reset( &ssl );

    mbedtls_ssl_set_bio( &ssl, NULL, mbedtls_ssl_send, mbedtls_ssl_recv, NULL );

    return ret;
}

void mbedtls_server_exit(int ret)
{
#ifdef MBEDTLS_ERROR_C
    if( ret != 0 )
    {
        char error_buf[100];
        mbedtls_strerror( ret, error_buf, 100 );
        mbedtls_printf("Last error was: %d - %s\n\n", ret, error_buf );
    }
#endif

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    mbedtls_x509_crt_free( &srvcert );
    mbedtls_pk_free( &pkey );
#endif

    mbedtls_ssl_free( &ssl );
    mbedtls_ssl_config_free( &conf );
    mbedtls_ctr_drbg_free( &ctr_drbg );
    mbedtls_entropy_free( &entropy );

    printf("Exiting mbedtls...\n");
}

int start_server(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int ret;
    int len;
    unsigned char buf[MBEDTLS_SSL_MAX_CONTENT_LEN + 1];

    printf("Initializing server...\n");

    //mbedtls_debug_set_threshold(3);

#ifdef MBEDTLS_PLATFORM_MEMORY
    mbedtls_platform_set_calloc_free(MyCalloc,MyFree);
#endif

    ret = mbedtls_server_init();
    if( ret != 0){
        printf("mbedtls_client_init() failed!\n");
        mbedtls_server_exit(ret);
        return ret;
    }
    
    printf("Proceeding to handshake...\n");
    while( ( ret = mbedtls_ssl_handshake( &ssl ) ) != 0 )
    {
        if( ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE )
        {
            mbedtls_printf( " failed\n  ! mbedtls_ssl_handshake returned %d\n\n", ret );
            mbedtls_server_exit(ret);
            return ret;
        }
    }

    printf(">>> SERVER CONNECTED SUCCESSFULLY!\n");
    printf("Protocol is %s \nCiphersuite is %s\n\n",
        mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));
/*
    len = sizeof(buf) - 1;
    memset( buf, 0, sizeof(buf) );
    ret = mbedtls_ssl_read( &ssl, buf, len );

    len = ret;
    buf[len] = '\0';
    printf( ">>> %d bytes read\n\n%s\n", len, (char *) buf );

    memset( buf, 0, sizeof(buf) );
    len = sprintf( (char *) buf, RESPONSE );

    ret = mbedtls_ssl_write( &ssl, buf, len );
*/
    mbedtls_ssl_close_notify( &ssl );

#ifdef MBEDTLS_PLATFORM_MEMORY
    printf("MAX HEAP IS %d\n", mem_max);
#endif

    mbedtls_server_exit(0);

    return ret;
}