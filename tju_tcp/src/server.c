#include "tju_tcp.h"
#include <string.h>

int main(int argc, char **argv) {
    // 开启仿真环境 
    startSimulation();

    tju_tcp_t* my_server = tju_socket();
    // printf("my_tcp state %d\n", my_server->state);
    
    tju_sock_addr bind_addr;
    bind_addr.ip = inet_network(SERVER_IP);
    bind_addr.port = 1234;

    tju_bind(my_server, bind_addr);

    tju_listen(my_server);
    // printf("my_server state %d\n", my_server->state);

    tju_tcp_t* new_conn = tju_accept(my_server);
    // printf("new_conn state %d\n", new_conn->state);      

    // uint32_t conn_ip;
    // uint16_t conn_port;

    // conn_ip = new_conn->established_local_addr.ip;
    // conn_port = new_conn->established_local_addr.port;
    // printf("new_conn established_local_addr ip %d port %d\n", conn_ip, conn_port);

    // conn_ip = new_conn->established_remote_addr.ip;
    // conn_port = new_conn->established_remote_addr.port;
    // printf("new_conn established_remote_addr ip %d port %d\n", conn_ip, conn_port);


    /* Zero-window test: keep the application from draining received data so
       the advertised window can reach zero. */
    printf("[TEST][FLOW] server start, stop reading\n");
    printf("[TEST][FLOW] WAIT_BEFORE_RECV\n");
    fflush(stdout);
    sleep(10);

    size_t recv_len = (size_t)MAX_DLEN * 100;
    char *buf = malloc(recv_len);
    if (buf == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    printf("[TEST][FLOW] BEFORE_RECV received_len=%d expected_release=%zu\n",
           new_conn->received_len, recv_len);
    fflush(stdout);
    int consumed = tju_recv(new_conn, buf, (int)recv_len);
    printf("[TEST][FLOW] AFTER_RECV n=%d\n", consumed);
    fflush(stdout);
    free(buf);

    printf("[TEST][FLOW] KEEP_ALIVE\n");
    fflush(stdout);
    while (1)
        sleep(1);


    return EXIT_SUCCESS;
}
