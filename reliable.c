#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <math.h>

#include "rlib.h"
#include "buffer.h"

//Helper functions, defined at the bottom of the file


bool isDone(rel_t* r);
bool should_send_packet(rel_t* s);
bool enough_space(rel_t* r, packet_t* pkt);
bool is_ACK(packet_t* packet);
int is_EOF(packet_t* packet);
int MAX(int num1, int num2);
void create_packet(packet_t* packet, int len, int seqno, int ackno, int isData);
void send_packet(packet_t* packet, rel_t* s);
void create_send_ack(rel_t* r);
long currentTimeMillis();

struct reliable_state {
    rel_t* next;			/* Linked list for traversing all connections */
    rel_t** prev;

    conn_t* c;			/* This is the connection object */

    /* Add your own data fields below this */
    buffer_t* send_buffer;
    buffer_t* rec_buffer;

    /* ----------------------------SENDER----------------------------

    we need the following information:
    SND.UNA:            this is the lowest sequence number not acknowledged bytes SND.UNA = max(SND.UNA, ackno)
    SND.NXT:            represents sequence number of the next byte that the sender will send
    MAXWND:         The size of sending window can vary, and it should not exceed a maximum value SND.WND <= SND.MAXWND where SND.WND = SND.NXT - SND.UNA
    TIME_OUT:           If a frme is not acknowledged within a certain time period (timeout) the sender will resend the frame
    -> see PDF page 19*/

    int SND_UNA;
    int SND_NXT;
    int MAXWND;
    int timeout;

    /* ----------------------------RECEIVER----------------------------
    we need the following information:
    RCV.NXT:            represents sequence number of the next byte that the sender will send
    RCV.WND:            The size of sending window can vary, and it should not exceed a maximum value SND.WND <= SND.MAXWND where SND.WND = SND.NXT - SND.UNA

    if (seqno >= RCV.NXT + RCV.WND) {
        Drop frame
    } else {
    Store in the buffer if not already there
    }
    if seqno == RCV.NXT:
        set RCV.NXT to the highest seqno consecutively stored in the buffer + 1
        release data [seqno, RCV.NYT - 1] to application layer
    send back ACK with cumulative ackno = RCV.NXT
    -> see PDF page 25*/

    int RCV_NXT;
    int RCV_WND;

    /* ----------------------------ERROR_FLAGS----------------------------
    We need to keep track of the end of files*/

    int EOF_SENT;
    int EOF_RECV;
    int EOF_ACK_RECV;
    int EOF_seqno;
    int flushing;

}; rel_t* rel_list;



/**
 * Creates a new reliable protocol session, returns NULL on failure, ss is always NULL
 * @param   c       connection
 * @param   ss      const struct sockaddr_storage*, which is always NULL
 * @param   cc      const struct config_common* cc
 * @return  rel_t*  the reliable state
 */
rel_t* rel_create(conn_t* c, const struct sockaddr_storage* ss, const struct config_common* cc) {
    rel_t* r;

    r = xmalloc(sizeof(*r));
    memset(r, 0, sizeof(*r));
    /* You only need to call this in the server, when rel_create gets a * NULL conn_t. */
    if (!c) {
        c = conn_create(r, ss);
        if (!c) {
            free(r);
            return NULL;
        }
    }

    r->c = c;
    /*sets the next field to the current head of the linked list of all reliable protocol sessions */
    r->next = rel_list;
    /*This line sets the prev field of the reliable protocol session r to the memory address of the rel_list pointer.
    This is used to facilitate removing the reliable protocol session from the linked list if it needs to be deleted.*/
    r->prev = &rel_list;
    /*check if not NullNULL*/
    if (rel_list)
        rel_list->prev = &r->next;
    rel_list = r;

    /*memory*/
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;

    /*sender*/
    r->SND_UNA = 1;
    r->SND_NXT = 1;
    r->MAXWND = cc->window;
    r->timeout = cc->timeout;

    /*receiver*/
    r->RCV_NXT = 1;

    return r;
}

/**
 * destroys the connection
 * @param   r       rel_t *
 * @return void
 */
void rel_destroy(rel_t* r) {
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy(r->c);

    buffer_clear(r->send_buffer);
    free(r->send_buffer);

    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
}


/**
* Processes incoming packets for the reliable protocol session
* @param r rel_t*, the reliable state
* @param pkt packet_t*, the incoming packet
* @param n size_t, the size of the incoming packet
* @return void
* @remarks If the packet is corrupted, the function will return without further processing.
 */
void rel_recvpkt(rel_t* r, packet_t* pkt, size_t n) {
    uint16_t len = ntohs(pkt->len);
    uint16_t cksum_old = ntohs(pkt->cksum);
    uint16_t seqno = ntohl(pkt->seqno);

    pkt->cksum = 0;

    // Verify packet checksum and length -> check if corrupted
    if (len != n || cksum_old != ntohs(cksum(pkt, len))) {
        return;
    }

    if (is_ACK(pkt) && pkt->ackno == r->EOF_seqno + 1) {
        r->EOF_ACK_RECV = 1;
    }

    if (isDone(r)) {
        rel_destroy(r);
        return;
    }
    // If the packet is an ACK, remove it from the send buffer and update the SND_UNA variable
    if (is_ACK(pkt)) {
        buffer_remove(r->send_buffer, ntohl(pkt->ackno));
        r->SND_UNA = MAX(ntohl(pkt->ackno), r->SND_UNA);
        rel_read(r);
    }
    // If the packet is not an ACK and the sequence number is less than RCV_NXT, send an ACK
    else if (seqno < r->RCV_NXT) {
        if (seqno != 0) {
            create_send_ack(r);
        }
    }
    // If the packet is not an ACK and the sequence number is within the receive window, buffer and output the packet
    else if (seqno < r->RCV_NXT + r->MAXWND && conn_bufspace(r->c) >= len - 12) {
        if (!buffer_contains(r->rec_buffer, ntohl(pkt->seqno))) {
            buffer_insert(r->rec_buffer, pkt, currentTimeMillis());
        }
        rel_output(r);
    }
}
/**
* Outputs the received packets in the receive buffer.
* @param r rel_t* the reliable state
* @return void
*/
void rel_output(rel_t* r) {
    buffer_node_t* first_node = buffer_get_first(r->rec_buffer);
    if (!first_node) return;
    packet_t* pkt = &(first_node->packet);
    // Iterate through the receive buffer and output packets
    while (first_node && ntohl(pkt->seqno) == (uint32_t)r->RCV_NXT && enough_space(r, pkt)) {
        if (is_EOF(pkt)) {
            conn_output(r->c, pkt->data, htons(0));
            buffer_remove_first(r->rec_buffer);
            r->RCV_NXT++;
            r->EOF_RECV = 1;
            create_send_ack(r);
            if (isDone(r)) {
                rel_destroy(r);
                return;
            }
        }
        // If there is enough buffer space, output the packet
        else if (conn_bufspace(r->c) >= ntohs(pkt->len) - 12) {
            r->flushing = 1;
            conn_output(r->c, pkt->data, ntohs(pkt->len) - 12);
            buffer_remove_first(r->rec_buffer);
            r->RCV_NXT++;
            r->flushing = 0;
            create_send_ack(r);
        }
        first_node = buffer_get_first(r->rec_buffer);
        if (first_node) pkt = &(first_node->packet);
    }
}



/**
* Reads data from the connection and sends packets until there is no more data to be read or packets to be sent.
* @param s rel_t* representing the reliable state
* @return void
*/
void rel_read(rel_t* s) {
    if ((s->EOF_SENT)) {
        return;
    }
    // Keep sending packets while there is data to be read and packets to be sent
    while (should_send_packet(s)) {
        packet_t* packet = (packet_t*)xmalloc(512);
        memset(packet, 0, sizeof(packet_t));
        int read_byte = conn_input(s -> c, packet->data, 500);
        int SND_NXT = s->SND_NXT;

        // If there is no more data to read, break out of the loop
        if (read_byte == 0) {
            free(packet);
            break;
        }
        // If there was an error while reading, send an EOF packet and mark it as sent
        if (read_byte == -1) {
            s->EOF_SENT = 1;
            s->EOF_seqno = SND_NXT;
            create_packet(packet, 12, SND_NXT, 0, 1);
        }
        else {
            // Otherwise, create a packet with the data read and send it
            create_packet(packet, 12 + read_byte, SND_NXT, 0, 1);
        }

        s->SND_NXT++;
        send_packet(packet, s);
        free(packet);
    }
}
/**
 * rel_timer - Function to handle retransmissions for all active connections
 * @param None
 * @return None
 */
void rel_timer() {
    rel_t* current = rel_list;
    // Iterate through all the connections in the rel_list
    while (current) {
        buffer_node_t* node = buffer_get_first(current->send_buffer);
        // Iterate through all the packets in the send_buffer of the current connection
        while (node) {
            // Check if the time since the last retransmit is greater than or equal to the timeout period
            long cur_time = currentTimeMillis();
            long last_time = node->last_retransmit;
            long timeout = current->timeout;
            if ((cur_time - last_time) >= timeout) {
                // Retransmit the packet and update the last_retransmit time
                conn_sendpkt(current->c, &(node->packet), (size_t)ntohs(node->packet.len));
                node->last_retransmit = cur_time;
            }
            // Move on to the next packet in the buffer
            node = node->next;
        }
        // Move on to the next connection in the rel_list
        current = current->next;
    }
}


//-----------------------------------------------------------------------------------------------------------

/*helper functins */


/**
 * https//stackoverflow.com/questions/10098441/get-the-current-time-in-milliseconds-in-c
 * gets the current time
 * @param None
 * @return long
 */
long currentTimeMillis() {
    struct timeval time;
    gettimeofday(&time, NULL);
    int64_t s1 = (int64_t)(time.tv_sec) * 1000;
    int64_t s2 = (time.tv_usec / 1000);
    return s1 + s2;
}

/**
 * check if everything is send, received, acknoloeged, if yes you can destroy it
 * @param   rel_t *
 * @return  bool
 */
bool isDone(rel_t* r) {
    return (r->EOF_SENT && r->EOF_RECV && r->EOF_ACK_RECV && !r->flushing && buffer_size(r->send_buffer) == 0);
}

/**
 * function to check if the sender can send a packet
 * @param   rel_t *
 * @return  long
 */
bool should_send_packet(rel_t* s) {
    return (s->SND_NXT - s->SND_UNA < s->MAXWND) && (!(s->EOF_SENT));
}

/**
 * function to send a packet
 * @param   packet_ t *
 * @param   rel_t *
 * @return  void
 */
void send_packet(packet_t* packet, rel_t* s) {
    buffer_insert(s->send_buffer, packet, currentTimeMillis());
    conn_sendpkt(s->c, packet, (size_t)ntohs(packet->len));
}

int is_EOF(packet_t* packet) {
    return (ntohs(packet->len) == (uint16_t)12);
}

void create_packet(packet_t* packet, int len, int seqno, int ackno, int isData) {
    packet->len = htons((uint16_t)len);
    packet->ackno = htonl((uint32_t)ackno);

    if (isData) {
        packet->seqno = htonl((uint32_t)seqno);
    }

    packet->cksum = (uint16_t)0;
    packet->cksum = cksum(packet, len);
}

bool is_ACK(packet_t* packet) {
    return ntohs(packet->len) == 8;
}

int MAX(int num1, int num2) {
    return (num1 > num2) ? num1 : num2;
}

void create_send_ack(rel_t* r) {
    struct ack_packet* ack_pac = xmalloc(sizeof(struct ack_packet));
    create_packet((packet_t*)ack_pac, 8, -1, r->RCV_NXT, 0);
    conn_sendpkt(r->c, (packet_t*)ack_pac, 8);
    free(ack_pac);
}

bool enough_space(rel_t* r, packet_t* pkt) {
    return conn_bufspace(r->c) >= ntohs(pkt->len) - 12;
}