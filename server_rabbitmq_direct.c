/*
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MIT
 *
 * Portions created by Aboobeker Sidhik are Copyright (c) 2012-2013
 * Aboobeker Sidhik. All Rights Reserved.
 *
 * Portions created by VMware are Copyright (c) 2007-2012 VMware, Inc.
 * All Rights Reserved.
 *
 * Portions created by Tony Garnock-Jones are Copyright (c) 2009-2010
 * VMware, Inc. and Tony Garnock-Jones. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ***** END LICENSE BLOCK *****
 */
 
 // gcc server_rabbitmq_direct.c -o server -lrabbitmq `pkg-config --cflags --libs glib-2.0`

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <glib-2.0/glib.h>
#include <glib-2.0/glib/galloca.h>
#include <assert.h>


int main(int argc, char *argv[]) {
  char const *hostname;
  int port, status;
  char const *exchange;
  char const *routingkey;
  char const *messagebody;
  char const *queue;
  amqp_connection_state_t conn;
  amqp_bytes_t reply_to_queue;


  hostname = "172.26.76.45";
  port = 5672;
  exchange = "halles-recording-exchange";
  routingkey = "recording-to-mixing-server-r";
  messagebody = " fuck you";
  queue = "hello";
  //queue = "amq.rabbitmq.reply-to";
  conn = amqp_new_connection();
  amqp_socket_t *socket = amqp_tcp_socket_new(conn);
  if(socket == NULL) {
  	printf( "Can't connect to RabbitMQ server: error creating socket...\n");
  }
  status = amqp_socket_open(socket, (const char *)hostname, port);
  if(status != AMQP_STATUS_OK) {
	  printf("Can't connect to RabbitMQ server: error opening socket... (%s)\n", amqp_error_string2(status));
  }
  /* PROPERTIES-CAPABILITIES TABLE */
  /* table client-properties */
  amqp_table_entry_t client_properties_entries[1];
  amqp_table_t client_properties_table;
  // subtable capabilities
  amqp_table_entry_t capabilities_entries[1];
  amqp_table_t capabilities_table;
  capabilities_table.num_entries = 1;
  capabilities_entries[0].key = amqp_cstring_bytes("consumer_cancel_notify");
  capabilities_entries[0].value.kind = AMQP_FIELD_KIND_BOOLEAN;
  capabilities_entries[0].value.value.boolean = 1;
  capabilities_table.entries = capabilities_entries;
  client_properties_entries[0].key = amqp_cstring_bytes("capabilities");
  client_properties_entries[0].value.kind = AMQP_FIELD_KIND_TABLE;
  client_properties_entries[0].value.value.table = capabilities_table;
  client_properties_table.num_entries = 1;
  client_properties_table.entries = client_properties_entries;
  /* */
  printf("Logging in...\n");
  amqp_rpc_reply_t result = amqp_login_with_properties(
          conn,
          (const char *)"halles",
          0,
          131072,
          0,
          &client_properties_table,
          AMQP_SASL_METHOD_PLAIN,
		  "halles",
		  "75seuz21");
  if(result.reply_type != AMQP_RESPONSE_NORMAL) {
	  printf("Can't connect to RabbitMQ server: error logging in... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
  }
  printf("Opening channel...\n");
  amqp_channel_open(conn, 1);
  result = amqp_get_rpc_reply(conn);
  if(result.reply_type != AMQP_RESPONSE_NORMAL) {
	  printf("Can't connect to RabbitMQ server: error opening channel... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
  }

  printf("Declaring incoming queue... (%s)\n", (char *) queue);
    amqp_queue_declare(conn,
  		1,
		amqp_cstring_bytes(queue),
  		0, // passive
          0,
          0,
          1,
  		amqp_empty_table);
  result = amqp_get_rpc_reply(conn);
  if(result.reply_type != AMQP_RESPONSE_NORMAL) {
	  printf("Can't connect to RabbitMQ server: error declaring queue... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
  }

  amqp_queue_bind(conn, 1,
		  amqp_cstring_bytes(queue),
		  amqp_cstring_bytes(exchange),
		  amqp_cstring_bytes(routingkey),
                  amqp_empty_table);
  result = amqp_get_rpc_reply(conn);
  if(result.reply_type != AMQP_RESPONSE_NORMAL) {
	  printf("Can't connect to RabbitMQ server: error binding queue 'to_janus_admin_queue' ... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
  }

  amqp_bytes_t consumer_tag;
  consumer_tag = amqp_cstring_bytes("halles-recording-id-1");
  // Consume

  amqp_basic_consume(conn, 1,amqp_cstring_bytes(queue), consumer_tag, 0, 1, 0, amqp_empty_table);
  result = amqp_get_rpc_reply(conn);
  if(result.reply_type != AMQP_RESPONSE_NORMAL) {
	  printf("Can't connect to RabbitMQ server: error consuming... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
  }

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 20000;
	amqp_frame_t frame;
	int return_code;
	for(;;) {
		amqp_maybe_release_buffers(conn);
		int res = amqp_simple_wait_frame_noblock(conn, &frame, &timeout);
		if(res != AMQP_STATUS_OK) {
			if(res == AMQP_STATUS_TIMEOUT)
				continue;
			printf("Error on amqp_simple_wait_frame_noblock: %d (%s)\n", res, amqp_error_string2(res));
			break;
		}
		printf("Frame type %d, channel %d\n", frame.frame_type, frame.channel);
		if(frame.frame_type != AMQP_FRAME_METHOD)
			continue;
		printf("Method %s\n", amqp_method_name(frame.payload.method.id));
		amqp_basic_ack_t *a;
        switch(frame.payload.method.id) {
            case AMQP_CONNECTION_CLOSE_METHOD:
                /* a connection.close method happens when a connection exception occurs,
                 * this can happen by trying to use a channel that isn't open for example.
                 *
                 * In this case the whole connection must be restarted.
                 */
            	printf("AMQP: AMQP_CONNECTION_CLOSE_METHOD. Reconnecting...\n");
                break;
            case AMQP_BASIC_CANCEL_METHOD:
                /* Consumer cancelation */
                /* https://www.rabbitmq.com/consumer-cancel.html */
                /* Maybe node queue owner node failed, or queue has been deleted */
            	printf("AMQP: Consumer cancelation notify. Reconnecting...\n");
                break;
            case AMQP_CHANNEL_CLOSE_METHOD:
                /* a channel.close method happens when a channel exception occurs, this
                 * can happen by publishing to an exchange that doesn't exist for example
                 *
                 * In this case you would need to open another channel redeclare any queues
                 * that were declared auto-delete, and restart any consumers that were attached
                 * to the previous channel
                 */
            	printf("AMQP: Channel exception. Reconnecting...\n");
                break;
            case AMQP_BASIC_RETURN_METHOD:
                /* if a published message couldn't be routed and the mandatory flag was set
                 * this is what would be returned. The message then needs to be read.
                 */
                {
                    amqp_message_t message;
                    amqp_rpc_reply_t ret;
                    ret = amqp_read_message(conn, frame.channel, &message, 0);
                    if (AMQP_RESPONSE_NORMAL != ret.reply_type) {
                         break;
                    }

                    amqp_destroy_message(&message);
                }

                break;
            case AMQP_BASIC_ACK_METHOD:
                /* if we've turned publisher confirms on, and we've published a message
                 * here is a message being confirmed
                 * Here I have to check with hash table and find the UID related to mixing
                 * record the delivery tag in table with process
                 * when i receive the
                 */
                a = (amqp_basic_ack_t*)frame.payload.method.decoded;
                /* if you've kept a count of the messages you've published on the channel,
                 * the a->delivery_tag is the message serial being acknowledged.
                 * if a->multiple != 0, that means all messages up-to-and-including that message
                 * serial are being acknowledged */
                printf("AMQP: AMQP_BASIC_ACK_METHOD bool %d \n", a->multiple);


            	continue;
                break;
            case AMQP_BASIC_DELIVER_METHOD:
                break;
            default:
            	printf("An unexpected method was received %u\n", frame.payload.method.id);
                break;
        }


		amqp_basic_deliver_t *d;
		if(frame.payload.method.id == AMQP_BASIC_DELIVER_METHOD) {
			d = (amqp_basic_deliver_t *)frame.payload.method.decoded;
			printf("Delivery #%u, %.*s\n", (unsigned) d->delivery_tag, (int) d->routing_key.len, (char *) d->routing_key.bytes);
		}


		amqp_simple_wait_frame(conn, &frame);
		printf("Frame type %d, channel %d\n", frame.frame_type, frame.channel);
		if(frame.frame_type != AMQP_FRAME_HEADER) {
			continue;
		}
		amqp_basic_properties_t *p = (amqp_basic_properties_t *)frame.payload.properties.decoded;
		printf("  -- Reply-to: %.*s\n", (int) p->reply_to.len, (char *) p->reply_to.bytes);
		if(p->_flags & AMQP_BASIC_REPLY_TO_FLAG) {
			printf("  -- Reply-to: %.*s\n", (int) p->reply_to.len, (char *) p->reply_to.bytes);
		}

		if(p->_flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
			printf("  -- Content-type: %.*s\n", (int) p->content_type.len, (char *) p->content_type.bytes);
		}
		uint64_t total = frame.payload.properties.body_size, received = 0;
		char *payload = (char *)g_malloc0(total+1), *index = payload;
		while(received < total) {
			amqp_simple_wait_frame(conn, &frame);
			printf("Frame type %d, channel %d\n", frame.frame_type, frame.channel);
			if(frame.frame_type != AMQP_FRAME_BODY)
				break;
			sprintf(index, "%.*s", (int) frame.payload.body_fragment.len, (char *) frame.payload.body_fragment.bytes);
			received += frame.payload.body_fragment.len;
			index = payload+received;
		}

		amqp_basic_properties_t props;
		props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG |
                AMQP_BASIC_DELIVERY_MODE_FLAG |
                AMQP_BASIC_REPLY_TO_FLAG |
                AMQP_BASIC_CORRELATION_ID_FLAG;
		props.content_type = amqp_cstring_bytes("text/plain");
		props.delivery_mode = 2;
		props.reply_to = p->reply_to;
		props.correlation_id = amqp_cstring_bytes("2345");
		status = amqp_basic_publish(conn, 1, amqp_cstring_bytes(""), p->reply_to, 0, 0, &props,  amqp_cstring_bytes("fuck you"));

	}
  amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
  amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
  amqp_destroy_connection(conn);

  return 0;
}
