//
// Created by tobia on 24.02.2018.
//

#include <stdlib.h>
#include <unistd.h>
#include <rmemory.h>
#include <stdio.h>
#include <rastaredundancy_new.h>
#include <time.h>
#include <errno.h>
#include <syscall.h>
#include <rasta_new.h>
#include <event_system.h>
#include <rastahandle.h>
#include <rasta_lib.h>
#include <stdbool.h>

/**
 * this will generate a 4 byte timestamp of the current system time
 * @return current system time in s since 1970
 */
uint32_t cur_timestamp() {
    long ms;
    time_t s;
    struct timespec spec;

    clock_gettime(CLOCK_MONOTONIC, &spec);

    s = spec.tv_sec;

    // seconds to milliseconds
    ms = s * 1000;

    // nanoseconds to milliseconds
    ms += (long)(spec.tv_nsec / 1.0e6);

    return (uint32_t)ms;
}

unsigned long mix(unsigned long a, unsigned long b, unsigned long c)
{
    a=a-b;  a=a-c;  a=a^(c >> 13);
    b=b-c;  b=b-a;  b=b^(a << 8);
    c=c-a;  c=c-b;  c=c^(b >> 13);
    a=a-b;  a=a-c;  a=a^(c >> 12);
    b=b-c;  b=b-a;  b=b^(a << 16);
    c=c-a;  c=c-b;  c=c^(b >> 5);
    a=a-b;  a=a-c;  a=a^(c >> 3);
    b=b-c;  b=b-a;  b=b^(a << 10);
    c=c-a;  c=c-b;  c=c^(b >> 15);
    return c;
}
/**
 * generate a 4 byte random number
 * @return 4 byte random number
 */
uint32_t long_random(void) {
    srand(mix(clock(), time(NULL), getpid()));

    uint32_t r = 0;

    //int randomSrc = open("/dev/random", O_RDONLY);
    //unsigned long seed[1];
    //read(randomSrc , seed, sizeof(long) );
    //close(randomSrc);

    for (int i=0; i<32; i++) {
        r = r*2 + rand()%2;
    }
    return r;
    //return seed[0];
}

/**
 * Gets the initial sequence number from the config. If the sequence number was set to a negative value, a random number will
 * be used
 * @param config the config that is used to get the sequence number
 * @return the initial sequence number
 */
uint32_t get_initial_seq_num(struct RastaConfig * config){
    struct DictionaryEntry init_seq = config_get(config, RASTA_CONFIG_KEY_INITIAL_SEQ_NUM);

    if (init_seq.type == DICTIONARY_NUMBER){
        // random number when < 0 or the specified value else
        return (init_seq.value.number < 0) ? long_random() : init_seq.value.unumber;
    }

    // return random value if there is not a number in config
    return long_random();
}

/**
 * compares two RaSTA protocol version
 * @param local_version the local version
 * @param remote_version the remote version
 * @return  0 if local_version == remote_version
 *         -1 if local_version < remove_version
 *          1 if local_version > remote_version
 */
int compare_version(const char local_version[4], const char remote_version[4]){
    char * tmp;
    long local = strtol(local_version, &tmp, 4);
    long remote = strtol(remote_version, &tmp, 4);

    if (local == remote){
        return 0;
    } else if (local < remote) {
        return -1;
    } else {
        return 1;
    }
}

/**
 * checks if the given RaSTA protocol version is accepted
 * @param version the version of the remote
 * @return 1 if the remote version is accepted, else 0
 */
int version_accepted(struct rasta_receive_handle *h, const char version[4]){
    /*struct DictionaryEntry accepted_version = config_get(&con->configuration_parameters, RASTA_CONFIG_KEY_ACCEPTED_VERSIONS);
    if (accepted_version.type == DICTIONARY_ARRAY){
        for (int i = 0; i < accepted_version.value.array.count; ++i) {
            if (cmp_version(accepted_version.value.array.data[i].c, version) == 0){
                // match, version is in accepted version list
                return 1;
            }
        }
    }*/
    for (unsigned int i = 0; i < h->accepted_version.count; ++i) {
        if (compare_version(h->accepted_version.data[i].c, version) == 0){
            // match, version is in accepted version list
            return 1;
        }
    }
    return 1;

    // otherwise (something with config went wrong or version was not in accepted versions) return 0
    return 0;
}

/**
 * send a DiscReq to the specified host
 * @param connection the connection which should be used
 * @param reason the reason for the disconnect
 * @param details detailed information about the disconnect
 * @param host the host where the DiscReq will be sent to
 * @param port the port where the DiscReq will be sent to
 */
void send_DisconnectionRequest(redundancy_mux *mux, struct rasta_connection * connection, rasta_disconnect_reason reason, unsigned short details){
    struct RastaDisconnectionData disconnectionData;
    disconnectionData.reason = (unsigned short)reason;
    disconnectionData.details = details;

    struct RastaPacket discReq = createDisconnectionRequest(connection->remote_id, connection->my_id,
                                                            connection->sn_t, connection->cs_t,
                                                            cur_timestamp(), connection->ts_r, disconnectionData, &mux->sr_hashing_context);

    redundancy_mux_send(mux, discReq);

    freeRastaByteArray(&discReq.data);
}

/**
 * send a HB to the specified host
 * @param connection the connection which should be used
 * @param host the host where the HB will be sent to
 * @param port the port where the HB will be sent to
 */
void send_Heartbeat(redundancy_mux *mux, struct rasta_connection * connection, char reschedule_manually){
    struct RastaPacket hb = createHeartbeat(connection->remote_id, connection->my_id, connection->sn_t,
                                            connection->cs_t, cur_timestamp(), connection->ts_r, &mux->sr_hashing_context);

    redundancy_mux_send(mux, hb);

    connection->sn_t = connection->sn_t +1;
    if (reschedule_manually) {
        reschedule_event(&connection->send_heartbeat_event);
    }
}

void send_RetransmissionRequest(redundancy_mux *mux, struct rasta_connection * connection){
    struct RastaPacket retrreq = createRetransmissionRequest(connection->remote_id, connection->my_id,
                                                             connection->sn_t, connection->cs_t, cur_timestamp(),
                                                             connection->ts_r, &mux->sr_hashing_context);

    redundancy_mux_send(mux, retrreq);

    connection->sn_t = connection->sn_t + 1;
}

void send_RetransmissionResponse(redundancy_mux *mux, struct rasta_connection * connection) {
    struct RastaPacket retrresp = createRetransmissionResponse(connection->remote_id, connection->my_id,
                                                               connection->sn_t, connection->cs_t, cur_timestamp(),
                                                               connection->ts_r, &mux->sr_hashing_context);

    redundancy_mux_send(mux, retrresp);
    connection->sn_t = connection->sn_t + 1;
}




unsigned int sr_retr_data_available(struct logger_t *logger,struct rasta_connection * connection){
    (void)logger;
    return fifo_get_size(connection->fifo_retr);
}

unsigned int sr_rasta_send_data_available(struct logger_t *logger,struct rasta_connection * connection){
    (void)logger;
    return fifo_get_size(connection->fifo_send);
}

void updateTI(long confirmed_timestamp, struct rasta_connection * con, struct RastaConfigInfoSending cfg) {
    unsigned long t_local = cur_timestamp();
    unsigned long t_rtd = t_local + (1000 / sysconf(_SC_CLK_TCK)) - confirmed_timestamp;
    con->t_i = (uint32_t )(cfg.t_max - t_rtd);

    // update the timeout start time
    reschedule_event(&con->timeout_event);
}

void resetDiagnostic(struct rasta_connection * connection) {
    for (unsigned int i = 0; i < connection->diagnostic_intervals_length; i++) {
        connection->diagnostic_intervals[i].message_count = 0;
        connection->diagnostic_intervals[i].t_alive_message_count = 0;
    }
}

void updateDiagnostic(struct rasta_connection * connection, struct RastaPacket receivedPacket, struct RastaConfigInfoSending cfg, struct rasta_handle *h) {
    unsigned long t_local = cur_timestamp();
    unsigned long t_rtd = t_local + (1000 / sysconf(_SC_CLK_TCK)) - receivedPacket.confirmed_timestamp;
    unsigned long t_alive = t_local - connection->cts_r;
    for (unsigned int i = 0; i < connection->diagnostic_intervals_length; i++) {
        if (connection->diagnostic_intervals[i].interval_start >= t_rtd && connection->diagnostic_intervals[i].interval_end <= t_rtd) {
            // found the sub interval this message can be assigned to
            ++connection->diagnostic_intervals[i].message_count;

            // lies t_alive in interval range, too?
            if (connection->diagnostic_intervals[i].interval_start >= t_alive && connection->diagnostic_intervals[i].interval_end <= t_alive) {
                ++connection->diagnostic_intervals[i].t_alive_message_count;
            }
            break; // we found our interval. Other executions aren't necessary anymore
        }
    }
    ++connection->received_diagnostic_message_count;
    if (connection->received_diagnostic_message_count >= cfg.diag_window) {
        fire_on_diagnostic_notification(sr_create_notification_result(h,connection));
        resetDiagnostic(connection);
    }
}

/**
 * Converts a unsigned long into a uchar array
 * @param v the uchar array
 * @param result the assigned uchar array; length should be 4
 */
void longToBytes2(unsigned long v, unsigned char* result) {
    result[0] = (unsigned char) (v >> 24 & 0xFF);
    result[1] = (unsigned char) (v >> 16 & 0xFF);
    result[2] = (unsigned char) (v >> 8 & 0xFF);
    result[3] = (unsigned char) (v & 0xFF);
}

/**
 * Converts a uchar array to a ulong
 * @param v the uchar array
 * @return the ulong
 */
uint32_t bytesToLong2(const unsigned char v[4]) {
    uint32_t result = 0;
    result = (v[0] << 24) + (v[1] << 16) + (v[2] << 8) + v[3];
    return result;
}

void sr_add_app_messages_to_buffer(struct rasta_receive_handle *h, struct rasta_connection * con, struct RastaPacket packet){
    struct RastaMessageData received_data;
    received_data = extractMessageData(packet);

    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA add to buffer", "received %d application messages", received_data.count);


    for (unsigned int i = 0; i < received_data.count; ++i) {
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA add to buffer", "received msg: %s", received_data.data_array[i].bytes);

        rastaApplicationMessage * elem = rmalloc(sizeof(rastaApplicationMessage));
        elem->id = packet.sender_id;
        allocateRastaByteArray(&elem->appMessage, received_data.data_array[i].length);

        rmemcpy(elem->appMessage.bytes, received_data.data_array[i].bytes, received_data.data_array[i].length);
        fifo_push(con->fifo_app_msg, elem);
        // fire onReceive event
        fire_on_receive(sr_create_notification_result(h->handle,con));

        updateTI(packet.confirmed_timestamp, con,h->config);
        updateDiagnostic(con,packet,h->config,h->handle);
    }
}

/**
* removes all confirmed messages from the retransmission fifo
* @param con the connection that is used
*/
void sr_remove_confirmed_messages(struct rasta_receive_handle *h,struct rasta_connection * con){
    // remove confirmed messages from retransmission fifo
    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA remove confirmed", "confirming messages with SN_PDU <= %lu", (long unsigned int) con->cs_r);

    struct RastaByteArray * elem;
    while ((elem = fifo_pop(con->fifo_retr)) != NULL){
        struct RastaPacket packet = bytesToRastaPacket(*elem, h->hashing_context);
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA remove confirmed", "removing packet with sn = %lu",
                   (long unsigned int) packet.sequence_number);


        // message is confirmed when CS_R - SN_PDU >= 0
        // equivalent to SN_PDU <= CS_R
        if (packet.sequence_number == con->cs_r){
            // this packet has the last same sequence number as the confirmed sn, i.e. the next packet in the queue's
            // SN_PDU will be bigger than CS_R (because of FIFO property of mqueue)
            // that means we removed all confirmed messages and have to leave the loop to stop removing packets
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA remove confirmed", "last confirmed packet removed");

            freeRastaByteArray(elem);
            freeRastaByteArray(&packet.data);
            rfree(elem);

            break;
        }

        freeRastaByteArray(elem);
        freeRastaByteArray(&packet.data);
        rfree(elem);
    }
}

/* ----- processing of received packet types ----- */

/**
 * calculates cts_in_seq for the given @p packet
 * @param con the connection that is used
 * @param packet the packet
 * @return cts_in_seq (bool)
 */
int sr_cts_in_seq(struct rasta_connection* con, struct RastaConfigInfoSending cfg, struct RastaPacket packet){

    if (packet.type == RASTA_TYPE_HB || packet.type == RASTA_TYPE_DATA || packet.type == RASTA_TYPE_RETRDATA){
        // Workaround rs 05.04.22
        // what should happen if cts_r is 0 (i.e. no packet received yet)
        if (con->cts_r == 0) {
            return 1;
        }

        // cts_in_seq := 0 <= CTS_PDU - CTS_R < t_i
        if (packet.confirmed_timestamp < con->cts_r) {
            return 0;
        }
        return (packet.confirmed_timestamp - con->cts_r) < cfg.t_max;
    } else {
        // for any other type return always true
        return 1;
    }
}

/**
 * calculates sn_in_seq for the given @p packet
 * @param con the connection that is used
 * @param packet the packet
 * @return sn_in_seq (bool)
 */
int sr_sn_in_seq(struct rasta_connection * con, struct RastaPacket packet){
    if (packet.type == RASTA_TYPE_CONNREQ || packet.type == RASTA_TYPE_CONNRESP ||
        packet.type == RASTA_TYPE_RETRRESP || packet.type == RASTA_TYPE_DISCREQ){
        // return always true
        return 1;
    } else{
        // check sn_in_seq := sn_r == sn_pdu
        return (con->sn_r == packet.sequence_number);
    }

}

/**
 * Checks the sequence number range as in 5.5.3.2
 * @param con connection that is used
 * @param packet the packet
 * @return 1 if the sequency number of the @p packet is in range
 */
int sr_sn_range_valid(struct rasta_connection * con, struct RastaConfigInfoSending cfg,  struct RastaPacket packet){
    // for types ConReq, ConResp and RetrResp return true
    if (packet.type == RASTA_TYPE_CONNREQ || packet.type == RASTA_TYPE_CONNRESP || packet.type == RASTA_TYPE_RETRRESP){
        return 1;
    }

    // else
    // seq. nr. in range when 0 <= SN_PDU - SN_R <= N_SENDMAX * 10
    return ((packet.sequence_number >= con->sn_r) &&
            (packet.sequence_number - con->sn_r) <= (cfg.send_max * 10));
}

/**
 * checks the confirmed sequence number integrity as in 5.5.4
 * @param con connection that is used
 * @param packet the packet
 * @return 1 if the integrity of the confirmed sequency number is confirmed, 0 otherwise
 */
int sr_cs_valid(struct rasta_connection * con, struct RastaPacket packet){
    if (packet.type == RASTA_TYPE_CONNREQ){
        // initial CS_PDU has to be 0
        return (packet.confirmed_sequence_number == 0);
    } else if (packet.type == RASTA_TYPE_CONNRESP){
        // has to be identical to last used (sent) seq. nr.
        return (packet.confirmed_sequence_number == (con->sn_t - 1));
    } else{
        // 0 <= CS_PDU - CS_R < SN_T - CS_R
        return ((packet.confirmed_sequence_number >= con->cs_r) &&
                (packet.confirmed_sequence_number - con->cs_r) < (con->sn_t - con->cs_r));
    }
}

/**
 * checks the packet authenticity as in 5.5.2 2)
 * @param con connection that is used
 * @param packet the packet
 * @return 1 if sender and receiver of the @p packet are authentic, 0 otherwise
 */
int sr_message_authentic(struct rasta_connection * con, struct RastaPacket packet){
    return (packet.sender_id == con->remote_id && packet.receiver_id == con->my_id);
}

int sr_check_packet(struct rasta_connection *con, struct logger_t *logger, struct RastaConfigInfoSending cfg, struct RastaPacket receivedPacket, char* location) {
    // check received packet (5.5.2)
    if (!(receivedPacket.checksum_correct &&
          sr_message_authentic(con, receivedPacket) &&
          sr_sn_range_valid(con,cfg, receivedPacket) &&
          sr_cs_valid(con, receivedPacket) &&
          sr_sn_in_seq(con, receivedPacket) &&
          sr_cts_in_seq(con, cfg, receivedPacket))){
        // something is invalid -> connection failure
        logger_log(logger, LOG_LEVEL_INFO, location, "received packet invalid");

        logger_log(logger, LOG_LEVEL_DEBUG, location, "checksum = %d", receivedPacket.checksum_correct);
        logger_log(logger, LOG_LEVEL_DEBUG, location, "authentic = %d", sr_message_authentic(con, receivedPacket));
        logger_log(logger, LOG_LEVEL_DEBUG, location, "sn_range_valid = %d", sr_sn_range_valid(con,cfg, receivedPacket));
        logger_log(logger, LOG_LEVEL_DEBUG, location, "cs_valid = %d", sr_cs_valid(con, receivedPacket));
        logger_log(logger, LOG_LEVEL_DEBUG, location, "sn_in_seq = %d", sr_sn_in_seq(con, receivedPacket));
        logger_log(logger, LOG_LEVEL_DEBUG, location, "cts_in_seq = %d", sr_cts_in_seq(con,cfg, receivedPacket));



        return 0;
    }

    return 1;
}

void sr_reset_connection(struct rasta_connection* connection, unsigned long id, struct RastaConfigInfoGeneral info) {
    connection->remote_id = (uint32_t )id;
    connection->current_state = RASTA_CONNECTION_CLOSED;
    connection->my_id = (uint32_t )info.rasta_id;
    connection->network_id = (uint32_t )info.rasta_network;
    connection->connected_recv_buffer_size = -1;
    connection->hb_locked = 1;
    connection->hb_stopped = 0;

    // set all error counters to 0
    struct rasta_error_counters error_counters;
    error_counters.address = 0;
    error_counters.cs = 0;
    error_counters.safety= 0;
    error_counters.sn = 0;
    error_counters.type = 0;

    connection->errors = error_counters;
}

void sr_close_connection(struct rasta_connection * connection,struct rasta_handle *handle, redundancy_mux *mux,
        struct RastaConfigInfoGeneral info, rasta_disconnect_reason reason, unsigned short details){
    if (connection->current_state == RASTA_CONNECTION_DOWN || connection->current_state == RASTA_CONNECTION_CLOSED){
        sr_reset_connection(connection,connection->remote_id,info);

        // fire connection tls_state changed event
        fire_on_connection_state_change(sr_create_notification_result(handle, connection));
    } else{
        // need to send DiscReq
        sr_reset_connection(connection,connection->remote_id,info);
        send_DisconnectionRequest(mux,connection, reason, details);

        connection->current_state = RASTA_CONNECTION_CLOSED;

        // fire connection tls_state changed event
        fire_on_connection_state_change(sr_create_notification_result(handle, connection));
    }
}

void sr_diagnostic_interval_init(struct rasta_connection * connection, struct RastaConfigInfoSending cfg) {
    connection->received_diagnostic_message_count = 0;

    unsigned int diagnostic_interval_length = cfg.t_max / DIAGNOSTIC_INTERVAL_SIZE;
    if (cfg.t_max % DIAGNOSTIC_INTERVAL_SIZE > 0) {
        ++diagnostic_interval_length;
    }
    connection->diagnostic_intervals = rmalloc(diagnostic_interval_length * sizeof(struct diagnostic_interval));
    connection->diagnostic_intervals_length = diagnostic_interval_length;
    for (unsigned int i = 0; i < diagnostic_interval_length; i++) {
        struct diagnostic_interval sub_interval;

        sub_interval.interval_start = DIAGNOSTIC_INTERVAL_SIZE * i;
        // last interval_end could be greater than T_MAX but we don't care. Every connection will be closed when you exceed T_MAX
        sub_interval.interval_end = sub_interval.interval_start + DIAGNOSTIC_INTERVAL_SIZE;
        sub_interval.message_count = 0;
        sub_interval.t_alive_message_count = 0;

        connection->diagnostic_intervals[i] = sub_interval;
    }
}

void sr_init_connection(struct rasta_connection* connection, unsigned long id, struct RastaConfigInfoGeneral info, struct RastaConfigInfoSending cfg, struct logger_t *logger, rasta_role role) {
    (void)logger;
    sr_reset_connection(connection,id,info);
    connection->role = role;

    // initalize diagnostic interval and store it in connection
    sr_diagnostic_interval_init(connection, cfg);

    // create receive queue
    connection->fifo_app_msg = fifo_init(cfg.send_max);

    // init retransmission fifo
    connection->fifo_retr = fifo_init(MAX_QUEUE_SIZE);

    // create send queue
    connection->fifo_send = fifo_init(2* cfg.max_packet);

    // reset last rekeying time
#ifdef ENABLE_OPAQUE
    connection->kex_state.last_key_exchanged_millis = 0;
#endif
}

void sr_retransmit_data(struct rasta_receive_handle *h, struct rasta_connection * connection){
    /**
         *  * retransmit messages in queue
         */

    // prepare Array Buffer
    struct RastaByteArray packets[MAX_QUEUE_SIZE];

    int buffer_n = 0; // how many valid elements are in the buffer
    buffer_n = fifo_get_size(connection->fifo_retr);

    // get all packets and store them in the buffer
    for (int j = 0; j < buffer_n; ++j) {
        struct RastaByteArray * element;
        element = fifo_pop(connection->fifo_retr);

        allocateRastaByteArray(&packets[j], element->length);
        rmemcpy(packets[j].bytes, element->bytes, element->length);

        freeRastaByteArray(element);
        rfree(element);
    }

    // re-open fifo in write mode
    // now retransmit each packet in the buffer with new sequence numbers
    for (int i = 0; i <= buffer_n; i++)
    {
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA retransmission", "retransmit packet %d", i);

        // retrieve retransmission data to
        struct RastaPacket old_p = bytesToRastaPacket(packets[i], h->hashing_context);
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA retransmission", "convert packet %d to packet structure", i);

        struct RastaMessageData app_messages = extractMessageData(old_p);
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA retransmission", "extract data from packet %d ", i);

        // create new packet for retransmission
        struct RastaPacket data = createRetransmittedDataMessage(connection->remote_id, connection->my_id, connection->sn_t,
                                                                 connection->cs_t, cur_timestamp(), connection->cts_r,
                                                                 app_messages, h->hashing_context);
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA retransmission", "created retransmission packet %d ", i);

        struct RastaByteArray new_p = rastaModuleToBytes(data, h->hashing_context);

        // add packet to retrFifo again
        struct RastaByteArray * to_fifo = rmalloc(sizeof(struct RastaByteArray));
        allocateRastaByteArray(to_fifo, new_p.length);
        rmemcpy(to_fifo->bytes, new_p.bytes, new_p.length);
        fifo_push(connection->fifo_retr, to_fifo);
        logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA retransmission", "added packet %d to queue", i);

        // send packet
        redundancy_mux_send(h->mux, data);
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA retransmission", "retransmitted packet with old sn=%lu",
            (long unsigned int) old_p.sequence_number);

        // increase sn_t
        connection->sn_t = connection->sn_t +1;

        // set last message ts
        reschedule_event(&connection->send_heartbeat_event);

        // free allocated memory of current packet
        freeRastaByteArray(&packets[i]);
        freeRastaByteArray(&new_p);
        freeRastaByteArray(&old_p.data);
    }

    // close retransmission with heartbeat
    send_Heartbeat(h->mux,connection, 1);
}

int event_connection_expired(void* carry_data);
void init_connection_timeout_event(timed_event* ev, struct timed_event_data* carry_data,
        struct rasta_connection* connection, struct rasta_handle* h) {
    memset(ev, 0, sizeof(timed_event));
    ev->callback = event_connection_expired;
    ev->carry_data = carry_data;
    ev->interval = h->heartbeat_handle->config.t_max * 1000000lu;
    carry_data->handle = h->heartbeat_handle;
    carry_data->connection = connection;
    enable_timed_event(ev);
}

int heartbeat_send_event(void* carry_data);
void init_send_heartbeat_event(timed_event* ev, struct timed_event_data* carry_data,
        struct rasta_connection* connection, struct rasta_handle* h) {
    memset(ev, 0, sizeof(timed_event));
    ev->callback = heartbeat_send_event;
    ev->carry_data = carry_data;
    ev->interval = h->heartbeat_handle->config.t_h * 1000000lu;
    carry_data->handle = h->heartbeat_handle;
    carry_data->connection = connection;
    enable_timed_event(ev);
}

int send_timed_key_exchange(void *arg);
void init_send_key_exchange_event(timed_event* ev, struct timed_event_data* carry_data,
                                  struct rasta_connection* connection, struct rasta_handle* h) {
    ev->callback = send_timed_key_exchange;
    ev->carry_data = carry_data;
    ev->interval = h->config.values.kex.rekeying_interval_ms  * NS_PER_MS;
    // add some headroom for computation and communication
    ev->interval /= 2;
    carry_data->handle = h->receive_handle;
    carry_data->connection = connection;
    enable_timed_event(ev);
}

void init_connection_events(struct rasta_handle* h, struct rasta_connection* connection) {
    init_connection_timeout_event(&connection->timeout_event, &connection->timeout_carry_data, connection, h);
    init_send_heartbeat_event(&connection->send_heartbeat_event, &connection->timeout_carry_data, connection, h);
    add_timed_event(h->ev_sys, &connection->timeout_event);
    add_timed_event(h->ev_sys, &connection->send_heartbeat_event);
#ifdef ENABLE_OPAQUE
    if(connection->role == RASTA_ROLE_CLIENT && h->config.values.kex.rekeying_interval_ms) {
        init_send_key_exchange_event(&connection->rekeying_event, &connection->rekeying_carry_data, connection, h);
        add_timed_event(h->ev_sys, &connection->rekeying_event);
    }
#endif
}

void add_connection_to_list(struct rasta_handle* h, struct rasta_connection* con) {
    if (h->last_con) {
        con->linkedlist_prev = h->last_con;
        con->linkedlist_next = NULL;
        h->last_con->linkedlist_next = con;
    }
    else {
        h->first_con = con;
        h->last_con = con;
        con->linkedlist_prev = NULL;
        con->linkedlist_next = NULL;
    }
}

void remove_connection_from_list(struct rasta_handle* h, struct rasta_connection* con) {
    if (h->first_con == con) {
        h->first_con = con->linkedlist_next;
    }
    if (h->last_con == con) {
        h->last_con = con->linkedlist_prev;
    }
    if (con->linkedlist_prev) con->linkedlist_prev->linkedlist_next = con->linkedlist_next;
    if (con->linkedlist_next) con->linkedlist_next->linkedlist_prev = con->linkedlist_prev;
}

/*
 * Handle packet functions
 */

static uint64_t get_current_time_ms() {
    uint64_t current_time;
    struct timespec current_time_tv;
    clock_gettime(CLOCK_MONOTONIC, &current_time_tv);

    current_time = current_time_tv.tv_nsec/NS_PER_MS + current_time_tv.tv_sec * MS_PER_S;
    return current_time;
}

/**
 * send a Key Exchange Request to the specified host
 * @param connection the connection which should be used
 * @param host the host where the HB will be sent to
 * @param port the port where the HB will be sent to
 */
void send_KexRequest(redundancy_mux *mux, struct rasta_connection * connection, struct rasta_receive_handle *h){
#ifdef ENABLE_OPAQUE
    struct RastaPacket hb = createKexRequest(connection->remote_id, connection->my_id, connection->sn_t,
                                             connection->cs_t, cur_timestamp(), connection->ts_r,
                                             &mux->sr_hashing_context, h->handle->config.values.kex.psk, &connection->kex_state, h->logger);

    if(!connection->kex_state.last_key_exchanged_millis && h->handle->config.values.kex.rekeying_interval_ms){
        // first key exchanged - need to enable periodic rekeying
        init_send_key_exchange_event(&connection->rekeying_event,&connection->rekeying_carry_data,connection,h->handle);
        add_timed_event(h->handle->ev_sys, &connection->rekeying_event);
    }
    else{
        logger_log(h->logger,LOG_LEVEL_INFO,"RaSTA KEX", "Rekeying at %"PRIu64,get_current_time_ms());
    }

    redundancy_mux_send(mux, hb);

    connection->sn_t = connection->sn_t +1;

    connection->kex_state.last_key_exchanged_millis = get_current_time_ms();
    connection->current_state = RASTA_CONNECTION_KEX_RESP;
#else
    // should never be called
    (void) mux;
    (void) connection;
    (void) h;
    abort();
#endif
}

int send_timed_key_exchange(void *arg){
#ifdef ENABLE_OPAQUE
    struct timed_event_data *event_data = (struct timed_event_data *) arg;
    struct rasta_receive_handle *handle = (struct rasta_receive_handle *) event_data->handle;
    send_KexRequest(handle->mux,event_data->connection,handle);
    // call periodically
    reschedule_event(&event_data->connection->rekeying_event);
#else
    // should never be called
    (void) arg;
#endif
    return 0;
}

struct rasta_connection* handle_conreq(struct rasta_receive_handle *h, struct rasta_connection* connection, struct RastaPacket receivedPacket) {
    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Received ConnectionRequest from %d", receivedPacket.sender_id);
    //struct rasta_connection* con = rastalist_getConnectionByRemote(&h->connections, receivedPacket.sender_id);
    if (connection == 0 || connection->current_state == RASTA_CONNECTION_CLOSED || connection->current_state == RASTA_CONNECTION_DOWN) {
        //new client
        if (connection == 0) {
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Prepare new client");
        }
        else {
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Reset existing client");
        }
        struct rasta_connection new_con;

        sr_init_connection(&new_con,receivedPacket.sender_id,h->info,h->config,h->logger, RASTA_ROLE_SERVER);

        // initialize seq num
        new_con.sn_t = new_con.sn_i = receivedPacket.sequence_number;
        //new_con.sn_t = 55;
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Using %lu as initial sequence number",
            (long unsigned int) new_con.sn_t);

        new_con.current_state = RASTA_CONNECTION_DOWN;

        // check received packet (5.5.2)
        if (!sr_check_packet(&new_con,h->logger,h->config,receivedPacket, "RaSTA HANDLE: ConnectionRequest")) {
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Packet is not valid");
            sr_close_connection(&new_con,h->handle,h->mux,h->info,RASTA_DISC_REASON_PROTOCOLERROR,0);
            return connection;
        }

        // received packet is a ConReq -> check version
        struct RastaConnectionData connectionData = extractRastaConnectionData(receivedPacket);

        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Client has version %s", connectionData.version);

        if (compare_version(RASTA_VERSION, connectionData.version) == 0 ||
            compare_version(RASTA_VERSION, connectionData.version) == -1 ||
            version_accepted(h, connectionData.version)) {

            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Version accepted");

            // same version, or lower version -> client has to decide -> send ConResp

            // set values according to 5.6.2 [3]
            new_con.sn_r = receivedPacket.sequence_number + 1;
            new_con.cs_t = receivedPacket.sequence_number;
            new_con.ts_r = receivedPacket.timestamp;
            new_con.cts_r = receivedPacket.confirmed_timestamp;
            new_con.cs_r = receivedPacket.confirmed_sequence_number;

            // save N_SENDMAX of partner
            new_con.connected_recv_buffer_size = connectionData.send_max;

            new_con.t_i = h->config.t_max;

            unsigned char *version = (unsigned char *) RASTA_VERSION;

            // send ConResp
            struct RastaPacket conresp = createConnectionResponse(new_con.remote_id, new_con.my_id,
                                                                  new_con.sn_t, new_con.cs_t,
                                                                  cur_timestamp(), new_con.cts_r,
                                                                  h->config.send_max,
                                                                  version, h->hashing_context);

            //printf("SENDING ConResp with SN_T=%lu\n", conresp.sequence_number);
            new_con.sn_t = new_con.sn_t + 1;

            new_con.current_state = RASTA_CONNECTION_START;

            //check if the connection was just closed
            if (connection) {
                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Update Client %d", receivedPacket.sender_id);
                *connection = new_con;
                fire_on_connection_state_change(sr_create_notification_result(h->handle, connection));
                init_connection_events(h->handle, connection);
            }
            else {
                struct rasta_connection* memory = h->handle->user_handles->on_connection_start(&new_con);
                if (memory == NULL) {
                    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: ConnectionRequest", "refused %d", receivedPacket.sender_id);
                    return NULL;
                }
                *memory = new_con;
                add_connection_to_list(h->handle, memory);
                fire_on_connection_state_change(sr_create_notification_result(h->handle, memory));
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: ConnectionRequest", "Add new client %d", receivedPacket.sender_id);

                init_connection_events(h->handle, memory);
            }

            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Send Connection Response - waiting for Heartbeat");
            redundancy_mux_send(h->mux, conresp);

            freeRastaByteArray(&conresp.data);
        }
        else {
            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: ConnectionRequest", "Version unacceptable - sending DisconnectionRequest");
            sr_close_connection(&new_con,h->handle,h->mux,h->info,RASTA_DISC_REASON_INCOMPATIBLEVERSION,0);
            return connection;
        }
    }
    else {
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Connection is in invalid tls_state (%d) send DisconnectionRequest",connection->current_state);
        sr_close_connection(connection,h->handle,h->mux,h->info,RASTA_DISC_REASON_UNEXPECTEDTYPE,0);
    }
    return connection;
}

struct rasta_connection* handle_conresp(struct rasta_receive_handle *h, struct rasta_connection* con, struct RastaPacket receivedPacket) {

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Received ConnectionResponse from %d", receivedPacket.sender_id);

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Checking packet..");
    if (!sr_check_packet(con,h->logger,h->config,receivedPacket,"RaSTA HANDLE: ConnectionResponse")) {
        sr_close_connection(con,h->handle,h->mux,h->info, RASTA_DISC_REASON_PROTOCOLERROR, 0);
        return con;
    }

    if (con->current_state == RASTA_CONNECTION_START) {
        if (con->role == RASTA_ROLE_CLIENT) {
            //handle normal conresp
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Current tls_state is in order");

            // correct type of packet received -> version check
            struct RastaConnectionData connectionData = extractRastaConnectionData(receivedPacket);

            //logger_log(&connection->logger, LOG_LEVEL_INFO, "RaSTA open con", "server is running RaSTA version %s", connectionData.version);

            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Client has version %s",connectionData.version);

            if (version_accepted(h, connectionData.version)) {

                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Version accepted");

                // same version or accepted versions -> send hb to complete handshake

                // set values according to 5.6.2 [3]
                con->sn_r = receivedPacket.sequence_number + 1;
                con->cs_t = receivedPacket.sequence_number;
                con->ts_r = receivedPacket.timestamp;
                con->cs_r = receivedPacket.confirmed_sequence_number;

                //printf("RECEIVED CS_PDU=%lu (Type=%d)\n", receivedPacket.sequence_number, receivedPacket.type);

                // update tls_state, ready to send data
                con->current_state = RASTA_CONNECTION_UP;

                // send hb
                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Sending heartbeat..");
                send_Heartbeat(h->mux, con, 1);

#ifdef ENABLE_OPAQUE
                if(h->handle->config.values.kex.mode == KEY_EXCHANGE_MODE_OPAQUE) {
                    send_KexRequest(h->mux, con, h);
                }
#endif

                // fire connection tls_state changed event
                fire_on_connection_state_change(sr_create_notification_result(h->handle, con));
                // fire handshake complete event
                fire_on_handshake_complete(sr_create_notification_result(h->handle, con));

                init_connection_events(h->handle, con);

                // start sending heartbeats
                enable_timed_event(&con->send_heartbeat_event);

                con->hb_locked = 0;

                // save the N_SENDMAX of remote
                con->connected_recv_buffer_size = connectionData.send_max;

                // arm the timeout timer
                enable_timed_event(&con->timeout_event);

            } else {
                // version not accepted -> disconnect
                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Version not acceptable - send DisonnectionRequest");
                sr_close_connection(con,h->handle,h->mux,h->info,RASTA_DISC_REASON_INCOMPATIBLEVERSION, 0);
                return con;
            }
        }
        else {
            //Server don't receive conresp
            sr_close_connection(con, h->handle, h->mux, h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);

            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Server received ConnectionResponse - send Disconnection Request");
            return con;
        }
    }
    else if (con->current_state == RASTA_CONNECTION_RETRREQ || con->current_state == RASTA_CONNECTION_RETRRUN || con->current_state == RASTA_CONNECTION_UP) {
        sr_close_connection(con,h->handle,h->mux,h->info,RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Received ConnectionResponse in wrong tls_state - semd DisconnectionRequest");
        return con;
    }
    return con;
}

/**
 * processes a received Key Exchange Request packet
 * @param con the used connection
 * @param packet the received Key Exchange Request packet
 */
void handle_kex_request(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket){
    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Key Exchange Request", "received Data");

    if (sr_sn_in_seq(connection, receivedPacket)){
        if (connection->current_state != RASTA_CONNECTION_KEX_REQ && (!h->handle->config.values.kex.rekeying_interval_ms || connection->current_state != RASTA_CONNECTION_UP)){
            // received Key Exchange Request in the wrong phase -> disconnect and close
            sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
        } else{
            // sn_in_seq == true -> check cts_in_seq

            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Req", "SN in SEQ");

            if (sr_cts_in_seq(connection,h->config, receivedPacket)){
                if(h->handle->config.values.kex.mode == KEY_EXCHANGE_MODE_NONE){
                    logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Req","Key exchange request received when not activated!");
                    sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
                    return;
                }
#ifdef ENABLE_OPAQUE

                struct RastaPacket response;
                connection->kex_state.last_key_exchanged_millis = get_current_time_ms();

                if(connection->kex_state.last_key_exchanged_millis){
                    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Resp", "Accepted rekeying at %"PRIu64,connection->kex_state.last_key_exchanged_millis);
                }

                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Req", "CTS in SEQ");

                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Req","Key exchange request received at %"PRIu64,connection->kex_state.last_key_exchanged_millis);
                // valid Key Exchange request packet received

                response = createKexResponse(connection->remote_id, connection->my_id, connection->sn_t,
                                             receivedPacket.sequence_number, current_ts(),
                                             receivedPacket.timestamp, h->hashing_context, h->handle->config.values.kex.psk,
                                             (uint8_t *) receivedPacket.data.bytes, receivedPacket.data.length, connection->sn_i,
                                             &connection->kex_state,
                                             &h->handle->config.values.kex, h->logger);

                redundancy_mux_send(h->mux, response);

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket.sequence_number +1;
                connection->sn_t += 1;
                connection->cs_t = receivedPacket.sequence_number;
                connection->cs_r = receivedPacket.confirmed_sequence_number;
                connection->ts_r = receivedPacket.timestamp;
                connection->cts_r = receivedPacket.confirmed_timestamp;
                // con->cts_r = current_timestamp();

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(h,connection);

                // wait for client to send auth packet, indicating that on the client's side, the exchange worked
                connection->current_state = RASTA_CONNECTION_KEX_AUTH;

                logger_hexdump(h->logger,LOG_LEVEL_INFO,connection->kex_state.session_key,sizeof(connection->kex_state.session_key),"Setting hash key to:");

                rasta_set_hash_key_variable(h->hashing_context, (char *) connection->kex_state.session_key, sizeof(connection->kex_state.session_key));

#else
                logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Req", "Not implemented!");

                abort();
#endif


            } else{
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Kex", "CTS not in SEQ");

                // increase cs error counter
                connection->errors.cs++;

                // send DiscReq and close connection
                sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_PROTOCOLERROR ,0);
            }
        }
    } else{
        // received key exchange request in phase during which I should not have received one -> disconnect and close
        sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
    }
}

/**
 * processes a received Key Exchange Response packet
 * @param con the used connection
 * @param packet the received Key Exchange Response packet
 */
void handle_kex_response(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket){
    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Key Exchange Response", "received Data");

    if (sr_sn_in_seq(connection, receivedPacket)){
        if (connection->current_state != RASTA_CONNECTION_KEX_RESP){
            // received Key Exchange Response in the wrong phase -> disconnect and close
            sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
        } else{
            // sn_in_seq == true -> check cts_in_seq
            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Resp", "SN in SEQ");

            if (sr_cts_in_seq(connection,h->config, receivedPacket)){
                if(h->handle->config.values.kex.mode == KEY_EXCHANGE_MODE_NONE){
                    logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Resp","Key exchange response received when not activated!");
                    sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
                    return;
                }
#ifdef ENABLE_OPAQUE

                struct RastaPacket response;
                int ret;

                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Resp", "CTS in SEQ");
                // valid Key Exchange response packet received
                ret = kex_recover_credential(&connection->kex_state,(const uint8_t *) receivedPacket.data.bytes,receivedPacket.data.length,connection->my_id,connection->remote_id,connection->sn_i,h->logger);

                if(ret){
                    logger_log(h->logger,LOG_LEVEL_ERROR,"RaSTA HANDLE: KEX Resp", "Could not recover credentials!");
                    sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
                    return;
                }
                response = createKexAuthentication(connection->remote_id,connection->my_id,connection->sn_t,receivedPacket.sequence_number,current_ts(),receivedPacket.timestamp,h->hashing_context,connection->kex_state.user_auth_server,sizeof(connection->kex_state.user_auth_server),h->logger);

                redundancy_mux_send(h->mux, response);

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket.sequence_number +1;
                connection->sn_t += 1;
                connection->cs_t = receivedPacket.sequence_number;
                connection->cs_r = receivedPacket.confirmed_sequence_number;
                connection->ts_r = receivedPacket.timestamp;
                connection->cts_r = receivedPacket.confirmed_timestamp;
                // con->cts_r = current_timestamp();

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(h,connection);

                // kex is done from our PoV, can expect data from now
                connection->current_state = RASTA_CONNECTION_UP;

                logger_hexdump(h->logger,LOG_LEVEL_INFO,connection->kex_state.session_key,sizeof(connection->kex_state.session_key),"Setting hash key to:");

                rasta_set_hash_key_variable(h->hashing_context, (char *) connection->kex_state.session_key, sizeof(connection->kex_state.session_key));
#else
                logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Resp", "Not implemented!");
                abort();
#endif

            } else{
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Kex", "CTS not in SEQ");

                // increase cs error counter
                connection->errors.cs++;

                // send DiscReq and close connection
                sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_PROTOCOLERROR ,0);
            }
        }
    } else{
        // received key exchange response in phase during which I should not have received one -> disconnect and close
        sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
    }
}

/**
 * processes a received Key Exchange Authentication packet
 * @param con the used connection
 * @param packet the received Key Exchange Authentication packet
 */
void handle_kex_auth(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket){
    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Key Exchange Authentication", "received Data");

    if (sr_sn_in_seq(connection, receivedPacket)){
        if (connection->current_state != RASTA_CONNECTION_KEX_AUTH){
            // received Key Exchange Authentication in the wrong phase -> disconnect and close
            sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
        } else{
            // sn_in_seq == true -> check cts_in_seq

            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Auth", "SN in SEQ");

            if (sr_cts_in_seq(connection,h->config, receivedPacket)){

                if(h->handle->config.values.kex.mode == KEY_EXCHANGE_MODE_NONE){
                    logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Auth","Key exchange Authentication received when not activated!");
                    sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
                    return;
                }
#ifdef ENABLE_OPAQUE
                int ret;
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Auth", "CTS in SEQ");
                // valid Key Exchange Authentication packet received
                ret = kex_authenticate_user(&connection->kex_state,(const uint8_t *) receivedPacket.data.bytes,receivedPacket.data.length,h->logger);

                if(ret){
                    logger_log(h->logger,LOG_LEVEL_ERROR,"RaSTA HANDLE: KEX Auth", "Could not authenticate user");
                    sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
                    return;
                }

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket.sequence_number +1;
                connection->cs_t = receivedPacket.sequence_number;
                connection->cs_r = receivedPacket.confirmed_sequence_number;
                connection->ts_r = receivedPacket.timestamp;
                connection->cts_r = receivedPacket.confirmed_timestamp;
                // con->cts_r = current_timestamp();

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(h,connection);

                // kex is done from our PoV, can expect data from now
                connection->current_state = RASTA_CONNECTION_UP;
#else
                logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Auth", "Not implemented!");
                abort();
#endif

            } else{
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Kex", "CTS not in SEQ");

                // increase cs error counter
                connection->errors.cs++;

                // send DiscReq and close connection
                sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_PROTOCOLERROR ,0);
            }
        }
    } else{
        // received key exchange response in phase during which I should not have received one -> disconnect and close
        sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
    }
}

void handle_discreq(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket){
    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: DisconnectionRequest", "received DiscReq");

    connection->current_state = RASTA_CONNECTION_CLOSED;
    sr_reset_connection(connection,connection->remote_id,h->info);

    // remove redundancy channel
    redundancy_mux_remove_channel(h->mux, connection->remote_id);

    //struct RastaDisconnectionData disconnectionData = extractRastaDisconnectionData(receivedPacket);

    // fire connection tls_state changed event
    fire_on_connection_state_change(sr_create_notification_result(h->handle,connection));
    // fire disconnection request received event
    struct RastaDisconnectionData data= extractRastaDisconnectionData(receivedPacket);
    fire_on_discrequest_state_change(sr_create_notification_result(h->handle,connection),data);
}

void handle_hb(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket) {
    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Received heartbeat from %d", receivedPacket.sender_id);

    if (connection->current_state == RASTA_CONNECTION_START) {
        //heartbeat is for connection setup
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Establish connection");

        // if SN not in Seq -> disconnect and close connection
        if (!sr_sn_in_seq(connection, receivedPacket)){
            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Heartbeat", "Connection HB SN not in Seq");

            if (connection->role == RASTA_ROLE_SERVER){
                // SN not in Seq
                sr_close_connection(connection, h->handle, h->mux, h->info, RASTA_DISC_REASON_SEQNERROR,0);
            } else{
                // Client
                sr_close_connection(connection, h->handle, h->mux, h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE,0);
            }
        }

        // if client receives HB in START -> disconnect and close
        if (connection->role == RASTA_ROLE_CLIENT){
            sr_close_connection(connection, h->handle, h->mux, h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE,0);
        }

        if (sr_cts_in_seq(connection, h->config, receivedPacket)){
            // set values according to 5.6.2 [3]
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Heartbeat is valid connection successful");
            connection->sn_r = receivedPacket.sequence_number +1;
            connection->cs_t = receivedPacket.sequence_number;
            connection->cs_r = receivedPacket.confirmed_sequence_number;
            connection->ts_r = receivedPacket.timestamp;

            if(h->handle->config.values.kex.mode == KEY_EXCHANGE_MODE_NONE) {
                // sequence number correct, ready to receive data
                connection->current_state = RASTA_CONNECTION_UP;
            }
            else{
                // need to negotiate session key first
                connection->current_state = RASTA_CONNECTION_KEX_REQ;
            }

            connection->hb_locked = 0;

            // fire connection tls_state changed event
            fire_on_connection_state_change(sr_create_notification_result(h->handle,connection));
            // fire handshake complete event
            fire_on_handshake_complete(sr_create_notification_result(h->handle, connection));

            // start sending heartbeats
            enable_timed_event(&connection->send_heartbeat_event);

            // arm the timeout timer
            enable_timed_event(&connection->timeout_event);
            return;
        } else{
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Heartbeat is invalid");

            // sequence number check failed -> disconnect
            sr_close_connection(connection,h->handle,h->mux,h->info,RASTA_DISC_REASON_PROTOCOLERROR,0);
            return;
        }
    }

    if (sr_sn_in_seq(connection, receivedPacket)){
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "SN in SEQ");
        // heartbeats also permissible during key exchange phase, since computation could exceed heartbeat interval
        if (connection->current_state == RASTA_CONNECTION_UP || connection->current_state == RASTA_CONNECTION_RETRRUN || connection->current_state == RASTA_CONNECTION_KEX_REQ || connection->current_state == RASTA_CONNECTION_KEX_RESP || connection->current_state == RASTA_CONNECTION_KEX_AUTH){
            // check cts_in_seq
            if (sr_cts_in_seq(connection,h->config, receivedPacket)){
                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "CTS in SEQ");

                updateTI(receivedPacket.confirmed_timestamp, connection,h->config);
                updateDiagnostic(connection,receivedPacket,h->config,h->handle);

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket.sequence_number +1;
                connection->cs_t = receivedPacket.sequence_number;
                connection->cs_r = receivedPacket.confirmed_sequence_number;
                connection->ts_r = receivedPacket.timestamp;

                connection->cts_r = receivedPacket.confirmed_timestamp;
                if (connection->current_state == RASTA_CONNECTION_RETRRUN) {
                    connection->current_state = RASTA_CONNECTION_UP;
                    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "State changed from RetrRun to Up");
                    fire_on_connection_state_change(sr_create_notification_result(h->handle,connection));
                }
            } else{
                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "CTS not in SEQ - send DisconnectionRequest");
                // close connection
                sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_TIMEOUT, 0);
            }
        }
    } else{
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "SN not in SEQ");

        if (connection->current_state == RASTA_CONNECTION_UP || connection->current_state == RASTA_CONNECTION_RETRRUN){
            // ignore message, send RetrReq and goto tls_state RetrReq
            //TODO:send retransmission
            //send_retrreq(con);
            connection->current_state = RASTA_CONNECTION_RETRREQ;
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Send retransmission");

            // fire connection tls_state changed event
            fire_on_connection_state_change(sr_create_notification_result(h->handle,connection));
        }
    }
}

/**
 * processes a received Data packet
 * @param con the used connection
 * @param packet the received data packet
 */
void handle_data(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket){
    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "received Data");

    if (sr_sn_in_seq(connection, receivedPacket)){
        if (connection->current_state == RASTA_CONNECTION_START || connection->current_state == RASTA_CONNECTION_KEX_REQ ||connection->current_state == RASTA_CONNECTION_KEX_RESP || connection->current_state == RASTA_CONNECTION_KEX_AUTH){
            // received data in START or when key exchange still in progress-> disconnect and close
            sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
        } else if (connection->current_state == RASTA_CONNECTION_UP){
            // sn_in_seq == true -> check cts_in_seq

            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "SN in SEQ");

            if (sr_cts_in_seq(connection,h->config, receivedPacket)){
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "CTS in SEQ");
                // valid data packet received
                // read application messages and push into queue
                sr_add_app_messages_to_buffer(h,connection,receivedPacket);

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket.sequence_number +1;
                connection->cs_t = receivedPacket.sequence_number;
                connection->cs_r = receivedPacket.confirmed_sequence_number;
                connection->ts_r = receivedPacket.timestamp;
                connection->cts_r = receivedPacket.confirmed_timestamp;
                // con->cts_r = current_timestamp();

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(h,connection);


            } else{
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "CTS not in SEQ");

                // increase cs error counter
                connection->errors.cs++;

                // send DiscReq and close connection
                sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_PROTOCOLERROR ,0);
            }
        } else if (connection->current_state == RASTA_CONNECTION_RETRRUN){
            if (sr_cts_in_seq(connection, h->config, receivedPacket)){
                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket.sequence_number + 1;
                connection->cs_t = receivedPacket.sequence_number;
                connection->ts_r = receivedPacket.timestamp;
            } else{
                // retransmission failed, disconnect and close
                sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_PROTOCOLERROR ,0);
            }
        }
    } else{

        if (connection->current_state == RASTA_CONNECTION_START){
            // received data in START -> disconnect and close
            sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_UNEXPECTEDTYPE ,0);
        } else if (connection->current_state == RASTA_CONNECTION_RETRRUN || connection->current_state == RASTA_CONNECTION_UP){
            // increase SN error counter
            connection->errors.sn++;

            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "send retransmission request");
            // send RetrReq
            send_RetransmissionRequest(h->mux, connection);

            // change tls_state to RetrReq
            connection->current_state = RASTA_CONNECTION_RETRREQ;

            fire_on_connection_state_change(sr_create_notification_result(h->handle,connection));
        } else if (connection->current_state == RASTA_CONNECTION_RETRREQ){
            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "package is ignored - waiting for RETRResponse");
        }
    }
}

/**
 * processes a received RetrReq packet
 * @param con the used connection
 * @param packet the received RetrReq packet
 */
void handle_retrreq(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket){
    logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA receive", "received RetrReq");

    if (sr_sn_in_seq(connection,receivedPacket)){
        // sn_in_seq == true
        logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA receive", "RetrReq SNinSeq");


        if (connection->current_state == RASTA_CONNECTION_RETRRUN) {
            logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA receive", "RetrReq: got RetrReq packet in RetrRun mode. closing connection.");

            // send DiscReq to client
            sr_close_connection(connection,h->handle,h->mux,h->info,RASTA_DISC_REASON_RETRFAILED, 0);
            //printf("Connection closed / DiscReq sent!\n");
        }

        // FIXME: update connection attr (copy from RASTA_TYPE_DATA case, DRY)
        // set values according to 5.6.2 [3]
        connection->sn_r = receivedPacket.sequence_number +1;
        connection->cs_t = receivedPacket.sequence_number;
        connection->cs_r = receivedPacket.confirmed_sequence_number;
        connection->ts_r = receivedPacket.timestamp;

        // con->cts_r = current_timestamp();

        // cs_r updated, remove confirmed messages
        sr_remove_confirmed_messages(h,connection);

        // send retransmission response
        send_RetransmissionResponse(h->mux,connection);
        logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA receive", "send RetrRes");

        sr_retransmit_data(h,connection);

        if (connection->current_state == RASTA_CONNECTION_UP){
            // change tls_state to up
            connection->current_state = RASTA_CONNECTION_UP;
        } else if(connection->current_state == RASTA_CONNECTION_RETRREQ){
            // change tls_state to RetrReq
            connection->current_state = RASTA_CONNECTION_RETRREQ;
        }

        // fire connection tls_state changed event
        fire_on_connection_state_change(sr_create_notification_result(h->handle,connection));
    } else{
        // sn_in_seq == false
        connection->cs_r = receivedPacket.confirmed_sequence_number;

        // cs_r updated, remove confirmed messages
        sr_remove_confirmed_messages(h,connection);

        // send retransmission response
        send_RetransmissionResponse(h->mux, connection);
        logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA receive", "send RetrRes");

        sr_retransmit_data(h,connection);
        // change tls_state to RetrReq
        connection->current_state = RASTA_CONNECTION_RETRREQ;

        // fire connection tls_state changed event
        fire_on_connection_state_change(sr_create_notification_result(h->handle,connection));
    }
}


/**
 * processes a received RetrResp packet
 * @param con the used connection
 * @param packet the received RetrResp packet
 */
void handle_retrresp(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket){
    if (connection->current_state == RASTA_CONNECTION_RETRREQ) {
        logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA receive", "starting receive retransmitted data");
        // check cts_in_seq
        logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA receive", "RetrResp: CTS in Seq");

        // change to retransmission tls_state
        connection->current_state = RASTA_CONNECTION_RETRRUN;

        // set values according to 5.6.2 [3]
        connection->sn_r = receivedPacket.sequence_number +1;
        connection->cs_t = receivedPacket.sequence_number;
        connection->cs_r = receivedPacket.confirmed_sequence_number;
        connection->ts_r = receivedPacket.timestamp;

        connection->cts_r = receivedPacket.confirmed_timestamp;
    } else {
        logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA receive", "received packet type retr_resp, but not in tls_state retr_req");
        sr_close_connection(connection,h->handle,h->mux,h->info,RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
    }
}

/**
 * processes a received RetrData packet
 * @param con the used connection
 * @param packet the received data packet
 */
void handle_retrdata(struct rasta_receive_handle *h, struct rasta_connection *connection, struct RastaPacket receivedPacket){
    if (sr_sn_in_seq(connection,receivedPacket)){

        if (connection->current_state == RASTA_CONNECTION_UP){
            // close connection
            sr_close_connection(connection,h->handle,h->mux,h->info,RASTA_DISC_REASON_UNEXPECTEDTYPE,0);
        } else if (connection->current_state == RASTA_CONNECTION_RETRRUN){
            if (!sr_cts_in_seq(connection,h->config,receivedPacket)){
                // cts not in seq -> close connection
                sr_close_connection(connection,h->handle,h->mux,h->info,RASTA_DISC_REASON_PROTOCOLERROR,0);
            } else{
                //cts is in seq -> add data to receive buffer
                logger_log(h->logger, LOG_LEVEL_DEBUG, "Process RetrData", "CTS in seq, adding messages to buffer");
                sr_add_app_messages_to_buffer(h,connection,receivedPacket);

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket.sequence_number +1;
                connection->cs_t = receivedPacket.sequence_number;
                connection->cs_r = receivedPacket.confirmed_sequence_number;
                connection->ts_r = receivedPacket.timestamp;
            }
        }
    } else{
        // sn not in seq
        logger_log(h->logger, LOG_LEVEL_DEBUG, "Process RetrData", "SN not in Seq");
        logger_log(h->logger, LOG_LEVEL_DEBUG, "Process RetrData", "SN_PDU=%lu, SN_R=%lu",
            (long unsigned int) receivedPacket.sequence_number, (long unsigned int) connection->sn_r);
        if (connection->current_state == RASTA_CONNECTION_UP){
            // close connection
            sr_close_connection(connection,h->handle,h->mux,h->info,RASTA_DISC_REASON_UNEXPECTEDTYPE,0);
        }else if (connection->current_state == RASTA_CONNECTION_RETRRUN){
            // send RetrReq
            logger_log(h->logger, LOG_LEVEL_DEBUG, "Process RetrData", "changing to tls_state RetrReq");
            send_RetransmissionRequest(h->mux,connection);
            connection->current_state = RASTA_CONNECTION_RETRREQ;
            fire_on_connection_state_change(sr_create_notification_result(h->handle,connection));
        }
    }
}

#ifdef ENABLE_OPAQUE
bool sr_rekeying_skipped(struct rasta_connection *connection, struct RastaConfigKex *kexConfig) {
    uint64_t current_time;
    if(connection->current_state == RASTA_CONNECTION_KEX_REQ){
        // already waiting for key exchange
        return false;
    }

    if(connection->role != RASTA_ROLE_SERVER){
        // client cannot expect to receive key requests from server
        return false;
    }

    if(!kexConfig->rekeying_interval_ms || !connection->kex_state.last_key_exchanged_millis){
        // no rekeying or no initial time yet
        return false;
    }

    current_time = get_current_time_ms();

    return current_time - connection->kex_state.last_key_exchanged_millis > REKEYING_ALLOWED_DELAY_MS + kexConfig->rekeying_interval_ms;
}
#else
bool sr_rekeying_skipped(struct rasta_connection *connection, struct RastaConfigKex *kexConfig){
    // no rekeying possible without key exchange
    (void) connection;
    (void) kexConfig;
    return false;
}
#endif

/*
 * threads
 */

int on_readable_event(void* handle) {
    struct rasta_receive_handle *h = (struct rasta_receive_handle*) handle;

    // wait for incoming packets
    struct RastaPacket receivedPacket;
    if (!redundancy_mux_try_retrieve_all(h->mux, &receivedPacket)) {
        return 0;
    }

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA RECEIVE", "Received packet %d from %d to %d", receivedPacket.type, receivedPacket.sender_id, receivedPacket.receiver_id);

    struct rasta_connection* con;
    for (con = h->handle->first_con; con; con = con->linkedlist_next) {
        if (con->remote_id == receivedPacket.sender_id) break;
    }
    //new client request
    if (receivedPacket.type == RASTA_TYPE_CONNREQ){
        con = handle_conreq(h, con, receivedPacket);

        freeRastaByteArray(&receivedPacket.data);
        return 0;
    }

    if (con == NULL) {
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA RECEIVE", "Received packet (%d) from unknown source %d", receivedPacket.type, receivedPacket.sender_id);
        //received packet from unknown source
        //TODO: can these packets be ignored?

        freeRastaByteArray(&receivedPacket.data);
        return 0;
    }

    //handle response
    if (receivedPacket.type == RASTA_TYPE_CONNRESP) {
        handle_conresp(h, con, receivedPacket);

        freeRastaByteArray(&receivedPacket.data);
        return 0;
    }


    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA RECEIVE", "Checking packet ...");

    // check message checksum
    if (!receivedPacket.checksum_correct){
        logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received packet checksum incorrect");
        // increase safety error counter
        con->errors.safety++;

        freeRastaByteArray(&receivedPacket.data);
        return 0;
    }

    // check for plausible ids
    if (!sr_message_authentic(con, receivedPacket)) {
        logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received packet invalid sender/receiver");
        // increase address error counter
        con->errors.address++;

        freeRastaByteArray(&receivedPacket.data);
        return 0;
    }

    // check sequency number range
    if (!sr_sn_range_valid(con, h->config, receivedPacket)){
        logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received packet sn range invalid");

        // invalid -> increase error counter and discard packet
        con->errors.sn++;

        freeRastaByteArray(&receivedPacket.data);
        return 0;
    }

    // check confirmed sequence number
    if (!sr_cs_valid(con, receivedPacket)){
        logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received packet cs invalid");

        // invalid -> increase error counter and discard packet
        con->errors.cs++;

        freeRastaByteArray(&receivedPacket.data);
        return 0;
    }

    if(sr_rekeying_skipped(con,&h->handle->config.values.kex)){
        logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA KEX", "Did not receive key exchange request for rekeying in time at %"PRIu64" - disconnecting!",get_current_time_ms());
        sr_close_connection(con,h->handle,h->mux,h->info,RASTA_DISC_REASON_TIMEOUT,0);
        freeRastaByteArray(&receivedPacket.data);
        return 0;
    }

    switch (receivedPacket.type){
        case RASTA_TYPE_RETRDATA:
            handle_retrdata(h,con, receivedPacket);
            break;
        case RASTA_TYPE_DATA:
            handle_data(h,con, receivedPacket);
            break;
        case RASTA_TYPE_RETRREQ:
            handle_retrreq(h,con, receivedPacket);
            break;
        case RASTA_TYPE_RETRRESP:
            handle_retrresp(h,con, receivedPacket);
            break;
        case RASTA_TYPE_DISCREQ:
            handle_discreq(h,con, receivedPacket);
            break;
        case RASTA_TYPE_HB:
            handle_hb(h,con, receivedPacket);
            break;
#ifdef ENABLE_OPAQUE
        case RASTA_TYPE_KEX_REQUEST:
            handle_kex_request(h,con,receivedPacket);
            break;
        case RASTA_TYPE_KEX_RESPONSE:
            handle_kex_response(h,con,receivedPacket);
            break;
        case RASTA_TYPE_KEX_AUTHENTICATION:
            handle_kex_auth(h,con,receivedPacket);
            break;
#endif
        default:
            logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received unexpected packet type %d", receivedPacket.type);
            // increase type error counter
            con->errors.type++;
            break;
    }

    freeRastaByteArray(&receivedPacket.data);
    return 0;
}

int event_connection_expired(void* carry_data) {
    struct timed_event_data* data = carry_data;
    struct rasta_heartbeat_handle *h = (struct rasta_heartbeat_handle*) data->handle;
    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "T_i timer expired - send DisconnectionRequest");

    struct rasta_connection* connection = data->connection;
    //so check if connection is valid

    if (connection == NULL || connection->hb_locked) {
        return 0;
    }

    //connection is valid, check current tls_state
    if (connection->current_state == RASTA_CONNECTION_UP
        || connection->current_state == RASTA_CONNECTION_RETRREQ
        || connection->current_state == RASTA_CONNECTION_RETRRUN) {

        // fire heartbeat timeout event
        fire_on_heartbeat_timeout(sr_create_notification_result(h->handle, connection));

        // T_i expired -> close connection
        sr_close_connection(connection,h->handle,h->mux,h->info, RASTA_DISC_REASON_TIMEOUT, 0);
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "T_i timer expired - \033[91mdisconnected\033[0m");
    }

    disable_timed_event(&connection->send_heartbeat_event);
    disable_timed_event(&connection->timeout_event);
    return 0;
}

int heartbeat_send_event(void* carry_data) {
    struct timed_event_data* data = carry_data;
    struct rasta_heartbeat_handle* h = (struct rasta_heartbeat_handle*) data->handle;

    struct rasta_connection* connection = data->connection;


    if (connection == NULL || connection->hb_locked) {
        return 0;
    }


    //connection is valid, check current tls_state
    if (connection->current_state == RASTA_CONNECTION_UP
        || connection->current_state == RASTA_CONNECTION_RETRREQ
        || connection->current_state == RASTA_CONNECTION_RETRRUN) {
        send_Heartbeat(h->mux, connection, 0);

        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "Heartbeat sent to %d", connection->remote_id);
    }

    return 0;
}

// TODO: split up this mess of a function
int data_send_event(void * carry_data) {
    struct rasta_sending_handle * h = carry_data;

    for (struct rasta_connection* con = h->handle->first_con; con; con = con->linkedlist_next) {

        if (con->current_state == RASTA_CONNECTION_DOWN || con->current_state == RASTA_CONNECTION_CLOSED) {
            continue;
        }


        unsigned int retr_data_count = sr_retr_data_available(h->logger,con);
        if (retr_data_count <= h->config.max_packet) {
            unsigned int msg_queue = sr_rasta_send_data_available(h->logger,con);

            if (msg_queue > 0) {
                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler", "Messages waiting to be send: %d",
                            msg_queue);

                con->hb_stopped = 1;
                con->is_sending = 1;

                struct RastaMessageData app_messages;
                struct RastaByteArray msg;

                if (msg_queue >= h->config.max_packet) {
                    msg_queue = h->config.max_packet;
                }
                allocateRastaMessageData(&app_messages, msg_queue);

                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler",
                            "Sending %d application messages from queue",
                            msg_queue);


                for (unsigned int i = 0; i < msg_queue; i++) {

                    struct RastaByteArray * elem;
                    elem = fifo_pop(con->fifo_send);
                    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler",
                                "Adding application message '%s' to data packet",
                                elem->bytes);

                    allocateRastaByteArray(&msg, elem->length);
                    msg.bytes = rmemcpy(msg.bytes, elem->bytes, elem->length);
                    freeRastaByteArray(elem);
                    rfree(elem);
                    app_messages.data_array[i] = msg;
                }

                struct RastaPacket data = createDataMessage(con->remote_id, con->my_id, con->sn_t,
                                                            con->cs_t, cur_timestamp(), con->ts_r,
                                                            app_messages, h->hashing_context);


                struct RastaByteArray packet = rastaModuleToBytes(data, h->hashing_context);

                struct RastaByteArray * to_fifo = rmalloc(sizeof(struct RastaByteArray));
                allocateRastaByteArray(to_fifo, packet.length);
                rmemcpy(to_fifo->bytes, packet.bytes, packet.length);
                fifo_push(con->fifo_retr, to_fifo);

                redundancy_mux_send(h->mux, data);

                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler", "Sent data packet from queue");

                con->sn_t = data.sequence_number + 1;

                // set last message ts
                reschedule_event(&con->send_heartbeat_event);

                con->hb_stopped = 0;

                freeRastaMessageData(&app_messages);
                freeRastaByteArray(&packet);
                freeRastaByteArray(&data.data);

                con->is_sending = 0;
            }
        }

        usleep(50);
    }
    return 0;
}

void sr_init_handle_manually(struct rasta_handle *handle, struct RastaConfigInfo configuration, struct DictionaryArray accepted_version, struct logger_t logger) {
    rasta_handle_manually_init(handle,configuration,accepted_version, logger);

    // init the redundancy layer
    handle->mux = redundancy_mux_init_(handle->redlogger, handle->config.values);
    //redundancy_mux_set_config_id(&handle->mux,handle->own_id);
    // register redundancy layer diagnose notification handler
    handle->mux.notifications.on_diagnostics_available = handle->notifications.on_redundancy_diagnostic_notification;

    // setup MD4
    /*setMD4checksum(handle->config.values.sending.md4_type,
                   handle->config.values.sending.md4_a,
                   handle->config.values.sending.md4_b,
                   handle->config.values.sending.md4_c,
                   handle->config.values.sending.md4_d);*/
}

void sr_init_handle(struct rasta_handle* handle, const char* config_file_path) {

    rasta_handle_init(handle, config_file_path);

    // init the redundancy layer
    handle->mux = redundancy_mux_init_(handle->redlogger, handle->config.values);
    //redundancy_mux_set_config_id(&handle->mux,handle->own_id);
    // register redundancy layer diagnose notification handler
    handle->mux.notifications.on_diagnostics_available = handle->notifications.on_redundancy_diagnostic_notification;

    // setup MD4
    /*setMD4checksum(handle->config.values.sending.md4_type,
                   handle->config.values.sending.md4_a,
                   handle->config.values.sending.md4_b,
                   handle->config.values.sending.md4_c,
                   handle->config.values.sending.md4_d);*/

    handle->hashing_context.algorithm = RASTA_ALGO_MD4;
    handle->hashing_context.hash_length = handle->config.values.sending.md4_type;
    rasta_md4_set_key(&handle->hashing_context, handle->config.values.sending.md4_a, handle->config.values.sending.md4_b,
                      handle->config.values.sending.md4_c, handle->config.values.sending.md4_d);
}

void sr_connect(struct rasta_handle *h, unsigned long id, struct RastaIPData *channels) {
    for (struct rasta_connection* con = h->first_con; con; con = con->linkedlist_next) {
        //TODO: Error handling
        if (con->remote_id == id) return;
    }
    //TODO: const ports in redundancy? (why no dynamic port length)
    redundancy_mux_add_channel(&h->mux,id,channels);

    struct rasta_connection new_con;
    memset(&new_con, 0, sizeof(struct rasta_connection));
    sr_init_connection(&new_con,id,h->config.values.general,h->config.values.sending,&h->logger, RASTA_ROLE_CLIENT);
    redundancy_mux_set_config_id(&h->mux, id);

    // initialize seq nums and timestamps
    new_con.sn_t = get_initial_seq_num(&h->config);
    //new_con.sn_t = 66;
    logger_log(&h->logger, LOG_LEVEL_DEBUG, "RaSTA CONNECT", "Using %lu as initial sequence number",
        (long unsigned int) new_con.sn_t);

    new_con.cs_t = 0;
    new_con.cts_r = cur_timestamp();
    new_con.t_i = h->config.values.sending.t_max;

    unsigned char * version = (unsigned char*)RASTA_VERSION;

    logger_log(&h->logger, LOG_LEVEL_DEBUG, "RaSTA CONNECT", "Local version is %s", version);


    // send ConReq
    struct RastaPacket conreq = createConnectionRequest(new_con.remote_id, new_con.my_id,
                                                        new_con.sn_t, cur_timestamp(),
                                                        h->config.values.sending.send_max,
                                                        version, &h->hashing_context);
    new_con.sn_i = new_con.sn_t;

    redundancy_mux_send(&h->mux,conreq);

    // increase sequence number
    new_con.sn_t++;

    // update tls_state
    new_con.current_state = RASTA_CONNECTION_START;

    void* memory = h->user_handles->on_connection_start(&new_con);
    if (memory == NULL) {
        logger_log(&h->logger, LOG_LEVEL_DEBUG, "RaSTA CONNECT", "connection refused by user to %d", new_con.remote_id);
        return;
    }

    struct rasta_connection *con = memory;
    *con = new_con;
    add_connection_to_list(h, con);

    freeRastaByteArray(&conreq.data);

    // fire connection tls_state changed event
    fire_on_connection_state_change(sr_create_notification_result(h,con));

    init_connection_events(h, con);
}

void sr_send(struct rasta_handle *h, unsigned long remote_id, struct RastaMessageData app_messages){

    struct rasta_connection *con;
    for (con = h->first_con; con; con = con->linkedlist_next) {
        if (con->remote_id == remote_id) break;
    }

    if (con == 0) return;

    if(con->current_state == RASTA_CONNECTION_UP){
        if (app_messages.count > h->config.values.sending.max_packet){
            // to many application messages
            logger_log(&h->logger, LOG_LEVEL_ERROR, "RaSTA send", "too many application messages to send in one packet. Maximum is %d",
                       h->config.values.sending.max_packet);
            // do nothing and leave method with error code 2
            return;
        }

        for (unsigned int i = 0; i < app_messages.count; ++i) {
            struct RastaByteArray msg;
            msg = app_messages.data_array[i];

            // push into queue
            struct RastaByteArray * to_fifo = rmalloc(sizeof(struct RastaByteArray));
            allocateRastaByteArray(to_fifo, msg.length);
            rmemcpy(to_fifo->bytes, msg.bytes, msg.length);
            fifo_push(con->fifo_send, to_fifo);
        }

        logger_log(&h->logger, LOG_LEVEL_INFO, "RaSTA send", "data in send queue");

    } else if (con->current_state == RASTA_CONNECTION_CLOSED || con->current_state == RASTA_CONNECTION_DOWN){
        // nothing to do besides changing tls_state to closed
        con->current_state = RASTA_CONNECTION_CLOSED;

        // fire connection tls_state changed event
        fire_on_connection_state_change(sr_create_notification_result(h,con));
    } else {
        logger_log(&h->logger, LOG_LEVEL_ERROR, "RaSTA send", "service not allowed");

        // disconnect and close
        send_DisconnectionRequest(&h->mux,con, RASTA_DISC_REASON_SERVICENOTALLOWED, 0);
        con->current_state = RASTA_CONNECTION_CLOSED;

        // fire connection tls_state changed event
        fire_on_connection_state_change(sr_create_notification_result(h,con));

        // leave with error code 1
        return;
    }
}

rastaApplicationMessage sr_get_received_data(struct rasta_handle *h, struct rasta_connection * connection){
    rastaApplicationMessage message;
    rastaApplicationMessage * element;

    element = fifo_pop(connection->fifo_app_msg);

    message.id = element->id;
    message.appMessage = element->appMessage;

    logger_log(&h->logger, LOG_LEVEL_DEBUG, "RaSTA retrieve", "application message with l %d", message.appMessage.length);
    //logger_log(&h->logger, LOG_LEVEL_DEBUG, "RETRIEVE DATA", "Convert bytes to packet");

    rfree(element);

    //struct RastaPacket packet = bytesToRastaPacket(msg);
    //return packet;
    return message;
}

/**
 * cleanup a connection after a disconnect
 * @param h
 * @param remote_id
 */
void sr_disconnect(struct rasta_handle* h, struct rasta_connection* con) {
    logger_log(&h->logger, LOG_LEVEL_INFO, "RaSTA connection", "disconnected %X", con->remote_id);

    sr_close_connection(con, h, &h->mux, h->config.values.general, RASTA_DISC_REASON_USERREQUEST, 0);

    remove_timed_event(h->ev_sys,&con->timeout_event);
    remove_timed_event(h->ev_sys,&con->send_heartbeat_event);
    remove_connection_from_list(h,con);
#ifdef ENABLE_OPAQUE
    if(h->config.values.kex.rekeying_interval_ms) {
        remove_timed_event(h->ev_sys,&con->rekeying_event);
    }
#endif

    h->user_handles->on_disconnect(con, con);
}

void sr_cleanup(struct rasta_handle *h) {
    logger_log(&h->logger, LOG_LEVEL_DEBUG, "RaSTA Cleanup", "Cleanup called");

    h->hb_running = 0;
    h->recv_running = 0;
    h->send_running = 0;

    if (h->user_handles->on_rasta_cleanup) {
        h->user_handles->on_rasta_cleanup();
    }

    for (struct rasta_connection* connection = h->first_con; connection; connection = connection->linkedlist_next) {
        // free memory allocated for diagnostic intervals
        rfree(connection->diagnostic_intervals);

        //free FIFOs
        fifo_destroy(connection->fifo_app_msg);
        fifo_destroy(connection->fifo_send);
        fifo_destroy(connection->fifo_retr);
    }

    // set notification pointers to NULL
    h->notifications.on_receive = NULL;
    h->notifications.on_connection_state_change= NULL;
    h->notifications.on_diagnostic_notification = NULL;
    h->notifications.on_disconnection_request_received = NULL;
    h->notifications.on_redundancy_diagnostic_notification = NULL;


    // close mux
    redundancy_mux_close(&h->mux);


    // free config
    config_free(&h->config);

    // free accepted version
    free_DictionaryArray(&h->receive_handle->accepted_version);



    rfree(h->receive_handle);
    rfree(h->send_handle);
    rfree(h->heartbeat_handle);

    logger_log(&h->logger, LOG_LEVEL_DEBUG, "RaSTA Cleanup", "Cleanup done");

    logger_destroy(&h->logger);
}

void log_main_loop_state(struct rasta_handle* h, event_system* ev_sys, const char* message) {
    int fd_event_count = 0, fd_event_active_count = 0, timed_event_count = 0, timed_event_active_count = 0;
    for (fd_event* ev = ev_sys->fd_events.first; ev; ev = ev->next) {
        fd_event_count++;
        fd_event_active_count += !!ev->enabled;
    }
    for (timed_event* ev = ev_sys->timed_events.first; ev; ev = ev->next) {
        timed_event_count++;
        timed_event_active_count += !!ev->enabled;
    }
    logger_log(&h->logger, LOG_LEVEL_DEBUG, "RaSTA EVENT-SYSTEM", "%s | %d/%d fd events and %d/%d timed events active",
        message, fd_event_active_count, fd_event_count, timed_event_active_count, timed_event_count);
}

#define IO_INTERVAL 10000
void sr_begin(struct rasta_handle* h, event_system* event_system, int channel_timeout_ms) {
    timed_event send_event, receive_event;
    timed_event channel_timeout_event;
    struct timeout_event_data timeout_data;

    h->ev_sys = event_system;

    // busy wait like io events TODO: move to a position so it is only called when needed
    memset(&send_event, 0, sizeof(timed_event));
    send_event.callback = data_send_event;
    send_event.interval = IO_INTERVAL * 1000lu;
    send_event.carry_data = h->send_handle;
    enable_timed_event(&send_event);
    add_timed_event(event_system, &send_event);

    memset(&receive_event, 0, sizeof(timed_event));
    receive_event.callback = on_readable_event;
    receive_event.interval = IO_INTERVAL * 1000lu;
    receive_event.carry_data = h->receive_handle;
    enable_timed_event(&receive_event);
    add_timed_event(event_system, &receive_event);

    // Handshake timeout event
    init_channel_timeout_events(&channel_timeout_event, &timeout_data, &h->mux, channel_timeout_ms);
    if (channel_timeout_ms) {
        enable_timed_event(&channel_timeout_event);
    }
    add_timed_event(event_system, &channel_timeout_event);

    int channel_event_data_len = h->mux.port_count;
    fd_event channel_events[channel_event_data_len];
    struct receive_event_data channel_event_data[channel_event_data_len];
    for (int i = 0; i < channel_event_data_len; i++) {
        memset(&channel_events[i], 0, sizeof(fd_event));
        channel_events[i].enabled = 1;
        channel_events[i].callback = channel_receive_event;
        channel_events[i].carry_data = channel_event_data + i;
        channel_events[i].fd = h->mux.udp_socket_states[i].file_descriptor;
        channel_event_data[i].channel_index = i;
        channel_event_data[i].event = channel_events + i;
        channel_event_data[i].h = h;
    }
    for (int i = 0; i < channel_event_data_len; i++) {
        add_fd_event(event_system, &channel_events[i], EV_READABLE);
    }

    log_main_loop_state(h, event_system, "event-system started");
    event_system_start(event_system);

    // Remove all stack entries from linked lists...
    remove_timed_event(event_system, &send_event);
    remove_timed_event(event_system, &receive_event);
    remove_timed_event(event_system, &channel_timeout_event);
    for (int i = 0; i < channel_event_data_len; i++) {
        remove_fd_event(event_system, &channel_events[i]);
    }
}
