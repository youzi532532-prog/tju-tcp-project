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
    tju_tcp_t *sock = tju_socket();
    tju_sock_addr server = { inet_network(SERVER_IP), 1234 };
    if (tju_connect(sock, server) != 0) return EXIT_FAILURE;

    unsigned char *data = malloc(total);
    if (data == NULL) return EXIT_FAILURE;
    for (size_t i = 0; i < total; ++i) data[i] = test_byte(i);

    int sent = tju_send(sock, data, (int)total);
    free(data);
    if (sent != (int)total) {
        fprintf(stderr, "[CC_TEST][CLIENT_FAIL] sent=%d expected=%zu\n",
                sent, total);
        return EXIT_FAILURE;
    }
    printf("[CC_TEST][CLIENT_SENT] bytes=%zu\n", total);
    fflush(stdout);

    if (tju_close(sock) != 0) return EXIT_FAILURE;
    printf("[CC_TEST][CLIENT_PASS] bytes=%zu\n", total);
    return EXIT_SUCCESS;
}
