#include "wifi.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_sockfd = -1;
static struct sockaddr_in g_esp_addr;

// mDNS discovery helper
static int resolve_mdns(const char *hostname, char *ip_out, size_t ip_len) {
        struct addrinfo hints, *result, *rp;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        int s = getaddrinfo(hostname, NULL, &hints, &result);
        if (s != 0) {
                fprintf(stderr, "mDNS lookup failed: %s\n", gai_strerror(s));
                return -1;
        }

        for (rp = result; rp != NULL; rp = rp->ai_next) {
                struct sockaddr_in *addr = (struct sockaddr_in *)rp->ai_addr;
                inet_ntop(AF_INET, &addr->sin_addr, ip_out, ip_len);
                freeaddrinfo(result);
                return 0;
        }

        freeaddrinfo(result);
        return -1;
}

int wifi_init(const char *esp_hostname, uint16_t port, int timeout_ms) {
        char esp_ip[INET_ADDRSTRLEN];

        // Try mDNS resolution (e.g., "esp32-bias.local")
        if (resolve_mdns(esp_hostname, esp_ip, sizeof(esp_ip)) == -1) {
                fprintf(stderr, "Failed to resolve %s via mDNS\n", esp_hostname);
                return -1;
        }

        printf("Resolved %s to %s\n", esp_hostname, esp_ip);

        g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_sockfd < 0) {
                perror("socket");
                return -1;
        }

        // Set socket timeout
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(g_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        memset(&g_esp_addr, 0, sizeof(g_esp_addr));
        g_esp_addr.sin_family = AF_INET;
        g_esp_addr.sin_port = htons(port);
        inet_pton(AF_INET, esp_ip, &g_esp_addr.sin_addr);

        return 0;
}

ssize_t wifi_tx(const uint8_t *data, size_t len) {
        if (g_sockfd < 0) {
                return -1;
        }

        ssize_t sent =
            sendto(g_sockfd, data, len, 0, (struct sockaddr *)&g_esp_addr, sizeof(g_esp_addr));

        if (sent < 0) {
                perror("sendto");
                return -1;
        }

        return sent;
}

void wifi_close(void) {
        if (g_sockfd >= 0) {
                close(g_sockfd);
                g_sockfd = -1;
        }
}
