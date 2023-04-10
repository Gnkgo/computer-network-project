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

rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc)
{
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
    r->EOF_SENT = 0;
    r->EOF_RECV = 0;
    r->EOF_ACK_RECV = 0;
    r->EOF_seqno = 0;

    r->fptr = fopen("./mycodeErrLog.txt", "w");

      
    return r;
}

void
rel_destroy (rel_t *r)
{

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

bool is_ACK(packet_t* pkt) {
	return (ntohs(pkt->len) == 8);
}

// n is the expected length of pkt
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
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

    if (is_ACK(pkt)) {
    /*if the packet is an ACK*/
        fprintf(r->fptr, "ACKACKACKACKACK\n\n\n");
		if (rec_ackno == r->EOF_seqno + 1) {
			/*if the ACK is for the EOF packet*/
			r->EOF_ACK_RECV = 1;
			if (r->EOF_SENT && r->EOF_RECV && r->EOF_ACK_RECV) {
				/*if all EOF flags are set, close the connection*/
				rel_destroy(r);
			}
		} else {
			/*if the ACK is for a data packet*/
			int acked_packet_number = buffer_remove(r->send_buffer, rec_ackno);
            fprintf(stderr, "Sender buffer removed acked packets: %x\n", acked_packet_number);			
            
            /*if (r->send_buffer->head == NULL) {
				/*if the send buffer is empty, stop the timer
				conn_timer(r->c, 0);
			}*/
		}
	} else {
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
    } 
}




void
rel_read (rel_t *s)
{
    /* Your logic implementation here */
}

void
rel_output (rel_t *r)
{
    /* Your logic implementation here */
}

void
rel_timer ()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    while (current != NULL) {
        // ...
        current = current->next;
    }
}
