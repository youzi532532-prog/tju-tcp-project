#include "tju_tcp.h"

/*
创建 TCP socket 
初始化对应的结构体
设置初始状态为 CLOSED
*/
tju_tcp_t* tju_socket(){
    tju_tcp_t* sock = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;

    sock->seq_num=0;
    sock->ack_num=0;/*初始化序号和确认号*/
    
    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = NULL;
    sock->sending_len = 0;

    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = NULL;
    sock->received_len = 0;
    
    if(pthread_cond_init(&sock->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    sock->window.wnd_send = calloc(1, sizeof(sender_window_t));
    sock->window.wnd_recv = calloc(1, sizeof(receiver_window_t));
    sock->window.wnd_send->window_size = 1;
    sock->window.wnd_send->mss = MAX_DLEN;
    sock->window.wnd_send->peer_wnd = TCP_RECVWN_SIZE;
    sock->window.wnd_send->rto_ms = 1000;
    sock->window.wnd_recv->capacity = TCP_RECVWN_SIZE;
    sock->window.wnd_recv->advertised_wnd = TCP_RECVWN_SIZE;

    sock->pending_conn = NULL;
    sock->listener = NULL;

    return sock;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr){
    sock->bind_addr = bind_addr;
    return 0;
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t* sock){
    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;
    return 0;
}

/*
接受连接 
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t* tju_accept(tju_tcp_t* listen_sock){
    /* Wait for the connection created by the receive thread.  The previous
       implementation copied the listening socket and inserted a fake
       ESTABLISHED entry, which shadowed the LISTEN lookup for the SYN. */
    if (listen_sock == NULL || listen_sock->state != LISTEN)
        return NULL;
    pthread_mutex_lock(&(listen_sock->recv_lock));
    while (listen_sock->pending_conn == NULL ||
           listen_sock->pending_conn->state != ESTABLISHED) {
        pthread_cond_wait(&(listen_sock->wait_cond),
                          &(listen_sock->recv_lock));
    }
    tju_tcp_t* accepted_conn = listen_sock->pending_conn;
    listen_sock->pending_conn = NULL;
    pthread_mutex_unlock(&(listen_sock->recv_lock));
    return accepted_conn;

#if 0
    /* Legacy code retained only as historical reference. */
    tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    memcpy(new_conn, listen_sock, sizeof(tju_tcp_t));

    tju_sock_addr local_addr, remote_addr;
    /*
     这里涉及到TCP连接的建立
     正常来说应该是收到客户端发来的SYN报文
     从中拿到对端的IP和PORT
     换句话说 下面的处理流程其实不应该放在这里 应该在tju_handle_packet中
    */ 
    remote_addr.ip = inet_network(CLIENT_IP);  //具体的IP地址
    remote_addr.port = 5678;  //端口

    local_addr.ip = listen_sock->bind_addr.ip;  //具体的IP地址
    local_addr.port = listen_sock->bind_addr.port;  //端口

    new_conn->established_local_addr = local_addr;
    new_conn->established_remote_addr = remote_addr;

    // 这里应该是经过三次握手后才能修改状态为ESTABLISHED
    new_conn->state = ESTABLISHED;

    // 将新的conn放到内核建立连接的socket哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, remote_addr.ip, remote_addr.port);
    established_socks[hashval] = new_conn;

    // 如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
    // 在linux中 每个listen socket都维护一个已经完成连接的socket队列
    // 每次调用accept 实际上就是取出这个队列中的一个元素
    // 队列为空,则阻塞 
    return new_conn;
#endif
}


/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t* sock, tju_sock_addr target_addr){

    sock->established_remote_addr = target_addr;


    tju_sock_addr local_addr;

    local_addr.ip = inet_network(CLIENT_IP);
    local_addr.port = 5678;

    sock->established_local_addr = local_addr;


    /*
     * 进入主动连接状态
     */
    sock->state = SYN_SENT;



    /*
     * 注意：
     * 当前框架没有SYN_SENT专用哈希表
     * 所以必须提前加入established_socks
     * 否则收到SYN+ACK时kernel找不到socket
     */

    int hashval = cal_hash(
        local_addr.ip,
        local_addr.port,
        target_addr.ip,
        target_addr.port
    );


    established_socks[hashval] = sock;



    /*
     * 发送SYN
     */

    char* msg = create_packet_buf(
        sock->established_local_addr.port,
        sock->established_remote_addr.port,

        sock->seq_num,
        0,

        DEFAULT_HEADER_LEN,
        DEFAULT_HEADER_LEN,

        SYN_FLAG_MASK,

        1,
        0,

        NULL,
        0
    );
    printf("client send SYN\n");

    sendToLayer3(
        msg,
        DEFAULT_HEADER_LEN
    );


    free(msg);



    /*
     * SYN占用一个序号
     */
    sock->seq_num++;

    sock->window.wnd_send->snd_una = sock->seq_num;
    sock->window.wnd_send->snd_nxt = sock->seq_num;



    /*
     * 等待握手完成
     */

    pthread_mutex_lock(&(sock->recv_lock));


    while(sock->state != ESTABLISHED){

        pthread_cond_wait(
            &(sock->wait_cond),
            &(sock->recv_lock)
        );

    }


    pthread_mutex_unlock(&(sock->recv_lock));


    return 0;
}
int tju_send(tju_tcp_t* sock, const void *buffer, int len){
    // 这里当然不能直接简单地调用sendToLayer3
    if (sock == NULL || buffer == NULL || len <= 0 || sock->state != ESTABLISHED)
        return -1;
    pthread_mutex_lock(&(sock->send_lock));
    int offset = 0;
    while (offset < len) {
        int data_len = len - offset;
        if (data_len > MAX_DLEN) data_len = MAX_DLEN;
        pthread_mutex_lock(&(sock->recv_lock));
        uint32_t seq = sock->window.wnd_send->snd_nxt;
        sock->sending_buf = malloc(data_len);
        if (sock->sending_buf == NULL) {
            pthread_mutex_unlock(&(sock->recv_lock));
            pthread_mutex_unlock(&(sock->send_lock));
            return -1;
        }
        memcpy(sock->sending_buf, (const char *)buffer + offset, data_len);
        sock->sending_len = data_len;
        sock->window.wnd_send->snd_nxt = seq + data_len;
        printf("[RDT][SEND] seq=%u len=%d snd_una=%u snd_nxt=%u\n",
               seq, data_len, sock->window.wnd_send->snd_una,
               sock->window.wnd_send->snd_nxt);
        pthread_mutex_unlock(&(sock->recv_lock));
        uint16_t plen = DEFAULT_HEADER_LEN + data_len;
        char *msg = create_packet_buf(sock->established_local_addr.port,
            sock->established_remote_addr.port, seq, sock->ack_num,
            DEFAULT_HEADER_LEN, plen, ACK_FLAG_MASK, 1, 0,
            sock->sending_buf, data_len);
        sendToLayer3(msg, plen);
        free(msg);
        printf("[RDT][WAIT_ACK] seq_end=%u\n", seq + (uint32_t)data_len);
        pthread_mutex_lock(&(sock->recv_lock));
        while (sock->window.wnd_send->snd_una < seq + (uint32_t)data_len)
            pthread_cond_wait(&(sock->wait_cond), &(sock->recv_lock));
        free(sock->sending_buf);
        sock->sending_buf = NULL;
        sock->sending_len = 0;
        pthread_mutex_unlock(&(sock->recv_lock));
        printf("[RDT][ACKED] ack=%u\n", seq + (uint32_t)data_len);
        offset += data_len;
    }
    pthread_mutex_unlock(&(sock->send_lock));
    return len;
}
int tju_recv(tju_tcp_t* sock, void *buffer, int len){
    while(sock->received_len<=0){
        // 阻塞
    }

    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    int read_len = 0;
    if (sock->received_len >= len){ // 从中读取len长度的数据
        read_len = len;
    }else{
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    memcpy(buffer, sock->received_buf, read_len);

    if(read_len < sock->received_len) { // 还剩下一些
        char* new_buf = malloc(sock->received_len - read_len);
        memcpy(new_buf, sock->received_buf + read_len, sock->received_len - read_len);
        free(sock->received_buf);
        sock->received_len -= read_len;
        sock->received_buf = new_buf;
    }else{
        free(sock->received_buf);
        sock->received_buf = NULL;
        sock->received_len = 0;
    }
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    return 0;
}

int tju_handle_packet(tju_tcp_t* sock, char* pkt){

    uint8_t flags = get_flags(pkt);
    /*
     * 情况1：
     * server LISTEN状态收到SYN
     *
     * client              server
     *
     * SYN  ------------>
     *
     *                 SYN_RECV
     *
     * SYN+ACK <------------
     *
     */
    if(sock->state == LISTEN &&
       (flags & SYN_FLAG_MASK)){
        printf("server received SYN\n");
        tju_tcp_t* new_conn = tju_socket();
        new_conn->listener = sock;
        /*
         * 状态转换
         */
        new_conn->state = SYN_RECV;
        /*
         * 保存四元组
         */
        // server地址
        new_conn->established_local_addr.ip =
            sock->bind_addr.ip;
        new_conn->established_local_addr.port =
            get_dst(pkt);
        // client地址
        new_conn->established_remote_addr.ip =
            inet_network(CLIENT_IP);
        new_conn->established_remote_addr.port =
            get_src(pkt);
        /*
         * 序号处理
         */
        // client SYN占用一个序号
        new_conn->ack_num =
            get_seq(pkt)+1;
        new_conn->window.wnd_recv->rcv_nxt = new_conn->ack_num;
        // server初始序号
        new_conn->seq_num = 100;
        /*
         * 保存到连接表
         *
         * 课程框架没有半连接队列
         * 暂时直接保存
         */
        int hashval = cal_hash(
            new_conn->established_local_addr.ip,
            new_conn->established_local_addr.port,
            new_conn->established_remote_addr.ip,
            new_conn->established_remote_addr.port
        );
        established_socks[hashval] = new_conn;

        /* Publish the half-open connection for tju_accept(). */
        pthread_mutex_lock(&(sock->recv_lock));
        if (sock->pending_conn == NULL)
            sock->pending_conn = new_conn;
        pthread_cond_signal(&(sock->wait_cond));
        pthread_mutex_unlock(&(sock->recv_lock));
        /*
         * 构造SYN+ACK
         */
        char* synack = create_packet_buf(
            new_conn->established_local_addr.port,
            new_conn->established_remote_addr.port,
            new_conn->seq_num,
            new_conn->ack_num,
            DEFAULT_HEADER_LEN,
            DEFAULT_HEADER_LEN,
            SYN_FLAG_MASK | ACK_FLAG_MASK,
            1,
            0,
            NULL,
            0
        );
        sendToLayer3(
            synack,
            DEFAULT_HEADER_LEN
        );
        /*
         * SYN消耗一个序号
         */
        new_conn->seq_num++;
        new_conn->window.wnd_send->snd_una = new_conn->seq_num;
        new_conn->window.wnd_send->snd_nxt = new_conn->seq_num;
        return 0;
    }

    /*
     * 情况2：
     * client收到SYN+ACK
     *
     * SYN_SENT
     *
     *          SYN+ACK
     * <----------------
     *
     * ESTABLISHED
     */
    if(sock->state == SYN_SENT &&
       (flags & SYN_FLAG_MASK) &&
       (flags & ACK_FLAG_MASK)){
        printf("client received SYN+ACK\n");

        pthread_mutex_lock(&(sock->recv_lock));
        /*
         * 更新确认号
         *
         * server SYN占一个序号
         */
        sock->ack_num =
            get_seq(pkt)+1;
        sock->window.wnd_recv->rcv_nxt = sock->ack_num;
        /*
         * 回复ACK
         */
        char* ack_pkt = create_packet_buf(
            sock->established_local_addr.port,
            sock->established_remote_addr.port,
            sock->seq_num,
            sock->ack_num,
            DEFAULT_HEADER_LEN,
            DEFAULT_HEADER_LEN,
            ACK_FLAG_MASK,
            1,
            0,
            NULL,
            0
        );
        sendToLayer3(
            ack_pkt,
            DEFAULT_HEADER_LEN
        );

        /*
         * 进入ESTABLISHED
         */

        sock->state = ESTABLISHED;
        /*
         * 放入已建立连接表
         */

        int hashval = cal_hash(
            sock->established_local_addr.ip,
            sock->established_local_addr.port,
            sock->established_remote_addr.ip,
            sock->established_remote_addr.port
        );
        established_socks[hashval] = sock;
        /*
         * 唤醒connect()
         */

        pthread_cond_signal(
            &(sock->wait_cond)
        );

        pthread_mutex_unlock(
            &(sock->recv_lock)
        );

        return 0;

    }
    /*
     * 情况3：
     * server收到ACK
     *
     * SYN_RECV
     *
     * ACK
     * <------------
     *
     * ESTABLISHED
     */

    if(sock->state == SYN_RECV &&
       (flags & ACK_FLAG_MASK)){
        printf("server received ACK\n");

        pthread_mutex_lock(&(sock->recv_lock));

        sock->state = ESTABLISHED;

        /* Wake accept() only after the third handshake packet arrives. */
        if (sock->listener != NULL) {
            pthread_mutex_lock(&(sock->listener->recv_lock));
            pthread_cond_signal(&(sock->listener->wait_cond));
            pthread_mutex_unlock(&(sock->listener->recv_lock));
        }

        /*
         * 确保在连接表中
         */

        int hashval = cal_hash(
            sock->established_local_addr.ip,
            sock->established_local_addr.port,
            sock->established_remote_addr.ip,
            sock->established_remote_addr.port
        );
        established_socks[hashval] = sock;

        pthread_cond_signal(
            &(sock->wait_cond)
        );

        pthread_mutex_unlock(
            &(sock->recv_lock)
        );

        return 0;

    }

    /*
     * 普通数据包
     */
    if (sock->state == ESTABLISHED && (flags & ACK_FLAG_MASK)) {
        uint32_t ack = get_ack(pkt);
        pthread_mutex_lock(&(sock->recv_lock));
        if (ack > sock->window.wnd_send->snd_una) {
            sock->window.wnd_send->snd_una = ack;
            printf("[RDT][ACK_RX] ack=%u snd_una=%u\n", ack,
                   sock->window.wnd_send->snd_una);
            pthread_cond_broadcast(&(sock->wait_cond));
        }
        pthread_mutex_unlock(&(sock->recv_lock));
    }

    uint16_t plen = get_plen(pkt);
    if (plen < DEFAULT_HEADER_LEN)
        return -1;
    uint32_t data_len = plen - DEFAULT_HEADER_LEN;

    if(data_len <= 0)
        return 0;
    pthread_mutex_lock(
        &(sock->recv_lock)
    );

    if(sock->received_buf == NULL){

        sock->received_buf =
            malloc(data_len);
    }else{

        sock->received_buf =
            realloc(
                sock->received_buf,
                sock->received_len + data_len
            );
    }

    memcpy(
        sock->received_buf + sock->received_len,
        pkt + DEFAULT_HEADER_LEN,
        data_len
    );

    sock->received_len += data_len;
    sock->ack_num = get_seq(pkt) + data_len;
    sock->window.wnd_recv->rcv_nxt = sock->ack_num;
    printf("[RDT][RECV] seq=%u len=%u next_ack=%u\n",
           get_seq(pkt), data_len, sock->ack_num);
    char *ack_pkt = create_packet_buf(sock->established_local_addr.port,
        sock->established_remote_addr.port, sock->seq_num, sock->ack_num,
        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0,
        NULL, 0);
    sendToLayer3(ack_pkt, DEFAULT_HEADER_LEN);
    free(ack_pkt);
    pthread_cond_broadcast(&(sock->wait_cond));

    pthread_mutex_unlock(
        &(sock->recv_lock)
    );

    return 0;
}
// int tju_handle_packet(tju_tcp_t* sock, char* pkt){
    
//     uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;

//     // 把收到的数据放到接受缓冲区
//     while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

//     if(sock->received_buf == NULL){
//         sock->received_buf = malloc(data_len);
//     }else {
//         sock->received_buf = realloc(sock->received_buf, sock->received_len + data_len);
//     }
//     memcpy(sock->received_buf + sock->received_len, pkt + DEFAULT_HEADER_LEN, data_len);
//     sock->received_len += data_len;

//     pthread_mutex_unlock(&(sock->recv_lock)); // 解锁


//     return 0;
// }

int tju_close (tju_tcp_t* sock){
    return 0;
}
