/* Pico HTTPS request example *************************************************
 *                                                                            *
 *  An HTTPS client example for the Raspberry Pi Pico W                       *
 *                                                                            *
 *  A simple yet complete example C application which sends a single request  *
 *  to a web server over HTTPS and reads the resulting response.              *
 *                                                                            *
 ******************************************************************************/

#ifndef PICOHTTPS_H
#define PICOHTTPS_H



/* Options ********************************************************************/

// Wireless region
//
//  `country` argument to cyw43_arch_init_with_country().
//
//  For best performance, set to local region.
//
//  https://www.raspberrypi.com/documentation/pico-sdk/networking.html#CYW43_COUNTRY_
//
#define PICOHTTPS_INIT_CYW43_COUNTRY                CYW43_COUNTRY_SWEDEN

// Wireless network SSID
#define PICOHTTPS_WIFI_SSID                         "Zzz"

// Wireless network connection timeout
//
//  `timeout` argument to cyw43_arch_wifi_connect_timeout_ms().
//
//  https://www.raspberrypi.com/documentation/pico-sdk/networking.html
//
#define PICOHTTPS_WIFI_TIMEOUT                      30000           // ms

// Wireless network password
//
//  N.b. _Strongly_ recommend setting this from the environment rather than
//  here. Environment values will have greater precedence. See CMakeLists.txt.
//
#ifndef PICOHTTPS_WIFI_PASSWORD
#define PICOHTTPS_WIFI_PASSWORD                     "i6b22krm"
#endif // PICOHTTPS_WIFI_PASSWORD

// HTTP server hostname
#define PICOHTTPS_HOSTNAME                          "webhook.site"

// DNS response polling interval
//
//  Interval with which to poll for responses to DNS queries.
//
#define PICOHTTPS_RESOLVE_POLL_INTERVAL             100             // ms

// Certificate authority root certificate
//
//  CA certificate used to sign the HTTP server's certificate. DER or PEM
//  formats (char array representation).
//
//  This is most readily obtained via inspection of the server's certificate
//  chain, e.g. in a browser.
//
// #define PICOHTTPS_CA_ROOT_CERT \
// "-----BEGIN CERTIFICATE-----\n" \
// "MIIFBjCCAu6gAwIBAgIRAMISMktwqbSRcdxA9+KFJjwwDQYJKoZIhvcNAQELBQAw\n" \
// "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n" \
// "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMjQwMzEzMDAwMDAw\n" \
// "WhcNMjcwMzEyMjM1OTU5WjAzMQswCQYDVQQGEwJVUzEWMBQGA1UEChMNTGV0J3Mg\n" \
// "RW5jcnlwdDEMMAoGA1UEAxMDUjEyMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB\n" \
// "CgKCAQEA2pgodK2+lP474B7i5Ut1qywSf+2nAzJ+Npfs6DGPpRONC5kuHs0BUT1M\n" \
// "5ShuCVUxqqUiXXL0LQfCTUA83wEjuXg39RplMjTmhnGdBO+ECFu9AhqZ66YBAJpz\n" \
// "kG2Pogeg0JfT2kVhgTU9FPnEwF9q3AuWGrCf4yrqvSrWmMebcas7dA8827JgvlpL\n" \
// "Thjp2ypzXIlhZZ7+7Tymy05v5J75AEaz/xlNKmOzjmbGGIVwx1Blbzt05UiDDwhY\n" \
// "XS0jnV6j/ujbAKHS9OMZTfLuevYnnuXNnC2i8n+cF63vEzc50bTILEHWhsDp7CH4\n" \
// "WRt/uTp8n1wBnWIEwii9Cq08yhDsGwIDAQABo4H4MIH1MA4GA1UdDwEB/wQEAwIB\n" \
// "hjAdBgNVHSUEFjAUBggrBgEFBQcDAgYIKwYBBQUHAwEwEgYDVR0TAQH/BAgwBgEB\n" \
// "/wIBADAdBgNVHQ4EFgQUALUp8i2ObzHom0yteD763OkM0dIwHwYDVR0jBBgwFoAU\n" \
// "ebRZ5nu25eQBc4AIiMgaWPbpm24wMgYIKwYBBQUHAQEEJjAkMCIGCCsGAQUFBzAC\n" \
// "hhZodHRwOi8veDEuaS5sZW5jci5vcmcvMBMGA1UdIAQMMAowCAYGZ4EMAQIBMCcG\n" \
// "A1UdHwQgMB4wHKAaoBiGFmh0dHA6Ly94MS5jLmxlbmNyLm9yZy8wDQYJKoZIhvcN\n" \
// "AQELBQADggIBAI910AnPanZIZTKS3rVEyIV29BWEjAK/duuz8eL5boSoVpHhkkv3\n" \
// "4eoAeEiPdZLj5EZ7G2ArIK+gzhTlRQ1q4FKGpPPaFBSpqV/xbUb5UlAXQOnkHn3m\n" \
// "FVj+qYv87/WeY+Bm4sN3Ox8BhyaU7UAQ3LeZ7N1X01xxQe4wIAAE3JVLUCiHmZL+\n" \
// "qoCUtgYIFPgcg350QMUIWgxPXNGEncT921ne7nluI02V8pLUmClqXOsCwULw+PVO\n" \
// "ZCB7qOMxxMBoCUeL2Ll4oMpOSr5pJCpLN3tRA2s6P1KLs9TSrVhOk+7LX28NMUlI\n" \
// "usQ/nxLJID0RhAeFtPjyOCOscQBA53+NRjSCak7P4A5jX7ppmkcJECL+S0i3kXVU\n" \
// "y5Me5BbrU8973jZNv/ax6+ZK6TM8jWmimL6of6OrX7ZU6E2WqazzsFrLG3o2kySb\n" \
// "zlhSgJ81Cl4tv3SbYiYXnJExKQvzf83DYotox3f0fwv7xln1A2ZLplCb0O+l/AK0\n" \
// "YE0DS2FPxSAHi0iwMfW2nNHJrXcY3LLHD77gRgje4Eveubi2xxa+Nmk/hmhLdIET\n" \
// "iVDFanoCrMVIpQ59XWHkzdFmoHXHBV7oibVjGSO7ULSQ7MJ1Nz51phuDJSgAIU7A\n" \
// "0zrLnOrAj/dfrlEWRhCvAgbuwLZX1A2sjNjXoPOHbsPiy+lO1KF8/XY7\n" \
// "-----END CERTIFICATE-----\n"


// webhook.site pem key
#define PICOHTTPS_CA_ROOT_CERT \
"-----BEGIN CERTIFICATE-----\n" \
"MIIFBjCCAu6gAwIBAgIRAMISMktwqbSRcdxA9+KFJjwwDQYJKoZIhvcNAQELBQAw\n" \
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n" \
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMjQwMzEzMDAwMDAw\n" \
"WhcNMjcwMzEyMjM1OTU5WjAzMQswCQYDVQQGEwJVUzEWMBQGA1UEChMNTGV0J3Mg\n" \
"RW5jcnlwdDEMMAoGA1UEAxMDUjEyMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB\n" \
"CgKCAQEA2pgodK2+lP474B7i5Ut1qywSf+2nAzJ+Npfs6DGPpRONC5kuHs0BUT1M\n" \
"5ShuCVUxqqUiXXL0LQfCTUA83wEjuXg39RplMjTmhnGdBO+ECFu9AhqZ66YBAJpz\n" \
"kG2Pogeg0JfT2kVhgTU9FPnEwF9q3AuWGrCf4yrqvSrWmMebcas7dA8827JgvlpL\n" \
"Thjp2ypzXIlhZZ7+7Tymy05v5J75AEaz/xlNKmOzjmbGGIVwx1Blbzt05UiDDwhY\n" \
"XS0jnV6j/ujbAKHS9OMZTfLuevYnnuXNnC2i8n+cF63vEzc50bTILEHWhsDp7CH4\n" \
"WRt/uTp8n1wBnWIEwii9Cq08yhDsGwIDAQABo4H4MIH1MA4GA1UdDwEB/wQEAwIB\n" \
"hjAdBgNVHSUEFjAUBggrBgEFBQcDAgYIKwYBBQUHAwEwEgYDVR0TAQH/BAgwBgEB\n" \
"/wIBADAdBgNVHQ4EFgQUALUp8i2ObzHom0yteD763OkM0dIwHwYDVR0jBBgwFoAU\n" \
"ebRZ5nu25eQBc4AIiMgaWPbpm24wMgYIKwYBBQUHAQEEJjAkMCIGCCsGAQUFBzAC\n" \
"hhZodHRwOi8veDEuaS5sZW5jci5vcmcvMBMGA1UdIAQMMAowCAYGZ4EMAQIBMCcG\n" \
"A1UdHwQgMB4wHKAaoBiGFmh0dHA6Ly94MS5jLmxlbmNyLm9yZy8wDQYJKoZIhvcN\n" \
"AQELBQADggIBAI910AnPanZIZTKS3rVEyIV29BWEjAK/duuz8eL5boSoVpHhkkv3\n" \
"4eoAeEiPdZLj5EZ7G2ArIK+gzhTlRQ1q4FKGpPPaFBSpqV/xbUb5UlAXQOnkHn3m\n" \
"FVj+qYv87/WeY+Bm4sN3Ox8BhyaU7UAQ3LeZ7N1X01xxQe4wIAAE3JVLUCiHmZL+\n" \
"qoCUtgYIFPgcg350QMUIWgxPXNGEncT921ne7nluI02V8pLUmClqXOsCwULw+PVO\n" \
"ZCB7qOMxxMBoCUeL2Ll4oMpOSr5pJCpLN3tRA2s6P1KLs9TSrVhOk+7LX28NMUlI\n" \
"usQ/nxLJID0RhAeFtPjyOCOscQBA53+NRjSCak7P4A5jX7ppmkcJECL+S0i3kXVU\n" \
"y5Me5BbrU8973jZNv/ax6+ZK6TM8jWmimL6of6OrX7ZU6E2WqazzsFrLG3o2kySb\n" \
"zlhSgJ81Cl4tv3SbYiYXnJExKQvzf83DYotox3f0fwv7xln1A2ZLplCb0O+l/AK0\n" \
"YE0DS2FPxSAHi0iwMfW2nNHJrXcY3LLHD77gRgje4Eveubi2xxa+Nmk/hmhLdIET\n" \
"iVDFanoCrMVIpQ59XWHkzdFmoHXHBV7oibVjGSO7ULSQ7MJ1Nz51phuDJSgAIU7A\n" \
"0zrLnOrAj/dfrlEWRhCvAgbuwLZX1A2sjNjXoPOHbsPiy+lO1KF8/XY7\n" \
"-----END CERTIFICATE-----\n"


//
//  or
//
//#define PICOHTTPS_CA_ROOT_CERT                                       \
//"-----BEGIN CERTIFICATE-----\n"                                      \
//"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\n" \
//"-----END CERTIFICATE-----\n"

// TCP + TLS connection establishment polling interval
//
//  Interval with which to poll for establishment of TCP + TLS connection
//
#define PICOHTTPS_ALTCP_CONNECT_POLL_INTERVAL       100             // ms

// TCP + TLS idle connection polling interval
//
//  Interval with which to poll application (i.e. call registered polling
//  callback function) when TCP + TLS connection is idle.
//
//  The callback function should be registered with altcp_poll(). The polling
//  interval is given in units of 'coarse grain timer shots'; one shot
//  corresponds to approximately 500 ms.
//
//  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
//
#define PICOHTTPS_ALTCP_IDLE_POLL_INTERVAL          2               // shots

// HTTP request
//
//  Plain-text HTTP request to send to server
//
#define PICOHTTPS_BODY \
    "{\n" \
    "  \"sender\": \"PICO W\",\n" \
    "  \"message\": \"UwU Stinky\"\n" \
    "}"







// HTTP response polling interval
//
//  Interval with which to poll for HTTP response from server.
//
#define PICOHTTPS_HTTP_RESPONSE_POLL_INTERVAL       100             // ms

// Mbed TLS debug levels
//
//  Seemingly not defined in Mbed TLS‽
//
//  https://github.com/Mbed-TLS/mbedtls/blob/62e79dc913325a18b46aaea554a2836a4e6fc94b/include/mbedtls/debug.h#L141
//
#define PICOHTTPS_MBEDTLS_DEBUG_LEVEL               3


/* Macros *********************************************************************/

// Array length
#define LEN(array) (sizeof array)/(sizeof array[0])



/* Data structures ************************************************************/

// lwIP errors
//
//  typedef here to make source of error code more explicit
//
typedef err_t lwip_err_t;

// Mbed TLS errors
//
//  typedef here to make source of error code more explicit
//
typedef int mbedtls_err_t;

// TCP connection callback argument
//
//  All callbacks associated with lwIP TCP (+ TLS) connections can be passed a
//  common argument. This is intended to allow application state to be accessed
//  from within the callback context. The argument should be registered with
//  altcp_arg().
//
//  The following structure is used for this argument in order to supply all
//  the relevant application state required by the various callbacks.
//
//  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
//
struct altcp_callback_arg{

    // TCP + TLS connection configurtaion
    //
    //  Memory allocated to the connection configuration structure needs to be
    //  freed (with altcp_tls_free_config) in the connection error callback
    //  (callback_altcp_err).
    //
    //  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
    //  https://www.nongnu.org/lwip/2_1_x/group__altcp__tls.html
    //
    struct altcp_tls_config* config;

    // TCP + TLS connection state
    //
    //  Successful establishment of a connection needs to be signaled to the
    //  application from the connection connect callback
    //  (callback_altcp_connect).
    //
    //  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
    //
    bool connected;

    // Data reception acknowledgement
    //
    //  The amount of data acknowledged as received by the server needs to be
    //  communicated to the application from the connection sent callback
    //  (callback_altcp_sent) for validatation of successful transmission.
    //
    u16_t acknowledged;

};



/* Functions ******************************************************************/

// Initialise standard I/O over USB
//
//  @return         `true` on success
//
bool init_stdio(void);

// Initialise Pico W wireless hardware
//
//  @return         `true` on success
//
bool init_cyw43(void);

// Connect to wireless network
//
//  @return         `true` on success
//
bool connect_to_network(void);

// Resolve hostname
//
//  @param ipaddr   Pointer to an `ip_addr_t` where the resolved IP address
//                  should be stored.
//
//  @return         `true` on success
//
bool resolve_hostname(ip_addr_t* ipaddr);

// Free TCP + TLS protocol control block
//
//  Memory allocated for a protocol control block (with altcp_tls_new) needs to
//  be freed (with altcp_close).
//
//  @param pcb      Pointer to a `altcp_pcb` structure to be freed
//
void altcp_free_pcb(struct altcp_pcb* pcb);

// Free TCP + TLS connection configuration
//
//  Memory allocated for TCP + TLS connection configuration (with
//  altcp_tls_create_config_client) needs to be freed (with
//  altcp_tls_free_config).
//
//  @param config   Pointer to a `altcp_tls_config` structure to be freed
//
void altcp_free_config(struct altcp_tls_config* config);

// Free TCP + TLS connection callback argument
//
//  The common argument passed to lwIP connection callbacks must remain in
//  scope for the duration of all callback contexts (i.e. connection lifetime).
//  As such, it cannot be declared with function scope when registering the
//  callback, but rather should be allocated on the heap. This implies the
//  allocated memory must be freed on connection close.
//
//  @param arg      Pointer to a `altcp_callback_arg` structure to be freed
//
void altcp_free_arg(struct altcp_callback_arg* arg);

// Establish TCP + TLS connection with server
//
//  @param ipaddr   Pointer to an `ip_addr_t` containing the server's IP
//                  address
//  @param pcb      Double pointer to a `altcp_pcb` structure where the
//                  protocol control block for the established connection
//                  should be stored.
//
//  @return         `true` on success
//
bool connect_to_host(ip_addr_t* ipaddr, struct altcp_pcb** pcb);

// Send HTTP request
//
//  @param pcb      Pointer to a `altcp_pcb` structure containing the TCP + TLS
//                  connection PCB to the server.
//
//  @return         `true` on success
//
bool send_request(struct altcp_pcb* pcb, const char *body);

// DNS response callback
//
//  Callback function fired on DNS query response.
//
//  Registered with dns_gethostbyname().
//
//  https://www.nongnu.org/lwip/2_1_x/group__dns.html
//
void callback_gethostbyname(
    const char* name,
    const ip_addr_t* resolved,
    void* ipaddr
);

// TCP + TLS connection error callback
//
//  Callback function fired on TCP + TLS connection fatal error.
//
//  Registered with altcp_err().
//
//  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
//
void callback_altcp_err(void* arg, lwip_err_t err);

// TCP + TLS connection idle callback
//
//  Callback function fired on idle TCP + TLS connection.
//
//  Registered with altcp_err().
//
//  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
//
lwip_err_t callback_altcp_poll(void* arg, struct altcp_pcb* pcb);

// TCP + TLS data acknowledgement callback
//
//  Callback function fired on acknowledgement of data reception by server over
//  a TCP + TLS connection.
//
//  Registered with altcp_sent().
//
//  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
//
lwip_err_t callback_altcp_sent(void* arg, struct altcp_pcb* pcb, u16_t len);

// TCP + TLS data reception callback
//
//  Callback function fired on reception of data from server over a TCP +
//  TLS connection.
//
//  Registered with altcp_recv().
//
//  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
//
lwip_err_t callback_altcp_recv(
    void* arg,
    struct altcp_pcb* pcb,
    struct pbuf* buf,
    lwip_err_t err
);

// TCP + TLS connection establishment callback
//
//  Callback function fired on successful establishment of TCP + TLS connection.
//
//  Registered with altcp_connect().
//
//  https://www.nongnu.org/lwip/2_1_x/group__altcp.html
//
lwip_err_t callback_altcp_connect(
    void* arg,
    struct altcp_pcb* pcb,
    lwip_err_t err
);



#endif //PICOHTTPS_H
