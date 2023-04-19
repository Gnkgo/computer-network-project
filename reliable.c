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

long currentTimeMillis();
bool isDone(rel_t * r);
bool should_send_packet(rel_t *s);
void send_packet(packet_t * packet, rel_t * s);
int is_EOF(packet_t* packet);
void create_packet(packet_t * packet, int len, int seqno, int ackno, int isData);
bool is_ACK(packet_t* packet);
int max(int num1, int num2);



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

    int EOF_ERR;
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

    /*error*/
    r -> EOF_SENT = 0;
    r -> EOF_RECV = 0;
    r -> EOF_ACK_RECV = 0;
    r -> EOF_seqno = 0;
    r -> flushing = 0;


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





void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{

    uint16_t packet_length = ntohs(pkt->len);
    uint16_t packet_cksum_old = ntohs(pkt->cksum);
    uint16_t packet_seqno = ntohl(pkt->seqno);

    uint16_t cksum_old_to_restore = pkt->cksum;
    pkt->cksum = (uint16_t) 0;


    if((packet_length != (uint16_t) n) || (packet_cksum_old != ntohs(cksum(pkt, (int) packet_length)))){

        return;
    }
    pkt->cksum = cksum_old_to_restore;

    if (is_ACK(pkt)){

        if(pkt->ackno == r->EOF_seqno + 1){
            r->EOF_ACK_RECV = 1;
        }



        int acked_packet_number = buffer_remove(r->send_buffer, (uint32_t) ntohl(pkt->ackno));


        r->SND_UNA = max((int) ntohl(pkt->ackno), r->SND_UNA);



        if(isDone(r)){

            rel_destroy(r);
            return;
        }
            rel_read(r);


//        return NULL;
    }else{

        if(packet_seqno >= r->RCV_NXT){


            int MAXWND = r->MAXWND;


            if(packet_seqno >= r->RCV_NXT + r->MAXWND){

                return;
            }else{
                if(conn_bufspace(r->c) >= (packet_length - 12)){
                    if(!buffer_contains(r->rec_buffer, ntohl(pkt->seqno))) {
                        buffer_insert(r->rec_buffer, pkt, currentTimeMillis());
                    }
                    rel_output(r);
                }else{
                    return;
                }
            }
        }else{
            if(packet_seqno != 0){
                for(int i = 0; i < 1;i++){
                    struct ack_packet* ack_pac = xmalloc(sizeof(struct ack_packet));
                    ack_pac->ackno = htonl((uint32_t) (r->RCV_NXT));
                    ack_pac->len = htons ((uint16_t) 8);
                    ack_pac->cksum = (uint16_t) 0;
                    ack_pac->cksum = cksum(ack_pac, (int) 8);
                    conn_sendpkt(r->c, (packet_t *)ack_pac, (size_t) 8);
                    free(ack_pac);
                }
                return;
            }
        }
    }
}




void rel_output (rel_t *r) {

    buffer_node_t* first_node = buffer_get_first(r->rec_buffer);

    if(first_node == NULL){
        return;
    }

    packet_t* packet = &(first_node->packet);
    uint16_t packet_length = ntohs(packet->len);
    uint16_t packet_seqno = ntohl(packet->seqno);
    while((first_node != NULL) &&
          (ntohl(packet->seqno) == (uint32_t) r->RCV_NXT) &&
          (conn_bufspace(r->c) >= (packet_length - 12))){
        packet_length = ntohs(packet->len);
        packet_seqno = ntohl(packet->seqno);

        if(is_EOF(packet)){

            conn_output(r->c, packet->data, htons(0)); //send a signal to output by calling conn_output with len 0

            buffer_remove_first(r->rec_buffer); //remove either EOF or Data packet whatever
            r->RCV_NXT ++;
            r->EOF_RECV = 1;

            struct ack_packet* ack_pac = xmalloc(sizeof(struct ack_packet));
            ack_pac->ackno = htonl((uint32_t) (r->RCV_NXT));
            ack_pac->len = htons ((uint16_t) 8);
            ack_pac->cksum = (uint16_t) 0;
            ack_pac->cksum = cksum(ack_pac, (int) 8);
            conn_sendpkt(r->c, (packet_t *)ack_pac, (size_t) 8);


            free(ack_pac);



            if(r->EOF_ACK_RECV && r->EOF_SENT &&r->EOF_RECV && !r->flushing && buffer_size(r->send_buffer) == 0){


                rel_destroy(r);
                return;
            }


        }else{ //is not EOF!!!!!

            int bytes_flushed;
            if(conn_bufspace(r->c) >= (packet_length - 12)){
                r->flushing = 1;
                bytes_flushed = conn_output(r->c, packet->data, (size_t) (packet_length - 12));



                buffer_remove_first(r->rec_buffer); //remove either EOF or Data packet whatever
                r->RCV_NXT ++;

                r->flushing = 0;

                struct ack_packet* ack_pac = xmalloc(sizeof(struct ack_packet));
                ack_pac->ackno = htonl((uint32_t) (r->RCV_NXT));
                ack_pac->len = htons ((uint16_t) 8);
                ack_pac->cksum = (uint16_t) 0;
                ack_pac->cksum = cksum(ack_pac, (int) 8);

                conn_sendpkt(r->c, (packet_t *)ack_pac, (size_t) 8);


                free(ack_pac);

            }else{
                bytes_flushed = -1;

            }
        }



        first_node = buffer_get_first(r->rec_buffer);
        if(first_node != NULL){
            packet = &(first_node->packet);
        }

        packet_length = ntohs(packet->len);
        packet_seqno = ntohl(packet->seqno);
    }

}







void
rel_read (rel_t *s)
{
    int SND_UNA;
    int SND_NXT;
    int MAXWND;
    int read_byte;
    SND_UNA = s->SND_UNA;
    SND_NXT = s->SND_NXT;
    MAXWND = s->MAXWND;

    if((s->EOF_SENT)){

       return;
    }

    while(should_send_packet(s)) {
        packet_t *packet = (packet_t *) xmalloc(512);
        memset(packet, 0 , sizeof(packet_t));
        read_byte = conn_input(s->c, packet->data, 500);

        if (read_byte == -1) {
            s->EOF_SENT = 1;
            s->EOF_seqno = s->SND_NXT;

            packet->len = htons((uint16_t) 12);
            packet->ackno = htonl((uint32_t) 0); //EOF packet, ackno doesn't matter
            packet->seqno = htonl((uint32_t) SND_NXT);
            s->SND_NXT = s->SND_NXT + 1;

            packet->cksum = (uint16_t) 0;
            packet->cksum = cksum(packet, 12);

            buffer_insert(s->send_buffer, packet, currentTimeMillis());
            conn_sendpkt(s->c, packet, (size_t) 12);

        } else if (read_byte == 0) {
            free(packet);
            break;
        } else {
            packet->len = htons((uint16_t)(12 + read_byte));
            packet->ackno = htonl((uint32_t) 0); //data packet, ackno doesn't matter
            packet->cksum = htons((uint16_t) 0);
            packet->seqno = htonl((uint32_t) SND_NXT);
            s->SND_NXT = s->SND_NXT + 1;

            packet->cksum = (uint16_t) 0;
            packet->cksum = cksum(packet, 12 + read_byte);

            buffer_insert(s->send_buffer, packet, currentTimeMillis());

            conn_sendpkt(s->c, packet, (size_t) (12 + read_byte));
        }
        free(packet);
        SND_UNA = s->SND_UNA;
        SND_NXT = s->SND_NXT;
        MAXWND = s->MAXWND;

    }
}

void
rel_timer ()
{

    rel_t *current = rel_list;
    while (current != NULL) {
        buffer_node_t* node = buffer_get_first(current->send_buffer);
        packet_t* packet;
        int i = 1;

        while(i > 0 && node != NULL){
          if(node != NULL) {
              packet = &node->packet;
              long cur_time = currentTimeMillis();
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





/*helper functins */
/*https//stackoverflow.com/questions/10098441/get-the-current-time-in-milliseconds-in-c*/
long currentTimeMillis() {
  struct timeval time;
  gettimeofday(&time, NULL);
  int64_t s1 = (int64_t)(time.tv_sec) * 1000;
  int64_t s2 = (time.tv_usec / 1000);
  return s1 + s2;
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
    conn_sendpkt(s->c, packet, (size_t) 12);
}


int is_EOF(packet_t* packet){
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

int max(int num1, int num2) {
    return (num1 > num2 ) ? num1 : num2;
}