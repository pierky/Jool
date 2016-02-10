#include "nat64/mod/common/nl/global.h"

#include <linux/sort.h>
#include "nat64/common/constants.h"
#include "nat64/mod/common/types.h"
#include "nat64/mod/common/config.h"
#include "nat64/mod/common/pool6.h"
#include "nat64/mod/common/nl/nl_common.h"
#include "nat64/mod/common/nl/nl_core2.h"
#include "nat64/mod/stateful/joold.h"
#include "nat64/mod/stateful/session/db.h"
#include "nat64/mod/stateless/eam.h"
#include "nat64/usr/global.h"


static int ensure_siit(char *field)
{
	if (!xlat_is_siit()) {
		log_err("Field '%s' is SIIT-only.", field);
		return -EINVAL;
	}

	return 0;
}

static int ensure_nat64(char *field)
{
	if (!xlat_is_nat64()) {
		log_err("Field '%s' is NAT64-only.", field);
		return -EINVAL;
	}

	return 0;
}

static bool ensure_bytes(size_t actual, size_t expected)
{
	if (actual < expected) {
		log_err("Expected a %zu-byte value, got %zu bytes.",
				expected, actual);
		return false;
	}
	return true;
}

static int parse_bool(__u8 *field, struct global_value *chunk, size_t size)
{
	if (!ensure_bytes(size, 1))
		return -EINVAL;
	*field = *((__u8 *)(chunk + 1));
	return 0;
}

static int parse_u64(__u64 *field, struct global_value *chunk, size_t size)
{
	if (!ensure_bytes(size, 8))
		return -EINVAL;
	*field = *((__u64 *)(chunk + 1));
	return 0;
}

static int parse_u32(__u32 *field, struct global_value *chunk, size_t size)
{
	__u64 value;
	int error;

	error = parse_u64(&value, chunk, size);
	if (error)
		return error;

	*field = value;
	return 0;
}

static int parse_u8(__u8 *field, struct global_value *chunk, size_t size)
{
	__u64 value;
	int error;

	error = parse_u64(&value, chunk, size);
	if (error)
		return error;

	*field = value;
	return 0;
}

static int parse_timeout(__u64 *field, struct global_value *chunk, size_t size,
		unsigned int min)
{
	/*
	 * TODO (fine) this max is somewhat arbitrary. We do have a maximum,
	 * but I don't recall what or why it was. I do remember it's bigger than
	 * this.
	 */
	const __u32 MAX_U32 = 0xFFFFFFFFU;
	__u64 value64;

	if (!ensure_bytes(size, 8))
		return -EINVAL;

	value64 = *((__u64 *)(chunk + 1));

	if (value64 < 1000 * min) {
		log_err("The timeout must be at least %u seconds.", min);
		return -EINVAL;
	}
	if (value64 > MAX_U32) {
		log_err("Expected a timeout less than %u seconds",
				MAX_U32 / 1000);
		return -EINVAL;
	}

	*field = msecs_to_jiffies(value64);
	return 0;
}

static int be16_compare(const void *a, const void *b)
{
	return *(__u16 *)b - *(__u16 *)a;
}

static void be16_swap(void *a, void *b, int size)
{
	__u16 t = *(__u16 *)a;
	*(__u16 *)a = *(__u16 *)b;
	*(__u16 *)b = t;
}

static int update_plateaus(struct global_config *config,
		struct global_value *hdr,
		size_t max_size)
{
	__u16 *list;
	int list_length;
	unsigned int i, j;

	list_length = (hdr->len - sizeof(*hdr)) / sizeof(__u16);
	if (list_length < 1) {
		log_err("The MTU list received from userspace is empty.");
		return -EINVAL;
	}
	if (list_length > ARRAY_SIZE(config->mtu_plateaus)) {
		log_err("Too many plateau values; there's only room for %zu.",
				ARRAY_SIZE(config->mtu_plateaus));
		return -EINVAL;
	}

	list = (__u16 *)(hdr + 1);

	if (list_length * sizeof(*list) > max_size - sizeof(*hdr)) {
		log_err("The request seems truncated.");
		return -EINVAL;
	}

	/* Sort descending. */
	sort(list, list_length, sizeof(*list), be16_compare, be16_swap);

	/* Remove zeroes and duplicates. */
	for (i = 0, j = 1; j < list_length; j++) {
		if (list[j] == 0)
			break;
		if (list[i] != list[j]) {
			i++;
			list[i] = list[j];
		}
	}

	if (list[0] == 0) {
		log_err("The MTU list contains nothing but zeroes.");
		return -EINVAL;
	}

	/* Update. */
	memcpy(config->mtu_plateaus, list, (i + 1) * sizeof(*list));
	config->mtu_plateau_count = i + 1;

	return 0;
}

/* El máximo que puedo enviar a través de nlcore_respond_struct() es 4056. */

static __u8 confif[4056];

static int handle_global_display(struct xlator *jool, struct genl_info *info)
{
	struct full_config *config = (struct full_config *)confif;
	bool enabled;

	log_info("nl_core_buffer es %zu bytes.", sizeof(struct nlcore_buffer));
	log_info("Van a ser %zu bytes.", sizeof(confif));

	log_debug("Returning 'Global' options.");

	xlator_copy_config(jool, config);

	enabled = !pool6_is_empty(jool->pool6);
	if (xlat_is_nat64())
		enabled |= !eamt_is_empty(jool->siit.eamt);
	prepare_config_for_userspace(config, enabled);

	return nlcore_respond_struct(info, config, sizeof(confif));
}

static int massive_switch(struct full_config *cfg, struct global_value *chunk,
		size_t size)
{
	__u8 tmp8;
	int error;

log_info("massive");

	if (!ensure_bytes(size, chunk->len))
		return -EINVAL;

	switch (chunk->type) {
	case ENABLE:
		cfg->global.enabled = true;
		return 0;
	case DISABLE:
		cfg->global.enabled = false;
		return 0;
	case ENABLE_BOOL:
		return parse_bool(&cfg->global.enabled, chunk, size);
	case RESET_TCLASS:
		return parse_bool(&cfg->global.reset_traffic_class, chunk, size);
	case RESET_TOS:
		return parse_bool(&cfg->global.reset_tos, chunk, size);
	case NEW_TOS:
		return parse_u8(&cfg->global.new_tos, chunk, size);
	case ATOMIC_FRAGMENTS:
		error = parse_bool(&tmp8, chunk, size);
		if (error)
			return error;
		cfg->global.atomic_frags.df_always_on = tmp8;
		cfg->global.atomic_frags.build_ipv6_fh = tmp8;
		cfg->global.atomic_frags.build_ipv4_id = !tmp8;
		cfg->global.atomic_frags.lower_mtu_fail = !tmp8;
		return 0;
	case DF_ALWAYS_ON:
		return parse_bool(&cfg->global.atomic_frags.df_always_on, chunk, size);
	case BUILD_IPV6_FH:
		return parse_bool(&cfg->global.atomic_frags.build_ipv6_fh, chunk, size);
	case BUILD_IPV4_ID:
		return parse_bool(&cfg->global.atomic_frags.build_ipv4_id, chunk, size);
	case LOWER_MTU_FAIL:
		return parse_bool(&cfg->global.atomic_frags.lower_mtu_fail, chunk, size);
	case MTU_PLATEAUS:
		return update_plateaus(&cfg->global, chunk, size);
	case COMPUTE_UDP_CSUM_ZERO:
		error = ensure_siit(OPTNAME_AMEND_UDP_CSUM);
		return error ? : parse_bool(&cfg->global.siit.compute_udp_csum_zero, chunk, size);
	case RANDOMIZE_RFC6791:
		error = ensure_siit(OPTNAME_RANDOMIZE_RFC6791);
		return error ? : parse_bool(&cfg->global.siit.randomize_error_addresses, chunk, size);
	case EAM_HAIRPINNING_MODE:
		error = ensure_siit(OPTNAME_EAM_HAIRPIN_MODE);
		return error ? : parse_bool(&cfg->global.siit.eam_hairpin_mode, chunk, size);
	case DROP_BY_ADDR:
		error = ensure_nat64(OPTNAME_DROP_BY_ADDR);
		return error ? : parse_bool(&cfg->global.nat64.drop_by_addr, chunk, size);
	case DROP_ICMP6_INFO:
		error = ensure_nat64(OPTNAME_DROP_ICMP6_INFO);
		return error ? : parse_bool(&cfg->global.nat64.drop_icmp6_info, chunk, size);
	case DROP_EXTERNAL_TCP:
		error = ensure_nat64(OPTNAME_DROP_EXTERNAL_TCP);
		return error ? : parse_bool(&cfg->global.nat64.drop_external_tcp, chunk, size);
	case SRC_ICMP6ERRS_BETTER:
		error = ensure_nat64(OPTNAME_SRC_ICMP6E_BETTER);
		return error ? : parse_bool(&cfg->global.nat64.src_icmp6errs_better, chunk, size);
	case UDP_TIMEOUT:
		error = ensure_nat64(OPTNAME_UDP_TIMEOUT);
		return error ? : parse_timeout(&cfg->session.ttl.udp, chunk, size, UDP_MIN);
	case ICMP_TIMEOUT:
		error = ensure_nat64(OPTNAME_ICMP_TIMEOUT);
		return error ? : parse_timeout(&cfg->session.ttl.icmp, chunk, size, 0);
	case TCP_EST_TIMEOUT:
		error = ensure_nat64(OPTNAME_TCPEST_TIMEOUT);
		return error ? : parse_timeout(&cfg->session.ttl.tcp_est, chunk, size, TCP_EST);
	case TCP_TRANS_TIMEOUT:
		error = ensure_nat64(OPTNAME_TCPTRANS_TIMEOUT);
		return error ? : parse_timeout(&cfg->session.ttl.tcp_trans, chunk, size, TCP_TRANS);
	case FRAGMENT_TIMEOUT:
		error = ensure_nat64(OPTNAME_FRAG_TIMEOUT);
		return error ? : parse_timeout(&cfg->global.nat64.ttl.frag, chunk, size, FRAGMENT_MIN);
	case BIB_LOGGING:
		error = ensure_nat64(OPTNAME_BIB_LOGGING);
		return error ? : parse_bool(&cfg->bib.log_changes, chunk, size);
	case SESSION_LOGGING:
		error = ensure_nat64(OPTNAME_SESSION_LOGGING);
		return error ? : parse_bool(&cfg->session.log_changes, chunk, size);
	case MAX_PKTS:
		error = ensure_nat64(OPTNAME_MAX_SO);
		return error ? : parse_u32(&cfg->session.pktqueue.max_stored_pkts, chunk, size);
	case SYNCH_ENABLE:
		error = ensure_nat64(OPTNAME_SYNCH_ENABLE);
		if (!error)
			cfg->session.joold.enabled = true;
		return error;
	case SYNCH_DISABLE:
		error = ensure_nat64(OPTNAME_SYNCH_DISABLE);
		if (!error)
			cfg->session.joold.enabled = false;
		return error;
	case SYNCH_ELEMENTS_LIMIT:
		error = ensure_nat64(OPTNAME_SYNCH_MAX_SESSIONS);
		return error ? : parse_u32(&cfg->session.joold.queue_capacity, chunk, size);
	case SYNCH_PERIOD:
		error = ensure_nat64(OPTNAME_SYNCH_PERIOD);
		return error ? : parse_u32(&cfg->session.joold.timer_period, chunk, size);
	}

	log_err("Unknown config type: %u", chunk->type);
	return -EINVAL;
}

/**
 * On success, returns the number of bytes consumed from @payload.
 * On error, returns a negative error code.
 */
int config_parse(struct full_config *config, void *payload, size_t payload_len)
{
	struct global_value *chunk;
	size_t bytes_read = 0;
	int error;

	while (payload_len > 0) {
		if (!ensure_bytes(payload_len, sizeof(struct global_value)))
			return -EINVAL;

		chunk = payload;
		error = massive_switch(config, chunk, payload_len);
		if (error)
			return error;

		payload += chunk->len;
		payload_len -= chunk->len;
		bytes_read += chunk->len;
	}

	return bytes_read;
}

static int commit_config(struct xlator *jool, struct full_config *config)
{
	int error;

	config_put(jool->global);
	error = config_init(&jool->global, false);
	if (error)
		return error;

	config_copy(&config->global, &jool->global->cfg);
	bibdb_config_set(jool->nat64.bib, &config->bib);
	sessiondb_config_set(jool->nat64.session, &config->session);

	return xlator_replace(jool);
}

static int handle_global_update(struct xlator *jool, struct genl_info *info)
{
	struct request_hdr *hdr = get_jool_hdr(info);
	struct full_config config;
	int error;

	if (verify_superpriv())
		return nlcore_respond_error(info, -EPERM);

	log_debug("Updating 'Global' options.");

	xlator_copy_config(jool, &config);

	error = config_parse(&config, hdr + 1, hdr->length - sizeof(*hdr));
	if (error < 0)
		return nlcore_respond_error(info, error);

	error = commit_config(jool, &config);
	return nlcore_respond(info, error);
}

int handle_global_config(struct xlator *jool, struct genl_info *info)
{
	struct request_hdr *jool_hdr = get_jool_hdr(info);

	switch (jool_hdr->operation) {
	case OP_DISPLAY:
		return handle_global_display(jool, info);
	case OP_UPDATE:
		return handle_global_update(jool, info);
	}

	log_err("Unknown operation: %d", jool_hdr->operation);
	return nlcore_respond_error(info, -EINVAL);
}