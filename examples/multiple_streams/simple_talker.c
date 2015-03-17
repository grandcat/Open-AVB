/******************************************************************************

  Copyright (c) 2012, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

   3. Neither the name of the Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

******************************************************************************/

/******************************************************************************/
/***   Includes                                                             ***/
/******************************************************************************/
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/if.h>   // MAC address handling

#include "avb.h"
#include "igb.h"
#include "talker_mrp_client.h"

/******************************************************************************/
/***   Declarations                                                         ***/
/******************************************************************************/
#define VERSION_STR "0.1"

#define SRC_CHANNELS (2)
#define GAIN (0.5)
#define L16_PAYLOAD_TYPE (96) /* for layer 4 transport - should be negotiated via RTSP */
#define ID_B_HDR_EXT_ID (0) /* for layer 4 transport - should be negotiated via RTSP */
#define L2_SAMPLES_PER_FRAME (6)
#define L4_SAMPLES_PER_FRAME (60)
#define L4_SAMPLE_SIZE (2)
#define CHANNELS (2)
#define RTP_SUBNS_SCALE_NUM (20000000)
#define RTP_SUBNS_SCALE_DEN (4656613)
#define XMIT_DELAY (200000000) /* us */
#define RENDER_DELAY (XMIT_DELAY+2000000)	/* us */
#define L2_PACKET_IPG (125000) /* (1) packet every 125 usec */
#define L4_PACKET_IPG (1250000)	/* (1) packet every 1.25 millisec */
#define L4_PORT ((uint16_t)5004)
#define PKT_SZ (100)

#define TX_QUEUE (0)                        //< tx-queue-0/1 for Class A/B traffic

/******************************************************************************/
/***   Type definitions                                                     ***/
/******************************************************************************/
typedef struct streamDesc_s {
    uint8_t stream_ID[8];
    uint8_t l2_dest_addr[6];

} streamDesc_t;

typedef struct __attribute__ ((packed)) {
    uint8_t version_length;
    uint8_t DSCP_ECN;
    uint16_t ip_length;
    uint16_t id;
    uint16_t fragmentation;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t hdr_cksum;
    uint32_t src;
    uint32_t dest;

    uint16_t source_port;
    uint16_t dest_port;
    uint16_t udp_length;
    uint16_t cksum;

    uint8_t version_cc;
    uint8_t mark_payload;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;

    uint8_t tag[2];
    uint16_t total_length;
    uint8_t tag_length;
    uint8_t seconds[3];
    uint32_t nanoseconds;
} IP_RTP_Header;

typedef struct __attribute__ ((packed)) {
    uint32_t source;
    uint32_t dest;
    uint8_t zero;
    uint8_t protocol;
    uint16_t length;
} IP_PseudoHeader;

/******************************************************************************/
/***   Variables                                                            ***/
/******************************************************************************/
static const char *version_str = "simple_talker v" VERSION_STR "\n";

uint num_streams;
streamDesc_t* streamState;

uint8_t glob_station_addr[] = { 0, 0, 0, 0, 0, 0 };       //< overwritten by get_mac_address()
unsigned char glob_stream_id[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
unsigned char glob_stream_id2[] = { 0, 0, 0, 0, 0, 0, 0, 1 };
/* IEEE 1722 reserved address */
const unsigned char glob_l2_dest_addr[] = { 0x91, 0xE0, 0xF0, 0x00, 0x0e, 0x80 };
unsigned char glob_l3_dest_addr[] = { 224, 0, 0, 115 };

/******************************************************************************/
/***   Implementation                                                       ***/
/******************************************************************************/
uint16_t inet_checksum
(
    uint8_t *ip,
    int len
)
{
    uint32_t sum = 0;  /* assume 32 bit long, 16 bit short */

    while(len > 1){
        sum += *(( uint16_t *) ip); ip += 2;
        if(sum & 0x80000000)   /* if high order bit set, fold */
            sum = (sum & 0xFFFF) + (sum >> 16);
        len -= 2;
    }

    if(len)       /* take care of left over byte */
        sum += (uint16_t) *(uint8_t *)ip;

    while(sum>>16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

uint16_t inet_checksum_sg
(
    struct iovec *buf_iov,
    size_t buf_iovlen
)
{
    size_t i;
    uint32_t sum = 0;  /* assume 32 bit long, 16 bit short */
    uint8_t residual;
    int has_residual = 0;

    for( i = 0; i < buf_iovlen; ++i,++buf_iov ) {
        if( has_residual ) {
            if( buf_iov->iov_len > 0 ) {
                if(sum & 0x80000000)   /* if high order bit set, fold */
                    sum = (sum & 0xFFFF) + (sum >> 16);
                sum += residual | (*(( uint8_t *) buf_iov->iov_base) << 8);
                buf_iov->iov_base += 1;
                buf_iov->iov_len -= 1;
            } else {
                if(sum & 0x80000000)   /* if high order bit set, fold */
                    sum = (sum & 0xFFFF) + (sum >> 16);
                sum += (uint16_t) residual;
            }
            has_residual = 0;

        }
        while(buf_iov->iov_len > 1){
            if(sum & 0x80000000)   /* if high order bit set, fold */
                sum = (sum & 0xFFFF) + (sum >> 16);
            sum += *(( uint16_t *) buf_iov->iov_base); buf_iov->iov_base += 2;
            buf_iov->iov_len -= 2;
        }
        if( buf_iov->iov_len ) {
            residual = *(( uint8_t *) buf_iov->iov_base);
            has_residual = 1;
        }
    }
    if( has_residual ) {
        sum += (uint16_t) residual;
    }

    while(sum>>16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

static inline
uint64_t ST_rdtsc(void)
{
    uint64_t ret;
    unsigned c, d;
    asm volatile ("rdtsc":"=a" (c), "=d"(d));
    ret = d;
    ret <<= 32;
    ret |= c;
    return ret;
}

void gensine32
(
    int32_t * buf,
    unsigned count
)
{
    long double interval = (2 * ((long double)M_PI)) / count;
    unsigned i;
    for (i = 0; i < count; ++i) {
        buf[i] =
            (int32_t) (MAX_SAMPLE_VALUE * sinl(i * interval) * GAIN);
    }
}

int get_samples
(
    unsigned count,
    int32_t * buffer
)
{
    static int init = 0;
    static int32_t samples_onechannel[100];
    static unsigned index = 0;

    if (init == 0) {
        gensine32(samples_onechannel, 100);
        init = 1;
    }

    while (count > 0) {
        int i;
        for (i = 0; i < SRC_CHANNELS; ++i) {
            *(buffer++) = samples_onechannel[index];
        }
        index = (index + 1) % 100;
        --count;
    }

    return 0;
}

void sigint_handler
(
    int signum
)
{
    printf("got SIGINT\n");
    halt_tx = signum;
}

void l3_to_l2_multicast(
    unsigned char *l2,
    unsigned char *l3_dest_addr 
)
{
     l2[0]  = 0x1;
     l2[1]  = 0x0;
     l2[2]  = 0x5e;
     l2[3]  = l3_dest_addr[1] & 0x7F;
     l2[4]  = l3_dest_addr[2];
     l2[5]  = l3_dest_addr[3];
}

int get_mac_address
(
    char *interface
)
{
    struct ifreq if_request;
    int lsock;
    int rc;

    lsock = socket(PF_PACKET, SOCK_RAW, htons(0x800));
    if (lsock < 0)
        return -1;

    memset(&if_request, 0, sizeof(if_request));
    strncpy(if_request.ifr_name, interface, sizeof(if_request.ifr_name) - 1);
    rc = ioctl(lsock, SIOCGIFHWADDR, &if_request);
    if (rc < 0) {
        close(lsock);
        return -1;
    }

    memcpy(glob_station_addr, if_request.ifr_hwaddr.sa_data,
           sizeof(glob_station_addr));
    close(lsock);
    return 0;
}

void
avb_hdr1722_set_streamID
(
    seventeen22_header *h1722,
    unsigned char baseStreamID[]
)
{
    memcpy(&(h1722->stream_id), baseStreamID, ETH_ALEN);
}

static void
init_default_streams
(
    streamDesc_t **streams,
    int number_of_streams
)
{
    int i;

    assert(0 < number_of_streams && number_of_streams < 256);
    *streams = (streamDesc_t *)calloc(sizeof(streamDesc_t), number_of_streams);
    assert(0 != *streams);

    for (i = 0; i < number_of_streams; ++i)
    {
        streamDesc_t *curStream = &((*streams)[i]);

        // Stream ID: source MAC || identifier
        memcpy(&(curStream->stream_ID), glob_station_addr, ETH_ALEN);
        curStream->stream_ID[7] = i;
        // Destination MAC: base multicast address + last byte identifer
        memcpy(&(curStream->l2_dest_addr), glob_l2_dest_addr, ETH_ALEN);
        curStream->l2_dest_addr[5] += i;
    }

    for (i = 0; i < number_of_streams; ++i)
    {
        streamDesc_t *curStream = &((*streams)[i]);
        printf("Stream %i: %x %x\n", i, curStream->stream_ID[4], curStream->stream_ID[7]);
        printf("Dest %i: %x %x\n", i, curStream->l2_dest_addr[4], curStream->l2_dest_addr[5]);
    }
}

static int
advertise_streams
(
    streamDesc_t* streams,
    bool unadvertise
)
{
    unsigned int i;
    int rc;

    fprintf(stderr, (!unadvertise) ? "MRPD: advertising streams:\n" :
                                     "MRPD: unadvertising streams:\n");
    for (i = 0; i < num_streams; ++i)
    {
        streamDesc_t* curStream = &(streams[i]);

        if (!unadvertise)
        {
            rc = mrp_advertise_stream((curStream->stream_ID), (curStream->l2_dest_addr),
                                      domain_class_a_vid, PKT_SZ - 16,
                                      L2_PACKET_IPG / 125000 * num_streams,
                                      MSRP_SR_CLASS_A_PRIO, 3900);
        }
        else
        {
            rc = mrp_unadvertise_stream((curStream->stream_ID), (curStream->l2_dest_addr),
                                   domain_class_a_vid, PKT_SZ - 16,
                                   L2_PACKET_IPG / 125000 * num_streams,
                                   MSRP_SR_CLASS_A_PRIO, 3900);
        }

        if (rc) {
            printf("mrp_advertise_stream failed for stream %i\n", i);
            return EXIT_FAILURE;
        }

        printf("  Stream ID: %x %x %x\n",
               curStream->stream_ID[5], curStream->stream_ID[6], curStream->stream_ID[7]);

        // MRPD has problems with too fast advertisements
        //sleep(1);
    }

    return 0;
}

static void
fini_streams
(
    streamDesc_t **streams
)
{
    assert(0 != *streams);
    free(*streams);
}

static void
avb_prepare_packet_structure
(
    void *tx_packet,
    uint8_t l2_dest_addr[],
    uint8_t l2_src_addr[]
)
{
    seventeen22_header *hdr1722;
    six1883_header *hdr61883;

    // Put together all required headers for an AVB 1722 packet
    //
    // Consists of the following headers
    // * Ethernet (MAC) header
    // * 802.1Q tag header
    // * 1722 AVB header (includes presentation time)
    // * Audio sample data specific 61883 header

    hdr1722 = (seventeen22_header *)((char *)tx_packet + sizeof(eth_header) + 4);
    hdr61883 = (six1883_header *) (hdr1722 + 1);

    // Set minimal MAC header
    //avb_eth_header_set_mac(default_tx_packet, dest_addr, (int8_t *)interface);
    avb_eth_header_set_dest_mac(tx_packet, l2_dest_addr);
    memcpy(tx_packet + ETH_ALEN, l2_src_addr, ETH_ALEN);

    // AVB Q-Tag header (TODO: remove global access to these variables)
    ((char *)tx_packet)[12] = 0x81;
    ((char *)tx_packet)[13] = 0x00;
    ((char *)tx_packet)[14] =
            ((domain_class_a_priority << 13 | domain_class_a_vid)) >> 8;
    ((char *)tx_packet)[15] =
            ((domain_class_a_priority << 13 | domain_class_a_vid)) & 0xFF;
    ((char *)tx_packet)[16] = 0x22;	/* 1722 eth type */
    ((char *)tx_packet)[17] = 0xF0;

    // Set default 1722 header
    avb_initialize_h1722_to_defaults(hdr1722);
    avb_set_1722_sid_valid(hdr1722, 0x1);
    avb_hdr1722_set_streamID(hdr1722, (unsigned char *)l2_src_addr); // TODO: check whether dynamic change is needed here
    avb_set_1722_length(hdr1722, htons(32));

    // Set default 61883 header (for audio)
    avb_initialize_61883_to_defaults(hdr61883);
    avb_set_61883_format_tag(hdr61883, 0x1);
    avb_set_61883_packet_channel(hdr61883, 0x1F);
    avb_set_61883_packet_tcode(hdr61883, 0xA);
    avb_set_61883_source_id(hdr61883 , 0x3F);
    avb_set_61883_data_block_size(hdr61883, 0x1);
    avb_set_61883_eoh(hdr61883, 0x2);
    avb_set_61883_format_id(hdr61883, 0x10);
    avb_set_61883_format_dependent_field(hdr61883, 0x02);
    avb_set_61883_syt(hdr61883, 0xFFFF);
}

static void*
thread_tx_packets
(
    void *arg
) 
{
    (void) arg; /* unused */

    return NULL;
}

static void usage(void)
{
    fprintf(stderr, "\n"
        "usage: simple_talker [-h] -i interface-name"
        "\n"
        "options:\n"
        "    -h  show this message\n"
        "    -i  specify interface for AVB connection\n"
        "    -n  number of parallel streams\n"
        "\n" "%s" "\n", version_str);
    exit(EXIT_FAILURE);
}

int main
(
    int argc,
    char *argv[]
)
{
    unsigned i;
    int err;
    device_t igb_dev;
    int igb_shm_fd = -1;
    char *igb_mmap = NULL;
    struct igb_dma_alloc a_page;
    struct igb_dma_alloc b_page;
    struct igb_packet a_packet;
    struct igb_packet *tmp_packet;
    struct igb_packet *cleaned_packets;
    struct igb_packet *free_packets;
    uint current_listener_id;
    int c;
    u_int64_t last_time;
    int rc = 0;
    char *interface = NULL;
    int transport = 2;

    num_streams = -1;

    uint16_t seqnum;
    // uint32_t rtp_timestamp;
    uint64_t time_stamp;
    unsigned total_samples = 0;
    gPtpTimeData td;
    int32_t sample_buffer[L4_SAMPLES_PER_FRAME * SRC_CHANNELS];

    // Construction of default packet
    void *default_tx_packet;
    int frame_size;

    seventeen22_header *l2_header0;
    six1883_header *l2_header1;
    six1883_sample *sample;

    // Old RTP audio
    IP_RTP_Header *l4_headers;
    // IP_PseudoHeader pseudo_hdr;
    unsigned l4_local_address = 0;
    int sd;
    struct sockaddr_in local;
    struct ifreq if_request;

    uint64_t now_local, now_8021as;
    uint64_t update_8021as;
    unsigned delta_8021as, delta_local;
    uint8_t dest_addr[6];
    size_t packet_size;

    for (;;) {
        c = getopt(argc, argv, "hi:n:");
        if (c < 0)
            break;
        switch (c) {
        case 'h':
            usage();
            break;
        case 'i':
            if (interface) {
                printf
                    ("only one interface per daemon is supported\n");
                usage();
            }
            interface = strdup(optarg);
            break;
        case 'n':
            num_streams = strtoul(optarg, NULL, 10);
        }
    }
    if (optind < argc)
        usage();
    if (NULL == interface) {
        usage();
    }
    if(num_streams < 1 || num_streams > 256) {
        fprintf( stderr, "Must specify valid number of streams (range 1 - 256).\n" );
        usage();
    }
    rc = mrp_connect();
    if (rc) {
        printf("socket creation failed\n");
        return errno;
    }
    err = pci_connect(&igb_dev);
    if (err) {
        printf("connect failed (%s) - are you running as root?\n",
               strerror(errno));
        return errno;
    }
    err = igb_init(&igb_dev);
    if (err) {
        printf("init failed (%s) - is the driver really loaded?\n",
               strerror(errno));
        return errno;
    }
    err = igb_dma_malloc_page(&igb_dev, &a_page);
    err += igb_dma_malloc_page(&igb_dev, &b_page);
    // printf("DMA page A:%" PRIu64 ", B:%" PRIu64 "\n", a_page.dma_paddr, b_page.dma_paddr);
    if (err) {
        printf("malloc failed (%s) - out of memory?\n",
               strerror(errno));
        return errno;
    }
    signal(SIGINT, sigint_handler);
    // Set global station address
    rc = get_mac_address(interface);
    if (rc) {
        printf("failed to open interface\n");
        usage();
    }
    if(transport == 2) {
        memcpy(dest_addr, glob_l2_dest_addr, sizeof(dest_addr));

    } else {
        // Transport 3: RTP
        memset( &local, 0, sizeof( local ));
        local.sin_family = PF_INET;
        local.sin_addr.s_addr = htonl( INADDR_ANY );
        local.sin_port = htons(L4_PORT);
        l3_to_l2_multicast(dest_addr, glob_l3_dest_addr);
        memset( &if_request, 0, sizeof( if_request ));
        strncpy(if_request.ifr_name, interface, sizeof(if_request.ifr_name)-1);
        sd = socket( AF_INET, SOCK_DGRAM, 0 );
        if(sd == -1) {
            printf( "Failed to open socket: %s\n", strerror( errno ));
            return errno;
        }
        if(bind( sd, (struct sockaddr *) &local, sizeof( local )) != 0) {
            printf( "Failed to bind on socket: %s\n", strerror( errno ));
            return errno;
        }
        if(ioctl( sd, SIOCGIFADDR, &if_request ) != 0) {
            printf
                ( "Failed to get interface address (ioctl) on socket: %s\n",
                  strerror( errno ));
            return errno;
        }
        memcpy(&l4_local_address,
               &(( struct sockaddr_in *)&if_request.ifr_addr)->sin_addr,
               sizeof(l4_local_address));

    }

    rc = mrp_monitor();
    if (rc) {
        printf("failed creating MRP monitor thread\n");
        return EXIT_FAILURE;
    }

    /*
     * should use mrp_get_domain() but this is a simplification
     */
    domain_a_valid = 1;
    domain_class_a_id = MSRP_SR_CLASS_A;
    domain_class_a_priority = MSRP_SR_CLASS_A_PRIO;
    domain_class_a_vid = 2;
    printf("Register domain Class A PRIO=%d VID=%04x...\n", domain_class_a_priority,
           domain_class_a_vid);

    rc = mrp_register_domain(&domain_class_a_id, &domain_class_a_priority, &domain_class_a_vid);
    if (rc) {
        printf("mrp_register_domain for class A failed\n");
        return EXIT_FAILURE;
    }

    // Care only about class A traffic right now
//    domain_class_b_id = MSRP_SR_CLASS_B;
//    domain_class_b_priority = MSRP_SR_CLASS_B_PRIO;
//    domain_class_b_vid = 3;
//    rc = mrp_register_domain(&domain_class_b_id, &domain_class_b_priority, &domain_class_b_vid);
//    if (rc) {
//        printf("mrp_register_domain for class B failed\n");
//        return EXIT_FAILURE;
//    }

    // MRP: listern to messages of vlan 2 (Class A)
    rc = mrp_join_vlan();
    if (rc) {
        printf("mrp_join_vlan failed\n");
        return EXIT_FAILURE;
    }

    // Reservations on driver level for parallel class A and class B streams.
    // We actually use only class A at the moment.
    rc = igb_set_class_bandwidth
            (&igb_dev, 125000/L2_PACKET_IPG * num_streams, 0 * 250000/L2_PACKET_IPG,
             PKT_SZ - 22, 0);
    if (rc) {
        printf("igb_set_class_bandwidth failed\n");
        return EXIT_FAILURE;
    }

    init_default_streams(&streamState, num_streams);

    // Note: glob_stream_id set to iface MAC address (base address)
    memset(glob_stream_id, 0, sizeof(glob_stream_id));  // stream id: 0
    memcpy(glob_stream_id, glob_station_addr, sizeof(glob_station_addr));

    if( transport == 2 ) {
        packet_size = PKT_SZ;
    } else {
        packet_size = 18 + sizeof(*l4_headers) +
                (L4_SAMPLES_PER_FRAME * CHANNELS * L4_SAMPLE_SIZE);
    }

    // Prepare basic structure for all packets
    //
    // default_tx_packet is used as a reference for all packets enqueued for
    // transmission.
    frame_size = sizeof(eth_header) + 4
               + sizeof(seventeen22_header) + sizeof(six1883_header);

    default_tx_packet = avb_create_packet(4); // sizeof(Q-Tag header) = 4
    avb_prepare_packet_structure(default_tx_packet, dest_addr, glob_station_addr);

    // Prepare DMA mapping
    a_packet.dmatime = a_packet.attime = a_packet.flags = 0;
    a_packet.map.paddr = a_page.dma_paddr;
    a_packet.map.mmap_size = a_page.mmap_size;
    a_packet.offset = 0;
    a_packet.vaddr = a_page.dma_vaddr + a_packet.offset;
    a_packet.len = packet_size;
    a_packet.next = NULL;
    free_packets = NULL;
    seqnum = 0;
    // rtp_timestamp = 0; /* Should be random start */

    /* divide the dma page into buffers for packets */
    for (i = 1; i < ((a_page.mmap_size) / packet_size); i++) {
        tmp_packet = malloc(sizeof(struct igb_packet));
        if (NULL == tmp_packet) {
            printf("failed to allocate igb_packet memory!\n");
            return errno;
        }
        *tmp_packet = a_packet;
        tmp_packet->offset = (i * packet_size);
        tmp_packet->vaddr += tmp_packet->offset;
        tmp_packet->next = free_packets;

        // Apply default structure for each packet
        memcpy(((char *)tmp_packet->vaddr), default_tx_packet, frame_size);
        tmp_packet->len =
                sizeof(eth_header) + 4 + sizeof(seventeen22_header) + sizeof(six1883_header) +
                (L2_SAMPLES_PER_FRAME * CHANNELS * sizeof(six1883_sample));

        free_packets = tmp_packet;
    }
    free(default_tx_packet);

    /*
     * subtract 16 bytes for the MAC header/Q-tag - pktsz is limited to the
     * data payload of the ethernet frame.
     *
     * IPG is scaled to the Class (A) observation interval of packets per 125 usec.
     */
    rc = advertise_streams(streamState, 0);
    if (0 != rc)
    {
        printf("Advertising streams failed.\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "awaiting a listener ...\n");
    rc = mrp_await_listener(glob_stream_id);
    if (rc) {
        printf("mrp_await_listener failed\n");
        return EXIT_FAILURE;
    }
    listeners = 1;
    printf("got a listener ...\n");
    halt_tx = 0;

    if(-1 == gptpinit(&igb_shm_fd, &igb_mmap)) {
        fprintf(stderr, "GPTP init failed.\n");
        return EXIT_FAILURE;
    }

    if (-1 == gptpscaling(igb_mmap, &td)) {
        fprintf(stderr, "GPTP scaling failed.\n");
        return EXIT_FAILURE;
    }

    if(igb_get_wallclock( &igb_dev, &now_local, NULL ) > 0) {
        fprintf( stderr, "Failed to get wallclock time\n" );
        return EXIT_FAILURE;
    }
    update_8021as = td.local_time - td.ml_phoffset;
    delta_local = (unsigned)(now_local - td.local_time);
    delta_8021as = (unsigned)(td.ml_freqoffset * delta_local);
    now_8021as = update_8021as + delta_8021as;

    // Accounting for delay until listener accepts stream
    last_time = now_local + XMIT_DELAY;
    time_stamp = now_8021as + RENDER_DELAY;

    rc = nice(-20);

    // Prepare packets for the subscribed listeners and submit them

    current_listener_id = -1;
    while (listeners && !halt_tx) {
        char *raw_tx_packet;
        streamDesc_t *curStream;

        tmp_packet = free_packets;
        // Busy waiting
        // by jumping between here and cleanup if no free packet is available
        if (NULL == tmp_packet)
            goto cleanup;
        free_packets = tmp_packet->next;

        uint32_t timestamp_l;
        get_samples(L2_SAMPLES_PER_FRAME, sample_buffer);
        raw_tx_packet = (char *)tmp_packet->vaddr;
        l2_header0 = (seventeen22_header *) (raw_tx_packet + 18);
        l2_header1 = (six1883_header *) (l2_header0 + 1);

        // Adapt package for current listener (if multiple)
        current_listener_id = (current_listener_id + 1) % num_streams;
        curStream = &(streamState[current_listener_id]);

        avb_eth_header_set_dest_mac((eth_header *)raw_tx_packet, curStream->l2_dest_addr);
        memcpy(&(l2_header0->stream_id), curStream->stream_ID, STREAM_ID_SIZE);

        /* unfortuntely unless this thread is at rtprio
         * you get pre-empted between fetching the time
         * and programming the packet and get a late packet
         */
        // Local transmission time
        tmp_packet->attime = last_time
                + (u_int64_t)(L2_PACKET_IPG / num_streams) * (u_int64_t)current_listener_id;
        // Presentation time in the network (established by 802.1AS daemon)
        timestamp_l = time_stamp
                + (u_int64_t)(L2_PACKET_IPG / num_streams) * (u_int64_t)current_listener_id;
        l2_header0->timestamp = htonl(timestamp_l);

        // Shift by one observation interval if one packet of each stream is
        // transmitted in this round
        if (current_listener_id == (num_streams - 1))
        {
            last_time += L2_PACKET_IPG;
            time_stamp += L2_PACKET_IPG;
        }

        l2_header0->seq_number = seqnum++;
        if (seqnum % 4 == 0)
            l2_header0->timestamp_valid = 0;
        else
            l2_header0->timestamp_valid = 1;

        l2_header1->data_block_continuity = total_samples;
        total_samples += L2_SAMPLES_PER_FRAME*CHANNELS;
        sample =
                (six1883_sample *) (((char *)tmp_packet->vaddr) +
                                    (18 + sizeof(seventeen22_header) +
                                     sizeof(six1883_header)));

        for (i = 0; i < L2_SAMPLES_PER_FRAME * CHANNELS; ++i) {
            uint32_t tmp = htonl(sample_buffer[i]);
            sample[i].label = 0x40;
            memcpy(&(sample[i].value), &(tmp),
                   sizeof(sample[i].value));
        }

        err = igb_xmit(&igb_dev, TX_QUEUE, tmp_packet);

        if (!err) {
            continue;
        }

        // This part only met in rare cases
        if (ENOSPC == err) {

            /* put back for now */
            tmp_packet->next = free_packets;
            free_packets = tmp_packet;
        }

    cleanup:
        // Busy waiting if no free packets are available
        //
        // * igb_clean returns list of already transmitted packets. These are the
        //   free ones which can be used. Busy waiting results from the lack of
        //   available slots.
        // * These free items are associated with our free_packets list.
        // * Free packets are refilled and staged for transmission via xmit().

        igb_clean(&igb_dev, &cleaned_packets);
        // Inverse direction
        i = 0;
        while (cleaned_packets) {
            i++;
            tmp_packet = cleaned_packets;
            cleaned_packets = cleaned_packets->next;
            tmp_packet->next = free_packets;
            free_packets = tmp_packet;
        }
    }
    rc = nice(0);

    if (halt_tx == 0)
        printf("listener left ...\n");
    halt_tx = 1;

    rc = advertise_streams(streamState, 1);
    if (rc)
        printf("mrp_unadvertise_stream failed\n");

    igb_set_class_bandwidth(&igb_dev, 0, 0, 0, 0);	/* disable Qav */

    fini_streams(&streamState);

    rc = mrp_disconnect();
    if (rc)
        printf("mrp_disconnect failed\n");

    igb_dma_free_page(&igb_dev, &a_page);
    igb_dma_free_page(&igb_dev, &b_page);
    rc = gptpdeinit(&igb_shm_fd, &igb_mmap);
    err = igb_detach(&igb_dev);

    pthread_exit(NULL);

    return EXIT_SUCCESS;
}
