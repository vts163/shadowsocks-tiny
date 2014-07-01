#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "log.h"
#include "common.h"

struct pollfd *clients;
int nfds = MAX_CONNECTION;
struct link *link_head;

void _pr_link(const char *level, struct link *ln)
{
	enum link_state state = ln->state;
	char state_str[512] = {'\0'};

	if (state & LOCAL && state & SERVER)
		strcat(state_str, "linked");
	else if (state & LOCAL)
		strcat(state_str, "local");
	else if (state & SERVER)
		strcat(state_str, "server");

	if (state & SS_UDP)
		strcat(state_str, ", udp");

	if (state & SS_IV_SENT && state & SS_IV_RECEIVED)
		strcat(state_str, ", iv exchanged");
	else if (state & SS_IV_SENT)
		strcat(state_str, ", iv sent");
	else if (state & SS_IV_RECEIVED)
		strcat(state_str, ", iv received");

	if (state & SS_TCP_HEADER_SENT)
		strcat(state_str, ", ss tcp header sent");
	else if (state & SS_TCP_HEADER_RECEIVED)
		strcat(state_str, ", ss tcp header received");
	else if (state & SOCKS5_CMD_REPLY_SENT)
		strcat(state_str, ", socks5 cmd reply sent");
	else if (state & SOCKS5_CMD_REQUEST_RECEIVED)
		strcat(state_str, ", socks5 cmd request received");
	else if (state & SOCKS5_AUTH_REPLY_SENT)
		strcat(state_str, ", socks5 auth reply sent");
	else if (state & SOCKS5_AUTH_REQUEST_RECEIVED)
		strcat(state_str, ", socks5 auth request received");

	if (state & LOCAL_SEND_PENDING)
		strcat(state_str, ", local send pending");

	if (state & LOCAL_READ_PENDING)
		strcat(state_str, ", local read pending");

	if (state & SERVER_READ_PENDING)
		strcat(state_str, ", server read pending");

	if (state & SERVER_SEND_PENDING)
		strcat(state_str, ", server_send_pending");

	printf("%s: state: %s\n"
	       "%s: local sockfd: %d; server sockfd: %d; "
	       "text len: %d; cipher len: %d;\n",
	       level, state_str,
	       level, ln->local_sockfd, ln->server_sockfd,
	       ln->text_len, ln->cipher_len);
}

void pr_link_debug(struct link *ln)
{
	if (!debug)
		return;

	_pr_link("debug", ln);
}

void pr_link_info(struct link *ln)
{
	if (!verbose)
		return;

	_pr_link("info", ln);
}

void pr_link_warn(struct link *ln)
{
	_pr_link("WARNING", ln);
}

void poll_init(void)
{
	int i;

	clients = calloc(nfds, sizeof(struct pollfd));
	if (clients == NULL)
		pr_exit("%s: calloc failed", __func__);

	for (i = 0; i < nfds; i++)
		clients[i].fd = -1;
}

void poll_events_string(short events, char *events_str)
{
	if (events & POLLIN) {
		if (strlen(events_str) == 0)
			strcat(events_str, "POLLIN");
		else
			strcat(events_str, " POLLIN");
	}

	if (events & POLLOUT) {
		if (strlen(events_str) == 0)
			strcat(events_str, "POLLOUT");
		else
			strcat(events_str, " POLLOUT");
	}
}

int poll_set(int sockfd, short events)
{
	int i;
	char events_str[42] = {'\0'};

	poll_events_string(events, events_str);

	/* i == 0 is listen sockfd, it's not needed to be checked by now */
	for (i = 1; i < nfds; i++) {
		if (clients[i].fd == sockfd) {
			clients[i].events = events;
			return 0;
		}
	}

	for (i = 1; i < nfds; i++) {
		if (clients[i].fd == -1) {
			clients[i].fd = sockfd;
			clients[i].events = events;
			return 0;
		}
	}

	sock_warn(sockfd, "too many connections!");
	return -1;
}

int poll_add(int sockfd, short events)
{
	int i;
	char events_str[42] = {'\0'};

	poll_events_string(events, events_str);

	/* i == 0 is listen sockfd, it's not needed to be checked by now */
	for (i = 1; i < nfds; i++)
		if (clients[i].fd == sockfd)
			break;

	if (i == nfds) {
		sock_warn(sockfd, "%s: not found", __func__);
		return poll_set(sockfd, events);
	}

	clients[i].events |= events;
	return 0;
}

int poll_rm(int sockfd, short events)
{
	int i;
	char events_str[42] = {'\0'};

	poll_events_string(events, events_str);

	/* i == 0 is listen sockfd, it's not needed to be checked by now */
	for (i = 1; i < nfds; i++)
		if (clients[i].fd == sockfd)
			break;

	if (i == nfds) {
		sock_warn(sockfd, "%s: not found", __func__);
		return poll_set(sockfd, events);
	}

	clients[i].events &= ~events;

	return 0;
}

int poll_del(int sockfd)
{
	int i;

	for (i = 1; i < nfds; i++) {
		if (clients[i].fd == sockfd) {
			clients[i].fd = -1;
			return 0;
		}
	}

	pr_warn("%s: sockfd(%d) not in poll\n", __func__, sockfd);
	return -1;
}

int get_events(int sockfd)
{
	int i;

	/* i == 0 is listen sockfd, it's not needed to be checked by now */
	for (i = 1; i < nfds; i++)
		if (clients[i].fd == sockfd)
			break;

	if (i == nfds) {
		sock_warn(sockfd, "%s: not in poll", __func__);
		return -1;
	}

	return clients[i].events;
}

/**
 * time_out - check if it's timed out
 *
 * @this: the time_t we want to compare(usually is NOW)
 * @that: the time_t we want to check
 * @value: how long we think it's a timeout
 *
 * Return: 0 means time out, -1 means not time out
 */
static int time_out(time_t this, time_t that, double value)
{
	if (difftime(this, that) > value)
		return 0;
	else
		return -1;
}

void reaper(void)
{
	double value;
	struct link *next = link_head;
	struct link *ln;
	time_t now = time(NULL);
	static time_t checked = (time_t)-1;

	if (checked == (time_t)-1) {
		checked = now;
	} else if (time_out(now, checked, TCP_READ_TIMEOUT) == -1) {
		return;
	} else {
		checked = now;
	}

	while (next) {
		ln = next;
		next = ln->next;

		if (ln->state & SERVER)
			value = TCP_READ_TIMEOUT;
		else
			value = TCP_CONNECT_TIMEOUT;

		if (time_out(now, ln->time, value) == 0) {
			pr_info("%s: timeout, closing\n", __func__);
			pr_link_debug(ln);
			destroy_link(ln);
		}
	}
}

struct link *create_link(int sockfd, const char *type)
{
	struct link *ln;
	struct link *head = link_head;

	ln = calloc(1, sizeof(*ln));
	if (ln == NULL)
		goto err;

	ln->text = malloc(TEXT_BUF_SIZE);
	if (ln->text == NULL)
		goto err;

	ln->cipher = malloc(CIPHER_BUF_SIZE);
	if (ln->cipher == NULL)
		goto err;

	/* cipher to encrypt local data */
	ln->local_ctx = EVP_CIPHER_CTX_new();
	if (ln->local_ctx == NULL)
		goto err;

	/* cipher to decrypt server data */
	ln->server_ctx = EVP_CIPHER_CTX_new();
	if (ln->server_ctx == NULL)
		goto err;

	ln->state |= LOCAL;

	if (strcmp(type, "client") == 0)
		ln->state |= SS_CLIENT;
	else if (strcmp(type, "server") == 0)
		ln->state |= SS_SERVER;

	ln->local_sockfd = sockfd;
	ln->server_sockfd = -1;

	if (head) {
		while (head->next != NULL)
			head = head->next;
		head->next = ln;
	} else {
		link_head = ln;
	}

	ln->time = time(NULL);
	return ln;

err:
	if (ln->local_ctx)
		EVP_CIPHER_CTX_free(ln->local_ctx);

	if (ln->text)
		free(ln->text);

	if (ln->cipher)
		free(ln->cipher);

	if (ln)
		free(ln);

	sock_warn(sockfd, "%s: failed", __func__);
	return NULL;
}

struct link *get_link(int sockfd)
{
	struct link *head = link_head;

	while (head) {
		if (head->local_sockfd == sockfd ||
		    head->server_sockfd == sockfd) {
			return head;
		} else {
			head = head->next;
		}
	}

	sock_warn(sockfd, "%s: failed", __func__);
	return NULL;
}

static int unlink_link(struct link *ln)
{
	struct link *head = link_head;
	struct link *previous = link_head;

	if (head == NULL) {
		pr_warn("%s: link list is empty\n", __func__);
		return -1;
	} else {
		while (head) {
			if (head->local_sockfd == ln->local_sockfd &&
			    head->server_sockfd == ln->server_sockfd) {
				previous->next = head->next;

				if (previous == head) {
					/* the link we want to unlink
				         * is link_head */
					link_head = head->next;
					goto out;
				}

				goto out;
			}

			previous = head;
			head = head->next;
		}
	}

	pr_link_warn(ln);
	pr_warn("%s failed: link not found\n", __func__);
	return -1;

out:
	return 0;
}

static void free_link(struct link *ln)
{
	if (ln->text)
		free(ln->text);

	if (ln->cipher)
		free(ln->cipher);

	if (ln->local_ctx)
		EVP_CIPHER_CTX_free(ln->local_ctx);

	if (ln->server_ctx)
		EVP_CIPHER_CTX_free(ln->server_ctx);

	if (ln)
		free(ln);
}

void destroy_link(struct link *ln)
{
	unlink_link(ln);
	poll_del(ln->local_sockfd);
	poll_del(ln->server_sockfd);
	close(ln->local_sockfd);
	close(ln->server_sockfd);
	free_link(ln);
}

/* for udp, we just bind it, since udp can't listen */
int do_listen(struct addrinfo *info, const char *type_str)
{
	int sockfd, type;
	struct addrinfo *lp = info;

	if (strcmp(type_str, "tcp") == 0)
		type = SOCK_STREAM;
	else if (strcmp(type_str, "udp") == 0)
		type = SOCK_DGRAM;
	else
		pr_exit("%s: unknown socket type\n", __func__);

	while (lp) {
		if (lp->ai_socktype == type) {
			type |= SOCK_NONBLOCK;
			sockfd = socket(lp->ai_family, type, 0);
			if (sockfd == -1)
				goto err;

			if (bind(sockfd, lp->ai_addr, lp->ai_addrlen) == -1)
				goto err;

			if (type & SOCK_STREAM)
				if (listen(sockfd, SOMAXCONN) == -1)
					goto err;

			return sockfd;
		}

		lp = lp->ai_next;
	}

err:
	err_exit("do_listen");
}

int connect_server(struct link *ln)
{
	int ret, sockfd, type;
	struct addrinfo *ai = ln->server;

	if (ln->server_sockfd != -1) {
		pr_warn("%s is called twice on link, "
			"return without doing anything\n",
			__func__);
		return 0;
	}

	if (ln->state & SS_UDP)
		type = SOCK_DGRAM;
	else
		type = SOCK_STREAM;

	while (ai) {
		if (ai->ai_socktype == type) {
			type |= SOCK_NONBLOCK;
			sockfd = socket(ai->ai_family, type, 0);
			if (sockfd == -1)
				goto err;

			ln->server_sockfd = sockfd;
			ln->time = time(NULL);
			ret = connect(sockfd, ai->ai_addr, ai->ai_addrlen);
			if (ret == -1) {
				/* it's ok to return inprogress, will
				 * handle it later */
				if (errno == EINPROGRESS) {
					poll_set(sockfd, POLLOUT);
					return 0;
				} else {
					goto err;
				}
			}

			/* sucessfully connected */
			ln->state |= SERVER;
			poll_set(sockfd, POLLIN);
			sock_info(sockfd, "%s: connected", __func__);
			return 0;
		}

		ai = ai->ai_next;
	}

err:
	perror("connect_server");
	return -1;
}

int add_data(int sockfd, struct link *ln,
	     const char *type, char *data, int size)
{
	char *buf;
	int len;

	if (strcmp(type, "text") == 0) {
		buf = ln->text;
		len = ln->text_len;

		if (len + size > TEXT_BUF_SIZE) {
			sock_warn(sockfd, "%s: data exceed max length(%d/%d)",
				  __func__, len + size, TEXT_BUF_SIZE);
			return -1;
		}

		ln->text_len += size;
	} else if (strcmp(type, "cipher") == 0) {
		buf = ln->cipher;
		len = ln->cipher_len;

		if (len + size > CIPHER_BUF_SIZE) {
			sock_warn(sockfd, "%s: data exceed max length(%d/%d)",
				  __func__, len + size, CIPHER_BUF_SIZE);
			return -1;
		}

		ln->cipher_len += size;
	} else {
		sock_warn(sockfd, "%s: unknown type", __func__);
		return -1;
	}

	/* if len == 0, no data need to be moved */
	if (len > 0)
		memmove(buf + size, buf, len);

	memcpy(buf, data, size);
	return 0;
}

int rm_data(int sockfd, struct link *ln, const char *type, int size)
{
	char *buf;
	int len;

	if (strcmp(type, "text") == 0) {
		buf = ln->text;

		if (ln->text_len < size) {
			sock_warn(sockfd, "%s: size is too big(%d/%d)",
				  __func__, size, ln->text_len);
			return -1;
		}

		ln->text_len -= size;
		len = ln->text_len;
	} else if (strcmp(type, "cipher") == 0) {
		buf = ln->cipher;
		
		if (ln->cipher_len < size) {
			sock_warn(sockfd, "%s: size is too big(%d/%d)",
				  __func__, size, ln->cipher_len);
			return -1;
		}

		ln->cipher_len -= size;
		len = ln->cipher_len;
	} else {
		sock_warn(sockfd, "%s: unknown type", __func__);
		return -1;
	}

	memmove(buf, buf + size, len);

	return 0;
}

int check_ss_header(int sockfd, struct link *ln)
{
	int ret;
	char atyp;
	char addr[256];
	unsigned short port;
	char port_str[6];
	short addr_len;
	struct ss_header *req;
	struct addrinfo hint;
	struct addrinfo *res;

	memset(&hint, 0, sizeof(hint));

	req = (void *)ln->text;

	if (ln->state & SS_UDP) {
		hint.ai_socktype = SOCK_DGRAM;
	} else {
		hint.ai_socktype = SOCK_STREAM;
	}
	
	atyp = req->atyp;
	if (atyp == SOCKS5_ADDR_IPV4) {
		addr_len = 4;

		/* atyp(1) + ipv4_addrlen(4) + port(2) */
		if (ln->text_len < 7) {
			goto too_short;
		}

		hint.ai_family = AF_INET;

		if (inet_ntop(AF_INET, req->dst, addr, sizeof(addr)) == NULL) {
			sock_warn(sockfd, "%s: inet_ntop() %s",
				  __func__, strerror(errno));
			return -1;
		}

		port = ntohs(*(unsigned short *)(req->dst + addr_len));
	} else if (atyp == SOCKS5_ADDR_DOMAIN) {
		addr_len = req->dst[0];

		/* atyp(1) + addr_size(1) + domain_len(addr_len) + port(2) */
		if (ln->text_len < 1 + 1 + addr_len + 2)
			goto too_short;

		hint.ai_family = AF_UNSPEC;
		strncpy(addr, req->dst + 1, addr_len);
		addr[addr_len] = '\0';
		port = ntohs(*(unsigned short *)(req->dst + addr_len + 1));
		/* to compute the right data length(except header) */
		addr_len += 1;
	} else if (atyp == SOCKS5_ADDR_IPV6) {
		hint.ai_family = AF_INET6;
		addr_len = 16;

		if (inet_ntop(AF_INET6, req->dst, addr, sizeof(addr)) == NULL) {
			sock_warn(sockfd, "%s: inet_ntop() %s",
				  __func__, strerror(errno));
			return -1;
		}

		port = ntohs(*(unsigned short *)(req->dst + addr_len));
	} else {
		sock_warn(sockfd, "%s: ATYP(%d) isn't legal");
		return -1;
	}

	sock_info(sockfd, "%s: remote address: %s; port: %d",
		  __func__, addr, port);
	sprintf(port_str, "%d", port);
	ret = getaddrinfo(addr, port_str, &hint, &res);
	if (ret != 0) {
		sock_warn(sockfd, "getaddrinfo error: %s", gai_strerror(ret));
		return -1;
	}

	if (ln->state & SS_UDP) {
		ln->ss_header_len = ln->text_len;
	} else {
		ln->ss_header_len = 1 + addr_len + 2;
		if (rm_data(sockfd, ln, "text", ln->ss_header_len) == -1)
			return -1;
	}

	ln->server = res;

	if (connect_server(ln) == -1)
		return -1;

	return 0;

too_short:
	sock_warn(sockfd, "%s: text is too short",
		  __func__);
	return -1;
}

int check_socks5_auth_header(int sockfd, struct link *ln)
{
	unsigned short i;
	struct socks5_auth_request *req;

	if (ln->text_len < 3) {
		sock_warn(sockfd, "%s: text len is smaller than auth request",
			  __func__);
		return -1;
	}

	req = (void *)ln->text;

	if (req->ver != 0x05) {
		sock_warn(sockfd, "%s: VER(%d) is not 5",
			  __func__, req->ver);
		return -1;
	}

	i = req->nmethods;
	if ((i + 2) != ln->text_len) {
		sock_warn(sockfd, "%s: NMETHODS(%d) isn't correct",
			  __func__, i);
		return -1;
	}

	while (i-- > 0)
		if (req->methods[i] == 0x00)
			return 0;

	sock_warn(sockfd, "%s: only support NO AUTHENTICATION");
	return -1;
}

int check_socks5_cmd_header(int sockfd, struct link *ln)
{
	char cmd, atyp;
	int ss_header_len;
	struct socks5_cmd_request *req;

	req = (void *)ln->text;

	if (req->ver != 0x05) {
		sock_warn(sockfd, "%s: VER(%d) is not 5",
			  __func__, req->ver);
		return -1;
	}

	cmd = req->cmd;
	if (cmd == SOCKS5_CONNECT) {
		/* nothing to do */
	} else if (cmd == SOCKS5_UDP_ASSOCIATE) {
		ln->state |= SS_UDP;
		sock_info(sockfd, "%s: udp associate received",
			  __func__);
		sock_warn(sockfd, "udp socks5 not supported(for now)");
		return -1;
	} else {
		sock_warn(sockfd, "%s: CMD(%d) isn't supported", cmd);
		return -1;
	}

	if (req->rsv != 0x00) {
		sock_warn(sockfd, "%s: RSV(%d) is not 0x00");
		return -1;
	}

	atyp = req->atyp;
	/* the following magic number 3 is actually ver(1) + cmd(1) +
	 * rsv(1) */
	if (atyp == SOCKS5_ADDR_IPV4) {
		/* atyp(1) + ipv4(4) + port(2) */
		ss_header_len = 1 + 4 + 2;

		if (ln->text_len < ss_header_len + 3)
			goto too_short;
	} else if (atyp == SOCKS5_ADDR_DOMAIN) {
		/* atyp(1) + addr_size(1) + domain_length(req->dst[0]) +
		 * port(2) */
		ss_header_len = 1 + 1 + req->dst[0] + 2;

		if (ln->text_len < ss_header_len + 3)
			goto too_short;
	} else if (atyp == SOCKS5_ADDR_IPV6) {
		/* atyp(1) + ipv6_addrlen(16) + port(2) */
		ss_header_len = 1 + 16 + 2;

		if (ln->text_len < ss_header_len + 3)
			goto too_short;
	} else {
		sock_warn(sockfd, "%s: ATYP(%d) isn't legal");
		return -1;
	}

	ln->ss_header_len = ss_header_len;

	/* remove VER, CMD, RSV for shadowsocks protocol */
	if (rm_data(sockfd, ln, "text", 3) == -1)
		return -1;

        /* copy ss tcp header to cipher buffer, it will be sent
	 * together with data received from local */
	memcpy(ln->cipher, ln->text, ln->ss_header_len);

	/* all seem okay, connect to server! */
	if (connect_server(ln) == -1)
		return -1;

	return 0;

too_short:
	sock_warn(sockfd, "%s: text is too short",
		  __func__);
	return -1;
}

int create_socks5_auth_reply(int sockfd, struct link *ln, bool ok)
{
	struct socks5_auth_reply rep;

	rep.ver = 0x05;

	if (ok)
		rep.method = SOCKS5_METHOD_NOT_REQUIRED;
	else
		rep.method = SOCKS5_METHOD_ERROR;

	ln->text_len = 0;

	if (add_data(sockfd, ln, "text", (void *)&rep, sizeof(rep)) == -1)
		return -1;

	return 0;
}

int create_socks5_cmd_reply(int sockfd, struct link *ln, int cmd)
{
	unsigned short port;
	void *addrptr;
	int addr_len;
	struct sockaddr_storage ss_addr;
	int len = sizeof(struct sockaddr_storage);
	struct addrinfo *ai = ln->server;
	struct socks5_cmd_reply *rep = (void *)ln->text;

	rep->ver = 0x05;
	rep->rep = cmd;
	rep->rsv = 0x00;

	if (getpeername(sockfd, (struct sockaddr *)&ss_addr,
			(void *)&len) == -1) {
		sock_warn(sockfd, "%s: getsockname() %s",
			  __func__, strerror(errno));
		return -1;
	}

	while (ai) {
		if (ai->ai_family == ss_addr.ss_family) {
			if (ai->ai_family == AF_INET) {
				rep->atyp = SOCKS5_ADDR_IPV4;
				port = ((SA_IN *)ai->ai_addr)->sin_port;
				addrptr = &((SA_IN *)ai->ai_addr)->sin_addr;
				addr_len = sizeof(struct in_addr);
			} else {
				rep->atyp = SOCKS5_ADDR_IPV6;
				port = ((SA_IN6 *)ai->ai_addr)->sin6_port;
				addrptr = &((SA_IN6 *)ai->ai_addr)->sin6_addr;
				addr_len = sizeof(struct in6_addr);
			}

			break;
		}

		ai = ai->ai_next;
	}

	if (ai == NULL)
		return -1;

	memcpy(rep->bnd, addrptr, addr_len);
	memcpy(rep->bnd + addr_len, (void *)&port, sizeof(short));

	len = sizeof(*rep) + addr_len + 2;
	ln->text_len = 0;
	if (add_data(sockfd, ln, "text", (void *)rep, len) == -1)
		return -1;

	return 0;
}

int do_read(int sockfd, struct link *ln, const char *type, int offset)
{
	int ret, len;
	char *buf;

	if (strcmp(type, "text") == 0) {
		buf = ln->text + offset;
		len = TEXT_BUF_SIZE - offset;
	} else if (strcmp(type, "cipher") == 0) {
		buf = ln->cipher + offset;
		/* cipher read only accept text buffer length data, or
		 * it may overflow text buffer */
		len = TEXT_BUF_SIZE - offset;
	} else {
		sock_warn(sockfd, "%s: unknown type %s",
			  __func__, type);
		return -2;
	}

	ret = recv(sockfd, buf, len, 0);
	if (ret == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			sock_warn(sockfd, "%s(%s): recv() %s",
				  __func__, type, strerror(errno));
			return -2;
		}

		poll_add(sockfd, POLLIN);
		return -1;
	} else if (ret == 0) {
		/* recv() returned 0 means the peer has shut down,
		 * return -2 to let the caller do the closing work */
		sock_info(sockfd, "%s(%s): the peer has shut down",
			  __func__, type);
		return -2;
	}

	ln->time = time(NULL);

	if (strcmp(type, "text") == 0) {
		ln->text_len = ret + offset;
	} else if (strcmp(type, "cipher") == 0) {
		ln->cipher_len = ret + offset;
	}

	sock_debug(sockfd, "%s(%s): offset = %d, buf_len = %d, recv %d bytes",
		   __func__, type, offset, len, ret);
	pr_link_debug(ln);

	return ret;
}

int do_send(int sockfd, struct link *ln, const char *type, int offset)
{
	int ret, len;
	char *buf;

	if (strcmp(type, "text") == 0) {
		buf = ln->text + offset;
		len = ln->text_len - offset;
	} else if (strcmp(type, "cipher") == 0) {
		buf = ln->cipher + offset;
		len = ln->cipher_len - offset;
	} else {
		sock_warn(sockfd, "%s: unknown type %s",
			  __func__, type);
		return -2;
	}

	ret = send(sockfd, buf, len, 0);
	if (ret == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != ENOTCONN && errno != EPIPE) {
			sock_warn(sockfd, "%s(%s): send() %s",
				  __func__, type, strerror(errno));
			return -2;
		} else {
			/* wait for unblocking send, or wait for
			 * connection finished */
			poll_add(sockfd, POLLOUT);
			return -1;
		}
	}

	ln->time = time(NULL);

	if (rm_data(sockfd, ln, type, ret) == -1)
		return -2;

	sock_debug(sockfd, "%s(%s): offset = %d, buf_len = %d, send %d bytes",
		   __func__, type, offset, len, ret);
	pr_link_debug(ln);

	if (ret != len) {
		poll_add(sockfd, POLLOUT);
		sock_info(sockfd, "%s(%s): send() partial send(%d/%d)",
			  __func__, type, ret, len);
		return -1;
	}
		
	poll_set(sockfd, POLLIN);
	return ret;
}
