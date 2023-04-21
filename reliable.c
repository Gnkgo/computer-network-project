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
#include <sys/time.h>

#include "rlib.h"
#include "buffer.h"

//Helper functions, defined at the bottom of the file

long currentTimeMillis();
bool isDone(rel_t * r);
bool should_send_packet(rel_t *s);
void send_packet(packet_t * packet, rel_t * s);
bool is_EOF(packet_t* packet);
void create_packet(packet_t * packet, int len, int seqno, int ackno, int isData);
bool is_ACK(packet_t* packet);

struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

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


}; rel_t *rel_list;



/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */

/*struct config_common {
    int window;			 # of unacknowledged packets in flight 
    int timer;			 How often rel_timer called in milliseconds 
    int timeout;			 Retransmission timeout in milliseconds 
    int single_connection;         Exit after first connection failure 
};*/


/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */

rel_t * rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc) {
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));
    /* You only need to call this in the server, when rel_create gets a * NULL conn_t. */    
    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
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
    r->MAXWND = cc -> window;
    r->timeout = cc -> timeout;

    /*receiver*/
    r->RCV_NXT = 1;

    return r;
}

void rel_destroy (rel_t *r) {
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy (r->c);

    buffer_clear(r->send_buffer);
    free(r->send_buffer);

    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
}

void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
    uint16_t len = ntohs(pkt->len);
    uint16_t cksum_old = ntohs(pkt->cksum);
    uint16_t seqno = ntohl(pkt->seqno);

    pkt->cksum = 0;

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

    if (is_ACK(pkt)) {
        buffer_remove(r->send_buffer, ntohl(pkt->ackno));
        r->SND_UNA = max(ntohl(pkt->ackno), r->SND_UNA);
        rel_read(r);
    } else if (seqno < r->RCV_NXT) {
        if (seqno != 0) {
            struct ack_packet* ack_pac = xmalloc(sizeof(struct ack_packet));
            create_packet((packet_t*)ack_pac, 8, -1, r->RCV_NXT, 0);
            conn_sendpkt(r->c, (packet_t*)ack_pac, 8);
            free(ack_pac);
        }
    } else if (seqno < r->RCV_NXT + r->MAXWND && conn_bufspace(r->c) >= len - 12) {
        if (!buffer_contains(r->rec_buffer, ntohl(pkt->seqno))) {
            buffer_insert(r->rec_buffer, pkt, currentTimeMillis());
        }
        rel_output(r);
    }
}



void rel_output(rel_t *r) {
    buffer_node_t* first_node = buffer_get_first(r->rec_buffer);
    if (!first_node) return;
    packet_t* packet = &(first_node->packet);
    while (first_node && ntohl(packet->seqno) == (uint32_t) r->RCV_NXT && conn_bufspace(r->c) >= ntohs(packet->len) - 12) {
        if (is_EOF(packet)) {
            conn_output(r->c, packet->data, htons(0));
            buffer_remove_first(r->rec_buffer);
            r->RCV_NXT++;
            r->EOF_RECV = 1;
            struct ack_packet* ack_pac = xmalloc(sizeof(struct ack_packet));
            create_packet((packet_t *) ack_pac, 8, -1, r->RCV_NXT, 0);
            conn_sendpkt(r->c, (packet_t *)ack_pac, (size_t) 8);
            free(ack_pac);
            if (isDone(r)) {
                rel_destroy(r);
                return;
            }
        } else if (conn_bufspace(r->c) >= ntohs(packet->len) - 12) {
            r->flushing = 1;
            conn_output(r->c, packet->data, ntohs(packet->len) - 12);
            buffer_remove_first(r->rec_buffer);
            r->RCV_NXT++;
            r->flushing = 0;
            struct ack_packet* ack_pac = xmalloc(sizeof(struct ack_packet));
            create_packet((packet_t *) ack_pac, 8, -1, r->RCV_NXT, 0);
            conn_sendpkt(r->c, (packet_t *)ack_pac, (size_t) 8);
            free(ack_pac);
        }
        first_node = buffer_get_first(r->rec_buffer);
        if (first_node) packet = &(first_node->packet);
    }
}



void rel_read (rel_t *s) {
    if((s->EOF_SENT)){
       return;
    }

    while(should_send_packet(s)) {
        packet_t *packet = (packet_t *) xmalloc(512);
        memset(packet, 0 , sizeof(packet_t));
        int read_byte = conn_input(s->c, packet->data, 500);
        int SND_NXT = s->SND_NXT;

        if (read_byte == 0) {
            free(packet);
            break;
        }

        if (read_byte == -1) {
            s->EOF_SENT = 1;
            s->EOF_seqno = SND_NXT;
            create_packet(packet, 12, SND_NXT, 0, 1);
        } else {
            create_packet(packet, 12 + read_byte, SND_NXT, 0, 1);
        }

        s->SND_NXT++;
        send_packet(packet, s);
        free(packet);
    }
}

void rel_timer() {
    rel_t *current = rel_list;
    while (current) {
        buffer_node_t* node = buffer_get_first(current->send_buffer);
        while (node) {
            long cur_time = currentTimeMillis();
            long last_time = node->last_retransmit;
            long timeout = current->timeout;
            if ((cur_time - last_time) >= timeout) {
                conn_sendpkt(current->c, &(node->packet), (size_t) ntohs(node->packet.len));
                node->last_retransmit = cur_time;
            }
            node = node->next;
        }
        current = current->next;
    }
}




//-----------------------------------------------------------------------------------------------------------

/*helper functins */
/*https//stackoverflow.com/questions/10098441/get-the-current-time-in-milliseconds-in-c*/
long currentTimeMillis() {
    struct timeval now;
    gettimeofday(&now, NULL);
    long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;
    return now_ms;
}

bool isDone(rel_t * r) {
    return (r->EOF_SENT && r->EOF_RECV && r->EOF_ACK_RECV && !r -> flushing && buffer_size(r->send_buffer) == 0);
}

/*function to check if the sender can send a packet*/
bool should_send_packet(rel_t *s) {
	return (s->SND_NXT - s->SND_UNA < s->MAXWND) && (!(s->EOF_SENT));
}

/*function to send a packet*/
void send_packet(packet_t * packet, rel_t * s) {
    buffer_insert(s->send_buffer, packet, currentTimeMillis());
    conn_sendpkt(s->c, packet, (size_t) ntohs(packet -> len));
}

bool is_EOF(packet_t* packet){
    return (ntohs(packet->len) == (uint16_t) 12);
}

void create_packet(packet_t * packet, int len, int seqno, int ackno, int isData) {
    packet -> len = htons((uint16_t) len);
    packet -> ackno = htonl((uint32_t) ackno);

    if (isData) {
		packet -> seqno = htonl((uint32_t) seqno);
	} 

    packet -> cksum = (uint16_t) 0;
    packet -> cksum = cksum(packet, len);
 }

bool is_ACK(packet_t* packet) {
    return ntohs(packet->len) == 8;
}
