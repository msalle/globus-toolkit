/******************************************************************************
globus_gass_transfer_http.c
 
Description:
    This module implements the http and https URL schemes for the GASS transfer
    library
 
CVS Information:
 
    $Source$
    $Date$
    $Revision$
    $Author$
******************************************************************************/

#include "globus_i_gass_transfer.h"
#include "globus_l_gass_transfer_http.h"
#include "globus_io.h"

#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

#if !defined(GLOBUS_GASS_TRANSFER_HTTP_PARSER_TEST)

int
globus_l_gass_transfer_http_activate(void)
{
    OM_uint32				maj_stat;
    OM_uint32				min_stat;
    gss_name_t 				name;
    static gss_cred_id_t		globus_l_gass_transfer_http_credential;
    gss_buffer_desc			name_buffer;

    name_buffer.value = GLOBUS_NULL;
    name_buffer.length = 0;

    globus_mutex_init(&globus_l_gass_transfer_http_mutex, GLOBUS_NULL);

    maj_stat = globus_gss_assist_acquire_cred(
	&min_stat,
	GSS_C_BOTH,
	&globus_l_gass_transfer_http_credential);

    if (maj_stat != GSS_S_COMPLETE)
    {
	return GLOBUS_FAILURE;
    }

    maj_stat = gss_inquire_cred(
	&min_stat,
	globus_l_gass_transfer_http_credential,
	&name,
	GLOBUS_NULL,
	GLOBUS_NULL,
	GLOBUS_NULL);

    if (maj_stat != GSS_S_COMPLETE)
    {
	return GLOBUS_FAILURE;
    }

    maj_stat = gss_display_name(
	&min_stat,
	name,
	&name_buffer,
	GLOBUS_NULL);

    if (maj_stat != GSS_S_COMPLETE)
    {
	return GLOBUS_FAILURE;
    }
    globus_l_gass_transfer_http_subject_name = name_buffer.value;

    return GLOBUS_SUCCESS;
}

int
globus_l_gass_transfer_http_deactivate(void)
{
    globus_mutex_destroy(&globus_l_gass_transfer_http_mutex);
    globus_free(globus_l_gass_transfer_http_subject_name);

    return GLOBUS_SUCCESS;
}
globus_module_descriptor_t globus_i_gass_transfer_http_module =
{
    "globus_i_gass_transfer_http",
    globus_l_gass_transfer_http_activate,
    globus_l_gass_transfer_http_deactivate,
    GLOBUS_NULL
};

/* Protocol Descriptor, which is registered with the GASS system */
globus_gass_transfer_proto_descriptor_t
globus_i_gass_transfer_http_descriptor =
{
    "http",

    /* client-side support */
    globus_l_gass_transfer_http_new_requestattr,
    globus_l_gass_transfer_http_new_request,

    /* server-side support */
    globus_l_gass_transfer_http_new_listenerattr /* new_listenerattr */,
    globus_l_gass_transfer_http_new_listener /* new_listener */
};

globus_gass_transfer_proto_descriptor_t
globus_i_gass_transfer_https_descriptor =
{
    "https",

    /* client-side support */
    globus_l_gass_transfer_http_new_requestattr,
    globus_l_gass_transfer_http_new_request,

    /* server-side support */
    globus_l_gass_transfer_http_new_listenerattr /* new_listenerattr */,
    globus_l_gass_transfer_http_new_listener /* new_listener */
};

/*
 * Function: globus_l_gass_transfer_http_send()
 * 
 * Description: Send a byte array to an HTTP server
 * 
 * Parameters: 
 * 
 * Returns: 
 */
static
void
globus_l_gass_transfer_http_send(
    globus_gass_transfer_request_proto_t *	proto,
    globus_gass_transfer_request_t		request,
    globus_byte_t *				buffer,
    globus_size_t				buffer_length,
    globus_bool_t				last_data)
{
    globus_result_t				result;
    globus_gass_transfer_http_request_proto_t *		new_proto;

    globus_l_gass_transfer_http_lock();
    new_proto = (globus_gass_transfer_http_request_proto_t *) proto;
    new_proto->last_data = last_data;

    /* We can only process a send if the proto is in the "idle" state */
    globus_assert(new_proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE);

    /* state change to "pending" */
    new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_PENDING;

    /* Update the buffers to point to those supplied by the user */
    new_proto->user_buffer = buffer;
    new_proto->user_buflen = buffer_length;

    /* If a a failure occurred, callback with the failure bit set,
     * and close the handle
     */
    if(new_proto->failure_occurred)
    {
	goto fail_exit;
    }

    /* Register the send of the data */
    if(new_proto->chunked)
    {
	globus_size_t			num_iovecs;

	/* send chunk header and footer as an iovec array */
	sprintf((char *) new_proto->iov[0].iov_base,
		"%x%s",
		new_proto->user_buflen,
		CRLF);
	new_proto->iov[0].iov_len = strlen((char *) new_proto->iov[0].iov_base);

	new_proto->iov[1].iov_base = (void *) new_proto->user_buffer;
	new_proto->iov[1].iov_len = new_proto->user_buflen;

	new_proto->iov[2].iov_base = CRLF;
	new_proto->iov[2].iov_len = strlen(CRLF);

	if(last_data && new_proto->user_buflen != 0)
	{
	    /* last data, need to append a zero-length chunk to
	     * indicate this
	     */
	    num_iovecs = 4;
	}
	else if(last_data && new_proto->user_buflen == 0)
	{
	    /* last data, with a zero-length chunk from the user */
	    num_iovecs = 1;
	}
	{
	    /* normal chunk */
	    num_iovecs = 3;
	}

	result = globus_io_register_writev(
	    &new_proto->handle,
	    new_proto->iov,
	    num_iovecs /* 3 iovecs header, body, final CRLF */,
	    globus_l_gass_transfer_http_writev_callback,
	    new_proto);
    }
    else
    {
	/* send data raw */
	result = globus_io_register_write(
	    &new_proto->handle,
	    new_proto->user_buffer,
	    new_proto->user_buflen,
	    globus_l_gass_transfer_http_write_callback,
	    new_proto);
    }

    if(result == GLOBUS_SUCCESS)
    {
	/*
	 * Registration succeeded. Callback to GASS occurs when I/O
	 * completes.
	 */
	globus_l_gass_transfer_http_unlock();
	return;
    }

  fail_exit:
    /* Registration failed, close up handle and signal failure to GASS */
    new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;

    result =
	globus_io_register_close(&new_proto->handle,
				 globus_l_gass_transfer_http_close_callback,
				 new_proto);
    if(result != GLOBUS_SUCCESS)
    {
	new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
    }

    globus_callback_register_oneshot(
	GLOBUS_NULL /* callback handle */,
	(globus_time_t) 0,
	globus_l_gass_transfer_http_callback_send_callback,
	(void *) new_proto,
	GLOBUS_NULL /* wakeup func */,
	GLOBUS_NULL /* wakeup arg */);

    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_send() */


/*
 * Function: globus_l_gass_transfer_http_receive()
 * 
 * Description: Schedule the next block of data from the http server
 *              to end up in the provided byte array
 * 
 * Parameters: 
 * 
 * Returns: 
 */
static
void
globus_l_gass_transfer_http_receive(
    globus_gass_transfer_request_proto_t *	proto,
    globus_gass_transfer_request_t		request,
    globus_byte_t *				buffer,
    globus_size_t				buffer_length,
    globus_size_t				wait_for_length)
{
    globus_result_t				result;
    globus_gass_transfer_http_request_proto_t *		new_proto;

    globus_l_gass_transfer_http_lock();
    new_proto = (globus_gass_transfer_http_request_proto_t *) proto;

    /* We can only process a receive if the proto is in the "idle" state */
    globus_assert(new_proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE);

    /* state change to "pending" */
    new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_PENDING;

    /* Update the buffers to point to those supplied by the user */
    new_proto->user_buffer = buffer;
    new_proto->user_buflen = buffer_length;
    new_proto->user_offset = 0;
    new_proto->user_waitlen = wait_for_length;

    globus_callback_register_oneshot(
	GLOBUS_NULL /* callback handle */,
	(globus_time_t) 0,
	globus_l_gass_transfer_http_callback_read_buffered_callback,
	(void *) new_proto,
	GLOBUS_NULL /* wakeup func */,
	GLOBUS_NULL /* wakeup arg */);

    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_receive() */

/*
 * Function: globus_l_gass_transfer_http_fail()
 * 
 * Description: Cause the given request to fail for client-caused reasons
 * 
 * Parameters: 
 * 
 * Returns: 
 */
static
void
globus_l_gass_transfer_http_fail(
    globus_gass_transfer_request_proto_t *	proto,
    globus_gass_transfer_request_t		request)
{
    globus_gass_transfer_http_request_proto_t *		new_proto;
    globus_result_t				result;

    new_proto = (globus_gass_transfer_http_request_proto_t *) proto;

    globus_l_gass_transfer_http_lock();

    switch(new_proto->state)
    {
      case GLOBUS_GASS_TRANSFER_HTTP_STATE_CONNECTING:
      case GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE:
      case GLOBUS_GASS_TRANSFER_HTTP_STATE_PENDING:
	/* We will transition to the closing state, signalling the failure,
	 * and registering the close (which will transition us to the 
	 * done state).
	 */
	new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
	new_proto->failure_occurred = GLOBUS_TRUE;

	result = globus_io_register_close(
	    &new_proto->handle,
	    globus_l_gass_transfer_http_close_callback,
	    new_proto);

	if(result != GLOBUS_SUCCESS)
	{
	    new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
	}
	globus_l_gass_transfer_http_unlock();

	break;

      case GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING:
      case GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE:
      case GLOBUS_GASS_TRANSFER_HTTP_STATE_REQUESTING:
	new_proto->failure_occurred = GLOBUS_TRUE;
	globus_l_gass_transfer_http_unlock();
	break;
    }
}
/* globus_l_gass_transfer_http_fail() */

static
void
globus_l_gass_transfer_http_write_callback(
    void *					callback_arg,
    globus_io_handle_t *			handle,
    globus_result_t 				result,
    globus_byte_t *				buf,
    globus_size_t				nbytes)
{
    globus_gass_transfer_http_request_proto_t *		proto;

    proto = (globus_gass_transfer_http_request_proto_t *) callback_arg;

    globus_l_gass_transfer_http_lock();

    if(result != GLOBUS_SUCCESS ||
	    proto->failure_occurred ||
	    proto->parse_error)
    {
	proto->last_data = GLOBUS_TRUE;
    }

    if(proto->last_data)
    {
	proto->user_offset = nbytes;
	globus_l_gass_transfer_http_unlock();
	if(proto->type == GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT ||
	   proto->type == GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND)
	{
	    /* Need to check for a response, and call back from there */
	    result = globus_io_register_read(
		&proto->handle,
		proto->response_buffer,
		proto->response_buflen,
		1,
		globus_l_gass_transfer_http_response_callback,
		proto);
	}
	else
	{
	    /* need to register the close, and clalback to the user */
	    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;

	    globus_l_gass_transfer_http_unlock();
	    globus_gass_transfer_proto_send_complete(
		proto->request,
		proto->user_buffer,
		proto->user_offset,
		proto->failure_occurred,
		GLOBUS_TRUE);

	    result = globus_io_register_close(
		&proto->handle,
		globus_l_gass_transfer_http_close_callback,
		proto);
	    if(result != GLOBUS_SUCCESS)
	    {
		globus_l_gass_transfer_http_close_callback(proto,
							   &proto->handle,
							   GLOBUS_SUCCESS);
	    }
	}
    }
    else
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;

	globus_l_gass_transfer_http_unlock();
	globus_gass_transfer_proto_send_complete(proto->request,
						 proto->user_buffer,
						 nbytes,
						 proto->failure_occurred,
						 proto->last_data);
    }
    return;
}
/* globus_l_gass_transfer_http_write_callback() */

static
void
globus_l_gass_transfer_http_writev_callback(
    void *					callback_arg,
    globus_io_handle_t *			handle,
    globus_result_t 				result,
    struct iovec *				iov,
    globus_size_t				iovcnt,
    globus_size_t				nbytes)
{
    globus_gass_transfer_http_request_proto_t *		proto;

    proto = (globus_gass_transfer_http_request_proto_t *) callback_arg;

    globus_l_gass_transfer_http_lock();

    if(result != GLOBUS_SUCCESS ||
	    proto->failure_occurred ||
	    proto->parse_error)
    {
	proto->last_data = GLOBUS_TRUE;
    }

    if(proto->last_data)
    {
	proto->user_offset = nbytes - iov[0].iov_len - iov[2].iov_len;

	globus_l_gass_transfer_http_unlock();
	/* Need to check for a response, and call back from there */
	result = globus_io_register_read(
	    &proto->handle,
	    proto->response_buffer,
	    proto->response_buflen,
	    1,
	    globus_l_gass_transfer_http_response_callback,
	    proto);
    }
    else
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;

	globus_l_gass_transfer_http_unlock();
	globus_gass_transfer_proto_send_complete(proto->request,
						 proto->user_buffer,
						 nbytes -
					             iov[0].iov_len -
					             iov[2].iov_len,
						 proto->failure_occurred,
						 proto->last_data);
    }
    return;
}
/* globus_l_gass_transfer_http_writev_callback() */

/*
 * Function: globus_l_gass_transfer_http_read_callack()
 * 
 * Description: Callback when the read of from the connection to the active
 *              buffer has completed or failed.
 * 
 * Parameters: 
 * 
 * Returns: 
 */
static
void
globus_l_gass_transfer_http_read_callback(
    void *					callback_arg,
    globus_io_handle_t *			handle,
    globus_result_t				result,
    globus_byte_t *				buf,
    globus_size_t				nbytes)
{
    globus_object_t *				err = GLOBUS_NULL;
    globus_gass_transfer_http_request_proto_t *		proto;

    proto = (globus_gass_transfer_http_request_proto_t *) callback_arg;

    if(result != GLOBUS_SUCCESS)
    {
	err = globus_error_get(result);
    }

    globus_l_gass_transfer_http_lock();

    proto->user_offset += nbytes;
    proto->handled += nbytes;
    if(nbytes > proto->user_waitlen)
    {
	proto->user_waitlen = 0;
    }
    else
    {
	proto->user_waitlen -= nbytes;
    }

    if(proto->chunked)
    {
	proto->chunk_left -= nbytes;
    }

    if(result != GLOBUS_SUCCESS &&
       globus_io_eof(err))
    {
	proto->eof_read = GLOBUS_TRUE;
    }
    else if(result != GLOBUS_SUCCESS ||
	    proto->failure_occurred ||
	    proto->parse_error)
    {
	proto->recv_state = GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
    }

    if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_EOF &&
       proto->eof_read == GLOBUS_TRUE)
    {
	proto->recv_state = GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF;
    }
    else if(proto->recv_state ==
	        GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_LENGTH &&
	    proto->handled == proto->length)
    {
	proto->recv_state = GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF;
    }

    if((proto->type == GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT ||
       proto->type == GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND) &&
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF)
    {
	char  *					response;
	globus_size_t				response_len=0;
	globus_size_t				offset;


	response_len += 1;
	response_len += strlen(GLOBUS_L_GENERIC_RESPONSE);
	response_len += 3;
	response_len += strlen(GLOBUS_L_OK);
	response_len += strlen(CRLF);
	response = globus_malloc(response_len);

	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_RESPONDING;
	offset = sprintf(response,
		GLOBUS_L_GENERIC_RESPONSE,
		0,
		200,
		GLOBUS_L_OK);
	offset += sprintf(response + offset,
			  CRLF);

	globus_io_register_write(&proto->handle,
				 (globus_byte_t *) response,
				 strlen(response),
				 globus_l_gass_transfer_http_write_response,
				 proto);
    }
    /* Register the socket for closing if we're done reading from it */
    else if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF ||
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
	result =
	    globus_io_register_close(
		&proto->handle,
		globus_l_gass_transfer_http_close_callback,
		proto);
	if(result != GLOBUS_SUCCESS)
	{
	    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
	}
    }

    if(proto->user_waitlen == 0 ||
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF ||
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR)
    {
	globus_bool_t				last_data = GLOBUS_FALSE;
	/*
	 * Received the required minimum of data from connection, an
	 * error, or the end-of file, signal this to GASS
	 */
	if(proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_PENDING)
	{
	    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;
	}
	if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF ||
	   proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR)
	{
	    last_data = GLOBUS_TRUE;
	}

	globus_l_gass_transfer_http_unlock();
	globus_gass_transfer_proto_receive_complete(proto->request,
						    proto->user_buffer,
						    proto->user_offset,
						    proto->failure_occurred,
						    last_data);
    }
    else
    {
	result = globus_l_gass_transfer_http_register_read(proto);
    }

    if(err)
    {
	globus_object_free(err);
    }

    return;
}
/* globus_l_gass_transfer_http_read_callback() */

/*
 * Function: globus_l_gass_transfer_http_read_buffered_callack()
 * 
 * Description: Callback when the read of from the http to the
 *              response buffer has completed or failed.
 * 
 * Parameters: 
 * 
 * Returns: 
 */
static
void
globus_l_gass_transfer_http_read_buffered_callback(
    void *					callback_arg,
    globus_io_handle_t *			handle,
    globus_result_t				result,
    globus_byte_t *				buf,
    globus_size_t				nbytes)
{
    globus_object_t *				err = GLOBUS_NULL;
    globus_gass_transfer_http_request_proto_t *		proto;

    proto = (globus_gass_transfer_http_request_proto_t *) callback_arg;

    if(result != GLOBUS_SUCCESS)
    {
	err = globus_error_get(result);
    }

    globus_l_gass_transfer_http_lock();

    proto->response_offset += nbytes;

    if(result != GLOBUS_SUCCESS &&
       globus_io_eof(err))
    {
	proto->eof_read = GLOBUS_TRUE;
    }
    else if(result != GLOBUS_SUCCESS)
    {
	proto->recv_state = GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
    }

    /*
     * Copy the document from the response buffer to the user-supplied
     * buffer, translating end-of-line if necessary, and handling any
     * chunk header/footer information
     */
    globus_l_gass_transfer_http_handle_chunk(proto);

    if(proto->failure_occurred)
    {
	proto->recv_state = GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
    }

    /* successful read for server, send response */
    if((proto->type == GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT ||
       proto->type == GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND) &&
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF)
    {
	char  *					response;
	globus_size_t				response_len=0;
	globus_size_t				offset;


	response_len += 1;
	response_len += strlen(GLOBUS_L_GENERIC_RESPONSE);
	response_len += 3;
	response_len += strlen(GLOBUS_L_OK);
	response_len += strlen(CRLF);
	response = globus_malloc(response_len);

	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_RESPONDING;
	offset = sprintf(response,
		GLOBUS_L_GENERIC_RESPONSE,
		0,
		200,
		GLOBUS_L_OK);
	offset += sprintf(response + offset,
			  CRLF);

	globus_io_register_write(&proto->handle,
				 (globus_byte_t *) response,
				 strlen(response),
				 globus_l_gass_transfer_http_write_response,
				 proto);
    }
    /* Register the socket for closing if we're done reading from it */
    else if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF ||
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
	result =
	    globus_io_register_close(
		&proto->handle,
		globus_l_gass_transfer_http_close_callback,
		proto);
	if(result != GLOBUS_SUCCESS)
	{
	    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
	}
    }
    if(proto->user_waitlen == 0 ||
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF ||
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR)
    {
	globus_bool_t				last_data = GLOBUS_FALSE;
	/*
	 * Received the required minimum of data from connection, an
	 * error, or the end-of file, signal this to GASS
	 */
	if(proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_PENDING)
	{
	    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;
	}
	if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF ||
	   proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR)
	{
	    last_data = GLOBUS_TRUE;
	}

	globus_l_gass_transfer_http_unlock();
	globus_gass_transfer_proto_receive_complete(proto->request,
						    proto->user_buffer,
						    proto->user_offset,
						    proto->failure_occurred,
						    last_data);
    }
    else
    {
	result = globus_l_gass_transfer_http_register_read(proto);
    }

    if(result != GLOBUS_SUCCESS)
    {
        goto error_exit;
    }


    globus_l_gass_transfer_http_unlock();
    if(err)
    {
	globus_object_free(err);
    }

    return;

  error_exit:
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
    proto->failure_occurred = GLOBUS_TRUE;
		    
    result =
	globus_io_register_close(
	    &proto->handle,
	    globus_l_gass_transfer_http_close_callback,
	    proto);
    if(result != GLOBUS_SUCCESS)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
    }

    globus_l_gass_transfer_http_unlock();
    globus_gass_transfer_proto_receive_complete(proto->request,
						proto->user_buffer,
						proto->user_offset,
						GLOBUS_TRUE,
						GLOBUS_TRUE);
		    
    if(err)
    {
	globus_object_free(err);
    }
    return;
}
/* globus_l_gass_transfer_http_read_buffered_callback() */

/*
 * Function: globus_l_gass_transfer_http_close_callback()
 *
 * Description: Called upon completion of close()ing the file handle,
 *              Will free the proto instance if the client has called
 *              the "done" function
 *
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_close_callback(
    void *					callback_arg,
    globus_io_handle_t *			handle,
    globus_result_t				result)
{
    globus_gass_transfer_http_request_proto_t *		proto;


    proto = (globus_gass_transfer_http_request_proto_t *) callback_arg;
    globus_l_gass_transfer_http_lock();
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;

    if(proto->destroy_called)
    {
	globus_l_gass_transfer_http_proto_destroy(proto);
    }
    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_close_callback() */

/*
 * Function: globus_l_gass_transfer_http_listener_close_callback()
 *
 * Description:
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_listener_close_callback(
    void *				callback_arg,
    globus_io_handle_t *		handle,
    globus_result_t			result)
{
    globus_gass_transfer_http_listener_proto_t *
					proto;

    proto = (globus_gass_transfer_http_listener_proto_t *) callback_arg;

    globus_l_gass_transfer_http_lock();

    proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSED;

    if(proto->destroy_called)
    {
	globus_l_gass_transfer_http_listener_proto_destroy(proto);
    }
    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_listener_close_callback() */

/*
 * Function: globus_l_gass_transfer_http_listener_proto_destroy()
 *
 * Description:
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_listener_proto_destroy(
    globus_gass_transfer_http_listener_proto_t *
					proto)
{
    globus_free(proto);
}
/* globus_l_gass_transfer_http_listener_proto_destroy() */

/*
 * Function: globus_l_gass_transfer_http_destroy()
 *
 * Description:
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_destroy(
    globus_gass_transfer_request_proto_t *	proto,
    globus_gass_transfer_request_t		request)
{
    globus_gass_transfer_http_request_proto_t *		new_proto;

    new_proto = (globus_gass_transfer_http_request_proto_t *) proto;

    globus_l_gass_transfer_http_lock();
    if(new_proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING ||
       new_proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_REFERRED ||
       new_proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_RESPONDING ||
       new_proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_DENIED)
    {
	new_proto->destroy_called=GLOBUS_TRUE;
    }
    else if(new_proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE)
    {
	globus_l_gass_transfer_http_proto_destroy(new_proto);
    }
    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_destroy() */

/*
 * Function: globus_l_gass_transfer_http_listener_destroy()
 *
 * Description:
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_listener_destroy(
    globus_gass_transfer_listener_proto_t *	proto,
    globus_gass_transfer_listener_t		listener)
{
    globus_gass_transfer_http_listener_proto_t *new_proto;

    new_proto = (globus_gass_transfer_http_listener_proto_t *) proto;

    globus_l_gass_transfer_http_lock();
    if(new_proto->state != GLOBUS_GASS_TRANSFER_LISTENER_CLOSED)
    {
	new_proto->destroy_called=GLOBUS_TRUE;
    }
    else
    {
	globus_l_gass_transfer_http_listener_proto_destroy(new_proto);
    }
    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_listener_destroy() */

/*
 * Function: globus_l_gass_transfer_http_listen()
 *
 * Description:
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_listen(
    globus_gass_transfer_listener_proto_t *	proto,
    globus_gass_transfer_listener_t		listener)
{
    globus_gass_transfer_http_listener_proto_t *new_proto;
    globus_result_t				result;

    new_proto = (globus_gass_transfer_http_listener_proto_t *) proto;

    globus_l_gass_transfer_http_lock();

    result = globus_io_tcp_register_listen(
	&new_proto->handle,
	globus_l_gass_transfer_http_listen_callback,
	(void *) new_proto);
    
    if(result != GLOBUS_SUCCESS)
    {
	globus_callback_register_oneshot(
	    GLOBUS_NULL /* callback handle */,
	    (globus_time_t) 0,
	    globus_l_gass_transfer_http_callback_listen_callback,
	    (void *) new_proto,
	    GLOBUS_NULL /* wakeup func */,
	    GLOBUS_NULL /* wakeup arg */);
	
    }
    else
    {
	new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_LISTENING;
    }
    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_listen() */

static
void
globus_l_gass_transfer_http_listen_callback(
    void *					callback_arg,
    globus_io_handle_t *			handle,
    globus_result_t				result)
{
    globus_gass_transfer_http_listener_proto_t *proto;
    globus_gass_transfer_listener_t		listener;

    proto = (globus_gass_transfer_http_listener_proto_t *) callback_arg;
    globus_l_gass_transfer_http_lock();

    switch(proto->state)
    {
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_LISTENING:
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_READY;
	break;
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING1:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSED:
	break;
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_STARTING:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_ACCEPTING:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_READY:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING2:
	globus_assert(proto->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_STARTING);
	globus_assert(proto->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_ACCEPTING);
	globus_assert(proto->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_READY);
	globus_assert(proto->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING2);
    }

    listener = proto->listener;
    globus_l_gass_transfer_http_unlock();

    globus_gass_transfer_proto_listener_ready(listener);
}
/* globus_l_gass_transfer_http_listen_callback() */

static
void
globus_l_gass_transfer_http_accept_callback(
    void *					callback_arg,
    globus_io_handle_t *			handle,
    globus_result_t				result)
{
    globus_gass_transfer_http_listener_proto_t *l;

    globus_l_gass_transfer_http_lock();

    fflush(stdout);

    l = (globus_gass_transfer_http_listener_proto_t *) callback_arg;
    
    switch(l->state)
    {
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_ACCEPTING:
	l->request->response_buffer = globus_malloc(GLOBUS_L_GASS_RESPONSE_LEN *
							sizeof(globus_byte_t));
	l->request->response_buflen = GLOBUS_L_GASS_RESPONSE_LEN;
	l->request->response_offset = 0;
	l->request->parsed_offset = 0;

	if(result != GLOBUS_SUCCESS)
	{
	    globus_l_gass_transfer_http_unlock();
	    globus_l_gass_transfer_http_request_callback(
		l,
		&l->request->handle,
		result,
		l->request->response_buffer,
		0);
	    return;
	}
	else
	{
	    globus_io_register_read(&l->request->handle,
				    l->request->response_buffer,
				    l->request->response_buflen,
				    1,
				    globus_l_gass_transfer_http_request_callback,
				    l);
	}
	break;

      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING2:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSED:
	globus_l_gass_transfer_http_unlock();
	globus_gass_transfer_proto_new_listener_request(l->listener,
							l->request->request,
							GLOBUS_NULL);
	globus_l_gass_transfer_http_lock();
	/* should destroy the proto->request here? */
	break;
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_STARTING:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_LISTENING:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_READY:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING1:
	globus_assert(l->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_STARTING);
	globus_assert(l->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_LISTENING);
	globus_assert(l->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_READY);
	globus_assert(l->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING1);
    }
    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_accept_callback() */

static
void
globus_l_gass_transfer_http_request_refer(
    globus_gass_transfer_request_proto_t *	rproto,
    globus_gass_transfer_request_t		request)
{
    globus_gass_transfer_http_request_proto_t *	proto;
    globus_gass_transfer_referral_t 		referral;
    int						rc;
    char *					referral_string;
    globus_size_t				referral_count;
    globus_size_t				body_count=0; /* :) */
    globus_size_t				offset;
    globus_size_t				x;
    globus_size_t				i;
    globus_size_t				digits = 0;

    globus_l_gass_transfer_http_lock();
    proto = (globus_gass_transfer_http_request_proto_t *) rproto;

    rc = globus_gass_transfer_request_get_referral(request,
						   &referral);

    /* HTTP/1.1 302 Document Moved CRLF
     * Location: referral.url[0] CRLF
     * Content-Length: $body_count CRLF
     * Content-Type: text/html CRLF
     * CRLF
     * <html><head><title>300 Multiple Choices</title></head><body>
     * <a href="<referral.url[1]>">referral.url[1]</a><br>
     * ..
     * <a href="<referral.url[i]>">referral.url[i]</a><br>
     * </body></html>
     */
    referral_count = 1;
    referral_count += strlen(GLOBUS_L_REFER_RESPONSE);
    referral_count += strlen(GLOBUS_L_LOCATION_HEADER);
    referral_count += strlen(CRLF);
    referral_count += strlen(GLOBUS_L_CONTENT_LENGTH_HEADER);

    referral_count += strlen(GLOBUS_L_HTML_HEADER);

    referral_count += strlen(referral.url[0]);

    body_count += strlen(GLOBUS_L_HTML_REFERRAL_BODY_HEAD);
    body_count += strlen(GLOBUS_L_HTML_REFERRAL_BODY_TAIL);
    for(i = 0 ; i < referral.count; i++)
    {
	body_count += strlen(GLOBUS_L_HTML_HREF);
	body_count += strlen(referral.url[i]);
	body_count += strlen(referral.url[i]);
    }

    /* count the number of decimal digits in the body */
    x=body_count;
    do
    {
	digits++;
	x /= 10;
    } while(x > 0);

    referral_count += digits;

    referral_string = globus_malloc(referral_count + body_count);

    offset = sprintf(referral_string,
		     GLOBUS_L_REFER_RESPONSE);
    offset += sprintf(referral_string + offset,
		      GLOBUS_L_LOCATION_HEADER,
		      referral.url[0]);
    
    offset += sprintf(referral_string + offset,
		      GLOBUS_L_HTML_HEADER);
    offset += sprintf(referral_string + offset,
		      GLOBUS_L_CONTENT_LENGTH_HEADER,
		      (int) body_count);
    offset += sprintf(referral_string + offset,
		      CRLF);
    
    offset += sprintf(referral_string + offset,
		      GLOBUS_L_HTML_REFERRAL_BODY_HEAD);
    for(i = 0 ; i < referral.count; i++)
    {
	offset += sprintf(referral_string + offset,
			  GLOBUS_L_HTML_HREF,
			  referral.url[i],
			  referral.url[i]);
    }
    offset += sprintf(referral_string + offset,
		      GLOBUS_L_HTML_REFERRAL_BODY_TAIL);

    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_REFERRED;

    globus_gass_transfer_referral_destroy(&referral);

    globus_io_register_write(&proto->handle,
			     (globus_byte_t *) referral_string,
			     strlen(referral_string),
			     globus_l_gass_transfer_http_write_response,
			     proto);

    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_request_refer() */

static
void
globus_l_gass_transfer_http_request_deny(
    globus_gass_transfer_request_proto_t *	rproto,
    globus_gass_transfer_request_t		request)
{
    globus_gass_transfer_http_request_proto_t *	proto;
    char *					deny_string;
    globus_size_t				deny_count;
    globus_size_t				body_count=0; /* :) */
    globus_size_t				offset;
    globus_size_t				x;
    globus_size_t				digits = 0;
    int						reason;
    char *					message;

    globus_l_gass_transfer_http_lock();
    proto = (globus_gass_transfer_http_request_proto_t *) rproto;

    reason = globus_gass_transfer_request_get_denial_reason(request);

    if(reason < 400 ||
       reason >= 600)
    {
	reason = 500;
	message = globus_libc_strdup(GLOBUS_L_DEFAULT_DENIAL_MESSAGE);
    }
    else
    {
	message = globus_gass_transfer_request_get_denial_message(request);
	if(message == GLOBUS_NULL)
	{
	    message = globus_libc_strdup(GLOBUS_L_DEFAULT_DENIAL_MESSAGE);
	}
    }

    /* HTTP/1.1 %d %s CRLF
     * Content-Length: $body_count CRLF
     * Content-Type: text/html CRLF
     * CRLF
     * <html><head><title>%d %s</title></head><body> CRLF
     * %d %s</title></body></html> CRLF
     */
    
    deny_count = 1;
    deny_count += strlen(GLOBUS_L_DENIAL_RESPONSE);
    deny_count += 3 ; /* code */
    deny_count += strlen(message);
    deny_count += strlen(GLOBUS_L_CONTENT_LENGTH_HEADER);
    deny_count += strlen(GLOBUS_L_HTML_HEADER);
    deny_count += strlen(CRLF);

    body_count += strlen(GLOBUS_L_HTML_DENIAL_BODY);
    body_count += (strlen(message) * 3);
    body_count += (3 * 3); /* code */

    /* count the number of decimal digits in the body */
    x=body_count;
    do
    {
	digits++;
	x /= 10;
    } while(x > 0);

    deny_count += digits;

    deny_string = globus_malloc(deny_count + body_count);

    offset = sprintf(deny_string,
		     GLOBUS_L_DENIAL_RESPONSE,
		     reason,
		     message);
    offset += sprintf(deny_string + offset,
		      GLOBUS_L_HTML_HEADER);
    offset += sprintf(deny_string + offset,
		      GLOBUS_L_CONTENT_LENGTH_HEADER,
		      (int) body_count);
    offset += sprintf(deny_string + offset,
		      CRLF);
    
    offset += sprintf(deny_string + offset,
		      GLOBUS_L_HTML_DENIAL_BODY,
		      reason,
		      message,
		      reason,
		      message);

    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DENIED;

    globus_free(message);

    globus_io_register_write(&proto->handle,
			     (globus_byte_t *) deny_string,
			     strlen(deny_string),
			     globus_l_gass_transfer_http_write_response,
			     proto);

    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_request_deny() */

static
void
globus_l_gass_transfer_http_request_authorize(
    globus_gass_transfer_request_proto_t *	rproto,
    globus_gass_transfer_request_t		request)
{
    globus_gass_transfer_http_request_proto_t *	proto;
    char *					authorize_string;
    globus_size_t				authorize_count=0;
    globus_size_t				offset;
    globus_size_t				length;

    globus_l_gass_transfer_http_lock();
    proto = (globus_gass_transfer_http_request_proto_t *) rproto;

    switch(proto->type)
    {
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_GET:
	/* Let's always send an HTTP/1.0 response, to make things easier */
	length = globus_gass_transfer_request_get_length(proto->request);

	if(length != 0)
	{
	    globus_size_t			x = length;
	    globus_size_t			digits = 0;

	    /* count the number of decimal digits in length */
	    do
	    {
		digits++;
		x /= 10;
	    } while(x > 0);
	    
	    /* Send a content-length field */
	    authorize_count += strlen(GLOBUS_L_CONTENT_LENGTH_HEADER);
	    authorize_count += digits;
        }
	    
	authorize_count += 1;
	authorize_count += strlen(GLOBUS_L_GENERIC_RESPONSE);
	authorize_count += 3;
	authorize_count += strlen(GLOBUS_L_OK);
	authorize_count += strlen(CRLF);

	if(proto->text_mode)
	{
	    authorize_count += strlen(GLOBUS_L_TEXT_HEADER);
	    authorize_string = globus_malloc(authorize_count);
	    offset = sprintf(authorize_string,
			     GLOBUS_L_GENERIC_RESPONSE,
			     0,
			     200,
			     GLOBUS_L_OK);
	    offset += sprintf(authorize_string + offset,
			      GLOBUS_L_TEXT_HEADER);
	}
	else
	{
	    authorize_count += strlen(GLOBUS_L_BINARY_HEADER);
	    authorize_string = globus_malloc(authorize_count);
	    offset = sprintf(authorize_string,
			     GLOBUS_L_GENERIC_RESPONSE,
			     0,
			     200,
			     GLOBUS_L_OK);
	    offset += sprintf(authorize_string + offset,
			      GLOBUS_L_BINARY_HEADER);
	}

	if(length != 0)
	{
	    offset += sprintf(authorize_string + offset,
			      GLOBUS_L_CONTENT_LENGTH_HEADER,
			      length);
	}
	offset += sprintf(authorize_string + offset,
			  CRLF);
	break;
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT:
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND:
	/* send nothing back yet */
	break;
      default:
	globus_assert(GLOBUS_FALSE);
    }

    if(authorize_count != 0)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_RESPONDING;

	globus_io_register_write(&proto->handle,
				 (globus_byte_t *) authorize_string,
				 strlen(authorize_string),
				 globus_l_gass_transfer_http_write_response,
				 proto);
    }
    else
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;

	globus_callback_register_oneshot(
	    GLOBUS_NULL /* callback handle */,
	    (globus_time_t) 0,
	    globus_l_gass_transfer_http_callback_ready_callback,
	    (void *) proto,
	    GLOBUS_NULL /* wakeup func */,
	    GLOBUS_NULL /* wakeup arg */);

    }

    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_request_authorize() */

static
void
globus_l_gass_transfer_http_write_response(
    void *				arg,
    globus_io_handle_t *		handle,
    globus_result_t			result,
    globus_byte_t *			buf,
    globus_size_t			nbytes)
{
    globus_gass_transfer_http_request_proto_t *
					proto;
    globus_free(buf);

    globus_l_gass_transfer_http_lock();

    proto = (globus_gass_transfer_http_request_proto_t *) arg;

    switch(proto->state)
    {
      case GLOBUS_GASS_TRANSFER_HTTP_STATE_RESPONDING:
	if(proto->type == GLOBUS_GASS_TRANSFER_REQUEST_TYPE_GET)
	{
	    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;
	    globus_l_gass_transfer_http_unlock();
	    
	    globus_gass_transfer_proto_request_ready(proto->request,
						     (globus_gass_transfer_request_proto_t *) proto);
	    globus_l_gass_transfer_http_unlock();
	    return;
	}
	/* other types fall through */
      default:
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
	result = globus_io_register_close(
	    &proto->handle,
	    globus_l_gass_transfer_http_close_callback,
	    proto);

	if(result != GLOBUS_SUCCESS)
	{
	    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
	    globus_l_gass_transfer_http_unlock();
	    globus_l_gass_transfer_http_close_callback(proto,
						       &proto->handle,
						       GLOBUS_SUCCESS);
	    return;
	}
	else
	{
	    globus_l_gass_transfer_http_unlock();
	    return;
	}
    }
}
/* globus_l_gass_transfer_http_write_response() */

static
globus_bool_t
globus_l_gass_transfer_http_authorization_callback(
    void *					arg,
    globus_io_handle_t *			handle,
    globus_result_t				result,
    char *					identity,
    gss_ctx_id_t *				context_handle)
{
    globus_gass_transfer_http_listener_proto_t *proto;
    int						rc;

    globus_l_gass_transfer_http_lock();
    proto = (globus_gass_transfer_http_listener_proto_t *) arg;

    proto->request->connected_subject = globus_libc_strdup(identity);

    switch(proto->request->authorization_mode)
    {
      case GLOBUS_GASS_TRANSFER_AUTHORIZE_SELF:
	if(strcmp(identity, globus_l_gass_transfer_http_subject_name) == 0)
	{
	    rc = GLOBUS_TRUE;
	    goto finish;
	}
	else
	{
	    rc = GLOBUS_FALSE;
	    goto finish;
	}
      case GLOBUS_GASS_TRANSFER_AUTHORIZE_HOST:
	rc = GLOBUS_FALSE;
	goto finish;
      case GLOBUS_GASS_TRANSFER_AUTHORIZE_SUBJECT:
	if(strcmp(identity, proto->request->authorized_subject) == 0)
	{
	    rc = GLOBUS_TRUE;
	    goto finish;
	}
	else
        {
	    rc = GLOBUS_FALSE;
	    goto finish;
	}
      case GLOBUS_GASS_TRANSFER_AUTHORIZE_CALLBACK:
	rc = GLOBUS_TRUE;
	goto finish;
    }

  finish:
    globus_l_gass_transfer_http_unlock();

    return rc;
}
/* globus_l_gass_transfer_http_authorization_callback() */

static
void
globus_l_gass_transfer_http_proto_destroy(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    if(proto->response_buffer != GLOBUS_NULL)
    {
	globus_free(proto->response_buffer);
    }
    if(proto->reason != GLOBUS_NULL)
    {
	globus_free(proto->reason);
    }
    if(proto->client_side)
    {
	globus_url_destroy(&proto->url);
    }
    else
    {
	if(proto->method)
	{
	    globus_free(proto->method);
	}
	if(proto->uri)
	{
	    globus_free(proto->uri);
	}
    }
    globus_i_gass_transfer_keyvalue_destroy(
	&proto->headers);
    globus_free(proto);
}
/* globus_l_gass_transfer_http_proto_destroy() */

/*
 * Function: globus_l_gass_transfer_http_new_request(()
 *
 * Description: Create a new request's "proto" structure
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_new_request(
    globus_gass_transfer_request_t		request,
    globus_gass_transfer_requestattr_t *	attr)
{
    int						rc=GLOBUS_SUCCESS;
    char *					proxy=GLOBUS_NULL;
    globus_gass_transfer_file_mode_t		file_mode=GLOBUS_GASS_TRANSFER_FILE_MODE_BINARY;
    globus_io_attr_t				tcp_attr;
    globus_result_t				result;
    globus_gass_transfer_http_request_proto_t *		proto;
    int						sndbuf;
    int						rcvbuf;
    int						nodelay;

    switch(globus_gass_transfer_request_get_type(request))
    {
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_GET:
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT:
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND:
	break;
      default:
	goto error_exit;
    }

    /* Allocate proto instance */
    proto = (globus_gass_transfer_http_request_proto_t *) 
	globus_malloc(sizeof(globus_gass_transfer_http_request_proto_t));

    if(proto == GLOBUS_NULL)
    {
	goto error_exit;
    }

    result = globus_io_tcpattr_init(&tcp_attr);
    if(result != GLOBUS_SUCCESS)
    {
	goto proto_error;
    }

    if(*attr != GLOBUS_NULL)
    {
	/* Check attributes we care about */
	globus_gass_transfer_requestattr_get_proxy_url(attr,
						       &proxy);

	rc = globus_gass_transfer_requestattr_get_socket_sndbuf(
	    attr,
	    &sndbuf);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	else
	{
	    if(sndbuf != 0)
	    {
		globus_io_attr_set_socket_sndbuf(&tcp_attr,
						 sndbuf);
	    }
	}

	rc = globus_gass_transfer_requestattr_get_socket_rcvbuf(
	    attr,
	    &rcvbuf);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	else
	{
	    if(rcvbuf != 0)
	    {
		globus_io_attr_set_socket_rcvbuf(&tcp_attr,
						 rcvbuf);
	    }
	}

	rc = globus_gass_transfer_requestattr_get_socket_nodelay(
	    attr,
	    &nodelay);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	else
	{
	    globus_io_attr_set_tcp_nodelay(&tcp_attr,
					   nodelay);
	}

	/* File mode is important on Windows */
	rc = globus_gass_transfer_requestattr_get_file_mode(attr,
							    &file_mode);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	rc = globus_gass_transfer_requestattr_get_block_size(attr,
							     &proto->block_size);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
    }

    /* Verify URL */
    if(proxy)
    {
	rc = globus_url_parse(proxy,
			      &proto->proxy_url);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	if(strcmp(proto->proxy_url.scheme, "http") != 0 &&
	   strcmp(proto->proxy_url.scheme, "https") != 0)
	{
	    goto proxy_error;
	}
    }
    proto->url_string = globus_gass_transfer_request_get_url(request);

    rc = globus_url_parse(proto->url_string,
			  &proto->url);
    if(rc != GLOBUS_SUCCESS)
    {
	goto proxy_error;
    }
    if(proto->url.url_path == GLOBUS_NULL)
    {
	proto->url.url_path = globus_libc_strdup("/");
    }
    if(strcmp(proto->url.scheme, "http") != 0 &&
       strcmp(proto->url.scheme, "https") != 0)
    {
	goto url_error;
    }


    /* If https, set security attributes of TCP handle */
    if(strcmp(proto->url.scheme, "https")== 0)
    {
	globus_io_secure_authorization_data_t	data;
	globus_gass_transfer_authorization_t	mode;
	char *					subject;
	globus_result_t				result;

	
	globus_io_secure_authorization_data_initialize(&data);
	result = globus_io_attr_set_secure_authentication_mode(
	    &tcp_attr,
	    GLOBUS_IO_SECURE_AUTHENTICATION_MODE_GSSAPI,
	    GLOBUS_NULL);

	if(result != GLOBUS_SUCCESS)
	{
	    goto url_error;
	}
	result = globus_io_attr_set_secure_channel_mode(
	    &tcp_attr,
	    GLOBUS_IO_SECURE_CHANNEL_MODE_SSL_WRAP);
	if(result != GLOBUS_SUCCESS)
	{
	    goto url_error;
	}
	rc = globus_gass_transfer_secure_requestattr_get_authorization(
	    attr,
	    &mode,
	    &subject);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto url_error;
	}

	switch(mode)
	{
	  case GLOBUS_GASS_TRANSFER_AUTHORIZE_SELF:
	    globus_io_attr_set_secure_authorization_mode(
		&tcp_attr,
		GLOBUS_IO_SECURE_AUTHORIZATION_MODE_SELF,
		GLOBUS_NULL);
	    break;
	  case GLOBUS_GASS_TRANSFER_AUTHORIZE_HOST:
	    subject = globus_malloc(strlen(proto->url.host) +
				     strlen("/CN=")
				     +1);
	    sprintf(subject,
		    "/CN=%s",
		    proto->url.host);

	    globus_io_secure_authorization_data_set_identity(
		&data,
		subject);
	    globus_io_attr_set_secure_authorization_mode(
		&tcp_attr,
		GLOBUS_IO_SECURE_AUTHORIZATION_MODE_IDENTITY,
		&data);
	    globus_io_secure_authorization_data_destroy(&data);
	    globus_free(subject);
	    break;
	  case GLOBUS_GASS_TRANSFER_AUTHORIZE_SUBJECT:
	    globus_io_secure_authorization_data_set_identity(
		&data,
		subject);
	    globus_io_attr_set_secure_authorization_mode(
		&tcp_attr,
		GLOBUS_IO_SECURE_AUTHORIZATION_MODE_IDENTITY,
		&data);
	    globus_io_secure_authorization_data_destroy(&data);
	    break;
	  case GLOBUS_GASS_TRANSFER_AUTHORIZE_CALLBACK:
	    globus_assert(mode != GLOBUS_GASS_TRANSFER_AUTHORIZE_CALLBACK);
	    goto url_error;
	}
    }

    if(proto == GLOBUS_NULL)
    {
	goto url_error;
    }

    /* Initialize the proto instance */
    proto->send_buffer	= globus_l_gass_transfer_http_send;
    proto->recv_buffer	= globus_l_gass_transfer_http_receive;
    proto->fail		= globus_l_gass_transfer_http_fail;
    proto->deny		= GLOBUS_NULL;
    proto->refer	= GLOBUS_NULL;
    proto->authorize	= GLOBUS_NULL;
    proto->destroy	= globus_l_gass_transfer_http_destroy;
    proto->text_mode	= (file_mode == GLOBUS_GASS_TRANSFER_FILE_MODE_TEXT);
    proto->line_mode	= GLOBUS_L_LINE_MODE_UNKNOWN;
    proto->state	= GLOBUS_GASS_TRANSFER_HTTP_STATE_CONNECTING;
    proto->request	= request;
    proto->type		= globus_gass_transfer_request_get_type(request);
    proto->code		= 0;
    proto->reason	= 0;
    proto->parse_error	= GLOBUS_FALSE;
    proto->destroy_called = GLOBUS_FALSE;
    proto->headers	= GLOBUS_NULL;
    proto->response_buffer = GLOBUS_NULL;
    proto->length	= 0;
    proto->handled	= 0;
    proto->chunked	= GLOBUS_FALSE;
    proto->chunk_left	= 0;
    proto->failure_occurred = GLOBUS_FALSE;
    proto->eof_read	= GLOBUS_FALSE;
    proto->client_side	= GLOBUS_TRUE;
    proto->connected_subject	= GLOBUS_NULL;

    /* Open the handle */
#   if 0
    if(proxy)
    {
	if(proto->proxy_url.scheme_type == GLOBUS_URL_SCHEME_HTTP &&
	   proto->proxy_url.port == 0)
	{
	    proto->proxy_url.port = GLOBUS_L_DEFAULT_HTTP_PORT;
	}
	else if(proto->proxy_url.scheme_type == GLOBUS_URL_SCHEME_HTTPS &&
		proto->proxy_url.port == 0)
	{
	    proto->proxy_url.port = GLOBUS_L_DEFAULT_HTTPS_PORT;
	}
	result = globus_io_tcp_register_connect(
	    proto->proxy_url.host,
	    proto->proxy_url.port,
	    &tcp_attr,
	    globus_l_gass_transfer_http_proxy_callback,
	    proto,
	    &proto->handle);
    }
    else
#   endif
    {
	if(proto->url.scheme_type == GLOBUS_URL_SCHEME_HTTP &&
	   proto->url.port == 0)
	{
	    proto->url.port = GLOBUS_L_DEFAULT_HTTP_PORT;
	}
	else if(proto->url.scheme_type == GLOBUS_URL_SCHEME_HTTPS &&
		proto->url.port == 0)
	{
	    proto->url.port = GLOBUS_L_DEFAULT_HTTPS_PORT;
	}
	result = globus_io_tcp_register_connect(
	    proto->url.host,
	    proto->url.port,
	    &tcp_attr,
	    globus_l_gass_transfer_http_connect_callback,
	    proto,
	    &proto->handle);
    }
    if(proxy)
    {
	globus_url_destroy(&proto->proxy_url);
    }

    if(result != GLOBUS_SUCCESS)
    {
	goto url_error;
    }
    /* Success! */
    globus_io_tcpattr_destroy(&tcp_attr);
    return ;

  url_error:
    globus_url_destroy(&proto->url);
  proxy_error:
    if(proxy)
    {
	globus_url_destroy(&proto->proxy_url);
    }
  tcpattr_error:
    globus_io_tcpattr_destroy(&tcp_attr);
  proto_error:
    globus_free(proto);
  error_exit:
    /* should be a oneshot */
    globus_gass_transfer_proto_request_denied(
	request,
	GLOBUS_L_DEFAULT_FAILURE_CODE,
	globus_libc_strdup(GLOBUS_L_DEFAULT_FAILURE_REASON));
}
/* globus_l_gass_transfer_http_new_request() */

/*
 * Function: globus_l_gass_transfer_http_new_requestattr()
 *
 * Description: Create a new request attribute structure,
 *              appropriate for the "http" url scheme
 *
 * Parameters:
 *
 * Returns:
 */
static
globus_object_t *
globus_l_gass_transfer_http_new_requestattr(
    char *                                      url_scheme)
{
    globus_object_t *				obj;

    if(strcmp(url_scheme, "https") == 0)
    {
	obj = globus_object_construct(GLOBUS_GASS_OBJECT_TYPE_SECURE_REQUESTATTR);

	return
	    globus_gass_transfer_secure_requestattr_initialize(
		obj,
		GLOBUS_NULL,
		0,
		GLOBUS_GASS_TRANSFER_FILE_MODE_BINARY,
		GLOBUS_FALSE,
		0,
		0,
		GLOBUS_FALSE,
		GLOBUS_GASS_TRANSFER_AUTHORIZE_HOST,
		GLOBUS_NULL);
    }
    else if(strcmp(url_scheme, "http") == 0)
    {
	obj = globus_object_construct(GLOBUS_GASS_OBJECT_TYPE_SOCKET_REQUESTATTR);

	return
	    globus_gass_transfer_socket_requestattr_initialize(
		obj,
		GLOBUS_NULL,
		0,
		GLOBUS_GASS_TRANSFER_FILE_MODE_BINARY,
		GLOBUS_FALSE,
		0,
		0,
		GLOBUS_FALSE);
    }
    else
    {
	return GLOBUS_NULL;
    }
}
/* globus_l_gass_transfer_http_new_requestattr() */

/*
 * Function: globus_l_gass_transfer_http_new_listenerattr()
 *
 * Description: Create a new listener attribute structure,
 *              appropriate for the "http" url scheme
 *
 * Parameters:
 *
 * Returns:
 */
static
globus_object_t *
globus_l_gass_transfer_http_new_listenerattr(
    char *                                      url_scheme)
{
    globus_object_t *				obj;

    if(strcmp(url_scheme, "https") == 0 ||
       strcmp(url_scheme, "http") == 0)
    {
	obj = globus_object_construct(GLOBUS_GASS_OBJECT_TYPE_LISTENERATTR);

	return
	    globus_gass_transfer_listenerattr_initialize(
		obj,
		-1,
		0);
    }
    else
    {
	return GLOBUS_NULL;
    }
}
/* globus_l_gass_transfer_http_new_listenerattr() */

/*
 * Function: globus_l_gass_transfer_http_new_listener()
 *
 * Description: Create a new listener structure,
 *              appropriate for the "http" and "https" url schemes
 *
 * Parameters:
 *
 * Returns:
 */
static
int
globus_l_gass_transfer_http_new_listener(
    globus_gass_transfer_listener_t		listener,
    globus_gass_transfer_listenerattr_t *	attr,
    char *					scheme,
    char **					base_url,
    globus_gass_transfer_listener_proto_t **	ret_proto)
{
    globus_gass_transfer_http_listener_proto_t *
						proto;
    globus_io_attr_t				tcpattr;
    globus_io_secure_authorization_data_t	data;
    globus_result_t				result;
    int						rc;
    unsigned short				port=0;
    int						backlog=-1;
    char					hostname[MAXHOSTNAMELEN];
    globus_size_t				url_size;

    globus_io_tcpattr_init(&tcpattr);

    /* Allocate proto instance */
    proto = (globus_gass_transfer_http_listener_proto_t *)
	globus_malloc(sizeof(globus_gass_transfer_http_listener_proto_t));

    if(proto == GLOBUS_NULL)
    {
	goto free_tcpattr;
    }

    proto->close_listener = globus_l_gass_transfer_http_close_listener;
    proto->listen = globus_l_gass_transfer_http_listen;
    proto->accept = globus_l_gass_transfer_http_accept;
    proto->destroy = globus_l_gass_transfer_http_listener_destroy;

    proto->listener = listener;
    if(strcmp(scheme, "http") == 0)
    {
	proto->url_scheme = GLOBUS_URL_SCHEME_HTTP;
    }
    else if(strcmp(scheme, "https") == 0)
    {
	result = globus_io_attr_set_secure_authentication_mode(
	    &tcpattr,
	    GLOBUS_IO_SECURE_AUTHENTICATION_MODE_GSSAPI,
	    GLOBUS_NULL);
	if(result != GLOBUS_SUCCESS)
	{
	    goto free_proto;
	}
	
	result = globus_io_attr_set_secure_channel_mode(
	    &tcpattr,
	    GLOBUS_IO_SECURE_CHANNEL_MODE_SSL_WRAP);

	if(result != GLOBUS_SUCCESS)
	{
	    goto free_proto;
	}
	result = globus_io_secure_authorization_data_initialize(&data);

	if(result != GLOBUS_SUCCESS)
	{
	    goto free_auth_data;
	}

	result = globus_io_secure_authorization_data_set_callback(
	    &data,
	    globus_l_gass_transfer_http_authorization_callback,
	    proto);
	    
	if(result != GLOBUS_SUCCESS)
	{
	    goto free_auth_data;
	}

	result = globus_io_attr_set_secure_authorization_mode(
	    &tcpattr,
	    GLOBUS_IO_SECURE_AUTHORIZATION_MODE_CALLBACK,
	    &data);

	if(result != GLOBUS_SUCCESS)
	{
	    goto free_auth_data;
	}

	globus_io_secure_authorization_data_destroy(&data);

	proto->url_scheme = GLOBUS_URL_SCHEME_HTTPS;
    }
    else
    {
	goto free_proto;
    }

    if(attr)
    {
	rc = globus_gass_transfer_listenerattr_get_port(attr,
							&port);

	if(rc != GLOBUS_SUCCESS)
	{
	    goto free_proto;
	}

	rc = globus_gass_transfer_listenerattr_get_backlog(attr,
							   &backlog);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto free_proto;
	}

    }
    result = globus_io_tcp_create_listener(&port,
					   backlog,
					   &tcpattr,
					   &proto->handle);
    globus_io_tcpattr_destroy(&tcpattr);
    if(result != GLOBUS_SUCCESS)
    {
	goto free_proto;
    }

    url_size = 15; /* https://:65536\0 */
    globus_libc_gethostname(hostname,
			    MAXHOSTNAMELEN);
    url_size += strlen(hostname);

    *base_url = globus_malloc(url_size);
    sprintf(*base_url,
	    "%s://%s:%d",
	    proto->url_scheme == GLOBUS_URL_SCHEME_HTTPS ?
	    "https" : "http",
	    hostname,
	    (int) port);


    proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_STARTING;
    *ret_proto = (globus_gass_transfer_listener_proto_t *) proto;
    return GLOBUS_SUCCESS;

  free_auth_data:
    globus_io_secure_authorization_data_destroy(&data);
  free_proto:
    globus_free(proto);
  free_tcpattr:
    globus_io_tcpattr_destroy(&tcpattr);

    return GLOBUS_FAILURE;
}
/* globus_l_gass_transfer_http_new_listener() */

/*
 * Function: globus_l_gass_transfer_http_close_listener(()
 *
 * Description: Called by the GASS system when the user has
 *              requested that a listener be closed. This
 *              may be called at any time, but only once
 *		per listener.
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_close_listener(
    globus_gass_transfer_listener_proto_t *	proto,
    globus_gass_transfer_listener_t		listener)
{
    globus_gass_transfer_http_listener_proto_t *
						new_proto;
    globus_result_t				result;

    new_proto = (globus_gass_transfer_http_listener_proto_t *) proto;

    globus_l_gass_transfer_http_lock();

    switch(new_proto->state)
    {
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_STARTING:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_READY:
	/*
	 * If the listener is in the "idle" or "ready" state, then we can simply
	 * register the close, which will free the proto. (GASS is not waiting
	 * for a callback now.
	 */
	result = globus_io_register_close(
	    &new_proto->handle,
	    globus_l_gass_transfer_http_listener_close_callback,
	    new_proto);

	if(result != GLOBUS_SUCCESS)
	{
	    new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSED;
	    globus_l_gass_transfer_http_lock();
	    globus_l_gass_transfer_http_listener_close_callback(
		new_proto,
		&new_proto->handle,
		GLOBUS_SUCCESS);
	    return;
	}
	break;
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_LISTENING:
	/*
	 * If we are in the "listening" state, registering the
	 * close will cause the listen callback to finish, and
	 * after that calls the user, the close callback will delete
	 * things
	 */
	new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING1;

	globus_io_register_close(
	    &new_proto->handle,
	    globus_l_gass_transfer_http_listener_close_callback,
	    new_proto);
	
	break;
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_ACCEPTING:
	/*
	 * If we are in the "accepting" state, registering the
	 * close will cause any outstanding listen callbacks to finish,
	 * from where we can call back to GASS.
	 */
	new_proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING2;
	result = globus_io_register_close(
	    &new_proto->handle,
	    globus_l_gass_transfer_http_listener_close_callback,
	    new_proto);
	
	break;
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING1:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING2:
      case GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSED:
	/* should not happen */
	globus_assert(new_proto->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING1);
	globus_assert(new_proto->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSING2);
	globus_assert(new_proto->state != GLOBUS_GASS_TRANSFER_HTTP_LISTENER_CLOSED);
	break;
    }

    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_close_listener() */

/*
 * Function: globus_l_gass_transfer_http_connect_callback(()
 *
 * Description: Called upon completion of the TCP connect
 *              protocol (and the SSL authentication, if
 *              applicable).
 *
 *              The request string is sent to the server
 *              for processing.
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_connect_callback(
    void *					arg,
    globus_io_handle_t *			handle,
    globus_result_t				result)
{
    char *					cmd;
    globus_gass_transfer_http_request_proto_t *		proto;

    /*
     * In this function, we have completed the TCP (and SSL)
     * connection protocol, and will send our request to the
     * server for processing. 
     */
    proto = (globus_gass_transfer_http_request_proto_t *)
	arg;

    globus_l_gass_transfer_http_lock();

    globus_assert(proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_CONNECTING);

    if(result != GLOBUS_SUCCESS)
    {
	/*
	 * TODO: Evaluate error object here, put into
	 * proto->code and proto->reason
	 */
	goto deny_exit;
    }

    cmd = globus_l_gass_transfer_http_construct_request(proto);

    if(cmd == GLOBUS_NULL)
    {
	goto deny_exit;
    }
    
    /* Send our command to the server */
    result = globus_io_register_write(
	&proto->handle,
	(globus_byte_t *) cmd,
	strlen(cmd) * sizeof(char),
	globus_l_gass_transfer_http_command_callback,
	proto);

    if(result == GLOBUS_SUCCESS)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_REQUESTING;
	globus_l_gass_transfer_http_unlock();
	return;
    }

    /* Write registration failed, fall through to deny_exit */
  deny_exit:
    /* Give a default error message, if none is generated above */
    if(proto->code == 0)
    {
	proto->code = GLOBUS_L_DEFAULT_FAILURE_CODE;
	proto->reason = globus_libc_strdup(GLOBUS_L_DEFAULT_FAILURE_REASON);
    }
    /* Close up the request */
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
    /*
     * Because the proto is not being returned in a request ready,
     * we must not wait for the GASS system to call the destroyed
     * method of the proto
     */
    proto->destroy_called=GLOBUS_TRUE;
    globus_l_gass_transfer_http_unlock();
    /* Signal Denial to the GASS system */
    globus_gass_transfer_proto_request_denied(
	proto->request,
	proto->code,
	proto->reason);
    globus_l_gass_transfer_http_lock();
    result = globus_io_register_close(
	&proto->handle,
	globus_l_gass_transfer_http_close_callback,
	proto);
    if(result != GLOBUS_SUCCESS)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
	globus_l_gass_transfer_http_unlock();
	globus_l_gass_transfer_http_close_callback(proto,
						   &proto->handle,
						   GLOBUS_SUCCESS);
	return;
    }
    else
    {
	globus_l_gass_transfer_http_unlock();
	return;
    }
}
/* globus_l_gass_transfer_http_connect_callback() */

/*
 * Function: globus_l_gass_transfer_http_command_callback(()
 *
 * Description: Called when the HTTP request has been sent
 *              to the server for processing.
 *
 *              The request string is sent to the server
 *              for processing.
 *
 * Parameters:
 *
 * Returns:
 */
static
void
globus_l_gass_transfer_http_command_callback(
    void *				arg, 
    globus_io_handle_t *		handle, 
    globus_result_t			result,
    globus_byte_t *			buf,
    globus_size_t			nbytes)
{
    globus_gass_transfer_http_request_proto_t * proto;
    /*
     * In this function, we have completed sending our request
     * to the server. If there was some sort of error, we
     * will signal a "deny" to the server.
     */
    proto = (globus_gass_transfer_http_request_proto_t *) arg;

    globus_l_gass_transfer_http_lock();

    globus_assert(proto->state == GLOBUS_GASS_TRANSFER_HTTP_STATE_REQUESTING);

    if(result != GLOBUS_SUCCESS)
    {
	goto deny_exit;
    }
    /*
     * Free the request command buffer.
     */
    globus_free(buf);

    /*
     * We will now register a response handling buffer & callback
     * The buffer may have to grow if there are a lot of headers.
     */
    proto->response_buffer = globus_malloc(GLOBUS_L_GASS_RESPONSE_LEN *
					   sizeof(globus_byte_t));
    proto->response_buflen = GLOBUS_L_GASS_RESPONSE_LEN;
    proto->response_offset = 0;
    proto->parsed_offset = 0;

    switch(proto->type)
    {
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT:
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND:
	/*
	 * If we are a "put" or "append" request, then we can start
	 * sending data sort of immediately. So we send an "ready"
	 * message to the server. A little optimistic, perhaps...
	 * We will not register for the response message until
	 * we've sent the last data.
	 */
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;

	globus_l_gass_transfer_http_unlock();

	globus_gass_transfer_proto_request_ready(
	    proto->request,
	    (globus_gass_transfer_request_proto_t *) proto);

	return;
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_GET:
	/*
	 * If we are a "GET" request we can't do anything
	 * until the server sends us some info, anyway
	 */
	result = globus_io_register_read(
	    &proto->handle,
	    proto->response_buffer,
	    proto->response_buflen,
	    1,
	    globus_l_gass_transfer_http_response_callback,
	    proto);
	if(result != GLOBUS_SUCCESS)
	{
	    /* should interpret the error object */
	    goto deny_exit;
	}
	globus_l_gass_transfer_http_unlock();
	return;
      default:
	globus_assert(proto->type !=
		      GLOBUS_GASS_TRANSFER_REQUEST_TYPE_INVALID);
	goto deny_exit;
    }

  deny_exit:
    /* Give a default error message, if none is generated above */
    if(proto->code == 0)
    {
	proto->code = GLOBUS_L_DEFAULT_FAILURE_CODE;
	proto->reason = globus_libc_strdup(GLOBUS_L_DEFAULT_FAILURE_REASON);
    }
    globus_l_gass_transfer_http_unlock();

    /* Signal Denial to the GASS system */
    globus_gass_transfer_proto_request_denied(
	proto->request,
	proto->code,
	proto->reason);

    globus_l_gass_transfer_http_lock();
    /* Close up the request */
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
    result = globus_io_register_close(
	&proto->handle,
	globus_l_gass_transfer_http_close_callback,
	proto);
    if(result != GLOBUS_SUCCESS)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
	globus_l_gass_transfer_http_close_callback(proto,
						   &proto->handle,
						   GLOBUS_SUCCESS);
    }
    globus_l_gass_transfer_http_unlock();
}
/* globus_l_gass_transfer_http_command_callback() */
    
static
void
globus_l_gass_transfer_http_response_callback(
    void *					arg,
    globus_io_handle_t *			handle,
    globus_result_t				result,
    globus_byte_t *				buf,
    globus_size_t				nbytes)
{
    globus_gass_transfer_http_request_proto_t *		proto;
    globus_object_t *				err=GLOBUS_NULL;

    proto = (globus_gass_transfer_http_request_proto_t *) arg;
    if(result != GLOBUS_SUCCESS)
    {
	err = globus_error_get(result);
    }

    globus_l_gass_transfer_http_lock();

    /* Did the read succeed? */
    if(result != GLOBUS_SUCCESS &&
       !globus_io_eof(err))
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;

	result = globus_io_register_close(
	    &proto->handle,
	    globus_l_gass_transfer_http_close_callback,
	    proto);

	if(result != GLOBUS_SUCCESS)
	{
	    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
	}

	globus_l_gass_transfer_http_unlock();

	/* TODO: Evaluate error object, or response from the server here */
	globus_gass_transfer_proto_request_denied(
	    proto->request,
	    GLOBUS_L_DEFAULT_FAILURE_CODE,
	    globus_libc_strdup(GLOBUS_L_DEFAULT_FAILURE_REASON));

	if(err)
	{
	    globus_object_free(err);
	}

	return;
    }

    /* Update our counters */
    proto->response_offset += nbytes;

    do
    {
	/* Parse the buffer that we have */
	if(globus_l_gass_transfer_http_parse_response(proto))
	{
	    /* returns true, if we need to read some more */
	    goto repost_read;
	}

	switch(proto->type)
	{
	  case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_GET:
	    /* If it is a GET request, we need an affirmative
	     * response, or a continue, or a referral
	     */
	    if(proto->code < 100 ||
	       proto->code >= 400)
	    {
		/* Denied */
		goto deny_exit;
	    }
	    else if(proto->code >= 100 &&
		    proto->code < 200)
	    {
		/*
		 * Continue... Reset our response buffers,
		 * and get another set of headers
		 */
		memmove(proto->response_buffer,
			proto->response_buffer + proto->parsed_offset,
			proto->response_offset - proto->parsed_offset);

		proto->response_offset -= proto->parsed_offset;
		proto->parsed_offset = 0;

		if(proto->reason)
		{
		    globus_free(proto->reason);
		}
		proto->code = 0;
		proto->reason = GLOBUS_NULL;

		globus_i_gass_transfer_keyvalue_destroy(
		    &proto->headers);

		/*
		 * There may be more headers in our buffer, so
		 * we should go through the parse loop again,
		 * maybe
		 */
		if(proto->response_offset != 0)
		{
		    continue;
		}
		else
		{
		    goto repost_read;
		}
	    }
	    else if(proto->code >= 300 &&
		    proto->code < 400)
	    {
		/* We've got a referral from the server */
		char * location;

		location = globus_i_gass_transfer_keyvalue_lookup(
		    &proto->headers,
		    "location");
		if(location == GLOBUS_NULL)
		{
		    goto deny_exit;
		}
		else
		{
		    char ** referral;
		    char * p;

		    p = location;

		    for(p=location; *p != '\0'; p++)
		    {
			if(!isspace(*p))
			{
			    break;
			}
		    }

		    referral = (char **) globus_malloc(sizeof(char *));
		    referral[0] = globus_libc_strdup(p);
  
		    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;

		    result = globus_io_register_close(
			&proto->handle,
			globus_l_gass_transfer_http_close_callback,
			proto);

		    if(result != GLOBUS_SUCCESS)
		    {
			proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
		    }
		    globus_l_gass_transfer_http_unlock();

		    globus_gass_transfer_proto_request_referred(
			proto->request,
			referral,
			1);
		    
		    globus_l_gass_transfer_http_destroy(
			(globus_gass_transfer_request_proto_t *) proto,
			proto->request);
		    return;
		}
	    }
	    else
	    {
		/*
		 * Successful response for the "get", check for
		 * interesting headers, and indicate "ready" to
		 * the GASS system. (note that first read will
		 * have to deal with the data in 
		 * proto->response_buffer)
		 */
		char *			value;

		/*
		 * Look to see if there are any headers we
		 * care about
		 */
		value = globus_i_gass_transfer_keyvalue_lookup(
		    &proto->headers,
		    "transfer-encoding");
		if(value)
		{
		    char *			tmp;

		    for(tmp = value; *tmp != '\0'; tmp++)
		    {
			if(! isspace(*tmp))
			{
			    break;
			}
		    }
		    if(strncasecmp(tmp, "chunked", strlen("chunked")) == 0)
		    {
			proto->recv_buffer =
			    globus_l_gass_transfer_http_receive;
			proto->chunked = GLOBUS_TRUE;
			proto->recv_state =
			    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_SIZE;
			proto->length = 0;
		    }
		}
		if(!proto->chunked)
		{
		    /* If both
		     * Transfer-Encoding: chunked and
		     * Content-Length: <something>
		     * are passed, the Content-length should be ignored,
		     * according to RFC 2068
		     */
		    value = globus_i_gass_transfer_keyvalue_lookup(
			&proto->headers,
			"content-length");

		    if(value)
		    {
			int			save_errno;
			char *			tmp;

			for(tmp = value; *tmp != '\0'; tmp++)
			{
			    if(! isspace(*tmp))
			    {
				break;
			    }
			}
			globus_libc_lock();
			errno=0;
			proto->length = strtoul(tmp,
						GLOBUS_NULL,
						10);
			save_errno=errno;
			if(save_errno != 0)
			{
			    proto->code = GLOBUS_L_PROTOCOL_FAILURE_CODE;
			    proto->reason = globus_libc_strdup(
				GLOBUS_L_PROTOCOL_FAILURE_REASON);
			    goto deny_exit;
			}
			if(proto->length == 0)
			{
			    proto->recv_state =
				GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
			}
			else
			{
			    proto->recv_state =
				GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_LENGTH;
			}
		    }
		    else
		    {
			proto->recv_state =
			    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_EOF;
		    }
		    
		    if(!proto->text_mode)
		    {
			globus_gass_transfer_request_set_length(proto->request,
								proto->length);
		
		    }
		}
		/*
		 * The response buffer's residue may contain some
		 * entity information, so let's keep it around.
		 * Also, if we are in text mode, we'll have to copy
		 * the data anyway (but should use the block_size)
		 */
	
		 if(proto->text_mode &&
		    proto->block_size > proto->response_buflen)
		 {
		    globus_byte_t *		tmp;

		    tmp = globus_libc_realloc(proto->response_buffer,
					      proto->block_size *
						  sizeof(globus_byte_t));
		    if(tmp != GLOBUS_NULL)
		    {
			proto->response_buffer = tmp;
			proto->response_buflen = proto->block_size;
		    }

		 }

		proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;
		globus_l_gass_transfer_http_unlock();

		globus_gass_transfer_proto_request_ready(
		    proto->request,
		    (globus_gass_transfer_request_proto_t *) proto);
	    }
	    break;
	  case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT:
	  case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND:
	    /* If it is a PUT or APPEND request, we don't expect any
	     * response from the server until we're done sending,
	     * or the server kills our request
	     */
	    if(proto->code < 100 ||
	       proto->code >= 300)
	    {
		/* Request failed. */
		goto put_fail_exit;
	    }
	    else if(proto->code >= 100 &&
		    proto->code < 200)
	    {
		/*
		 * Continue... Reset our response buffers,
		 * and get another set of headers
		 */
		memmove(proto->response_buffer,
			proto->response_buffer + proto->parsed_offset,
			proto->response_offset - proto->parsed_offset);

		proto->response_offset -= proto->parsed_offset;
		proto->parsed_offset = 0;

		if(proto->reason)
		{
		    globus_free(proto->reason);
		}
		proto->code = 0;
		proto->reason = GLOBUS_NULL;

		globus_i_gass_transfer_keyvalue_destroy(
		    &proto->headers);

		/*
		 * There may be more headers in our buffer, so
		 * we should go through the parse loop again,
		 * maybe
		 */
		if(proto->response_offset != 0)
		{
		    continue;
		}
		else
		{
		    goto repost_read;
		}
	    }
	    else
	    {
		goto put_success_exit;
	    }
	  case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_INVALID:
	  default:
	    globus_assert(proto->type !=
			  GLOBUS_GASS_TRANSFER_REQUEST_TYPE_INVALID);
	    globus_assert(GLOBUS_FALSE);
	    return;
	}
    }
    while(proto->code == 0 ||
	  proto->code == 100);
    return;

  repost_read:
    if(result != GLOBUS_NULL)
    {
	proto->code = GLOBUS_L_DEFAULT_FAILURE_CODE;
	proto->reason = globus_libc_strdup(GLOBUS_L_DEFAULT_FAILURE_REASON);
	goto deny_exit;
    }

    /* Reallocate a larger buffer, if we need to */
    if(proto->response_buflen == proto->response_offset)
    {
	globus_byte_t *				tmp;
	tmp = globus_libc_realloc(proto->response_buffer,
				  proto->response_buflen *
				      2 *
				      sizeof(globus_byte_t));
	if(tmp == GLOBUS_NULL)
	{
	    proto->code = GLOBUS_L_MALLOC_FAILURE_CODE;
	    proto->reason = globus_libc_strdup(GLOBUS_L_MALLOC_FAILURE_REASON);

	    goto deny_exit;
	}
	else
	{
	    proto->response_buffer = tmp;
	    proto->response_buflen *= 2;
	}
    }

    /* Register another read */
    result = globus_io_register_read(&proto->handle,
				     proto->response_buffer +
				     proto->response_offset,
				     proto->response_buflen -
				     proto->response_offset,
				     1,
				     globus_l_gass_transfer_http_response_callback,
				     proto);
    if(result != GLOBUS_SUCCESS)
    {
	/* TODO interpret the error object */
	goto deny_exit;
    }
    globus_l_gass_transfer_http_unlock();

    return;

  deny_exit:
    if(err)
    {
	globus_object_free(err);
    }

    /*
     * The request hasn't been made "ready" yet for GASS, so we can issue
     * a "denied" response now
     */
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
    /*
     * Because the proto is not being returned in a request ready,
     * we must not wait for the GASS system to call the destroyed
     * method of the proto
     */
    proto->destroy_called=GLOBUS_TRUE;

    globus_l_gass_transfer_http_unlock();

    globus_gass_transfer_proto_request_denied(proto->request,
					      proto->code,
					      globus_libc_strdup(proto->reason));
    globus_l_gass_transfer_http_lock();
    result = globus_io_register_close(
	&proto->handle,
	globus_l_gass_transfer_http_close_callback,
	proto);
    if(result != GLOBUS_SUCCESS)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
    }
    globus_l_gass_transfer_http_unlock();
    return;

  put_success_exit:
    /* 
     * request failed after a put or append operation. We
     * need to signal the failure to the GASS system
     */
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
	
    result = globus_io_register_close(
	&proto->handle,
	globus_l_gass_transfer_http_close_callback,
	proto);
    if(result != GLOBUS_SUCCESS)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
    }

    globus_l_gass_transfer_http_unlock();
    globus_gass_transfer_proto_send_complete(proto->request,
					     proto->user_buffer,
					     proto->user_offset,
					     proto->failure_occurred,
					     GLOBUS_TRUE);

    return;
  put_fail_exit:
    /* 
     * request failed after a put or append operation. We
     * need to signal the failure to the GASS system
     */
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
	
    result = globus_io_register_close(
	&proto->handle,
	globus_l_gass_transfer_http_close_callback,
	proto);
    if(result != GLOBUS_SUCCESS)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
    }

    globus_l_gass_transfer_http_unlock();
    globus_gass_transfer_proto_send_complete(proto->request,
					     proto->user_buffer,
					     proto->user_offset,
					     GLOBUS_TRUE,
					     GLOBUS_TRUE);

    return;
}
/* globus_l_gass_transfer_http_response_callback() */

    
static
void
globus_l_gass_transfer_http_request_callback(
    void *					arg,
    globus_io_handle_t *			handle,
    globus_result_t				result,
    globus_byte_t *				buf,
    globus_size_t				nbytes)
{
    globus_gass_transfer_http_request_proto_t *		proto;
    globus_gass_transfer_http_listener_proto_t *	l_proto;
    globus_object_t *					err=GLOBUS_NULL;
    char *						value;

    l_proto = (globus_gass_transfer_http_listener_proto_t *) arg;
    proto = l_proto->request;

    if(result != GLOBUS_SUCCESS)
    {
	err = globus_error_get(result);
    }

    globus_l_gass_transfer_http_lock();

    /* Did the read succeed? */
    if(result != GLOBUS_SUCCESS &&
       !globus_io_eof(err))
    {
	goto deny_exit;
    }

    /* Update our counters */
    proto->response_offset += nbytes;

    /* Parse the buffer that we have */
    if(globus_l_gass_transfer_http_parse_request(proto))
    {
	/* returns true, if we need to read some more */
	if(proto->parsed_offset == 0 &&
	   proto->response_offset > 0 &&
	   !isupper(proto->response_buffer[0]))
	{
	    goto deny_exit;
	}
	goto repost_read;
    }
    else if(proto->parse_error)
    {
	goto deny_exit;
    }
    else if(strcmp(proto->uri, "*") == 0)
    {
	/* Request we don't handle */
	goto deny_exit;
    }
    else if(strcmp(proto->method, "GET") == 0)
    {
	proto->type = GLOBUS_GASS_TRANSFER_REQUEST_TYPE_GET;
    }
    else if(strcmp(proto->method, "PUT") == 0)
    {
	proto->type = GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT;
    }
    else if(strcmp(proto->method, "POST") == 0 &&
	    strncmp(proto->uri,
		    GLOBUS_L_APPEND_URI,
		    strlen(GLOBUS_L_APPEND_URI)) == 0)
    {
	globus_size_t				append_len;
	globus_size_t				uri_len;

	append_len = strlen(GLOBUS_L_APPEND_URI);
	uri_len = strlen(proto->uri) - strlen(GLOBUS_L_APPEND_URI);

	proto->type = GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND;
	memmove(proto->uri,
		proto->uri + append_len,
		uri_len);
	proto->uri[uri_len] = '\0';

    }
    else
    {
	/* Unknown/Unsupported method */
	goto deny_exit;
    }
    globus_gass_transfer_request_set_type(proto->request,
					  proto->type);
    /* Construct URL for this request */
    if(strncmp(proto->uri, "https://", strlen("https://")) == 0 ||
       strncmp(proto->uri, "http://", strlen("http://")) == 0)
    {
	globus_gass_transfer_request_set_url(proto->request,
					     proto->uri);
    }
    else
    {
	char * url_base;
	char * url;

	url_base = globus_gass_transfer_listener_get_base_url(l_proto->listener);
	url = globus_malloc(strlen(url_base) + strlen(proto->uri) + 1);
	sprintf(url, "%s%s", url_base, proto->uri);
	globus_gass_transfer_request_set_url(proto->request,
					     url);
    }

    /*
     * Look to see if there are any headers we
     * care about
     */
    value = globus_i_gass_transfer_keyvalue_lookup(
	&proto->headers,
	"transfer-encoding");
    if(value)
    {
	char *			tmp;

	for(tmp = value; *tmp != '\0'; tmp++)
	{
	    if(! isspace(*tmp))
	    {
		break;
	    }
	}
	if(strncasecmp(tmp, "chunked", strlen("chunked")) == 0)
	{
	    proto->recv_buffer =
		globus_l_gass_transfer_http_receive;
	    proto->chunked = GLOBUS_TRUE;
	    proto->recv_state =
		GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_SIZE;
	    proto->length = 0;
	}
    }
    if(!proto->chunked)
    {
	/* If both
	 * Transfer-Encoding: chunked and
	 * Content-Length: <something>
	 * are passed, the Content-length should be ignored,
	 * according to RFC 2068
	 */
	value = globus_i_gass_transfer_keyvalue_lookup(
	    &proto->headers,
	    "content-length");

	if(value)
	{
	    int			save_errno;
	    char *			tmp;

	    for(tmp = value; *tmp != '\0'; tmp++)
	    {
		if(! isspace(*tmp))
		{
		    break;
		}
	    }
	    globus_libc_lock();
	    errno=0;
	    proto->length = strtoul(tmp,
				    GLOBUS_NULL,
				    10);
	    save_errno=errno;
	    if(save_errno != 0)
	    {
		proto->code = GLOBUS_L_PROTOCOL_FAILURE_CODE;
		proto->reason = globus_libc_strdup(
		    GLOBUS_L_PROTOCOL_FAILURE_REASON);
		goto deny_exit;
	    }
	    if(proto->length == 0)
	    {
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
	    }
	    else
	    {
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_LENGTH;
	    }
	}
	else
	{
	    proto->recv_state =
		GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_EOF;
	}
		    
	if(!proto->text_mode)
	{
	    globus_gass_transfer_request_set_length(proto->request,
						    proto->length);
		
	}
    }
    /*
     * The response buffer's residue may contain some
     * entity information, so let's keep it around.
     * Also, if we are in text mode, we'll have to copy
     * the data anyway (but should use the block_size)
     */
	
    if(proto->text_mode &&
       proto->block_size > proto->response_buflen)
    {
	globus_byte_t *		tmp;

	tmp = globus_libc_realloc(proto->response_buffer,
				  proto->block_size *
				  sizeof(globus_byte_t));
	if(tmp != GLOBUS_NULL)
	{
	    proto->response_buffer = tmp;
	    proto->response_buflen = proto->block_size;
	}

    }

    /* We now have a request, callback to user */
    l_proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_STARTING;
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_IDLE;
    if(proto->connected_subject)
    {
	globus_gass_transfer_request_set_subject(proto->request,
						 globus_libc_strdup(proto->connected_subject));
    }

    globus_l_gass_transfer_http_unlock();

    globus_gass_transfer_proto_new_listener_request(
	l_proto->listener,
	proto->request,
	(globus_gass_transfer_request_proto_t *) proto);


    return;

  repost_read:
    if(result != GLOBUS_NULL)
    {
	goto deny_exit;
    }

    /* Reallocate a larger buffer, if we need to */
    if(proto->response_buflen == proto->response_offset)
    {
	globus_byte_t *				tmp;
	tmp = globus_libc_realloc(proto->response_buffer,
				  proto->response_buflen *
				      2 *
				      sizeof(globus_byte_t));
	if(tmp == GLOBUS_NULL)
	{
	    proto->code = GLOBUS_L_MALLOC_FAILURE_CODE;
	    proto->reason = globus_libc_strdup(GLOBUS_L_MALLOC_FAILURE_REASON);

	    goto deny_exit;
	}
	else
	{
	    proto->response_buffer = tmp;
	    proto->response_buflen *= 2;
	}
    }

    /* Register another read */
    result = globus_io_register_read(&proto->handle,
				     proto->response_buffer +
				     proto->response_offset,
				     proto->response_buflen -
				     proto->response_offset,
				     1,
				     globus_l_gass_transfer_http_request_callback,
				     l_proto);
    if(result != GLOBUS_SUCCESS)
    {
	/* TODO interpret the error object */
	goto deny_exit;
    }
    globus_l_gass_transfer_http_unlock();

    return;

  deny_exit:
    if(err)
    {
	globus_object_free(err);
    }

    /*
     * Since we are the server we must fail this request by calling
     * new_listener with a NULL proto
     */
    proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_CLOSING;
    /*
     * Because the proto is not being returned in a request ready,
     * we must not wait for the GASS system to call the destroyed
     * method of the proto
     */
    proto->destroy_called=GLOBUS_TRUE;

    globus_l_gass_transfer_http_unlock();

    globus_gass_transfer_proto_new_listener_request(
	l_proto->listener,
	proto->request,
	(globus_gass_transfer_request_proto_t *) GLOBUS_NULL);

    globus_l_gass_transfer_http_lock();
    result = globus_io_register_close(
	&proto->handle,
	globus_l_gass_transfer_http_close_callback,
	proto);
    if(result != GLOBUS_SUCCESS)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_STATE_DONE;
    }
    globus_l_gass_transfer_http_unlock();
    return;
}
/* globus_l_gass_transfer_http_request_callback() */

static
globus_bool_t
globus_l_gass_transfer_http_callback_send_callback(
    globus_time_t				time_can_block,
    void *					arg)
{
    globus_gass_transfer_http_request_proto_t *		proto;

    proto = (globus_gass_transfer_http_request_proto_t *) arg;

    globus_gass_transfer_proto_send_complete(proto->request,
					     proto->user_buffer,
					     proto->user_offset,
					     proto->failure_occurred,
					     proto->failure_occurred);
    return GLOBUS_TRUE;
}


static
globus_bool_t
globus_l_gass_transfer_http_callback_ready_callback(
    globus_time_t				time_can_block,
    void *					arg)
{
    globus_gass_transfer_http_request_proto_t *	proto;

    proto = (globus_gass_transfer_http_request_proto_t *) arg;

    globus_gass_transfer_proto_request_ready(proto->request,
					     (globus_gass_transfer_request_proto_t *) proto);

    return GLOBUS_TRUE;
}
/* globus_l_gass_transfer_http_callback_read_buffered_callback() */
static
globus_bool_t
globus_l_gass_transfer_http_callback_read_buffered_callback(
    globus_time_t				time_can_block,
    void *					arg)
{
    globus_gass_transfer_http_request_proto_t *	proto;

    proto = (globus_gass_transfer_http_request_proto_t *) arg;

    globus_l_gass_transfer_http_read_buffered_callback(arg,
						       &proto->handle,
						       GLOBUS_SUCCESS,
						       proto->response_buffer +
						        proto->response_offset,
						       0);
    return GLOBUS_TRUE;
}
/* globus_l_gass_transfer_http_callback_read_buffered_callback() */

static
globus_bool_t
globus_l_gass_transfer_http_callback_receive_callback(
    globus_time_t				time_can_block,
    void *					arg)
{
    globus_gass_transfer_http_request_proto_t *		proto;
    globus_bool_t				last_data = GLOBUS_FALSE;

    proto = (globus_gass_transfer_http_request_proto_t *) arg;

    if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF ||
       proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR)
    {
	last_data = GLOBUS_TRUE;
    }
    globus_gass_transfer_proto_receive_complete(proto->request,
						proto->user_buffer,
						proto->user_offset,
						proto->failure_occurred,
						last_data);
    return GLOBUS_TRUE;
}

static
globus_bool_t
globus_l_gass_transfer_http_callback_listen_callback(
    globus_time_t				time_can_block,
    void *					arg)
{
    globus_gass_transfer_http_listener_proto_t *proto;
    globus_gass_transfer_listener_t		listener;

    proto = (globus_gass_transfer_http_listener_proto_t *) arg;

    globus_l_gass_transfer_http_lock();

    if(proto->state == GLOBUS_GASS_TRANSFER_HTTP_LISTENER_LISTENING)
    {
	proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_READY;
    }

    listener = proto->listener;
    globus_l_gass_transfer_http_unlock();

    globus_gass_transfer_proto_listener_ready(listener);

    return GLOBUS_TRUE;
}
/* globus_l_gass_transfer_http_callback_listen_callback() */

static
void
globus_l_gass_transfer_http_accept(
    globus_gass_transfer_listener_proto_t *	proto,
    globus_gass_transfer_listener_t		listener,
    globus_gass_transfer_request_t		request,
    globus_gass_transfer_requestattr_t *	attr)
{
    globus_result_t				result;
    globus_io_attr_t				tcp_attr;
    globus_gass_transfer_http_listener_proto_t *
						l_proto;
    int						rc;
    int						sndbuf;
    int						rcvbuf;
    globus_bool_t				nodelay;
    globus_gass_transfer_file_mode_t		file_mode=GLOBUS_GASS_TRANSFER_FILE_MODE_BINARY;
    globus_io_secure_authorization_data_t	data;

    l_proto = (globus_gass_transfer_http_listener_proto_t *) proto;

    /* Allocate proto instance */
    l_proto->request = (globus_gass_transfer_http_request_proto_t *) 
	globus_malloc(sizeof(globus_gass_transfer_http_request_proto_t));

    if(l_proto->request == GLOBUS_NULL)
    {
	goto error_exit;
    }

    result = globus_io_tcpattr_init(&tcp_attr);
    if(result != GLOBUS_SUCCESS)
    {
	goto proto_error;
    }

    if(attr != GLOBUS_NULL &&
       *attr != GLOBUS_NULL)
    {
	/* Check attributes we care about */
	rc = globus_gass_transfer_requestattr_get_socket_sndbuf(
	    attr,
	    &sndbuf);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	else
	{
	    if(sndbuf != 0)
	    {
		globus_io_attr_set_socket_sndbuf(&tcp_attr,
						 sndbuf);
	    }
	}

	rc = globus_gass_transfer_requestattr_get_socket_rcvbuf(
	    attr,
	    &rcvbuf);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	else
	{
	    if(rcvbuf != 0)
	    {
		globus_io_attr_set_socket_rcvbuf(&tcp_attr,
						 rcvbuf);
	    }
	}

	rc = globus_gass_transfer_requestattr_get_socket_nodelay(
	    attr,
	    &nodelay);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	else
	{
	    globus_io_attr_set_tcp_nodelay(&tcp_attr,
					   nodelay);
	}

	/* File mode is important on Windows */
	rc = globus_gass_transfer_requestattr_get_file_mode(attr,
							    &file_mode);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
	rc = globus_gass_transfer_requestattr_get_block_size(
	    attr,
	    &l_proto->request->block_size);
	if(rc != GLOBUS_SUCCESS)
	{
	    goto tcpattr_error;
	}
    }

    /* If https, set security attributes for the request */
    if(l_proto->url_scheme == GLOBUS_URL_SCHEME_HTTPS)
    {
	globus_result_t				result;

	
	globus_io_secure_authorization_data_initialize(&data);

	result = globus_io_attr_set_secure_authentication_mode(
	    &tcp_attr,
	    GLOBUS_IO_SECURE_AUTHENTICATION_MODE_GSSAPI,
	    GLOBUS_NULL);

	if(result != GLOBUS_SUCCESS)
	{
	    goto free_auth_data;
	}
	result = globus_io_attr_set_secure_channel_mode(
	    &tcp_attr,
	    GLOBUS_IO_SECURE_CHANNEL_MODE_SSL_WRAP);

	if(result != GLOBUS_SUCCESS)
	{
	    goto free_auth_data;
	}
	rc = globus_gass_transfer_secure_requestattr_get_authorization(
	    attr,
	    &l_proto->request->authorization_mode,
	    &l_proto->request->authorized_subject);

	if(rc != GLOBUS_SUCCESS)
	{
	    goto free_auth_data;
	}
	result = globus_io_secure_authorization_data_set_callback(
	    &data,
	    globus_l_gass_transfer_http_authorization_callback,
	    proto);
	    
	if(result != GLOBUS_SUCCESS)
	{
	    goto free_auth_data;
	}

	result = globus_io_attr_set_secure_authorization_mode(
	    &tcp_attr,
	    GLOBUS_IO_SECURE_AUTHORIZATION_MODE_CALLBACK,
	    &data);

	if(result != GLOBUS_SUCCESS)
	{
	    goto free_auth_data;
	}
	globus_io_secure_authorization_data_destroy(&data);
    }

    /* Initialize the proto instance */
    l_proto->request->send_buffer= globus_l_gass_transfer_http_send;
    l_proto->request->recv_buffer	= globus_l_gass_transfer_http_receive;
    l_proto->request->fail		= globus_l_gass_transfer_http_fail;
    l_proto->request->deny		= globus_l_gass_transfer_http_request_deny;
    l_proto->request->refer		= globus_l_gass_transfer_http_request_refer;
    l_proto->request->authorize		= globus_l_gass_transfer_http_request_authorize;
    l_proto->request->destroy		= globus_l_gass_transfer_http_destroy;
    l_proto->request->text_mode		= (file_mode == GLOBUS_GASS_TRANSFER_FILE_MODE_TEXT);
    l_proto->request->line_mode		= GLOBUS_L_LINE_MODE_UNKNOWN;
    l_proto->request->state		= GLOBUS_GASS_TRANSFER_HTTP_STATE_CONNECTING;
    l_proto->request->request		= request;
    l_proto->request->type		= globus_gass_transfer_request_get_type(request);
    l_proto->request->code		= 0;
    l_proto->request->reason		= 0;
    l_proto->request->parse_error	= GLOBUS_FALSE;
    l_proto->request->destroy_called 	= GLOBUS_FALSE;
    l_proto->request->headers		= GLOBUS_NULL;
    l_proto->request->response_buffer	= GLOBUS_NULL;
    l_proto->request->length		= 0;
    l_proto->request->handled		= 0;
    l_proto->request->chunked		= GLOBUS_FALSE;
    l_proto->request->chunk_left	= 0;
    l_proto->request->failure_occurred	= GLOBUS_FALSE;
    l_proto->request->eof_read		= GLOBUS_FALSE;
    l_proto->request->method		= GLOBUS_NULL;
    l_proto->request->uri		= GLOBUS_NULL;
    l_proto->request->response_offset	= 0;
    l_proto->request->parsed_offset	= 0;
    l_proto->request->client_side	= GLOBUS_FALSE;
    l_proto->request->connected_subject	= GLOBUS_NULL;

    /* Register accept with new request */
    l_proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_ACCEPTING;

    globus_io_tcp_register_accept(&l_proto->handle,
				  &tcp_attr,
				  &l_proto->request->handle,
				  globus_l_gass_transfer_http_accept_callback,
				  l_proto);

    globus_io_tcpattr_destroy(&tcp_attr);


    return;

  free_auth_data:
    globus_io_secure_authorization_data_destroy(&data);
  tcpattr_error:
    globus_io_tcpattr_destroy(&tcp_attr);
  proto_error:
    globus_l_gass_transfer_http_proto_destroy(l_proto->request);
  error_exit:
    l_proto->state = GLOBUS_GASS_TRANSFER_HTTP_LISTENER_STARTING;

    globus_l_gass_transfer_http_unlock();
    /* should callback here */
}
/* globus_l_gass_transfer_http_accept() */

static
globus_result_t
globus_l_gass_transfer_http_register_read(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    /*
     * If binary, and some chunk data is unread from the handle,
     * or we are not chunked, we can read directly into the user's
     * buffer. Otherwise, we must buffer the data and then copy it
     * to the user's buffer.
     */
    if(proto->text_mode == GLOBUS_FALSE &&
	    (
		(proto->chunked && proto->chunk_left > 0) ||
		(proto->chunked == GLOBUS_FALSE)
	    )
           )

    {
	globus_size_t				minimum;
	globus_size_t				maximum;

	minimum = proto->user_waitlen;

	if(minimum > proto->user_buflen - proto->user_offset)
	{
	    minimum = proto->user_buflen - proto->user_offset;
	}
	if(proto->chunked && minimum > proto->chunk_left)
	{
	    minimum = proto->chunk_left;
	}
	if(proto->length != 0 &&
	   minimum > proto->length - proto->handled)
	{
	   minimum = proto->length - proto->handled;
	}

	maximum = proto->user_buflen - proto->user_offset;

	if(proto->chunked && maximum > proto->chunk_left)
	{
	    maximum = proto->chunk_left;
	}
	if(proto->length != 0 &&
	   maximum > proto->length - proto->handled)
	{
	   maximum = proto->length - proto->handled;
	}

	return
	    globus_io_register_read(&proto->handle,
				    proto->user_buffer +
				        proto->user_offset,
				    maximum,
				    minimum,
				    globus_l_gass_transfer_http_read_callback,
				    proto);
    }
    else
    {
	/*
	 * In text mode or in "chunked" mode, we will have
	 * to buffer the data and then apply some transformation
	 * to it upon receiving it.
	 */
	globus_size_t				smaller;

	smaller = proto->user_waitlen;

	if(smaller > proto->response_buflen - proto->response_offset)
	{
	    smaller = proto->response_buflen - proto->response_offset;
	}

	return
	    globus_io_register_read(&proto->handle,
				    proto->response_buffer +
				        proto->response_offset,
				    proto->response_buflen -
				        proto->response_offset,
				    smaller,
				    globus_l_gass_transfer_http_read_buffered_callback,
				    proto);
    }
}


static
char *
globus_l_gass_transfer_http_construct_request(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    globus_size_t				cmd_len;
    char *					cmd = GLOBUS_NULL;
    globus_size_t				length;
    
    /* Construct the request string to send to the server */
    cmd_len = 3;			/* for CRLF\0 termination */
    cmd_len += strlen(proto->url.url_path); /* What we want */
    cmd_len += strlen(proto->url.host); /* Required for http/1.1*/

    switch(proto->type)
    {
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_GET:
	cmd_len += strlen(GLOBUS_L_GET_COMMAND);
	cmd = globus_malloc(cmd_len * sizeof(globus_byte_t));

	if(cmd == GLOBUS_NULL)
	{
	    return GLOBUS_NULL;    
	}
	
	sprintf(cmd,
		GLOBUS_L_GET_COMMAND,
		proto->url.url_path,
		proto->url.host);

	strcat(cmd,
	       CRLF);
	
	return cmd;

      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_PUT:
	cmd_len += strlen(GLOBUS_L_PUT_COMMAND);
	cmd_len += 2;

	if(proto->text_mode == GLOBUS_TRUE)
	{
	    cmd_len += strlen(GLOBUS_L_TEXT_HEADER);
	}
	else
	{
	    cmd_len += strlen(GLOBUS_L_BINARY_HEADER);
	}
	
	length = globus_gass_transfer_request_get_length(proto->request);
	if(length != 0)
	{
	    globus_size_t			x = length;
	    globus_size_t			digits = 0;

	    /* count the number of decimal digits in length */
	    do
	    {
		digits++;
		x /= 10;
	    } while(x > 0);
	    
	    cmd_len += strlen(GLOBUS_L_CONTENT_LENGTH_HEADER);
	    cmd_len += digits;
	    cmd = globus_malloc(cmd_len * sizeof(globus_byte_t));

	    if(cmd == GLOBUS_NULL)
	    {
		return GLOBUS_NULL;
	    }
	    
	    sprintf((char *) cmd,
		    GLOBUS_L_PUT_COMMAND,
		    proto->url.url_path,
		    proto->url.host);

	    sprintf(cmd + strlen(cmd),
		    GLOBUS_L_CONTENT_LENGTH_HEADER,
		    length);
	}
	else
	{
	    cmd_len += strlen(GLOBUS_L_CHUNKED_HEADER);
	    cmd = globus_malloc(cmd_len * sizeof(globus_byte_t));
	    proto->chunked = GLOBUS_TRUE;

	    /* hex encoding of globus_size_t: two bytes to encode per 8 bits,
	     * plus a CRLF to end the chunk header
	     */
	    proto->iov[0].iov_base = (void *)
		globus_malloc((sizeof(globus_size_t) * 2) + strlen(CRLF));
	    /* This never changes */
	    proto->iov[2].iov_base = CRLF;
	    proto->iov[2].iov_len = strlen(CRLF);
	    proto->iov[3].iov_base = "0" CRLF;
	    proto->iov[3].iov_len = strlen("0" CRLF);

	    if(cmd == GLOBUS_NULL)
	    {
		return GLOBUS_NULL;
	    }
	    
	    sprintf((char *) cmd,
		    GLOBUS_L_PUT_COMMAND,
		    proto->url.url_path,
		    proto->url.host);
	    
	    strcat(cmd,
		    GLOBUS_L_CHUNKED_HEADER);
	}
	if(proto->text_mode)
	{
	    strcat(cmd,
		   GLOBUS_L_TEXT_HEADER);
	}
	else
	{
	    strcat(cmd,
		   GLOBUS_L_BINARY_HEADER);
	}
	strcat(cmd,
	       CRLF);

	return cmd;
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_APPEND:
	cmd_len += strlen(GLOBUS_L_APPEND_COMMAND);
	cmd_len += 2;

	if(proto->text_mode == GLOBUS_TRUE)
	{
	    cmd_len += strlen(GLOBUS_L_TEXT_HEADER);
	}
	else
	{
	    cmd_len += strlen(GLOBUS_L_BINARY_HEADER);
	}
	
	length = globus_gass_transfer_request_get_length(proto->request);
	if(length != 0)
	{
	    globus_size_t			x = length;
	    globus_size_t			digits = 0;

	    /* count the number of decimal digits in length */
	    do
	    {
		digits++;
		x /= 10;
	    } while(x > 0);
	    
	    cmd_len += strlen(GLOBUS_L_CONTENT_LENGTH_HEADER);
	    cmd_len += digits;
	    cmd = globus_malloc(cmd_len * sizeof(globus_byte_t));

	    if(cmd == GLOBUS_NULL)
	    {
		return GLOBUS_NULL;
	    }
	    
	    sprintf((char *) cmd,
		    GLOBUS_L_APPEND_COMMAND,
		    proto->url.url_path,
		    proto->url.host);
	    sprintf((char *) cmd + strlen(cmd),
		    GLOBUS_L_CONTENT_LENGTH_HEADER,
		    length);
	}
	else
	{
	    cmd_len += strlen(GLOBUS_L_CHUNKED_HEADER);
	    cmd = globus_malloc(cmd_len * sizeof(globus_byte_t));

	    if(cmd == GLOBUS_NULL)
	    {
		return GLOBUS_NULL;
	    }
	    
	    sprintf((char *) cmd,
		    GLOBUS_L_APPEND_COMMAND,
		    proto->url.url_path,
		    proto->url.host);
	    
	    strcat(cmd,
		    GLOBUS_L_CHUNKED_HEADER);
	}
	if(proto->text_mode)
	{
	    strcat(cmd,
		   GLOBUS_L_TEXT_HEADER);
	}
	else
	{
	    strcat(cmd,
		   GLOBUS_L_BINARY_HEADER);
	}
	strcat(cmd,
	       CRLF);

	return cmd;
      case GLOBUS_GASS_TRANSFER_REQUEST_TYPE_INVALID:
      default:
	globus_assert(proto->type !=
		      GLOBUS_GASS_TRANSFER_REQUEST_TYPE_INVALID);
	globus_assert(GLOBUS_FALSE);

	return GLOBUS_NULL;
    }    
}

#endif /* !parser only */

/*
 * Function: globus_l_gass_transfer_http_parse_response()
 * 
 * Description: Parse a byte array containing a (maybe)
 *              partial response from the server.
 * 
 * Parameters:  Protocol module this pertains to
 * 
 * Returns:  GLOBUS_TRUE if more headers must be read.
 *           
 */
static
globus_bool_t
globus_l_gass_transfer_http_parse_response(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    /*
     * Parse the HTTP Response
     *
     * Grammar (from RFC 2068)
     *
     * Response      = Status-Line               ; Section 6.1
     *                  *( general-header        ; Section 4.5
     *                   | response-header       ; Section 6.2
     *                   | entity-header )       ; Section 7.1
     *                  CRLF
     *                  [ message-body ]         ; Section 7.2
     */
    if(proto->reason == GLOBUS_NULL)
    {
	if(globus_l_gass_transfer_http_parse_status_line(proto))
	{
	    goto repost_read;
	}
	else if(proto->parse_error)
	{
	    goto parse_error;
	}
    }

    if(globus_l_gass_transfer_http_parse_headers(proto))
    {
	goto repost_read;
    }
    else if(proto->parse_error)
    {
	goto parse_error;
    }

    return GLOBUS_FALSE;

  repost_read:
    return GLOBUS_TRUE;
  parse_error:
    return GLOBUS_FALSE;
}
/* globus_l_gass_transfer_http_parse_response() */

static
globus_bool_t
globus_l_gass_transfer_http_parse_request(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    /*
     * Parse the HTTP Request
     *
     *     Request       = Request-Line              ; Section 5.1
     *                     *( general-header         ; Section 4.5
     *                      | request-header         ; Section 5.3
     *                      | entity-header )        ; Section 7.1
     *                     CRLF
     *                     [ message-body ]          ; Section 7.2
     *
     */
    if(proto->method == GLOBUS_NULL)
    {
	if(globus_l_gass_transfer_http_parse_request_line(proto))
	{
	    goto repost_read;
	}
	else if(proto->parse_error)
	{
	    goto parse_error;
	}
    }

    if(globus_l_gass_transfer_http_parse_headers(proto))
    {
	goto repost_read;
    }
    else if(proto->parse_error)
    {
	goto parse_error;
    }

    return GLOBUS_FALSE;

  repost_read:
    return GLOBUS_TRUE;
  parse_error:
    return GLOBUS_FALSE;
}
/* globus_l_gass_transfer_http_parse_request() */

static
globus_bool_t
globus_l_gass_transfer_http_parse_status_line(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    globus_size_t				offset;
    globus_size_t				reason_offset;
    int						r_offset;

    offset = 0;
    /*
     * Status-Line = HTTP-Version SP Status-Code SP Reason-Phrase CRLF
     *               HTTP-Version   = "HTTP" "/" 1*DIGIT "." 1*DIGIT
     */
    if(!globus_l_gass_transfer_http_find_crlf(
	proto->response_buffer + proto->parsed_offset,
	proto->response_offset - proto->parsed_offset,
	&offset))
    {
	return GLOBUS_TRUE;
    }
    /* Replace CRLF with NULL */
    proto->response_buffer[proto->parsed_offset + offset] = '\0';

    if(sscanf((char *) proto->response_buffer + proto->parsed_offset,
	      "HTTP/%d.%d %d %n",
	      &proto->major,
	      &proto->minor,
	      &proto->code,
	      &r_offset) != 3)
    {
	/* Not a HTTP response */
	if(proto->code == 0)
	{
	    proto->code	=
		GLOBUS_L_PROTOCOL_FAILURE_CODE;
	    proto->reason	=
		globus_libc_strdup(GLOBUS_L_PROTOCOL_FAILURE_REASON);
	}
	proto->parsed_offset += offset;
	/* skip the CRLF, as well */
	proto->parsed_offset+=2;

	goto parse_error;
    }
    reason_offset = (globus_size_t) r_offset;
    
    proto->reason = globus_libc_strdup((char *) (proto->response_buffer +
						 proto->parsed_offset +
						 reason_offset));
    proto->parsed_offset += offset;
    /* skip the CRLF, as well */
    proto->parsed_offset+=2;

    return GLOBUS_FALSE;

  parse_error:
    proto->parse_error = GLOBUS_TRUE;

    return GLOBUS_FALSE;
}


static
globus_bool_t
globus_l_gass_transfer_http_parse_request_line(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    globus_size_t				offset;

    offset = 0;
    /*
     *    Request-Line   = Method SP Request-URI SP HTTP-Version CRLF
     *    Method         = "OPTIONS"                ; Section 9.2
     *                   | "GET"                    ; Section 9.3
     *                   | "HEAD"                   ; Section 9.4
     *                   | "POST"                   ; Section 9.5
     *                   | "PUT"                    ; Section 9.6
     *                   | "DELETE"                 ; Section 9.7
     *                   | "TRACE"                  ; Section 9.8
     *                   | extension-method
     *
     *    extension-method = token
     */
    if(!globus_l_gass_transfer_http_find_crlf(
	proto->response_buffer + proto->parsed_offset,
	proto->response_offset - proto->parsed_offset,
	&offset))
    {
	return GLOBUS_TRUE;
    }
    /* Replace CRLF with NULL */
    proto->response_buffer[proto->parsed_offset + offset] = '\0';

    proto->method = globus_malloc(offset);
    proto->uri = globus_malloc(offset);

    if(sscanf((char *) proto->response_buffer + proto->parsed_offset,
	      "%s %s HTTP/%d.%d",
	      proto->method,
	      proto->uri,
	      &proto->major,
	      &proto->minor) != 4)
    {
	/* Not a HTTP request */
	if(proto->code == 0)
	{
	    proto->code	=
		GLOBUS_L_PROTOCOL_FAILURE_CODE;
	    proto->reason	=
		globus_libc_strdup(GLOBUS_L_PROTOCOL_FAILURE_REASON);
	}
	proto->parsed_offset += offset;
	/* skip the CRLF, as well */
	proto->parsed_offset+=2;

	goto parse_error;
    }

    /* skip the CRLF, as well */
    proto->parsed_offset += offset + 2;

    return GLOBUS_FALSE;

  parse_error:
    proto->parse_error = GLOBUS_TRUE;

    return GLOBUS_FALSE;
}

static
globus_bool_t
globus_l_gass_transfer_http_parse_headers(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    globus_bool_t				all_headers = GLOBUS_FALSE;

    while(!all_headers)
    {
	if(globus_l_gass_transfer_http_parse_one_header(proto,
							&all_headers))
	{
	    return GLOBUS_TRUE;
	}
	else if(proto->parse_error)
	{
	    return GLOBUS_FALSE;
	}
    }
    return GLOBUS_FALSE;
}
/* globus_l_gass_transfer_http_parse_headers() */

static
globus_bool_t
globus_l_gass_transfer_http_parse_one_header(
    globus_gass_transfer_http_request_proto_t *		proto,
    globus_bool_t *				last_header)
{
    globus_size_t				offset;
    globus_bool_t				end_of_header;
    globus_size_t			        continuation=0;
    char *					value;
    char *					new_value;
    int						i;

    /*
     * message-header = field-name ":" [ field-value ] CRLF
     *
     *    field-name     = token
     *    field-value    = *( field-content | LWS )
     *    LWS		 = [CRLF] 1*( SP | HT )
     *
     *    field-content  = <the OCTETs making up the field-value
     *                     and consisting of either *TEXT or combinations
     *                     of token, tspecials, and quoted-string>
     *    token          = 1*<any CHAR except CTLs or tspecials>
     *    CTL            = <any US-ASCII control character
     *                     (octets 0 - 31) and DEL (127)>
     *    tspecials      = "(" | ")" | "<" | ">" | "@"
     *                   | "," | ";" | ":" | "\" | <">
     *                   | "/" | "[" | "]" | "?" | "="
     *			 | "{" | "}" | SP | HT
     *
     * This implementation is a little lax on the character restrictions...
     */

    end_of_header = GLOBUS_FALSE;

    /* Find the complete header (which may span multiple lines) */
    while(!end_of_header)
    {
	/* Find the end of this header line */
	if(! globus_l_gass_transfer_http_find_crlf(
	    proto->response_buffer +
	        proto->parsed_offset +
	        continuation,
	    proto->response_offset -
	        proto->parsed_offset -
	        continuation,
	    &offset))
	{
	    return GLOBUS_TRUE;
	}
	else if(offset == 0)
	{
	    end_of_header = GLOBUS_TRUE;
	    break;
	}
	/*
	 * Reached end-of-read data before being able to detect a
	 * continuation
	 */
	if(proto->parsed_offset + continuation + offset + 2 >=
	   proto->response_offset)
	{
	    return GLOBUS_TRUE;
	}
	/* Check for continuation (LWS) */
	if(islws(proto->response_buffer[proto->parsed_offset +
				       continuation + offset + 2]))
	{
	    continuation += offset + 2;
	}
	else
	{
	    /* No continuation, we have a header */
	    end_of_header = GLOBUS_TRUE;
	}
    }

    /* This is the last header if it consists of CRLF only */
    if(proto->response_buffer[proto->parsed_offset] == CR &&
       proto->response_buffer[proto->parsed_offset + 1] == LF &&
       continuation + offset == 0)
    {
	*last_header = GLOBUS_TRUE;
	proto->parsed_offset += strlen(CRLF);

	return GLOBUS_FALSE;
    }

    /* Canonical form of header is lower-case */
    for(i = proto->parsed_offset; i < proto->parsed_offset + continuation + offset; i++)
    {
	if(proto->response_buffer[i] == ':')
	{
	    break;
	}
	else
	{
	    proto->response_buffer[i] =
		(char) tolower((int) (proto->response_buffer[i]));
	}
    }

    if(proto->response_buffer[i] != ':')
    {
	/* The header's name is illegal */
	proto->code	= GLOBUS_L_PROTOCOL_FAILURE_CODE;
	if(proto->reason != GLOBUS_NULL)
	{
	    globus_free(proto->reason);
	}
	proto->reason =
	    globus_libc_strdup(GLOBUS_L_PROTOCOL_FAILURE_REASON);

	proto->parse_error = GLOBUS_TRUE;

	return GLOBUS_FALSE;
    }

    /* NULL-terminate the header's name */
    proto->response_buffer[i] = '\0';

    new_value = (char *) &proto->response_buffer[i+1];

    /* Make the header's value NULL terminated */
    proto->response_buffer[proto->parsed_offset+continuation+offset] = '\0';

    /*
     * Add header to table. If it already there, append a 
     * comma and the new header's value.
     */
    value = (char *) globus_i_gass_transfer_keyvalue_lookup(
	&proto->headers,
	(char *) (proto->response_buffer + proto->parsed_offset));

    if(value == GLOBUS_NULL)
    {
	/* New header */
	globus_i_gass_transfer_keyvalue_insert(
	    &proto->headers,
	    globus_libc_strdup((char *) (proto->response_buffer +
					 proto->parsed_offset)),
	    globus_libc_strdup(new_value));
    }
    else
    {
	/* Existing header, append */
	globus_byte_t *			new_ptr;

	new_ptr = globus_libc_realloc(value,
				      (strlen(value) +
				          strlen(new_value) + 2) *
				          sizeof(globus_byte_t));
	strcat((char *) new_ptr,
	       ",");
	strcat((char *) new_ptr,
		new_value);
	globus_i_gass_transfer_keyvalue_replace(
	    &proto->headers,
	    (char *) (proto->response_buffer + proto->parsed_offset),
	    (char *) new_ptr);
    }

    /* Move the "parsed" pointer to the end of what we've just handled */
    proto->parsed_offset += continuation + offset + 2;

    return GLOBUS_FALSE;
}
/* globus_l_gass_transfer_http_parse_one_header() */

static
globus_bool_t
globus_l_gass_transfer_http_find_crlf(
    globus_byte_t *				bytes,
    globus_size_t				len,
    globus_size_t *				crlf_offset)
{
    int						i;

    if(len == 0)
    {
	return GLOBUS_FALSE;
    }
    /* See if we can find the end an http meta-information line */
    for (i = 0; i < len-1; i++)
    {
	if(bytes[i] == CR &&
	   bytes[i+1] == LF)
	{
	    *crlf_offset = i;
	    return GLOBUS_TRUE;
	}
    }
    return GLOBUS_FALSE;
}
/* globus_l_gass_transfer_http_find_crlf() */

/*
 * Function: globus_l_gass_transfer_http_copy_text_buffer()
 * 
 * Description: Copy a text array from an HTTP-message to
 *              a user's buffer, converting end-of-line characters
 *		to the local host format. Determines the message
 *		line format based on the first end-of-line it
 *		reaches if it is unknown.
 * 
 * Parameters: 
 * 
 * Returns: 
 */
void
globus_l_gass_transfer_http_copy_text_buffer(
    globus_byte_t *				output,
    globus_byte_t *				input,
    globus_gass_transfer_http_line_mode_t *	line_mode,
    globus_size_t				max_input,
    globus_size_t				max_output,
    globus_size_t *				input_copied,
    globus_size_t *				output_copied)
{
    globus_size_t				src;
    globus_size_t				dst;
    src = 0;
    dst = 0;

    /* Need to determine the line terminator */
    if(*line_mode == GLOBUS_L_LINE_MODE_UNKNOWN)
    {
	while(src < max_input-1 && dst < max_output-1)
	{
            if(input[src] == CR && (*line_mode) ==
		    GLOBUS_L_LINE_MODE_UNKNOWN)
	    {
		if(input[src+1] == LF)
		{
		    *line_mode = GLOBUS_L_LINE_MODE_CRLF;
		    break;
		}
		else
		{
		    *line_mode = GLOBUS_L_LINE_MODE_CR;
		    break;
		}
	    }
	    else if(input[src] == LF && (*line_mode) ==
		    GLOBUS_L_LINE_MODE_UNKNOWN)
	    {
		*line_mode = GLOBUS_L_LINE_MODE_LF;
		break;
	    }
	    else
	    {
		output[dst] = GLOBUS_L_TEXT_BYTE(input[src]);
		dst++;
		src++;
		
		continue;
	    }
	}
	/* did we finish because we read the end-of-input or output? */
	if(src == max_input-1 ||
	   dst == max_output-1)
	{
	    *input_copied = src;
	    *output_copied = dst;
	    return;
	}
    }

    /*
     * Convert from *line_mode terminated text, to the local-machine's line
     * mode
     */
    while(src < max_input && dst < max_output)
    {
	if(input[src] == CR && input[src+1] == LF &&
	   *line_mode == GLOBUS_L_LINE_MODE_CRLF)
	{
	    switch(globus_l_gass_transfer_http_line_mode)
	    {
	      case GLOBUS_L_LINE_MODE_CR:
		/* CRLF to CR */
		output[dst] = CR;
		dst++;
		src += 2;
		break;
	      case GLOBUS_L_LINE_MODE_CRLF:
		/* CRLF to CRLF */
		output[dst]= CR;
		dst++;
		output[dst]= LF;
		dst++;
		src += 2;
		break;
	      case GLOBUS_L_LINE_MODE_LF:
		/* CRLF to LF */
		output[dst]= LF;
		dst++;
		src += 2;
		break;
	      case GLOBUS_L_LINE_MODE_UNKNOWN:
		globus_assert(globus_l_gass_transfer_http_line_mode !=
			      GLOBUS_L_LINE_MODE_UNKNOWN);
	    }
	}
	else if(input[src] == CR && *line_mode == GLOBUS_L_LINE_MODE_CR)
	{
	    switch(globus_l_gass_transfer_http_line_mode)
	    {
	      case GLOBUS_L_LINE_MODE_CR:
		/* CR to CR */
		output[dst] = CR;
		dst++;
		src++;
		break;
	      case GLOBUS_L_LINE_MODE_CRLF:
		/* CR to CRLF */
		output[dst]= CR;
		dst++;
		output[dst]= LF;
		dst++;
		src++;
		break;
	      case GLOBUS_L_LINE_MODE_LF:
		/* CRLF to LF */
		output[dst]= LF;
		dst++;
		src++;
		break;
	      case GLOBUS_L_LINE_MODE_UNKNOWN:
		globus_assert(globus_l_gass_transfer_http_line_mode !=
			      GLOBUS_L_LINE_MODE_UNKNOWN);
	    }
	}
	else if(input[src] == LF && *line_mode == GLOBUS_L_LINE_MODE_LF)
	{
	    switch(globus_l_gass_transfer_http_line_mode)
	    {
	      case GLOBUS_L_LINE_MODE_CR:
		/* LF to CR */
		output[dst] = CR;
		dst++;
		src++;
		break;
	      case GLOBUS_L_LINE_MODE_CRLF:
		/* LF to CRLF */
		output[dst]= CR;
		dst++;
		output[dst]= LF;
		dst++;
		src++;
		break;
	      case GLOBUS_L_LINE_MODE_LF:
		/* LF to LF */
		output[dst]= LF;
		dst++;
		src++;
		break;
	      case GLOBUS_L_LINE_MODE_UNKNOWN:
		globus_assert(globus_l_gass_transfer_http_line_mode !=
			      GLOBUS_L_LINE_MODE_UNKNOWN);
	    }
	}
	else
	{
	    output[dst] = GLOBUS_L_TEXT_BYTE(input[src]);
	    dst++;
	    src++;
	    
	    continue;
	}
    }
    *input_copied = src;
    *output_copied = dst;
}
/* globus_l_gass_transfer_http_copy_text_buffer() */

static
void
globus_l_gass_transfer_unbuffer_text(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    /*
     * Copy the text from the proto's response buffer to the
     * user's buffer
     */
    globus_size_t			input_copied;
    globus_size_t			output_copied;
    globus_size_t			src_max;
    globus_bool_t			redo = GLOBUS_FALSE;

    do
    {
	src_max = proto->response_offset - proto->parsed_offset;

	if(proto->chunked &&
	   (src_max > proto->chunk_left))
	{
	    src_max = proto->chunk_left;
	}

    /*
     * Copy the text, converting to 7-bit US-ASCII, and
     * converting the document's end-of-line to be the local
     * machine's end-of-line character
     */
	globus_l_gass_transfer_http_copy_text_buffer(
	    proto->user_buffer + proto->user_offset,
	    proto->response_buffer + proto->parsed_offset,
	    &proto->line_mode,
	    src_max,
	    proto->user_buflen - proto->user_offset,
	    &input_copied,
	    &output_copied);

	proto->user_offset += output_copied;
	proto->parsed_offset += input_copied;
	proto->handled += input_copied;
	if(proto->chunked)
	{
	    proto->chunk_left -= input_copied;
	}

	if(output_copied > proto->user_waitlen)
	{
	    proto->user_waitlen = 0;
	}
	else
	{
	    proto->user_waitlen -= output_copied;
	}
	if(proto->response_offset - proto->parsed_offset == 1 &&
	   proto->line_mode == GLOBUS_L_LINE_MODE_UNKNOWN)
	{
	    if(proto->response_buffer[proto->parsed_offset] == CR)
	    {
		proto->line_mode = GLOBUS_L_LINE_MODE_CR;
		redo = GLOBUS_TRUE;
	    }
	    else if (proto->response_buffer[proto->parsed_offset] == LF)
	    {
		proto->line_mode = GLOBUS_L_LINE_MODE_LF;
		redo = GLOBUS_TRUE;
	    }
	    else
	    {
		/* doesn't matter, since the document contains no newlines */
		proto->line_mode = GLOBUS_L_LINE_MODE_LF;
		redo = GLOBUS_TRUE;
	    }
	}
	else
	{
	    redo = GLOBUS_FALSE;
	}
    } while(redo);

    /* Reset our buffer, if we've read it all */
    if(proto->parsed_offset == proto->response_offset)
    {
	proto->parsed_offset = 0;
	proto->response_offset = 0;
    }
    else if(proto->parsed_offset != 0)
    {
	/* This may not be necessary */
	memmove(proto->response_buffer,
		proto->response_buffer + proto->parsed_offset,
		proto->response_offset - proto->parsed_offset);
	proto->response_offset -= proto->parsed_offset;
	proto->parsed_offset = 0;
    }
}
/* globus_l_gass_transfer_unbuffer_text() */

static
void
globus_l_gass_transfer_unbuffer_binary(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    /*
     * Copy the binary from the proto's response buffer to the
     * user's buffer
     */
    globus_size_t			smaller;

    smaller = proto->response_offset - proto->parsed_offset;
    if(smaller > proto->user_buflen - proto->user_offset)
    {
	smaller = proto->user_buflen - proto->user_offset;
    }
    if(proto->chunked &&
       smaller > proto->chunk_left)
    {
	smaller = proto->chunk_left;
    }
    memcpy(proto->user_buffer + proto->user_offset,
	   proto->response_buffer + proto->parsed_offset,
	   smaller);

    proto->user_offset += smaller;
    proto->parsed_offset += smaller;
    proto->handled += smaller;
    if(proto->chunked)
    {
	proto->chunk_left -= smaller;
    }

    if(smaller > proto->user_waitlen)
    {
	proto->user_waitlen = 0;
    }
    else
    {
	proto->user_waitlen -= smaller;
    }
    /* Reset our buffer, if we've read it all */
    if(proto->parsed_offset == proto->response_offset)
    {
	proto->parsed_offset = 0;
	proto->response_offset = 0;
    }
    else if(proto->parsed_offset != 0)
    {
	/* This may not be necessary */
	memmove(proto->response_buffer,
		proto->response_buffer + proto->parsed_offset,
		proto->response_offset - proto->parsed_offset);
	proto->response_offset -= proto->parsed_offset;
	proto->parsed_offset = 0;
    }
}
/* globus_l_gass_transfer_unbuffer_binary() */


/* Code for parsing HTTP responses */
static
globus_bool_t
islws(
    char 					byte)
{
    return(byte == ' ' ||
	   byte == '\t');
}
/* islws() */

static
globus_bool_t
ischar(
    char 					byte)
{
    return(byte >= (char) 0 &&
	   byte <= (char) 127);
}
/* ischar() */

static
globus_bool_t
istspecial(
    char 					byte)
{
    return (byte == '(' || byte == ')' || byte == '<' ||
	    byte == '>' || byte == '@' || byte == ',' ||
	    byte == ';' || byte == ':' || byte == '\\' ||
	    byte == '"' || byte == '/' || byte == '[' ||
	    byte == ']' || byte == '?' || byte == '=' ||
	    byte == '{' || byte == '}' || byte == ' ' ||
	    byte == '\t');
}
/* istspecial() */

globus_bool_t
isctl(
    char					byte)
{
    return ((byte >= (char) 0 && byte <= (char) 31) ||
	    byte == (char) 127);
}

static
globus_bool_t
ishex(
    char					byte)
{
    return (byte == 'A' || byte == 'B' || byte == 'C' ||
	    byte == 'D' || byte == 'E' || byte == 'F' ||
	    byte == 'a' || byte == 'b' || byte == 'c' ||
	    byte == 'd' || byte == 'e' || byte == 'f' ||
	    byte == '0' || byte == '1' || byte == '2' ||
	    byte == '3' || byte == '4' || byte == '5' ||
	    byte == '6' || byte == '7' || byte == '8' ||
	    byte == '9');
}
/*
 * all scan and parse functions return GLOBUS_TRUE if more data
 * is needed to read a complete token
 */
static
globus_bool_t
globus_l_gass_transfer_http_scan_star_lws(
    globus_byte_t *				input,
    globus_size_t				max_to_scan,
    globus_size_t *				end_of_token)
{
    globus_size_t				i;

    *end_of_token = 0;
    /*
     * an interesting note from the HTTP/1.1 RFC
     * implied *LWS
     * The grammar described by this specification is word-based. Except
     * where noted otherwise, linear whitespace (LWS) can be included
     * between any two adjacent words (token or quoted-string), and
     * between adjacent tokens and delimiters (tspecials), without
     * changing the interpretation of a field. At least one delimiter
     * (tspecials) must exist between any two tokens, since they would
     * otherwise be interpreted as a single token.
     *
     * And the definition of the LWS that we are parsing
     *
     * LWS            = [CRLF] 1 *(SP | HT)
     * CRLF           = CR LF
     * CR             = <US-ASCII CR, carriage return (13)>
     * LF             = <US-ASCII LF, linefeed (10)>
     * SP             = <US-ASCII SP, space (32)>
     * HT             = <US-ASCII HT, horizontal-tab (9)>
     */
    for(i = 0; i < max_to_scan; i++)
    {
	if(input[i] == ' ' || input[i] == '\t')
	{
	    continue;
	}
	if(input[i] == CR)
	{
	    if(i + 2 > max_to_scan)
	    {
		/* not enough data */
		return GLOBUS_TRUE;
	    }
	    else
	    {
		if(input[i+1] == LF &&
		   (input[i+2] == ' ' ||
		    input[i+2] == '\t'))
		{
		    /* pass over LF */
		    i++;
		    continue;
		}
		else
		{
		    /* This CR doesn't match our pattern */
		    if(i != 0)
		    {
			/* If we've swallowed any whitespace, note it */
			*end_of_token = i;
		    }
		    return GLOBUS_FALSE;
		}
	    }
	}
	else
	{
	    /* end of match */
	    if(i != 0)
	    {
		*end_of_token = i;
	    }
	    return GLOBUS_FALSE;
	}
    }
    /*
     * If we haven't hit an end of the LWS by the end of input,
     * our (*LWS) can't be greedy, so we need more input
     */
    return GLOBUS_TRUE;
}
/* globus_l_gass_transfer_http_scan_star_lws() */

static
globus_bool_t
globus_l_gass_transfer_http_scan_token(
    globus_byte_t *				input,
    globus_size_t				max_to_scan,
    globus_size_t *				end_of_token)
{
    globus_size_t				i;

    *end_of_token = 0;

    /*
     * token          = 1*<any CHAR except CTLs or tspecials>
     * CTL            = <any US-ASCII control character
     *                  (octets 0 - 31) and DEL (127)>
     * tspecials      = "(" | ")" | "<" | ">" | "@"
     *                   | "," | ";" | ":" | "\" | <">
     *                   | "/" | "[" | "]" | "?" | "="
     *			 | "{" | "}" | SP | HT
     */
    for(i = 0; i < max_to_scan; i++)
    {
	if(!ischar(input[i]) ||
	   isctl(input[i]) ||
	   istspecial(input[i]))
	{
	    if(i != 0)
	    {
		*end_of_token = i;
	    }
	    return GLOBUS_FALSE;
	}
    }
    return GLOBUS_TRUE; /* need more data */
}
/* globus_l_gass_transfer_http_scan_token() */

static
globus_bool_t
globus_l_gass_transfer_http_scan_qdtext(
    globus_byte_t *				input,
    globus_size_t				max_to_scan,
    globus_size_t *				end_of_qdtext)
{
    globus_size_t				i;
    globus_size_t				j;

    /*
     * qdtext         = <any TEXT except <">>
     * TEXT           = <any OCTET except CTLs,
     *                     but including LWS>
     * quoted-pair    = "\" CHAR
     * CHAR           = <any US-ASCII character (octets 0 - 127)>
     */
    *end_of_qdtext = 0;

    for(i = 0; i < max_to_scan; i++)
    {
	/* Always absorb LWS in quotes */
	if(globus_l_gass_transfer_http_scan_star_lws(
	    input + i,
	    max_to_scan - i,
	    &j))
	{
	    return GLOBUS_TRUE; /* need more to scan */
	}
	else if(j != 0)
	{
	    i += j;		/* scanned some LWS */
	    continue;
	}
	else if(input[i] == '\\')
	{
	    /* absorb quoted-pair */
	    if(i + 1 < max_to_scan)
	    {
		if(ischar(input[i+1]))
		{
		    i++;
		    
		    continue;
		}
		else
		{
		    *end_of_qdtext = i;
		    return GLOBUS_FALSE;
		}
	    }
	    else
	    {
		return GLOBUS_TRUE;	/* need more to scan */
	    }
	}
	else if(!isctl(input[i]))
	{
	    continue;    
	}
	else if(i != 0)
	{
	    *end_of_qdtext = i;
	    return GLOBUS_FALSE;
	}   
	else
	{
	    return GLOBUS_FALSE;
	}
    }
    return GLOBUS_TRUE;
}
/* globus_l_gass_transfer_http_scan_qdtext() */

static
globus_bool_t
globus_l_gass_transfer_http_scan_quoted_string(
    globus_byte_t *				input,
    globus_size_t				max_to_scan,
    globus_size_t *				end_of_qtd_string)
{
    globus_size_t 				i;
    /*
     * quoted-string  = ( <"> *(qdtext) <"> ) 
     */

    *end_of_qtd_string = 0;

    if(max_to_scan == 0)
    {
	return GLOBUS_TRUE;
    }

    /* quoted text must begin with '"' */
    if(input[0] != '"')
    {
	return GLOBUS_FALSE;
    }

    if(globus_l_gass_transfer_http_scan_qdtext(
	input+1,
	max_to_scan-1,
	&i))
    {
	return GLOBUS_TRUE;	/* need more data */
    }

    if(i == max_to_scan - 1)
    {
	return GLOBUS_TRUE;	/* need more data */
    }

    /* quoted text must end with '"' */
    if(input[i] == '"')
    {
	*end_of_qtd_string = i+1;
	return GLOBUS_FALSE;
    }
    else
    {
	return GLOBUS_FALSE;
    }
}
/* globus_l_gass_transfer_http_scan_quoted_string() */


static
globus_bool_t
globus_l_gass_transfer_http_scan_chunk_ext(
    globus_byte_t *				input,
    globus_size_t				max_to_scan,
    globus_size_t *				end_of_chunk_ext)
{
    globus_size_t				i;
    globus_size_t				j;
    globus_bool_t				semicolon = GLOBUS_FALSE;

    /*
     * chunk-ext      = *( ";" chunk-ext-name [ "=" chunk-ext-value ] )
     * chunk-ext-name = token
     * chunk-ext-val  = token | quoted-string
     */

    *end_of_chunk_ext = 0;
    i = 0;

    for(;;)
    {
	if(max_to_scan - i == 0)
	{
	    return GLOBUS_TRUE;
	}
	else if(globus_l_gass_transfer_http_scan_star_lws(
	    input + i,
	    max_to_scan - i,
	    &j))
	{
	    return GLOBUS_TRUE; /* more to scan */
	}
	else if(j != 0)
	{
	    i += j; /* scanned some leading LWS */
	}

	if(i + 1 >= max_to_scan)
	{
	    return GLOBUS_TRUE;
	}
	/*
	 * Only consume LWS if there is a semicolon,
	 * otherwise, we may consume part of CRLF/message body
	 */
	if(input[i] != ';' && semicolon)
	{
	    *end_of_chunk_ext = i;
	    return GLOBUS_FALSE;
	}
	else if(input[i] != ';')
	{
	    return GLOBUS_FALSE;
	}

	semicolon = GLOBUS_TRUE;

	/* pass over ';' */
	i++;

	/* skip any LWS */
	if(globus_l_gass_transfer_http_scan_star_lws(
	    input + i,
	    max_to_scan - i,
	    &j))
	{
	    return GLOBUS_TRUE; /* more to scan */
	}
	else if(j != 0)
	{
	    i += j; /* scanned some LWS */
	}

	/* scan for chunk-ext-name */
	if(globus_l_gass_transfer_http_scan_token(
	    input + i,
	    max_to_scan - i,
	    &j))
	{
	    return GLOBUS_TRUE;
	}
	else if(j == 0) /* illegal, ';' but no token */
	{
	    *end_of_chunk_ext = 0;
	    return GLOBUS_FALSE;
	}
	else
	{
	    i += j;
	}

	/* skip any LWS */
	if(globus_l_gass_transfer_http_scan_star_lws(
	    input + i,
	    max_to_scan - i,
	    &j))
	{
	    return GLOBUS_TRUE; /* more to scan */
	}
	else if(j != 0)
	{
	    i += j; /* scanned some leading LWS */
	}

	/* check for '=' */
	if(i + 1 >= max_to_scan)
	{
	    return GLOBUS_TRUE;
	}
	if(input[i] == ';') /* chunk-ext without chunk-ext-value */
	{
	    continue;
	}
	else if(input[i] != '=')
	{
	    *end_of_chunk_ext = i;
	    return GLOBUS_FALSE;
	}
	/* pass over '=' */
	i++;

	/* skip any LWS */
	if(globus_l_gass_transfer_http_scan_star_lws(
	    input+i,
	    max_to_scan-i,
	    &j))
	{
	    return GLOBUS_TRUE; /* more to scan */
	}
	else if(j != 0)
	{
	    i += j; /* scanned some LWS */
	}


	/* check for chunk-ext-value, either a token, or a quoted-string */
	if(globus_l_gass_transfer_http_scan_token(
	    input + i,
	    max_to_scan - i,
	    &j))
	{
	    return GLOBUS_TRUE; /* more to scan */
	}
	/* no token, try to scan quoted string */
	else if(j == 0 &&
		globus_l_gass_transfer_http_scan_quoted_string(
		    input + i,
		    max_to_scan - i,
		    &j))
	{
	    return GLOBUS_TRUE; /* more to scan */
	}
	else
	{
	    i += j;
	}
    }
}
/* globus_l_gass_transfer_http_scan_chunk_ext() */

static
globus_bool_t
globus_l_gass_transfer_http_scan_chunk_size(
    globus_byte_t *				input,
    globus_size_t				max_to_scan,
    globus_size_t *				end_of_chunk_size)
{
    globus_size_t				i;

    /*
     * hex-no-zero    = <HEX excluding "0">
     * chunk-size     = hex-no-zero *HEX
     */

    *end_of_chunk_size = 0;
    i = 0;

    if(i >= max_to_scan)
    {
	return GLOBUS_TRUE;
    }

    if(input[i] == '0' &&
       i + 1 < max_to_scan)
    {
	*end_of_chunk_size = 1;
	return GLOBUS_FALSE;
    }

    for(; i < max_to_scan; i++)
    {
	if(input[i] == ' ' ||
	   input[i] == '\t')
	{
	    continue;
	}
	if(!ishex(input[i]))
	{
	    *end_of_chunk_size = i;
	    return GLOBUS_FALSE;
	}
    }
    return GLOBUS_TRUE;
}
/* globus_l_gass_transfer_http_scan_chunk_size() */

/*
 * Function: globus_l_gass_transfer_http_handle_chunk()
 * 
 * Description:  Parse any chunk header/footer information, and
 *               copy chunk data (with appropriate text-mode
 *               translations) into the user's buffers
 * 
 * Parameters: 
 * 
 * Returns: 
 */
static
globus_bool_t
globus_l_gass_transfer_http_handle_chunk(
    globus_gass_transfer_http_request_proto_t *		proto)
{
    globus_size_t				i;

    while(proto->response_offset - proto->parsed_offset > 0)
    {
	switch(proto->recv_state)
	{
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_SIZE:
	    if(globus_l_gass_transfer_http_scan_chunk_size(
		proto->response_buffer + proto->parsed_offset,
		proto->response_offset - proto->parsed_offset,
		&i))
	    {
		/* true == need more data */
		return GLOBUS_TRUE;
	    }
	    else if(i == 0)
	    {
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
		proto->failure_occurred =
		    GLOBUS_TRUE;
		return GLOBUS_FALSE;
	    }

	    proto->chunk_left = strtoul((char *) proto->response_buffer +
					proto->parsed_offset,
					GLOBUS_NULL,
					16);
	    proto->parsed_offset += i;
	    if(proto->chunk_left == 0)
	    {
		/*
		 * last chunk can not be followed by chunk-ext elements,
		 * but may be followed by a set of footers
		 */
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_FOOTER;
		break;
	    }
	    else
	    {
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_EXT;
	    }
	    /* FALLSTHROUGH */
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_EXT:
	    if(globus_l_gass_transfer_http_scan_chunk_ext(
		proto->response_buffer + proto->parsed_offset,
		proto->response_offset - proto->parsed_offset,
		&i))
	    {
		/* true == need more data */
		return GLOBUS_TRUE;
	    }
	    else
	    {
		proto->parsed_offset += i;
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_END_HEADER_CR;
		/* FALLSTHROUGH */
	    }
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_END_HEADER_CR:
	    if(proto->response_offset - proto->parsed_offset > 0)
	    {
		if(proto->response_buffer[proto->parsed_offset] != CR)
		{
		    proto->parse_error=GLOBUS_TRUE;
		    proto->failure_occurred = GLOBUS_TRUE;
		    proto->recv_state =
			GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
		    return GLOBUS_FALSE;
		}
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_END_HEADER_LF;
		proto->parsed_offset++;
	    }
	    else
	    {
		return GLOBUS_TRUE;
	    }
	    /* FALLSTHROUGH */
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_END_HEADER_LF:
	    if(proto->response_offset - proto->parsed_offset > 0)
	    {
		if(proto->response_buffer[proto->parsed_offset] != LF)
		{
		    proto->parse_error=GLOBUS_TRUE;
		    proto->failure_occurred=GLOBUS_TRUE;
		    return GLOBUS_FALSE;
		}
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_BODY;
		proto->parsed_offset++;
	    }
	    else
	    {
		return GLOBUS_TRUE;
	    }
	    /* FALLSTHROUGH */
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_BODY:
	    if(proto->chunk_left == 0)
	    {
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_END_BODY_CR;
	    }
	    else
	    {
		break;
	    }
	    /* FALLSTHROUGH */

	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_END_BODY_CR:
	    if(proto->response_offset - proto->parsed_offset > 0)
	    {
		if(proto->response_buffer[proto->parsed_offset] != CR)
		{
		    proto->parse_error=GLOBUS_TRUE;
		    proto->failure_occurred=GLOBUS_TRUE;
		    proto->recv_state =
			GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
		    return GLOBUS_FALSE;
		}
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_END_BODY_LF;
		proto->parsed_offset++;
	    }
	    else
	    {
		return GLOBUS_TRUE;
	    }
	    /* FALLSTHROUGH */

	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_END_BODY_LF:
	    if(proto->response_offset - proto->parsed_offset > 0)
	    {
		if(proto->response_buffer[proto->parsed_offset] != LF)
		{
		    proto->parse_error=GLOBUS_TRUE;
		    proto->failure_occurred=GLOBUS_TRUE;
		    proto->recv_state =
			GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
		    return GLOBUS_FALSE;
		}
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_SIZE;
		proto->parsed_offset++;
		break;
	    }
	    else
	    {
		return GLOBUS_TRUE;
	    }
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_FOOTER:
	    if(globus_l_gass_transfer_http_parse_headers(proto))
	    {
		/* need more data */
		return GLOBUS_TRUE;
	    }
	    else if(proto->parse_error)
	    {
		proto->failure_occurred=GLOBUS_TRUE;
		proto->recv_state = 
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
		return GLOBUS_FALSE;
	    }
	    else
	    {
		proto->recv_state =
		    GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF;
		return GLOBUS_FALSE;
	    }
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_EOF:
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_LENGTH:
	    break;

	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR:
	  case GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF:
	    return GLOBUS_FALSE;
	}

	if(proto->recv_state ==
	       GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_LENGTH ||
	   proto->recv_state ==
	       GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_EOF ||
	   proto->recv_state ==
	       GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_CHUNK_BODY)
	{
	    if(proto->user_buflen > proto->user_offset &&
	       proto->response_offset > proto->parsed_offset)
	    {
		/* some room is in the user's buffer for new data */
		if(proto->text_mode)
		{
		    globus_l_gass_transfer_unbuffer_text(proto);
		}
		else
		{
		    globus_l_gass_transfer_unbuffer_binary(proto);
		}
		/* check to see if we've failed/completed because of this */
		if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_LENGTH &&
		   proto->length == proto->handled)
		{
		    proto->recv_state =
			GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF;
		}
		else if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_LENGTH &&
			proto->eof_read &&
			proto->response_offset - proto->parsed_offset == 0)
		{
		    proto->recv_state =
			GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_ERROR;
		}
		else if(proto->recv_state == GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_UNTIL_EOF &&
			proto->eof_read &&
			proto->response_offset - proto->parsed_offset == 0)
		{
		    proto->recv_state =
			GLOBUS_GASS_TRANSFER_HTTP_RECV_STATE_EOF;
		}

	    }
	    else
	    {
		return GLOBUS_FALSE;
	    }
	}
    }
    return GLOBUS_FALSE;
}
/* globus_l_gass_transfer_http_handle_chunk() */

