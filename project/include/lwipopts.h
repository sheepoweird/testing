#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Common settings used in most of the pico_w examples
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html for details)

// allow override in some examples
#ifndef NO_SYS
#define NO_SYS                      1
#endif
// allow override in some examples
#ifndef LWIP_SOCKET
#define LWIP_SOCKET                 0
#endif
#if PICO_CYW43_ARCH_POLL
#define MEM_LIBC_MALLOC             1
#else
// MEM_LIBC_MALLOC is incompatible with non polling versions
#define MEM_LIBC_MALLOC             0
#endif
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              32
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1

// Enable DNS
#define LWIP_DNS                    1
#define DNS_TABLE_SIZE              4
#define DNS_MAX_SERVERS             2

#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define TCP_WND                     (16 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (16 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETCONN                0
#define MEM_STATS                   1
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define IP_FORWARD                  0
#define IP_OPTIONS_ALLOWED          0
#define IP_REASSEMBLY               0
#define IP_FRAG                     0
#define ARP_QUEUEING                0
#define TCP_KEEPALIVE               1
#define TCP_MAXRTX                  12
#define TCP_SYNMAXRTX               6

#define LWIP_DISABLE_TCP_SANITY_CHECKS 1

#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#endif

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0

#define LWIP_DHCP                   1
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define DHCP_DOES_ARP_CHECK         0

// HTTPS/TLS Support - ENABLED
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1

// Set authentication mode
#define ALTCP_MBEDTLS_AUTHMODE      MBEDTLS_SSL_VERIFY_REQUIRED

#endif /* __LWIPOPTS_H__ */