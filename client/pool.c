/* пул и майнер, T13.744-T13.895 $DVS:time$ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#if defined(_WIN32) || defined(_WIN64)
#if defined(_WIN64)
#define poll WSAPoll
#else
#define poll(a, b, c) ((a)->revents = (a)->events, (b))
#endif
#else
#include <poll.h>
#endif
#include "system.h"
#include "../dus/programs/dfstools/source/dfslib/dfslib_string.h"
#include "../dus/programs/dar/source/include/crc.h"
#include "address.h"
#include "block.h"
#include "init.h"
#include "pool.h"
#include "storage.h"
#include "sync.h"
#include "transport.h"
#include "wallet.h"
#include "log.h"
#include "commands.h"
#include "mining_common.h"

#define START_MINERS_COUNT     256
#define START_MINERS_IP_COUNT  8
#define HEADER_WORD            0x3fca9e2bu
#define FUND_ADDRESS           "FQglVQtb60vQv2DOWEUL7yh3smtj7g1s"  /* community fund */
#define SHARES_PER_TASK_LIMIT  20                                  /* maximum count of shares per task */

xdag_hash_t g_xdag_mined_hashes[CONFIRMATIONS_COUNT], g_xdag_mined_nonce[CONFIRMATIONS_COUNT];

static int g_max_miners_count = START_MINERS_COUNT, g_max_miner_ip_count = START_MINERS_IP_COUNT;
static int g_miners_count = 0, g_socket = -1;
static double g_pool_fee = 0, g_pool_reward = 0, g_pool_direct = 0, g_pool_fund = 0;
static struct xdag_block *g_firstb = 0, *g_lastb = 0;

//function sets minimal share for the task and calculates share difficulty for further payment calculations, pool side
static void pool_set_share(struct miner *m, struct xdag_pool_task *task, xdag_hash_t last, xdag_hash_t hash)
{
	const xdag_time_t task_time = task->task_time;

	if (xdag_cmphash(hash, task->minhash.data) < 0) {
		pthread_mutex_lock(&g_share_mutex);

		if (xdag_cmphash(hash, task->minhash.data) < 0) {
			memcpy(task->minhash.data, hash, sizeof(xdag_hash_t));
			memcpy(task->lastfield.data, last, sizeof(xdag_hash_t));
		}

		pthread_mutex_unlock(&g_share_mutex);
	}

	if (m->task_time <= task_time) {
		double diff = ((uint64_t*)hash)[2];
		int i = task_time & (CONFIRMATIONS_COUNT - 1);

		diff = ldexp(diff, -64);
		diff += ((uint64_t*)hash)[3];

		if (diff < 1) diff = 1;

		diff = 46 - log(diff);

		if (m->task_time < task_time) {
			m->task_time = task_time;

			if (m->maxdiff[i] > 0) {
				m->prev_diff += m->maxdiff[i];
				m->prev_diff_count++;
			}

			m->maxdiff[i] = diff;
			m->state &= ~MINER_BALANCE;
		} else if (diff > m->maxdiff[i]) {
			m->maxdiff[i] = diff;
		}
	}
}

void *pool_main_thread(void *arg)
{
	struct xdag_pool_task *task;
	const char *mess;
	uint64_t task_index;
	int todo, done;

	while (!g_xdag_sync_on) {
		sleep(1);
	}

	for (;;) {
		const int miners_count = g_miners_count;
		if (!poll(g_fds, miners_count, 1000)) continue;

		for (int i = done = 0; i < miners_count; ++i) {
			struct miner *m = g_miners + i;
			struct pollfd *p = g_fds + i;
			
			if (m->state & (MINER_ARCHIVE | MINER_FREE)) continue;
			
			if (p->revents & POLLNVAL) continue;
			
			if (p->revents & POLLHUP) {
				done = 1; 
				mess = "socket hangup";
 disconnect:
				m->state |= MINER_ARCHIVE;
 disconnect_free:
				close(p->fd);
				
				p->fd = -1;
				
				if (m->block) {
					free(m->block);
					m->block = 0;
				}

				int ip = m->ip;
				
				xdag_info("Pool  : miner %d disconnected from %u.%u.%u.%u:%u by %s", i,
							   ip & 0xff, ip >> 8 & 0xff, ip >> 16 & 0xff, ip >> 24 & 0xff, ntohs(m->port), mess);
				
				continue;
			}

			if (p->revents & POLLERR) {
				done = 1;
				mess = "socket error"; 
				goto disconnect;
			}

			if (p->revents & POLLIN) {
				done = 1;
				todo = sizeof(struct xdag_field) - m->data_size;
				todo = read(p->fd, (uint8_t*)m->data + m->data_size, todo);
				
				if (todo <= 0) {
					mess = "read error"; 
					goto disconnect;
				}
				
				m->data_size += todo;
				
				if (m->data_size == sizeof(struct xdag_field)) {
					m->data_size = 0;
					dfslib_uncrypt_array(g_crypt, m->data, DATA_SIZE, m->nfield_in++);
					
					if (!m->block_size && m->data[0] == HEADER_WORD) {
						m->block = malloc(sizeof(struct xdag_block));
						
						if (!m->block) continue;
						
						memcpy(m->block->field, m->data, sizeof(struct xdag_field));
						m->block_size++;
					} else if (m->nfield_in == 1) {
						mess = "protocol mismatch";
						m->state = MINER_FREE; 
						goto disconnect_free;
					} else if (m->block_size) {
						memcpy(m->block->field + m->block_size, m->data, sizeof(struct xdag_field));
						m->block_size++;
						if (m->block_size == XDAG_BLOCK_FIELDS) {
							uint32_t crc = m->block->field[0].transport_header >> 32;
							
							m->block->field[0].transport_header &= (uint64_t)0xffffffffu;
							
							if (crc == crc_of_array((uint8_t*)m->block, sizeof(struct xdag_block))) {
								m->block->field[0].transport_header = 0;
								
								pthread_mutex_lock(&g_pool_mutex);
								
								if (!g_firstb) {
									g_firstb = g_lastb = m->block;
								} else {
									g_lastb->field[0].transport_header = (uintptr_t)m->block;
									g_lastb = m->block;
								}
								
								pthread_mutex_unlock(&g_pool_mutex);
							} else {
								free(m->block);
							}

							m->block = 0;
							m->block_size = 0;
						}
					} else {
						xdag_hash_t hash;

						task_index = g_xdag_pool_task_index;
						task = &g_xdag_pool_task[task_index & 1];

						//if (++m->shares_count > SHARES_PER_TASK_LIMIT) {   //if shares count limit is exceded it is considered as spamming and current connection is disconnected
						//	mess = "Spamming of shares";
						//	goto disconnect;	//TODO: get rid of gotos
						//}

						if (!(m->state & MINER_ADDRESS) || memcmp(m->id.data, m->data, sizeof(xdag_hashlow_t))) {
							xdag_time_t t;

							memcpy(m->id.data, m->data, sizeof(struct xdag_field));
							const int64_t pos = xdag_get_block_pos(m->id.data, &t);
							
							if (pos < 0) {
								m->state &= ~MINER_ADDRESS;
							} else {
								m->state |= MINER_ADDRESS;
							}
						} else {
							memcpy(m->id.data, m->data, sizeof(struct xdag_field));
						}

						xdag_hash_final(task->ctx0, m->data, sizeof(struct xdag_field), hash);
						pool_set_share(m, task, m->id.data, hash);
					}
				}
			}

			if (p->revents & POLLOUT) {
				struct xdag_field data[2];
				int nfld = 0;

				task_index = g_xdag_pool_task_index;
				task = &g_xdag_pool_task[task_index & 1];

				if (m->task_index < task_index) {
					m->task_index = task_index;
					//m->shares_count = 0;
					nfld = 2;
					memcpy(data, task->task, nfld * sizeof(struct xdag_field));
				} else if (!(m->state & MINER_BALANCE) && time(0) >= (m->task_time << 6) + 4) {
					m->state |= MINER_BALANCE;
					memcpy(data[0].data, m->id.data, sizeof(xdag_hash_t));
					data[0].amount = xdag_get_balance(data[0].data);
					nfld = 1;
				}

				if (nfld) {
					done = 1;

					for (int j = 0; j < nfld; ++j) {
						dfslib_encrypt_array(g_crypt, (uint32_t*)(data + j), DATA_SIZE, m->nfield_out++);
					}

					todo = write(p->fd, (void*)data, nfld * sizeof(struct xdag_field));
					
					if (todo != nfld * sizeof(struct xdag_field)) {
						mess = "write error"; 
						goto disconnect;
					}
				}
			}
		}

		if (!done) {
			sleep(1);
		}
	}

	return 0;
}

#define diff2pay(d, n) ((n) ? exp((d) / (n) - 20) * (n) : 0)

static double countpay(struct miner *m, int i, double *pay)
{
	double sum = 0;
	int n = 0;

	if (m->maxdiff[i] > 0) {
		sum += m->maxdiff[i]; m->maxdiff[i] = 0; n++;
	}

	*pay = diff2pay(sum, n);
	sum += m->prev_diff;
	n += m->prev_diff_count;
	m->prev_diff = 0;
	m->prev_diff_count = 0;
	
	return diff2pay(sum, n);
}

static int pay_miners(xdag_time_t t)
{
	struct xdag_field fields[12];
	struct xdag_block buf, *b;
	uint64_t *h, *nonce;
	xdag_amount_t reward = 0, direct = 0, fund = 0;
	struct miner *m;
	int64_t pos;
	int i, n, nminers, reward_ind = -1, key, defkey, nfields, nfld;
	double *diff, *prev_diff, sum, prev_sum, topay;

	nminers = g_miners_count;
	
	if (!nminers) return -1;
	
	n = t & (CONFIRMATIONS_COUNT - 1);
	h = g_xdag_mined_hashes[n];
	nonce = g_xdag_mined_nonce[n];
	xdag_amount_t balance = xdag_get_balance(h);
	
	if (!balance) return -2;
	
	xdag_amount_t pay = balance - (xdag_amount_t)(g_pool_fee * balance);
	if (!pay) return -3;
	
	reward = (xdag_amount_t)(balance * g_pool_reward);
	pay -= reward;
	
	if (g_pool_fund) {
		if (!(g_fund_miner.state & MINER_ADDRESS)) {
			xdag_time_t t;
			if (!xdag_address2hash(FUND_ADDRESS, g_fund_miner.id.hash) && xdag_get_block_pos(g_fund_miner.id.hash, &t) >= 0) {
				g_fund_miner.state |= MINER_ADDRESS;
			}
		}

		if (g_fund_miner.state & MINER_ADDRESS) {
			fund = balance * g_pool_fund;
			pay -= fund;
		}
	}

	key = xdag_get_key(h);
	if (key < 0) return -4;
	
	if (!xdag_wallet_default_key(&defkey)) return -5;
	
	nfields = (key == defkey ? 12 : 10);
	
	pos = xdag_get_block_pos(h, &t);
	if (pos < 0) return -6;
	
	b = xdag_storage_load(h, t, pos, &buf);
	if (!b) return -7;
	
	diff = malloc(2 * nminers * sizeof(double));
	if (!diff) return -8;
	
	prev_diff = diff + nminers;
	prev_sum = countpay(&g_local_miner, n, &sum);

	for (i = 0; i < nminers; ++i) {
		m = g_miners + i;
		
		if (m->state & MINER_FREE || !(m->state & MINER_ADDRESS)) continue;
		
		prev_diff[i] = countpay(m, n, &diff[i]);
		sum += diff[i];
		prev_sum += prev_diff[i];
		
		if (reward_ind < 0 && !memcmp(nonce, m->id.data, sizeof(xdag_hashlow_t))) {
			reward_ind = i;
		}
	}

	if (!prev_sum) return -9;
	
	if (sum > 0) {
		direct = balance * g_pool_direct;
		pay -= direct;
	}
	
	memcpy(fields[0].data, h, sizeof(xdag_hashlow_t));
	fields[0].amount = 0;

	for (nfld = 1, i = 0; i <= nminers; ++i) {
		if (i < nminers) {
			m = g_miners + i;
			
			if (m->state & MINER_FREE || !(m->state & MINER_ADDRESS)) continue;
			
			topay = pay * (prev_diff[i] / prev_sum);
			
			if (sum > 0) {
				topay += direct * (diff[i] / sum);
			}

			if (i == reward_ind) {
				topay += reward;
			}
		} else {
			m = &g_fund_miner;
			if (!(m->state & MINER_ADDRESS)) continue;
			topay = fund;
		}

		if (!topay) continue;
		
		memcpy(fields[nfld].data, m->id.data, sizeof(xdag_hashlow_t));
		fields[nfld].amount = topay;
		fields[0].amount += topay;
		
		xdag_log_xfer(fields[0].data, fields[nfld].data, topay);
		
		if (++nfld == nfields) {
			xdag_create_block(fields, 1, nfld - 1, 0, 0, NULL);
			nfld = 1;
			fields[0].amount = 0;
		}
	}

    if(nfld > 1) {
        xdag_create_block(fields, 1, nfld - 1, 0, 0, NULL);
    }

	return 0;
}

void *pool_block_thread(void *arg)
{
	xdag_time_t t0 = 0;
	struct xdag_block *b;
	int res;

	while(!g_xdag_sync_on) {
		sleep(1);
	}

	for (;;) {
		int done = 0;
		uint64_t ntask = g_xdag_pool_task_index;
		struct xdag_pool_task *task = &g_xdag_pool_task[ntask & 1];
		xdag_time_t t = task->task_time;

		if (t > t0) {
			uint64_t *h = g_xdag_mined_hashes[(t - CONFIRMATIONS_COUNT + 1) & (CONFIRMATIONS_COUNT - 1)];

			done = 1;
			t0 = t;
			
			res = pay_miners(t - CONFIRMATIONS_COUNT + 1);
			
			xdag_info("%s: %016llx%016llx%016llx%016llx t=%llx res=%d", (res ? "Nopaid" : "Paid  "),
				h[3], h[2], h[1], h[0], (t - CONFIRMATIONS_COUNT + 1) << 16 | 0xffff, res);
		}

		pthread_mutex_lock(&g_pool_mutex);

		if (g_firstb) {
			b = g_firstb;
			g_firstb = (struct xdag_block *)(uintptr_t)b->field[0].transport_header;
			if (!g_firstb) g_lastb = 0;
		} else {
			b = 0;
		}

		pthread_mutex_unlock(&g_pool_mutex);
		
		if (b) {
			done = 1;
			b->field[0].transport_header = 2;
			
			res = xdag_add_block(b);
			if (res > 0) {
				xdag_send_new_block(b);
			}
		}

		if (!done) sleep(1);
	}

	return 0;
}

/* gets pool parameters as a string, 0 - if the pool is disabled */
char *xdag_pool_get_config(char *buf)
{
	if (!g_xdag_pool) return 0;
	
	sprintf(buf, "%d:%.2lf:%.2lf:%.2lf:%d:%.2lf", g_max_miners_count, g_pool_fee * 100, g_pool_reward * 100,
			g_pool_direct * 100, g_max_miner_ip_count, g_pool_fund * 100);
	
	return buf;
}

/* sets pool parameters */
int xdag_pool_set_config(const char *pool_config)
{
	char buf[0x100], *lasts;

	if (!g_xdag_pool) return -1;
	strcpy(buf, pool_config);

	pool_config = strtok_r(buf, " \t\r\n:", &lasts);
	
	if (pool_config) {
		int open_max = sysconf(_SC_OPEN_MAX);

		sscanf(pool_config, "%d", &g_max_miners_count);

		if (g_max_miners_count < 0)
			g_max_miners_count = 0;
		else if (g_max_miners_count > MAX_MINERS_COUNT)
			g_max_miners_count = MAX_MINERS_COUNT;
		else if (g_max_miners_count > open_max - 64)
			g_max_miners_count = open_max - 64;
	}

	pool_config = strtok_r(0, " \t\r\n:", &lasts);
	if (pool_config) {
		sscanf(pool_config, "%lf", &g_pool_fee);
		
		g_pool_fee /= 100;
		
		if (g_pool_fee < 0)
			g_pool_fee = 0;
		
		if (g_pool_fee > 1)
			g_pool_fee = 1;
	}

	pool_config = strtok_r(0, " \t\r\n:", &lasts);
	if (pool_config) {
		sscanf(pool_config, "%lf", &g_pool_reward);
		
		g_pool_reward /= 100;
		
		if (g_pool_reward < 0)
			g_pool_reward = 0;
		if (g_pool_fee + g_pool_reward > 1)
			g_pool_reward = 1 - g_pool_fee;
	}

	pool_config = strtok_r(0, " \t\r\n:", &lasts);
	if (pool_config) {
		sscanf(pool_config, "%lf", &g_pool_direct);
		
		g_pool_direct /= 100;
		
		if (g_pool_direct < 0)
			g_pool_direct = 0;
		if (g_pool_fee + g_pool_reward + g_pool_direct > 1)
			g_pool_direct = 1 - g_pool_fee - g_pool_reward;
	}

	pool_config = strtok_r(0, " \t\r\n:", &lasts);
	if (pool_config) {
		sscanf(pool_config, "%d", &g_max_miner_ip_count);
		
		if (g_max_miner_ip_count <= 0)
			g_max_miner_ip_count = 1;
	}

	pool_config = strtok_r(0, " \t\r\n:", &lasts);
	if (pool_config) {
		sscanf(pool_config, "%lf", &g_pool_fund);
		
		g_pool_fund /= 100;
		
		if (g_pool_fund < 0)
			g_pool_fund = 0;
		if (g_pool_fee + g_pool_reward + g_pool_direct + g_pool_fund > 1)
			g_pool_fund = 1 - g_pool_fee - g_pool_reward - g_pool_direct;
	}

	return 0;
}

void *pool_net_thread(void *arg)
{
	const char *pool_arg = (const char*)arg;
	char buf[0x100];
	const char *mess, *mess1 = "";
	struct sockaddr_in peeraddr;
//	struct hostent *host;
	char *lasts;
	int res = 0, rcvbufsize = 1024, reuseaddr = 1, i, i0, count;
//	unsigned long nonblock = 1;
	struct linger linger_opt = { 1, 0 }; // Linger active, timeout 0
	socklen_t peeraddr_len = sizeof(peeraddr);
	struct miner *m;

	while (!g_xdag_sync_on) {
		sleep(1);
	}

	// Create a socket
	int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET) {
		mess = "cannot create a socket"; goto err;
	}

	if (fcntl(sock, F_SETFD, FD_CLOEXEC) == -1) {
		xdag_err("pool  : can't set FD_CLOEXEC flag on socket %d, %s\n", sock, strerror(errno));
	}

	// Fill in the address of server
	memset(&peeraddr, 0, sizeof(peeraddr));
	peeraddr.sin_family = AF_INET;

	// Resolve the server address (convert from symbolic name to IP number)
	strcpy(buf, pool_arg);
	pool_arg = strtok_r(buf, " \t\r\n:", &lasts);
	if (!pool_arg) {
		mess = "host is not given"; goto err;
	}

//	if (!strcmp(str, "any")) {
	peeraddr.sin_addr.s_addr = htonl(INADDR_ANY);
//	} else {
//		host = gethostbyname(str);
//		if (!host || !host->h_addr_list[0]) { mess = "cannot resolve host ", mess1 = str; res = h_errno; goto err; }
//		// Write resolved IP address of a server to the address structure
//		memmove(&peeraddr.sin_addr.s_addr, host->h_addr_list[0], 4);
//	}

	// Resolve port
	pool_arg = strtok_r(0, " \t\r\n:", &lasts);
	if (!pool_arg) {
		mess = "port is not given"; goto err;
	}
	peeraddr.sin_port = htons(atoi(pool_arg));

	res = bind(sock, (struct sockaddr*)&peeraddr, sizeof(peeraddr));
	if (res) {
		mess = "cannot bind a socket"; 
		goto err;
	}

	// Set the "LINGER" timeout to zero, to close the listen socket
	// immediately at program termination.
	setsockopt(sock, SOL_SOCKET, SO_LINGER, (char*)&linger_opt, sizeof(linger_opt));
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseaddr, sizeof(int));
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbufsize, sizeof(int));

	pool_arg = strtok_r(0, " \t\r\n", &lasts);
	if (pool_arg) {
		xdag_pool_set_config(pool_arg);
	}

	// Now, listen for a connection
	res = listen(sock, MAX_MINERS_COUNT);    // "1" is the maximal length of the queue
	if (res) {
		mess = "cannot listen"; 
		goto err;
	}

	for (;;) {
		// Accept a connection (the "accept" command waits for a connection with
		// no timeout limit...)
		int fd = accept(sock, (struct sockaddr*)&peeraddr, &peeraddr_len);
		if (fd < 0) {
			mess = "cannot accept"; 
			goto err;
		}
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbufsize, sizeof(int));
//		ioctl(fd, FIONBIO, (char*)&nonblock);

		xdag_time_t t = xdag_main_time();

		for (i = 0, count = 1, i0 = -1; i < g_miners_count; ++i) {
			m = g_miners + i;

			if (m->state & MINER_FREE) {
				if (i0 < 0)
					i0 = i;
			} else if (m->state & MINER_ARCHIVE && t - m->task_time > CONFIRMATIONS_COUNT) {
				if (i0 < 0)
					i0 = i;
			} else if (m->ip == peeraddr.sin_addr.s_addr && ++count > g_max_miner_ip_count) {
				goto closefd;
			}
		}

		if (i0 >= 0) i = i0;
		
		if (i >= g_max_miners_count) {
 closefd:
			close(fd);
		} else {
			m = g_miners + i;
			struct pollfd *p = g_fds + i;
			p->fd = fd;
			p->events = POLLIN | POLLOUT;
			p->revents = 0;

			memset(m, 0, sizeof(struct miner));
			
			int j = m->ip = peeraddr.sin_addr.s_addr;
			m->port = peeraddr.sin_port;
			
			if (i == g_miners_count)
				g_miners_count++;
			
			xdag_info("Pool  : miner %d connected from %u.%u.%u.%u:%u", i,
						   j & 0xff, j >> 8 & 0xff, j >> 16 & 0xff, j >> 24 & 0xff, ntohs(m->port));
		}
	}

	return 0;

 err:
	xdag_err("pool  : %s %s (error %d)", mess, mess1, res);
	
	return 0;
}

static int send_to_pool(struct xdag_field *fld, int nfld)
{
	struct xdag_field f[XDAG_BLOCK_FIELDS];
	xdag_hash_t h;
	struct miner *m = &g_local_miner;
	int i, res, todo = nfld * sizeof(struct xdag_field), done = 0;

	if (g_socket < 0) {
		pthread_mutex_unlock(&g_pool_mutex);
		return -1;
	}

	memcpy(f, fld, todo);

	if (nfld == XDAG_BLOCK_FIELDS) {
		f[0].transport_header = 0;
		
		xdag_hash(f, sizeof(struct xdag_block), h);
		
		f[0].transport_header = HEADER_WORD;
		
		uint32_t crc = crc_of_array((uint8_t*)f, sizeof(struct xdag_block));
		
		f[0].transport_header |= (uint64_t)crc << 32;
	}

	for (i = 0; i < nfld; ++i) {
		dfslib_encrypt_array(g_crypt, (uint32_t*)(f + i), DATA_SIZE, m->nfield_out++);
	}

	while (todo) {
		struct pollfd p;
		
		p.fd = g_socket;
		p.events = POLLOUT;
		
		if (!poll(&p, 1, 1000)) continue;
		
		if (p.revents & (POLLHUP | POLLERR)) {
			pthread_mutex_unlock(&g_pool_mutex);
			return -1;
		}

		if (!(p.revents & POLLOUT)) continue;
		
		res = write(g_socket, (uint8_t*)f + done, todo);
		if (res <= 0) {
			pthread_mutex_unlock(&g_pool_mutex);
			return -1;
		}

		done += res, todo -= res;
	}

	pthread_mutex_unlock(&g_pool_mutex);
	
	if (nfld == XDAG_BLOCK_FIELDS) {
		xdag_info("Sent  : %016llx%016llx%016llx%016llx t=%llx res=%d",
					   h[3], h[2], h[1], h[0], fld[0].time, 0);
	}

	return 0;
}

/* send block to network via pool */
int xdag_send_block_via_pool(struct xdag_block *b)
{
	if (g_socket < 0) return -1;
	
	pthread_mutex_lock(&g_pool_mutex);
	
	return send_to_pool(b->field, XDAG_BLOCK_FIELDS);
}
static int print_miner(FILE *out, int n, struct miner *m)
{
	double sum = m->prev_diff;
	int count = m->prev_diff_count;
	char buf[32], buf2[64];
	uint32_t ip = m->ip;
	
	for (int j = 0; j < CONFIRMATIONS_COUNT; ++j) {
		if (m->maxdiff[j] > 0) {
			sum += m->maxdiff[j]; 
			count++;
		}
	}
	
	sprintf(buf, "%u.%u.%u.%u:%u", ip & 0xff, ip >> 8 & 0xff, ip >> 16 & 0xff, ip >> 24 & 0xff, ntohs(m->port));
	sprintf(buf2, "%llu/%llu", (unsigned long long)m->nfield_in * sizeof(struct xdag_field),
			(unsigned long long)m->nfield_out * sizeof(struct xdag_field));
	fprintf(out, "%3d. %s  %s  %-21s  %-16s  %lf\n", n, xdag_hash2address(m->id.data),
			(m->state & MINER_FREE ? "free   " : (m->state & MINER_ARCHIVE ? "archive" :
												  (m->state & MINER_ADDRESS ? "active " : "badaddr"))), buf, buf2, diff2pay(sum, count));
	
	return m->state & (MINER_FREE | MINER_ARCHIVE) ? 0 : 1;
}

/* output to the file a list of miners */
int xdag_print_miners(FILE *out)
{
	fprintf(out, "List of miners:\n"
			" NN  Address for payment to            Status   IP and port            in/out bytes      nopaid shares\n"
			"------------------------------------------------------------------------------------------------------\n");
	int res = print_miner(out, -1, &g_local_miner);

	for (int i = 0; i < g_miners_count; ++i) {
		res += print_miner(out, i, g_miners + i);
	}

	fprintf(out,
			"------------------------------------------------------------------------------------------------------\n"
			"Total %d active miners.\n", res);
	
	return res;
}
