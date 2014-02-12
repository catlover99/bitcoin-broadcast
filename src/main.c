/* -*- mode: c; c-file-style: "linux" -*-
 *  vi: set shiftwidth=8 tabstop=8 noexpandtab:
 */

#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h> 
#include <err.h>
#include <stdbool.h>
#include <openssl/sha.h>

uint8_t join_message[] = {
	0xF9, 0xBE, 0xB4, 0xD9, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6F,
	0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
	0x35, 0x8D, 0x49, 0x32, 0x62, 0xEA, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0xB2, 0xD0, 0x50,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x2E, 0xB3, 0x5D,
	0x8C, 0xE6, 0x17, 0x65, 0x0F, 0x2F, 0x53, 0x61, 0x74, 0x6F,
	0x73, 0x68, 0x69, 0x3A, 0x30, 0x2E, 0x37, 0x2E, 0x32, 0x2F,
	0xC0, 0x3E, 0x03, 0x00
};

const int join_message_len = sizeof(join_message);

struct __attribute__ ((__packed__)) msg {
	uint32_t magic;
	char command[12];
	uint32_t length;
	uint32_t checksum;
	uint8_t payload[];
};

struct __attribute__ ((__packed__)) msg_inv_vect {
	uint32_t type;
	unsigned char hash[32];
};

// Prototypes
uint64_t var_int(uint8_t *buf);
int var_int_len(uint8_t *buf);
uint32_t checksum(struct msg *m);
unsigned char *dhash(const unsigned char *d, unsigned long n);
char *hex256(const unsigned char *buf);
void log_msg(struct msg *m);

int main(int argc, char *argv[])
{
	int sockfd;
	struct sockaddr_in serv_addr;
	int buf_len = sizeof(struct msg);
	struct msg *buf = malloc(buf_len);
	if (buf == NULL) {
		errx(5,"Memory allocation failed");
	}

	if(argc != 2) {
		errx(1,"Usage: %s <ip of server>",argv[0]);
	} 

	if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		err(2,"Could not create socket");
	}

	memset(&serv_addr, '0', sizeof(serv_addr)); 

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(8333); 

	if(inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
		errx(1,"IP address conversion failed");
	} 

	if( connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		err(2,"Connect Failed");
	} 

	// Making FILE from socket fd
	FILE *bitcoind = fdopen(sockfd,"r+");
	if (bitcoind == NULL) err(2,"Buffered reader has failed");

	// Send greeting
	if (fwrite(join_message,join_message_len,1,bitcoind) != 1) {
		errx(2,"Sending of welcome message has failed");
	}

	printf("Connected.\n");

	// Message receiver loop. Processes messages having this format:
	// https://en.bitcoin.it/wiki/Protocol_specification#Message_structure
	while (true) {

		// Take header
		if (fread(buf,sizeof(struct msg),1,bitcoind) != 1) {
			errx(3,"Receiving data from bitcoind has failed");
		}

		// FIXME One should swap byte ordering on big-endian machines!

		// Scaling the buffer
		buf_len = sizeof(struct msg)+buf->length;
		buf = realloc(buf, buf_len);
		if (buf == NULL) {
			errx(5,"Memory allocation failed");
		}

		if (buf->length != 0 && fread(buf->payload,buf->length,1,bitcoind) != 1) {
			errx(3,"Receiving data from bitcoind has failed");
		}

		if (checksum(buf) != buf->checksum) {
			errx(3,"Checksum error. Probably we got out of sync.");
		}

		// Store to flat files
		log_msg(buf);

		// Message valid, parsing it
		if (strcmp(buf->command,"inv") == 0) {
			// Payload in getdata request is identical to
			// inv and because the command name is shorter
			// in inv, we may just alter the inv to
			// getdata and send the payload back. The
			// checksum is not affected because it is
			// calculated from payload only.
			strcpy(buf->command,"getdata");

			if (fwrite(buf,sizeof(struct msg)+buf->length,1,bitcoind) != 1) {
			 	errx(2,"Sending of getdata has failed");
			}
		}

		// Make sure debugging strings are printed
		fflush(stdout);
	}

	// Never reached
}

uint64_t var_int(uint8_t *buf)
{
	if (*buf == 0xfd) return *(uint16_t*)(buf+1);
	if (*buf == 0xfe) return *(uint32_t*)(buf+1);
	if (*buf == 0xff) return *(uint64_t*)(buf+1);
	return *buf;
}

int var_int_len(uint8_t *buf)
{
	if (*buf == 0xfd) return 3;
	if (*buf == 0xfe) return 5;
	if (*buf == 0xff) return 9;
	return 1;
}

uint32_t checksum(struct msg *m)
{
	return *(uint32_t*)dhash(m->payload,m->length);
}

unsigned char *dhash(const unsigned char *d, unsigned long n)
{
	return SHA256(SHA256(d,n,NULL),32,NULL);
}

char *hex256(const unsigned char *in)
{
	static char out[65];

	for (int i=0; i<32; i++) {
		snprintf(out+2*i,3,"%02hhx",in[31-i]);
	}

	return out;
}

void log_msg(struct msg *m)
{
	int hash_end = m->length;

	if (strcmp(m->command,"inv") == 0) {
		// Inventory messages may be dropped from logs
		return;
	} else if (strcmp(m->command,"block") == 0) {
		// Block hash is calculated only from 6 first fields
		hash_end = 4+32+32+4+4+4;
	}
	
	// Prepare path name.
	char pathname[4+11+1];
	snprintf(pathname,sizeof(pathname),"log/%s",m->command);

	// Create directory. If already exists, ignore error.
	int ret = mkdir(pathname,0777);
	if (ret != 0 && errno != EEXIST) err(5,"Unable to create directory %s",pathname);

	// Prepare file name
	char filename[4+11+1+64+1];
	snprintf(filename,sizeof(filename),
		 "log/%s/%s",m->command,hex256(dhash(m->payload,hash_end)));

	// Open file
	FILE *f = fopen(filename,"wb");
	if (f == NULL) err(5,"Unable to open file for writing %s",filename);
	
	// Store data to file
	if (m->length > 0 && fwrite(m->payload,m->length,1,f) != 1) {
		errx(5,"Storing message to log file has failed");
	}

	// Closing log file
	if (fclose(f) != 0) err(5,"Closing log file has failed");

	// Report what's done
	printf("Storing %s\n",filename);
}