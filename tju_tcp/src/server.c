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


    size_t recv_len = (size_t)MAX_DLEN * 8;
    char *buf = malloc(recv_len);
    if (buf == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    /* tju_recv() is an application read; wait until the complete test
       payload has arrived before actively closing.  This keeps the close
       test from sending FIN while the peer still has data in flight. */
    printf("[TEST][CLOSE] waiting for expected data=%zu\n", recv_len);
    fflush(stdout);
    pthread_mutex_lock(&(new_conn->recv_lock));
    while ((size_t)new_conn->received_len < recv_len &&
           new_conn->state != CLOSED) {
        pthread_cond_wait(&(new_conn->wait_cond), &(new_conn->recv_lock));
    }
    printf("[TEST][CLOSE] data ready received_len=%d expected=%zu\n",
           new_conn->received_len, recv_len);
    pthread_mutex_unlock(&(new_conn->recv_lock));

    printf("[TEST][CLOSE] BEFORE_RECV received_len=%d expected=%zu\n",
           new_conn->received_len, recv_len);
    fflush(stdout);
    int consumed = tju_recv(new_conn, buf, (int)recv_len);
    printf("[TEST][CLOSE] AFTER_RECV n=%d\n", consumed);
    fflush(stdout);
    free(buf);

    printf("[TEST][CLOSE] active close\n");
    fflush(stdout);
    tju_close(new_conn);


    return EXIT_SUCCESS;
}
