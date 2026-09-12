#include "tju_tcp.h"
#include <errno.h>

static uint16_t refresh_advertised_window(tju_tcp_t *sock);
static void tx_ack(tju_tcp_t *sock);
static void start_rto_timer(sender_window_t *wnd);

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
    sock->send_segments = NULL;

    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = NULL;
    sock->received_len = 0;
    
    if(pthread_cond_init(&sock->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    sock->window.wnd_send = calloc(1, sizeof(sender_window_t));
    sock->window.wnd_recv = calloc(1, sizeof(receiver_window_t));
    sock->window.wnd_send->window_size = 4;
    sock->window.wnd_send->buffer_capacity = SEND_BUFFER_CAPACITY;
    sock->window.wnd_send->mss = MAX_DLEN;
    sock->window.wnd_send->peer_wnd = UINT16_MAX;
    sock->window.wnd_send->rto_ms = RTO_MIN_MS;
    sock->window.wnd_send->timer_running = 0;
    sock->window.wnd_send->has_rtt_sample = 0;
    sock->window.wnd_send->last_ack = 0;
    sock->window.wnd_send->dup_ack_count = 0;
    sock->window.wnd_send->fast_retransmit_done = 0;
    sock->window.wnd_send->persist_active = 0;
    sock->window.wnd_send->persist_interval_ms = 0;
    sock->window.wnd_recv->capacity = RECV_BUFFER_CAPACITY;
    sock->window.wnd_recv->used = 0;
    sock->window.wnd_recv->ooo_used = 0;
    sock->window.wnd_recv->advertised_wnd = UINT16_MAX;
    sock->window.wnd_recv->ooo_head = NULL;

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

        refresh_advertised_window(sock),
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
    sock->window.wnd_send->last_ack = sock->seq_num;



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
        uint32_t flight = sock->window.wnd_send->snd_nxt - sock->window.wnd_send->snd_una;
        if (sock->window.wnd_send->peer_wnd == 0)
            printf("[FLOW][SEND_BLOCK_ZERO_WINDOW]\n");
        while ((uint64_t)flight + (uint32_t)data_len > sock->window.wnd_send->peer_wnd) {
            if (sock->window.wnd_send->peer_wnd == 0)
                printf("[FLOW][SEND_BLOCK_ZERO_WINDOW]\n");
            printf("[FLOW][SEND_WINDOW] flight=%u rwnd=%u allowed=%u\n",
                   flight, sock->window.wnd_send->peer_wnd,
                   (sock->window.wnd_send->peer_wnd > flight) ?
                   sock->window.wnd_send->peer_wnd - flight : 0);
            if (sock->window.wnd_send->peer_wnd == 0) {
                sender_window_t *wnd = sock->window.wnd_send;
                if (!wnd->persist_active) {
                    wnd->persist_active = 1;
                    wnd->persist_interval_ms = wnd->rto_ms ? wnd->rto_ms : 1;
                    printf("[FLOW][PERSIST_START] rto_ms=%u\n",
                           wnd->persist_interval_ms);
                }

                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_sec += wnd->persist_interval_ms / 1000;
                deadline.tv_nsec += (long)(wnd->persist_interval_ms % 1000) * 1000000L;
                if (deadline.tv_nsec >= 1000000000L) {
                    deadline.tv_sec++;
                    deadline.tv_nsec -= 1000000000L;
                }
                int wait_rc = pthread_cond_timedwait(&(sock->wait_cond),
                                                     &(sock->recv_lock),
                                                     &deadline);
                if (wait_rc == ETIMEDOUT && wnd->peer_wnd == 0) {
                    uint16_t advertised = refresh_advertised_window(sock);
                    uint32_t probe_seq = wnd->snd_nxt;
                    char *probe = create_packet_buf(
                        sock->established_local_addr.port,
                        sock->established_remote_addr.port,
                        probe_seq, sock->ack_num,
                        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                        ACK_FLAG_MASK, advertised, ZERO_PROBE_EXT,
                        NULL, 0);
                    printf("[FLOW][ZERO_PROBE_TX] seq=%u interval_ms=%u\n",
                           probe_seq, wnd->persist_interval_ms);
                    sendToLayer3(probe, DEFAULT_HEADER_LEN);
                    free(probe);

                    uint32_t old_interval = wnd->persist_interval_ms;
                    uint64_t next_interval = (uint64_t)old_interval * 2;
                    if (next_interval > PERSIST_MAX_MS)
                        next_interval = PERSIST_MAX_MS;
                    wnd->persist_interval_ms = (uint32_t)next_interval;
                    printf("[FLOW][PERSIST_BACKOFF] next_ms=%u\n",
                           wnd->persist_interval_ms);
                }
            } else {
                pthread_cond_wait(&(sock->wait_cond), &(sock->recv_lock));
            }
            flight = sock->window.wnd_send->snd_nxt - sock->window.wnd_send->snd_una;
        }
        printf("[FLOW][SEND_WINDOW] flight=%u rwnd=%u allowed=%u\n",
               flight, sock->window.wnd_send->peer_wnd,
               (sock->window.wnd_send->peer_wnd > flight) ?
               sock->window.wnd_send->peer_wnd - flight : 0);
        printf("[RDT][WINDOW] snd_una=%u snd_nxt=%u window_size=%u flight_size=%u\n",
               sock->window.wnd_send->snd_una, sock->window.wnd_send->snd_nxt,
               sock->window.wnd_send->window_size,
               sock->window.wnd_send->snd_nxt - sock->window.wnd_send->snd_una);
        pthread_mutex_unlock(&(sock->recv_lock));
        pthread_mutex_lock(&(sock->recv_lock));
        uint32_t seq = sock->window.wnd_send->snd_nxt;
        char *segment = malloc(data_len);
        if (segment == NULL) {
            pthread_mutex_unlock(&(sock->recv_lock));
            pthread_mutex_unlock(&(sock->send_lock));
            return -1;
        }
        memcpy(segment, (const char *)buffer + offset, data_len);
        if ((size_t)sock->sending_len + (size_t)data_len >
            sock->window.wnd_send->buffer_capacity) {
            free(segment);
            pthread_mutex_unlock(&(sock->recv_lock));
            pthread_mutex_unlock(&(sock->send_lock));
            return -1;
        }
        char *combined = realloc(sock->sending_buf, sock->sending_len + data_len);
        if (combined == NULL) {
            free(segment);
            pthread_mutex_unlock(&(sock->recv_lock));
            pthread_mutex_unlock(&(sock->send_lock));
            return -1;
        }
        sock->sending_buf = combined;
        memcpy(sock->sending_buf + sock->sending_len, segment, data_len);
        sock->sending_len += data_len;
        sock->window.wnd_send->snd_nxt = seq + data_len;
        gettimeofday(&sock->window.wnd_send->send_time, NULL);
        send_segment_t *pending = calloc(1, sizeof(*pending));
        pending->seq = seq;
        pending->len = data_len;
        pending->data = malloc(data_len);
        memcpy(pending->data, segment, data_len);
        gettimeofday(&pending->send_time, NULL);
        if (sock->send_segments == NULL) {
            sock->send_segments = pending;
        } else {
            send_segment_t *tail = sock->send_segments;
            while (tail->next != NULL) tail = tail->next;
            tail->next = pending;
        }
        if (!sock->window.wnd_send->timer_running) {
            start_rto_timer(sock->window.wnd_send);
            printf("[RDT][RTO_TIMER_START] seq=%u rto_ms=%u\n",
                   seq, sock->window.wnd_send->rto_ms);
        }
        printf("[RDT][TIMER_START] seq=%u timestamp=%ld.%06ld\n", seq,
               (long)sock->window.wnd_send->send_time.tv_sec,
               (long)sock->window.wnd_send->send_time.tv_usec);
        printf("[RDT][SEND] seq=%u len=%d snd_una=%u snd_nxt=%u\n",
               seq, data_len, sock->window.wnd_send->snd_una,
               sock->window.wnd_send->snd_nxt);
        printf("[RDT][WINDOW] snd_una=%u snd_nxt=%u window_size=%u flight_size=%u\n",
               sock->window.wnd_send->snd_una, sock->window.wnd_send->snd_nxt,
               sock->window.wnd_send->window_size,
               sock->window.wnd_send->snd_nxt - sock->window.wnd_send->snd_una);
        pthread_mutex_unlock(&(sock->recv_lock));
        uint16_t plen = DEFAULT_HEADER_LEN + data_len;
        uint16_t advertised = refresh_advertised_window(sock);
        char *msg = create_packet_buf(sock->established_local_addr.port,
            sock->established_remote_addr.port, seq, sock->ack_num,
            DEFAULT_HEADER_LEN, plen, ACK_FLAG_MASK, advertised, 0,
            segment, data_len);
        printf("[FLOW][RWND_TX] free=%zu advertised=%u\n",
               sock->window.wnd_recv->capacity - sock->window.wnd_recv->used,
               advertised);
        /* Temporary RTO test hook: drop the single test segment (seq=1) once. */
        static uint32_t drop_seq = 1;

        if (seq == drop_seq) {
            printf("[RDT][TEST_DROP] seq=%u\n", seq);
            drop_seq = 0;
        } else {
            sendToLayer3(msg, plen);
        }
        free(msg);
        free(segment);
        printf("[RDT][WAIT_ALL_ACK] seq_end=%u\n", seq + (uint32_t)data_len);
        offset += data_len;
    }
    pthread_mutex_lock(&(sock->recv_lock));
    if (sock->send_segments != NULL &&
        !sock->window.wnd_send->timer_running) {
        start_rto_timer(sock->window.wnd_send);
        printf("[RDT][RTO_TIMER_START] seq=%u rto_ms=%u\n",
               sock->send_segments->seq,
               sock->window.wnd_send->rto_ms);
    }
    while (sock->window.wnd_send->snd_una < sock->window.wnd_send->snd_nxt) {
        sender_window_t *wnd = sock->window.wnd_send;
        int wait_rc = pthread_cond_timedwait(&(sock->wait_cond),
                                             &(sock->recv_lock),
                                             &wnd->rto_deadline);
        if (wait_rc == ETIMEDOUT) {
            send_segment_t *pending = sock->send_segments;
            if (pending == NULL) continue;
            uint32_t rseq = pending->seq;
            int rlen = pending->len;
            printf("[RDT][RTO_TIMEOUT] seq=%u rto_ms=%u\n",
                   rseq, wnd->rto_ms);
            /* Keep the historical marker for existing test scripts. */
            printf("[RDT][TIMEOUT] seq=%u\n", rseq);
            uint32_t old_rto = wnd->rto_ms;
            uint64_t backed_off = (uint64_t)old_rto * 2;
            wnd->rto_ms = (backed_off > RTO_MAX_MS) ?
                RTO_MAX_MS : (uint32_t)backed_off;
            if (wnd->rto_ms < RTO_MIN_MS)
                wnd->rto_ms = RTO_MIN_MS;
            printf("[RDT][RTO_BACKOFF] old_ms=%u new_ms=%u\n",
                   old_rto, wnd->rto_ms);
            char *retry = create_packet_buf(sock->established_local_addr.port,
                sock->established_remote_addr.port, rseq, sock->ack_num,
                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN + rlen,
                ACK_FLAG_MASK, refresh_advertised_window(sock), 0, pending->data, rlen);
            printf("[RDT][RETRANSMIT] seq=%u\n", rseq);
            sendToLayer3(retry, DEFAULT_HEADER_LEN + rlen);
            free(retry);
            gettimeofday(&pending->send_time, NULL);
            pending->retransmitted = 1;
            start_rto_timer(wnd);
        }
    }
    sock->window.wnd_send->timer_running = 0;
    free(sock->sending_buf);
    sock->sending_buf = NULL;
    sock->sending_len = 0;
    printf("[RDT][ACKED] ack=%u\n", sock->window.wnd_send->snd_una);
    pthread_mutex_unlock(&(sock->recv_lock));
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
    size_t old_window = sock->window.wnd_recv->advertised_wnd;
    refresh_advertised_window(sock);
    if (old_window == 0 && sock->window.wnd_recv->advertised_wnd > 0)
        printf("[FLOW][WINDOW_UPDATE] old=0 new=%u\n",
               sock->window.wnd_recv->advertised_wnd);
    tx_ack(sock);
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    return 0;
}

static void append_received(tju_tcp_t *sock, const char *data, uint32_t len){
    sock->received_buf = realloc(sock->received_buf, sock->received_len + len);
    memcpy(sock->received_buf + sock->received_len, data, len);
    sock->received_len += len;
}

/* Store an absolute deadline so condition-variable wakeups (including
   duplicate ACKs) do not restart the RTO from the current time. */
static void start_rto_timer(sender_window_t *wnd){
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    wnd->rto_deadline = now;
    wnd->rto_deadline.tv_sec += wnd->rto_ms / 1000;
    wnd->rto_deadline.tv_nsec +=
        (long)(wnd->rto_ms % 1000) * 1000000L;
    if (wnd->rto_deadline.tv_nsec >= 1000000000L) {
        wnd->rto_deadline.tv_sec++;
        wnd->rto_deadline.tv_nsec -= 1000000000L;
    }
    wnd->timer_running = 1;
}

static uint16_t refresh_advertised_window(tju_tcp_t *sock){
    receiver_window_t *rw = sock->window.wnd_recv;
    size_t occupied = (size_t)sock->received_len + rw->ooo_used;
    rw->used = occupied;
    size_t free_space = (rw->capacity > occupied) ? rw->capacity - occupied : 0;
    rw->advertised_wnd = (free_space > UINT16_MAX) ? UINT16_MAX : (uint16_t)free_space;
    return rw->advertised_wnd;
}

static void tx_ack(tju_tcp_t *sock){
    uint16_t advertised = refresh_advertised_window(sock);
    size_t occupied = sock->window.wnd_recv->used;
    size_t free_space = sock->window.wnd_recv->capacity > occupied ?
        sock->window.wnd_recv->capacity - occupied : 0;
    char *ack_pkt = create_packet_buf(sock->established_local_addr.port,
        sock->established_remote_addr.port, sock->seq_num, sock->ack_num,
        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, advertised, 0, NULL, 0);
    printf("[RDT][ACK_TX] ack=%u\n", sock->ack_num);
    printf("[FLOW][RWND_TX] free=%zu advertised=%u\n",
           free_space,
           advertised);
    sendToLayer3(ack_pkt, DEFAULT_HEADER_LEN);
    free(ack_pkt);
}

/* Reply to a persist probe without treating it as application data.  The
   probe carries no payload and its sequence number is deliberately not
   consumed; this ACK therefore advertises the receiver's current rcv_nxt and
   real available window. */
static void tx_probe_ack(tju_tcp_t *sock){
    uint16_t advertised = refresh_advertised_window(sock);
    sock->ack_num = sock->window.wnd_recv->rcv_nxt;
    size_t occupied = sock->window.wnd_recv->used;
    size_t free_space = sock->window.wnd_recv->capacity > occupied ?
        sock->window.wnd_recv->capacity - occupied : 0;
    char *ack_pkt = create_packet_buf(
        sock->established_local_addr.port,
        sock->established_remote_addr.port,
        sock->seq_num, sock->ack_num,
        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
        ACK_FLAG_MASK, advertised, 0, NULL, 0);
    printf("[FLOW][ZERO_PROBE_ACK] ack=%u rwnd=%u\n",
           sock->ack_num, advertised);
    printf("[RDT][ACK_TX] ack=%u\n", sock->ack_num);
    printf("[FLOW][RWND_TX] free=%zu advertised=%u\n",
           free_space, advertised);
    sendToLayer3(ack_pkt, DEFAULT_HEADER_LEN);
    free(ack_pkt);
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
            refresh_advertised_window(new_conn),
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
        new_conn->window.wnd_send->last_ack = new_conn->seq_num;
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
            refresh_advertised_window(sock),
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
    if (sock->state == ESTABLISHED) {
        uint16_t rwnd = get_advertised_window(pkt);
        pthread_mutex_lock(&(sock->recv_lock));
        sock->window.wnd_send->peer_wnd = rwnd;
        printf("[FLOW][RWND_RX] rwnd=%u\n", rwnd);
        if (rwnd == 0)
            printf("[FLOW][ZERO_WINDOW_RX]\n");
        else if (sock->window.wnd_send->persist_active) {
            printf("[FLOW][PERSIST_STOP] rwnd=%u\n", rwnd);
            sock->window.wnd_send->persist_active = 0;
            sock->window.wnd_send->persist_interval_ms = 0;
        }
        pthread_cond_broadcast(&(sock->wait_cond));
        pthread_mutex_unlock(&(sock->recv_lock));
    }

    /* A persist probe is an ACK-only control packet marked in the existing
       header extension byte.  Handle it before normal ACK/duplicate-ACK logic
       so it cannot spuriously trigger fast retransmit. */
    if (sock->state == ESTABLISHED &&
        (flags & ACK_FLAG_MASK) &&
        get_ext(pkt) == ZERO_PROBE_EXT &&
        get_plen(pkt) == DEFAULT_HEADER_LEN) {
        pthread_mutex_lock(&(sock->recv_lock));
        printf("[FLOW][ZERO_PROBE_RX] seq=%u rcv_nxt=%u\n",
               get_seq(pkt), sock->window.wnd_recv->rcv_nxt);
        tx_probe_ack(sock);
        pthread_mutex_unlock(&(sock->recv_lock));
        return 0;
    }

    if (sock->state == ESTABLISHED && (flags & ACK_FLAG_MASK)) {
        uint32_t ack = get_ack(pkt);
        pthread_mutex_lock(&(sock->recv_lock));
        if (ack > sock->window.wnd_send->snd_una) {
            uint32_t confirmed = ack - sock->window.wnd_send->snd_una;
            struct timeval now;
            gettimeofday(&now, NULL);
            send_segment_t *sample = sock->send_segments;
            int karn_ambiguous = 0;
            while (sample && sample->seq + sample->len <= ack) {
                if (sample->retransmitted) karn_ambiguous = 1;
                sample = sample->next;
            }
            if (karn_ambiguous) {
                printf("[RDT][KARN_SKIP_ACK] ack=%u\n", ack);
            } else {
                sample = sock->send_segments;
                /* One ACK event contributes at most one RTT sample. */
                if (sample && sample->seq + sample->len <= ack && !sample->retransmitted) {
                    long rtt = (now.tv_sec - sample->send_time.tv_sec) * 1000L +
                        (now.tv_usec - sample->send_time.tv_usec) / 1000L;
                    if (rtt < 1) rtt = 1;
                    printf("[RDT][RTT_SAMPLE] seq=%u rtt_ms=%ld\n", sample->seq, rtt);
                    if (!sock->window.wnd_send->has_rtt_sample) {
                        sock->window.wnd_send->srtt_ms = rtt;
                        sock->window.wnd_send->rttvar_ms = rtt / 2;
                        sock->window.wnd_send->has_rtt_sample = 1;
                    } else {
                        uint32_t diff = sock->window.wnd_send->srtt_ms > (uint32_t)rtt ?
                            sock->window.wnd_send->srtt_ms - rtt : rtt - sock->window.wnd_send->srtt_ms;
                        sock->window.wnd_send->rttvar_ms =
                            (3 * sock->window.wnd_send->rttvar_ms + diff) / 4;
                        sock->window.wnd_send->srtt_ms =
                            (7 * sock->window.wnd_send->srtt_ms + rtt) / 8;
                    }
                    uint64_t raw_rto =
                        (uint64_t)sock->window.wnd_send->srtt_ms +
                        ((4ULL * sock->window.wnd_send->rttvar_ms > 1) ?
                         4 * (uint64_t)sock->window.wnd_send->rttvar_ms : 1);
                    uint64_t bounded_rto = raw_rto;
                    if (bounded_rto < RTO_MIN_MS)
                        bounded_rto = RTO_MIN_MS;
                    if (bounded_rto > RTO_MAX_MS)
                        bounded_rto = RTO_MAX_MS;
                    sock->window.wnd_send->rto_ms = (uint32_t)bounded_rto;
                    printf("[RDT][RTT_UPDATE] srtt_ms=%u rttvar_ms=%u raw_rto_ms=%llu rto_ms=%u\n",
                           sock->window.wnd_send->srtt_ms,
                           sock->window.wnd_send->rttvar_ms,
                           (unsigned long long)raw_rto,
                           sock->window.wnd_send->rto_ms);
                }
            }
            sock->window.wnd_send->snd_una = ack;
            while (sock->send_segments != NULL &&
                   sock->send_segments->seq + sock->send_segments->len <= ack) {
                send_segment_t *done = sock->send_segments;
                sock->send_segments = done->next;
                free(done->data);
                free(done);
            }
            if (confirmed >= (uint32_t)sock->sending_len) {
                free(sock->sending_buf);
                sock->sending_buf = NULL;
                sock->sending_len = 0;
            } else if (sock->sending_buf != NULL) {
                memmove(sock->sending_buf, sock->sending_buf + confirmed,
                        sock->sending_len - confirmed);
                sock->sending_len -= confirmed;
                sock->sending_buf = realloc(sock->sending_buf, sock->sending_len);
            }
            printf("[RDT][ACK_RX] ack=%u snd_una=%u\n", ack,
                   sock->window.wnd_send->snd_una);
            sock->window.wnd_send->last_ack = ack;
            sock->window.wnd_send->dup_ack_count = 0;
            sock->window.wnd_send->fast_retransmit_done = 0;
            if (sock->send_segments != NULL) {
                start_rto_timer(sock->window.wnd_send);
                printf("[RDT][RTO_TIMER_RESTART] ack=%u rto_ms=%u\n",
                       ack, sock->window.wnd_send->rto_ms);
            } else {
                sock->window.wnd_send->timer_running = 0;
            }
            pthread_cond_broadcast(&(sock->wait_cond));
        } else if (ack == sock->window.wnd_send->snd_una &&
                   sock->send_segments != NULL) {
            sender_window_t *wnd = sock->window.wnd_send;
            if (wnd->last_ack == ack)
                wnd->dup_ack_count++;
            else {
                wnd->last_ack = ack;
                wnd->dup_ack_count = 1;
            }
            printf("[RDT][DUP_ACK] ack=%u count=%u\n", ack,
                   wnd->dup_ack_count);
            if (wnd->dup_ack_count >= 3 && !wnd->fast_retransmit_done) {
                send_segment_t *seg = sock->send_segments;
                printf("[RDT][FAST_RETRANSMIT] seq=%u len=%u\n",
                       seg->seq, seg->len);
                char *retry = create_packet_buf(
                    sock->established_local_addr.port,
                    sock->established_remote_addr.port,
                    seg->seq, sock->ack_num,
                    DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN + seg->len,
            ACK_FLAG_MASK, refresh_advertised_window(sock), 0, seg->data, seg->len);
                sendToLayer3(retry, DEFAULT_HEADER_LEN + seg->len);
                free(retry);
                seg->retransmitted = 1;
                gettimeofday(&seg->send_time, NULL);
                wnd->fast_retransmit_done = 1;
            }
        }
        pthread_mutex_unlock(&(sock->recv_lock));
    }

    uint16_t plen = get_plen(pkt);
    if (plen < DEFAULT_HEADER_LEN)
        return -1;
    uint32_t data_len = plen - DEFAULT_HEADER_LEN;

    if(data_len <= 0)
        return 0;
    pthread_mutex_lock(&(sock->recv_lock));
    size_t occupied = (size_t)sock->received_len + sock->window.wnd_recv->ooo_used;
    if (occupied >= sock->window.wnd_recv->capacity ||
        (size_t)data_len > sock->window.wnd_recv->capacity - occupied) {
        sock->ack_num = sock->window.wnd_recv->rcv_nxt;
        tx_ack(sock);
        pthread_mutex_unlock(&(sock->recv_lock));
        return 0;
    }
    uint32_t packet_seq = get_seq(pkt);
    uint32_t expected = sock->window.wnd_recv->rcv_nxt;
    if (packet_seq < expected) {
        printf("[RDT][DUPLICATE] seq=%u expected=%u\n", packet_seq, expected);
        sock->ack_num = expected;
        tx_ack(sock);
    } else if (packet_seq > expected) {
        printf("[RDT][OUT_OF_ORDER] seq=%u expected=%u\n", packet_seq, expected);
        recv_segment_t *cur = sock->window.wnd_recv->ooo_head;
        int duplicate = 0;
        while (cur) { if (cur->seq == packet_seq) duplicate = 1; cur = cur->next; }
        if (!duplicate) {
            recv_segment_t *seg = calloc(1, sizeof(*seg));
            seg->seq = packet_seq; seg->len = data_len; seg->data = malloc(data_len);
            memcpy(seg->data, pkt + DEFAULT_HEADER_LEN, data_len);
            /* Keep the out-of-order list sorted so contiguous segments can
               be consumed from its head after a missing segment arrives. */
            recv_segment_t **insert = &sock->window.wnd_recv->ooo_head;
            while (*insert != NULL && (*insert)->seq < seg->seq)
                insert = &(*insert)->next;
            seg->next = *insert;
            *insert = seg;
            sock->window.wnd_recv->ooo_used += data_len;
        }
        sock->ack_num = expected;
        tx_ack(sock);
    } else {
        append_received(sock, pkt + DEFAULT_HEADER_LEN, data_len);
        expected += data_len;
        printf("[RDT][IN_ORDER] seq=%u len=%u rcv_nxt=%u\n", packet_seq, data_len, expected);
        /* The list is sorted; repeatedly consume its head while it directly
           follows rcv_nxt.  This handles any number of cached segments. */
        while (sock->window.wnd_recv->ooo_head != NULL &&
               sock->window.wnd_recv->ooo_head->seq == expected) {
            recv_segment_t *seg = sock->window.wnd_recv->ooo_head;
            append_received(sock, seg->data, seg->len);
            expected += seg->len;
            sock->window.wnd_recv->ooo_used -= seg->len;
            sock->window.wnd_recv->ooo_head = seg->next;
            free(seg->data);
            free(seg);
        }
        sock->ack_num = expected;
        sock->window.wnd_recv->rcv_nxt = expected;
        tx_ack(sock);
    }
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
