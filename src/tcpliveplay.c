

/*
 * Main Author & Publisher: Yazan H. Siam (tcpliveplay@gmail.com)  
 * File: tcpliveplay.c 
 * Started as a Senior Design project @ North Carolina State University
 * Last Updated Date: September 5, 2012
 * 
*/

/**
 * Program Description: 
 * This program, 'tcpliveplay' replays a captured set of packets using new TCP connections with the 
 * captured TCP payloads against a remote host in order to do comprehensive vulnerability testings. 
 * This program takes in a "*.pcap" file that contains only one tcp flow connection and replays it
 * against a live host exactly how the captured packets are layed out.  At the beginning, the program 
 * establishes who the 'client' is and the 'server' is based on who initiates the SYN compares each 
 * packet's source ip against the ip of the 'client' (which is named local in the code) and the 'server' 
 * (remote) to correctly determine the expected seqs & acks. This also extracts the MACs of both local 
 * and remote clients. The program is also capable of rewriting the local and remote MAC & IP so that 
 * the packets are properly replayed when used on live networks. The current state of the program is that 
 * it takes in a pcap file on command line and writes a new file called "newfile.pcap" which is used thereafter 
 * for the rest of the program's calculations and set expectations. The program prints out a summary of the 
 * new file on the command prompt. Once the program is done, "newfile.pcap" is cleaned up. 

 * Prgram Design Overview: 
 * Before replaying the packets, the program reads in the pcap file that contains one tcp flow, 
 * and takes the SEQ/ACK #s. 
 * Based on the number of packets, a struct schedule of events are is set up. Based on 
 * the SEQ/ACK numbers read in, the schedule is setup to be relative numbers rather than 
 * absolute. This is done by starting with local packets, subtracting the first SEQ (which 
 * is that of the first SYN packet) from all the SEQs of the local packets then by subtracting 
 * the first remote sequence (which is that of the SYN-ACK packet) from all the local packet's 
 * ACKs. After the local side SEQ/ACK numbers are fixed to relative numbers, 'lseq_adjust' 
 * the locally generated random number for the SYN packet gets added to all the local SEQs 
 * to adjust the schedule to absolute number configuration. Then doing the remote side is similar 
 * except we only fix the remote ACKs based on our locally generated random number because 
 * we do not yet know the remote random number of the SYN-ACK packet. This means that at this 
 * point the entire schedule of local packets and remote packets are set in such a way that 
 * the local packets' SEQ's are absolute, but ACKs are relative and the remote packets' SEQ's are 
 * relative but ACKs as absolute. Once this is set, the replay starts by sending first SYN packet. 
 * If the remote host's acks with the SYN packet_SEQ+1 then we save their remote SEQ and adjust
 * the local ACKs and remote SEQs in the struct schedule to be absolute based this remote SEQ. 
 * From this point on forward, we know or 'expect' what the remote host's ACKs and SEQs are exactly. 
 * If the remote host responds correctly as we expect (checking the schedule position expectation 
 * as packets are received) then we proceed in the schedule whether the next event is to send a local 
 * packet or wait for a remote packet to arrive. 

 *
 * Usage: tcpliveplay <eth0/eth1> <file.pcap> <Destinatin IP [1.2.3.4]> <Destination mac [0a:1b:2c:3d:4e:5f]> <'random' dst port OR specify dport #>
 *
 * Example:  
 * yhsiam@yhsiam-VirtualBox:~$ tcpliveplay eth0 test1.pcap 192.168.1.4 52:57:01:11:31:92 random
 * 
 * NOTE: This program may not completely replay the packets due to the remote host responding in an unexpected 
 * fashion such as responding with packets never seen in the given *.pcap file or coupling packets together, etc. 
 * if you have any suggestion on improving this software or if you find any bugs, please let me know at my 
 * email: tcpliveplay@gmail.com 
 * 
 * Past Contributors (Last contributed May 4, 2012): Andrew Leonard & Beau Luck 
 *
 */

#include "config.h"
#include "defines.h"
#include "common.h"

#include <signal.h>
#include <time.h>
#include <utime.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/types.h>    
#include <sys/socket.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>

#include "tcpliveplay.h"
#include "tcpliveplay_opts.h"
#include "common/sendpacket.h"
#include "send_packets.h"

volatile int didsig;

#ifdef DEBUG  /* set -DDEBUG=1 */
int debug = 0;
#endif

pcap_t *set_live_filter(char *dev, in_addr* hostip, unsigned int port);
pcap_t *set_offline_filter(char* file);
pcap_t *live_handle; 
u_char *recvd_pkt_data = NULL;
unsigned int buf_write_index = 0; 
unsigned int sched_index = 0; 
unsigned int initial_rseq=0; 
sendpacket_t *sp;

const u_char *packet_keeper_rprev = NULL;
ether_hdr *etherhdr_rprev = NULL; /*g for Global header pointers used in pcap_loop callback*/
tcp_hdr *tcphdr_rprev = NULL;
ipv4_hdr *iphdr_rprev = NULL;
unsigned int size_payload_prev = 0; 
unsigned int finack_rindex = 0; 
unsigned int acked_index = 0;
unsigned int diff_payload_index = 0; 
bool different_payload = false;  
volatile sig_atomic_t keep_going = 1;
int random_port();
unsigned int pkts_scheduled=0;   /* packet counter */ 
struct tcp_sched* sched = NULL; 
void got_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet);
void catch_alarm (int sig);
void iface_addrs(char* iface, in_addr* ip, struct mac_addr* mac);
int extmac(char* new_rmac_ptr, struct mac_addr* new_remotemac);
int extip(char *ip_string, in_addr* new_remoteip);
int rewrite(in_addr* new_remoteip, struct mac_addr* new_remotemac, in_addr* myip, struct mac_addr* mymac, char* file, unsigned int new_src_port); 
int setup_sched(struct tcp_sched* sched);
int relative_sched(struct tcp_sched* sched, u_int32_t first_rseq, int num_packets);
int fix_all_checksum_liveplay(ipv4_hdr *iphdr);
int compip(in_addr* lip, in_addr* rip, in_addr* pkgip);
int do_checksum_liveplay(u_int8_t *data, int proto, int len);
int do_checksum_math_liveplay(u_int16_t *data, int len);

/**
 * This is the main function of the program that handles calling other 
 * functions to implemented the needed operations of the replay functionaily.
 */
int 
main(int argc, char **argv) 
{
    unsigned int k;
    unsigned int num_packets=0;	           

    char port_mode[10];    /* does user specify random port generation?*/
    char random_strg[7] = "random";
 
    char* iface = argv[1];
    char* new_rmac_ptr; 
    char* new_rip_ptr; 
    in_addr new_remoteip; 
    struct mac_addr new_remotemac;
    in_addr myip;
    struct mac_addr mymac;


    /*temporary packet buffers*/


    unsigned int new_src_port = 0; 
    unsigned int retransmissions = 0; 
    
    pcap_t *local_handle;
    char errbuf[PCAP_ERRBUF_SIZE];


     
    char ebuf[SENDPACKET_ERRBUF_SIZE];
    

    optionProcess(&tcpliveplayOptions, argc, argv); /*Process AutoOpts for manpage options*/

    if((argc < 5) || (argv[1]==NULL) || (argv[2]==NULL) || (argv[3]==NULL) || (argv[4]==NULL) || (argv[5]==NULL)){
        printf("ERROR: Incorrect Usage!\n"); 
        printf("Usage: tcpliveplay <eth0/eth1> <file.pcap> <Destinatin IP [1.2.3.4]> <Destination mac [0a:1b:2c:3d:4e:5f]> <specify 'random' or specific port#>\n");
        printf("Example:\n    yhsiam@yhsiam-VirtualBox:~$ sudo tcpliveplay eth0 test1.pcap 192.168.1.4 52:57:01:11:31:92 random\n\n"); 
        exit(0);
    }

    iface_addrs(iface, &myip, &mymac);	/* Extract MAC of interface replay is being request on */

    /* open send function socket*/
    if ((sp = sendpacket_open(iface, ebuf, TCPR_DIR_C2S, SP_TYPE_NONE)) == NULL)
        errx(-1, "Can't open %s: %s", argv[1], ebuf);

    /* random dport vs. specified dport operation*/
    strcpy(port_mode, argv[5]);
    /*for(int i = 0; i<10; i++) tolower(port_mode[i]);*/
    if(strcmp(port_mode, random_strg)==0){
         new_src_port = random_port();
    } else new_src_port = atoi(argv[5]);
     /*else {
	 printf("Port specification error. Please specify 'random' for random source port generation between 49152 and 65535 OR specifiy a specific source port number.\n");
         return; 
    }*/
    printf("new source port:: %d\n", new_src_port);

    /* Establish a handler for SIGALRM signals. */
    /* This is used as timeout for unresponsive remote hosts */
    signal (SIGALRM, catch_alarm);


    /* Extract new Remote MAC & IP inputted at command line */
    new_rmac_ptr= argv[4];
    new_rip_ptr = argv[3]; 

    /* These function setup the MAC & IP addresses in the mac_addr & in_addr structs */
    extmac(new_rmac_ptr, &new_remotemac);
    extip(new_rip_ptr, &new_remoteip);

    /* Rewrites the given "*.pcap" file with all the new parameters and returns the number of packets */
    /* that need to be replayed */
    num_packets = rewrite(&new_remoteip, &new_remotemac, &myip, &mymac, argv[2], new_src_port); 
    
    /* create schedule & set it up */
    sched = (struct tcp_sched*) malloc(num_packets*sizeof(struct tcp_sched));  
    pkts_scheduled = setup_sched(sched);    /* Returns number of packets in schedule*/

    /* Set up the schedule struct to be relative numbers rather than absolute*/
    relative_sched(sched, sched[1].exp_rseq, num_packets);
    printf("Packets Scheduled %d\n", pkts_scheduled);

    /* Open socket for savedfile traffic to be sent*/
    local_handle = pcap_open_offline("newfile.pcap", errbuf);   /*call pcap library function*/
    if (local_handle == NULL) {
        fprintf(stderr,"Couldn't open pcap file %s: %s\n", "newfile.pcap", errbuf);
        return(2);
    }

    /* Open socket for live traffic to be listed to*/
    live_handle = set_live_filter(iface, &myip, new_src_port);  /* returns a pcap_t that filters out traffic other than TCP*/
    if (live_handle == NULL) {
        fprintf(stderr,"Error occured while listing on traffic: %s\n", errbuf);
        return(2);
    }
    

    /* Printout when no packets are scheduled */
    if(pkts_scheduled==0){
        printf("\n+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
        printf("+ ERROR:: There are no TCP packets to sent                      +\n"); 
        printf("+ Closing replay...                                             +\n");
        printf("+ Thank you for Playing, Play again!                            +\n");
        printf("+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n\n");
        return ERROR;
    }

    /* Start replay by sending the first packet, the SYN, from the schedule */
    else if(sched[0].local){  /* Send first packet*/
        sendpacket(sp, sched[sched_index].packet_ptr, sched[sched_index].pkthdr.len, &sched[sched_index].pkthdr);
        printf("Sending Local Packet...............	[%d]\n",sched_index+1);
        sched_index++; /* Proceed in the schedule */
    }
    
    /* Main while loop that handles the decision making and the replay oprations */
    while(sched_index<pkts_scheduled){
        if(!keep_going) { /*Check the timeout variable */
            printf("\n======================================================================\n");
            printf("= TIMEOUT:: Remote host is not responding. You may have crashed      =\n"); 
            printf("= the host you replayed these packets against OR the packet sequence =\n");
            printf("= changed since the capture was taken resulting in differing         =\n");
            printf("= expectations. Closing replay...                                    =\n");
            printf("======================================================================\n\n"); 
            break; 
        } 
        /* tcphdr_rprev carries the last remote tcp header */
        if(tcphdr_rprev == NULL) {
	    //printf("FIRST PASS!\n");
        } 
        /* Check if received RST or RST-ACK flagged packets*/
        else if((tcphdr_rprev->th_flags==TH_RST) || (tcphdr_rprev->th_flags==(TH_RST|TH_ACK))){
            printf("\n++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
            printf("+ ERROR:: Remote host has requested to RESET the connection.   +\n"); 
            printf("+ Closing replay...                                            +\n");
            printf("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n\n"); 
            break; 
        }
        /*Check if received earlier FIN-ACK than expected 
        else if((sched_index-1 < finack_rindex) && (tcphdr_rprev->th_flags==(TH_FIN|TH_ACK))){
            printf("\n++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
            printf("+ ERROR:: Remote host sent an earlier FIN-ACK than expected.   +\n"); 
            printf("+ Closing replay...                                            +\n");
            printf("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n\n");
            return; 
        } */
        /* Do the following if we receive a packet that ACKs for the same ACKing of next packet */
        else if((tcphdr_rprev->th_seq==htonl(sched[sched_index].exp_rseq)) && (tcphdr_rprev->th_ack==htonl(sched[sched_index].exp_rack)) && (size_payload_prev>0)){
            printf("Received Remote Packet...............	[%d]\n",sched_index+1);
            printf("Skipping Packet......................	[%d] to Packet [%d]\n",sched_index+1, sched_index+2);
            printf("Next Remote Packet Expectation met.\nProceeding in replay...\n");
            sched_index++;   
        } 
        /* Do the following if payload does not meet expectation and re-attempt with the remote host for 3 tries*/
        else if(different_payload){
            printf("\n+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
            printf("+ WARNING: Remote host is not meeting packet size expectations.               +\n"); 
            printf("+ for packet %-d. Application layer data differs from capture being replayed.  +\n", diff_payload_index+1);
            printf("+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n\n"); 
            printf("Requesting retransmission.\n Proceeding...\n");
            different_payload = false;  
        }

        /* Local Packets */
        if(sched[sched_index].local) {
            /*Reset alarm timeout*/
            alarm (ALARM_TIMEOUT);
            printf("Sending Local Packet...............	[%d]\n",sched_index+1); 
          
            /* edit each packet tcphdr before sending based on the schedule*/  
            if(sched_index>0){ 
                sched[sched_index].tcphdr->th_ack = htonl(sched[sched_index].curr_lack); 
                fix_all_checksum_liveplay(sched[sched_index].iphdr);
            }
            
            /* If 3 attempts of resending was made, then error out to the user */
            if(sched[sched_index].sent_counter==3){
                printf("\n++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
                printf("+ ERROR: Re-sent packet [%-d] 3 times, but remote host is not  +\n", sched_index+1); 
                printf("+ responding as expected. 3 resend attempts are a maximum.     +\n");
                printf("+ Closing replay...                                            +\n");
                printf("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n\n");
                break; 
            }

            /* If nothing goes wrong, then send the packet scheduled to be sent, then proceed in the schedule */
            sendpacket(sp, sched[sched_index].packet_ptr, sched[sched_index].pkthdr.len, &sched[sched_index].pkthdr);
            sched[sched_index].sent_counter++; /* Keep track of how many times this specific packet was attempted */
            sched_index++;   /* proceed */
        }

        /* Remote Packets */
        else if(sched[sched_index].remote){
            
            alarm (ALARM_TIMEOUT);
            printf("Receiving Packets from remote host...\n");
            pcap_dispatch(live_handle, 1, got_packet, NULL); /* Listen in on NIC for tcp packets */
            //printf("pcap_loop returned\n");
        }
    } /* end of main while loop*/

    
    pcap_breakloop(live_handle); 

    pcap_close(live_handle);
    sendpacket_close(sp);  /* Close Send socket*/
    remove("newfile.pcap"); /* Remote the rewritten file that was created*/

    for(k=0; k<pkts_scheduled; k++){
        retransmissions+=sched[k].sent_counter; 
    }


    /* User Debug Result Printouts*/
    if(sched_index==pkts_scheduled){
        printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"); 
        printf("~ CONGRATS!!! You have successfully Replayed your pcap file '%s'  \n", argv[2]);
        printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n\n");
    }
    else {
        printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"); 
        printf("~ Unfortunately an error has occurred  halting the replay of  \n");
        printf("~ the pcap file '%s'. Please see error above for details...   \n", argv[2]);
        printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n\n");
    }

        printf("----------------TCP Live Play Summary----------------\n"); 
        printf("- Packets Scheduled to be Sent & Received: 	    %-d   \n", pkts_scheduled);
        printf("- Actual Packets Sent & Received:                   %-d   \n", sched_index); 
        printf("- Total Local Packet Re-Transmissions due to packet       \n");
        printf("- loss and/or differing payload size than expected: %-d   \n", retransmissions);
        printf("- Thank you for Playing, Play again!                      \n");
        printf("----------------------------------------------------------\n\n"); 

return 0;
} 
/*end of main() function*/


/**
 * This function serves as a timer alarm 
 */
void
catch_alarm (int sig){
    keep_going = 0;
    signal (sig, catch_alarm);
}

/**
 * This function returns a random number between 49152 and 65535
 */
int
random_port() {
    srand(time(NULL));
    int port_num = 49152 + (rand() % 16383);
    return port_num;
}


/**
 * This function sets up the scheduled local ACK and Remote SEQ to be relative numbers, 
 * While it sets up the local SEQs and remote ACKs to be absolute within the schedule. 
 */

int
relative_sched(struct tcp_sched* sched, u_int32_t first_rseq, int num_packets){
    int i;
    u_int32_t lseq_adjust; 
    srand(time(NULL));
    lseq_adjust = rand();  /*Local SEQ number for SYN packet*/
    printf("Random Local SEQ: %u\n",lseq_adjust);
   

   u_int32_t first_lseq = sched[0].curr_lseq;   /* SYN Packet SEQ number */
   /* Fix schedule to relative and absolute */
   for(i = 0; i < num_packets; i++){
       if(sched[i].local){
            sched[i].curr_lseq = sched[i].curr_lseq - first_lseq; /* Fix current local SEQ to relative */
            sched[i].curr_lseq = sched[i].curr_lseq + lseq_adjust; /* Make absolute. lseq_adjust is the locally generated random number */
            sched[i].curr_lack = sched[i].curr_lack - first_rseq; /* Fix current local ACK to relative */
            sched[i].tcphdr->th_seq = htonl(sched[i].curr_lseq); /* Edit the actual packet header data */
            fix_all_checksum_liveplay(sched[i].iphdr); /* Fix the checksum */
	    sched[i].exp_rseq = sched[i].exp_rseq - first_rseq; 
            sched[i].exp_rack = sched[i].exp_rack - first_lseq;
            sched[i].exp_rack = sched[i].exp_rack + lseq_adjust;  
       }
       else if(sched[i].remote){
            sched[i].exp_rseq = sched[i].exp_rseq - first_rseq; /* Fix expected remote SEQ to be relative */
            sched[i].exp_rack = sched[i].exp_rack - first_lseq; /* Fix expected remote ACK to be relative*/
            sched[i].exp_rack = sched[i].exp_rack + lseq_adjust; /* Fix expeted remote ACK to be absolute */
       }
    }


return SUCCESS; 

}


/**
 * This function sets up the schedule for the rest of the program
 * extracting all the needed information from the given pcap file
 * and coping into memory (into a struct format) 
 *
 */

int
setup_sched(struct tcp_sched* sched){

    in_addr sip, dip;  /* Source & Destination IP */ 
    in_addr local_ip, remote_ip;	/* ip address of client and server*/
    /*temporary packet buffers*/
    struct pcap_pkthdr header; 	// The header that pcap gives us
    const u_char *packet; 		// The actual packet
    pcap_t *local_handle;
    unsigned int pkt_counter=0; 
    ether_hdr *etherhdr = NULL; 
    tcp_hdr *tcphdr = NULL;
    ipv4_hdr *iphdr = NULL;
    local_ip.byte1=0; 
    local_ip.byte2=0; 
    local_ip.byte3=0; 
    local_ip.byte4=0; 

    remote_ip.byte1=0; 
    remote_ip.byte2=0; 
    remote_ip.byte3=0; 
    remote_ip.byte4=0; 

    unsigned int size_ip, i = 0; 
    unsigned int size_tcp; 
    unsigned int size_payload; 
    char errbuf[PCAP_ERRBUF_SIZE];
    unsigned int flags=0; 
    bool remote = false; 	/* flags to test if data is from 'cleint'=local or 'server'=remote */
    bool local = false; 


    local_handle = pcap_open_offline("newfile.pcap", errbuf);   /*call pcap library function*/

    if (local_handle == NULL) {
        fprintf(stderr,"Couldn't open pcap file %s: %s\n", "newfile.pcap", errbuf);
        return(2);
    }
	
    /*Before sending any packet, setup the schedule with the proper parameters*/
    while((packet = pcap_next(local_handle,&header))) {
        pkt_counter++; /*increment number of packets seen*/
        
        memcpy(&sched[i].pkthdr, &header, sizeof(struct pcap_pkthdr));
        //sched[i].len = header.len; 
        //sched[i].caplen = header.caplen; 
	sched[i].packet_ptr = safe_malloc(sched[i].pkthdr.len);
	memcpy(sched[i].packet_ptr, packet, sched[i].pkthdr.len);  
        
	/* extract necessary data */
        etherhdr = (ether_hdr*)(sched[i].packet_ptr);
        iphdr = (ipv4_hdr *)(sched[i].packet_ptr + SIZE_ETHERNET);
        size_ip = iphdr->ip_hl << 2;
        if (size_ip < 20) {
		printf("ERROR: Invalid IP header length: %u bytes\n", size_ip);
                return 0;
        }
        tcphdr = (tcp_hdr *)(sched[i].packet_ptr + SIZE_ETHERNET + size_ip);
        size_tcp = tcphdr->th_off*4; 
        if (size_tcp < 20) {
            printf("ERROR: Invalid TCP header length: %u bytes\n", size_tcp);
            return 0;
	}
        /* payload = (u_char *)(sched[i].packet_ptr + SIZE_ETHERNET + size_ip + size_tcp); */
        size_payload = ntohs(iphdr->ip_len) - (size_ip + (size_tcp));

        
 	/* Source IP and Destination IP */
        sip = iphdr->ip_src;
        dip = iphdr->ip_dst;

        flags = tcphdr->th_flags;
		
        if (flags == TH_SYN){	/* set IPs who's local and who's remote based on the SYN flag */
            local_ip = sip;  
            remote_ip = dip;   
	}

        /*Compare IPs to see which packet is this comming from*/
        if(compip(&local_ip, &remote_ip, &sip)==LOCAL_IP_MATCH){
            local = true;
            remote = false;   
	}
        if(compip(&local_ip, &remote_ip, &sip)==REMOTE_IP_MATCH){
            local = false;
            remote = true;     
	}

        /* Setup rest of Schedule, parameter by parameter */
        /* Refer to header file for details on each of the parameters */
	
        sched[i].etherhdr = etherhdr; 
        sched[i].iphdr = iphdr; 
        sched[i].tcphdr = tcphdr; 
        sched[i].size_ip = size_ip; 
        sched[i].size_tcp = size_tcp;  
        sched[i].size_payload = size_payload;
        sched[i].sent_counter = 0; 

        /* Do the following only for the first packet (SYN)*/
        if(i==0){
            sched[i].length_last_ldata = 0;
            sched[i].length_curr_ldata = 0; 
            sched[i].length_last_rdata = 0;
            sched[i].length_curr_rdata = 0; 
            sched[i].local = true; 
            sched[i].remote = false;
            sched[i].curr_lseq = ntohl(sched[i].tcphdr->th_seq); 
            sched[i].curr_lack = 0;
            sched[i].exp_rseq = 0; 	/* Keep track of previous remote seq & ack #s*/
            sched[i].exp_rack = 0;
           
        } 

	/* Local Packet operations */
        else if(local){
            sched[i].length_last_ldata = sched[i-1].length_curr_ldata; 
            sched[i].length_curr_ldata = size_payload; 
            sched[i].length_last_rdata = sched[i-1].length_curr_rdata;
            sched[i].length_curr_rdata = 0; 
            sched[i].local = true; 
            sched[i].remote = false;
            sched[i].curr_lseq = ntohl(sched[i].tcphdr->th_seq); 
            sched[i].curr_lack = ntohl(sched[i].tcphdr->th_ack);
            sched[i].exp_rseq = sched[i-1].exp_rseq; 	/* Keep track of previous remote seq & ack #s*/
            sched[i].exp_rack = sched[i-1].exp_rack;
           
        }

        /* Remote Packet operations */
        else if(remote){
            sched[i].length_last_ldata = sched[i-1].length_curr_ldata;
            sched[i].length_curr_ldata = 0; 
            sched[i].length_last_rdata = sched[i-1].length_curr_rdata;
            sched[i].length_curr_rdata = size_payload; 
            sched[i].local = false; 
            sched[i].remote = true;
            sched[i].curr_lseq = sched[i-1].curr_lseq; 
            sched[i].curr_lack = sched[i-1].curr_lack;
            sched[i].exp_rseq = ntohl(sched[i].tcphdr->th_seq); 	/* Keep track of previous remote seq & ack #s*/
            sched[i].exp_rack = ntohl(sched[i].tcphdr->th_ack);
            /* Setup global variable where remote FIN-ACK exists*/
	    if(flags == (TH_FIN|TH_ACK)) finack_rindex = i; 
            //printf("REMOTE --------%d\n",i+1);
        }
        

        i++; /* increment schedule index */

    } /*end internal loop for reading packets (all in one file)*/

    pcap_close(local_handle);  /*close the pcap file*/

return pkt_counter; /* Return number of packets scheduled */
}


/**
 * This function returns a pcap_t for the live traffic handler which 
 * filters out traffic other than TCP
 *
 */

pcap_t* 
set_live_filter(char *dev, in_addr* hostip, unsigned int port)
{
    pcap_t *handle = NULL;			/* Session handle */
    char errbuf[PCAP_ERRBUF_SIZE];	/* Error string buffer */
    struct bpf_program fp;		/* The compiled filter */
    char filter_exp[50]; 
    sprintf(filter_exp,"tcp and dst host %d.%d.%d.%d and dst port %u",
        hostip->byte1, hostip->byte2, hostip->byte3, hostip->byte4, port); 	/* The filter expression */    
    bpf_u_int32 mask;		        /* Our network mask */
    bpf_u_int32 net;		 	/* Our IP */

    /* Define the device */
    if (dev == NULL) {
        fprintf(stderr, "Couldn't find default device: %s\n", errbuf);
        return handle;
    }
    /* Find the properties for the device */
    if (pcap_lookupnet(dev, &net, &mask, errbuf) == -1) {
        fprintf(stderr, "Couldn't get netmask for device %s: %s\n", dev, errbuf);
	net = 0;
	mask = 0;
    }

    /* Open the session in promiscuous mode */
    handle = pcap_open_live(dev, BUFSIZ_PLUS, PROMISC_OFF, TIMEOUT_ms, errbuf);
    if (handle == NULL) {
        fprintf(stderr, "Couldn't open device %s: %s\n", dev, errbuf);
        return handle;
    }
    /* Compile and apply the filter */
    if (pcap_compile(handle, &fp, filter_exp, 0, net) == -1) {
        fprintf(stderr, "Couldn't parse filter %s: %s\n", filter_exp, pcap_geterr(handle));
        return handle;
    }
    if (pcap_setfilter(handle, &fp) == -1) {
        fprintf(stderr, "Couldn't install filter %s: %s\n", filter_exp, pcap_geterr(handle));
        return handle;
    }

return handle;
}


/**
 * This function returns a pcap_t for the savedfile traffic handler which 
 * filters out traffic other than TCP
 *
 */
pcap_t* 
set_offline_filter(char* file)
{
    pcap_t *handle;			/* Session handle */
    char errbuf[PCAP_ERRBUF_SIZE];	/* Error string */
    struct bpf_program fp;		/* The compiled filter */
    char filter_exp[] = "tcp";
    bpf_u_int32 net=0;		 	/* Our IP */

    
    /* Open savedfile  */
    handle = pcap_open_offline(file, errbuf); 
    if (handle == NULL) {
        fprintf(stderr, "Couldn't open file %s\n", errbuf);
        return handle;
    }
    /* Compile and apply the filter */
    if (pcap_compile(handle, &fp, filter_exp, 0, net) == -1) {
        fprintf(stderr, "Couldn't parse filter %s: %s\n", filter_exp, pcap_geterr(handle));
        return handle;
    }
    if (pcap_setfilter(handle, &fp) == -1) {
        fprintf(stderr, "Couldn't install filter %s: %s\n", filter_exp, pcap_geterr(handle));
        return handle;
    }

return handle;
}


/**
 * This is the callback function for pcap_loop
 * This function is called every time we receive a remote packet
 */
void 
got_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet){

    ether_hdr *etherhdr = NULL; 
    tcp_hdr *tcphdr = NULL;
    ipv4_hdr *iphdr = NULL;

    unsigned int size_ip, size_tcp, size_payload;
    unsigned int flags = 0;

    /* This is to get rid of the warning */
    args = NULL;
    if(args == NULL) header = NULL;
    if(header == NULL) args = NULL; 

    /* Extract and examine recieved packet headers */
    etherhdr = (ether_hdr*)(packet);
    iphdr = (ipv4_hdr *)(packet + SIZE_ETHERNET);
    size_ip = iphdr->ip_hl << 2;
    if (size_ip < 20) {
	printf("ERROR: Invalid IP header length: %u bytes\n", size_ip);
        return;
    }
    tcphdr = (tcp_hdr *)(packet + SIZE_ETHERNET + size_ip);
    size_tcp = tcphdr->th_off*4;
    if (size_tcp < 20) {
        printf("ERROR: Invalid TCP header length: %u bytes\n", size_tcp);
        return;
    }
    size_payload = ntohs(iphdr->ip_len) - (size_ip + (size_tcp));    


    flags = tcphdr->th_flags;
    /* Check correct SYN-ACK expecation, if so then proceed in fixing entire schedule from relative to absolute SEQs+ACKs */
    if((flags == (TH_SYN|TH_ACK)) && (sched_index==1) && (tcphdr->th_ack==htonl(sched[sched_index-1].curr_lseq + 1))){
        unsigned int j;
        printf("Received Remote Packet...............	[%d]\n",sched_index+1);
        printf("Remote Pakcet Expectation met.\nProceeding in replay....\n");
        //printf("SYN-ACKed Random SEQ set!\n");
        initial_rseq = ntohl(tcphdr->th_seq); 
        //printf("initial_rseq: %u\n", initial_rseq);
        /* After we receiving the first SYN-ACK, then adjust the entire sched to be absolute rather than relative #s*/
        sched[1].exp_rseq = sched[1].exp_rseq + initial_rseq;
        for(j = 2; j<pkts_scheduled; j++){ /* Based on correctly recieving the random SEQ from the SYN-ACK packet, do the following:*/
            if(sched[j].local){ /* Set local ACKs for entire sched to be absolute #s*/
                sched[j].curr_lack = sched[j].curr_lack + initial_rseq; 
            }
            else if(sched[j].remote){ /* Set remote SEQs for entire sched to be absolute #s*/
                sched[j].exp_rseq = sched[j].exp_rseq + initial_rseq;
                
           }
        }
    sched_index++; /* Proceed in the schedule*/
    return; 
    }


    printf(">Received a Remote Packet\n");
    printf(">>Checking Expectations\n");
   

    /* Handle Remote Packet Loss */
    if(sched[sched_index].exp_rack > ntohl(tcphdr->th_ack)) { 
        //printf("Remote Packet Loss! Resending Lost packet\n");
        sched_index=acked_index; /* Reset the schedule index back to the last correctly ACKed packet */
        //printf("ACKED Index = %d\n", acked_index); 
        while(!sched[sched_index].local){
            sched_index++; 
        }
        return; 
    } 

    /* Handle Local Packet Loss <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<COME BACK TO THIS<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< */
    else if((sched[sched_index].exp_rseq  < ntohl(tcphdr->th_seq)) && sched[sched_index].remote){ 
        /* Resend immediate previous LOCAL packet */
        printf("Local Packet Loss! Resending Lost packet >> DupACK Issued!\n");
        sched_index=acked_index; /* Reset the schedule index back to the last correctly ACKed packet */
        /*sched[sched_index].sent_counter=0; Reset the re-transmission counter for this ACKed packet?*/
        //printf("ACKED Index = %d\n", acked_index); 
        while(!sched[sched_index].local){
            sched_index++; 
        }
        return; 
    } 
        

        

    /* No Packet Loss... Proceed Normally (if expectations are met!) */
    else if((tcphdr->th_seq==htonl(sched[sched_index].exp_rseq)) && 
	(tcphdr->th_ack==htonl(sched[sched_index].exp_rack))){
            printf("Received Remote Packet...............	[%d]\n",sched_index+1);
            /* Handles differing payload size and does not trigger on unnecessary ACK + window update issues*/
            if((sched[sched_index].size_payload!=size_payload) && (size_payload!=0)){
                printf("Payload size of received packet does not meet expectations\n");
                /* Resent last local packet, maybe remote host behaves this time*/
                different_payload=true; 
                /* Set global variable of where differing payload size is not meeting expectations*/
                diff_payload_index = sched_index; 

                /*Treat this as packet loss, and attempt resetting index to resend packets where*/
                /* packets were received matching expectation*/
                sched_index=acked_index; /* Reset the schedule index back to the last correctly ACKed packet */
                //printf("ACKED Index = %d\n", acked_index); 
                while(!sched[sched_index].local){
                    sched_index++; 
                }
                return; 
            }
            printf("Remote Packet Expectation met.\nProceeding in replay....\n");
            sched_index++;
            acked_index = sched_index; /*Keep track correctly ACKed packet index*/
    } 

    /* Global variable to keep tack of last recieved packet info */
    packet_keeper_rprev = packet;  
    etherhdr_rprev = etherhdr; 
    tcphdr_rprev = tcphdr;
    iphdr_rprev = iphdr; 
    size_payload_prev = size_payload;

 
return; 
}



/**
 * This function prints out the ip address, given an in_addr struct pointer
 *
 */
void 
printip(in_addr* IP)
{

    printf("[%d.%d.%d.%d]\n", IP->byte1, IP->byte2, IP->byte3, IP->byte4);  

return; 
}

/**
 * This function prints out the mac address, given a mac_addr struct pointer
 *
 */
void 
printmac(struct mac_addr* MAC)
{

    printf("[%x:%x:%x:%x:%x:%x]\n", MAC->byte1, MAC->byte2, MAC->byte3, MAC->byte4, MAC->byte5, MAC->byte6);  

return; 
}


/**
 * This function compares two IPs, 
 * returns 1 if match with local ip
 * returns 2 if matches with remote ip
 * returns 0 if no match
 *
 */
int 
compip(in_addr* lip, in_addr* rip, in_addr* pkgip)
{

    if((lip->byte1==pkgip->byte1)&&(lip->byte2==pkgip->byte2)&&(lip->byte3==pkgip->byte3)&&(lip->byte4==pkgip->byte4))
	return LOCAL_IP_MATCH; 
    
     else if((rip->byte1==pkgip->byte1)&&(rip->byte2==pkgip->byte2)&&(rip->byte3==pkgip->byte3)&&(rip->byte4==pkgip->byte4))
	return REMOTE_IP_MATCH; 
    
    else return NO_MATCH; 
}


/**
 * This function sets the IP and MAC of a given interface (i.e. eth0)
 * into in_addr & mac_addr struct pointers
 *
 */
void iface_addrs(char* iface, in_addr* ip, struct mac_addr* mac) 
{
    int s;
    struct ifreq buffer;
    s = socket(PF_INET, SOCK_DGRAM, 0);
    memset(&buffer, 0x00, sizeof(buffer));
    strcpy(buffer.ifr_name, iface);
    
    ioctl(s, SIOCGIFADDR, &buffer);
    struct in_addr localip = ((struct sockaddr_in *)&buffer.ifr_addr)->sin_addr;
    ip->byte4 = (localip.s_addr)>>24;
    ip->byte3 = ((localip.s_addr)>>16)&255;
    ip->byte2 = ((localip.s_addr)>>8)&255;
    ip->byte1 = (localip.s_addr)&255;
    
    ioctl(s, SIOCGIFHWADDR, &buffer);
    mac->byte1 = buffer.ifr_hwaddr.sa_data[0];
    mac->byte2 = buffer.ifr_hwaddr.sa_data[1];
    mac->byte3 = buffer.ifr_hwaddr.sa_data[2];
    mac->byte4 = buffer.ifr_hwaddr.sa_data[3];
    mac->byte5 = buffer.ifr_hwaddr.sa_data[4];
    mac->byte6 = buffer.ifr_hwaddr.sa_data[5];

return;
}


/**
 * This function rewrites the IPs and MACs of a given packet, 
 * creates a newfile.pcap. It returns the number of packets of the newfile. 
 * This function only starts rewriting the newfile once it sees the first
 * SYN packet. This is so that the first packet in the newfile is always 
 * the first packet to be sent. 
 */
int
rewrite(in_addr* new_remoteip, struct mac_addr* new_remotemac, in_addr* myip, struct mac_addr* mymac, char* file, unsigned int new_src_port)
{

    ether_hdr* etherhdr; 
    ipv4_hdr *iphdr;
    tcp_hdr *tcphdr;
    in_addr local_ip; 
    in_addr remote_ip; 
    unsigned int size_ip;
    unsigned int size_tcp; 
    char* newfile = "newfile.pcap";
    char ErrBuff [1024];
    int pkt_counter, len; 
    const u_char *packet;
    struct pcap_pkthdr *header;
    in_addr sip;  /* Source IP */ 
    unsigned int flags; 
    int local_packets = 0; 
    bool initstep1 = false;  /* keep track of successful handshake step */

    local_ip.byte1=0; 
    local_ip.byte2=0; 
    local_ip.byte3=0; 
    local_ip.byte4=0; 

    remote_ip.byte1=0; 
    remote_ip.byte2=0; 
    remote_ip.byte3=0; 
    remote_ip.byte4=0;  


    /*Read the header of the PCAP*/
    int fp_for_header = open(file,O_RDONLY);
    if(fp_for_header < 0){
        fprintf(stderr, "Cannot open PCAP file to get file header.\n");
        return PCAP_OPEN_ERROR;
    }
 
    u_int8_t data[sizeof(struct pcap_file_header)];
 
    len = read(fp_for_header, data, sizeof(struct pcap_file_header));
    if(len == 0){
        fprintf(stderr, "Could not read from file.\n");
    }	
    close(fp_for_header);

    /*Open file for reading each packet*/
    int fp = open(newfile,O_CREAT|O_WRONLY,S_IRWXU);
    if(fp < 0){
        fprintf(stderr, "Cannot open file: %s for writing.\n",newfile);
        return PCAP_OPEN_ERROR;
    }		
 
    /* Write the header to new file */
    len = write(fp, data,sizeof(struct pcap_file_header));
 
    
    pcap_t *pcap = set_offline_filter(file); 

    if (!pcap){
        fprintf (stderr, "Cannot open PCAP file '%s'\n", file);
                fprintf(stderr, "%s\n",ErrBuff);
                return PCAP_OPEN_ERROR;
    }
 
    /*Modify each packet's IP & MAC based on the passed args then do a checksum of each packet*/
    for (pkt_counter = 0; pcap_next_ex(pcap, &header, &packet) > 0; pkt_counter++){

        etherhdr = (ether_hdr*)(packet);
        iphdr = (ipv4_hdr *)(packet + SIZE_ETHERNET);
        size_ip = iphdr->ip_hl << 2; 
        if (size_ip < 20) {
	    printf("ERROR: Invalid IP header length: %u bytes\n", size_ip);
        return ERROR;
        }
        tcphdr = (tcp_hdr *)(packet + SIZE_ETHERNET + size_ip);
        size_tcp = tcphdr->th_off*4;
        if (size_tcp < 20) {
            printf("ERROR: Invalid TCP header length: %u bytes\n", size_tcp);
        return ERROR;
	}
        /* payload = (u_char *)(packet + SIZE_ETHERNET + size_ip + size_tcp); */


        sip = iphdr->ip_src;

        flags = tcphdr->th_flags;


	/* set IPs who's local and who's remote based on the SYN flag */
	if(flags == TH_SYN){
            local_ip = iphdr->ip_src; 
            remote_ip = iphdr->ip_dst; 
            initstep1 = true; /* This flag is set to signify the first encounter of the SYN within the pacp*/ 						
	}

	if(compip(&local_ip, &remote_ip, &sip)==LOCAL_IP_MATCH){
                /* Set the source MAC */
		etherhdr->ether_shost[0] = mymac->byte1;	
		etherhdr->ether_shost[1] = mymac->byte2;
		etherhdr->ether_shost[2] = mymac->byte3;
		etherhdr->ether_shost[3] = mymac->byte4;
		etherhdr->ether_shost[4] = mymac->byte5;
		etherhdr->ether_shost[5] = mymac->byte6;
                
                /* Set the source IP */
		iphdr->ip_src = *myip; 
		/* Set the destination IP */
		iphdr->ip_dst = *new_remoteip; 

                /* Set the destination MAC */
		etherhdr->ether_dhost[0] = new_remotemac->byte1;	
		etherhdr->ether_dhost[1] = new_remotemac->byte2;
		etherhdr->ether_dhost[2] = new_remotemac->byte3;
		etherhdr->ether_dhost[3] = new_remotemac->byte4;
		etherhdr->ether_dhost[4] = new_remotemac->byte5;
		etherhdr->ether_dhost[5] = new_remotemac->byte6;


                /* This is to change the source port, whether it is specified as random or as a port # by the user */
		tcphdr->th_sport = htons(new_src_port);
	}
	else if(compip(&local_ip, &remote_ip, &sip)==REMOTE_IP_MATCH){

                /* Set the destination MAC */
                etherhdr->ether_dhost[0] = mymac->byte1;	
		etherhdr->ether_dhost[1] = mymac->byte2;
		etherhdr->ether_dhost[2] = mymac->byte3;
		etherhdr->ether_dhost[3] = mymac->byte4;
		etherhdr->ether_dhost[4] = mymac->byte5;
		etherhdr->ether_dhost[5] = mymac->byte6;
                /* Set the destination IP */
		iphdr->ip_dst = *myip; 
                /* Set the source IP */
		iphdr->ip_src = *new_remoteip; 
                /* Set the source MAC */ 
		etherhdr->ether_shost[0] = new_remotemac->byte1;	
		etherhdr->ether_shost[1] = new_remotemac->byte2;
		etherhdr->ether_shost[2] = new_remotemac->byte3;
		etherhdr->ether_shost[3] = new_remotemac->byte4;
		etherhdr->ether_shost[4] = new_remotemac->byte5;
		etherhdr->ether_shost[5] = new_remotemac->byte6; 

                /* This is to change the source port, whether it is specified as random or as a port # by the user */
                tcphdr->th_dport = htons(new_src_port);
	}
 
        /*Calculate & fix checksum for newly edited-packet*/
        fix_all_checksum_liveplay(iphdr);

        if(initstep1){ /*only start rewriting new pcap with SYN packets on wards*/
            local_packets ++; 
            len = write(fp,header,16);
            if(len == 0){
                fprintf(stderr, "Error occurred writing pcap_header.\n");
                pcap_close(pcap);
                close(fp);
                return REWRITE_ERROR;
            }
 
            len = write(fp,packet,header->caplen);
            if(len == 0){
                fprintf(stderr, "Error occurred writing pcap data.\n");
                pcap_close(pcap);
                close(fp);
                return REWRITE_ERROR;
            }	
        }
        
  

    } /* end of while loop */
 
    pcap_close (pcap);
    close(fp);
    return local_packets;
}

/**
 * This function extracts the MAC address (from command line format 
 * and sets the mac_addr struct)
 *
 */
int
extmac(char* new_rmac_ptr, struct mac_addr* new_remotemac){
    int i;

    u_int8_t new_rmac[6]; 
    for (i = 0; i < 6; i++){
        long b = strtol(new_rmac_ptr+(3*i), (char **) NULL, 16);
        new_rmac[i] = (char)b;
    }

    new_remotemac->byte1 = new_rmac[0];
    new_remotemac->byte2 = new_rmac[1];
    new_remotemac->byte3 = new_rmac[2];
    new_remotemac->byte4 = new_rmac[3];
    new_remotemac->byte5 = new_rmac[4];
    new_remotemac->byte6 = new_rmac[5]; 

return SUCCESS; 

}

/**
 * This function extracts the IP address (from command line format 
 * and sets the in_addr struct)
 *
 */
int
extip(char *ip_string, in_addr* new_remoteip){

    unsigned char ip[4] = {0};
    size_t i = 0;

    while (*ip_string) {
        if (isdigit((unsigned char)*ip_string)) {
            ip[i] *= 10;
            ip[i] += *ip_string - '0';
        } else {
            i++;
        }
        ip_string++;
    }

    new_remoteip->byte1 = ip[0];
    new_remoteip->byte2 = ip[1];
    new_remoteip->byte3 = ip[2];
    new_remoteip->byte4 = ip[3];

return SUCCESS;
}


/**
 * This function calls all the checksum function given the IP Header
 * and edits the checksums fixing them appropriately 
 *
 */
int
fix_all_checksum_liveplay(ipv4_hdr *iphdr){

    int ret1 = 0, ret2 = 0; 

    /*Calculate TCP Checksum*/
    ret2 = do_checksum_liveplay((u_char *) iphdr,iphdr->ip_p, ntohs(iphdr->ip_len) - (iphdr->ip_hl << 2));
    if(ret2==-1){
        printf("*******An Error Occured calculating TCP Checksum*******\n");
    return -1; 
    }

    /*Calculate IP Checksum*/
    ret1 = do_checksum_liveplay((u_char *) iphdr, IPPROTO_IP, ntohs(iphdr->ip_len));
    if(ret1==-1){
        printf("*******An Error Occured calculating IP Checksum*******\n");
    return -1; 
    }

 
return 0; 
}

/************************************************************************************/

/*[copied from Aaron Turnor's checksum.c, but ommitting tcpedit_t structs] */
/*[The following functions have been slightly modified to be integrated with tcpliveplay code structure] */

/** 
 * This code re-calcs the IP and Layer 4 checksums
 * the IMPORTANT THING is that the Layer 4 header 
 * is contiguious in memory after *ip_hdr we're actually
 * writing to the layer 4 header via the ip_hdr ptr.
 * (Yes, this sucks, but that's the way libnet works, and
 * I was too lazy to re-invent the wheel.
 * Returns 0 on sucess, -1 on error
 */


/**
 * Returns -1 on error and 0 on success, 1 on warn
 */
int
do_checksum_liveplay(u_int8_t *data, int proto, int len) {
    ipv4_hdr *ipv4;
    tcp_hdr *tcp;
    int ip_hl;
    int sum;
    
    sum = 0;
    ipv4 = NULL;
        
    ipv4 = (ipv4_hdr *)data;
    ip_hl = ipv4->ip_hl << 2;

    switch (proto) {
        case IPPROTO_TCP:
            tcp = (tcp_hdr *)(data + ip_hl);
#ifdef STUPID_SOLARIS_CHECKSUM_BUG
            tcp->th_sum = tcp->th_off << 2;
            return (TCPEDIT_OK);
#endif
            tcp->th_sum = 0;
            
            /* Note, we do both src & dst IP's at the same time, that's why the
             * length is 2x a single IP
             */

            sum = do_checksum_math_liveplay((u_int16_t *)&ipv4->ip_src, 8);
            
            sum += ntohs(IPPROTO_TCP + len);
            sum += do_checksum_math_liveplay((u_int16_t *)tcp, len);
            tcp->th_sum = CHECKSUM_CARRY(sum);
            break;      
    
        case IPPROTO_IP:
            ipv4->ip_sum = 0;
            sum = do_checksum_math_liveplay((u_int16_t *)data, ip_hl);
            ipv4->ip_sum = CHECKSUM_CARRY(sum);
            break;
       

        default:
            printf("Unsupported protocol for checksum:\n");
            return TCPEDIT_WARN;
    }
    
    return TCPEDIT_OK;
}

/**
 * code to do a ones-compliment checksum
 */
int
do_checksum_math_liveplay(u_int16_t *data, int len)
{
    int sum = 0;
    union {
        u_int16_t s;
        u_int8_t b[2];
    } pad;
    
    while (len > 1) {
        sum += *data++;
        len -= 2;
    }
    
    if (len == 1) {
        pad.b[0] = *(u_int8_t *)data;
        pad.b[1] = 0;
        sum += pad.s;
    }
    
    return (sum);
}


