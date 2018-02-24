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

/*#ifdef __cplusplus
extern "C"
{
#endif
*/



#include <libplctag.h>
#include <ab/ab_defs.h>
#include <ab/common.h>
#include <ab/pccc.h>
#include <ab/plc5_dhp.h>


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
    if(tag->read_in_progress) {
        int rc = check_read_status(tag);

        tag->status = rc;

        return rc;
    }

    if(tag->write_in_progress) {
        int rc = check_write_status(tag);

        tag->status = rc;

        return rc;
    }

    /*
     * if the session we are using is not yet connected,
     * then return PENDING and let the tag that started
     * the connection finish it.
     *
     * FIXME - if the tag that started the connection
     * fails to connect or is aborted/destroyed, then the
     * connection will never be created.
     */
    if(tag->session) {
        if(!tag->session->is_connected) {
            tag->status = PLCTAG_STATUS_PENDING;
        } else {
            tag->status = PLCTAG_STATUS_OK;
        }
    }

    return tag->status;
}





/*
 * eip_dhp_pccc_tag_read_start
 *
 * This does not support multiple request fragments.
 */
int eip_dhp_pccc_tag_read_start(ab_tag_p tag)
{
    eip_pccc_dhp_req_old *pccc;
    uint8_t *data;
    int num_reqs;
    int data_per_packet;
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = 0;
    ab_request_p req;
    int debug = tag->debug;

    pdebug(DEBUG_INFO,"Starting");

    data_per_packet = MAX_PCCC_PACKET_SIZE - sizeof(eip_pccc_dhp_req_old) - 4;

    num_reqs = (tag->size + (data_per_packet-1)) / data_per_packet;

    if(num_reqs > 1) {
        pdebug(DEBUG_WARN,"PCCC requests cannot be fragmented.  Too much data requested.");
        return PLCTAG_ERR_TOO_LONG;
    }

    pdebug(DEBUG_DETAIL,"We need %d requests of up to %d bytes each.", num_reqs, data_per_packet);

    tag->reqs = (ab_request_p*)mem_alloc(num_reqs * sizeof(ab_request_p));

    if(!tag->reqs) {
        pdebug(DEBUG_ERROR,"Unable to get memory for request array!");
        tag->status = PLCTAG_ERR_NO_MEM;
        return rc;
    }


    /* get a request buffer */
    rc = request_create(&req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to get new request.  rc=%d",rc);
        tag->status = rc;
        return rc;
    }

    req->debug = tag->debug;

    pccc = (eip_pccc_dhp_req_old*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_pccc_req_old);

    /* copy laa into the request */
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
    //pccc->cpf_targ_conn_id = tag->connection->targ_connection_id;
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16(data - (uint8_t*)(&(pccc->dest_link)));/* REQ: fill in with length of remaining data. */

    /* connection sequence id */
    //conn_seq_id = connection_get_new_seq_id(tag->connection);

    /*
     * FIXME
     *
     * Is this really missing?
     */
    /* pccc->cpf_conn_seq_num = h2le16(tag->connection->conn_seq_num);*/

    /* DH+ Routing */
    pccc->dest_link = 0;
    pccc->dest_node = tag->dhp_dest;
    pccc->src_link = 0;
    pccc->src_node = tag->dhp_src;

    /* PCCC Command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = h2le16(conn_seq_id);
    pccc->pccc_function = AB_EIP_PCCC_TYPED_READ_FUNC;
    pccc->pccc_transfer_size = h2le16(tag->elem_count); /* This is not in the docs, but it is in the data. */
                                                        /* FIXME - bytes or INTs? */

    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);
    req->send_request = 1;
    //req->conn_id = tag->connection->orig_connection_id;
    req->conn_seq = conn_seq_id;

    /* add the request to the session's list. */
    rc = request_add(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to lock add request to session! rc=%d",rc);
        request_destroy(&req);
        tag->status = rc;
        return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;
    tag->read_in_progress = 1;

    /* the read is now pending */
    tag->status = PLCTAG_STATUS_PENDING;

    pdebug(DEBUG_INFO,"Done.");

    return PLCTAG_STATUS_PENDING;
}



/* FIXME -- this needs to be converted to unconnected messaging */
int eip_dhp_pccc_tag_write_start(ab_tag_p tag)
{
    eip_pccc_dhp_req_old *pccc;
    uint8_t *data;
    uint8_t element_def[16];
    int element_def_size;
    uint8_t array_def[16];
    int array_def_size;
    int pccc_data_type;
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = 0;
    ab_request_p req;
    int data_per_packet;
    int num_reqs;
    int debug = tag->debug;

    pdebug(DEBUG_INFO, "Starting.");

    data_per_packet = MAX_PCCC_PACKET_SIZE - sizeof(eip_pccc_dhp_req_old) - 4;

    num_reqs = (tag->size + (data_per_packet-1)) / data_per_packet;

    if(num_reqs > 1) {
        pdebug(DEBUG_WARN,"PCCC requests cannot be fragmented.  Too much data requested.");
        return PLCTAG_ERR_TOO_LONG;
    }

    pdebug(DEBUG_DETAIL,"We need %d requests of up to %d bytes each.", num_reqs, data_per_packet);

    tag->reqs = (ab_request_p*)mem_alloc(num_reqs * sizeof(ab_request_p));

    if(!tag->reqs) {
        pdebug(DEBUG_ERROR,"Unable to get memory for request array!");
        tag->status = PLCTAG_ERR_NO_MEM;
        return rc;
    }



    /* get a request buffer */
    rc = request_create(&req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to get new request.  rc=%d",rc);
        tag->status = rc;
        return rc;
    }

    req->debug = tag->debug;

    pccc = (eip_pccc_dhp_req_old*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_pccc_req_old);

    /* copy laa into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* What type and size do we have? */
    if(tag->elem_size != 2 && tag->elem_size != 4) {
        pdebug(DEBUG_WARN,"Unsupported data type size: %d",tag->elem_size);
        request_destroy(&req);
        tag->status = PLCTAG_ERR_NOT_ALLOWED;
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    if(tag->elem_size == 4)
        pccc_data_type = AB_PCCC_DATA_REAL;
    else
        pccc_data_type = AB_PCCC_DATA_INT;

    /* generate the data type/data size fields, first the element part so that
     * we can get the size for the array part.
     */

    if(!(element_def_size = pccc_encode_dt_byte(element_def,sizeof(element_def),pccc_data_type,tag->elem_size))) {
        pdebug(DEBUG_WARN,"Unable to encode PCCC request array element data type and size fields!");
        request_destroy(&req);
        tag->status = PLCTAG_ERR_ENCODE;
        return PLCTAG_ERR_ENCODE;
    }

    if(!(array_def_size = pccc_encode_dt_byte(array_def,sizeof(array_def),AB_PCCC_DATA_ARRAY,element_def_size + tag->size))) {
        pdebug(DEBUG_WARN,"Unable to encode PCCC request data type and size fields!");
        request_destroy(&req);
        tag->status = PLCTAG_ERR_ENCODE;
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
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    //pccc->cpf_targ_conn_id = tag->connection->targ_connection_id;
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16(data - (uint8_t*)(&(pccc->dest_link)));/* REQ: fill in with length of remaining data. */

    /* DH+ Routing */
    pccc->dest_link = 0;
    pccc->dest_node = tag->dhp_dest;
    pccc->src_link = 0;
    pccc->src_node = tag->dhp_src;

    /* PCCC Command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */

    //conn_seq_id = connection_get_new_seq_id(tag->connection);
    pccc->pccc_seq_num = h2le16(conn_seq_id);
    pccc->pccc_function = AB_EIP_PCCC_TYPED_WRITE_FUNC;
    /* FIXME - what should be the count here?  It is bytes, 16-bit
     * words or something else??
     *
     * Seems to be the number of elements?
     */
    pccc->pccc_transfer_size = h2le16(tag->elem_count); /* This is not in the docs, but it is in the data. */


    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);
    req->send_request = 1;
    //req->conn_id = tag->connection->orig_connection_id;
    //req->conn_seq = connection_get_new_seq_id(tag->connection);

    /* add the request to the session's list. */
    rc = request_add(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to lock add request to session! rc=%d",rc);
        request_destroy(&req);
        tag->status = rc;
        return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;

    /* the write is now pending */
    tag->write_in_progress = 1;
    tag->status = PLCTAG_STATUS_PENDING;

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
    eip_pccc_dhp_resp_old *pccc_resp;
    uint8_t *data;
    uint8_t *data_end;
    int pccc_res_type;
    int pccc_res_length;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;
    int debug = tag->debug;

    pdebug(DEBUG_DETAIL,"Starting");

    /* is there an outstanding request? */
    if(!tag->reqs || !(tag->reqs[0]) ) {
        tag->read_in_progress = 0;
        tag->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    req = tag->reqs[0];

    if(!req->resp_received) {
        tag->status = PLCTAG_STATUS_PENDING;
        return PLCTAG_STATUS_PENDING;
    }


    /*
     * we have a response. Remove the request/response from
     * the list for the session.
     */
    rc = request_remove(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to remove the request from the list! rc=%d",rc);

        /* since we could not remove it, maybe the thread can. */
        plc_tag_abort((plc_tag)tag);

        /* not our request any more */
        req = NULL;

        tag->status = rc;

        return rc;
    }


     /* fake exception */
     do {
        pccc_resp = (eip_pccc_dhp_resp_old*)(req->data);

        data_end = (req->data + pccc_resp->encap_length + sizeof(eip_encap_t));

        if( le2h16(pccc_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN,"Unexpected EIP packet type received: %d!",pccc_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h16(pccc_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",pccc_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc_resp->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"PCCC error: %d - %s", pccc_resp->pccc_data[0], pccc_decode_error(pccc_resp->pccc_data[0]));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* point to the start of the data */
        data = pccc_resp->pccc_data;

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
                pdebug(debug,"Unable to decode PCCC response array element data type and data size!");
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

     if(req) {
        request_destroy(&(req));
        req = NULL;
        mem_free(tag->reqs);
        tag->reqs = NULL;
     }

     tag->read_in_progress = 0;

     tag->status = rc;

     pdebug(debug,"Done.");

     return rc;
}





/* FIXME -- this needs to be converted to unconnected messaging. */
static int check_write_status(ab_tag_p tag)
{
    eip_pccc_dhp_resp_old *pccc_resp;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;
    int debug = tag->debug;

    pdebug(debug,"Starting.");

    /* is there an outstanding request? */
    if(!tag->reqs || !(tag->reqs[0]) ) {
        tag->write_in_progress = 0;
        tag->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    req = tag->reqs[0];

    /* is there an outstanding request? */
     if(!req) {
         tag->write_in_progress = 0;
        tag->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
     }

     if(!req->resp_received) {
        tag->status = PLCTAG_STATUS_PENDING;
        return PLCTAG_STATUS_PENDING;
     }

    /*
     * we have a response. Remove the request/response from
     * the list for the session.
     */
    rc = request_remove(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(debug,"Unable to remove the request from the list! rc=%d",rc);

        /* since we could not remove it, maybe the thread can. */
        plc_tag_abort((plc_tag)tag);

        /* not our request any more */
        req = NULL;

        tag->status = rc;

        return rc;
    }


    /* fake exception */
    do {
        pccc_resp = (eip_pccc_dhp_resp_old*)(req->data);

        /* check the response status */
        if( le2h16(pccc_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(debug,"EIP unexpected response packet type: %d!",pccc_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h16(pccc_resp->encap_status) != AB_EIP_OK) {
            pdebug(debug,"EIP command failed, response code: %d",pccc_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc_resp->pccc_status != AB_EIP_OK) {
            /*pdebug(debug,"PCCC command failed, response code: %d",pccc_resp->pccc_status);*/
            pdebug(tag->debug,"PCCC error: %d - %s", pccc_resp->pccc_data[0], pccc_decode_error(pccc_resp->pccc_data[0]));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /*if(pccc_resp->general_status != AB_EIP_OK) {
            pdebug(debug,"PCCC command failed, response code: %d",pccc_resp->general_status);
            return 0;
        }*/

        /* everything OK */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    tag->write_in_progress = 0;
    tag->status = rc;

    if(req) {
        request_destroy(&(req));
        req = NULL;
        if(tag->reqs) {
            mem_free(tag->reqs);
            tag->reqs = NULL;
        }
    }

    pdebug(debug,"Done.");

    return rc;
}






START_PACK typedef struct {
        /* encap header */
    uint16_t encap_command;         /* ALWAYS 0x006f Unconnected Send*/
    uint16_t encap_length;          /* packet size in bytes - 24 */
    uint32_t encap_session_handle;  /* from session set up */
    uint32_t encap_status;          /* always _sent_ as 0 */
    uint64_t encap_sender_context;  /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_t encap_options;         /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds, 5 or 10 seems to be good.*/

    /* Common Packet Format - CPF Unconnected */
    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_nai_item_type;     /* ALWAYS 0 */
    uint16_t cpf_nai_item_length;   /* ALWAYS 0 */
    uint16_t cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_t cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */

    /* PCCC Command Req Routing */
    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
    uint8_t req_path_size;          /* 0x03, in 16-bit words */
    uint8_t req_path[6];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_t vendor_id;             /* Our CIP Vendor ID */
    uint32_t vendor_serial_number;  /* Our CIP Vendor Serial Number */

    /* needed when talking to PLC5 over DH+ */
    uint16_t dest_link;
    uint16_t dest_node;
    uint16_t src_link;
    uint16_t src_node;

    /* PCCC Command */
    uint8_t pccc_command;           /* CMD read, write etc. */
    uint8_t pccc_status;            /* STS 0x00 in request */
    uint16_t pccc_seq_num;          /* TNS transaction/sequence id */
    uint8_t pccc_function;          /* FNC sub-function of command */
    uint16_t pccc_offset;           /* offset of requested in total request */
    uint16_t pccc_transfer_size;    /* total number of words requested */
    uint8_t pccc_data[ZLA_SIZE];   /* send_data for request */
} END_PACK pccc_dhp_req;



START_PACK typedef struct {
        /* encap header */
    uint16_t encap_command;         /* ALWAYS 0x006f Unconnected Send*/
    uint16_t encap_length;          /* packet size in bytes - 24 */
    uint32_t encap_session_handle;  /* from session set up */
    uint32_t encap_status;          /* always _sent_ as 0 */
    uint64_t encap_sender_context;  /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_t encap_options;         /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds, 5 or 10 seems to be good.*/

    /* Common Packet Format - CPF Unconnected */
    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_nai_item_type;     /* ALWAYS 0 */
    uint16_t cpf_nai_item_length;   /* ALWAYS 0 */
    uint16_t cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_t cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */

    /* PCCC Reply */
    uint8_t reply_code;          /* 0xCB Execute PCCC Reply */
    uint8_t reserved;               /* 0x00 in reply */
    uint8_t general_status;         /* 0x00 for success */
    uint8_t status_size;            /* number of 16-bit words of extra status, 0 if success */

    /* PCCC Command Req Routing */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_t vendor_id;             /* Our CIP Vendor ID */
    uint32_t vendor_serial_number;  /* Our CIP Vendor Serial Number */

    /* PCCC Command */
    uint8_t pccc_command;           /* CMD read, write etc. */
    uint8_t pccc_status;            /* STS 0x00 in request */
    uint16_t pccc_seq_num;          /* TNSW transaction/connection sequence number */
    uint8_t pccc_data[ZLA_SIZE];    /* data for PCCC response. */
} END_PACK pccc_dhp_resp;



