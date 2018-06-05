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
 *                                                                        *
 * 2015-09-12  KRH - modified for use with new infrastructure and         *
 *                   connection-based use.                                *
 **************************************************************************/



#include <lib/libplctag.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <ab/eip_dhp_pccc.h>
#include <ab/tag.h>
#include <ab/connection.h>
#include <ab/session.h>
#include <ab/eip.h>
#include <util/debug.h>


static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);

/*
 * eip_dhp_pccc_tag_status
 *
 * PCCC/DH+-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int eip_dhp_pccc_tag_status(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    int session_rc = PLCTAG_STATUS_OK;
    int connection_rc = PLCTAG_STATUS_OK;

    if(tag->read_in_progress) {
        rc = check_read_status(tag);

        return rc;
    }

    if(tag->write_in_progress) {
        rc = check_write_status(tag);

        return rc;
    }

    /* propagate the status up. */
    if (tag->session) {
        session_rc = tag->session->status;
    } else {
        /* this is not OK.  This is fatal! */
        session_rc = PLCTAG_ERR_CREATE;
    }

    if(tag->needs_connection) {
        if(tag->connection) {
            connection_rc = tag->connection->status;
        } else {
            /* fatal! */
            connection_rc = PLCTAG_ERR_CREATE;
        }
    } else {
        connection_rc = PLCTAG_STATUS_OK;
    }

    /* now collect the status.  Highest level wins. */
    rc = session_rc;

    if(rc == PLCTAG_STATUS_OK) {
        rc = connection_rc;
    }

    if(rc == PLCTAG_STATUS_OK) {
        rc = tag->status;
    }

    return rc;
}



/*
 * eip_dhp_pccc_tag_read_start
 *
 * This does not support multiple request fragments.
 */
int eip_dhp_pccc_tag_read_start(ab_tag_p tag)
{
    pccc_dhp_co_req *pccc;
    uint8_t *data;
    int data_per_packet;
    int overhead;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_INFO,"Starting");

    /* how many packets will we need? How much overhead? */
    overhead = sizeof(pccc_resp) + 4 + tag->encoded_name_size; /* MAGIC 4 = fudge */

    data_per_packet = MAX_PCCC_PACKET_SIZE - overhead;

    if(data_per_packet <= 0) {
        pdebug(DEBUG_WARN,"Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, MAX_EIP_PACKET_SIZE);
        return PLCTAG_ERR_TOO_LONG;
    }

    if(data_per_packet < tag->size) {
        pdebug(DEBUG_WARN,"PCCC requests cannot be fragmented.  Too much data requested.");
        return PLCTAG_ERR_TOO_LONG;
    }

    if(!tag->reqs) {
        tag->reqs = (ab_request_p*)mem_alloc(1 * sizeof(ab_request_p));
        tag->max_requests = 1;
        tag->num_read_requests = 1;
        tag->num_write_requests = 1;

        if(!tag->reqs) {
            pdebug(DEBUG_ERROR,"Unable to get memory for request array!");
            return PLCTAG_ERR_NO_MEM;
        }
    }

    /* get a request buffer */
    rc = request_create(&req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to get new request.  rc=%d",rc);
        return rc;
    }

    req->num_retries_left = tag->num_retries;
    req->retry_interval = tag->default_retry_interval;

    pccc = (pccc_dhp_co_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(pccc_dhp_co_req);

    /* copy encoded into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* we need the count twice? */
    *((uint16_t*)data) = h2le16(tag->elem_count); /* FIXME - bytes or INTs? */
    data += sizeof(uint16_t);

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16(data - (uint8_t*)(&(pccc->cpf_conn_seq_num)));/* REQ: fill in with length of remaining data. */

    /* DH+ Routing */
    pccc->dest_link = 0;
    pccc->dest_node = h2le16(tag->dhp_dest);
    pccc->src_link = 0;
    pccc->src_node = 0 /*h2le16(tag->dhp_src)*/;

    /* PCCC Command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = /*h2le16(conn_seq_id)*/ h2le16((uint16_t)(intptr_t)(tag->connection));
    pccc->pccc_function = AB_EIP_PCCC_TYPED_READ_FUNC;
    pccc->pccc_transfer_size = h2le16(tag->elem_count); /* This is not in the docs, but it is in the data. */

    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);

    /* store the connection */
    req->connection = tag->connection;

    /* this request is connected, so it needs the session exclusively */
    req->connected_request = 1;

    /* mark the request ready for sending */
    req->send_request = 1;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        request_release(req);
        tag->reqs[0] = NULL;
        return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;
    tag->read_in_progress = 1;

    /* the read is now pending */
    pdebug(DEBUG_INFO,"Done.");

    return PLCTAG_STATUS_PENDING;
}



int eip_dhp_pccc_tag_write_start(ab_tag_p tag)
{
    pccc_dhp_co_req *pccc;
    uint8_t *data;
    uint8_t element_def[16];
    int element_def_size;
    uint8_t array_def[16];
    int array_def_size;
    int pccc_data_type;
    int data_per_packet = 0;
    int overhead = 0;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_INFO,"Starting");

    /* how many packets will we need? How much overhead? */
    overhead = sizeof(pccc_resp) + 4 + tag->encoded_name_size; /* MAGIC 4 = fudge */

    data_per_packet = MAX_PCCC_PACKET_SIZE - overhead;

    if(data_per_packet <= 0) {
        pdebug(DEBUG_WARN,"Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, MAX_EIP_PACKET_SIZE);
        return PLCTAG_ERR_TOO_LONG;
    }

    if(data_per_packet < tag->size) {
        pdebug(DEBUG_WARN,"PCCC requests cannot be fragmented.  Too much data requested.");
        return PLCTAG_ERR_TOO_LONG;
    }

    if(!tag->reqs) {
        tag->reqs = (ab_request_p*)mem_alloc(1 * sizeof(ab_request_p));
        tag->max_requests = 1;
        tag->num_read_requests = 1;
        tag->num_write_requests = 1;

        if(!tag->reqs) {
            pdebug(DEBUG_ERROR,"Unable to get memory for request array!");
            return PLCTAG_ERR_NO_MEM;
        }
    }

    /* get a request buffer */
    rc = request_create(&req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to get new request.  rc=%d",rc);
        return rc;
    }

    req->num_retries_left = tag->num_retries;
    req->retry_interval = tag->default_retry_interval;

    pccc = (pccc_dhp_co_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(pccc_dhp_co_req);

    /* copy laa into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* What type and size do we have? */
    if(tag->elem_size != 2 && tag->elem_size != 4) {
        pdebug(DEBUG_ERROR,"Unsupported data type size: %d",tag->elem_size);
        //~ request_destroy(&req);
        request_release(req);
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    /* FIXME - base this on the data type. N7:0 -> INT F8:0 -> Float etc. */
    if(tag->elem_size == 4) {
        pccc_data_type = AB_PCCC_DATA_REAL;
    } else {
        pccc_data_type = AB_PCCC_DATA_INT;
    }

    /* generate the data type/data size fields, first the element part so that
     * we can get the size for the array part.
     */

    if(!(element_def_size = pccc_encode_dt_byte(element_def,sizeof(element_def),pccc_data_type,tag->elem_size))) {
        pdebug(DEBUG_WARN,"Unable to encode PCCC request array element data type and size fields!");
        //~ request_destroy(&req);
        request_release(req);
        return PLCTAG_ERR_ENCODE;
    }

    if(!(array_def_size = pccc_encode_dt_byte(array_def,sizeof(array_def),AB_PCCC_DATA_ARRAY,element_def_size + tag->size))) {
        pdebug(DEBUG_WARN,"Unable to encode PCCC request data type and size fields!");
        //~ request_destroy(&req);
        request_release(req);
        return PLCTAG_ERR_ENCODE;
    }

    /* copy the array data first. */
    mem_copy(data,array_def,array_def_size);
    data += array_def_size;

    /* copy the element data */
    mem_copy(data,element_def,element_def_size);
    data += element_def_size;

    /* now copy the data to write */
    mem_copy(data,tag->data, tag->size);
    data += tag->size;


    /* now fill in the rest of the structure. */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16(data - (uint8_t*)(&(pccc->cpf_conn_seq_num)));/* REQ: fill in with length of remaining data. */

    /* DH+ Routing */
    pccc->dest_link = 0;
    pccc->dest_node = h2le16(tag->dhp_dest);
    pccc->src_link = 0;
    pccc->src_node = 0 /*h2le16(tag->dhp_src)*/;

    /* PCCC Command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = h2le16((uint16_t)(intptr_t)(tag->connection));
    pccc->pccc_function = AB_EIP_PCCC_TYPED_WRITE_FUNC;
    pccc->pccc_transfer_size = h2le16(tag->elem_count); /* This is not in the docs, but it is in the data. */

    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);

    /* store the connection */
    req->connection = tag->connection;

    /* ready the request for sending */
    req->send_request = 1;

    /* this request is connected, so it needs the session exclusively */
    req->connected_request = 1;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        request_release(req);
        tag->reqs[0] = NULL;
        return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;
    tag->write_in_progress = 1;

    pdebug(DEBUG_INFO,"Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * check_read_status
 *
 * PCCC does not support request fragments.
 */
static int check_read_status(ab_tag_p tag)
{
    pccc_dhp_co_resp *resp;
    uint8_t *data;
    uint8_t *data_end;
    int pccc_res_type;
    int pccc_res_length;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_DETAIL,"Starting");

    /* is there an outstanding request? */
    if(!tag->reqs || !(tag->reqs[0]) ) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_WARN,"Read was in progress, but there are no outstanding requests!");
        /* FIXME - this should be a different error, but which one? */
        return PLCTAG_ERR_NULL_PTR;
    }

    req = tag->reqs[0];

    if(!req->resp_received) {
        /* still waiting */
        return PLCTAG_STATUS_PENDING;
    }

    /* fake exception */
    do {
        resp = (pccc_dhp_co_resp*)(req->data);

        /* point to the start of the data */
        data = (uint8_t *)resp + sizeof(*resp);

        /* point to the end of the data */
        data_end = (req->data + le2h16(resp->encap_length) + sizeof(eip_encap_t));

        if( le2h16(resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN,"Unexpected EIP packet type received: %d!",resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h16(resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(resp->pccc_status != AB_EIP_OK) {
            /* data points to the byte following the header */
            pdebug(DEBUG_WARN,"PCCC error: %d - %s", *data, pccc_decode_error(*data));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* all status is good, try to decode the data type */
        if(!(data = pccc_decode_dt_byte(data,data_end - data, &pccc_res_type,&pccc_res_length))) {
            pdebug(DEBUG_WARN,"Unable to decode PCCC response data type and data size!");
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        /* this gives us the overall type of the response and the number of bytes remaining in it.
         * If the type is an array, then we need to decode another one of these words
         * to get the type of each element and the size of each element.  We will
         * need to adjust the size if we care.
         */

        if(pccc_res_type == AB_PCCC_DATA_ARRAY) {
            if(!(data = pccc_decode_dt_byte(data,data_end - data, &pccc_res_type,&pccc_res_length))) {
                pdebug(DEBUG_WARN,"Unable to decode PCCC response array element data type and data size!");
                rc = PLCTAG_ERR_BAD_DATA;
                break;
            }
        }

        /* copy data into the tag. */
        if((data_end - data) > tag->size) {
            rc = PLCTAG_ERR_TOO_LONG;
            break;
        }

        /* all OK, copy the data. */
        mem_copy(tag->data, data, data_end - data);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up request */
    ab_tag_abort(tag);

    tag->read_in_progress = 0;

    pdebug(DEBUG_DETAIL,"Done.");

    return rc;
}


static int check_write_status(ab_tag_p tag)
{
    pccc_dhp_co_resp *pccc_resp;
    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_DETAIL,"Starting.");

    /* is there an outstanding request? */
    if(!tag->reqs || !(tag->reqs[0]) ) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_WARN,"Write was in progress, but no requests are in flight!");
        return PLCTAG_ERR_NULL_PTR;
    }

    req = tag->reqs[0];

    /* is there an outstanding request? */
    if(!req) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_WARN,"Write was in progress, but no requests are outstanding!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!req->resp_received) {
        /* still waiting */
        return PLCTAG_STATUS_PENDING;
    }

    /* fake exception */
    do {
        pccc_resp = (pccc_dhp_co_resp*)(req->data);

        /* point data just past the header */
        data = (uint8_t *)pccc_resp + sizeof(*pccc_resp);

        /* check the response status */
        if( le2h16(pccc_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN,"EIP unexpected response packet type: %d!",pccc_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h16(pccc_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",pccc_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc_resp->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"PCCC error: %d - %s", *data, pccc_decode_error(*data));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /*if(pccc_resp->general_status != AB_EIP_OK) {
            pdebug(DEBUG_INFO,"PCCC command failed, response code: %d",pccc_resp->general_status);
            return 0;
        }*/

        /* everything OK */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    tag->write_in_progress = 0;

    /* clean up any outstanding requests. */
    ab_tag_abort(tag);

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}
