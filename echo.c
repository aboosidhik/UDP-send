/*
gcc echo.c -o echo
Format to send
./echo 127.0.0.1 10000 send


*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

 
 int main(int argc, char **argv)
{
  int s;
  int ret;
  char *buf;
  struct sockaddr_in addr;
 
  if (argc != 4) {
    puts("usage: send ipaddr port data");
    exit(1);
  }
 
  addr.sin_family = AF_INET;
  ret = inet_aton(argv[1], &addr.sin_addr);
  if (ret == 0) { perror("inet_aton"); exit(1); }
  addr.sin_port = htons(atoi(argv[2]));
  buf = argv[3];
 
  s = socket(PF_INET, SOCK_DGRAM, 0);
  if (s == -1) { perror("socket"); exit(1); }
 
  ret = sendto(s, buf, strlen(buf), 0, (struct sockaddr *)&addr, sizeof(addr));
  if (ret == -1) { perror("sendto"); exit(1); }
 
  return 0;
}
 
 
