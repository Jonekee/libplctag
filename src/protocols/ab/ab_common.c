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
 * 2013-11-19  KRH - Created file.                                        *
 **************************************************************************/


#include <platform.h>
#include <lib/libplctag.h>
#include <lib/libplctag_tag.h>
#include <ab/ab.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <ab/cip.h>
#include <ab/eip.h>
#include <ab/eip_cip.h>
#include <ab/eip_pccc.h>
#include <ab/eip_dhp_pccc.h>
#include <ab/session.h>
#include <ab/connection.h>
#include <ab/tag.h>
#include <ab/request.h>
#include <util/attr.h>
#include <util/debug.h>


/*
 * Externally visible global variables
 */

volatile ab_session_p sessions = NULL;
volatile mutex_p global_session_mut = NULL;


/* request/response handling thread */
volatile thread_p io_handler_thread = NULL;

volatile int library_terminating = 0;



/*
 * Generic Rockwell/Allen-Bradley protocol functions.
 *
 * These are the primary entry points into the AB protocol
 * stack.
 */




/* vtables for different kinds of tags */
struct tag_vtable_t default_vtable = {0}/*= { NULL, ab_tag_destroy, NULL, NULL }*/;
struct tag_vtable_t cip_vtable = {0}/*= { ab_tag_abort, ab_tag_destroy, eip_cip_tag_read_start, eip_cip_tag_status, eip_cip_tag_write_start }*/;
struct tag_vtable_t plc_vtable = {0}/*= { ab_tag_abort, ab_tag_destroy, eip_pccc_tag_read_start, eip_pccc_tag_status, eip_pccc_tag_write_start }*/;
struct tag_vtable_t plc_dhp_vtable = {0}/*= { ab_tag_abort, ab_tag_destroy, eip_dhp_pccc_tag_read_start, eip_dhp_pccc_tag_status, eip_dhp_pccc_tag_write_start}*/;


/* forward declarations*/
int session_check_incoming_data_unsafe(ab_session_p session);
int request_check_outgoing_data_unsafe(ab_session_p session, ab_request_p req);
tag_vtable_p set_tag_vtable(ab_tag_p tag);
//int setup_session_mutex(void);
static int check_read_group(ab_tag_p tag, const char *read_group);
static int read_group_remove_tag(ab_tag_p tag);

/* declare this so that the library initializer can pass it to atexit() */

/*
 * Public functions.
 */


int ab_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Initializing AB protocol library.");

    /* set up the vtables. */
    default_vtable.destroy  = (tag_destroy_func)ab_tag_destroy;

    plc_dhp_vtable.abort    = (tag_abort_func)ab_tag_abort;
    plc_dhp_vtable.destroy  = (tag_destroy_func)ab_tag_destroy;
    plc_dhp_vtable.read     = (tag_read_func)eip_dhp_pccc_tag_read_start;
    plc_dhp_vtable.status   = (tag_status_func)eip_dhp_pccc_tag_status;
    plc_dhp_vtable.write    = (tag_write_func)eip_dhp_pccc_tag_write_start;

    plc_vtable.abort        = (tag_abort_func)ab_tag_abort;
    plc_vtable.destroy      = (tag_destroy_func)ab_tag_destroy;
    plc_vtable.read         = (tag_read_func)eip_pccc_tag_read_start;
    plc_vtable.status       = (tag_status_func)eip_pccc_tag_status;
    plc_vtable.write        = (tag_write_func)eip_pccc_tag_write_start;

    cip_vtable.abort        = (tag_abort_func)ab_tag_abort;
    cip_vtable.destroy      = (tag_destroy_func)ab_tag_destroy;
    cip_vtable.read         = (tag_read_func)eip_cip_tag_read_start;
    cip_vtable.status       = (tag_status_func)eip_cip_tag_status;
    cip_vtable.write        = (tag_write_func)eip_cip_tag_write_start;

    /* this is a mutex used to synchronize most activities in this protocol */
    rc = mutex_create((mutex_p*)&global_session_mut);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create global tag mutex!");
        return rc;
    }

    /* create the background IO handler thread */
    rc = thread_create((thread_p*)&io_handler_thread,request_handler_func, 32*1024, NULL);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Unable to create request handler thread!");
        return rc;
    }


    pdebug(DEBUG_INFO,"Finished initializing AB protocol library.");

    return rc;
}

/*
 * called when the whole program is going to terminate.
 */
void ab_teardown(void)
{
    pdebug(DEBUG_INFO,"Releasing global AB protocol resources.");

    pdebug(DEBUG_INFO,"Terminating IO thread.");
    /* kill the IO thread first. */
    library_terminating = 1;

    /* wait for the thread to die */
    thread_join(io_handler_thread);
    thread_destroy((thread_p*)&io_handler_thread);

    pdebug(DEBUG_INFO,"Freeing global session mutex.");
    /* clean up the mutex */
    mutex_destroy((mutex_p*)&global_session_mut);

    pdebug(DEBUG_INFO,"Done.");
}



plc_tag_p ab_tag_create(attr attribs)
{
    ab_tag_p tag = AB_TAG_NULL;
    const char *path;

    pdebug(DEBUG_INFO,"Starting.");

    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (ab_tag_p)mem_alloc(sizeof(struct ab_tag_t));

    if(!tag) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for AB EIP tag!");
        return PLC_TAG_P_NULL;
    }

    /*
     * we got far enough to allocate memory, set the default vtable up
     * in case we need to abort later.
     */

    tag->vtable = &default_vtable;

    /*
     * check the CPU type.
     *
     * This determines the protocol type.
     */

    if(check_cpu(tag, attribs) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"CPU type not valid or missing.");
        tag->status = PLCTAG_ERR_BAD_DEVICE;
        return (plc_tag_p)tag;
    }

    /* AB PLCs are little endian. */
    tag->endian = PLCTAG_DATA_LITTLE_ENDIAN;

    /* allocate memory for the data */
    tag->elem_count = attr_get_int(attribs,"elem_count",1);
    tag->elem_size = attr_get_int(attribs,"elem_size",0);
    tag->size = (tag->elem_count) * (tag->elem_size);

    if(tag->size == 0) {
        /* failure! Need data_size! */
        pdebug(DEBUG_WARN,"Tag size is zero!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    tag->data = (uint8_t*)mem_alloc(tag->size);

    if(tag->data == NULL) {
        pdebug(DEBUG_WARN,"Unable to allocate tag data!");
        tag->status = PLCTAG_ERR_NO_MEM;
        return (plc_tag_p)tag;
    }

    /* get the connection path, punt if there is not one and we have a Logix-class PLC. */
    path = attr_get_str(attribs,"path",NULL);

    if(path == NULL && tag->protocol_type == AB_PROTOCOL_LGX) {
        pdebug(DEBUG_WARN,"Unable to find or determine base wire protocol type!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    tag->first_read = 1;

    /*
     * Since there is no explicit init function for the library (perhaps an error),
     * we need to check to see if library initialization has been done.
     */

    //if(!io_handler_thread) {
    //    pdebug(DEBUG_INFO,"entering critical block %p",global_session_mut);
    //    critical_block(global_session_mut) {
    //        /* check again because the state could have changed */
    //       if(!io_handler_thread) {
    //            rc = thread_create((thread_p*)&io_handler_thread,request_handler_func, 32*1024, NULL);
    //
    //            if(rc != PLCTAG_STATUS_OK) {
    //                pdebug(DEBUG_INFO,"Unable to create request handler thread!");
    //                tag->status = rc;
    //                break;
    //            }
    //        }
    //    }
    //    pdebug(DEBUG_INFO,"leaving critical block %p",global_session_mut);
    //
    //    if(tag->status != PLCTAG_STATUS_OK) {
    //        return (plc_tag_p)tag;
    //    }
    //}

    /* some kinds of tag need a connection and we know right away */
    if(tag->protocol_type == AB_PROTOCOL_MLGX800) {
        /* this type of tag must use connected mode. */
        tag->needs_connection = 1;
    }


    /* start parsing the parts of the tag. */

    /*
     * parse the link path into the tag.  Note that it must
     * pad the byte string to a multiple of 16-bit words. The function
     * also adds the protocol/PLC specific routing information to the
     * links specified.  This fills in fields in the connection about
     * any DH+ special data.
     *
     * Skip this if we don't have a path.
     */
    if(cip_encode_path(tag,path) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Unable to convert path links strings to binary path!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /*
     * handle the strange LGX->DH+->PLC5 case.
     *
     * This is separate from the check above of the PLC type.  The reason is
     * that we do not know whether we need a connection or not until we parse
     * the path element.  If we are doing a DH+ routing via a Logix chassis,
     * then we need to be in connected mode.  Even if the PLC that we want
     * to talk to is one that supports non-connected mode.
     */
    if(tag->use_dhp_direct) {
        /* this is a bit of a cheat.   The logic should be fixed up to combine with the check above.*/
        tag->needs_connection = 1;
    }

    /*
     * set up tag vtable.  This is protocol specific
     */
    tag->vtable = set_tag_vtable(tag);

    if(!tag->vtable) {
        pdebug(DEBUG_INFO,"Unable to set tag vtable!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /*
     * Find or create a session.
     *
     * All tags need sessions.  They are the TCP connection to the gateway PLC.
     */
    if(session_find_or_create(&tag->session, attribs) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Unable to create session!");
        tag->status = PLCTAG_ERR_BAD_GATEWAY;
        return (plc_tag_p)tag;
    }

    if(tag->needs_connection) {
        /* Find or create a connection.*/
        if((tag->status = connection_find_or_create(tag, attribs)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_INFO,"Unable to create connection! Status=%d",tag->status);
            return (plc_tag_p)tag;
        }

        /* set up the links between the tag and the connection. */
        //connection_add_tag(tag->connection, tag);
    } /*
    else {
        session_add_tag(tag->session, tag);
    }
    */

    /*
     * check the tag name, this is protocol specific.
     */

    if(check_tag_name(tag, attr_get_str(attribs,"name",NULL)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Bad tag name!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /*
     * check for a read group.
     */

    if(tag->protocol_type == AB_PROTOCOL_LGX && check_read_group(tag, attr_get_str(attribs,"read_group",NULL)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Unable to find or create read group!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    pdebug(DEBUG_INFO,"Done.");

    return (plc_tag_p)tag;
}





/*
 * set_tag_vtable
 *
 * Use various bits of information about the tag to determine
 * just what flavor of the protocol we will be using for this
 * tag.
 */
tag_vtable_p set_tag_vtable(ab_tag_p tag)
{
    switch(tag->protocol_type) {
        case AB_PROTOCOL_PLC:
            if(tag->use_dhp_direct) {
                //~ if(!plc_dhp_vtable.abort) {
                    //~ plc_dhp_vtable.abort     = (tag_abort_func)ab_tag_abort;
                    //~ plc_dhp_vtable.destroy   = (tag_destroy_func)ab_tag_destroy;
                    //~ plc_dhp_vtable.read      = (tag_read_func)eip_dhp_pccc_tag_read_start;
                    //~ plc_dhp_vtable.status    = (tag_status_func)eip_dhp_pccc_tag_status;
                    //~ plc_dhp_vtable.write     = (tag_write_func)eip_dhp_pccc_tag_write_start;
                //~ }

                return &plc_dhp_vtable;
            } else {
                //~ if(!plc_vtable.abort) {
                    //~ plc_vtable.abort     = (tag_abort_func)ab_tag_abort;
                    //~ plc_vtable.destroy   = (tag_destroy_func)ab_tag_destroy;
                    //~ plc_vtable.read      = (tag_read_func)eip_pccc_tag_read_start;
                    //~ plc_vtable.status    = (tag_status_func)eip_pccc_tag_status;
                    //~ plc_vtable.write     = (tag_write_func)eip_pccc_tag_write_start;
                //~ }

                return &plc_vtable;
            }

            break;

        case AB_PROTOCOL_MLGX:
            //~ if(!plc_vtable.abort) {
                //~ plc_vtable.abort     = (tag_abort_func)ab_tag_abort;
                //~ plc_vtable.destroy   = (tag_destroy_func)ab_tag_destroy;
                //~ plc_vtable.read      = (tag_read_func)eip_pccc_tag_read_start;
                //~ plc_vtable.status    = (tag_status_func)eip_pccc_tag_status;
                //~ plc_vtable.write     = (tag_write_func)eip_pccc_tag_write_start;
            //~ }

            return &plc_vtable;

            break;

        case AB_PROTOCOL_MLGX800:
        case AB_PROTOCOL_LGX:
            //~ if(!cip_vtable.abort) {
                //~ cip_vtable.abort     = (tag_abort_func)ab_tag_abort;
                //~ cip_vtable.destroy   = (tag_destroy_func)ab_tag_destroy;
                //~ cip_vtable.read      = (tag_read_func)eip_cip_tag_read_start;
                //~ cip_vtable.status    = (tag_status_func)eip_cip_tag_status;
                //~ cip_vtable.write     = (tag_write_func)eip_cip_tag_write_start;
            //~ }

            return &cip_vtable;

            break;

        default:
            return NULL;
            break;
    }

    return NULL;
}


/*
 * ab_tag_abort
 *
 * This does the work of stopping any inflight requests.
 * This is not thread-safe.  It must be called from a function
 * that locks the tag's mutex or only from a single thread.
 */

int ab_tag_abort(ab_tag_p tag)
{
    int i;

    for (i = 0; i < tag->max_requests; i++) {
        if (tag->reqs && tag->reqs[i]) {
            /* if any activity is still happening, signal the IO thread to kill the request */
            tag->reqs[i]->abort_request = 1;

            /* release our hold on the request */
            request_release(tag->reqs[i]);

            /* we are not holding on to this anymore */
            tag->reqs[i] = NULL;
        }
    }

    tag->read_in_progress = 0;
    tag->write_in_progress = 0;

    return PLCTAG_STATUS_OK;
}

/*
 * ab_tag_destroy
 *
 * This blocks on the global library mutex.  This should
 * be fixed to allow for more parallelism.  For now, safety is
 * the primary concern.
 */

int ab_tag_destroy(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* FIXME - this is going to serialize everything. */
    //critical_block(global_library_mutex) {
        ab_connection_p connection = NULL;
        ab_session_p session = NULL;

        /* already destroyed? */
        if (!tag) {
            pdebug(DEBUG_WARN,"Tag pointer is null!");
            rc = PLCTAG_ERR_NULL_PTR;
            //~ break;
            return rc;
        }

        connection = tag->connection;
        session = tag->session;

        /*
         * stop any current actions. Note that we
         * want to use the thread-safe version here.  We
         * do lock a mutex later, but a different one.
         */
        /* remove - this is already called by the main library code. */
        plc_tag_abort_mapped((plc_tag_p)tag);

        /* remove from any read groups */
        read_group_remove_tag(tag);

        /* tags are stored in different locations depending on the type. */
        if(connection) {
            pdebug(DEBUG_DETAIL, "Removing tag from connection.");
            connection_release(connection);
            tag->connection = NULL;
            //connection_remove_tag(connection, tag);
        }

        if(session) {
            pdebug(DEBUG_DETAIL, "Removing tag from session.");
            session_release(session);
            tag->session = NULL;
            //session_remove_tag(session, tag);
        }

        if (tag->reqs) {
            mem_free(tag->reqs);
            tag->reqs = NULL;
        }

        if (tag->read_req_sizes) {
            mem_free(tag->read_req_sizes);
            tag->read_req_sizes = NULL;
        }

        if (tag->write_req_sizes) {
            mem_free(tag->write_req_sizes);
            tag->write_req_sizes = NULL;
        }

        if (tag->data) {
            mem_free(tag->data);
            tag->data = NULL;
        }

        /* release memory */
        mem_free(tag);

        pdebug(DEBUG_INFO,"Finished releasing all tag resources.");
    //}

    pdebug(DEBUG_INFO, "done");

    return rc;
}



int check_cpu(ab_tag_p tag, attr attribs)
{
    const char* cpu_type = attr_get_str(attribs, "cpu", "NONE");

    if (!str_cmp_i(cpu_type, "plc") || !str_cmp_i(cpu_type, "plc5") || !str_cmp_i(cpu_type, "slc") ||
        !str_cmp_i(cpu_type, "slc500")) {
        tag->protocol_type = AB_PROTOCOL_PLC;
    } else if (!str_cmp_i(cpu_type, "micrologix800") || !str_cmp_i(cpu_type, "mlgx800") || !str_cmp_i(cpu_type, "micro800")) {
        tag->protocol_type = AB_PROTOCOL_MLGX800;
    } else if (!str_cmp_i(cpu_type, "micrologix") || !str_cmp_i(cpu_type, "mlgx")) {
        tag->protocol_type = AB_PROTOCOL_MLGX;
    } else if (!str_cmp_i(cpu_type, "compactlogix") || !str_cmp_i(cpu_type, "clgx") || !str_cmp_i(cpu_type, "lgx") ||
               !str_cmp_i(cpu_type, "controllogix") || !str_cmp_i(cpu_type, "contrologix") ||
               !str_cmp_i(cpu_type, "flexlogix") || !str_cmp_i(cpu_type, "flgx")) {
        tag->protocol_type = AB_PROTOCOL_LGX;
    } else {
        pdebug(DEBUG_WARN, "Unsupported device type: %s", cpu_type);

        return PLCTAG_ERR_BAD_DEVICE;
    }

    return PLCTAG_STATUS_OK;
}

int check_tag_name(ab_tag_p tag, const char* name)
{
    if (!name) {
        pdebug(DEBUG_WARN,"No tag name parameter found!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* attempt to parse the tag name */
    switch (tag->protocol_type) {
        case AB_PROTOCOL_PLC:
        case AB_PROTOCOL_MLGX:
            if (!pccc_encode_tag_name(tag->encoded_name, &(tag->encoded_name_size), name, MAX_TAG_NAME)) {
                pdebug(DEBUG_WARN, "parse of PCCC-style tag name %s failed!", name);

                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case AB_PROTOCOL_MLGX800:
        case AB_PROTOCOL_LGX:
            if (!cip_encode_tag_name(tag, name)) {
                pdebug(DEBUG_WARN, "parse of CIP-style tag name %s failed!", name);

                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        default:
            /* how would we get here? */
            pdebug(DEBUG_WARN, "unsupported protocol %d", tag->protocol_type);

            return PLCTAG_ERR_BAD_PARAM;

            break;
    }

    return PLCTAG_STATUS_OK;
}




/*
 * setup_session_mutex
 *
 * check to see if the global mutex is set up.  If not, do an atomic
 * lock and set it up.
 */
int setup_session_mutex(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    critical_block(global_library_mutex) {
        /* first see if the mutex is there. */
        if (!global_session_mut) {
            rc = mutex_create((mutex_p*)&global_session_mut);

            if (rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_ERROR, "Unable to create global tag mutex!");
            }
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


static int match_request_and_response(ab_request_p request, eip_cip_co_resp *response)
{
    int connected_response = (response->encap_command == le2h16(AB_EIP_CONNECTED_SEND) ? 1 : 0);

    /*
     * AB decided not to use the 64-bit sender context in connected messages.  No idea
     * why they did this, but it means that we need to look at the connection details
     * instead.
     */
    if(connected_response && request->conn_id == le2h32(response->cpf_orig_conn_id) && request->conn_seq == le2h16(response->cpf_conn_seq_num)) {
        /* if it is a connected packet, match the connection ID and sequence num. */
        return 1;
    } else if(!connected_response && response->encap_sender_context != (uint64_t)0 && response->encap_sender_context == request->session_seq_id) {
        /* if it is not connected, match the sender context, note that this is sent in host order. */
        return 1;
    }

    /* no match */
    return 0;
}



static void update_resend_samples(ab_session_p session, int64_t round_trip_time)
{
    int index;
    int64_t round_trip_sum = 0;
    int64_t round_trip_avg = 0;

    /* store the round trip information */
    session->round_trip_sample_index = ((++ session->round_trip_sample_index) >= SESSION_NUM_ROUND_TRIP_SAMPLES ? 0 : session->round_trip_sample_index);
    session->round_trip_samples[session->round_trip_sample_index] = round_trip_time;

    /* calculate the retry interval */
    for(index=0; index < SESSION_NUM_ROUND_TRIP_SAMPLES; index++) {
        round_trip_sum += session->round_trip_samples[index];
    }

    /* round up and double */
    round_trip_avg = (round_trip_sum + (SESSION_NUM_ROUND_TRIP_SAMPLES/2))/SESSION_NUM_ROUND_TRIP_SAMPLES;
    session->retry_interval = 3 * (round_trip_avg < SESSION_MIN_RESEND_INTERVAL ? SESSION_MIN_RESEND_INTERVAL : round_trip_avg);

    pdebug(DEBUG_INFO,"Packet round trip time %lld, running average round trip time is %lld",round_trip_time, session->retry_interval);
}



static void receive_response(ab_session_p session, ab_request_p request)
{
    /*
     * We received a packet.  Modify the packet interval downword slightly
     * to get to the maximum value.  We want to get to the point where we lose
     * a packet once in a while.
     */

    /*session->next_packet_interval_us -= SESSION_PACKET_RECEIVE_INTERVAL_DEC;
    if(session->next_packet_interval_us < SESSION_MIN_PACKET_INTERVAL) {
        session->next_packet_interval_us = SESSION_MIN_PACKET_INTERVAL;
    }
    pdebug(DEBUG_INFO,"Packet received, so decreasing packet interval to %lldus", session->next_packet_interval_us);
    */

    pdebug(DEBUG_INFO,"Packet sent initially %dms ago and was sent %d times",(int)(time_ms() - request->time_sent), request->send_count);

    update_resend_samples(session, time_ms() - request->time_sent);

    /* set the packet ready for processing. */
    pdebug(DEBUG_INFO, "got full packet of size %d", session->recv_offset);
    pdebug_dump_bytes(DEBUG_INFO, session->recv_data, session->recv_offset);

    /* copy the data from the session's buffer */
    mem_copy(request->data, session->recv_data, session->recv_offset);
    request->request_size = session->recv_offset;

    request->resp_received = 1;
    request->send_in_progress = 0;
    request->send_request = 0;
    request->recv_in_progress = 0;

    /* mark the connection as available if we need to */
    //~ clear_connection_for_request(request);
    //~ clear_session_for_request(request);
}


//~ static void handle_resend(ab_session_p session, ab_request_p request)
//~ {
    //~ eip_encap_t *encap = (eip_encap_t*)(&request->data[0]);


    //~ //if (encap->encap_command == le2h16(AB_EIP_CONNECTED_SEND)) {
        //~ pdebug(DEBUG_INFO,"Requeuing connected request connection %d sequence ID %d.", request->conn_id,request->conn_seq);

        //~ request->recv_in_progress = 0;
        //~ request->send_request = 1;

        //~ /*
         //~ * Change the packet interval.  We lost a packet, so we should
         //~ * back off a bit.
         //~ */
        //~ /*
        //~ session->next_packet_interval_us += SESSION_PACKET_LOSS_INTERVAL_INC;
        //~ if(session->next_packet_interval_us > SESSION_MAX_PACKET_INTERVAL) {
            //~ session->next_packet_interval_us = SESSION_MAX_PACKET_INTERVAL;
        //~ }
        //~ */
        //~ /* delay the next packet directly */
        //~ //session->next_packet_time_us += session->next_packet_interval_us;

        //~ /* make sure that we clear this packet from the session if we need to. */
        //~ //clear_session_for_request(request);

        //~ //pdebug(DEBUG_INFO,"packet lost, so increasing packet interval to %lldus", session->next_packet_interval_us);
    //~ //} else {
        //~ //pdebug(DEBUG_INFO,"Not requeuing unconnected request session sequence %llx", request->session_seq_id);
    //~ //}
//~ }


int ok_to_resend(ab_session_p session, ab_request_p request)
{
    if(!session) {
        return 0;
    }

    if(!request) {
        return 0;
    }

    /* was it already sent? */
    if(!request->recv_in_progress) {
        return 0;
    }

    /* did it have a response? */
    if(request->resp_received) {
        return 0;
    }

    /* does it want a resend? */
    if(request->no_resend) {
        return 0;
    }

    /* was the request aborted or should it be aborted? */
    if(request->abort_request || request->abort_after_send) {
        return 0;
    }

    /* have we waited enough time to resend? */
    if((request->time_sent + session->retry_interval) > time_ms()) {
        return 0;
    }

    pdebug(DEBUG_INFO,"Request waited %lldms, need to resend.",(time_ms() - request->time_sent));

    return 1;
}

int process_response_packet(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *response = (eip_cip_co_resp*)(&session->recv_data[0]);
    ab_request_p request = session->requests;

    /* find the request for which there is a response pending. */
    while(request) {
        if(match_request_and_response(request, response)) {
            receive_response(session, request);
        }

        /*
         * resend logic.
         *
         * If we see a request that has been sent, but has not received a response yet,
         * then resend it.  Requests are processed in order (maybe?).  The ones closest to the
         * head of the list are the oldest, so we process them in ascending time order.
         */
        /*if(ok_to_resend(session, request)) {
            handle_resend(session, request);
        }*/

        request = request->next;
    }

    return rc;
}



int session_check_incoming_data_unsafe(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    /*
     * check for data.
     *
     * Read a packet into the session's buffer and find the
     * request it is for.  Repeat while there is data.
     */

    do {
        rc = recv_eip_response_unsafe(session);

        /* did we get a packet? */
        if(rc == PLCTAG_STATUS_OK && session->has_response) {
            rc = process_response_packet(session);

            /* reset the session's buffer */
            mem_set(session->recv_data, 0, MAX_REQ_RESP_SIZE);
            session->recv_offset = 0;
            session->resp_seq_id = 0;
            session->has_response = 0;
        }
    } while(rc == PLCTAG_STATUS_OK);

    /* No data is not an error */
    if(rc == PLCTAG_ERR_NO_DATA) {
        rc = PLCTAG_STATUS_OK;
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Error while trying to receive a response packet.  rc=%d",rc);
    }

    return rc;
}

int request_check_outgoing_data_unsafe(ab_session_p session, ab_request_p request)
{
    int rc = PLCTAG_STATUS_OK;

    /*pdebug(DEBUG_DETAIL,"Starting.");*/

    /*
     * Check to see if we can send something.
     */

    if(!session->current_request && request->send_request /*&& session->next_packet_time_us < (time_ms() * 1000)*/) {
        /* nothing being sent and this request is outstanding */

        /* refcount++ since we are storing a pointer */
        request_acquire(request);
        session->current_request = request;
        //session->next_packet_time_us = (time_ms()*1000) + session->next_packet_interval_us;

        pdebug(DEBUG_INFO,"request->send_request=%d",request->send_request);
       // pdebug(DEBUG_INFO,"session->next_packet_time_us=%lld and time=%lld",session->next_packet_time_us, (time_ms()*1000));
        //pdebug(DEBUG_INFO,"Setting up request for sending, next packet in %lldus",(session->next_packet_time_us - (time_ms()*1000)));

        if(request->send_count == 0) {
            request->time_sent = time_ms();
        }

        request->send_count++;
    }

    /* if we are already sending this request, check its status */
    if (session->current_request == request) {
        /* is the request done? */
        if (request->send_request) {
            /* not done, try sending more */
            send_eip_request_unsafe(request);
            /* FIXME - handle return code! */
        } else {
            /*
             * done in some manner, remove it from the session to let
             * another request get sent.
             */
            /* we need to release the request since we are removing a pointer to it. */
            request_release(session->current_request);
            session->current_request = NULL;
        }
    }

    /*pdebug(DEBUG_DETAIL,"Done.");*/

    return rc;
}



static int session_send_current_request(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    if(!session->current_request) {
        return PLCTAG_STATUS_OK;
    }

    rc = send_eip_request_unsafe(session->current_request);

    if(rc != PLCTAG_STATUS_OK) {
        /*error sending packet!*/
        return rc;
    }

    /* if we are done, then clean up */
    if(!session->current_request->send_in_progress) {
        /* release the refcount on the request, we are not referencing it anymore */
        request_release(session->current_request);
        session->current_request = NULL;
    }

    return rc;
}

/*
 * Check to see if the session should send a request to the PLC.
 * This implements the checks for throttling.
 */
//~ static int ready_to_send(ab_request_p request)
//~ {
    //~ /* is there already a request being sent? */
    //~ if(request->session->current_request) {
        //~ return 0;
    //~ }

    //~ /* is this a serialized packet?  If so, are we already sending one? */
    //~ if(request->connected_request && request->session->connected_request_in_flight) {
        //~ return 0;
    //~ }

    //~ /*
     //~ * if this is a connected request type, then check the connection
     //~ * to make sure that we do not have a packet in flight.
     //~ *
     //~ * Only allow a few at a time until we figure out the packet loss problem.
     //~ */
    //~ if(request->connection) {
        //~ for(int index = 0; index < CONNECTION_MAX_IN_FLIGHT; index++) {
            //~ if(!request->connection->request_in_flight[index]) {
                //~ pdebug(DEBUG_INFO,"Found open slot at %d",index);
                //~ return 1;
            //~ } else {
                //~ pdebug(DEBUG_INFO,"Slot %d is full.",index);
            //~ }
        //~ }

        //~ pdebug(DEBUG_INFO,"No open slots to send.");

        //~ return 0;
    //~ }

    //~ /* not a connected message, so OK to send. */
    //~ return 1;
//~ }


//~ static int handle_abort_request(ab_request_p request)
//~ {
    //~ int rc = PLCTAG_STATUS_OK;

    //~ /* clear any in flight markers. */
    //~ clear_connection_for_request(request);
    //~ clear_session_for_request(request);

    //~ rc = session_remove_request_unsafe(request->session,request);

    //~ //request_destroy_unsafe(&request);
    //~ request_release(request);

    //~ return rc;
//~ }

static int session_check_outgoing_data_unsafe(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p request = session->requests;
    int connected_requests_in_flight = 0;
    int unconnected_requests_in_flight = 0;

    /* loop over the requests and process them one at a time. */
    while(request && rc == PLCTAG_STATUS_OK) {
        if(request->abort_request) {
            ab_request_p old_request = request;

            /* skip to the next one */
            request = request->next;

            //~ rc = handle_abort_request(old_request);
            rc = session_remove_request_unsafe(session,old_request);

            //request_destroy_unsafe(&old_request);
            //~ request_release(old_request);

            continue;
        }

        /* check resending */
        if(ok_to_resend(session, request)) {
            //~ handle_resend(session, request);
            if(request->connected_request) {
                pdebug(DEBUG_INFO,"Requeuing connected request.");
            } else {
                pdebug(DEBUG_INFO,"Requeuing unconnected request.");
            }

            request->recv_in_progress = 0;
            request->send_request = 1;
        }

        /* count requests in flight */
        if(request->recv_in_progress) {
            if(request->connected_request) {
                connected_requests_in_flight++;
                pdebug(DEBUG_INFO,"%d connected requests in flight.", connected_requests_in_flight);
            } else {
                unconnected_requests_in_flight++;
                pdebug(DEBUG_INFO,"%d unconnected requests in flight.", unconnected_requests_in_flight);
            }
        }


        /* is there a request ready to send and can we send? */
        if(!session->current_request && request->send_request) {
            if(request->connected_request) {
                if(connected_requests_in_flight < SESSION_MAX_CONNECTED_REQUESTS_IN_FLIGHT) {
                    pdebug(DEBUG_INFO,"Readying connected packet to send.");

                    /* increment the refcount since we are storing a pointer to the request */
                    request_acquire(request);
                    session->current_request = request;

                    connected_requests_in_flight++;

                    pdebug(DEBUG_INFO,"sending packet, so %d connected requests in flight.", connected_requests_in_flight);
                }
            } else {
                if(unconnected_requests_in_flight < SESSION_MAX_UNCONNECTED_REQUESTS_IN_FLIGHT) {
                    pdebug(DEBUG_INFO,"Readying unconnected packet to send.");

                    /* increment the refcount since we are storing a pointer to the request */
                    request_acquire(request);
                    session->current_request = request;

                    unconnected_requests_in_flight++;

                    pdebug(DEBUG_INFO,"sending packet, so %d unconnected requests in flight.", unconnected_requests_in_flight);
                }
            }
        }

        /* call this often to make sure we get the data out. */
        rc = session_send_current_request(session);

        /* get the next request to process */
        request = request->next;
    }

    return rc;
}


static void process_session_tasks_unsafe(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    if(!session->registered) {
        return;
    }

    /* check for incoming data. */
    rc = session_check_incoming_data_unsafe(session);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error when checking for incoming session data! %d", rc);
        /* FIXME - do something useful with this error */
    }

    /* check for incoming data. */
    rc = session_check_outgoing_data_unsafe(session);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error when checking for outgoing session data! %d", rc);
        /* FIXME - do something useful with this error */
    }
}




#ifdef _WIN32
DWORD __stdcall request_handler_func(LPVOID not_used)
#else
void* request_handler_func(void* not_used)
#endif
{
    ab_session_p cur_sess;

    /* garbage code to stop compiler from whining about unused variables */
    pdebug(DEBUG_NONE,"Starting with arg %p",not_used);

    while (!library_terminating) {
        /* we need the mutex */
        if (global_session_mut == NULL) {
            pdebug(DEBUG_ERROR, "global_session_mut is NULL!");
            break;
        }

        /*pdebug(DEBUG_INFO,"entering critical block %p",global_session_mut);*/
        critical_block(global_session_mut) {
            /*
             * loop over the sessions.  For each session, see if we can read some
             * data.  If we can, read it in and try to update a request.  If the
             * session has outstanding requests that need to be sent, try to send
             * them.
             */

            cur_sess = sessions;

            while (cur_sess) {
                /* process incoming and outgoing data for the session. */
                process_session_tasks_unsafe(cur_sess);

                /*  move to the next session */
                /*pdebug(DEBUG_INFO,"cur_sess=%p, cur_sess->next=%p",cur_sess, cur_sess->next);*/
                cur_sess = cur_sess->next;
            }
        } /* end synchronized block */
        /*pdebug(DEBUG_INFO,"leaving critical block %p",global_session_mut);*/

        /*
         * give up the CPU. 1ms is not really going to happen.  Usually it is more based on the OS
         * default time and is usually around 10ms.  But, this sleep usually causes context switch.
         */
        sleep_ms(1);
    }

    thread_stop();

    /* FIXME -- this should be factored out as a platform dependency.*/
#ifdef _WIN32
    return (DWORD)0;
#else
    return NULL;
#endif
}







/*
 * read group stuff.  FIXME - move to another file
 */


#define MAX_READ_GROUPS (300)
#define MAX_READ_GROUP_NAME_SIZE (64)

struct {
    int in_use;
    char name[MAX_READ_GROUP_NAME_SIZE+1];
    ab_tag_p driving_tag;
    ab_session_p session;
    ab_connection_p connection;
    ab_tag_p tags;
} read_group_entries[MAX_READ_GROUPS] = {0,};





int check_read_group(ab_tag_p tag, const char *read_group)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Starting.");

    /* punt if there is no read group configured */
    if(!read_group) {
        pdebug(DEBUG_INFO,"No read group configured for this tag.  Nothing to do.");
        return PLCTAG_STATUS_OK;
    }

    /* check name size */
    if(str_length(read_group) < 1 || str_length(read_group) > MAX_READ_GROUP_NAME_SIZE) {
        pdebug(DEBUG_INFO,"Read group name must be at least 1 and at most 64 characters.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* search for the group for this tag. */
    critical_block(global_session_mut) {
        int i = 0;
        int unused = -1;
        int found = -1;

        for(i=0; i < MAX_READ_GROUPS; i++) {
            /* do the easy comparisons first */
            if(read_group_entries[i].in_use && tag->session == read_group_entries[i].session && tag->connection == read_group_entries[i].connection) {
                if(str_cmp_i(read_group, read_group_entries[i].name) == 0) {
                    found = i;
                    break; /* found one */
                }
            }

            /* find the first unused one */
            if(unused == -1 && !read_group_entries[i].in_use) {
                unused = i;
                pdebug(DEBUG_DETAIL,"Found first unused slot %d", unused);
            }
        }

        if(found >= 0) {
            pdebug(DEBUG_INFO,"Using existing slot %d", found);
            tag->read_group = found + 1;
            tag->next = read_group_entries[found].tags;
            read_group_entries[found].tags = tag;
        } else {
            /* not found, make an entry */
            if(unused >= 0) {
                pdebug(DEBUG_INFO,"Creating new entry at slot %d",unused);
                read_group_entries[unused].in_use = 1;
                read_group_entries[unused].session = tag->session;
                read_group_entries[unused].connection = tag->connection;
                read_group_entries[unused].tags = tag;
                tag->next = NULL;
                tag->read_group = unused + 1; /* FIXME - no magic offsets! */
                str_copy(&(read_group_entries[unused].name[0]), read_group, MAX_READ_GROUP_NAME_SIZE);
            } else {
                pdebug(DEBUG_ERROR,"All read group entries are currently in use!");
                rc = PLCTAG_ERR_CREATE;
            }
        }
    }

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}



int read_group_remove_tag(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    int read_group = tag->read_group - 1;

    pdebug(DEBUG_INFO,"Starting.");

    if(read_group < 0) {
        pdebug(DEBUG_INFO, "Tag has no read group.");
        return PLCTAG_STATUS_OK;
    }

    /* this modifies shared state. */
    critical_block(global_session_mut) {
        ab_tag_p *walker = &read_group_entries[read_group].tags;

        while(*walker && *walker != tag) {
            walker = &((*walker)->next);
        }

        /* did we find it? */
        if(*walker == tag) {
            pdebug(DEBUG_INFO,"Found the tag, removing it from the list.");
            *walker = tag->next;

            /* if there are no tags left, recycle the resource. */
            if(read_group_entries[read_group].tags == NULL) {
                pdebug(DEBUG_INFO,"No tags left in read group %d, clear it for reuse.", read_group);
                read_group_entries[read_group].in_use = 0;
                read_group_entries[read_group].connection = NULL;
                read_group_entries[read_group].session = NULL;
                read_group_entries[read_group].name[0] = 0;
            } else {
                pdebug(DEBUG_INFO,"read group still has remaining tags.");
            }
        } else {
            pdebug(DEBUG_WARN,"Tag not found in read group %d!", read_group);
        }
    }

    return rc;
}



ab_tag_p read_group_get_tags_unsafe(ab_tag_p tag)
{
    int read_group = tag->read_group - 1;

    if(read_group < 0) {
        return NULL;
    }

    return read_group_entries[read_group].tags;
}



ab_tag_p read_group_get_driver_unsafe(ab_tag_p tag)
{
    int read_group = tag->read_group - 1;

    if(read_group < 0) {
        return NULL;
    }

    return read_group_entries[read_group].driving_tag;
}


ab_tag_p read_group_set_driver_unsafe(ab_tag_p tag)
{
    int read_group = tag->read_group - 1;
    ab_tag_p old_driver;

    if(read_group < 0) {
        return NULL;
    }

    old_driver = read_group_entries[read_group].driving_tag;

    read_group_entries[read_group].driving_tag = tag;

    return old_driver;
}






