/*********************************************************************
   PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
   See LICENSE and COPYING for usage.

   Author: Andrei Carp <andrei.carp@tass.be>
 *********************************************************************/
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "pico_tree.h"
#include "pico_config.h"
#include "pico_socket.h"
#include "pico_tcp.h"
#include "pico_dns_client.h"
#include "pico_http_client.h"
#include "pico_http_util.h"
#include "pico_ipv4.h"
#include "pico_stack.h"
/*
 * This is the size of the following header
 *
 * GET <resource> HTTP/1.1<CRLF>
 * Host: <host>:<port><CRLF>
 * User-Agent: picoTCP<CRLF>
 * Connection: closei/Keep-Alive<CRLF>
 * <CRLF>
 *
 * where <resource>,<host> and <port> will be added later.
 */

#define HTTP_GET_BASIC_SIZE                 70u
#define HTTP_POST_BASIC_SIZE                256u
#define HTTP_POST_MULTIPART_BASIC_SIZE      80u
#define HTTP_POST_HEADER_BASIC_SIZE         160u
#define HTTP_DELETE_BASIC_SIZE              70u
#define HTTP_HEADER_LINE_SIZE               50u
#define HTTP_MAX_FIXED_POST_MULTIPART_CHUNK 100u
#define RESPONSE_INDEX                      9u

#define HTTP_CHUNK_ERROR    0xFFFFFFFFu
/*
#ifdef dbg
    #undef dbg
#endif

#define dbg(...) do {} while(0)
*/
#define nop() do {} while(0)

#define consume_char(c)                          (pico_socket_read(client->sck, &c, 1u))
#define is_location(line)                        (memcmp(line, "Location", 8u) == 0)
#define is_content_length(line)           (memcmp(line, "Content-Length", 14u) == 0u)
#define is_transfer_encoding(line)        (memcmp(line, "Transfer-Encoding", 17u) == 0u)
#define is_chunked(line)                         (memcmp(line, " chunked", 8u) == 0u)
#define is_not_HTTPv1(line)                       (memcmp(line, "HTTP/1.", 7u))
#define is_hex_digit(x) ((('0' <= x) && (x <= '9')) || (('a' <= x) && (x <= 'f')))
#define hex_digit_to_dec(x) ((('0' <= x) && (x <= '9')) ? (x - '0') : ((('a' <= x) && (x <= 'f')) ? (x - 'a' + 10) : (-1)))

static uint16_t global_client_conn_ID = 0;

struct request_part
{
    uint8_t *buf;
    uint32_t buf_len;
    uint32_t buf_len_done;
    uint8_t copy;
    uint8_t mem;
};

struct pico_http_client
{
    uint16_t connectionID;
    uint8_t state;
    uint32_t body_read;
    uint8_t body_read_done;
    struct pico_socket *sck;
    void (*wakeup)(uint16_t ev, uint16_t conn);
    struct pico_ip4 ip;
    struct pico_http_uri *urikey;
    struct pico_http_header *header;
    struct request_part **request_parts;
    uint16_t request_parts_len_done;
    uint16_t request_parts_len;
    uint16_t request_parts_idx;
    uint8_t long_polling_state;
};

/* HTTP Client internal states */
#define HTTP_CONN_IDLE                  0
#define HTTP_START_READING_HEADER       1
#define HTTP_READING_HEADER             2
#define HTTP_READING_BODY               3
#define HTTP_READING_CHUNK_VALUE        4
#define HTTP_READING_CHUNK_TRAIL        5
#define HTTP_WRITING_REQUEST            6

/* HTTP Long Polling States */
#define HTTP_LONG_POLL_CONN_CLOSE       1
#define HTTP_LONG_POLL_CONN_KEEP_ALIVE  2
#define HTTP_LONG_POLL_CANCEL           3

/* MISC */
#define HTTP_NO_COPY_TO_HEAP    0
#define HTTP_COPY_TO_HEAP       1
#define HTTP_USER_MEM           2
#define HTTP_NO_USER_MEM        3

/* HTTP URI string parsing */
#define HTTP_PROTO_TOK      "http://"
#define HTTP_PROTO_LEN      7u

static int8_t free_uri(struct pico_http_client *to_be_removed);
static int32_t client_open(uint8_t *uri, void (*wakeup)(uint16_t ev, uint16_t conn), int32_t connID);
static void free_header(struct pico_http_client *to_be_removed);

struct request_part *request_part_create(uint8_t *buf, uint32_t buf_len, uint8_t copy, uint8_t mem)
{
    struct request_part *part = PICO_ZALLOC(sizeof(struct request_part));
    if (!part)
    {
        /* not enought memory */
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }

    if (buf)
    {
        if (copy == HTTP_COPY_TO_HEAP)
        {
            part->buf = PICO_ZALLOC(buf_len);
            if (!part->buf)
            {
                PICO_FREE(part);
                pico_err = PICO_ERR_ENOMEM;
                return NULL;
            }
            memcpy(part->buf, buf, buf_len);
        }
        else
        {
            part->buf = buf;
        }
        part->copy = copy;
        part->buf_len = buf_len;
        part->buf_len_done = 0;
        part->mem = mem;
        return part;
    }
    else
    {
        // TODO welke pico_err??
        PICO_FREE(part);
        return NULL;
    }
}

static void print_request_part_info(struct pico_http_client *client)
{
    uint32_t i = 0;
    dbg("request_parts_len: %d request_parts_len_done: %d\n", client->request_parts_len, client->request_parts_len_done);
    for (i=0; i<client->request_parts_len; i++)
    {
        dbg("i=%d buf_len: %d buf_len_done: %d buf: %p\n", i, client->request_parts[i]->buf_len, client->request_parts[i]->buf_len_done, client->request_parts[i]->buf);
    }
}

//User memory buffers will not be freed
static int8_t request_parts_destroy(struct pico_http_client *client)
{
    uint32_t i = 0;
    if (!client)
    {
        return HTTP_RETURN_ERROR;
    }

    for (i=0; i<client->request_parts_len; i++)
    {
        if (client->request_parts[i]->copy == HTTP_COPY_TO_HEAP || client->request_parts[i]->mem == HTTP_NO_USER_MEM)
        {
            PICO_FREE(client->request_parts[i]->buf);
        }
        PICO_FREE(client->request_parts[i]);
    }
    PICO_FREE(client->request_parts);
    client->request_parts = NULL;
    client->request_parts_len_done = 0;
    client->request_parts_len = 0;
    client->request_parts_idx = 0;
    return HTTP_RETURN_OK;
}

static int32_t socket_write_request_parts(struct pico_http_client *client)
{
    uint32_t bytes_written = 0;
    uint32_t i = 0;
    uint32_t bytes_to_write = 0;
    uint32_t idx = 0;

    client->state = HTTP_WRITING_REQUEST;
    for (i = client->request_parts_len_done; i<client->request_parts_len; i++)
    {
        bytes_to_write = client->request_parts[i]->buf_len - client->request_parts[i]->buf_len_done;
        idx = client->request_parts[i]->buf_len_done;
        bytes_written = pico_socket_write(client->sck, (void *)&client->request_parts[i]->buf[idx], bytes_to_write);
        client->request_parts[i]->buf_len_done += bytes_written;
        /*uint32_t x = 0;
        for (x=0; x<bytes_to_write; x++)
        {
            printf("client->request_parts[i]->buf[%d] = %c\n", x, client->request_parts[i]->buf[x]);
        }
*/
        if (bytes_written < 0)
        {
            request_parts_destroy(client);
            client->state = HTTP_CONN_IDLE;
            client->wakeup(EV_HTTP_WRITE_FAILED, client->connectionID);
        }
        else if (bytes_written == bytes_to_write)
        {
            client->request_parts_len_done += 1;
        }
        else
        {
            dbg("Could not fully write complete request.\n");
            break;
        }
        if (client->request_parts_len_done==client->request_parts_len)
        {
            print_request_part_info(client);
            request_parts_destroy(client);
            client->state = HTTP_START_READING_HEADER;
            client->wakeup(EV_HTTP_WRITE_SUCCESS, client->connectionID);
        }
    }
    return bytes_written;
}

static int8_t pico_process_uri(const uint8_t *uri, struct pico_http_uri *urikey)
{

    uint16_t last_index = 0, index;
    if (!uri || !urikey || uri[0] == '/')
    {
        pico_err = PICO_ERR_EINVAL;
        goto error;
    }
    urikey->raw = (uint8_t *)PICO_ZALLOC(strlen(uri));
    if (!urikey->raw)
    {
        pico_err = PICO_ERR_ENOMEM;
        goto error;
    }
    strcpy(urikey->raw, uri);

    /* detect protocol => search for  "colon-slash-slash" */
    if (memcmp(uri, HTTP_PROTO_TOK, HTTP_PROTO_LEN) == 0) /* could be optimized */
    { /* protocol identified, it is http */
        urikey->protoHttp = 1;
        last_index = HTTP_PROTO_LEN;
    }
    else
    {
        if (strstr(uri, "://")) /* different protocol specified */
        {
            urikey->protoHttp = 0;
            goto error;
        }

        /* no protocol specified, assuming by default it's http */
        urikey->protoHttp = 1;
    }

    /* detect hostname */
    index = last_index;
    while (uri[index] && uri[index] != '/' && uri[index] != ':') index++;
    if (index == last_index)
    {
        /* wrong format */
        urikey->host = urikey->resource = NULL;
        urikey->port = urikey->protoHttp = 0u;
        pico_err = PICO_ERR_EINVAL;
        goto error;
    }
    else
    {
        /* extract host */
        urikey->host = (uint8_t *)PICO_ZALLOC((uint32_t)(index - last_index + 1));

        if(!urikey->host)
        {
            /* no memory */
            pico_err = PICO_ERR_ENOMEM;
            goto error;
        }

        memcpy(urikey->host, uri + last_index, (size_t)(index - last_index));
    }

    if (!uri[index])
    {
        /* nothing specified */
        urikey->port = 80u;
        urikey->resource = PICO_ZALLOC(2u);
        if (!urikey->resource) {
            /* no memory */
            pico_err = PICO_ERR_ENOMEM;
            goto error;
        }

        urikey->resource[0] = '/';
        return HTTP_RETURN_OK;
    }
    else if (uri[index] == '/')
    {
        urikey->port = 80u;
    }
    else if (uri[index] == ':')
    {
        urikey->port = 0u;
        index++;
        while (uri[index] && uri[index] != '/')
        {
            /* should check if every component is a digit */
            urikey->port = (uint16_t)(urikey->port * 10 + (uri[index] - '0'));
            index++;
        }
    }

    /* extract resource */
    if (!uri[index])
    {
        urikey->resource = PICO_ZALLOC(2u);
        if (!urikey->resource) {
            /* no memory */
            pico_err = PICO_ERR_ENOMEM;
            goto error;
        }

        urikey->resource[0] = '/';
    }
    else
    {
        last_index = index;
        while (uri[index] /*&& uri[index] != '?' && uri[index] != '&' && uri[index] != '#'*/) index++;
        urikey->resource = (uint8_t *)PICO_ZALLOC((size_t)(index - last_index + 1));

        if (!urikey->resource)
        {
            /* no memory */
            pico_err = PICO_ERR_ENOMEM;
            goto error;
        }

        memcpy(urikey->resource, uri + last_index, (size_t)(index - last_index));
    }

    return HTTP_RETURN_OK;

error:
    if (urikey)
    {
        if (urikey->resource)
        {
            PICO_FREE(urikey->resource);
            urikey->resource = NULL;
        }

        if (urikey->raw)
        {
            PICO_FREE(urikey->raw);
            urikey->raw = NULL;
        }

        if (urikey->host)
        {
            PICO_FREE(urikey->host);
            urikey->host = NULL;
        }
    }
    return HTTP_RETURN_ERROR;
}

static int32_t compare_clients(void *ka, void *kb)
{
    return ((struct pico_http_client *)ka)->connectionID - ((struct pico_http_client *)kb)->connectionID;
}

PICO_TREE_DECLARE(pico_client_list, compare_clients);

/* Local functions */
static int8_t parse_header_from_server(struct pico_http_client *client, struct pico_http_header *header);
static int8_t read_chunk_line(struct pico_http_client *client);
/*  */

static inline void wait_for_header(struct pico_http_client *client)
{
    /* wait for header */
    int8_t http_ret;

    http_ret = parse_header_from_server(client, client->header);
    if (http_ret < 0)
    {
        client->wakeup(EV_HTTP_ERROR, client->connectionID);
    }
    else if (http_ret == HTTP_RETURN_BUSY)
    {
        client->state = HTTP_READING_HEADER;
    }
    else if (http_ret == HTTP_RETURN_NOT_FOUND)
    {
        client->state = HTTP_CONN_IDLE;
        client->body_read = 0;
        client->wakeup(EV_HTTP_REQ, client->connectionID);
    }
    else
    {
        /* call wakeup */
        if (client->header->response_code != HTTP_CONTINUE)
        {
            if (client->header->response_code == HTTP_OK)
            {
                client->wakeup((EV_HTTP_REQ | EV_HTTP_BODY), client->connectionID);
            }
            else
            {
                client->state = HTTP_CONN_IDLE;
                client->body_read = 0;
                client->wakeup(EV_HTTP_REQ, client->connectionID);
            }
        }
    }
}

static void treat_write_event(struct pico_http_client *client)
{
    /* write request parts if not everything has been written allready */
    //dbg("treat write event, client state: %d\n", client->state);
    uint32_t bytes_written = 0;
    if (client->request_parts_len_done != client->request_parts_len)
    {
        bytes_written = socket_write_request_parts(client);
        dbg("Bytes written: %d\n", bytes_written);
        if (bytes_written && !client->long_polling_state)
        {
            client->wakeup(EV_HTTP_WRITE_PROGRESS_MADE, client->connectionID);
        }
    }
    else
    {
        //dbg("No request parts to write.\n");
    }
}

static void treat_read_event(struct pico_http_client *client)
{
    /* read the header, if not read */
    dbg("treat read event, client state: %d\n", client->state);
    if (client->state == HTTP_START_READING_HEADER)
    {
        /* wait for header */
        free_header(client); //when using keep alive, we create a new one
        client->header = PICO_ZALLOC(sizeof(struct pico_http_header));
        if (!client->header)
        {
            pico_err = PICO_ERR_ENOMEM;
            return;
        }

        wait_for_header(client);
    }
    else if (client->state == HTTP_READING_HEADER)
    {
        wait_for_header(client);
    }
    else
    {
        /* just let the user know that data has arrived, if chunked data comes, will be treated in the */
        /* read api. */
        client->wakeup(EV_HTTP_BODY, client->connectionID);
    }
}

static void treat_long_polling(struct pico_http_client *client, uint16_t ev)
{
    uint8_t rv = 0;
    uint32_t conn = 0;
    uint8_t *raw = NULL;
    void *wakeup = NULL;
    dbg("TREAT LONG POLLING\n");

    conn = client->connectionID;
    if ((ev & PICO_SOCK_EV_CLOSE) || (ev & PICO_SOCK_EV_FIN))
    {
        if (client->long_polling_state != HTTP_LONG_POLL_CONN_CLOSE)
        {
            dbg("Connection: Keep-Alive, but still got close ev, setup new connection.");
        }
        raw = PICO_ZALLOC(strlen(client->urikey->raw));
        if (!raw)
        {
            //ERROR  TODO call_back with HTTP_LONG_POLL_ERROR??
            pico_err = PICO_ERR_ENOMEM;
        }
        strcpy(raw, client->urikey->raw);
        wakeup = client->wakeup;
        rv = pico_http_client_close(client->connectionID);
        conn = client_open(raw, wakeup, conn);
        PICO_FREE(raw);
        if (conn < 0)
        {
            //ERROR  TODO call_back with HTTP_LONG_POLL_ERROR??
        }
    }

    if (client->long_polling_state == HTTP_LONG_POLL_CONN_CLOSE)
    {
        rv = pico_http_client_long_poll_send_get(conn, HTTP_CONN_CLOSE);
    }
    else
    {
        rv = pico_http_client_long_poll_send_get(conn, HTTP_CONN_KEEP_ALIVE);
    }

}

static void tcp_callback(uint16_t ev, struct pico_socket *s)
{
    struct pico_http_client *client = NULL;
    struct pico_tree_node *index;
    int16_t r_ev = 0;
    dbg("tcp callback (%d)\n", ev);
    /* find http_client */
    pico_tree_foreach(index, &pico_client_list)
    {
        if (((struct pico_http_client *)index->keyValue)->sck == s )
        {
            client = (struct pico_http_client *)index->keyValue;
            break;
        }
    }

    if (!client)
    {
        dbg("Client not found...Something went wrong !\n");
        return;
    }

    if (!client)
    {
        return;
    }

    if (ev & PICO_SOCK_EV_CONN)
    {
        client->wakeup(EV_HTTP_CON, client->connectionID);
    }

    if (ev & PICO_SOCK_EV_ERR)
    {
        r_ev = EV_HTTP_ERROR;
        if (client->request_parts)
        {
            request_parts_destroy(client);
            client->wakeup(EV_HTTP_WRITE_FAILED, client->connectionID);
            r_ev = r_ev | EV_HTTP_WRITE_FAILED;
        }
        client->state = HTTP_CONN_IDLE;
        client->wakeup(r_ev, client->connectionID);
    }

    if ((ev & PICO_SOCK_EV_CLOSE) || (ev & PICO_SOCK_EV_FIN))
    {
        r_ev = EV_HTTP_CLOSE;
        if (client->request_parts)
        {
            request_parts_destroy(client);
            client->wakeup(EV_HTTP_WRITE_FAILED, client->connectionID);
            r_ev = r_ev | EV_HTTP_WRITE_FAILED;
        }
        client->state = HTTP_CONN_IDLE;
        //long polling
        dbg("long polling state %d\n", client->long_polling_state);
        if (client->long_polling_state)
        {
            treat_long_polling(client, ev);
        }
        else
        {
            client->wakeup(r_ev, client->connectionID);
        }
    }

    if (ev & PICO_SOCK_EV_WR)
    {
        treat_write_event(client);
    }

    if (ev & PICO_SOCK_EV_RD)
    {
        treat_read_event(client);
    }
}

/* used for getting a response from DNS servers */
static void dns_callback(uint8_t *ip, void *ptr)
{
    struct pico_http_client *client = (struct pico_http_client *)ptr;

    if (!client)
    {
        dbg("Who made the request ?!\n");
        return;
    }

    if (ip)
    {
        client->wakeup(EV_HTTP_DNS, client->connectionID);

        /* add the ip address to the client, and start a tcp connection socket */
        pico_string_to_ipv4(ip, &client->ip.addr);
        client->sck = pico_socket_open(PICO_PROTO_IPV4, PICO_PROTO_TCP, &tcp_callback);
        if (!client->sck)
        {
            client->wakeup(EV_HTTP_ERROR, client->connectionID);
            return;
        }
        if (pico_socket_connect(client->sck, &client->ip, short_be(client->urikey->port)) < 0)
        {
            client->wakeup(EV_HTTP_ERROR, client->connectionID);
            return;
        }
    }
    else
    {
        /* wakeup client and let know error occured */
        client->wakeup(EV_HTTP_ERROR, client->connectionID);

        /* close the client (free used heap) */
        pico_http_client_close(client->connectionID);
    }
}

/*
 * API for reading received data.
 *
 * Builds a GET request based on the fields on the uri.
 */
int8_t *pico_http_client_build_get(const struct pico_http_uri *uri_data, uint8_t connection)
{
    uint8_t *header;
    uint8_t port[6u]; /* 6 = max length of a uint16 + \0 */

    uint64_t header_size = HTTP_GET_BASIC_SIZE;

    if (!uri_data->host || !uri_data->resource || !uri_data->port)
    {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }

    /*  */
    header_size = (header_size + strlen(uri_data->host));
    header_size = (header_size + strlen(uri_data->resource));
    header_size = (header_size + pico_itoa(uri_data->port, port) + 4u); /* 3 = size(CRLF + \0) */
    header = PICO_ZALLOC(header_size);

    if (!header)
    {
        /* not enought memory */
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }

    /* build the actual header */
    strcpy(header, "GET ");
    strcat(header, uri_data->resource);
    strcat(header, " HTTP/1.1\r\n");
    strcat(header, "Host: ");
    strcat(header, uri_data->host);
    strcat(header, ":");
    strcat(header, port);
    strcat(header, "\r\n");
    strcat(header, "User-Agent: picoTCP\r\n");
    if (connection == HTTP_CONN_CLOSE)
    {
        strcat(header, "Connection: close\r\n\r\n");
    }
    else
    {
        strcat(header, "Connection: Keep-Alive\r\n\r\n");
    }
    return header;
}


/*
 * Builds a DELETE header based on the fields of the uri
 */
static int8_t *pico_http_client_build_delete(const struct pico_http_uri *uri_data, uint8_t connection)
{
    uint8_t *header;
    uint8_t port[6u]; /* 6 = max length of a uint16 + \0 */
    uint64_t header_size = HTTP_DELETE_BASIC_SIZE;

    if (!uri_data->host || !uri_data->resource || !uri_data->port)
    {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }

    /*  */
    header_size = (header_size + strlen(uri_data->host));
    header_size = (header_size + strlen(uri_data->resource));
    header_size = (header_size + pico_itoa(uri_data->port, port) + 4u); /* 3 = size(CRLF + \0) */
    header = PICO_ZALLOC(header_size);

    if (!header)
    {
        /* not enought memory */
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }

    /* build the actual header */
    strcpy(header, "DELETE ");
    strcat(header, uri_data->resource);
    strcat(header, " HTTP/1.1\r\n");
    strcat(header, "User-Agent: picoTCP\r\n");
    strcat(header, "Accept: */*\r\n");
    strcat(header, "Host: ");
    strcat(header, uri_data->host);
    strcat(header, ":");
    strcat(header, port);
    strcat(header, "\r\n");
    if (connection == HTTP_CONN_CLOSE)
    {
        strcat(header, "Connection: close\r\n\r\n");
    }
    else
    {
        strcat(header, "Connection: Keep-Alive\r\n\r\n");
    }
    return header;
}


struct multipart_chunk *multipart_chunk_create(uint8_t *data, uint64_t length_data, uint8_t *name, uint8_t *filename, uint8_t *content_disposition, uint8_t *content_type)
{
    if (length_data <= 0 || data == NULL)
    {
        return NULL;
    }

    struct multipart_chunk *chunk = PICO_ZALLOC(sizeof(struct multipart_chunk));

    if (!chunk)
    {
        /* not enought memory */
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }
    if (data)
    {
        chunk->data = PICO_ZALLOC(length_data);
        memcpy(chunk->data, data, length_data);
        chunk->length_data = length_data;
    }
    if (name)
    {
        chunk->name = strdup(name);
        chunk->length_name = strlen(name);
    }
    if (content_disposition)
    {
        chunk->content_disposition = strdup(content_disposition);
        chunk->length_content_disposition = strlen(content_disposition);
    }
    if (filename)
    {
        chunk->filename = strdup(filename);
        chunk->length_filename = strlen(filename);
    }
    if (content_type)
    {
        chunk->content_type = strdup(content_type);
        chunk->length_content_type = strlen(content_type);
    }
    return chunk;
}

int8_t multipart_chunk_destroy(struct multipart_chunk *chunk)
{
    if (!chunk)
    {
        return -1;
    }
    PICO_FREE(chunk->data);
    PICO_FREE(chunk->name);
    PICO_FREE(chunk->filename);
    PICO_FREE(chunk->content_type);
    PICO_FREE(chunk->content_disposition);
    PICO_FREE(chunk);
    return 0;
}

static int32_t get_content_length(struct multipart_chunk **post_data, uint16_t length, uint8_t *boundary)
{
    uint32_t i;
    uint64_t total_size = 0;
    for (i=0; i<length; i++)
    {
        if (post_data[i]->data != NULL)
        {
            total_size += 2; // "--"
            total_size += strlen(boundary);
            total_size += 2; // "\r\n"
            if (post_data[i]->content_disposition != NULL)
            {
                total_size += 21 ; // "Content-Disposition: "
                total_size += post_data[i]->length_content_disposition;
                if (post_data[i]->name != NULL)
                {
                    total_size += 8; // "; name=\""
                    total_size += post_data[i]->length_name;
                    total_size += 1; // "\""
                }
                if (post_data[i]->filename != NULL)
                {
                    total_size += 12; // "; filename=\""
                    total_size += post_data[i]->length_filename;
                    total_size += 1; // "\""
                }
            }
            if (post_data[i]->content_type != NULL)
            {
                total_size += 2; // "\r\n"
                total_size += 14; // "Content-type: "
                total_size += post_data[i]->length_content_type;
            }
            total_size += 4; // "\r\n\r\n"
            total_size += post_data[i]->length_data;
            total_size += 2; // "\r\n"
        }
    }
    total_size += 2; // "--"
    total_size += strlen(boundary);
    total_size += 4; // "--\r\n"
    return total_size;
}

static int32_t get_max_multipart_header_size(struct multipart_chunk **post_data, uint16_t length)
{
    uint32_t max = 0;
    uint32_t len = 0;
    uint32_t i = 0;
    for (i=0; i<length; i++)
    {
        len = 0;
        len += post_data[i]->length_content_disposition;
        len += post_data[i]->length_name;
        len += post_data[i]->length_filename;
        len += post_data[i]->length_content_type;
        if (max < len)
        {
            max = len;
        }
    }
    return max;
}

static int8_t add_multipart_chunks(struct multipart_chunk **post_data, uint16_t post_data_length, uint8_t *boundary, struct pico_http_client *http)
{
    uint32_t i = 0;
    uint32_t bytes_written = 0;
    uint32_t idx = 0;
    uint8_t *buf = NULL;
    uint32_t buf_size = HTTP_POST_MULTIPART_BASIC_SIZE;

    buf_size += get_max_multipart_header_size(post_data, post_data_length);
    buf_size += strlen(boundary);
    buf = PICO_ZALLOC(buf_size);

    if(!buf)
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    for (i=0; i<post_data_length; i++)
    {
        if (post_data[i]->data != NULL)
        {
            if (i == 0)
                strcpy(buf,"--");
            else
                strcat(buf,"--");
            strcat(buf, boundary);
            strcat(buf, "\r\n");
            if (post_data[i]->content_disposition != NULL)
            {
                strcat(buf, "Content-Disposition: ");
                strcat(buf, post_data[i]->content_disposition);
                if (post_data[i]->name != NULL)
                {
                    strcat(buf, "; name=\"");
                    strcat(buf, post_data[i]->name);
                    strcat(buf, "\"");
                }
                if (post_data[i]->filename != NULL)
                {
                    strcat(buf, "; filename=\"");
                    strcat(buf, post_data[i]->filename);
                    strcat(buf, "\"");
                }
            }
            if (post_data[i]->content_type != NULL)
            {
                strcat(buf, "\r\n");
                strcat(buf, "Content-type: ");
                strcat(buf, post_data[i]->content_type);
            }
            strcat(buf, "\r\n\r\n");

            http->request_parts[http->request_parts_len] = request_part_create(buf, strlen(buf), HTTP_COPY_TO_HEAP, HTTP_NO_USER_MEM);
            if (!http->request_parts[http->request_parts_len])
            {
                PICO_FREE(buf);
                pico_err = PICO_ERR_ENOMEM;
                return HTTP_RETURN_ERROR;
            }
            http->request_parts_len += 1;

            http->request_parts[http->request_parts_len] = request_part_create(post_data[i]->data, post_data[i]->length_data, HTTP_NO_COPY_TO_HEAP, HTTP_USER_MEM);
            if (!http->request_parts[http->request_parts_len])
            {
                PICO_FREE(buf);
                pico_err = PICO_ERR_ENOMEM;
                return HTTP_RETURN_ERROR;
            }
            http->request_parts_len += 1;
            strcpy(buf, "\r\n");
        }
    }
    strcat(buf,"--");
    strcat(buf, boundary);
    strcat(buf, "--\r\n");

    http->request_parts[http->request_parts_len] = request_part_create(buf, strlen(buf), HTTP_COPY_TO_HEAP, HTTP_USER_MEM);
    if (!http->request_parts[http->request_parts_len])
    {
        PICO_FREE(buf);
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len += 1;
    PICO_FREE(buf);
    return HTTP_RETURN_OK;
}

static int8_t pico_http_client_build_post_multipart_request(const struct pico_http_uri *uri_data, struct multipart_chunk **post_data, uint16_t len, struct pico_http_client *http, uint8_t connection)
{
    uint8_t *header = NULL;
    uint32_t content_length = 0;
    uint32_t data_length = 0;
    uint8_t port[6u]; /* 6 = max length of a uint16 + \0 */
    uint32_t header_size = HTTP_POST_HEADER_BASIC_SIZE;
    uint8_t *boundary = "--------------------------c6b5ca0828dmx010";
    uint8_t str_content_length[6u];
    uint16_t i = 0;
    uint8_t rv = HTTP_RETURN_OK;

    if (!uri_data->host || !uri_data->resource || !uri_data->port)
    {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
    content_length = get_content_length(post_data, len, boundary);
    sprintf(str_content_length, "%d", content_length);
    for (i=0; i<len; i++)
    {
        if (post_data[i]->data)
        {
            data_length += post_data[i]->length_data;
        }
    }

    header_size = (header_size + strlen(uri_data->host));
    header_size = (header_size + strlen(uri_data->resource));
    header_size = (header_size + pico_itoa(uri_data->port, port) + 4u); /* 3 = size(CRLF + \0) */
    header_size = (header_size + strlen(boundary));
    header = PICO_ZALLOC(header_size);
    if (!header)
    {
        /* not enought memory */
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    /* build the actual header */
    strcpy(header, "POST ");
    strcat(header, uri_data->resource);
    strcat(header, " HTTP/1.1\r\n");
    strcat(header, "User-Agent: picoTCP\r\n");
    strcat(header, "Accept: */*\r\n");
    strcat(header, "Host: ");
    strcat(header, uri_data->host);
    strcat(header, ":");
    strcat(header, port);
    strcat(header, "\r\n");
    if (connection == HTTP_CONN_CLOSE)
    {
        strcat(header, "Connection: Close\r\n");
    }
    else
    {
        strcat(header, "Connection: Keep-Alive\r\n");
    }
    strcat(header, "Content-Length: ");
    strcat(header, str_content_length);
    strcat(header, "\r\n");
    strcat(header, "Content-Type: multipart/mixed; boundary=");
    strcat(header, boundary);
    strcat(header, "\r\n\r\n");
    http->request_parts[http->request_parts_len] = request_part_create(header, strlen(header), HTTP_NO_COPY_TO_HEAP, HTTP_NO_USER_MEM);
    if (!http->request_parts[0])
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len += 1;
    rv = add_multipart_chunks(post_data, len, boundary, http);
    return rv;
}

/*
 * Builds a POST header based on the fields of the uri provided.
 */
static int8_t *pico_http_client_build_post_header(const struct pico_http_uri *uri_data, uint32_t post_data_len, uint8_t connection, uint8_t *content_type, uint8_t *cache_control)
{
    uint8_t *header;
    uint8_t port[6u]; /* 6 = max length of a uint16 + \0 */
    uint64_t header_size = HTTP_POST_BASIC_SIZE;
    uint8_t str_post_data_len[6u];

    if (!uri_data->host || !uri_data->resource || !uri_data->port)
    {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }
    sprintf(str_post_data_len, "%d", post_data_len);
    /*  */
    header_size += (header_size + strlen(uri_data->host));
    header_size += (header_size + strlen(uri_data->resource));
    header_size += (header_size + pico_itoa(uri_data->port, port) + 4u); /* 3 = size(CRLF + \0) */
    header_size += 6u; //str_post_data_len
    header = PICO_ZALLOC(header_size);

    if (!header)
    {
        /* not enought memory */
        pico_err = PICO_ERR_ENOMEM;
        return NULL;
    }

    /* build the actual header */
    strcpy(header, "POST ");
    strcat(header, uri_data->resource);
    strcat(header, " HTTP/1.1\r\n");
    strcat(header, "User-Agent: picoTCP\r\n");
    strcat(header, "Accept: */*\r\n");
    strcat(header, "Host: ");
    strcat(header, uri_data->host);
    strcat(header, ":");
    strcat(header, port);
    strcat(header, "\r\n");
    if (connection == HTTP_CONN_CLOSE)
    {
        strcat(header, "Connection: Close\r\n");
    }
    else
    {
        strcat(header, "Connection: Keep-Alive\r\n");
    }
    if (content_type == NULL)
    {
        strcat(header, "Content-Type: application/x-www-form-urlencoded\r\n");
    }
    else
    {
        strcat(header, "Content-Type: ");
        strcat(header, content_type);
        strcat(header, "\r\n");
    }
    if (cache_control == NULL)
    {
        strcat(header, "Cache-Control: private, max-age=0, no-cache\r\n");
    }
    else
    {
        strcat(header, "Cache-Control: ");
        strcat(header, cache_control);
        strcat(header, "\r\n");
    }
    strcat(header, "Content-Length: ");
    strcat(header, str_post_data_len);
    strcat(header, "\r\n\r\n");
    return header;
}
/*  */

/*
 * API used for to check how many bytes are allready written.
 *
 * The acceptes the connectionID and 2 pointers to store the number of written bytes and total bytes to write
 *
 * The function returns -1 if connectionID is not found or there is nothing to write anymore.
 * 'Total_bytes_to_write' can be NULL
 */
int32_t MOCKABLE pico_http_client_get_write_progress(uint16_t conn, uint32_t *total_bytes_written, uint32_t *total_bytes_to_write)
{
    uint8_t *request = NULL;
    uint32_t i = 0;
    struct pico_http_client search = {
        .connectionID = conn
    };
    struct pico_http_client *client = pico_tree_findKey(&pico_client_list, &search);
    if (!client)
    {
        dbg("Client not found !\n");
        return HTTP_RETURN_ERROR;
    }
    *total_bytes_written = 0;
    *total_bytes_to_write = 0;

    if (client->request_parts)
    {
        for (i=0; i<client->request_parts_len; i++)
        {
            *total_bytes_written += client->request_parts[i]->buf_len_done;
            if (total_bytes_to_write)
            {
                *total_bytes_to_write += client->request_parts[i]->buf_len;
            }
        }
        return HTTP_RETURN_OK;
    }
    else
    {
        return HTTP_RETURN_ERROR;
    }

}

static int32_t client_open(uint8_t *uri, void (*wakeup)(uint16_t ev, uint16_t conn), int32_t connID)
{
    struct pico_http_client *client;
    uint32_t ip = 0;

    if (!wakeup || !uri)
    {
        return HTTP_RETURN_ERROR;
    }

    client = PICO_ZALLOC(sizeof(struct pico_http_client));
    if (!client)
    {
        /* memory error */
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }

    client->wakeup = wakeup;
    if (connID >= 0)
    {
        client->connectionID = connID;
    }
    else
    {
        client->connectionID = global_client_conn_ID++;
    }

    client->urikey = PICO_ZALLOC(sizeof(struct pico_http_uri));

    if (!client->urikey)
    {
        pico_err = PICO_ERR_ENOMEM;
        PICO_FREE(client);
        return HTTP_RETURN_ERROR;
    }

    pico_process_uri(uri, client->urikey);
    if (pico_tree_insert(&pico_client_list, client))
    {
        /* already in */
        pico_err = PICO_ERR_EEXIST;
        free_uri(client);
        PICO_FREE(client);
        return HTTP_RETURN_ALREADYIN;
    }

    /* dns query */
    if (pico_string_to_ipv4(client->urikey->host, &ip) == -1)
    {
        dbg("Querying : %s \n", client->urikey->host);
        pico_dns_client_getaddr(client->urikey->host, (void *)dns_callback, client);
    }
    else
    {
        dbg("host already and ip address, no dns required\n");
        dns_callback(client->urikey->host, client);
    }

    /* return the connection ID */
    return client->connectionID;
}


/*
 * API used for opening a new HTTP Client.
 *
 * The accepted uri's are [http:]hostname[:port]/resource
 * no relative uri's are accepted.
 *
 * The function returns a connection ID >= 0 if successful
 * -1 if an error occured.
 */
int32_t MOCKABLE pico_http_client_open(uint8_t *uri, void (*wakeup)(uint16_t ev, uint16_t conn))
{
    return client_open(uri, wakeup, -1);
}


/*
 * API for sending a header POST multipart to the client.
 *
 * The library will build the response request
 * based on the uri/post_data passed when opening the client.
 *
 * POST request:
 *  post_data: pointer to multipart_chunk
 *  length_post_data: length of the multipart_chunk array
 */
int8_t MOCKABLE pico_http_client_send_post_multipart(uint16_t conn, struct multipart_chunk **post_data, uint16_t length_post_data, uint8_t connection)
{
    dbg("POST MULTIPART request\n");
    struct pico_http_client search = {
        .connectionID = conn
    };
    struct pico_http_client *http = pico_tree_findKey(&pico_client_list, &search);
    int32_t bytes_written;
    int8_t rv = 0;
    if (!http)
    {
        dbg("Client not found !\n");
        return HTTP_RETURN_ERROR;
    }
    if (http->state != HTTP_CONN_IDLE)
    {
        return HTTP_RETURN_CONN_BUSY;
    }
    if (connection != HTTP_CONN_CLOSE && connection != HTTP_CONN_KEEP_ALIVE)
    {
        return HTTP_RETURN_ERROR;
    }
    http->request_parts = PICO_ZALLOC((2+length_post_data*2) * sizeof(struct request_part *)); //header + end_boundary + 2*length_post_data
    if (!http->request_parts)
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len = 0;
    http->request_parts_len_done = 0;

    /* the api gives the possibility to the user to build the POST multipart header */
    /* based on the uri passed when opening the client, less headache for the user */
    rv = pico_http_client_build_post_multipart_request(http->urikey, post_data, length_post_data, http, connection);
    if (rv == HTTP_RETURN_ERROR)
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    bytes_written = socket_write_request_parts(http);
    dbg("bytes written: %d\n", bytes_written);
    return HTTP_RETURN_OK;
}

/*
 * API for sending a DELETE request.
 *
 * The library will build the request
 * based on the uri passed when opening the client.
 * Retruns HTTP_RETURN_CONN_BUSY (-3) if the connection is still writing or reading data
 */
int8_t MOCKABLE pico_http_client_send_delete(uint16_t conn, uint8_t connection)
{
    dbg("DELETE request\n");
    uint8_t *request = NULL;
    struct pico_http_client search = {
        .connectionID = conn
    };
    struct pico_http_client *http = pico_tree_findKey(&pico_client_list, &search);
    int32_t bytes_written = 0;
    if (!http)
    {
        dbg("Client not found !\n");
        return HTTP_RETURN_ERROR;
    }
    if (http->state != HTTP_CONN_IDLE)
    {
        return HTTP_RETURN_CONN_BUSY;
    }
    if (connection != HTTP_CONN_CLOSE && connection != HTTP_CONN_KEEP_ALIVE)
    {
        return HTTP_RETURN_ERROR;
    }
    http->request_parts = PICO_ZALLOC(1 * sizeof(struct request_part *));
    if (!http->request_parts)
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len = 0;
    http->request_parts_len_done = 0;

    request = pico_http_client_build_delete(http->urikey, connection);
    dbg("DELETE: request: \n%s\n", request);
    if (!request)
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts[http->request_parts_len] = request_part_create(request, strlen(request), HTTP_NO_COPY_TO_HEAP, HTTP_NO_USER_MEM);
    if (!http->request_parts[http->request_parts_len])
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len += 1;
    print_request_part_info(http);
    bytes_written = socket_write_request_parts(http);
    return HTTP_RETURN_OK;
}

/*
 * API for sending a POST request.
 *
 * The library will build the request
 * based on the uri passed when opening the client.
 *
 * User should not FREE the post_data until it has been written to the http_socket.
 * Callback will indicate this.
 *
 * connection: HTTP_CONN_CLOSE/HTTP_CONN_KEEP_ALIVE
 * content_type: if NULL --> default value is passed
 * cache_control: HTTP_CACHE_CONTROL/HTTP_NO_CACHE_CONTROL
 * post_data example: "var_1=value_1&var_2=value_2"
 *
 */
int8_t MOCKABLE pico_http_client_send_post(uint16_t conn, uint8_t *post_data, uint32_t post_data_len, uint8_t connection, uint8_t *content_type, uint8_t *cache_control)
{
    dbg("POST request\n");
    uint8_t *header = NULL;
    uint8_t i = 0;
    struct pico_http_client search = {
        .connectionID = conn
    };
    struct pico_http_client *http = pico_tree_findKey(&pico_client_list, &search);
    int32_t bytes_written = 0;

    if(!post_data || !post_data_len)
    {
        return HTTP_RETURN_ERROR;
    }

    if (!http)
    {
        dbg("Client not found !\n");
        return HTTP_RETURN_ERROR;
    }

    if (http->state != HTTP_CONN_IDLE)
    {
        return HTTP_RETURN_CONN_BUSY;
    }

    if (connection != HTTP_CONN_CLOSE && connection != HTTP_CONN_KEEP_ALIVE)
    {
        return HTTP_RETURN_ERROR;
    }

    http->request_parts = PICO_ZALLOC(2 * sizeof(struct request_part *));
    if (!http->request_parts)
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len = 0;
    http->request_parts_len_done = 0;
    header = pico_http_client_build_post_header(http->urikey, post_data_len, connection, content_type, cache_control);
    if (!header)
    {
        request_parts_destroy(http);
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts[http->request_parts_len] = request_part_create(header, strlen(header), HTTP_NO_COPY_TO_HEAP, HTTP_NO_USER_MEM);
    if (!http->request_parts[http->request_parts_len])
    {
        request_parts_destroy(http);
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len += 1;

    http->request_parts[http->request_parts_len] = request_part_create(post_data, post_data_len, HTTP_NO_COPY_TO_HEAP, HTTP_USER_MEM);
    if (!http->request_parts[http->request_parts_len])
    {
        request_parts_destroy(http);
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len += 1;

    print_request_part_info(http);
    bytes_written = socket_write_request_parts(http);
    return HTTP_RETURN_OK;
}

/*
 * API for sending a raw request.
 * User should not FREE the request until it has been written to the http_socket.
 * Callback will indicate this.
 */
int8_t MOCKABLE pico_http_client_send_raw(uint16_t conn, uint8_t *request)
{
    dbg("RAW request\n");
    struct pico_http_client search = {
        .connectionID = conn
    };
    struct pico_http_client *http = pico_tree_findKey(&pico_client_list, &search);
    int32_t bytes_written = 0;

    if (!http)
    {
        dbg("Client not found!\n");
        return HTTP_RETURN_ERROR;
    }
    if (!request)
    {
        dbg("Request is empty!\n");
        return HTTP_RETURN_ERROR;
    }

    if (http->state != HTTP_CONN_IDLE)
    {
        return HTTP_RETURN_CONN_BUSY;
    }

    http->request_parts = PICO_ZALLOC(1 * sizeof(struct request_part *));
    if (!http->request_parts)
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len = 0;
    http->request_parts_len_done = 0;
    http->request_parts[http->request_parts_len] = request_part_create(request, strlen(request), HTTP_NO_COPY_TO_HEAP, HTTP_USER_MEM);

    if (!http->request_parts[http->request_parts_len])
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }

    http->request_parts_len += 1;
    bytes_written = socket_write_request_parts(http);
    return HTTP_RETURN_OK;
}

/*
 * API for sending a long polling GET request to the client.
 * Can be used in combination with keep-alive.
 * The library will build the request
 * based on the uri passed when opening the client.
 */
int8_t MOCKABLE pico_http_client_long_poll_send_get(uint16_t conn, uint8_t connection)
{
    uint8_t *request = NULL;
    struct pico_http_client search = {
        .connectionID = conn
    };
    struct pico_http_client *client = pico_tree_findKey(&pico_client_list, &search);
    int32_t bytes_written;
    if (!client)
    {
        dbg("pico_http_client_long_poll_send_get | Client not found!\n");
        return HTTP_RETURN_ERROR;
    }
    if (connection == HTTP_CONN_CLOSE)
    {
        client->long_polling_state = HTTP_LONG_POLL_CONN_CLOSE;
    }
    else if( connection == HTTP_CONN_KEEP_ALIVE)
    {
        client->long_polling_state = HTTP_LONG_POLL_CONN_KEEP_ALIVE;
    }
    else
    {
        return HTTP_RETURN_ERROR;
    }
    return pico_http_client_send_get(conn, connection);
}

/*
 * API to cancel a long polling GET request.
 */
int8_t MOCKABLE pico_http_client_long_poll_cancel(uint16_t conn)
{
    struct pico_http_client search = {
        .connectionID = conn
    };
    struct pico_http_client *client = pico_tree_findKey(&pico_client_list, &search);
    int32_t bytes_written;
    if (!client)
    {
        dbg("pico_http_client_long_poll_cancel | Client not found! \n");
        return HTTP_RETURN_ERROR;
    }
    client->long_polling_state = 0;
    return pico_http_client_close(conn);
}


/*
 * API for sending a GET request to the client.
 *
 * The library will build the request
 * based on the uri passed when opening the client.
 */
int8_t MOCKABLE pico_http_client_send_get(uint16_t conn, uint8_t connection)
{
    uint8_t *request = NULL;
    struct pico_http_client search = {
        .connectionID = conn
    };
    struct pico_http_client *http = pico_tree_findKey(&pico_client_list, &search);
    int32_t bytes_written;

    if (!http)
    {
        dbg("Client not found !\n");
        return HTTP_RETURN_ERROR;
    }

    if (http->state != HTTP_CONN_IDLE)
    {
        return HTTP_RETURN_CONN_BUSY;
    }

    if (connection != HTTP_CONN_CLOSE && connection != HTTP_CONN_KEEP_ALIVE)
    {
        return HTTP_RETURN_ERROR;
    }

    http->request_parts = PICO_ZALLOC(1 * sizeof(struct request_part *));
    if (!http->request_parts)
    {
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len = 0;
    http->request_parts_len_done = 0;

    request = pico_http_client_build_get(http->urikey, connection);
    if (!request)
    {
        request_parts_destroy(http);
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    dbg("GET HEADER: %s\n", request);
    http->request_parts[http->request_parts_len] = request_part_create(request, strlen(request), HTTP_NO_COPY_TO_HEAP, HTTP_NO_USER_MEM);
    if (!http->request_parts[http->request_parts_len])
    {
        request_parts_destroy(http);
        pico_err = PICO_ERR_ENOMEM;
        return HTTP_RETURN_ERROR;
    }
    http->request_parts_len += 1;
    print_request_part_info(http);
    bytes_written = socket_write_request_parts(http);
    return HTTP_RETURN_OK;
}
/* / */

static inline int check_chunk_line(struct pico_http_client *client, int tmp_len_read)
{
    if (read_chunk_line(client) == HTTP_RETURN_ERROR)
    {
        dbg("Probably the chunk is malformed or parsed wrong...\n");
        client->wakeup(EV_HTTP_ERROR, client->connectionID);
        return HTTP_RETURN_ERROR;
    }

    if (client->state != HTTP_READING_BODY || !tmp_len_read)
    {
        return 0; /* force out */
    }
    return 1;
}

static inline void update_content_length(struct pico_http_client *client, uint32_t tmp_len_read )
{
    if (tmp_len_read > 0)
    {
        client->header->content_length_or_chunk = client->header->content_length_or_chunk - (uint32_t)tmp_len_read;
    }
}

static inline int32_t read_body(struct pico_http_client *client, uint8_t *data, uint16_t size, uint32_t *len_read, uint32_t *tmp_len_read)
{
    *tmp_len_read = 0;

    if (client->state == HTTP_READING_BODY)
    {

        /* if needed truncate the data */
        *tmp_len_read = pico_socket_read(client->sck, data + (*len_read),
                                       (client->header->content_length_or_chunk < ((uint32_t)(size - (*len_read)))) ? ((uint32_t)client->header->content_length_or_chunk) : (size - (*len_read)));

        update_content_length(client, *tmp_len_read);
        if (*tmp_len_read < 0)
        {
            /* error on reading */
            dbg(">>> Error returned pico_socket_read\n");
            pico_err = PICO_ERR_EBUSY;
            /* return how much data was read until now */
            return (*len_read);
        }
    }

    *len_read += *tmp_len_read;
    return 0;
}

static inline uint32_t read_big_chunk(struct pico_http_client *client, uint8_t *data, uint16_t size, uint32_t *len_read)
{
    uint32_t value;
    /* check if we need more than one chunk */
    if (size >= client->header->content_length_or_chunk)
    {
        /* read the rest of the chunk, if chunk is done, proceed to the next chunk */
        while ((uint16_t)(*len_read) <= size)
        {
            uint32_t tmp_len_read = 0;
            if (read_body(client, data, size, len_read, &tmp_len_read))
            {
                return (*len_read);
            }
            if ((value = check_chunk_line(client, tmp_len_read)) <= 0)
            {
                return value;
            }
        }
    }
    return 0;
}

static inline void read_small_chunk(struct pico_http_client *client, uint8_t *data, uint16_t size, uint32_t *len_read)
{
    if (size < client->header->content_length_or_chunk)
    {
        /* read the data from the chunk */
        *len_read = pico_socket_read(client->sck, (void *)data, size);

        if (*len_read)
        {
            client->header->content_length_or_chunk = client->header->content_length_or_chunk - (uint32_t)(*len_read);
        }
    }
}
static inline int32_t read_chunked_data(struct pico_http_client *client, uint8_t *data, uint16_t size)
{
    int32_t len_read = 0;
    int32_t value;
    /* read the chunk line */
    if (read_chunk_line(client) == HTTP_RETURN_ERROR)
    {
        dbg("Probably the chunk is malformed or parsed wrong...\n");
        client->wakeup(EV_HTTP_ERROR, client->connectionID);
        return HTTP_RETURN_ERROR;
    }

    /* nothing to read, no use to try */
    if (client->state != HTTP_READING_BODY)
    {
        pico_err = PICO_ERR_EAGAIN;
        return HTTP_RETURN_OK;
    }

    read_small_chunk(client, data, size, &len_read);
    value = read_big_chunk(client, data, size, &len_read);
    if (value)
    {
        return value;
    }
    return len_read;
}

/*
 * API for reading received body.
 *
 * This api hides from the user if the transfer-encoding
 * was chunked or a full length was provided, in case of
 * a chunked transfer encoding will "de-chunk" the data
 * and pass it to the user.
 * Body_read_done will be set to 1 if the body has been read completly.
 */
int32_t MOCKABLE pico_http_client_read_body(uint16_t conn, uint8_t *data, uint16_t size, uint8_t *body_read_done)
{
    uint32_t bytes_read = 0;
    struct pico_http_client dummy = {
        .connectionID = conn
    };
    struct pico_http_client *client = pico_tree_findKey(&pico_client_list, &dummy);

    if (!client)
    {
        dbg("Wrong connection id !\n");
        pico_err = PICO_ERR_EINVAL;
        return HTTP_RETURN_ERROR;
    }

    /* for the moment just read the data, do not care if it's chunked or not */
    if (client->header->transfer_coding == HTTP_TRANSFER_FULL)
    {
        bytes_read = pico_socket_read(client->sck, (void *)data, size);
        //bytes read and content-length are equal TODO >???
        if (client->header->content_length_or_chunk == client->body_read)
        {
            client->body_read_done = 1;
        }
    }
    else
    {
        //client->state will be set to HTTP_READ_BODY_DONE if we reach the ending '0' at the end of the body
        bytes_read = read_chunked_data(client, data, size);
    }
    client->body_read += bytes_read;

    if (client->body_read_done)
    {
        dbg("Body read finished! %d\n", client->body_read);
        client->state = HTTP_CONN_IDLE;
        client->body_read = 0;
        client->body_read_done = 0;
        *body_read_done = 1;
        if (client->long_polling_state == HTTP_LONG_POLL_CONN_KEEP_ALIVE)
        {
            treat_long_polling(client, 0);//only on keep alive connections, for the close connection we wait for the close event in the tcp_callback
        }
    }
    return bytes_read;
}

/*
 * API for reading received data.
 *
 * Reads out the header struct received from server.
 */
struct pico_http_header * MOCKABLE pico_http_client_read_header(uint16_t conn)
{
    struct pico_http_client dummy = {
        .connectionID = conn
    };
    struct pico_http_client *client = pico_tree_findKey(&pico_client_list, &dummy);

    if (client)
    {
        return client->header;
    }
    else
    {
        /* not found */
        dbg("Wrong connection id !\n");
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }
}

/*
 * API for reading received data.
 *
 * Reads out the uri struct after was processed.
 */
struct pico_http_uri *pico_http_client_read_uri_data(uint16_t conn)
{
    struct pico_http_client dummy = {
        .connectionID = conn
    };
    struct pico_http_client *client = pico_tree_findKey(&pico_client_list, &dummy);
    /*  */
    if (client)
    {
        return client->urikey;
    }
    else
    {
        /* not found */
        dbg("Wrong connection id !\n");
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }
}

/*
 * API for reading received data.
 *
 * Close the client.
 */
static void free_header(struct pico_http_client *to_be_removed)
{
    if (to_be_removed->header)
    {
        /* free space used */
        if (to_be_removed->header->location)
        {
            PICO_FREE(to_be_removed->header->location);
        }
        PICO_FREE(to_be_removed->header);
    }
}

static int8_t free_uri(struct pico_http_client *to_be_removed)
{
    if (!to_be_removed)
    {
        return HTTP_RETURN_ERROR;
    }

    if (to_be_removed->urikey)
    {
        if (to_be_removed->urikey->host)
        {
            PICO_FREE(to_be_removed->urikey->host);
        }
        if (to_be_removed->urikey->resource)
        {
            PICO_FREE(to_be_removed->urikey->resource);
        }
        if (to_be_removed->urikey->raw)
        {
            PICO_FREE(to_be_removed->urikey->raw);
        }
        PICO_FREE(to_be_removed->urikey);
    }

    return HTTP_RETURN_OK;

}

int8_t MOCKABLE pico_http_client_close(uint16_t conn)
{
    struct pico_http_client *to_be_removed = NULL;
    struct pico_http_client dummy = {
        0
    };
    dummy.connectionID = conn;

    dbg("Closing the client...\n");
    to_be_removed = pico_tree_delete(&pico_client_list, &dummy);
    if (!to_be_removed)
    {
        dbg("Warning ! Element not found ...");
        return HTTP_RETURN_ERROR;
    }

    /* close socket */
    if (to_be_removed->sck)
    {
        pico_socket_close(to_be_removed->sck);
    }
    free_header(to_be_removed);
    free_uri(to_be_removed);

    PICO_FREE(to_be_removed);

    return 0;
}

static inline void read_first_line(struct pico_http_client *client, uint8_t *line, uint32_t *index)
{
    uint8_t c;

    /* read the first line of the header */
    while (consume_char(c) > 0 && c != '\r')
    {
        if (*index < HTTP_HEADER_LINE_SIZE) /* truncate if too long */
            line[(*index)++] = c;
    }
    consume_char(c); /* consume \n */
}

static inline void start_reading_body(struct pico_http_client *client, struct pico_http_header *header)
{

    if (header->transfer_coding == HTTP_TRANSFER_CHUNKED)
    {
        /* read the first chunk */
        header->content_length_or_chunk = 0;

        client->state = HTTP_READING_CHUNK_VALUE;
        read_chunk_line(client);
    }
    else
        client->state = HTTP_READING_BODY;
}

static inline int32_t parse_loc_and_cont(struct pico_http_client *client, struct pico_http_header *header, uint8_t *line, uint32_t *index)
{
    uint8_t c;
    /* Location: */

    if (is_location(line))
    {
        *index = 0;
        while (consume_char(c) > 0 && c != '\r')
        {
            line[(*index)++] = c;
        }
        /* allocate space for the field */
        header->location = PICO_ZALLOC((*index) + 1u);
        if (header->location)
        {
            memcpy(header->location, line, (*index));
            return 1;
        }
        else
        {
            return -1;
        }
    }    /* Content-Length: */
    else if (is_content_length(line))
    {
        header->content_length_or_chunk = 0u;
        header->transfer_coding = HTTP_TRANSFER_FULL;
        /* consume the first space */
        consume_char(c);
        while (consume_char(c) > 0 && c != '\r')
        {
            header->content_length_or_chunk = header->content_length_or_chunk * 10u + (uint32_t)(c - '0');
        }
        return 1;
    }    /* Transfer-Encoding: chunked */

    return 0;
}

static inline int32_t parse_transfer_encoding(struct pico_http_client *client, struct pico_http_header *header, uint8_t *line, uint32_t *index)
{
    uint8_t c;

    if (is_transfer_encoding(line))
    {
        (*index) = 0;
        while (consume_char(c) > 0 && c != '\r')
        {
            line[(*index)++] = c;
        }
        if (is_chunked(line))
        {
            header->content_length_or_chunk = 0u;
            header->transfer_coding = HTTP_TRANSFER_CHUNKED;
        }

        return 1;
    } /* just ignore the line */

    return 0;
}


static inline int32_t parse_fields(struct pico_http_client *client, struct pico_http_header *header, int8_t *line, uint32_t *index)
{
    int8_t c;
    int32_t ret_val;

    ret_val = parse_loc_and_cont(client, header, line, index);
    if (ret_val == 0)
    {
        if (!parse_transfer_encoding(client, header, line, index))
        {
            while (consume_char(c) > 0 && c != '\r') nop();
        }
    }
    else if (ret_val == -1)
    {
        return -1;
    }

    /* consume the next one */
    consume_char(c);
    /* reset the index */
    (*index) = 0u;

    return 0;
}

static inline int32_t parse_rest_of_header(struct pico_http_client *client, struct pico_http_header *header, uint8_t *line, uint32_t *index)
{
    uint8_t c;
    uint32_t read_len = 0;

    /* parse the rest of the header */
    read_len = consume_char(c);
    if (read_len == 0)
        return HTTP_RETURN_BUSY;

    while (read_len > 0)
    {
        if (c == ':')
        {
            if (parse_fields(client, header, line, index) == -1)
                return HTTP_RETURN_ERROR;
        }
        else if (c == '\r' && !(*index))
        {
            /* consume the \n */
            consume_char(c);
            break;
        }
        else
        {
            line[(*index)++] = c;
        }

        read_len = consume_char(c);
    }
    return HTTP_RETURN_OK;
}

static int8_t parse_header_from_server(struct pico_http_client *client, struct pico_http_header *header)
{
    uint8_t line[HTTP_HEADER_LINE_SIZE];
    uint32_t index = 0;

    if (client->state == HTTP_START_READING_HEADER)
    {
        read_first_line(client, line, &index);
        /* check the integrity of the response */
        /* make sure we have enough characters to include the response code */
        /* make sure the server response starts with HTTP/1. */
        if ((index < RESPONSE_INDEX + 2u) || is_not_HTTPv1(line))
        {
            /* wrong format of the the response */
            pico_err = PICO_ERR_EINVAL;
            return HTTP_RETURN_ERROR;
        }

        /* extract response code */
        header->response_code = (uint16_t)((line[RESPONSE_INDEX] - '0') * 100 +
                                          (line[RESPONSE_INDEX + 1] - '0') * 10 +
                                          (line[RESPONSE_INDEX + 2] - '0'));
        if (header->response_code == HTTP_NOT_FOUND)
        {
            return HTTP_RETURN_NOT_FOUND;
        }
        else if (header->response_code >= HTTP_INTERNAL_SERVER_ERR)
        {
            /* invalid response type */
            header->response_code = 0;
            return HTTP_RETURN_ERROR;
        }
    }

    dbg("Server response : %d \n", header->response_code);

    if (parse_rest_of_header(client, header, line, &index) == HTTP_RETURN_BUSY)
        return HTTP_RETURN_BUSY;

    start_reading_body(client, header);
    dbg("End of header\n");
    return HTTP_RETURN_OK;

}

/* an async read of the chunk part, since in theory a chunk can be split in 2 packets */
static inline void set_client_chunk_state(struct pico_http_client *client)
{

    if (client->header->content_length_or_chunk == 0 && client->state == HTTP_READING_BODY)
    {
        client->state = HTTP_READING_CHUNK_VALUE;
    }
}
static inline void read_chunk_trail(struct pico_http_client *client)
{
    uint8_t c;

    if (client->state == HTTP_READING_CHUNK_TRAIL)
    {

        while (consume_char(c) > 0 && c != '\n')
        {
            nop();
        }
        if (c == '\n')
        {
            client->state = HTTP_READING_BODY;
        }
    }
}
static inline void read_chunk_value(struct pico_http_client *client)
{
    uint8_t c;

    while (consume_char(c) > 0 && c != '\r' && c != ';')
    {
        dbg("c: %c\n", c);
        if (is_hex_digit(c))
        {
            client->header->content_length_or_chunk = (client->header->content_length_or_chunk << 4u) + (uint32_t)hex_digit_to_dec(c);
        }
        if (c == '0' && client->header->content_length_or_chunk == 0)
        {
            dbg("End of chunked data\n");
            client->body_read_done = 1;
        }
    }

    if (c == '\r' || c == ';')
    {
        client->state = HTTP_READING_CHUNK_TRAIL;
    }
}

static int8_t read_chunk_line(struct pico_http_client *client)
{
    set_client_chunk_state(client);

    if (client->state == HTTP_READING_CHUNK_VALUE)
    {
        read_chunk_value(client);
    }

    read_chunk_trail(client);

    return HTTP_RETURN_OK;
}
