/*
  Copyright (c) 2013 Katja Rohloff <Katja.Rohloff@uni-jena.de>
  Copyright (c) 2014, Parrot SA

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

#include "listener_mrp_client.h"

#include "parse.h"

/* global variables */

int control_socket;
volatile int talker = 0;
unsigned char global_stream_id[8];

/*
 * private
 */

int send_msg(char *data, int data_len)
{
	struct sockaddr_in addr;

	if (control_socket == -1)
		return -1;
	if (data == NULL)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(MRPD_PORT_DEFAULT);
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	inet_aton("127.0.0.1", &addr.sin_addr);
	return sendto(control_socket, data, data_len, 0,
		(struct sockaddr*)&addr, (socklen_t)sizeof(addr));
}

/**
 * \brief Compares stream ID of incoming MSRP messages to accepted stream
 *        table and extracts further details about the advertised stream if it
 *        is a required stream.
 *
 * \param[in]  buf      Input message buffer of MRP daemon
 * \param[in]  buflen   Length of input message
 * \param[out] matched_stream   Matching stream descriptor if suitable stream
 *                              was found
 * \return success if 0
 */
int msg_match_streams
(
    char *buf,
    int buflen,
    streamDesc_t **matched_stream
)
{
    int parseErr, i;
    int res = -1;

    struct parse_param parse_specs[] = {
        {"S" PARSE_ASSIGN, parse_c64, global_stream_id},
//        {"L" PARSE_ASSIGN, parse_u32, &talker_ad->AccumulatedLatency},
        {0, parse_null, 0}
    };

    fprintf(stderr, "Rcvd msg: %s\n", buf);
    if (strncmp(buf, "SNE T:", 6) == 0 || strncmp(buf, "SJO T:", 6) == 0)
    {
        parse(buf, buflen, parse_specs, &parseErr);
        printf("Extracted stream ID (err: %i): ", parseErr);
        print_stream(global_stream_id);

        // Match stream IDs against set of prefefined IDs for multiple
        // stream support
        if (parseErr)
            return -1;

        for (i = 0; i < num_streams; i++)
        {
            streamDesc_t *streamIter = &(streams[i]);
            if (0 == memcmp(streamIter->stream_ID, global_stream_id, 8)
                    && 0 == streamIter->spawned)
            {
                streamIter->spawned = 1;    // will be spawned right after
                if (0 != matched_stream)
                    *matched_stream = streamIter;
                res = 0;
                // Debug
                printf("MRPD accepted stream: ");
                print_stream(global_stream_id);
            }
        }
        if (!parseErr && res)
        {
            // Keep this variable for legacy reasons like jackd talker
            talker = 1;
        }
    }
    return res;
}

/*
 * public
 */

int
mrp_retrieve_stream
(
    streamDesc_t **matched_stream
)
{
	char *databuf;
	int bytes = 0;
	int ret;

	databuf = (char *)malloc(1500);
	if (NULL == databuf)
		return -1;

	memset(databuf, 0, 1500);
	bytes = recv(control_socket, databuf, 1500, 0);
	if (bytes <= -1)
	{
		free(databuf);
		return -1;
	}
    ret = msg_match_streams(databuf, bytes, matched_stream);
	free(databuf);

	return ret;
}

int create_socket() // TODO FIX! =:-|
{
	struct sockaddr_in addr;
	control_socket = socket(AF_INET, SOCK_DGRAM, 0);
		
	/** in POSIX fd 0,1,2 are reserved */
	if (2 > control_socket)
	{
		if (-1 > control_socket)
			close(control_socket);
	return -1;
	}
	
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(0);
	
	if(0 > (bind(control_socket, (struct sockaddr*)&addr, sizeof(addr)))) 
	{
		fprintf(stderr, "Could not bind socket.\n");
		close(control_socket);
		return -1;
	}
	return 0;
}

int report_domain_status()
{
	char* msgbuf;
	int rc;

	msgbuf = malloc(1500);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "S+D:C=6,P=3,V=0002");
	rc = send_msg(msgbuf, 1500);
	free(msgbuf);

	if (rc != 1500)
		return -1;
	else
		return 0;
}

int join_vlan()
{
	char *msgbuf;
	int rc;

	msgbuf = malloc(1500);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "V++:I=0002");
	rc = send_msg(msgbuf, 1500);
	free(msgbuf);

	if (rc != 1500)
		return -1;
	else
		return 0;
}

int await_talker()
{
    int match = -1;
    while (0 != match)
        match = mrp_retrieve_stream(NULL);
    return 0;
}

int send_ready
(
    u_int8_t streamID[]
)
{
	char *databuf;
	int rc;

	databuf = malloc(1500);
	if (NULL == databuf)
		return -1;
	memset(databuf, 0, 1500);
    sprintf(databuf, "S+L:L=%02x%02x%02x%02x%02x%02x%02x%02x, D=2",
            streamID[0], streamID[1],
            streamID[2], streamID[3],
            streamID[4], streamID[5],
            streamID[6], streamID[7]);
	rc = send_msg(databuf, 1500);
	free(databuf);

	if (rc != 1500)
		return -1;
	else
		return 0;
}

int send_leave()
{
	char *databuf;
	int rc;

	databuf = malloc(1500);
	if (NULL == databuf)
		return -1;
	memset(databuf, 0, 1500);
	sprintf(databuf, "S-L:L=%02x%02x%02x%02x%02x%02x%02x%02x, D=3",
             global_stream_id[0], global_stream_id[1],
             global_stream_id[2], global_stream_id[3],
             global_stream_id[4], global_stream_id[5],
             global_stream_id[6], global_stream_id[7]);
	rc = send_msg(databuf, 1500);
	free(databuf);

	if (rc != 1500)
		return -1;
	else
		return 0;
}

int mrp_disconnect()
{
	int rc;
	char *msgbuf = malloc(1500);

	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);

	sprintf(msgbuf, "BYE");
	rc = send_msg(msgbuf, 1500);
	free(msgbuf);

	if (rc != 1500)
		return -1;
	else
		return 0;
}

inline
void print_stream
(
    uint8_t stream_ID[]
)
{
    printf("%02x %02x %02x %02x %02x %02x %02x %02x\n",
           stream_ID[0], stream_ID[1], stream_ID[2], stream_ID[3],
            stream_ID[4], stream_ID[5], stream_ID[6], stream_ID[7]);
}
