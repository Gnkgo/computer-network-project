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

#include "rlib.h"
#include "buffer.h"

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
    int TIME_OUT;

    int RCV_NXT;
    int RCV_WND;

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

    /* ----------------------------ERROR_FLAGS----------------------------
    We need to keep track of the end of files*/

    int EOF_SENT;
    int EOF_RECV;
    int EOF_ACK_RECV;
    int EOF_seqno;
    int flushing;
    FILE *fptr;



};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *

/*struct config_common {
    int window;			 # of unacknowledged packets in flight 
    int timer;			 How often rel_timer called in milliseconds 
    int timeout;			 Retransmission timeout in milliseconds 
    int single_connection;         Exit after first connection failure 
};*/

rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc) {
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
    r->TIME_OUT = cc -> timeout;

    /*receiver*/
    r->RCV_NXT = 1;

    /*error*/
    r -> EOF_SENT = 0;
    r -> EOF_RECV = 0;
    r -> EOF_ACK_RECV = 0;
    r -> EOF_seqno = 0;
    r -> flushing = 0;

    r->fptr = fopen("./mycodeErrLog.txt", "w");

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

int is_ACK(packet_t* pkt) {
	if (ntohs(pkt->len) == 8) {
        return 1;
    } else if (ntohs(pkt -> len) > 8 && ntohs(pkt -> len) <= 520) {
		return 0;
	} else {
		return -1;
}

// n is the expected length of pkt
void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
    uint16_t rec_len = ntohs(pkt->len);
    uint16_t rec_cksum = ntohs(pkt->cksum);
    uint32_t rec_seqno = ntohl(pkt->seqno);
    uint32_t rec_ackno = ntohl(pkt->ackno);
    /*cksum: 16-bit IP checksum (you can set the cksum field to 0 and use the cksum(const void *, int)
    function on a packet to compute the value of the checksum that should be in there).*/
    /*discard if the packet is corrupted*/
    
    pkt -> cksum = 0;

    if (rec_len != n || rec_cksum != cksum(pkt, (int) rec_len)) {
		return;
	}

    pkt -> cksum = rec_cksum;

    if (is_ACK(pkt) == 1) {
    /*if the packet is an ACK*/
        fprintf(r->fptr, "ACKACKACKACKACK\n\n\n");
		if (rec_ackno == r -> EOF_seqno + 1) {
			/*if the ACK is for the EOF packet*/
			r->EOF_ACK_RECV = 1;
	    } 
        if (isDone(r)) {
				/*if all EOF flags are set, close the connection*/
		    rel_destroy(r);
		} else {
			/*if the ACK is for a data packet*/
			int acked_packet_number = buffer_remove(r->send_buffer, rec_ackno);
            fprintf(stderr, "Sender buffer removed acked packets: %x\n", acked_packet_number);			
            
            /*if (r->send_buffer->head == NULL) {
				/*if the send buffer is empty, stop the timer
				conn_timer(r->c, 0);
			}*/
		}
	} else if (is_ACK(pkt) == 0){
		/*if the packet is a data packet*/
		if (rec_seqno == r->EOF_seqno) {
			/*if the packet is the EOF packet*/
			r->EOF_RECV = 1;
			if (r->EOF_SENT && r->EOF_RECV && r->EOF_ACK_RECV) {
				/*if all EOF flags are set, close the connection*/
				rel_destroy(r);
			}
		} else {
			/*if the packet is a data packet*/
			if (rec_seqno == r->RCV_NXT) {
				/*if the packet is the next expected packet*/
				r->RCV_NXT++;
				conn_output(r->c, pkt->data, rec_len - 8);
			}
			/*send an ACK for the packet*/
			packet_t* ack_pkt = xmalloc(sizeof(packet_t));
			ack_pkt->len = htons(8);
			ack_pkt->cksum = 0;
			ack_pkt->ackno = htonl(rec_seqno);
			ack_pkt->cksum = cksum(ack_pkt, 8);
			conn_sendpkt(r->c, ack_pkt, 8);
			free(ack_pkt);
		}
    } else {
        return NULL;
    }
}

/*https//stackoverflow.com/questions/10098441/get-the-current-time-in-milliseconds-in-c*/
long currentTimeMillis() {
  struct timeval time;
  gettimeofday(&time, NULL);
  int64_t s1 = (int64_t)(time.tv_sec) * 1000;
  int64_t s2 = (time.tv_usec / 1000);
  return s1 + s2;
}

int isDone(rel_t * r) {
    return (r->EOF_SENT && r->EOF_RECV && r->EOF_ACK_RECV && !r -> flushing && buffer_size(r->send_buffer) == 0);
}

/*function to check if the sender can send a packet*/
int should_send_packet(rel_t *s) {
	return (s->SND_NXT - s->SND_UNA < s->MAXWND) && (!s->EOF_SENT);
}

/*function to send a packet*/
void send_packet(packet_t *packet, rel_t * s) {
    buffer_insert(s->send_buffer, packet, currentTimeMillis());
    conn_sendpkt(s->c, packet, (size_t) 12);
}

/*function to updadte the flags when eof is reached*/
void eof_update(rel_t * s) {
    s -> EOF_seqno = SND_NXT;
    s -> EOF_SENT = 1;
}



int is_EOF(packet_t* packet){
    return (ntohs(packet->len) == (uint16_t) 12 && ntohs(packet -> data));
}

void create_packet(packet_t * packet, int len, int seqno, int ackno, int cksum, int isData) {
    
    packet -> len = htons((uint16_t) len);
    packet -> ackno = htonl((uint32_t) ackno);

    if (isData) {
		packet -> seqno = htonl((uint32_t) seqno);
	} 
    
    packet -> cksum = (uint16_t) 0;
    packet -> chsum = cksum(packet, sum);
 }



void
rel_read (rel_t *s)
{
    int SND_UNA = s->SND_UNA;
    int SND_NX = s->SND_NXTT;
    int MAXWND = s->MAXWND;
    int read_byte;

    /*It is already sent, and all done */
    if(s->EOF_SENT){
       return;
    }

    /*
    packet_t packet{
        uint16_t cksum;
        uint16_t len;
        uint32_t ackno;
        uint32_t seqno;		/* Only valid if length > 8 
        char data[500];
    }

    */

    /* while there is space in the window and there is data to send */
    while (should_send_packet(s) {
        packet_t * packet = (packet_t *) xmalloc(sizeof(512);

        packet_t packet
        packet_
        memset(packet, 0, sizeof(packet_t));

        /*malloc was not successfull, return NULL*/
	    if (packet == NULL) {
            return NULL;
        }

        /*conn_input returns -1 if EOF, 0 if no data, and otherwise the number of bytes read as data*/
        int read_byte = conn_input(s->c, packet->data, 500);
        if (read_byte == -1) {
            create_packet(packet, htons((uint16_t) 12, htonl((uint32_t) SND_NXT, htonl((uint32_t) 0, 1);
            s -> SND_NXT++;
            eof_update(s);
            send_packet(packet, s);
        } else if (read_byte == 0) {
            free(packet);
		    break;
	    } else {
		    create_packet(packet, htons((uint16_t) 12 + read_byte), htonl((uint32_t) SND_NXT, htonl((uint32_t) 0), 1);
            s -> SND_NXT++;
            send_packet(packet, s);
	    }
        SND_UNA = s->SND_UNA;
        SND_NXT = s->SND_NXT;
        MAXWND = s->MAXWND;
    }
}


/*
typedef struct buffer_node {
    packet_t packet;
    long last_retransmit;
    struct buffer_node* next;
} buffer_node_t;

typedef struct buffer {
    buffer_node_t* head;
} buffer_t;
*/

/*
struct packet {
    uint16_t cksum;
    uint16_t len;
    uint32_t ackno;
    uint32_t seqno;		/* Only valid if length > 8 
    char data[500];
};
typedef struct packet packet_t;
*/


/*when conn_output receives a packet with length 0:
    if (n == 0) {
        c->write_eof = 1;
        if (!c->outq)
            shutdown (c->wfd, SHUT_WR);
        return 0;
    }
 */
void rel_output (rel_t *r) {
    /* Your logic implementation here */

    buffer_node_t first_node = get_first_node(r->send_buffer);

    /* When we haven't rceived anything yet, we don't have to output anything*/
    if (first_node == NULL) {
		return;
	}
    /*check if packet in buffer can be sent to the output. Check whether the sequence number of the packet
    matches the expected sequence number (RCV_NXT) and check whether there is enough space in the connection's output buffer
    to accommodate the packet's data*/

    packet_t * packet = &(first_node -> packet);
    uint16_t packet_len = ntohs(packet -> len);
    uint32_t packet_seqno = ntohl(packet -> seqno);
    uint16_t packet_cksum = ntohs(packet -> cksum);
    uint32_t packet_ackno = ntohl(packet -> ackno);

    while(first_node != NULL && (ntohl(packet->seqno) == (uint32_t) r->RCV_NXT) && (conn_bufspace(r->c) >= (packet_length - 12))){
        packet_len = ntohs(packet -> len);
        packet_seqno = nthol(packet -> seqno);

        if (is_EOF(packet)) {
            conn_output(r->c, packet->data, htons(0)); /*check above what conn_output does when length is 0*/
            bufer_remove_first(r -> rec_buffer); /* remove EOF packet*/
            r -> RCV_NXT++;
            r -> EOF_RECEIVED = 1;


        } else {
            if (conn_bufspace(r -> c) >= (packet_len - 12)) {
                int bytes_flushed = conn_output(r -> c, packet -> data, (size_t) packet_len - 12);
                buffer_remove_first(r -> rec_buffer);
                r -> RCV_NXT++;
                r -> flushing = 0;
			} else {
				int bytes_flushed = -1;
			}
        }

        struct ack_packet * ack_pac = xmalloc(sizeof(struct ack_packet));
        create_packet(ack_pac, htons((uint16_t) 8), -1, htonl((uint32_t) (r -> RCV_NXT), 0);
        conn_sendpkt(r -> c, (packet_t *) ack_pac, (size_t) 8);
        free(ack_pac);
        if (isDone(r)) {
		    rel_destroy(r);
            return;
		}

        first_node = buffer_get_first(r->rec_buffer);
        if(first_node != NULL){
            packet = &(first_node->packet);
        }

        packet_length = ntohs(packet->len);
        packet_seqno = ntohl(packet->seqno);
    }
}



void rel_timer ()
{
    rel_t *current = rel_list;
    while (current != NULL) {
        buffer_node_t* node = buffer_get_first(current->send_buffer);
        packet_t* packet;
        int i = 1;
        while(i > 0 && node != NULL){
          if(node != NULL) {
              packet = &node->packet;
              long cur_time = get_current_system_time();
              long last_time = node->last_retransmit;
              long timeout = current->timeout;

              if ((cur_time - last_time) >= timeout) {
                  conn_sendpkt(current->c, packet, (size_t)(ntohs(packet->len)));
                  node->last_retransmit = cur_time;
              }
          }
            i--;
            node = node->next;
        }
        current = current->next;
    }
}
