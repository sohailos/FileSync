#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <libgen.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "ftree.h"
#include "hash.h"

int depth = 0;

#define MAXDATASIZE 100 /* max number of bytes we can get at once  */

/*
 * Concatenates child path to parent path.
 */
void complete_path(const char *parent, const char *child, char *dest) {
  strcpy(dest, parent);
  strcat(dest, "/");
  strcat(dest, child);
}

/*
 * Creates and returns a new socket connection.
 */
int accept_connnection(char *host, unsigned short port) {

  int sockfd;
  struct hostent *he;
  struct sockaddr_in their_addr; /* connector's address information  */

  if ((he=gethostbyname(host)) == NULL) {  /* get the host info  */
      perror("gethostbyname");
      exit(1);
  }

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("socket");
      exit(1);
  }

  their_addr.sin_family = AF_INET;    /* host byte order */
  their_addr.sin_port = htons(port);  /* short, network byte order */
  their_addr.sin_addr = *((struct in_addr *)he->h_addr);
  bzero(&(their_addr.sin_zero), 8);   /* zero the rest of the struct */

  if (connect(sockfd, (struct sockaddr *)&their_addr,
  sizeof(struct sockaddr)) == -1) {
      perror("connect");
      exit(1);
  }

  return sockfd;

}

/*
 * Copies file tree with root based at src to server.
 * r_src is the root for the tree on the server.
 */
int rcopy_client(char *src, char *host, unsigned short port, char *r_src, int type) {

    int sockfd = accept_connnection(host, port);
    int fcalls = 0; // keep track of children.
    int final = 0; // return value.

    struct stat properties;
    struct request r;

    if (lstat(src, &properties) == -1) {
      perror("client: lstat");
      exit(1);
    }

    if (!S_ISLNK(properties.st_mode)) { // skip links.
      strcpy(r.path, r_src);
      r.mode = properties.st_mode;
      r.size = properties.st_size;
      if (!S_ISDIR(properties.st_mode)) {
        if (type == 0) {
          r.type = REGFILE;
        } else {
          r.type = TRANSFILE;
        }
        FILE *in;
        if ((in = fopen(src, "rb")) != NULL) {
          hash(r.hash, in);
          fclose(in);
        } else {
          perror("client: fopen");
          exit(1);
        }
      } else {
        r.type = REGDIR;
      }

      int t, m;
      t = htonl(r.type);
      m = htonl(r.mode);

      if (write(sockfd, &t, sizeof(int)) == -1) {
        perror("client: writing type");
        exit(1);
      }
      if (write(sockfd, &r.path, MAXPATH) == -1) {
        perror("client: writing path");
        exit(1);
      }
      if (write(sockfd, &m, sizeof(int)) == -1) {
        perror("client: writing mode");
        exit(1);
      }
      if (write(sockfd, &r.size, sizeof(int)) == -1) {
        perror("client: writing size");
        exit(1);
      }
      if (write(sockfd, &r.hash, BLOCKSIZE) == -1) {
        perror("client: writing hash");
        exit(1);
      }

      if (r.type == TRANSFILE) {
        FILE *out;
        if ((out = fopen(src, "rb")) != NULL) {
          int s = 0;
          char byte;
          while ((fread(&byte, 1, 1, out) == 1) && s++ < r.size) {
            write(sockfd, &byte, 1);
          }
          fclose(out);
        } else {
          perror("client: fopen");
          exit(1);
        }
      }

      int status, len;
      len = read(sockfd, &status, sizeof(int));
      if (len != sizeof(int)) {
        perror("client: reading from socket");
        exit(1);
      } else {
        if (status == SENDFILE) {
          pid_t pid = fork();
          if (pid == 0) {
            close(sockfd); // child will initate a new connection.
            depth++;
            rcopy_client(src, host, port, r_src, TRANSFILE);
          } else {
            fcalls++;
          }
        }
        if (status == OK) {
          if (S_ISDIR(properties.st_mode)) {
            DIR *curr;
            if ((curr = opendir(src)) == NULL) {
              perror("client: opendir");
              exit(1);
            }
            struct dirent *child;
            while ((child = readdir(curr)) != NULL) {
              if (strncmp(child->d_name, ".", 1) != 0) {
                pid_t pid = fork();
                if (pid == 0) {
                  depth++;
                  char client_child_path[MAXPATH];
                  char server_child_path[MAXPATH];
                  complete_path(src, child->d_name, client_child_path);
                  complete_path(r_src, child->d_name, server_child_path);
                  close(sockfd);
                  rcopy_client(client_child_path, host, port, server_child_path, 0);
                } else {
                  fcalls++;
                }
              }
            }
          }
        }
        if (status == ERROR) {
          fprintf(stderr, "client: error occured with %s\n", src);
          final = 1;
        }
      }
    }

    close(sockfd);

    int status;
    char pid;
    while (fcalls > 0) {
      wait(&status);
      if (WIFEXITED(status)) {
        pid = WEXITSTATUS(status);
        if (!final) {
          final = pid;
        }
      } else {
        perror("client: exit");
        exit(1);
      }
      --fcalls;
    }

  if (depth == 0) {
    return final;
  } else {
    exit(final);
  }
}
