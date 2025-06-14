/* tftp defines */

#ifndef SLIRP_TFTP_H
#define SLIRP_TFTP_H

#define TFTP_SESSIONS_MAX 20

#define TFTP_SERVER	69

#define TFTP_RRQ    1
#define TFTP_WRQ    2
#define TFTP_DATA   3
#define TFTP_ACK    4
#define TFTP_ERROR  5
#define TFTP_OACK   6

#define TFTP_FILENAME_MAX 512

struct tftphdr {
  struct udphdr udp;
  uint16_t tp_op;
};

struct tftp_t {
  struct tftphdr hdr;
  union {
    struct {
      uint16_t tp_block_nr;
      uint8_t tp_buf[512];
    } tp_data;
    struct {
      uint16_t tp_error_code;
      uint8_t tp_msg[512];
    } tp_error;
    char tp_buf[512 + 2];
  } x;
} __attribute__((packed));

struct tftp_session {
    Slirp *slirp;
    char *filename;
    int fd;

    struct sockaddr_storage client_addr;
    uint16_t client_port;
    uint32_t block_nr;

    int timestamp;
};

void tftp_input(struct sockaddr_storage *srcsas, struct mbuf *m);

#endif
