#include "nat64/mod/stateful/bib/port_allocator.h"

#include <crypto/md5.h>
#include <linux/crypto.h>
#include "nat64/common/str_utils.h"
#include "nat64/mod/common/config.h"
#include "nat64/mod/stateful/bib/db.h"
#include "nat64/mod/stateful/pool4/db.h"

/* TODO (issue175) RFC 6056 wants us to change this from time to time. */
static unsigned char *secret_key;
static size_t secret_key_len;
static atomic_t next_ephemeral;

static struct crypto_hash *tfm;
static DEFINE_SPINLOCK(tfm_lock);

int palloc_init(void)
{
	unsigned int tmp;
	int error;

	/* Secret key stuff */
	secret_key_len = PAGE_SIZE;
	if (secret_key_len > 128)
		secret_key_len = 128;

	secret_key = kmalloc(secret_key_len, GFP_KERNEL);
	if (!secret_key)
		return -ENOMEM;
	get_random_bytes(secret_key, secret_key_len);

	/* Next ephemeral stuff */
	get_random_bytes(&tmp, sizeof(tmp));
	atomic_set(&next_ephemeral, tmp);

	/* TFC stuff */
	tfm = crypto_alloc_hash("md5", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		error = PTR_ERR(tfm);
		log_warn_once("Failed to load transform for MD5; errcode %d",
				error);
		kfree(secret_key);
		return error;
	}

	return 0;
}

void palloc_destroy(void)
{
	crypto_free_hash(tfm);
	kfree(secret_key);
}

void build_scatterlist(const struct tuple *tuple6, struct scatterlist *sg,
		unsigned int *sg_len)
{
	unsigned int f_args;
	unsigned int sg_index;
	unsigned int field_len;

	f_args = config_get_f_args();
	*sg_len = 0;
	sg_index = 0;

	if (f_args & F_ARGS_SRC_ADDR) {
		field_len = sizeof(tuple6->src.addr6.l3);
		sg_set_buf(&sg[sg_index], &tuple6->src.addr6.l3, field_len);
		*sg_len += field_len;
		sg_index++;
	}
	if (f_args & F_ARGS_SRC_PORT) {
		field_len = sizeof(tuple6->src.addr6.l4);
		sg_set_buf(&sg[sg_index], &tuple6->src.addr6.l4, field_len);
		*sg_len += field_len;
		sg_index++;
	}
	if (f_args & F_ARGS_DST_ADDR) {
		field_len = sizeof(tuple6->dst.addr6.l3);
		sg_set_buf(&sg[sg_index], &tuple6->dst.addr6.l3, field_len);
		*sg_len += field_len;
		sg_index++;
	}
	if (f_args & F_ARGS_DST_PORT) {
		field_len = sizeof(tuple6->dst.addr6.l4);
		sg_set_buf(&sg[sg_index], &tuple6->dst.addr6.l4, field_len);
		*sg_len += field_len;
		sg_index++;
	}

	sg_set_buf(&sg[sg_index], secret_key, secret_key_len);
	*sg_len += secret_key_len;
}

static int f(const struct tuple *tuple6, unsigned int *result)
{
	/*
	 * See http://stackoverflow.com/questions/3869028.
	 * user502515, nanoship and noaccount are the good ones.
	 */

	union {
		__be32 as32[4];
		__u8 as8[16];
	} md5_result;
	struct scatterlist sg[5];
	unsigned int sg_len;
	struct hash_desc desc;
	int error;

	sg_init_table(sg, ARRAY_SIZE(sg));
	build_scatterlist(tuple6, sg, &sg_len);

	desc.tfm = tfm;
	desc.flags = 0;

	spin_lock_bh(&tfm_lock);

	error = crypto_hash_init(&desc);
	if (error) {
		log_debug("crypto_hash_init() failed. Errcode: %d", error);
		goto unlock;
	}
	error = crypto_hash_digest(&desc, sg, sg_len, md5_result.as8);
	if (error) {
		log_debug("crypto_hash_digest() failed. Errcode: %d", error);
		goto unlock;
	}

	*result = md5_result.as32[3];
	/* Fall through. */

unlock:
	spin_unlock_bh(&tfm_lock);
	return error;
}

struct iteration_args {
	l4_protocol proto;
	struct ipv4_transport_addr *result;
};

static int choose_port(struct ipv4_transport_addr *addr, void *void_args)
{
	struct iteration_args *args = void_args;

	atomic_inc(&next_ephemeral);

	if (!bibdb_contains4(addr, args->proto)) {
		*(args->result) = *addr;
		return 1; /* positive = break iteration, no error. */
	}

	return 0; /* Keep looking */
}

/**
 * RFC 6056, Algorithm 3.
 */
int palloc_allocate(struct packet *in_pkt, const struct tuple *tuple6,
		struct in_addr *daddr, struct ipv4_transport_addr *result)
{
	struct iteration_args args;
	unsigned int offset;
	int error;

	error = f(tuple6, &offset);
	if (error)
		return error;

	args.proto = tuple6->l4_proto;
	args.result = result;

	error = pool4db_foreach_taddr4(in_pkt, tuple6->l4_proto, daddr,
			choose_port, &args,
			offset + atomic_read(&next_ephemeral));

	if (error == 1)
		return 0;
	if (error == -ESRCH) {
		/*
		 * Assume the user doesn't need this mark/protocol.
		 * From our point of view, this is completely normal.
		 */
		log_debug("There are no pool4 entries for %s packets with mark "
				"%u.", l4proto_to_string(tuple6->l4_proto),
				in_pkt->skb->mark);
		return -ESRCH;
	}
	if (error == 0) {
		log_warn_once("pool4 is exhausted! There are no transport "
				"addresses left for %s packets with mark %u.",
				l4proto_to_string(tuple6->l4_proto),
				in_pkt->skb->mark);
		return -ESRCH;
	}

	return error;
}
