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

#pragma once

#include <ab/ab_common.h>
#include <ab/request.h>
#include <util/debug.h>
#include <util/liveobj.h>
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


//uint64_t plc_get_new_seq_id_unsafe(ab_plc_p sess);
uint64_t plc_get_new_seq_id(ab_plc_p sess);

extern int plc_find_or_create(ab_plc_p *plc, attr attribs);
ab_connection_p plc_find_connection_by_path_unsafe(ab_plc_p plc,const char *path);
//extern int plc_add_connection_unsafe(ab_plc_p plc, ab_connection_p connection);
extern int plc_add_connection(ab_plc_p plc, ab_connection_p connection);
extern int plc_remove_connection_unsafe(ab_plc_p plc, ab_connection_p connection);
extern int plc_remove_connection(ab_plc_p plc, ab_connection_p connection);
//extern int plc_add_request_unsafe(ab_plc_p sess, ab_request_p req);
extern int plc_add_request(ab_plc_p sess, ab_request_p req);
//extern int plc_remove_request_unsafe(ab_plc_p sess, ab_request_p req);
extern int plc_remove_request(ab_plc_p sess, ab_request_p req);

extern uint32_t plc_get_new_connection_id(ab_plc_p plc);
//extern int plc_status_unsafe(ab_plc_p plc);

/* send/receive packets. */
//extern int recv_eip_response_unsafe(ab_plc_p plc);
//extern int send_eip_request_unsafe(ab_request_p req);

extern int plc_status(ab_plc_p plc);


extern int plc_setup();
extern void plc_teardown();

/* 
 * FIXME - we use this too much.  Break down into
 * finer grained mutex use.
 */
extern mutex_p global_plc_mut;

