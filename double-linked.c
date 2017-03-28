/* Inserting a value below a particular value and removing some values from bottom part of list 
aboosidhik@gmail.com
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct janus_pp_frame_packet {
        int i; 
        struct janus_pp_frame_packet *next;
	struct janus_pp_frame_packet *prev;
} janus_pp_frame_packet;

typedef struct janus_pp_frame_packet_list {
    size_t size;
    struct janus_pp_frame_packet *head;
    struct janus_pp_frame_packet *tail;
}janus_pp_frame_packet_list;
 
static int mp;

int main () {
    janus_pp_frame_packet_list *janus_pp_frame_packet_list_1 = malloc(sizeof(janus_pp_frame_packet_list));
    mp = 0;
    int qm;
    for(qm = 0; qm < 8; qm++) {
        janus_pp_frame_packet *janus_pp_frame_packet_1 = malloc(sizeof(janus_pp_frame_packet));
        mp++;
        janus_pp_frame_packet_1->i = mp;
        if (janus_pp_frame_packet_list_1->head) {
            janus_pp_frame_packet_1->next = janus_pp_frame_packet_list_1->head;
            janus_pp_frame_packet_1->prev = janus_pp_frame_packet_list_1->head->prev;
            janus_pp_frame_packet_list_1->head->prev->next = janus_pp_frame_packet_1;
            janus_pp_frame_packet_list_1->head->prev = janus_pp_frame_packet_1;
        } else {
            janus_pp_frame_packet_1->next = janus_pp_frame_packet_1;
            janus_pp_frame_packet_1->prev = janus_pp_frame_packet_1;
            janus_pp_frame_packet_list_1->tail = janus_pp_frame_packet_1;
        }
        janus_pp_frame_packet_list_1->head = janus_pp_frame_packet_1;
        janus_pp_frame_packet_list_1->size++;
    }
    janus_pp_frame_packet *janus_pp_frame_packet_1 = janus_pp_frame_packet_list_1->head;
    for(qm = 0; qm < 8; qm++) {
        printf("%i \n",janus_pp_frame_packet_1->i);
        janus_pp_frame_packet_1 = janus_pp_frame_packet_1->next;
    }
    
    for(qm = 0; qm < 4; qm++) {
        janus_pp_frame_packet *janus_pp_frame_packet_1_1 = janus_pp_frame_packet_list_1->tail;
        if (janus_pp_frame_packet_1_1 != NULL) {
            janus_pp_frame_packet_1_1->prev->next = janus_pp_frame_packet_1_1->next;
            janus_pp_frame_packet_1_1->next->prev = janus_pp_frame_packet_1_1->prev;
            if (janus_pp_frame_packet_list_1->head == janus_pp_frame_packet_1_1) {
                janus_pp_frame_packet_list_1->head = janus_pp_frame_packet_1_1->next;
            }
            if (janus_pp_frame_packet_list_1->tail == janus_pp_frame_packet_1_1) {
                janus_pp_frame_packet_list_1->tail = janus_pp_frame_packet_1_1->prev;
            }
            janus_pp_frame_packet_list_1->size--;
            if (janus_pp_frame_packet_list_1->size == 0) {
                janus_pp_frame_packet_list_1->tail = NULL;
                janus_pp_frame_packet_list_1->head = NULL;
            }
        }
        free(janus_pp_frame_packet_1_1);
    }
    janus_pp_frame_packet *janus_pp_frame_packet_20 = janus_pp_frame_packet_list_1->head;
    for(qm = 0; qm < janus_pp_frame_packet_list_1->size; qm++) {
        printf("****%i \n",janus_pp_frame_packet_20->i);
        janus_pp_frame_packet_20 = janus_pp_frame_packet_20->next;
    }
    
    for(qm = 0; qm < 8; qm++) {
        janus_pp_frame_packet *janus_pp_frame_packet_1_1 = malloc(sizeof(janus_pp_frame_packet));
        mp++;
        janus_pp_frame_packet_1_1->i = mp;
        if (janus_pp_frame_packet_list_1->head) {
            janus_pp_frame_packet_1_1->next = janus_pp_frame_packet_list_1->head;
            janus_pp_frame_packet_1_1->prev = janus_pp_frame_packet_list_1->head->prev;
            janus_pp_frame_packet_list_1->head->prev->next = janus_pp_frame_packet_1_1;
            janus_pp_frame_packet_list_1->head->prev = janus_pp_frame_packet_1_1;
        } else {
            janus_pp_frame_packet_1_1->next = janus_pp_frame_packet_1_1;
            janus_pp_frame_packet_1_1->prev = janus_pp_frame_packet_1_1;
            janus_pp_frame_packet_list_1->tail = janus_pp_frame_packet_1_1;
        }
        janus_pp_frame_packet_list_1->head = janus_pp_frame_packet_1_1;
        janus_pp_frame_packet_list_1->size++;
        
    }
    
    
    janus_pp_frame_packet_1 = janus_pp_frame_packet_list_1->head;
    for(qm = 0; qm < janus_pp_frame_packet_list_1->size; qm++) {
        printf("++++%i \n",janus_pp_frame_packet_1->i);
        janus_pp_frame_packet_1 = janus_pp_frame_packet_1->next;
    }
    
    janus_pp_frame_packet_1 = janus_pp_frame_packet_list_1->head;
    for(qm = 0; qm < janus_pp_frame_packet_list_1->size; qm++) {
        if(janus_pp_frame_packet_1->i == 16) {
            janus_pp_frame_packet *janus_pp_frame_packet_22 = malloc(sizeof(janus_pp_frame_packet));
            janus_pp_frame_packet_22->i = 4;
            if (janus_pp_frame_packet_list_1->head) {
                janus_pp_frame_packet_22->next = janus_pp_frame_packet_1->next;
                janus_pp_frame_packet_22->prev = janus_pp_frame_packet_1;
                janus_pp_frame_packet_1->next = janus_pp_frame_packet_22;
                janus_pp_frame_packet_1->next->prev = janus_pp_frame_packet_22;
                if(janus_pp_frame_packet_list_1->tail->i == janus_pp_frame_packet_1->i) 
                    janus_pp_frame_packet_list_1->tail = janus_pp_frame_packet_22;
            } else {
                janus_pp_frame_packet_22->next = janus_pp_frame_packet_22;
                janus_pp_frame_packet_22->prev = janus_pp_frame_packet_22;
                janus_pp_frame_packet_list_1->tail = janus_pp_frame_packet_22;
                janus_pp_frame_packet_list_1->head = janus_pp_frame_packet_22;
            }
            janus_pp_frame_packet_list_1->size++;
            break;
        }
        janus_pp_frame_packet_1 = janus_pp_frame_packet_1->next;
    }
    
    janus_pp_frame_packet_1 = janus_pp_frame_packet_list_1->head;
    for(qm = 0; qm < janus_pp_frame_packet_list_1->size*2; qm++) {
        printf("+*++*+%i \n",janus_pp_frame_packet_1->i);
        janus_pp_frame_packet_1 = janus_pp_frame_packet_1->next;
    }
    
    printf("head  %i \n",janus_pp_frame_packet_list_1->head->i);
    printf("tail  %i \n",janus_pp_frame_packet_list_1->tail->i);
    return 0;
}
