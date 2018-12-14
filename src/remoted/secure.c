/* Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "shared.h"
#include "os_net/os_net.h"
#include "remoted.h"

/* Global variables */
int sender_pool;

static netbuffer_t netbuffer;

// Message handler thread
static void * rem_handler_main(__attribute__((unused)) void * args);

// Key reloader thread
void * rem_keyupdate_main(__attribute__((unused)) void * args);

/* Handle each message received */
static void HandleSecureMessage(char *buffer, int recv_b, struct sockaddr_in *peer_info, int sock_client);

// Close and remove socket from keystore
int _close_sock(keystore * keys, int sock);

/* Handle secure connections */
void HandleSecure()
{
    const int protocol = logr.proto[logr.position];
    int sock_client;
    int n_events = 0;
    char buffer[OS_MAXSTR + 1];
    ssize_t recv_b;
    struct sockaddr_in peer_info;
    wnotify_t * notify = NULL;

    /* Initialize manager */
    manager_init();

    // Initialize messag equeue
    rem_msginit(logr.queue_size);

    /* Create Active Response forwarder thread */
    w_create_thread(update_shared_files, NULL);

    /* Create Active Response forwarder thread */
    w_create_thread(AR_Forward, NULL);

    // Create Request listener thread
    w_create_thread(req_main, NULL);

    // Create State writer thread
    w_create_thread(rem_state_main, NULL);

    /* Create wait_for_msgs threads */

    {
        int i;
        sender_pool = getDefine_Int("remoted", "sender_pool", 1, 64);

        mdebug2("Creating %d sender threads.", sender_pool);

        for (i = 0; i < sender_pool; i++) {
            w_create_thread(wait_for_msgs, NULL);
        }
    }

    // Create message handler thread pool
    {
        int worker_pool = getDefine_Int("remoted", "worker_pool", 1, 16);

        while (worker_pool > 0) {
            w_create_thread(rem_handler_main, NULL);
            worker_pool--;
        }
    }

    /* Connect to the message queue
     * Exit if it fails.
     */
    if ((logr.m_queue = StartMQ(DEFAULTQUEUE, WRITE)) < 0) {
        merror_exit(QUEUE_FATAL, DEFAULTQUEUE);
    }

    minfo(AG_AX_AGENTS, MAX_AGENTS);

    /* Read authentication keys */
    minfo(ENC_READ);
    OS_ReadKeys(&keys, 1, 0, 0);
    OS_StartCounter(&keys);

    // Key reloader thread
    w_create_thread(rem_keyupdate_main, NULL);

    /* Set up peer size */
    logr.peer_size = sizeof(peer_info);

    /* Initialize some variables */
    memset(buffer, '\0', OS_MAXSTR + 1);

    if (protocol == TCP_PROTO) {
        if (notify = wnotify_init(MAX_EVENTS), !notify) {
            merror_exit("wnotify_init(): %s (%d)", strerror(errno), errno);
        }

        if (wnotify_add(notify, logr.sock) < 0) {
            merror_exit("wnotify_add(%d): %s (%d)", logr.sock, strerror(errno), errno);
        }
    }

    while (1) {
        /* Receive message  */
        if (protocol == TCP_PROTO) {
            if (n_events = wnotify_wait(notify, EPOLL_MILLIS), n_events < 0) {
                if (errno != EINTR) {
                    merror("Waiting for connection: %s (%d)", strerror(errno), errno);
                    sleep(1);
                }

                continue;
            }

            int i;
            for (i = 0; i < n_events; i++) {
                int fd = wnotify_get(notify, i);

                if (fd == logr.sock) {
                    sock_client = accept(logr.sock, (struct sockaddr *)&peer_info, &logr.peer_size);
                    if (sock_client < 0) {
                        merror_exit(ACCEPT_ERROR);
                    }

                    nb_open(&netbuffer, sock_client, &peer_info);
                    rem_inc_tcp();
                    mdebug1("New TCP connection at %s [%d]", inet_ntoa(peer_info.sin_addr), sock_client);

                    if (wnotify_add(notify, sock_client) < 0) {
                        merror("wnotify_add(%d, %d): %s (%d)", notify->fd, sock_client, strerror(errno), errno);
                        _close_sock(&keys, sock_client);
                    }
                } else {
                    sock_client = fd;

                    switch (recv_b = nb_recv(&netbuffer, sock_client), recv_b) {
                    case -2:
                        mwarn("Too big message size from %s [%d].", inet_ntoa(peer_info.sin_addr), sock_client);
                        _close_sock(&keys, sock_client);
                        continue;

                    case -1:
                        switch (errno) {
                        case ECONNRESET:
                        case ENOTCONN:
                            mdebug2("TCP peer [%d] at %s: %s (%d)", sock_client, inet_ntoa(peer_info.sin_addr), strerror(errno), errno);
                            break;
                        default:
                            merror("TCP peer [%d] at %s: %s (%d)", sock_client, inet_ntoa(peer_info.sin_addr), strerror(errno), errno);
                        }

                        // Fallthrough

                    case 0:
                        if (wnotify_delete(notify, sock_client) < 0) {
                            merror("wnotify_delete(%d): %s (%d)", sock_client, strerror(errno), errno);
                        }

                        _close_sock(&keys, sock_client);
                        continue;

                    default:
                        rem_add_recv((unsigned long)recv_b);
                    }
                }
            }
        } else {
            recv_b = recvfrom(logr.sock, buffer, OS_MAXSTR, 0, (struct sockaddr *)&peer_info, &logr.peer_size);

            /* Nothing received */
            if (recv_b <= 0) {
                continue;
            } else {
                rem_msgpush(buffer, recv_b, &peer_info, -1);
                rem_add_recv((unsigned long)recv_b);
            }
        }
    }
}

// Message handler thread
void * rem_handler_main(__attribute__((unused)) void * args) {
    message_t * message;
    char buffer[OS_MAXSTR + 1] = "";
    mdebug1("Message handler thread started.");

    while (1) {
        message = rem_msgpop();
        memcpy(buffer, message->buffer, message->size);
        HandleSecureMessage(buffer, message->size, &message->addr, message->sock);
        rem_msgfree(message);
    }

    return NULL;
}

// Key reloader thread
void * rem_keyupdate_main(__attribute__((unused)) void * args) {
    int seconds;

    mdebug1("Key reloader thread started.");
    seconds = getDefine_Int("remoted", "keyupdate_interval", 1, 3600);

    while (1) {
        mdebug2("Checking for keys file changes.");
        check_keyupdate();
        sleep(seconds);
    }
}

static void HandleSecureMessage(char *buffer, int recv_b, struct sockaddr_in *peer_info, int sock_client) {
    int agentid;
    int protocol = logr.proto[logr.position];
    char cleartext_msg[OS_MAXSTR + 1];
    char srcmsg[OS_FLSIZE + 1];
    char srcip[IPSIZE + 1];
    char agname[KEYSIZE + 1];
    char *tmp_msg;
    size_t msg_length;

    /* Set the source IP */
    strncpy(srcip, inet_ntoa(peer_info->sin_addr), IPSIZE);
    srcip[IPSIZE] = '\0';

    /* Initialize some variables */
    memset(cleartext_msg, '\0', OS_MAXSTR + 1);
    memset(srcmsg, '\0', OS_FLSIZE + 1);
    tmp_msg = NULL;

    /* Get a valid agent id */
    if (buffer[0] == '!') {
        tmp_msg = buffer;
        tmp_msg++;

        /* We need to make sure that we have a valid id
         * and that we reduce the recv buffer size
         */
        while (isdigit((int)*tmp_msg)) {
            tmp_msg++;
            recv_b--;
        }

        if (*tmp_msg != '!') {
            merror(ENCFORMAT_ERROR, "(unknown)", srcip);

            if (sock_client >= 0)
                _close_sock(&keys, sock_client);

            return;
        }

        *tmp_msg = '\0';
        tmp_msg++;
        recv_b -= 2;

        key_lock_read();
        agentid = OS_IsAllowedDynamicID(&keys, buffer + 1, srcip);

        if (agentid == -1) {
            int id = OS_IsAllowedID(&keys, buffer + 1);

            if (id < 0) {
                strncpy(agname, "unknown", sizeof(agname));
            } else {
                strncpy(agname, keys.keyentries[id]->name, sizeof(agname));
            }

            key_unlock();

            agname[sizeof(agname) - 1] = '\0';

            mwarn(ENC_IP_ERROR, buffer + 1, srcip, agname);

            if (sock_client >= 0)
                _close_sock(&keys, sock_client);

            return;
        }
    } else {
        key_lock_read();
        agentid = OS_IsAllowedIP(&keys, srcip);

        if (agentid < 0) {
            key_unlock();
            mwarn(DENYIP_WARN, srcip);

            if (sock_client >= 0)
                _close_sock(&keys, sock_client);

            return;
        }
        tmp_msg = buffer;
    }

    /* Decrypt the message */

    tmp_msg = ReadSecMSG(&keys, tmp_msg, cleartext_msg, agentid, recv_b - 1, &msg_length, srcip);

    if (tmp_msg == NULL) {

        /* If duplicated, a warning was already generated */
        key_unlock();
        return;
    }

    /* Check if it is a control message */
    if (IsValidHeader(tmp_msg)) {
        int r = 2;

        /* We need to save the peerinfo if it is a control msg */

        memcpy(&keys.keyentries[agentid]->peer_info, peer_info, logr.peer_size);
        keyentry * key = OS_DupKeyEntry(keys.keyentries[agentid]);
        r = (protocol == TCP_PROTO) ? OS_AddSocket(&keys, agentid, sock_client) : 2;
        keys.keyentries[agentid]->rcvd = time(0);

        switch (r) {
        case 0:
            merror("Couldn't add TCP socket to keystore.");
            break;
        case 1:
            mdebug2("TCP socket %d already in keystore. Updating...", sock_client);
            break;
        default:
            ;
        }

        key_unlock();

        // The critical section for readers closes within this function
        save_controlmsg(key, tmp_msg, msg_length - 3);
        rem_inc_ctrl_msg();

        OS_FreeKey(key);
        return;
    }

    /* Generate srcmsg */

    snprintf(srcmsg, OS_FLSIZE, "[%s] (%s) %s", keys.keyentries[agentid]->id,
             keys.keyentries[agentid]->name, keys.keyentries[agentid]->ip->ip);

    key_unlock();

    /* If we can't send the message, try to connect to the
     * socket again. If it not exit.
     */
    if (SendMSG(logr.m_queue, tmp_msg, srcmsg,
                SECURE_MQ) < 0) {
        merror(QUEUE_ERROR, DEFAULTQUEUE, strerror(errno));

        if ((logr.m_queue = StartMQ(DEFAULTQUEUE, WRITE)) < 0) {
            merror_exit(QUEUE_FATAL, DEFAULTQUEUE);
        }
    } else {
        rem_inc_evt();
    }
}

// Close and remove socket from keystore
int _close_sock(keystore * keys, int sock) {
    int retval;

    key_lock_read();
    retval = OS_DeleteSocket(keys, sock);
    key_unlock();

    if (nb_close(&netbuffer, sock) == 0) {
        rem_dec_tcp();
    }

    mdebug1("TCP peer disconnected [%d]", sock);

    return retval;
}
