#include <linux/kernel.h>
#include <linux/module.h>

#include "nat64/unit/unit_test.h"
#include "pool4/db.c"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ramiro Nava");
MODULE_AUTHOR("Alberto Leiva");
MODULE_DESCRIPTION("IPv4 pool DB module test");

/*
 * These two are just parameter noise a function needs.
 * They're here because they won't fit in the stack and I don't want to
 * allocate.
 */
static struct sk_buff skb = { .mark = 1 };
static struct packet pkt = { .skb = &skb };

static bool test_init_power(void)
{
	unsigned int initial_power = power;
	bool success = true;

	success &= ASSERT_INT(0, init_power(0), "r0");
	success &= ASSERT_UINT(16U, slots(), "p0"); /* Because default. */
	success &= ASSERT_INT(0, init_power(1), "r1");
	success &= ASSERT_UINT(1U, slots(), "p1");
	success &= ASSERT_INT(0, init_power(2), "r2");
	success &= ASSERT_UINT(2U, slots(), "p2");
	success &= ASSERT_INT(0, init_power(3), "r3");
	success &= ASSERT_UINT(4U, slots(), "p3");
	success &= ASSERT_INT(0, init_power(4), "r4");
	success &= ASSERT_UINT(4U, slots(), "p4");
	success &= ASSERT_INT(0, init_power(5), "r5");
	success &= ASSERT_UINT(8U, slots(), "p5");
	success &= ASSERT_INT(0, init_power(1234), "r1234");
	success &= ASSERT_UINT(2048U, slots(), "p1234");
	success &= ASSERT_INT(0, init_power(0x80000000U), "rmax");
	success &= ASSERT_UINT(0x80000000U, slots(), "pmax");
	success &= ASSERT_INT(-EINVAL, init_power(0x80000001U), "2big1");
	success &= ASSERT_INT(-EINVAL, init_power(0xFFFFFFFFU), "2big2");

	power = initial_power;
	return success;
}

/**
 * add - Boilerplate code to add an entry to the pool during the tests.
 */
static bool add(__u32 addr, __u8 prefix_len, __u16 min, __u16 max)
{
	struct ipv4_prefix prefix;
	struct port_range ports;

	prefix.address.s_addr = cpu_to_be32(addr);
	prefix.len = prefix_len;
	ports.min = min;
	ports.max = max;

	return ASSERT_INT(0, pool4db_add(1, L4PROTO_TCP, &prefix, &ports),
			"add of %pI4/%u (%u-%u)",
			&prefix.address, prefix.len, min, max);
}

static bool rm(__u32 addr, __u8 prefix_len, __u16 min, __u16 max)
{
	struct ipv4_prefix prefix;
	struct port_range ports;

	prefix.address.s_addr = cpu_to_be32(addr);
	prefix.len = prefix_len;
	ports.min = min;
	ports.max = max;

	return ASSERT_INT(0, pool4db_rm(1, L4PROTO_TCP, &prefix, &ports),
			"rm of %pI4/%u (%u-%u)",
			&prefix.address, prefix.len, min, max);
}

static bool add_common_samples(void)
{
	if (!add(0xc0000200U, 31, 6, 7)) /* 192.0.2.0/31 (6-7) */
		return false;
	if (!add(0xc0000210U, 32, 15, 18)) /* 192.0.2.16 (15-18) */
		return false;
	if (!add(0xc0000220U, 30, 1, 1)) /* 192.0.2.32/30 (1-1) */
		return false;
	if (!add(0xc0000210U, 32, 22, 23)) /* 192.0.2.16 (22-23) */
		return false;
	if (!add(0xc0000210U, 31, 19, 19)) /* 192.0.2.16/31 (19-19) */
		return false;

	return true;
}

/**
 * init_taddr - Boilerplate code to initialize a transport address during the
 * tests.
 */
static void init_taddr(struct ipv4_transport_addr *taddr, __u32 addr,
		__u16 port)
{
	taddr->l3.s_addr = cpu_to_be32(addr);
	taddr->l4 = port;
}

struct foreach_taddr4_args {
	struct ipv4_transport_addr *expected;
	unsigned int expected_len;
	unsigned int i;
};

static int validate_taddr4(struct ipv4_transport_addr *addr, void *void_args)
{
	struct foreach_taddr4_args *args = void_args;
	bool success = true;

	/* log_debug("foreaching %pI4:%u", &addr->l3, addr->l4); */

	success &= ASSERT_BOOL(true, args->i < args->expected_len,
			"overflow (%u %u)", args->i, args->expected_len);
	if (!success)
		return -EINVAL;

	success &= __ASSERT_ADDR4(&args->expected[args->i].l3, &addr->l3, "addr");
	success &= ASSERT_UINT(args->expected[args->i].l4, addr->l4, "port");

	args->i++;
	return success ? 0 : -EINVAL;
}

#define COUNT 16

static bool test_foreach_taddr4(void)
{
	struct ipv4_transport_addr expected[2 * COUNT];
	unsigned int i = 0;
	struct foreach_taddr4_args args;
	int error;
	bool success = true;

	if (!add_common_samples())
		return false;

	/* 192.0.2.0/31 (6-7) */
	init_taddr(&expected[i++], 0xc0000200, 6);
	init_taddr(&expected[i++], 0xc0000200, 7);
	init_taddr(&expected[i++], 0xc0000201, 6);
	init_taddr(&expected[i++], 0xc0000201, 7);

	/* 192.0.2.16 (15-19, 22-23) */
	init_taddr(&expected[i++], 0xc0000210, 22);
	init_taddr(&expected[i++], 0xc0000210, 23);
	init_taddr(&expected[i++], 0xc0000210, 15);
	init_taddr(&expected[i++], 0xc0000210, 16);
	init_taddr(&expected[i++], 0xc0000210, 17);
	init_taddr(&expected[i++], 0xc0000210, 18);
	init_taddr(&expected[i++], 0xc0000210, 19);

	/*
	 * As you can see, the order of the transport addresses is not entirely
	 * intuitive, but we're good as long as it groups them by address and
	 * the foreach never revisits.
	 */

	/* 192.0.2.32/30 (1) */
	init_taddr(&expected[i++], 0xc0000220, 1);
	init_taddr(&expected[i++], 0xc0000221, 1);
	init_taddr(&expected[i++], 0xc0000222, 1);
	init_taddr(&expected[i++], 0xc0000223, 1);

	/* 192.0.2.17 (19) */
	init_taddr(&expected[i++], 0xc0000211, 19);

	if (i != COUNT) {
		log_err("Input mismatch. Unit test is broken: %u %u", i, COUNT);
		return false;
	}

	/*
	 * This simulates wrap-arounding without having to reinit the array for
	 * every test.
	 */
	memcpy(&expected[COUNT], &expected[0], COUNT * sizeof(*expected));

	for (i = 0; i < 3 * COUNT; i++) {
		args.expected = &expected[i % COUNT];
		args.expected_len = COUNT;
		args.i = 0;
		error = pool4db_foreach_taddr4(&pkt, L4PROTO_TCP, NULL,
				validate_taddr4, &args, i);
		success &= ASSERT_INT(0, error, "call %u", i);
		/* log_debug("--------------"); */
	}

	return success;
}

#undef COUNT

static void init_sample(struct pool4_sample *sample, __u32 addr, __u16 min,
		__u16 max)
{
	sample->addr.s_addr = cpu_to_be32(addr);
	sample->range.min = min;
	sample->range.max = max;
}

struct foreach_sample_args {
	struct pool4_sample *expected;
	unsigned int expected_len;
	unsigned int i;
};

static int validate_sample(struct pool4_sample *sample, void *void_args)
{
	struct foreach_sample_args *args = void_args;
	bool success = true;

	/* log_debug("foreaching %pI4 %u-%u", &sample->addr, sample->range.min,
			sample->range.max); */

	success &= ASSERT_BOOL(true, args->i < args->expected_len,
			"overflow (%u %u)", args->i, args->expected_len);
	if (!success)
		return -EINVAL;

	success &= __ASSERT_ADDR4(&args->expected[args->i].addr,
			&sample->addr, "addr");
	success &= ASSERT_UINT(args->expected[args->i].range.min,
			sample->range.min, "min");
	success &= ASSERT_UINT(args->expected[args->i].range.max,
			sample->range.max, "max");

	args->i++;
	return success ? 0 : -EINVAL;
}

#define COUNT 9

static bool test_foreach_sample(void)
{
	struct pool4_sample expected[COUNT];
	unsigned int i = 0;
	struct foreach_sample_args args;
	int error;
	bool success = true;

	if (!add_common_samples())
		return false;

	init_sample(&expected[i++], 0xc0000200U, 6, 7);
	init_sample(&expected[i++], 0xc0000201U, 6, 7);
	init_sample(&expected[i++], 0xc0000210U, 22, 23);
	init_sample(&expected[i++], 0xc0000210U, 15, 19);
	init_sample(&expected[i++], 0xc0000220U, 1, 1);
	init_sample(&expected[i++], 0xc0000221U, 1, 1);
	init_sample(&expected[i++], 0xc0000222U, 1, 1);
	init_sample(&expected[i++], 0xc0000223U, 1, 1);
	init_sample(&expected[i++], 0xc0000211U, 19, 19);

	if (i != COUNT) {
		log_err("Input mismatch. Unit test is broken: %u %u", i, COUNT);
		return false;
	}

	args.expected = &expected[0];
	args.expected_len = COUNT;
	args.i = 0;
	error = pool4db_foreach_sample(validate_sample, &args, NULL);
	success &= ASSERT_INT(0, error, "no-offset call");

	for (i = 0; i < COUNT; i++) {
		/* foreach sample skips offset. */
		args.expected = &expected[i + 1];
		args.expected_len = COUNT - i - 1;
		args.i = 0;
		error = pool4db_foreach_sample(validate_sample, &args,
				&expected[i]);
		success &= ASSERT_INT(0, error, "call %u", i);
		/* log_debug("--------------"); */
	}

	return success;
}

#undef COUNT

/**
 * assert_contains_range - "assert 192.0.2.@addr_min - 192.0.2.@addr_max on
 * ports @port_min through @port_max belong to the pool (@expected true) or not
 * (@expected false)."
 */
static bool assert_contains_range(__u32 addr_min, __u32 addr_max,
		__u16 port_min, __u16 port_max, bool expected)
{
	struct ipv4_transport_addr taddr;
	__u32 i;
	bool result;
	bool success = true;

	for (i = addr_min; i <= addr_max; i++) {
		taddr.l3.s_addr = cpu_to_be32(0xc0000200U | i);
		for (taddr.l4 = port_min; taddr.l4 <= port_max; taddr.l4++) {
			result = pool4db_contains(L4PROTO_TCP, &taddr);
			success &= ASSERT_BOOL(expected, result,
					"contains %pI4#%u",
					&taddr.l3, taddr.l4);
			result = pool4db_contains(L4PROTO_TCP, &taddr);
			success &= ASSERT_BOOL(expected, result,
					"contains_all %pI4#%u",
					&taddr.l3, taddr.l4);
		}
	}

	return success;
}

static bool __foreach(struct pool4_sample *expected, unsigned int expected_len)
{
	struct foreach_sample_args args;
	int error;
	bool success = true;

	args.expected = expected;
	args.expected_len = expected_len;
	args.i = 0;

	error = pool4db_foreach_sample(validate_sample, &args, NULL);
	success &= ASSERT_INT(0, error, "foreach result");
	success &= ASSERT_UINT(expected_len, args.i, "foreach count");
	return success;
}

static bool test_add(void)
{
	struct pool4_sample samples[8];
	bool success = true;

	/* ---------------------------------------------------------- */

	/* Add a single small range. */
	if (!add(0xc0000211U, 32, 10, 20)) /* 192.0.2.17 (10-20) */
		return false;

	success &= assert_contains_range(16, 16, 0, 30, false);
	success &= assert_contains_range(17, 17, 0, 9, false);
	success &= assert_contains_range(17, 17, 10, 20, true);
	success &= assert_contains_range(17, 17, 21, 30, false);
	success &= assert_contains_range(18, 18, 0, 30, false);

	init_sample(&samples[0], 0xc0000211U, 10, 20);
	success &= __foreach(samples, 1);

	/* ---------------------------------------------------------- */

	/* Append an adjacent range (left). They should join each other. */
	if (!add(0xc0000211U, 32, 5, 10)) /* 192.0.2.17 (5-10) */
		return false;

	success &= assert_contains_range(0, 16, 0, 30, false);
	success &= assert_contains_range(17, 17, 0, 4, false);
	success &= assert_contains_range(17, 17, 5, 20, true);
	success &= assert_contains_range(17, 17, 21, 30, false);
	success &= assert_contains_range(18, 32, 0, 30, false);

	init_sample(&samples[0], 0xc0000211U, 5, 20);
	success &= __foreach(samples, 1);

	/* ---------------------------------------------------------- */

	/* Append an adjacent range (right). They should join each other. */
	if (!add(0xc0000211U, 32, 20, 25)) /* 192.0.2.17 (20-25) */
		return false;

	success &= assert_contains_range(0, 16, 0, 30, false);
	success &= assert_contains_range(17, 17, 0, 4, false);
	success &= assert_contains_range(17, 17, 5, 25, true);
	success &= assert_contains_range(17, 17, 26, 30, false);
	success &= assert_contains_range(18, 32, 0, 30, false);

	init_sample(&samples[0], 0xc0000211U, 5, 25);
	success &= __foreach(samples, 1);

	/* ---------------------------------------------------------- */

	/* Add intersecting ranges. They should join each other. */
	if (!add(0xc0000210U, 32, 10, 20)) /* 192.0.2.16 (10-20) */
		return false;
	if (!add(0xc0000210U, 32, 5, 12)) /* 192.0.2.16 (5-12) */
		return false;
	if (!add(0xc0000210U, 32, 18, 25)) /* 192.0.2.16 (18-25) */
		return false;

	success &= assert_contains_range(15, 15, 0, 30, false);
	success &= assert_contains_range(16, 17, 0, 4, false);
	success &= assert_contains_range(16, 17, 5, 25, true);
	success &= assert_contains_range(16, 17, 26, 30, false);
	success &= assert_contains_range(18, 18, 0, 30, false);

	init_sample(&samples[1], 0xc0000210U, 5, 25);
	success &= __foreach(samples, 2);

	/* ---------------------------------------------------------- */

	/* Add a bigger range. The bigger one should replace. */
	if (!add(0xc0000212U, 32, 10, 20)) /* 192.0.2.18 (10-20) */
		return false;
	if (!add(0xc0000212U, 32, 5, 25)) /* 192.0.2.18 (5-25) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 18, 0, 4, false);
	success &= assert_contains_range(16, 18, 5, 25, true);
	success &= assert_contains_range(16, 18, 26, 30, false);
	success &= assert_contains_range(19, 32, 0, 30, false);

	init_sample(&samples[2], 0xc0000212U, 5, 25);
	success &= __foreach(samples, 3);

	/* ---------------------------------------------------------- */

	/* Add an already existing range. Nothing should change. */
	if (!add(0xc0000212U, 32, 5, 25)) /* 192.0.2.18 (5-25) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 18, 0, 4, false);
	success &= assert_contains_range(16, 18, 5, 25, true);
	success &= assert_contains_range(16, 18, 26, 30, false);
	success &= assert_contains_range(19, 32, 0, 30, false);

	success &= __foreach(samples, 3);

	/* ---------------------------------------------------------- */

	/* Add a smaller range. Nothing should change. */
	if (!add(0xc0000212U, 32, 5, 25)) /* 192.0.2.18 (10-20) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 18, 0, 4, false);
	success &= assert_contains_range(16, 18, 5, 25, true);
	success &= assert_contains_range(16, 18, 26, 30, false);
	success &= assert_contains_range(19, 32, 0, 30, false);

	success &= __foreach(samples, 3);

	/* ---------------------------------------------------------- */

	/* Fill a hole. The three ranges should become one. */
	if (!add(0xc0000213U, 32, 5, 10)) /* 192.0.2.19 (5-10) */
		return false;
	if (!add(0xc0000213U, 32, 20, 25)) /* 192.0.2.19 (20-25) */
		return false;
	if (!add(0xc0000213U, 32, 11, 19)) /* 192.0.2.19 (11-19) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 19, 0, 4, false);
	success &= assert_contains_range(16, 19, 5, 25, true);
	success &= assert_contains_range(16, 19, 26, 30, false);
	success &= assert_contains_range(20, 32, 0, 30, false);

	init_sample(&samples[3], 0xc0000213U, 5, 25);
	success &= __foreach(samples, 4);

	/* ---------------------------------------------------------- */

	/* Cover several holes with one big range. */
	if (!add(0xc0000214U, 32, 8, 11)) /* 192.0.2.20 (8-11) */
		return false;
	if (!add(0xc0000214U, 32, 14, 17)) /* 192.0.2.20 (14-17) */
		return false;
	if (!add(0xc0000214U, 32, 20, 23)) /* 192.0.2.20 (20-23) */
		return false;
	if (!add(0xc0000214U, 32, 5, 25)) /* 192.0.2.20 (5-25) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 20, 0, 4, false);
	success &= assert_contains_range(16, 20, 5, 25, true);
	success &= assert_contains_range(16, 20, 26, 30, false);
	success &= assert_contains_range(21, 32, 0, 30, false);

	init_sample(&samples[4], 0xc0000214U, 5, 25);
	success &= __foreach(samples, 5);

	/* ---------------------------------------------------------- */

	/*
	 * Now add four addresses in one call.
	 * First one intersects, so only 3 are committed.
	 */
	if (!add(0xc0000214U, 30, 5, 25)) /* 192.0.2.20-23 (5-25) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 23, 0, 4, false);
	success &= assert_contains_range(16, 23, 5, 25, true);
	success &= assert_contains_range(16, 23, 26, 30, false);
	success &= assert_contains_range(24, 32, 0, 30, false);

	init_sample(&samples[5], 0xc0000215U, 5, 25);
	init_sample(&samples[6], 0xc0000216U, 5, 25);
	init_sample(&samples[7], 0xc0000217U, 5, 25);
	success &= __foreach(samples, 8);

	return success;
}

static bool test_rm(void)
{
	struct pool4_sample samples[8];
	unsigned int i;
	bool success = true;

	if (!add(0xc0000210U, 29, 5, 25)) /* 192.0.2.16-23 (5-25) */
		return false;

	/* ---------------------------------------------------------- */

	/* Remove some outermost ports from multiple addresses. */
	if (!rm(0xc0000210U, 30, 5, 9)) /* Lower of 192.0.2.16-19 (exact)*/
		return false;
	if (!rm(0xc0000214U, 30, 5, 9)) /* Lower of 192.0.2.20-23 (excess) */
		return false;
	if (!rm(0xc0000210U, 30, 21, 25)) /* Upper of 192.0.2.16-19 (exact) */
		return false;
	if (!rm(0xc0000214U, 30, 21, 30)) /* Upper of 192.0.2.20-23 (excess) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 23, 0, 9, false);
	success &= assert_contains_range(16, 23, 10, 20, true);
	success &= assert_contains_range(16, 23, 21, 30, false);
	success &= assert_contains_range(24, 32, 0, 30, false);

	for (i = 0; i < 8; i++)
		init_sample(&samples[i], 0xc0000210U + i, 10, 20);
	success &= __foreach(samples, 8);

	/* ---------------------------------------------------------- */

	/* Remove a handful of addresses completely. */
	if (!rm(0xc0000214U, 31, 10, 20)) /* 192.0.2.20-21 (exact)*/
		return false;
	if (!rm(0xc0000216U, 31, 0, 30)) /* 192.0.2.22-23 (excess)*/
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 19, 0, 9, false);
	success &= assert_contains_range(16, 19, 10, 20, true);
	success &= assert_contains_range(16, 19, 21, 30, false);
	success &= assert_contains_range(20, 32, 0, 30, false);

	success &= __foreach(samples, 4);

	/* ---------------------------------------------------------- */

	/* Punch a hole in ranges from multiple addresses. */
	if (!rm(0xc0000212U, 31, 13, 17)) /* 192.0.2.18-19 (13-17) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 17, 0, 9, false);
	success &= assert_contains_range(16, 17, 10, 20, true);
	success &= assert_contains_range(16, 17, 21, 30, false);
	success &= assert_contains_range(18, 19, 0, 9, false);
	success &= assert_contains_range(18, 19, 10, 12, true);
	success &= assert_contains_range(18, 19, 13, 17, false);
	success &= assert_contains_range(18, 19, 18, 20, true);
	success &= assert_contains_range(18, 19, 21, 30, false);
	success &= assert_contains_range(20, 32, 0, 30, false);

	init_sample(&samples[2], 0xc0000212U, 10, 12);
	init_sample(&samples[3], 0xc0000212U, 18, 20);
	init_sample(&samples[4], 0xc0000213U, 10, 12);
	init_sample(&samples[5], 0xc0000213U, 18, 20);
	success &= __foreach(samples, 6);

	/* ---------------------------------------------------------- */

	/* Remove multiple ranges from a single address at once. */
	if (!rm(0xc0000213U, 32, 0, 30)) /* 192.0.2.19 (0-30) */
		return false;

	success &= assert_contains_range(0, 15, 0, 30, false);
	success &= assert_contains_range(16, 17, 0, 9, false);
	success &= assert_contains_range(16, 17, 10, 20, true);
	success &= assert_contains_range(16, 17, 21, 30, false);
	success &= assert_contains_range(18, 18, 0, 9, false);
	success &= assert_contains_range(18, 18, 10, 12, true);
	success &= assert_contains_range(18, 18, 13, 17, false);
	success &= assert_contains_range(18, 18, 18, 20, true);
	success &= assert_contains_range(18, 18, 21, 30, false);
	success &= assert_contains_range(19, 32, 0, 30, false);

	success &= __foreach(samples, 4);

	/* ---------------------------------------------------------- */

	/* Finally, test an empty database. */
	if (!rm(0xc0000200U, 24, 0, 65535U)) /* 192.0.2.0-255 (0-65535) */
		return false;

	success &= assert_contains_range(0, 32, 0, 30, false);
	success &= __foreach(samples, 0);

	return success;
}

static bool init(void)
{
	int error;

	error = pool4db_init(4, NULL, 0);
	if (error) {
		log_err("Errcode on pool4 init: %d", error);
		return false;
	}

	return true;
}

static void destroy(void)
{
	pool4db_destroy();
}

int init_module(void)
{
	START_TESTS("IPv4 Pool DB");

	/*
	 * TODO (test) This is missing a multiple-tables test.
	 * (it always does mark = 1.)
	 */

	INIT_CALL_END(init(), test_init_power(), destroy(), "Power init");
	INIT_CALL_END(init(), test_foreach_taddr4(), destroy(), "Taddr for");
	INIT_CALL_END(init(), test_foreach_sample(), destroy(), "Sample for");
	INIT_CALL_END(init(), test_add(), destroy(), "Add");
	INIT_CALL_END(init(), test_rm(), destroy(), "Rm");

	END_TESTS;
}

void cleanup_module(void)
{
	/* No code. */
}
