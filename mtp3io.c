/* mtp3d.c - mtp2/mtp3 daemon
 *
 * Copyright (C) 2006-2011 Netfors ApS.
 *
 * Author: Anders Baekgaard <ab@netfors.com>
 *
 * This file is part of chan_ss7.
 *
 * chan_ss7 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * chan_ss7 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with chan_ss7; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <linux/tcp.h>

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "aststubs.h"
#include "config.h"
#include "mtp.h"
#include "transport.h"
#include "utils.h"
#include "mtp3io.h"

#undef inet_ntoa

int mtp3_sockettype = MTP3_SOCKETTYPE;
int mtp3_ipproto = MTP3_IPPROTO;


static int setup_socket(int localport, int sockettype, int ipproto)
{
  struct sockaddr_in sock;
  in_addr_t addr = INADDR_ANY;
  int parm, res, flags;
  int s;

  memset(&sock, 0, sizeof(struct sockaddr_in));
  sock.sin_family = AF_INET;
  sock.sin_port = htons(localport);
  memcpy(&sock.sin_addr, &addr, sizeof(addr));

  s = socket(PF_INET, sockettype, ipproto);
  if (s < 0) {
    ast_log(LOG_ERROR, "Cannot create UDP socket, errno=%d: %s\n", errno, strerror(errno));
    return -1;
  }
  parm = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &parm, sizeof(int));

  res = fcntl(s, F_GETFL);
  if(res < 0) {
    ast_log(LOG_WARNING, "Could not obtain flags for socket fd: %s.\n", strerror(errno));
    return -1;
  }
  flags = res | O_NONBLOCK;
  res = fcntl(s, F_SETFL, flags);
  if(res < 0) {
    ast_log(LOG_WARNING, "Could not set socket fd non-blocking: %s.\n", strerror(errno));
    return -1;
  }

  if (bind(s, &sock, sizeof(sock)) < 0) {
    ast_log(LOG_ERROR, "Cannot bind receiver socket, errno=%d: %s\n", errno, strerror(errno));
    close(s);
    return -1;
  }
  if (sockettype != SOCK_DGRAM)
    if (listen(s, 8) < 0) {
      ast_log(LOG_ERROR, "Cannot listen on socket, errno=%d: %s\n", errno, strerror(errno));
      close(s);
      return -1;
    }
  return s;
}



int mtp3_setup_socket(int port, int schannel)
{
  return setup_socket(port + schannel, mtp3_sockettype, mtp3_ipproto);
}


int mtp3_connect_socket(const char* host, const char* port)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int s = -1, sock, oldflags, res, parm;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = mtp3_sockettype;
  hints.ai_protocol = mtp3_ipproto;
  res = getaddrinfo(host, port, NULL, &result);
  if (res != 0) {
    ast_log(LOG_ERROR, "Invalid hostname/IP address '%s' or port '%s': %s.\n", host, port, gai_strerror(res)
	    );
    return -1;
  }
  for (rp = result; rp; rp = rp->ai_next) {
    int flags;
    struct pollfd fds[1];
    /* This is not working for non TCP/UDP protocols:   res = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol); */
    sock = socket(rp->ai_family, hints.ai_socktype, hints.ai_protocol);
    if (sock == -1)
      continue;
    res = fcntl(sock, F_GETFL);
    if(res < 0) {
      ast_log(LOG_WARNING, "Could not obtain flags for socket fd: %s.\n", strerror(errno));
      return -1;
    }
    oldflags = res;
    flags = res | O_NONBLOCK;
    res = fcntl(sock, F_SETFL, flags);
    ast_log(LOG_DEBUG, "connecting to mtp3d %s:%d, fd %d\n", inet_ntoa(((struct sockaddr_in*) rp->ai_addr)->sin_addr), ntohs(((struct sockaddr_in*) rp->ai_addr)->sin_port), sock);
    if ((s = connect(sock, rp->ai_addr, rp->ai_addrlen)) != -1) {
      break;
    }
    else {
      if (errno == EINPROGRESS) {
	fds[0].events = POLLIN|POLLOUT|POLLHUP|POLLERR;
	fds[0].fd = sock;
	res = poll(fds, 1, 200);
	if ((res > 0) && (fds[0].revents & POLLOUT)  && !(fds[0].revents & (POLLHUP|POLLERR))) {
	  s = sock;
	  break;
	}
      }
    }
    close(sock);
  }
  if (rp == NULL) {
    ast_log(LOG_ERROR, "Could not connect to hostname/IP address '%s', port '%s': %s.\n", host, port, strerror(errno));
    res = -1;
  }
  freeaddrinfo(result);
  fcntl(s, F_SETFL, oldflags);

  parm = 1;
  if(setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &parm, sizeof(parm)) < 0) {
    close(s);
    return -1;
  }
  parm = 2;
  setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &parm, sizeof(parm));
  parm = 5;
  setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &parm, sizeof(parm));
  return s;
}


int mtp3_send(int s, const unsigned char* buff, unsigned int len)
{
  int res;
  do {
    res = send(s, buff, len, 0);
    if (res < 0) {
      if (errno != EINTR)
	break;
    }
    else if (res > 0) {
      buff += res;
      len -= res;
    }
  } while (len > 0);
  if (res < 0) {
    ast_log(LOG_ERROR, "Cannot send mtp3 packet: %s\n", strerror(errno));
  }
  return res;
}

void mtp3_reply(int s, const unsigned char* buff, unsigned int len, const struct sockaddr* to, socklen_t tolen)
{
  ast_log(LOG_DEBUG, "Send packet to fd:%d, %s:%d\n", s, inet_ntoa(((struct sockaddr_in*) to)->sin_addr), ntohs(((struct sockaddr_in*) to)->sin_port));
  int res;
  do {
    res = sendto(s, buff, len, 0, to, tolen);
    if (res < 0) {
      if (errno != EINTR)
	break;
    }
    else if (res > 0) {
      buff += res;
      len -= res;
    }
  } while (len > 0);
  if (res == -1)
    ast_log(LOG_ERROR, "Cannot send reply mtp3 packet: %s\n", strerror(errno));
}


int mtp3_register_isup(int s, int linkix)
{
  unsigned char buff[MTP_REQ_MAX_SIZE];
  struct mtp_req* req = (struct mtp_req *)buff;
  int res;

  memset(req, 0, sizeof(*req));
  req->isup.link = NULL;
  req->isup.slink = NULL;
  req->typ = MTP_REQ_REGISTER_L4;
  req->regist.ss7_protocol = SS7_PROTO_ISUP;
  req->regist.host_ix = this_host->host_ix;
  req->regist.linkix = linkix;
  res = send(s, buff, sizeof(*req), 0);
  if (res < 0) {
    ast_log(LOG_ERROR, "Cannot send mtp3 packet: %s\n", strerror(errno));
  }
  return res;
}


int mtp3_register_sccp(int s, int subsystem, int linkix)
{
  unsigned char buff[MTP_REQ_MAX_SIZE];
  struct mtp_req* req = (struct mtp_req *)buff;
  int res;

  memset(req, 0, sizeof(*req));
  req->isup.link = NULL;
  req->isup.slink = NULL;
  req->typ = MTP_REQ_REGISTER_L4;
  req->regist.ss7_protocol = SS7_PROTO_SCCP;
  req->regist.host_ix = this_host->host_ix;
  req->regist.linkix = linkix;
  req->regist.sccp.subsystem = subsystem;
  res = send(s, buff, sizeof(*req), 0);
  if (res < 0) {
    ast_log(LOG_ERROR, "Cannot send mtp3 packet: %s\n", strerror(errno));
  }
  return res;
}
