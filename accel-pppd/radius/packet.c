#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/mman.h>

#include "log.h"
#include "mempool.h"

#include "radius_p.h"

#include "memdebug.h"

static mempool_t packet_pool;
static mempool_t attr_pool;

struct rad_packet_t *rad_packet_alloc(int code)
{
	struct rad_packet_t *pack;

	pack = mempool_alloc(packet_pool);
	if (!pack) {
		log_emerg("radius:packet: out of memory\n");
		return NULL;
	}

	memset(pack, 0, sizeof(*pack));
	pack->code = code;
	pack->len = 20;
	pack->id = 1;
	INIT_LIST_HEAD(&pack->attrs);

	return pack;
}

void print_buf(uint8_t *buf,int size)
{
	int i;
	for(i=0;i<size;i++)
		printf("%x ",buf[i]);
	printf("\n");
}

int rad_packet_build(struct rad_packet_t *pack, uint8_t *RA)
{
	struct rad_attr_t *attr;
	uint8_t *ptr;

	if (!pack->buf) {
		ptr = mmap(NULL, REQ_LENGTH_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

		if (ptr == MAP_FAILED) {
			log_emerg("radius:packet: out of memory\n");
			return -1;
		}
		
		pack->buf = ptr;
	} else
		ptr = pack->buf;

	*ptr = pack->code; ptr++;
	*ptr = pack->id; ptr++;
	*(uint16_t*)ptr = htons(pack->len); ptr+= 2;
	memcpy(ptr, RA, 16);	ptr+=16;

	list_for_each_entry(attr, &pack->attrs, entry) {
		if (attr->vendor) {
			*ptr = 26; ptr++;
			*ptr = attr->len + 2 + 6; ptr++;
			*(uint32_t *)ptr = htonl(attr->vendor->id); ptr+=4;
		} 
		*ptr = attr->attr->id; ptr++;
		*ptr = attr->len + 2; ptr++;
		switch(attr->attr->type) {
			case ATTR_TYPE_INTEGER:
				*(uint32_t*)ptr = htonl(attr->val.integer);
				break;
			case ATTR_TYPE_OCTETS:
			case ATTR_TYPE_STRING:
				memcpy(ptr, attr->val.string, attr->len);
				break;
			case ATTR_TYPE_IPADDR:
				*(in_addr_t*)ptr = attr->val.ipaddr;
				break;
			case ATTR_TYPE_DATE:
				*(uint32_t*)ptr = htonl(attr->val.date);
				break;
			default:
				log_emerg("radius:packet:BUG: unknown attribute type\n");
				abort();
		}
		ptr += attr->len;
	}

	//print_buf(pack->buf, pack->len);
	return 0;
}

int rad_packet_recv(int fd, struct rad_packet_t **p, struct sockaddr_in *addr)
{
	struct rad_packet_t *pack;
	struct rad_attr_t *attr;
	struct rad_dict_attr_t *da;
	struct rad_dict_vendor_t *vendor;
	uint8_t *ptr;
	int n, id, len, vendor_id;
	socklen_t addr_len = sizeof(*addr);

	*p = NULL;

	pack = rad_packet_alloc(0);
	if (!pack)
		return 0;

	ptr = mmap(NULL, REQ_LENGTH_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (ptr == MAP_FAILED) {
		log_emerg("radius:packet: out of memory\n");
		goto out_err;
	}
	
	pack->buf = ptr;

	while (1) {
		if (addr)
			n = recvfrom(fd, pack->buf, REQ_LENGTH_MAX, 0, addr, &addr_len);
		else
			n = read(fd, pack->buf, REQ_LENGTH_MAX);
		if (n < 0) {
			if (errno == EAGAIN) {
				rad_packet_free(pack);
				return -1;
			}
			if (errno != ECONNREFUSED)
				log_ppp_error("radius:packet:read: %s\n", strerror(errno));
			goto out_err;
		}
		break;
	}

	if (n < 20) {
		log_ppp_warn("radius:packet: short packed received (%i)\n", n);
		goto out_err;
	}

	pack->code = *ptr; ptr++;
	pack->id = *ptr; ptr++;
	pack->len = ntohs(*(uint16_t*)ptr); ptr += 2;

	if (pack->len > n) {
		log_ppp_warn("radius:packet: short packet received %i, expected %i\n", pack->len, n);
		goto out_err;
	}

	ptr += 16;
	n -= 20;

	while (n>0) {
		id = *ptr; ptr++;
		len = *ptr - 2; ptr++;
		if (len < 0) {
			log_ppp_warn("radius:packet short attribute len received\n");
			goto out_err;
		}
		if (2 + len > n) {
			log_ppp_warn("radius:packet: too long attribute received (%i, %i)\n", id, len);
			goto out_err;
		}
		if (id == 26) {
			vendor_id = ntohl(*(uint32_t *)ptr);
			vendor = rad_dict_find_vendor_id(vendor_id);
			if (vendor) {
				ptr += 4;
				id = *ptr; ptr++;
				len = *ptr - 2; ptr++;
				n -= 2 + 4;
			} else
				log_ppp_warn("radius:packet: vendor %i not found\n", id);
		} else
			vendor = NULL;
		da = rad_dict_find_attr_id(vendor, id);
		if (da) {
			attr = mempool_alloc(attr_pool);
			if (!attr) {
				log_emerg("radius:packet: out of memory\n");
				goto out_err;
			}
			memset(attr, 0, sizeof(*attr));
			attr->vendor = vendor;
			attr->attr = da;
			attr->len = len;
			switch (da->type) {
				case ATTR_TYPE_STRING:
					attr->val.string = _malloc(len+1);
					if (!attr->val.string) {
						log_emerg("radius:packet: out of memory\n");
						_free(attr);
						goto out_err;
					}
					memcpy(attr->val.string, ptr, len);
					attr->val.string[len] = 0;
					break;
				case ATTR_TYPE_OCTETS:
					attr->val.octets = _malloc(len);
					if (!attr->val.octets) {
						log_emerg("radius:packet: out of memory\n");
						_free(attr);
						goto out_err;
					}
					memcpy(attr->val.octets, ptr, len);
					break;				
				case ATTR_TYPE_DATE:
				case ATTR_TYPE_INTEGER:
					attr->val.integer = ntohl(*(uint32_t*)ptr);
					break;
				case ATTR_TYPE_IPADDR:
					attr->val.integer = *(uint32_t*)ptr;
					break;
			}
			list_add_tail(&attr->entry, &pack->attrs);
		} else
			log_ppp_warn("radius:packet: unknown attribute received (%i,%i)\n", vendor ? vendor->id : 0, id);
		ptr += len;
		n -= 2 + len;
	}

	munmap(pack->buf, REQ_LENGTH_MAX);
	pack->buf = NULL;

	*p = pack;

	return 0;

out_err:
	rad_packet_free(pack);
	return 0;
}

void rad_packet_free(struct rad_packet_t *pack)
{
	struct rad_attr_t *attr;
	
	if (pack->buf)
		munmap(pack->buf, REQ_LENGTH_MAX);

	while(!list_empty(&pack->attrs)) {
		attr = list_entry(pack->attrs.next, typeof(*attr), entry);
		list_del(&attr->entry);
		if (attr->attr->type == ATTR_TYPE_STRING || attr->attr->type == ATTR_TYPE_OCTETS)
			_free(attr->val.string);
		mempool_free(attr);
	}

	mempool_free(pack);
}

void rad_packet_print(struct rad_packet_t *pack, void (*print)(const char *fmt, ...))
{
	struct rad_attr_t *attr;
	struct rad_dict_value_t *val;
	
	print("[RADIUS ");
	switch(pack->code) {
		case CODE_ACCESS_REQUEST:
			print("Access-Request");
			break;
		case CODE_ACCESS_CHALLENGE:
			print("Access-Challenge");
			break;
		case CODE_ACCESS_ACCEPT:
			print("Access-Accept");
			break;
		case CODE_ACCESS_REJECT:
			print("Access-Reject");
			break;
		case CODE_ACCOUNTING_REQUEST:
			print("Accounting-Request");
			break;
		case CODE_ACCOUNTING_RESPONSE:
			print("Accounting-Response");
			break;
		case CODE_DISCONNECT_REQUEST:
			print("Disconnect-Request");
			break;
		case CODE_DISCONNECT_ACK:
			print("Disconnect-ACK");
			break;
		case CODE_DISCONNECT_NAK:
			print("Disconnect-NAK");
			break;
		case CODE_COA_REQUEST:
			print("CoA-Request");
			break;
		case CODE_COA_ACK:
			print("CoA-ACK");
			break;
		case CODE_COA_NAK:
			print("CoA-NAK");
			break;
		default:
			print("Unknown (%i)", pack->code);
	}
	print(" id=%x", pack->id);

	list_for_each_entry(attr, &pack->attrs, entry) {
		if (attr->vendor)
			print("<%s %s ", attr->vendor->name, attr->attr->name);
		else
			print(" <%s ", attr->attr->name);
		switch (attr->attr->type) {
			case ATTR_TYPE_INTEGER:
				val = rad_dict_find_val(attr->attr, attr->val);
				if (val)
					print("%s", val->name);
				else
					print("%u", attr->val.integer);
				break;
			case ATTR_TYPE_STRING:
				print("\"%s\"", attr->val.string);
				break;
			case ATTR_TYPE_IPADDR:
				print("%i.%i.%i.%i", attr->val.ipaddr & 0xff, (attr->val.ipaddr >> 8) & 0xff, (attr->val.ipaddr >> 16) & 0xff, (attr->val.ipaddr >> 24) & 0xff);
				break;
		}
		print(">");
	}
	print("]\n");
}

int __export rad_packet_add_int(struct rad_packet_t *pack, const char *vendor_name, const char *name, int val)
{
	struct rad_attr_t *ra;
	struct rad_dict_attr_t *attr;
	struct rad_dict_vendor_t *vendor;

	if (pack->len + (vendor_name ? 8 : 2) + 4 >= REQ_LENGTH_MAX)
		return -1;

	if (vendor_name) {
		vendor = rad_dict_find_vendor_name(vendor_name);
		if (!vendor)
			return -1;
		attr = rad_dict_find_vendor_attr(vendor, name);
	} else {
		vendor = NULL;
		attr = rad_dict_find_attr(name);
	}

	if (!attr)
		return -1;
	
	ra = mempool_alloc(attr_pool);
	if (!ra)
		return -1;

	memset(ra, 0, sizeof(*ra));
	ra->vendor = vendor;
	ra->attr = attr;
	ra->len = 4;
	ra->val.integer = val;
	list_add_tail(&ra->entry, &pack->attrs);
	pack->len += (vendor_name ? 8 : 2) + 4;

	return 0;
}

int __export rad_packet_change_int(struct rad_packet_t *pack, const char *vendor_name, const char *name, int val)
{
	struct rad_attr_t *ra;
	
	ra = rad_packet_find_attr(pack, vendor_name, name);
	if (!ra)
		return -1;

	ra->val.integer = val;

	return 0;
}

int __export rad_packet_add_octets(struct rad_packet_t *pack, const char *vendor_name, const char *name, const uint8_t *val, int len)
{
	struct rad_attr_t *ra;
	struct rad_dict_attr_t *attr;
	struct rad_dict_vendor_t *vendor;

	if (pack->len + (vendor_name ? 8 : 2) + len >= REQ_LENGTH_MAX)
		return -1;

	if (vendor_name) {
		vendor = rad_dict_find_vendor_name(vendor_name);
		if (!vendor)
			return -1;
		attr = rad_dict_find_vendor_attr(vendor, name);
	} else {
		vendor = NULL;
		attr = rad_dict_find_attr(name);
	}
	
	if (!attr)
		return -1;
	
	ra = mempool_alloc(attr_pool);
	if (!ra) {
		log_emerg("radius: out of memory\n");
		return -1;
	}

	memset(ra, 0, sizeof(*ra));
	ra->vendor = vendor;
	ra->attr = attr;
	ra->len = len;
	ra->val.octets = _malloc(len);
	if (!ra->val.octets) {
		log_emerg("radius: out of memory\n");
		_free(ra);
		return -1;
	}
	memcpy(ra->val.octets, val, len);
	list_add_tail(&ra->entry, &pack->attrs);
	pack->len += (vendor_name ? 8 : 2) + len;

	return 0;
}

int __export rad_packet_change_octets(struct rad_packet_t *pack, const char *vendor_name, const char *name, const uint8_t *val, int len)
{
	struct rad_attr_t *ra;
	
	ra = rad_packet_find_attr(pack, vendor_name, name);
	if (!ra)
		return -1;

	if (ra->len != len) {
		if (pack->len - ra->len + len >= REQ_LENGTH_MAX)
			return -1;

		ra->val.octets = _realloc(ra->val.octets, len);
		if (!ra->val.octets) {
			log_emerg("radius: out of memory\n");
			return -1;
		}
	
		pack->len += len - ra->len;
		ra->len = len;
	}

	memcpy(ra->val.octets, val, len);

	return 0;
}


int __export rad_packet_add_str(struct rad_packet_t *pack, const char *vendor_name, const char *name, const char *val)
{
	struct rad_attr_t *ra;
	struct rad_dict_attr_t *attr;
	struct rad_dict_vendor_t *vendor;
	int len = strlen(val);

	if (pack->len + (vendor_name ? 8 : 2) + len >= REQ_LENGTH_MAX)
		return -1;

	if (vendor_name) {
		vendor = rad_dict_find_vendor_name(vendor_name);
		if (!vendor)
			return -1;
		attr = rad_dict_find_vendor_attr(vendor, name);
	} else {
		vendor = NULL;
		attr = rad_dict_find_attr(name);
	}

	if (!attr)
		return -1;
	
	ra = mempool_alloc(attr_pool);
	if (!ra) {
		log_emerg("radius: out of memory\n");
		return -1;
	}

	memset(ra, 0, sizeof(*ra));
	ra->vendor = vendor;
	ra->attr = attr;
	ra->len = len;
	ra->val.string = _malloc(len + 1);
	if (!ra->val.string) {
		log_emerg("radius: out of memory\n");
		_free(ra);
		return -1;
	}
	memcpy(ra->val.string, val, len);
	ra->val.string[len] = 0;
	list_add_tail(&ra->entry, &pack->attrs);
	pack->len += (vendor_name ? 8 : 2) + len;

	return 0;
}

int __export rad_packet_change_str(struct rad_packet_t *pack, const char *vendor_name, const char *name, const char *val, int len)
{
	struct rad_attr_t *ra;
	
	ra = rad_packet_find_attr(pack, vendor_name, name);
	if (!ra)
		return -1;

	if (ra->len != len) {
		if (pack->len - ra->len + len >= REQ_LENGTH_MAX)
			return -1;

		ra->val.string = _realloc(ra->val.string, len + 1);
		if (!ra->val.string) {
			log_emerg("radius: out of memory\n");
			return -1;
		}
	
		pack->len += len - ra->len;
		ra->len = len;
	}

	memcpy(ra->val.string, val, len);
	ra->val.string[len] = 0;

	return 0;
}

int __export rad_packet_add_val(struct rad_packet_t *pack, const char *vendor_name, const char *name, const char *val)
{
	struct rad_attr_t *ra;
	struct rad_dict_attr_t *attr;
	struct rad_dict_value_t *v;
	struct rad_dict_vendor_t *vendor;

	if (pack->len + (vendor_name ? 8 : 2) + 4 >= REQ_LENGTH_MAX)
		return -1;

	if (vendor_name) {
		vendor = rad_dict_find_vendor_name(vendor_name);
		if (!vendor)
			return -1;
		attr = rad_dict_find_vendor_attr(vendor, name);
	} else {
		vendor = NULL;
		attr = rad_dict_find_attr(name);
	}
	
	if (!attr)
		return -1;

	v = rad_dict_find_val_name(attr, val);
	if (!v)
		return -1;
	
	ra = mempool_alloc(attr_pool);
	if (!ra)
		return -1;

	memset(ra, 0, sizeof(*ra));
	ra->vendor = vendor;
	ra->attr = attr;
	ra->len = 4;
	ra->val = v->val;
	list_add_tail(&ra->entry, &pack->attrs);
	pack->len += (vendor_name ? 8 : 2) + 4;

	return 0;
}

int __export rad_packet_change_val(struct rad_packet_t *pack, const char *vendor_name, const char *name, const char *val)
{
	struct rad_attr_t *ra;
	struct rad_dict_value_t *v;
	
	ra = rad_packet_find_attr(pack, vendor_name, name);
	if (!ra)
		return -1;

	v = rad_dict_find_val_name(ra->attr, val);
	if (!v)
		return -1;

	ra->val = v->val;
	
	return 0;
}

int __export rad_packet_add_ipaddr(struct rad_packet_t *pack, const char *vendor_name, const char *name, in_addr_t ipaddr)
{
	return rad_packet_add_int(pack, vendor_name, name, ipaddr);
}


struct rad_attr_t __export *rad_packet_find_attr(struct rad_packet_t *pack, const char *vendor_name, const char *name)
{
	struct rad_attr_t *ra;
	struct rad_dict_vendor_t *vendor;

	if (vendor_name) {
		vendor = rad_dict_find_vendor_name(vendor_name);
		if (!vendor)
			return NULL;
	} else
		vendor = NULL;
	
	list_for_each_entry(ra, &pack->attrs, entry) {
		if (vendor && vendor != ra->vendor)
			continue;

		if (strcmp(ra->attr->name, name))
			continue;
		
		return ra;
	}

	return NULL;
}

int rad_packet_send(struct rad_packet_t *pack, int fd, struct sockaddr_in *addr)
{
	int n;

	while (1) {
		if (addr)
			n = sendto(fd, pack->buf, pack->len, 0, addr, sizeof(*addr));
		else
			n = write(fd, pack->buf, pack->len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			log_ppp_error("radius:write: %s\n", strerror(errno));
			return -1;
		} else if (n != pack->len) {
			log_ppp_error("radius:write: short write %i, excpected %i\n", n, pack->len);
			return -1;
		}
		break;
	}

	return 0;
}

static void __init init(void)
{
	attr_pool = mempool_create(sizeof(struct rad_attr_t));
	packet_pool = mempool_create(sizeof(struct rad_packet_t));
}
