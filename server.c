#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ftree.h"
#include "hash.h"

struct client {
    int fd;
    int state;
    struct in_addr ipaddr;
    struct request *info;
    struct client *next;
};

static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
struct request *createrequest(void);
void complete_path(const char *child, char *s);
int handleclient(struct client *p);
int checkfile(struct client *p);


int bindandlisten(unsigned short port);

char src[MAXPATH]; // source of file tree on server.

/*
 * Copies files sent from the client rooted at dest.
 */
void rcopy_server(unsigned short port, char *dest) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct client *head = NULL;
    socklen_t len;
    struct sockaddr_in q;
    fd_set allset;
    fd_set rset;
    strcpy(src, dest);

    int i;


    int listenfd = bindandlisten(port);
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;

        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == 0) {
            printf("No response from clients\n");
            continue;
        }

        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            // printf("a new client is connecting\n");
            len = sizeof(q);
            if ((clientfd = accept(listenfd, (struct sockaddr *)&q, &len)) < 0) {
                perror("accept");
                exit(1);
            }
            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            // printf("connection from %s\n", inet_ntoa(q.sin_addr));
            head = addclient(head, clientfd, q.sin_addr);
        }

        for(i = 0; i <= maxfd; i++) {
            if (FD_ISSET(i, &rset)) {
                for (p = head; p != NULL; p = p->next) {
                    if (p->fd == i) {
                        int result = handleclient(p);
                        if (result == -1) {
                            int tmp_fd = p->fd;
                            head = removeclient(head, p->fd);
                            FD_CLR(tmp_fd, &allset);
                            close(tmp_fd);
                        }
                        break;
                    }
                }
            }
        }
    }
}

/*
 * Based on the state of the client complete task.
 */
int handleclient(struct client *p) {

  if (p->state == AWAITING_TYPE) {
    int t;
    int len = read(p->fd, &t, sizeof(int));
    p->info->type = ntohl(t);
    if (len != sizeof(int)) {
      perror("server: reading error");
      exit(1);
    }
    // printf("type %d read %d bytes\n", p->info->type, len);
    p->state = AWAITING_PATH;
    return 1;
  }

  if (p->state == AWAITING_PATH) {
    int len = read(p->fd, &(p->info->path), MAXPATH);
    if (len != MAXPATH) {
      perror("server: reading error");
      exit(1);
    }
    // printf("path is %s read in %d bytes\n", p->info->path, len);
    p->state = AWAITING_PERM;
    return 1;
  }

  if (p->state == AWAITING_PERM) {
    int perm;
    int len = read(p->fd, &perm, sizeof(int));
    p->info->mode = ntohl(perm);
    if (len != sizeof(int)) {
      perror("server line 135: reading error");
      exit(-1);
    }
    // printf("mode %d read in %d bytes\n", p->info->mode, len);
    p->state = AWAITING_SIZE;
    return 1;
  }

  if(p->state == AWAITING_SIZE) {
    int len = read(p->fd, &(p->info->size), sizeof(int));
    if (len != sizeof(int)) {
      perror("server: reading error");
      exit(-1);
    }
    // printf("size %d read in %d bytes\n", p->info->size, len);
    p->state = AWAITING_HASH;
    return 1;
  }

  if (p->state == AWAITING_HASH) {
    int status;
    if (p->info->type == REGFILE) {
      int len = read(p->fd, &(p->info->hash), BLOCKSIZE);
      if (len != BLOCKSIZE) {
        perror("server: reading error");
      }
      // show_hash(p->info->hash);
      // printf("hash read in %d bytes\n", len);
      status = checkfile(p);
      write(p->fd, &status, sizeof(int));
      return -1;
    }
    if (p->info->type == REGDIR) {
      status = OK;
      int dir = mkdir(p->info->path, ((S_IRWXU | S_IRWXG | S_IRWXO) & (p->info->mode)));
      if (dir == -1) {
        if (errno != EEXIST) {
          perror("server: mkdir");
        }
      }
      write(p->fd, &status, sizeof(int));
      return -1;
    }
    int len = read(p->fd, &(p->info->hash), BLOCKSIZE);
    if (len != BLOCKSIZE) {
      perror("server: reading error");
    }
    p->state = AWAITING_DATA;
    return 1;
  }

  if (p->state == AWAITING_DATA) {
    int status = OK;
    FILE *in;
    char path[MAXPATH];
    complete_path(p->info->path, path);
    if ((in = fopen(path, "w")) != NULL) {
      int len;
      int s = 0;
      char byte;
      do {
        len = read(p->fd, &byte, 1);
        if (fwrite(&byte, 1, 1, in) != 1) {
          perror("server: fwrite");
          status = ERROR;
        }
      } while((len == 1) && ++s < p->info->size);
      fclose(in);
      if (len != 1) {
        perror("server: reading file");
        status = ERROR;
      }
      write(p->fd, &status, sizeof(int));
      return -1;
    } else {
      status = ERROR;
      perror("server: fopen");
      printf("error with %s\n", p->info->path);
      write(p->fd, &status, sizeof(int));
    }
  }
  return -1;
}

/*
 * Returns FD if success, exit if error.
 */
int bindandlisten(unsigned short port) {
    struct sockaddr_in r;
    int listenfd;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }
    int yes = 1;
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) {
        perror("setsockopt");
    }
    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr *)&r, sizeof r)) {
        perror("bind");
        exit(1);
    }

    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
    return listenfd;
}

/*
 * Adds client to list of clients.
 */
static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));
    if (!p) {
        perror("malloc");
        exit(1);
    }

    struct request *r = malloc(sizeof(struct request));
    if (!r) {
        perror("malloc");
        exit(1);
    }

    // printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->state = AWAITING_TYPE;
    p->ipaddr = addr;
    p->info = r;
    p->next = top;
    top = p;
    return top;
}

/*
 * Remove client from the list of clients.
 */
static struct client *removeclient(struct client *top, int fd) {
    struct client **p;

    for (p = &top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        // printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        free((*p)->info);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
    return top;
}

/*
 * Concatenates child path to src.
 */
void complete_path(const char *child, char *s) {
  strcpy(s, src);
  strcat(s, "/");
  strcat(s, child);
}


/*
 * Returns SENDFILE if transfer if needed, OK otherwise.
 */
int checkfile(struct client *p) {
  char target_path[MAXPATH];
  complete_path(p->info->path, target_path);

  struct stat properties;

  if (lstat(target_path, &properties) != -1) {
    if (S_ISDIR(properties.st_mode) && !S_ISDIR(p->info->mode)) {
      fprintf(stderr, "server: file type mismatch %s\n", target_path);
      return ERROR;
    }
    if (!S_ISDIR(properties.st_mode) && S_ISDIR(p->info->mode)) {
      fprintf(stderr, "server: file type mismatch %s\n", target_path);
      return ERROR;
    }
    if (properties.st_size != p->info->size) {
      return SENDFILE;
    }
    FILE *target_stream = fopen(target_path, "rb");
    char target_hash[BLOCKSIZE];
    hash(target_hash, target_stream);
    if (check_hash(target_hash, p->info->hash)) {
      return SENDFILE;
    }
    int new_permissions = (S_IRWXU | S_IRWXG | S_IRWXO) & (p->info->mode);
    int old_permissions = (S_IRWXU | S_IRWXG | S_IRWXO) & (properties.st_mode);
    if (new_permissions != old_permissions) { // update permissions.
    if (chmod(target_path, new_permissions) != 0) {
      perror("server: chmod");
    }
  }
    return OK;
  } else {
    if (ENOENT == errno) {
      return SENDFILE;
    } else {
      perror("server: lstat error");
      printf("error occured on %s\n", target_path);
      return ERROR;
    }
  }
}
