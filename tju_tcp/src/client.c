#include "tju_tcp.h"
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    // 开启仿真环境 
    startSimulation();

    tju_tcp_t* my_socket = tju_socket();
    // printf("my_tcp state %d\n", my_socket->state);
    
    tju_sock_addr target_addr;
    target_addr.ip = inet_network(SERVER_IP);
    target_addr.port = 1234;

    tju_connect(my_socket, target_addr);
    // printf("my_socket state %d\n", my_socket->state);      

    // uint32_t conn_ip;
    // uint16_t conn_port;

    // conn_ip = my_socket->established_local_addr.ip;
    // conn_port = my_socket->established_local_addr.port;
    // printf("my_socket established_local_addr ip %d port %d\n", conn_ip, conn_port);

    // conn_ip = my_socket->established_remote_addr.ip;
    // conn_port = my_socket->established_remote_addr.port;
    // printf("my_socket established_remote_addr ip %d port %d\n", conn_ip, conn_port);

    sleep(3);

    size_t total_len = (size_t)MAX_DLEN * 6000;
    char *data = malloc(total_len);

    if (data == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    memset(data, 'A', total_len);

    printf("[TEST] client send len=%zu bytes, segments=%zu\n",
        total_len,
        (total_len + MAX_DLEN - 1) / MAX_DLEN);

    tju_send(my_socket, data, total_len);

    free(data);

    char buf[2021];
    tju_recv(my_socket, (void*)buf, 12);
    printf("client recv %s\n", buf);

    tju_recv(my_socket, (void*)buf, 10);
    printf("client recv %s\n", buf);

    return EXIT_SUCCESS;
}
