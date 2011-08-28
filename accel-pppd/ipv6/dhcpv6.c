#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "triton.h"
#include "mempool.h"
#include "log.h"
#include "ppp.h"
#include "ipdb.h"
#include "events.h"

#include "memdebug.h"

#include "dhcpv6.h"

#define BUF_SIZE 65536
#define MAX_DNS_COUNT 3

static int conf_verbose;
static int conf_pref_lifetime = -1;
static int conf_valid_lifetime = -1;
static struct in6_addr conf_dns[MAX_DNS_COUNT];
static int conf_dns_count;
static void *conf_dnssl;
static int conf_dnssl_size;

struct dhcpv6_pd
{
	struct ppp_pd_t pd;
	struct dhcpv6_opt_clientid *clientid;
	struct dhcpv6_opt_serverid serverid;
	uint32_t addr_iaid;
	uint32_t dp_iaid;
	struct ipv6db_prefix_t *ipv6_dp;
};

static struct triton_md_handler_t dhcpv6_hnd;
static struct triton_context_t dhcpv6_ctx;

static uint8_t *buf;
static void *pd_key;

static void ev_ppp_started(struct ppp_t *ppp)
{
	struct ipv6_mreq mreq;
	struct dhcpv6_pd *pd;
	time_t t, t0;
	struct tm tm;

	if (!ppp->ipv6)
		return;

	time(&t);
	localtime_r(&t, &tm);

	tm.tm_year = 100;
	tm.tm_mon = 0;
	tm.tm_mday = 1;
	tm.tm_hour = 0;
	tm.tm_min = 0;
	tm.tm_sec = 0;

	t0 = mktime(&tm);

	pd = _malloc(sizeof(*pd));
	memset(pd, 0, sizeof(*pd));
	
	pd->pd.key = &pd_key;

	pd->serverid.hdr.code = htons(D6_OPTION_SERVERID);
	pd->serverid.hdr.len = htons(16);
	pd->serverid.duid.type = htons(DUID_LLT);
	pd->serverid.duid.u.llt.htype = htons(27);
	pd->serverid.duid.u.llt.time = htonl(t - t0);
	*(uint64_t *)pd->serverid.duid.u.llt.addr = ppp->ipv6->intf_id;

	list_add_tail(&pd->pd.entry, &ppp->pd_list);

	memset(&mreq, 0, sizeof(mreq));
	mreq.ipv6mr_interface = ppp->ifindex;
	mreq.ipv6mr_multiaddr.s6_addr32[0] = htonl(0xff020000);
	mreq.ipv6mr_multiaddr.s6_addr32[3] = htonl(0x010002);

	if (setsockopt(dhcpv6_hnd.fd, SOL_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) {
		log_ppp_error("dhcpv6: failed to join to All_DHCP_Relay_Agents_and_Servers\n");
		return;
	}
}

static struct dhcpv6_pd *find_pd(struct ppp_t *ppp)
{
	struct ppp_pd_t *pd;

	list_for_each_entry(pd, &ppp->pd_list, entry) {
		if (pd->key == &pd_key)
			return container_of(pd, struct dhcpv6_pd, pd);
	}

	return NULL;
}

static void ev_ppp_finished(struct ppp_t *ppp)
{
	struct dhcpv6_pd *pd = find_pd(ppp);

	if (!pd)
		return;

	list_del(&pd->pd.entry);

	if (pd->clientid)
		_free(pd->clientid);
	
	if (pd->ipv6_dp)
		ipdb_put_ipv6_prefix(ppp, pd->ipv6_dp);

	_free(pd);
}

static void dhcpv6_send(struct dhcpv6_packet *reply)
{
	struct sockaddr_in6 addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(DHCPV6_CLIENT_PORT);
	addr.sin6_addr.s6_addr32[0] = htons(0xfe80);
	*(uint64_t *)(addr.sin6_addr.s6_addr + 8) = reply->ppp->ipv6->peer_intf_id;
	addr.sin6_scope_id = reply->ppp->ifindex;

	sendto(dhcpv6_hnd.fd, reply->hdr, reply->endptr - (void *)reply->hdr, 0, (struct sockaddr *)&addr, sizeof(addr));
}

static void build_addr(struct ipv6db_addr_t *a, uint64_t intf_id, struct in6_addr *addr)
{
	memcpy(addr, &a->addr, sizeof(*addr));

	if (a->prefix_len <= 64)
		*(uint64_t *)(addr->s6_addr + 8) = intf_id;
	else
		*(uint64_t *)(addr->s6_addr + 8) |= intf_id & ((1 << (128 - a->prefix_len)) - 1);
}

static void insert_status(struct dhcpv6_packet *pkt, struct dhcpv6_option *opt, int code)
{
	struct dhcpv6_option *opt1;
	struct dhcpv6_opt_status *status;

	if (opt)
		opt1 = dhcpv6_nested_option_alloc(pkt, opt, D6_OPTION_STATUS_CODE, sizeof(struct dhcpv6_opt_status) - sizeof(struct dhcpv6_opt_hdr));
	else
		opt1 = dhcpv6_option_alloc(pkt, D6_OPTION_STATUS_CODE, sizeof(struct dhcpv6_opt_status) - sizeof(struct dhcpv6_opt_hdr));

	status = (struct dhcpv6_opt_status *)opt1->hdr;
	status->code = htons(code);
}

static void dhcpv6_send_reply(struct dhcpv6_packet *req, struct dhcpv6_pd *pd, int code)
{
	struct dhcpv6_packet *reply;
	struct dhcpv6_option *opt, *opt1, *opt2, *opt3;
	struct dhcpv6_opt_ia_na *ia_na;
	struct dhcpv6_opt_ia_addr *ia_addr;
	struct dhcpv6_opt_ia_prefix *ia_prefix;
	struct ipv6db_addr_t *a;
	struct in6_addr addr, *addr_ptr;
	int i, j, f = 0, f1, f2 = 0;
	uint16_t *ptr;

	reply = dhcpv6_packet_alloc_reply(req, code);
	if (!reply)
		return;
	
	list_for_each_entry(opt, &req->opt_list, entry) {

		// IA_NA
		if (ntohs(opt->hdr->code) == D6_OPTION_IA_NA) {
			opt1 = dhcpv6_option_alloc(reply, D6_OPTION_IA_NA, sizeof(struct dhcpv6_opt_ia_na) - sizeof(struct dhcpv6_opt_hdr));
			memcpy(opt1->hdr + 1, opt->hdr + 1, ntohs(opt1->hdr->len));

			ia_na = (struct dhcpv6_opt_ia_na *)opt1->hdr;
			ia_na->T1 = 0;
			ia_na->T2 = 0;

			if ((req->hdr->type == D6_RENEW || req->hdr->type == D6_REBIND) && pd->addr_iaid != ia_na->iaid) {
				insert_status(reply, opt1, D6_STATUS_NoBinding);
			} else if (list_empty(&req->ppp->ipv6->addr_list) || f) {
				insert_status(reply, opt1, D6_STATUS_NoAddrsAvail);
			} else {

				if (req->hdr->type == D6_REQUEST)
					pd->addr_iaid = ia_na->iaid;

				f = 1;

				list_for_each_entry(a, &req->ppp->ipv6->addr_list, entry) {
					opt2 = dhcpv6_nested_option_alloc(reply, opt1, D6_OPTION_IAADDR, sizeof(*ia_addr) - sizeof(struct dhcpv6_opt_hdr));
					ia_addr = (struct dhcpv6_opt_ia_addr *)opt2->hdr;

					build_addr(a, req->ppp->ipv6->peer_intf_id, &ia_addr->addr);

					ia_addr->pref_lifetime = htonl(conf_pref_lifetime);
					ia_addr->valid_lifetime = htonl(conf_valid_lifetime);	
				}

				list_for_each_entry(opt2, &opt->opt_list, entry) {
					if (ntohs(opt2->hdr->code) == D6_OPTION_IAADDR) {
						ia_addr = (struct dhcpv6_opt_ia_addr *)opt2->hdr;

						f1 = 0;
						list_for_each_entry(a, &req->ppp->ipv6->addr_list, entry) {
							build_addr(a, req->ppp->ipv6->peer_intf_id, &addr);
							if (memcmp(&addr, &ia_addr->addr, sizeof(addr)))
								continue;
							f1 = 1;
							break;
						}

						if (!f1) {
							opt3 = dhcpv6_nested_option_alloc(reply, opt1, D6_OPTION_IAADDR, sizeof(*ia_addr) - sizeof(struct dhcpv6_opt_hdr));
							memcpy(opt3->hdr->data, opt2->hdr->data, sizeof(*ia_addr) - sizeof(struct dhcpv6_opt_hdr));

							ia_addr = (struct dhcpv6_opt_ia_addr *)opt3->hdr;
							ia_addr->pref_lifetime = 0;
							ia_addr->valid_lifetime = 0;

							insert_status(reply, opt3, D6_STATUS_NotOnLink);
						}
					}
				}

				insert_status(reply, opt1, D6_STATUS_Success);
			}

		// IA_PD
		} else if (ntohs(opt->hdr->code) == D6_OPTION_IA_PD) {
			opt1 = dhcpv6_option_alloc(reply, D6_OPTION_IA_PD, sizeof(struct dhcpv6_opt_ia_na) - sizeof(struct dhcpv6_opt_hdr));
			memcpy(opt1->hdr + 1, opt->hdr + 1, ntohs(opt1->hdr->len));
			
			ia_na = (struct dhcpv6_opt_ia_na *)opt1->hdr;
			ia_na->T1 = 0;
			ia_na->T2 = 0;
			
			if (req->hdr->type == D6_SOLICIT && !pd->ipv6_dp)
				pd->ipv6_dp = ipdb_get_ipv6_prefix(req->ppp);

			if ((req->hdr->type == D6_RENEW || req->hdr->type == D6_REBIND) && pd->dp_iaid != ia_na->iaid) {
				insert_status(reply, opt1, D6_STATUS_NoBinding);
			} else if (!pd->ipv6_dp || list_empty(&pd->ipv6_dp->prefix_list) || f2) {
				insert_status(reply, opt1, D6_STATUS_NoPrefixAvail);
			} else {

				if (req->hdr->type == D6_REQUEST)
					pd->dp_iaid = ia_na->iaid;

				f2 = 1;

				list_for_each_entry(a, &pd->ipv6_dp->prefix_list, entry) {
					opt2 = dhcpv6_nested_option_alloc(reply, opt1, D6_OPTION_IAPREFIX, sizeof(*ia_prefix) - sizeof(struct dhcpv6_opt_hdr));
					ia_prefix = (struct dhcpv6_opt_ia_prefix *)opt2->hdr;
					
					memcpy(&ia_prefix->prefix, &a->addr, sizeof(a->addr));
					ia_prefix->prefix_len = a->prefix_len;
					ia_prefix->pref_lifetime = htonl(conf_pref_lifetime);
					ia_prefix->valid_lifetime = htonl(conf_valid_lifetime);	
				}

				list_for_each_entry(opt2, &opt->opt_list, entry) {
					if (ntohs(opt2->hdr->code) == D6_OPTION_IAPREFIX) {
						ia_prefix = (struct dhcpv6_opt_ia_prefix *)opt2->hdr;

						f1 = 0;
						list_for_each_entry(a, &pd->ipv6_dp->prefix_list, entry) {
							if (a->prefix_len != ia_prefix->prefix_len)
								continue;
							if (memcmp(&a->addr, &ia_prefix->prefix, sizeof(a->addr)))
								continue;
							f1 = 1;
							break;
						}

						if (!f1) {
							opt3 = dhcpv6_nested_option_alloc(reply, opt1, D6_OPTION_IAPREFIX, sizeof(*ia_prefix) - sizeof(struct dhcpv6_opt_hdr));
							memcpy(opt3->hdr->data, opt2->hdr->data, sizeof(*ia_prefix) - sizeof(struct dhcpv6_opt_hdr));
							ia_prefix = (struct dhcpv6_opt_ia_prefix *)opt3->hdr;
							ia_prefix->pref_lifetime = 0;
							ia_prefix->valid_lifetime = 0;

							insert_status(reply, opt3, D6_STATUS_NotOnLink);
						}
					}
				}
			
				insert_status(reply, opt1, D6_STATUS_Success);
		}

		// IA_TA
		} else if (ntohs(opt->hdr->code) == D6_OPTION_IA_TA) {
			opt1 = dhcpv6_option_alloc(reply, D6_OPTION_IA_TA, sizeof(struct dhcpv6_opt_ia_ta) - sizeof(struct dhcpv6_opt_hdr));
			memcpy(opt1->hdr + 1, opt->hdr + 1, ntohs(opt1->hdr->len));

			insert_status(reply, opt1, D6_STATUS_NoAddrsAvail);

		// Option Request
		}	else if (ntohs(opt->hdr->code) == D6_OPTION_ORO) {
			for (i = ntohs(opt->hdr->len) / 2, ptr = (uint16_t *)opt->hdr->data; i; i--, ptr++) {
				if (ntohs(*ptr) == D6_OPTION_DNS_SERVERS) {
					opt1 = dhcpv6_option_alloc(reply, D6_OPTION_DNS_SERVERS, conf_dns_count * sizeof(addr));
					for (j = 0, addr_ptr = (struct in6_addr *)opt1->hdr->data; j < conf_dns_count; j++, addr_ptr++)
						memcpy(addr_ptr, conf_dns + j, sizeof(addr));
				} else if (ntohs(*ptr) == D6_OPTION_DOMAIN_LIST) {
					opt1 = dhcpv6_option_alloc(reply, D6_OPTION_DOMAIN_LIST, conf_dnssl_size);
					memcpy(opt1->hdr->data, conf_dnssl, conf_dnssl_size);
				}
			}
		}
	}
	
	insert_status(reply, NULL, D6_STATUS_Success);

	if (conf_verbose) {
		log_ppp_info2("send ");
		dhcpv6_packet_print(reply, log_ppp_info2);
	}

	dhcpv6_send(reply);

	dhcpv6_packet_free(reply);
}

static void dhcpv6_recv_solicit(struct dhcpv6_packet *req)
{
	struct dhcpv6_pd *pd = find_pd(req->ppp);

	if (!pd)
		return;
	
	if (!req->clientid) {
		log_ppp_error("dhcpv6: no Client-ID option\n");
		return;
	}

	if (req->serverid) {
		log_ppp_error("dhcpv6: unexpected Server-ID option\n");
		return;
	}

	req->serverid = &pd->serverid;

	if (!pd->clientid) {
		pd->clientid = _malloc(sizeof(struct dhcpv6_opt_hdr) + ntohs(req->clientid->hdr.len));
		memcpy(pd->clientid, req->clientid, sizeof(struct dhcpv6_opt_hdr) + ntohs(req->clientid->hdr.len));
	} else if (pd->clientid->hdr.len != req->clientid->hdr.len || memcmp(pd->clientid, req->clientid, sizeof(struct dhcpv6_opt_hdr) + ntohs(req->clientid->hdr.len))) {
		log_ppp_warn("dhcpv6: Client-ID option was changed\n");
		return;
	}

	dhcpv6_send_reply(req, pd, D6_ADVERTISE);
}

static void dhcpv6_recv_request(struct dhcpv6_packet *req)
{
	struct dhcpv6_pd *pd = find_pd(req->ppp);

	if (!pd)
		return;
	
	if (!req->clientid) {
		log_ppp_error("dhcpv6: no Client-ID option\n");
		return;
	}

	if (!req->serverid) {
		log_ppp_error("dhcpv6: no Server-ID option\n");
		return;
	}

	if (!pd->clientid) {
		pd->clientid = _malloc(sizeof(struct dhcpv6_opt_hdr) + ntohs(req->clientid->hdr.len));
		memcpy(pd->clientid, req->clientid, sizeof(struct dhcpv6_opt_hdr) + ntohs(req->clientid->hdr.len));
	} else if (pd->clientid->hdr.len != req->clientid->hdr.len || memcmp(pd->clientid, req->clientid, sizeof(struct dhcpv6_opt_hdr) + ntohs(req->clientid->hdr.len))) {
		log_ppp_warn("dhcpv6: Client-ID option was changed\n");
		return;
	}
	
	dhcpv6_send_reply(req, pd, D6_REPLY);
}

static void dhcpv6_recv_renew(struct dhcpv6_packet *req)
{
	struct dhcpv6_pd *pd = find_pd(req->ppp);

	if (!pd)
		return;
	
	if (!req->clientid) {
		log_ppp_error("dhcpv6: no Client-ID option\n");
		return;
	}

	if (!req->serverid) {
		log_ppp_error("dhcpv6: no Server-ID option\n");
		return;
	}

	if (!pd->clientid) {
		log_ppp_error("dhcpv6: no Solicit or Request was received\n");
		return;
	}
	
	dhcpv6_send_reply(req, pd, D6_REPLY);
}

static void dhcpv6_recv_rebind(struct dhcpv6_packet *pkt)
{
	dhcpv6_recv_renew(pkt);
}

static void dhcpv6_recv_release(struct dhcpv6_packet *pkt)
{

}

static void dhcpv6_recv_decline(struct dhcpv6_packet *pkt)
{

}

static void dhcpv6_recv_packet(struct dhcpv6_packet *pkt)
{
	if (conf_verbose) {
		log_ppp_info2("recv ");
		dhcpv6_packet_print(pkt, log_ppp_info2);
	}

	switch (pkt->hdr->type) {
		case D6_SOLICIT:
			dhcpv6_recv_solicit(pkt);
			break;
		case D6_REQUEST:
			dhcpv6_recv_request(pkt);
			break;
		case D6_RENEW:
			dhcpv6_recv_renew(pkt);
			break;
		case D6_REBIND:
			dhcpv6_recv_rebind(pkt);
			break;
		case D6_RELEASE:
			dhcpv6_recv_release(pkt);
			break;
		case D6_DECLINE:
			dhcpv6_recv_decline(pkt);
			break;
	}

	dhcpv6_packet_free(pkt);
}

static int dhcpv6_read(struct triton_md_handler_t *h)
{
	int n;
	struct sockaddr_in6 addr;
	socklen_t len = sizeof(addr);
	struct dhcpv6_packet *pkt;
	struct ppp_t *ppp;

	while (1) {
		n = recvfrom(h->fd, buf, BUF_SIZE, 0, &addr, &len);
		if (n == -1) {
			if (errno == EAGAIN)
				return 0;
			log_error("dhcpv6: read: %s\n", strerror(errno));
		}

		if (!IN6_IS_ADDR_LINKLOCAL(&addr.sin6_addr))
			continue;

		if (addr.sin6_port != ntohs(DHCPV6_CLIENT_PORT))
			continue;

		pkt = dhcpv6_packet_parse(buf, n);
		if (!pkt || !pkt->clientid) {
			continue;
		}

		pthread_rwlock_rdlock(&ppp_lock);
		list_for_each_entry(ppp, &ppp_list, entry) {
			if (ppp->state != PPP_STATE_ACTIVE)
				continue;

			if (!ppp->ipv6)
				continue;

			if (ppp->ifindex != addr.sin6_scope_id)
				continue;

			if (ppp->ipv6->peer_intf_id != *(uint64_t *)(addr.sin6_addr.s6_addr + 8))
				continue;

			pkt->ppp = ppp;

			triton_context_call(ppp->ctrl->ctx, (triton_event_func)dhcpv6_recv_packet, pkt);
			break;
		}
		pthread_rwlock_unlock(&ppp_lock);
	}

	return 0;
}

static void dhcpv6_close(struct triton_context_t *ctx)
{
	triton_md_unregister_handler(&dhcpv6_hnd);
	close(dhcpv6_hnd.fd);
	triton_context_unregister(ctx);
}

static struct triton_md_handler_t dhcpv6_hnd = {
	.read = dhcpv6_read,
};

static struct triton_context_t dhcpv6_ctx = {
	.close = dhcpv6_close,
};

static void add_dnssl(const char *val)
{
	int n = strlen(val);
	const char *ptr;
	uint8_t *buf;

	if (val[n - 1] == '.')
		n++;
	else
		n += 2;
	
	if (n > 255) {
		log_error("dnsv6: dnssl '%s' is too long\n", val);
		return;
	}
	
	if (!conf_dnssl)
		conf_dnssl = _malloc(n);
	else
		conf_dnssl = _realloc(conf_dnssl, conf_dnssl_size + n);
	
	buf = conf_dnssl + conf_dnssl_size;
	
	while (1) {
		ptr = strchr(val, '.');
		if (!ptr)
			ptr = strchr(val, 0);
		if (ptr - val > 63) {
			log_error("dnsv6: dnssl '%s' is invalid\n", val);
			return;
		}
		*buf = ptr - val;
		memcpy(buf + 1, val, ptr - val);
		buf += 1 + (ptr - val);
		val = ptr + 1;
		if (!*ptr || !*val) {
				*buf = 0;
				break;
		}
	}
	
	conf_dnssl_size += n;
}

static void load_dns(void)
{
	struct conf_sect_t *s = conf_get_section("dnsv6");
	struct conf_option_t *opt;
	
	if (!s)
		return;
	
	conf_dns_count = 0;
	
	if (conf_dnssl)
		_free(conf_dnssl);
	conf_dnssl = NULL;
	conf_dnssl_size = 0;

	list_for_each_entry(opt, &s->items, entry) {
		if (!strcmp(opt->name, "dnssl")) {
			add_dnssl(opt->val);
			continue;
		}

		if (!strcmp(opt->name, "dns") || !opt->val) {
			if (conf_dns_count == MAX_DNS_COUNT)
				continue;

			if (inet_pton(AF_INET6, opt->val ? opt->val : opt->name, &conf_dns[conf_dns_count]) == 0) {
				log_error("dnsv6: faild to parse '%s'\n", opt->name);
				continue;
			}
			conf_dns_count++;
		}
	}
}

static void load_config(void)
{
	const char *opt;

	opt = conf_get_opt("dhcpv6", "verbose");
	if (opt)
		conf_verbose = atoi(opt);
	
	load_dns();
}

static void init(void)
{
	struct sockaddr_in6 addr;
	int sock;

	if (!triton_module_loaded("ipv6_nd"))
		log_warn("dhcpv6: ipv6_nd module is not loaded, you probably get misconfigured network environment\n");

	load_config();

	sock = socket(AF_INET6, SOCK_DGRAM, 0);
	if (!sock) {
		log_error("dhcpv6: socket: %s\n", strerror(errno));
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(DHCPV6_SERV_PORT);
	
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		log_error("dhcpv6: bind: %s\n", strerror(errno));
		close(sock);
		return;
	}

	fcntl(sock, F_SETFL, O_NONBLOCK);

	dhcpv6_hnd.fd = sock;

	buf = malloc(BUF_SIZE);

	triton_context_register(&dhcpv6_ctx, NULL);
	triton_md_register_handler(&dhcpv6_ctx, &dhcpv6_hnd);
	triton_md_enable_handler(&dhcpv6_hnd, MD_MODE_READ);
	triton_context_wakeup(&dhcpv6_ctx);

	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
	triton_event_register_handler(EV_PPP_STARTED, (triton_event_func)ev_ppp_started);
	triton_event_register_handler(EV_PPP_FINISHED, (triton_event_func)ev_ppp_finished);
}

DEFINE_INIT(10, init);