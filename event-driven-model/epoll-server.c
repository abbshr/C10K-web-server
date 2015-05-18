#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/syscall.h>
//#include <sys/sendfile.h>

// #define MAX_CONN 20000
#define MAX_EVENTS 10240
#define PORT "8888"

static int create_and_bind(char *port) {
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int ret, fd;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  ret = getaddrinfo(NULL, port, &hints, &result);
  if (ret != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
    return -1;
  }

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) {
      continue;
    }

    ret = bind(fd, rp->ai_addr, rp->ai_addrlen);
    if (ret == 0) {
      break;
    }
    close(fd);
  }

  if (rp == NULL) {
    perror("bind");
    return -1;
  }

  freeaddrinfo(result);
  return fd;
}

static int set_non_block(int fd) {
  int flags, ret;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    perror("fcntl");
    return -1;
  }

  flags |= O_NONBLOCK;
  ret = fcntl(fd, F_SETFL, flags);
  if (ret == -1) {
    perror("fcntl");
    return -1;
  }

  return 0;
}

int main(int argc, char* argv[]) {
  int listen_fd, ret;
  int epoll_fd;
  struct epoll_event event;
  struct epoll_event *events;

  listen_fd = create_and_bind(PORT);
  if (listen_fd == -1) {
    return -1;
  }

  ret = set_non_block(listen_fd);
  if (ret == -1) {
    return -1;
  }

  ret = listen(listen_fd, SOMAXCONN);
  if (ret == -1) {
    perror("listen");
    return -1;
  }

  epoll_fd = syscall(__NR_epoll_create1, 0);//epoll_create1(0);
  if (epoll_fd == -1) {
    perror("epoll_create");
    return -1;
  }

  event.data.fd = listen_fd;
  event.events = EPOLLIN | EPOLLET;
  ret = syscall(__NR_epoll_ctl, epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);//epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);
  if (ret == -1) {
    perror("epoll_ctl");
    return -1;
  }

  events = calloc(MAX_EVENTS, sizeof event);

  for (;;) {
    int n, i;

    n = syscall(__NR_epoll_wait, events, MAX_EVENTS, -1);//epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    for (i = 0; i < n; i++) {
      if (events[i].events & EPOLLERR || events[i].events &EPOLLHUP || !events[i].events & EPOLLIN) {
        fprintf(stderr, "epoll error\n");
        close(events[i].data.fd);
        continue;
      } else if (listen_fd == events[i].data.fd) {
        for (;;) {
          struct sockaddr in_addr;
          socklen_t in_len;
          int in_fd;
          char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

          in_len = sizeof in_addr;
          in_fd = accept(listen_fd, &in_addr, &in_len);
          if (in_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;
            } else {
              perror("accpet");
              break;
            }
          }

          ret = getnameinfo(&in_addr, in_len, hbuf, sizeof hbuf, sbuf, sizeof sbuf, NI_NUMERICHOST | NI_NUMERICSERV);
          if (ret == 0) {
            printf("accept connection on fd %d, [%s:%s]\n",  in_fd, hbuf, sbuf);
          }

          ret = set_non_block(in_fd);
          if (ret == -1) {
            return -1;
          }

          event.data.fd = in_fd;
          event.events = EPOLLIN | EPOLLET;
          ret = syscall(__NR_epoll_ctl, EPOLL_CTL_ADD, in_fd, &event);//epoll_ctl(epoll_fd, EPOLL_CTL_ADD, in_fd, &event);
          if (ret == -1) {
            perror("epoll_ctl");
            return -1;
          }
        }
        continue;
      } else {
        int done = 0;

        for (;;) {
          ssize_t count;
          char buf[512];

          count = read(events[i].data.fd, buf, sizeof buf);
          if (count == -1) {
            if (errno != EAGAIN) {
              perror("read");
              done = 1;
            }
            break;
          } else if (count == 0) {
            done = 1;
            break;
          }

          ret = write(1, buf, count);
          if (ret == -1) {
            perror("write");
            return -1;
          }
        }

        if (done == 1) {
          printf("process finished on fd: %d\n", events[i].data.fd);
          close(events[i].data.fd);
        }
      }
    }
  }
}
