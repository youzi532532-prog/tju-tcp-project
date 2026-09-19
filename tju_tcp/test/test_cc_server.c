#include "tju_tcp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned char test_byte(size_t offset){
    return (unsigned char)(offset % 251U);
}

int main(int argc, char **argv){
    size_t total = 512U * 1024U;
    if (argc > 1) total = strtoull(argv[1], NULL, 10);
    if (total == 0 || total > INT32_MAX) return EXIT_FAILURE;

    startSimulation();
    tju_tcp_t *listener = tju_socket();
    tju_sock_addr local = { inet_network(SERVER_IP), 1234 };
    if (tju_bind(listener, local) != 0 || tju_listen(listener) != 0)
        return EXIT_FAILURE;
    tju_tcp_t *sock = tju_accept(listener);
    if (sock == NULL) return EXIT_FAILURE;

    unsigned char buffer[16384];
    size_t received = 0;
    while (received < total) {
        size_t wanted = total - received;
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        int count = tju_recv(sock, buffer, (int)wanted);
        if (count <= 0) {
            fprintf(stderr, "[CC_TEST][SERVER_FAIL] recv=%d at=%zu\n",
                    count, received);
            return EXIT_FAILURE;
        }
        for (int i = 0; i < count; ++i) {
            if (buffer[i] != test_byte(received + (size_t)i)) {
                fprintf(stderr, "[CC_TEST][SERVER_FAIL] corrupt_at=%zu\n",
                        received + (size_t)i);
                return EXIT_FAILURE;
            }
        }
        received += (size_t)count;
    }
    printf("[CC_TEST][SERVER_RECEIVED] bytes=%zu\n", received);
    fflush(stdout);

    if (tju_close(sock) != 0) return EXIT_FAILURE;
    printf("[CC_TEST][SERVER_PASS] bytes=%zu\n", received);
    return EXIT_SUCCESS;
}
