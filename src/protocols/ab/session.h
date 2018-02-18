/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2015-09-12  KRH - Created file.                                        *
 *                                                                        *
 **************************************************************************/

#ifndef __PLCTAG_AB_SESSION_H__
#define __PLCTAG_AB_SESSION_H__ 1

#include <ab/common.h>
#include <ab/request.h>
#include <util/refcount.h>

#define MAX_SESSION_HOST    (128)

/* the following are in microseconds*/
#define SESSION_DEFAULT_PACKET_INTERVAL (5000)
#define SESSION_MAX_PACKET_INTERVAL (100000)
#define SESSION_MIN_PACKET_INTERVAL (3000)
#define SESSION_PACKET_LOSS_INTERVAL_INC (5000)
#define SESSION_PACKET_RECEIVE_INTERVAL_DEC (1000)


/* resend interval in milliseconds*/
#define SESSION_DEFAULT_RESEND_INTERVAL_MS (50)
#define SESSION_MIN_RESEND_INTERVAL  (10)

#define SESSION_NUM_ROUND_TRIP_SAMPLES (5)

/* how long to wait for session registration before timing out. In milliseconds. */
#define SESSION_REGISTRATION_TIMEOUT (1500)

/*
 * the queue depth depends on the type of the request.
 */

#define SESSION_MAX_CONNECTED_REQUESTS_IN_FLIGHT (2)
#define SESSION_MAX_UNCONNECTED_REQUESTS_IN_FLIGHT (8)

struct ab_session_t {
    ab_session_p next;
    ab_session_p prev;

    /* gateway connection related info */
    char host[MAX_SESSION_HOST];
    int port;
    sock_p sock;
    int is_connected;
    int status;

    /* registration info */
    uint32_t session_handle;
    int registered;

    /* Sequence ID for requests. */
    uint64_t session_seq_id;

    /* current request being sent, only one at a time */
    ab_request_p current_request;

    /* list of outstanding requests for this session */
    ab_request_p requests;

    /* counter for number of messages in flight */
    int num_reqs_in_flight;
    //int64_t next_packet_time_us;
    //int64_t next_packet_interval_us;

    int64_t retry_interval;

    /* short cumulative period for calculating round trip time. */
    int64_t round_trip_samples[SESSION_NUM_ROUND_TRIP_SAMPLES];
    int round_trip_sample_index;

    /* serialization control */
    //~ int serial_request_in_flight;
    //~ uint64_t serial_seq_in_flight;

    /* data for receiving messages */
    uint64_t resp_seq_id;
    int has_response;
    uint32_t recv_offset;
    uint8_t recv_data[MAX_REQ_RESP_SIZE];

    /*int recv_size;*/

    /* tags for this session */
    ab_tag_p tags;

    /* ref count for session */
    //refcount rc;

    /* connections for this session */
    ab_connection_p connections;
    uint32_t conn_serial_number; /* id for the next connection */
};

uint64_t session_get_new_seq_id_unsafe(ab_session_p sess);
uint64_t session_get_new_seq_id(ab_session_p sess);

extern int session_find_or_create(ab_session_p *session, attr attribs);
ab_connection_p session_find_connection_by_path_unsafe(ab_session_p session,const char *path);
extern int session_add_connection_unsafe(ab_session_p session, ab_connection_p connection);
extern int session_add_connection(ab_session_p session, ab_connection_p connection);
extern int session_remove_connection_unsafe(ab_session_p session, ab_connection_p connection);
extern int session_remove_connection(ab_session_p session, ab_connection_p connection);
extern int session_add_request_unsafe(ab_session_p sess, ab_request_p req);
extern int session_add_request(ab_session_p sess, ab_request_p req);
extern int session_remove_request_unsafe(ab_session_p sess, ab_request_p req);
extern int session_remove_request(ab_session_p sess, ab_request_p req);
//extern int session_acquire(ab_session_p session);
//extern int session_release(ab_session_p session);


#endif
