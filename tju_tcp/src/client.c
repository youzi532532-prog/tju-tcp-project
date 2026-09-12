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

    size_t total_len = (size_t)MAX_DLEN;
    char *data = malloc(total_len);

    if (data == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    memset(data, 'A', total_len);

    printf("[TEST][RTO] send one segment len=%zu\n", total_len);
    printf("[TEST][RTO] expect first transmission dropped\n");
    fflush(stdout);

    tju_send(my_socket, data, total_len);

    free(data);

    return EXIT_SUCCESS;
}
