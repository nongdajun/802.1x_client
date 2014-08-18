﻿#include "house_network.h"
#include <sys/types.h>
#include "md5.h"
#include <sys/socket.h>
#include <netpacket/packet.h> 
#include <sys/ioctl.h>

int crt_sock(struct ifreq * ifr)
{
	int s;
	int err;
	s = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL)); 
	
	/* 
		assert the ifr->ifr_ifrn.ifrn_name was known before
		interface_name was set in get_from_file(), and saved in /etc/8021.config file
	*/
	memset(ifr, 0, sizeof(struct ifreq));
	strncpy(ifr->ifr_ifrn.ifrn_name, interface_name, sizeof(ifr->ifr_ifrn.ifrn_name)); // interface_name: global value, in public.h
	
	/* get hardware address */
	err = ioctl(s, SIOCGIFHWADDR, ifr);
	if( err < 0)
	{
		perror("ioctl get hw_addr error");
		close(s);
		return -1;
	}

	// refer to: http://blog.chinaunix.net/uid-8048969-id-3417143.html
	err = ioctl(s, SIOCGIFFLAGS, ifr);
	if( err < 0)
	{
		perror("ioctl get if_flag error");
		close(s);
		return -1;
	}


	// check the if's status 
	if(ifr->ifr_ifru.ifru_flags & IFF_RUNNING )
	{
		printf("eth link up\n");
	}
	else
	{
		printf("eth link down, please check the eth is ok\n");
		return -1;
	}

	ifr->ifr_ifru.ifru_flags |= IFF_PROMISC;
	err = ioctl(s, SIOCSIFFLAGS, ifr);
	if( err < 0)
	{
		perror("ioctl set if_flag error");
		close(s);
		return -1;
	}
	
	return s;

}


// the dial route all uses the same fixed eth_header and the same sock
int  create_ethhdr_sock(struct ethhdr * eth_header)
{
	/* mac broadcast address, huawei's exchange */
	const char dev_dest[ETH_ALEN] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x03};

	/* acquire interface's id and hardaddress based in struct ifreq and mysock*/
	struct ifreq *myifr;
	myifr = (struct ifreq *) malloc( sizeof(struct ifreq) );
	if( NULL == myifr )
	{
		perror("Malloc for ifreq struct failed");
		exit(-1);
	}

	int mysock;
	mysock = crt_sock(myifr);
	if(-1 == mysock)
	{
		perror("Create socket failed");
		exit(-1);
	}

	/* create  eth header 
	 #define ETH_HLEN 14 */
	memcpy(eth_header->h_dest, dev_dest, ETH_ALEN);
	memcpy(eth_header->h_source, myifr->ifr_ifru.ifru_hwaddr.sa_data, ETH_ALEN);
	eth_header->h_proto = htons(ETH_P_PAE); // ETH_P_PAE = 0x888e
	
	free(myifr); 
	return mysock;
}

void init_dial_env()
{
	/* linklayer broadcast address, used to connect the huawei's exchange */
	const char dev_dest[ETH_ALEN] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x03};

	
	/* set struct sockaddr_ll for sendto function
	 sa_ll: global value, in "house_network.h" */
	sa_ll.sll_family = PF_PACKET;
	sa_ll.sll_protocol = htons(ETH_P_ALL);
	sa_ll.sll_ifindex = if_nametoindex(interface_name);   
	sa_ll.sll_hatype = 0;
	sa_ll.sll_pkttype = PACKET_HOST | PACKET_BROADCAST  | PACKET_MULTICAST;
	memcpy(sa_ll.sll_addr, dev_dest, ETH_ALEN);

	sock =  create_ethhdr_sock(&eth_header); // eth_header,sock: global value

}

void send_pkt(int mysock, uint8_t * send_buf, size_t size)
{
	if( -1 == sendto(mysock, send_buf, size, 0, (struct sockaddr *)&sa_ll,  sizeof(sa_ll)))
	{
		perror("sendto failed");
		exit(-5);
	}	
}

/* used to encpass the passwd */
int mk_response_md5( authhdr *request_eap, uint8_t *md)
{
    uint8_t data[256] = {0}, len = 0, request_md5_len;
    uint8_t *request_md5 = request_eap->ext_data.data.md5_data.data;
    md5_state_t md5_msg;

    md5_init(&md5_msg);

    request_md5_len = request_eap->ext_data.data.md5_data.size;
    // message = id + passwd + request_msg
    data[0] = request_eap->ext_data.id;
    len += 1;
    memcpy(data + len, passwd, strlen(passwd));
    len += strlen(passwd);
    memcpy(data + len, request_md5, request_md5_len);
    len += request_md5_len;

    md5_append(&md5_msg, data, len);
    md5_finish(&md5_msg, md);

    return 16;

}

/* cmd: ctl different pkt */
/* If cmd is "start" or "logoff", recv_buf = NULL, otherwise recv_buf contains respn pkt */
size_t mk_pkt(uint8_t * send_buf, int cmd, uint8_t * recv_buf, struct ethhdr * eth_header)
{
	assert(send_buf);

	int16_t len  = 0; 
	int16_t pkt_size;
	int16_t ret;
    	uint8_t md[32] = {0};
	
	authhdr auth_pkt;
	authhdr *cisco_auth = NULL;
	memset(&auth_pkt, 0, sizeof(authhdr));
	auth_pkt.version  = AUTH_VERSION;

#ifdef DEBUG
printf("\nhere0\n");
printf("%x\n", recv_buf);
printf("\nhere0\n");
#endif
	
	if( recv_buf ) 
		cisco_auth = (authhdr *)(recv_buf + sizeof(struct ethhdr));

#ifdef DEBUG
printf("\n");
printf("here\n");
printf("%x\n", cisco_auth);
if( cisco_auth )
	printf("mk_pkt cisco_auth->ext_data.id: %d\n", cisco_auth->ext_data.id);
printf("here\n");
#endif


	//  copy the eth_header to send_buf 
	memcpy(send_buf, eth_header, sizeof(struct ethhdr));
    	pkt_size = sizeof(struct ethhdr);
	

	// make the auth header and eap header
	switch(cmd)
	{
		case START:
			auth_pkt.auth_type = AUTH_TYPE_EAPOL;
			break;
		case RESPONSE_ID:
			auth_pkt.auth_type = AUTH_TYPE_EAP;
			auth_pkt.ext_data.code = EAP_CODE_RESPONSE;	
			auth_pkt.ext_data.id = cisco_auth->ext_data.id;
			auth_pkt.ext_data.eap_rspn_type = EAP_EXT_IDENTIFIER;
			memcpy(auth_pkt.ext_data.data.id_data, user_id, strlen(user_id)); // user_id defines in "public.h", global value
			len = strlen((char *) auth_pkt.ext_data.data.id_data); // cal the length of auth_pkt
			len += (int16_t ) &((struct _ext_data *)0)->data; 
			auth_pkt.ext_data.len = htons(len);  
			auth_pkt.auth_len = htons(len);
			break;
		case RESPONSE_MD5:
			auth_pkt.auth_type = AUTH_TYPE_EAP;
			auth_pkt.ext_data.code = EAP_CODE_RESPONSE;	
			auth_pkt.ext_data.id = cisco_auth->ext_data.id;
			auth_pkt.ext_data.eap_rspn_type = EAP_EXT_MD5_CHALLENGE;
			ret = mk_response_md5(cisco_auth, md);
			if( 16 == ret )
				memcpy(auth_pkt.ext_data.data.md5_data.data, md, ret);
			else 
				printf("Compute md5 error!\n");
			auth_pkt.ext_data.data.md5_data.size = ret;
			len += (int16_t) &((struct _ext_data *)0)->data.md5_data.data;
			len += ret;
			auth_pkt.ext_data.len = htons(len);
			auth_pkt.auth_len = htons(len);
			break;
		case LOGOFF:
			auth_pkt.auth_type = AUTH_TYPE_LOGOFF;
			break;
        	case HEARTBEAT:
            		memcpy(&auth_pkt.ext_data, &cisco_auth->ext_data,
               			     cisco_auth->auth_len);
         	        auth_pkt.auth_type = cisco_auth->auth_type;
            		auth_pkt.auth_len = cisco_auth->auth_len;
            		break;
       		 default : break;
	}
			

	len += (int16_t)&((authhdr*)0)->ext_data;  
	memcpy(send_buf + pkt_size, &auth_pkt, len);
	pkt_size += len;

	return pkt_size;
} 

void logon()
{
	printf("Now log on\n");
	int pkt_size;
	
	/* create mac_packet which will contain mac header and eap_pkt */ 
	uint8_t * logon_pkt;
	logon_pkt = (uint8_t *) malloc( sizeof(uint8_t) * ETH_FRAME_LEN);
	if( NULL == logon_pkt )
	{
		perror("Malloc for logon_pkt failed");
		exit(-1);
	}

	
#ifdef DEBUG
printf("\n");
printf("dest: \t");print_mac(eth_header.h_dest);
printf("src: \t");print_mac(eth_header.h_source);
printf("\n");
#endif
	/* make the eapol_start pkt, contains mac&eap packet. */
	pkt_size = mk_pkt(logon_pkt, START, NULL, &eth_header);


	send_pkt(sock, logon_pkt, pkt_size);
	free(logon_pkt);
	log_flag = ON;
}

void logoff()
{
	printf("Now log off\n");
	int pkt_size;
	
	uint8_t * logoff_pkt;
	logoff_pkt = (uint8_t *) malloc( sizeof(uint8_t) * ETH_FRAME_LEN);
	if( NULL == logoff_pkt)
	{
		perror("Malloc for logoff_pkt failed");
		exit(-1);
	}
	
	/* make the eapol_logoff pkt, contains mac&eap packet. */
	pkt_size = mk_pkt(logoff_pkt, LOGOFF, NULL, &eth_header);


	send_pkt(sock, logoff_pkt, pkt_size);
	free(logoff_pkt);
	log_flag = OFF; 
	
}

void parse_pkt(uint8_t * recv_buf, struct ethhdr * local_ethhdr, int rspn_sock)
{
	struct ethhdr *recv_hdr;

	recv_hdr = (struct ethhdr *) recv_buf;

	authhdr * rspd_auth;


	static int pkt_size;
	
	/* create mac_packet which will contain mac header and eap_pkt */ 
	uint8_t * rspn_pkt;
	rspn_pkt = (uint8_t *) malloc( sizeof(uint8_t) * ETH_FRAME_LEN);
	if( NULL == rspn_pkt )
	{
		perror("Malloc for logon_pkt failed");
		exit(-1);
	}

#ifdef DEBUG
printf("\nrecv_hdr->h_source: \t");print_mac(recv_hdr->h_source);
printf("eth_ethhdr->h_src: \t");print_mac(eth_header.h_source);
printf("\n");
#endif

	// check recv  pkt
	if(  ( ON == log_flag ) && (0 != memcmp(recv_hdr->h_source, eth_header.h_source, ETH_ALEN))
			&&  (0 == memcmp(recv_hdr->h_dest, local_ethhdr->h_dest, ETH_ALEN))
			&& (htons(ETH_P_PAE) ==  recv_hdr->h_proto) )
	{
			rspd_auth = (authhdr *) (recv_buf + sizeof(struct ethhdr)); 

#ifdef DEBUG
printf("\n");
printf("Had recv pkt, then PARSE_PKT    .................................\n");
printf("recv_hdr->h_dest: \t");print_mac(recv_hdr->h_dest);
printf("local_ethhdr->h_dest: \t");print_mac(local_ethhdr->h_dest);
printf("eap code: \t%d\n", rspd_auth->ext_data.code);
#endif

			// check the eap_header and process in different ways
			switch (rspd_auth->ext_data.code)
			{
			case EAP_CODE_SUCCESS:
	
				status = ONLINE;
				printf("logon success!\n");
				return ;
			
			case EAP_CODE_FAILURE:
				status = OFFLINE;
				printf("logon failed!\n");
				return ;

			case EAP_CODE_REQUEST:
				// mk_pkt to respond to the service
				if (rspd_auth->ext_data.eap_rspn_type == EAP_EXT_IDENTIFIER) 
				{
					if(status != ONLINE)
					{
						pkt_size = mk_pkt(rspn_pkt, RESPONSE_ID, recv_buf, local_ethhdr);
						printf("Request stage!\n");
					}
               			 }
				 else if (rspd_auth->ext_data.eap_rspn_type == EAP_EXT_MD5_CHALLENGE) 
				 {
					if(status != ONLINE)
					{
						pkt_size = mk_pkt(rspn_pkt, RESPONSE_MD5, recv_buf, local_ethhdr);
						printf("Passing password stage!\n");
					}
                		 }
               			 break;
				
			default:
				break;
			}

			send_pkt(rspn_sock, rspn_pkt, pkt_size );
			free(rspn_pkt);

	}


}


void recv_eap_pkt(const int sock_arg, struct sockaddr_ll * sa_ll_arg, struct ethhdr * local_ethhdr )
{
	uint8_t *recv_buf  = (uint8_t *)malloc( sizeof(uint8_t) * ETH_FRAME_LEN);
	if( NULL == recv_buf)
	{
		perror("Malloc for recv_buf failed");
		exit(-1);
	}
	static int irecv = -1;

	struct sockaddr sa_ll_recv;
 	static socklen_t len;
	

	for(;;)
	{
		irecv  = recvfrom(sock_arg, recv_buf, sizeof(uint8_t) * ETH_FRAME_LEN, 0,  &sa_ll_recv, &len);
		if( -1 == irecv)
		{
			if( EINTR == errno)
			{
				break;
			}
			else
			{
				perror("recv eap pkt failed");
				exit(-1);
				continue;
			}
		}
#ifdef DEBUG
printf("\nHad recv______________\n");
#endif
		parse_pkt( recv_buf, local_ethhdr, sock_arg );
	}
}
	

	
	
	
	
