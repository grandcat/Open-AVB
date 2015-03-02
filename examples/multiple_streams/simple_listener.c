/*
Copyright (c) 2013 Katja Rohloff <Katja.Rohloff@uni-jena.de>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include <pcap/pcap.h>
#include <sndfile.h>

#include "avb.h"
#include "listener_mrp_client.h"

#define DEBUG 0
#define PCAP 1
#define LIBSND 1

#define VERSION_STR "0.1"

#define REQ_STREAM_DST_MAC0 "91:E0:F0:00:0e:80"
#define REQ_STREAM_DST_MAC1 "91:E0:F0:00:0e:81"

#define ETHERNET_HEADER_SIZE (18)
#define SEVENTEEN22_HEADER_PART1_SIZE (4)
#define SEVENTEEN22_HEADER_PART2_SIZE (10)
#define SIX1883_HEADER_SIZE (10)
#define HEADER_SIZE (ETHERNET_HEADER_SIZE		\
            + SEVENTEEN22_HEADER_PART1_SIZE \
            + STREAM_ID_SIZE		\
            + SEVENTEEN22_HEADER_PART2_SIZE \
            + SIX1883_HEADER_SIZE)
#define SAMPLES_PER_SECOND (48000)
#define SAMPLES_PER_FRAME (6)
#define CHANNELS (2)

typedef struct streamRxThread_s {
    pthread_t threadHandle;
    int threadID;
    pcap_t* pcap_handle;
    SNDFILE* snd_file_handle;
    streamDesc_t *streamInfo;
} streamRxThread_t;

struct q_eth_hdr{
    u_char dst[6];
    u_char src[6];
    u_char stuff[4];
    u_char type[2];
};

/* globals */
static const char *version_str = "simple_listener v" VERSION_STR "\n"
    "Copyright (c) 2012, Intel Corporation\n";

const unsigned char accepted_stream_ids[NUM_ACCEPTED_STREAMS][8] = {
    {0xa0, 0x36, 0x9f, 0x4c, 0x92, 0x55, 0x00, 0x00},
    {0xa0, 0x36, 0x9f, 0x4c, 0x92, 0x55, 0x00, 0x01}
};

char filter_exp[] = "ether dst " REQ_STREAM_DST_MAC0
                " || ether dst " REQ_STREAM_DST_MAC1; /* PCAP filter expression */

streamDesc_t *streams;
int num_streams = 0;

/* internals */
volatile int halt = 0;

streamRxThread_t rx_threads[NUM_ACCEPTED_STREAMS];
int num_threads = 0;

char *dev = NULL;
char* base_file_name = NULL;
pcap_t* glob_pcap_handle;
const u_int8_t glob_ether_type[] = {0x22, 0xF0};
SNDFILE* glob_snd_file;

static void help()
{
    fprintf(stderr, "\n"
        "Usage: listener [-h] -i interface -f file_name.wav"
        "\n"
        "Options:\n"
        "    -h  show this message\n"
        "    -i  specify interface for AVB connection\n"
        "    -f  set the name of the output wav-file\n"
        "\n" "%s" "\n", version_str);
    exit(EXIT_FAILURE);
}

void pcap_callback(u_char* args, const struct pcap_pkthdr* packet_header, const u_char* packet)
{
    unsigned char* test_stream_id;
    struct q_eth_hdr* eth_header;
    uint32_t *buf;
    uint32_t frame[2] = { 0 , 0 };
    int i;
    (void) args; /* unused */
    (void) packet_header; /* unused */

#if DEBUG
    fprintf(stdout,"Got packet.\n");
#endif /* DEBUG*/

    eth_header = (struct q_eth_hdr*)(packet);

#if DEBUG
    fprintf(stdout,"Ether Type: 0x%02x%02x\n", eth_header->type[0], eth_header->type[1]);
#endif /* DEBUG*/

    if (0 == memcmp(glob_ether_type, eth_header->type, sizeof(eth_header->type)))
    {
        test_stream_id = (unsigned char*)(packet + ETHERNET_HEADER_SIZE + SEVENTEEN22_HEADER_PART1_SIZE);

#if DEBUG
        fprint("Received stream id: %02x%02x%02x%02x%02x%02x%02x%02x\n ",
                 test_stream_id[0], test_stream_id[1],
                 test_stream_id[2], test_stream_id[3],
                 test_stream_id[4], test_stream_id[5],
                 test_stream_id[6], test_stream_id[7]);
#endif /* DEBUG*/

        if (0 == memcmp(test_stream_id, global_stream_id, sizeof(STREAM_ID_SIZE)))
        {

#if DEBUG
            fprintf(stdout,"Stream ids matched.\n");
#endif /* DEBUG*/
            buf = (uint32_t*) (packet + HEADER_SIZE);
            for(i = 0; i < SAMPLES_PER_FRAME * CHANNELS; i += 2)
            {
                memcpy(&frame[0], &buf[i], sizeof(frame));

                frame[0] = ntohl(frame[0]);   /* convert to host-byte order */
                frame[1] = ntohl(frame[1]);
                frame[0] &= 0x00ffffff;       /* ignore leading label */
                frame[1] &= 0x00ffffff;
                frame[0] <<= 8;               /* left-align remaining PCM-24 sample */
                frame[1] <<= 8;

                sf_writef_int(glob_snd_file, (const int *)frame, 1);
            }
        }
    }
}

static void
initialize_accepted_streams
(
    streamDesc_t **streams,
    int number_of_streams
)
{
    int i;

    assert(0 < number_of_streams && number_of_streams <= NUM_ACCEPTED_STREAMS);
    num_streams = number_of_streams;
    *streams = (streamDesc_t *)calloc(sizeof(streamDesc_t), number_of_streams);
    assert(0 != *streams);

    for (i = 0; i < number_of_streams; ++i)
    {
        streamDesc_t *stream = &((*streams)[i]);
        // Assign stream description
//        stream->streamInfo = (streamDesc_t *)calloc(sizeof(streamDesc_t), 1);
//        assert(0 != stream->streamInfo);
        memcpy((stream->stream_ID), accepted_stream_ids[i], STREAM_ID_SIZE);
        stream->received_packets = 0;
        stream->spawned = 0;
    }

    for (i = 0; i < number_of_streams; ++i)
    {
        streamDesc_t *curStream = &((*streams)[i]);
        printf("Stream %i: %x %x\n", i, curStream->stream_ID[5], curStream->stream_ID[7]);
    }
}

static void
deinitialize_streams
(
    streamDesc_t **streams
)
{
    int i;

    assert(0 != *streams);
    // Leave stream(s) if it was used
    printf("MRPD: leave streams.\n");

    for (i = 0; i < num_streams; ++i)
    {
        streamDesc_t *streamIter = &((*streams)[i]);
        if (0 != streamIter->spawned)
        {
            // send leave indication
            send_leave(streamIter->stream_ID);
        }
    }

    free(*streams);
}


void
pcap_parse_packet
(
    u_char *arg,
    const struct pcap_pkthdr *packet_header,
    const u_char *packet
)
{
    u_int8_t* found_streamID;
    struct q_eth_hdr* eth_header;

    (void) packet_header; /* unused */
    streamRxThread_t *thread_config = (streamRxThread_t *)arg;
    streamDesc_t *stream = thread_config->streamInfo;

#if DEBUG
    fprintf(stdout,"Got packet.\n");
#endif /* DEBUG*/

    eth_header = (struct q_eth_hdr*)(packet);

#if DEBUG
    fprintf(stdout,"Ether Type: 0x%02x%02x\n", eth_header->type[0], eth_header->type[1]);
#endif /* DEBUG*/

    if (0 == memcmp(glob_ether_type, eth_header->type, sizeof(eth_header->type)))
    {
        found_streamID = (u_int8_t *)(packet + ETHERNET_HEADER_SIZE + SEVENTEEN22_HEADER_PART1_SIZE);
        // Select only packets for owned streamID
        if (0 == memcmp(stream->stream_ID, found_streamID, STREAM_ID_SIZE))
        {
            stream->received_packets++;
            printf("T%i: got packet with my stream ID ", thread_config->threadID);
            print_stream(found_streamID);
        }
    }
}

static void *
rx_thread(void *arg) {
    struct bpf_program pcap_comp_filter_exp;
    char errbuf[PCAP_ERRBUF_SIZE];
    SF_INFO* sf_info;
    char filename[20];

    streamRxThread_t *thread_config = (streamRxThread_t *)arg;
    int res;

    printf("RX thread %i started with streamState %p \n",
           thread_config->threadID, thread_config->streamInfo);

    // Notify talker to start our stream
    res = send_ready(thread_config->streamInfo->stream_ID);
    if (res) {
        printf("RX thread %i: send_ready failed\n", thread_config->threadID);
        return NULL;
    }

    // Setting up blocking PCAP Loop for packet processing
    thread_config->pcap_handle = pcap_open_live(dev, BUFSIZ, 1, -1, errbuf);
    if (NULL == thread_config->pcap_handle)
    {
        fprintf(stderr, "Thread: could not open device %s: %s\n", dev, errbuf);
        return NULL;
    }
    // PCAP filter
    if (-1 == pcap_compile(thread_config->pcap_handle, &pcap_comp_filter_exp, filter_exp, 0, PCAP_NETMASK_UNKNOWN))
    {
        fprintf(stderr, "Could not parse filter %s: %s\n", filter_exp, pcap_geterr(thread_config->pcap_handle));
        return NULL;
    }
    if (-1 == pcap_setfilter(thread_config->pcap_handle, &pcap_comp_filter_exp))
    {
        fprintf(stderr, "Could not install filter %s: %s\n", filter_exp, pcap_geterr(thread_config->pcap_handle));
        return NULL;
    }

    // Create file for storing received packets
    sf_info = (SF_INFO *)calloc(sizeof(SF_INFO), 1);

    sf_info->samplerate = SAMPLES_PER_SECOND;
    sf_info->channels = CHANNELS;
    sf_info->format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
    assert(0 != sf_format_check(sf_info));

    sprintf(filename, "output_%i.wav", thread_config->threadID);
    if (NULL == (thread_config->snd_file_handle = sf_open(filename, SFM_WRITE, sf_info)))
    {
        fprintf(stderr, "RX thread %i: could not create file.", thread_config->threadID);
        return NULL;
    }
    printf("RX thread %i: created file called %s\n", thread_config->threadID, filename);

    // Start packet processing loop (blocking)
    pcap_loop(thread_config->pcap_handle, -1, pcap_parse_packet, (u_char *)thread_config);

    free(sf_info);
    printf("RX thread %i stopped.\n", thread_config->threadID);

    return NULL;
}

static void
manage_rx_threads()
{
    fprintf(stdout,"Waiting for talker(s)...\n");
    while (!halt)
    {
        streamDesc_t *matchedStream;
        int match = mrp_retrieve_stream(&matchedStream);

        if (0 == match) // Got a matching stream ID in accepted stream table
        {
            printf("Got managed stream with stream desc: %p! \n", matchedStream);
            // Spawn separate thread for this stream
            streamRxThread_t *thread_config = &rx_threads[num_threads];
            thread_config->threadID = num_threads;
            num_threads++;
            thread_config->streamInfo = matchedStream;

            pthread_create(&(thread_config->threadHandle), NULL, rx_thread, thread_config);
        }
    }
}

void sigint_handler(int signum)
{
    int ret, i;

    fprintf(stdout,"Received signal %d:leaving...\n", signum);
    halt = 1;

//    if (0 != talker) {
//        ret = send_leave();
//        if (ret)
//            printf("send_leave failed\n");
//    }

    if (2 > control_socket)
    {
        close(control_socket);
        ret = mrp_disconnect();
        if (ret)
            printf("mrp_disconnect failed\n");
    }

    // Stop packet processing loop for all threads and sync files
    for (i = 0; i < num_streams; ++i)
    {
        streamRxThread_t *rxThread = &(rx_threads[i]);
        if (NULL != rxThread->pcap_handle)
        {
            pcap_breakloop(rxThread->pcap_handle);
            pcap_close(rxThread->pcap_handle);

            sf_write_sync(rxThread->snd_file_handle);
            sf_close(rxThread->snd_file_handle);
        }
    }

#if LIBSND
//    sf_write_sync(glob_snd_file);
//    sf_close(glob_snd_file);
#endif /* LIBSND */

    // Leave and cleanup streams
    deinitialize_streams(&streams);
}

int main(int argc, char *argv[])
{
    int rc;

    signal(SIGINT, sigint_handler);

    int c;
    while((c = getopt(argc, argv, "hi:f:")) > 0)
    {
        switch (c)
        {
        case 'h':
            help();
            break;
        case 'i':
            dev = strdup(optarg);
            break;
        case 'f':
            base_file_name = strdup(optarg);
            break;
        default:
                fprintf(stderr, "Unrecognized option!\n");
        }
    }

    if ((NULL == dev) || (NULL == base_file_name))
        help();


    initialize_accepted_streams(&streams, NUM_ACCEPTED_STREAMS);
    // return errno;

    if (create_socket())
    {
        fprintf(stderr, "Socket creation failed.\n");
        return errno;
    }

    rc = report_domain_status();
    if (rc) {
        printf("report_domain_status failed\n");
        return EXIT_FAILURE;
    }

    rc = join_vlan();
    if (rc) {
        printf("join_vlan failed\n");
        return EXIT_FAILURE;
    }

    // Find matching streams and spawn a thread for each unique stream ID
    manage_rx_threads();

//#if PCAP
//    /* session, get session handler */
//    /* take promiscuous vs. non-promiscuous sniffing? (0 or 1) */
//    glob_pcap_handle = pcap_open_live(dev, BUFSIZ, 1, -1, errbuf);
//    if (NULL == glob_pcap_handle)
//    {
//        fprintf(stderr, "Could not open device %s: %s\n", dev, errbuf);
//        return EXIT_FAILURE;
//    }

//#if DEBUG
//    fprintf(stdout,"Got session pcap handler.\n");
//#endif /* DEBUG */
//    /* compile and apply filter */
//    if (-1 == pcap_compile(glob_pcap_handle, &comp_filter_exp, filter_exp, 0, PCAP_NETMASK_UNKNOWN))
//    {
//        fprintf(stderr, "Could not parse filter %s: %s\n", filter_exp, pcap_geterr(glob_pcap_handle));
//        return EXIT_FAILURE;
//    }

//    if (-1 == pcap_setfilter(glob_pcap_handle, &comp_filter_exp))
//    {
//        fprintf(stderr, "Could not install filter %s: %s\n", filter_exp, pcap_geterr(glob_pcap_handle));
//        return EXIT_FAILURE;
//    }

//#if DEBUG
//    fprintf(stdout,"Compiled and applied filter.\n");
//#endif /* DEBUG */

//    /** loop forever and call callback-function for every received packet */
//    pcap_loop(glob_pcap_handle, -1, pcap_callback, NULL);
//#endif /* PCAP */

    return EXIT_SUCCESS;
}
