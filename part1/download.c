/**      (C)2000-2021 FEUP
 *       tidy up some includes and parameters
 * */

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#include <string.h>

#define SERVER_PORT 21
#define MAX_LENGTH 1024

struct url {
    const char *user;
    const char *password;
    const char *host;
    int port;
    const char *path;
    const char *file;
};

static void fail(const char *phase) {
    fprintf(stderr, "Download failed in phase: %s\n", phase);
    exit(-1);
}

/*ftp://[<user>:<password>@]<host>[:<port>]/<url-path>*/
static int parse_url(char *url, struct url *u) {
    char *user, *host, *path, *p;

    if (strncmp(url, "ftp://", 6) != 0)
        return -1;
    host = url + 6;
    if ((path = strchr(host, '/')) == NULL)
        return -1;
    *path++ = '\0';

    u->user = "anonymous";
    u->password = "anonymous@";
    if ((p = strrchr(host, '@')) != NULL) {
        user = host;
        host = p + 1;
        *p = '\0';
        u->user = user;
        u->password = "";
        if ((p = strchr(user, ':')) != NULL) {
            *p = '\0';
            u->password = p + 1;
        }
    }

    u->port = SERVER_PORT;
    if ((p = strchr(host, ':')) != NULL) {
        *p = '\0';
        u->port = atoi(p + 1);
    }

    u->host = host;
    u->path = path;
    u->file = (p = strrchr(path, '/')) != NULL ? p + 1 : path;

    if (*u->host == '\0' || *u->file == '\0' || u->port <= 0 || u->port > 65535)
        return -1;
    return 0;
}

/**
 * Example code for getting the IP address from hostname.
 * tidy up includes
 */
static int get_ip(const char *host, char *ip) {
    struct hostent *h;

/**
 * The struct hostent (host entry) with its terms documented

    struct hostent {
        char *h_name;    // Official name of the host.
        char **h_aliases;    // A NULL-terminated array of alternate names for the host.
        int h_addrtype;    // The type of address being returned; usually AF_INET.
        int h_length;    // The length of the address in bytes.
        char **h_addr_list;    // A zero-terminated array of network addresses for the host.
        // Host addresses are in Network Byte Order.
    };

    #define h_addr h_addr_list[0]	The first address in h_addr_list.
*/
    if ((h = gethostbyname(host)) == NULL) {
        herror("gethostbyname()");
        return -1;
    }

    printf("Host name  : %s\n", h->h_name);
    printf("IP Address : %s\n", inet_ntoa(*((struct in_addr *) h->h_addr)));

    strcpy(ip, inet_ntoa(*((struct in_addr *) h->h_addr)));
    return 0;
}

static int open_connection(const char *ip, int port) {
    int sockfd;
    struct sockaddr_in server_addr;

    /*server address handling*/
    bzero((char *) &server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);    /*32 bit Internet address network byte ordered*/
    server_addr.sin_port = htons(port);        /*server TCP port must be network byte ordered */

    /*open a TCP socket*/
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        return -1;
    }
    /*connect to the server*/
    if (connect(sockfd,
                (struct sockaddr *) &server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect()");
        close(sockfd);
        return -1;
    }
    return sockfd;
}

static int read_line(int sockfd, char *line) {
    int length = 0;
    char c;

    while (read(sockfd, &c, 1) == 1) {
        if (c == '\n') {
            line[length] = '\0';
            return 0;
        }
        if (c != '\r' && length < MAX_LENGTH - 1)
            line[length++] = c;
    }
    return -1;
}

/*a reply ends with a line containing the code, a space, optional text and CRLF*/
static int read_reply(int sockfd, char *reply) {
    char code[4] = "";

    do {
        if (read_line(sockfd, reply) < 0) {
            fprintf(stderr, "Control connection closed\n");
            return -1;
        }
        printf("%s\n", reply);
        if (code[0] == '\0') {
            if (strspn(reply, "0123456789") < 3)
                return -1;
            memcpy(code, reply, 3);
        }
    } while (strncmp(reply, code, 3) != 0 || (reply[3] != ' ' && reply[3] != '\0'));

    return atoi(code);
}

static int send_command(int sockfd, char *reply, const char *format, ...) {
    char buf[MAX_LENGTH];
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (length < 0 || length >= (int) sizeof(buf)) {
        fprintf(stderr, "Command too long\n");
        return -1;
    }
    printf("%.*s\n", length - 2, buf);

    /*send a string to the server*/
    if (write(sockfd, buf, length) < 0) {
        perror("write()");
        return -1;
    }
    return read_reply(sockfd, reply);
}

/*227 Entering Passive Mode (h1,h2,h3,h4,p1,p2).*/
static int open_data_connection(int sockfd, char *reply) {
    unsigned char h1, h2, h3, h4, p1, p2;
    char ip[INET_ADDRSTRLEN];
    const char *p;

    if (send_command(sockfd, reply, "PASV\r\n") != 227)
        return -1;
    for (p = reply + 3; *p != '\0'; p++)
        if (sscanf(p, "%hhu,%hhu,%hhu,%hhu,%hhu,%hhu", &h1, &h2, &h3, &h4, &p1, &p2) == 6) {
            sprintf(ip, "%d.%d.%d.%d", h1, h2, h3, h4);
            return open_connection(ip, p1 * 256 + p2);
        }
    return -1;
}

static long long receive_file(int datafd, const char *filename) {
    char buf[MAX_LENGTH];
    ssize_t bytes;
    long long total = 0;
    FILE *file;

    if ((file = fopen(filename, "wb")) == NULL) {
        perror("fopen()");
        return -1;
    }
    while ((bytes = read(datafd, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, bytes, file) != (size_t) bytes) {
            perror("fwrite()");
            fclose(file);
            return -1;
        }
        total += bytes;
    }
    if (bytes < 0) {
        perror("read()");
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        perror("fclose()");
        return -1;
    }
    return total;
}

int main(int argc, char *argv[]) {
    struct url url;
    char ip[INET_ADDRSTRLEN], reply[MAX_LENGTH];
    int sockfd, datafd, code;
    long long size = -1, bytes;

    if (argc != 2 || parse_url(argv[1], &url) < 0) {
        fprintf(stderr, "Usage: %s ftp://[<user>:<password>@]<host>[:<port>]/<url-path>\n", argv[0]);
        exit(-1);
    }

    if (get_ip(url.host, ip) < 0)
        fail("connect");
    if ((sockfd = open_connection(ip, url.port)) < 0 || read_reply(sockfd, reply) != 220)
        fail("connect");

    code = send_command(sockfd, reply, "USER %s\r\n", url.user);
    if (code == 331)
        code = send_command(sockfd, reply, "PASS %s\r\n", url.password);
    if (code != 230)
        fail("login");

    if (send_command(sockfd, reply, "TYPE I\r\n") != 200)
        fail("binary mode");
    if (send_command(sockfd, reply, "SIZE /%s\r\n", url.path) == 213)
        sscanf(reply, "213 %lld", &size);

    if ((datafd = open_data_connection(sockfd, reply)) < 0)
        fail("passive");

    if (send_command(sockfd, reply, "RETR /%s\r\n", url.path) / 100 != 1)
        fail("get path");

    bytes = receive_file(datafd, url.file);
    close(datafd);
    if (bytes < 0 || read_reply(sockfd, reply) / 100 != 2)
        fail("transfer");
    if (size >= 0 && bytes != size) {
        fprintf(stderr, "Received %lld of %lld bytes\n", bytes, size);
        fail("transfer");
    }

    send_command(sockfd, reply, "QUIT\r\n");

    if (close(sockfd)<0) {
        perror("close()");
        exit(-1);
    }
    printf("File saved as %s (%lld bytes)\n", url.file, bytes);
    return 0;
}