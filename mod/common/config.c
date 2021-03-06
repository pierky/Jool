#include "nat64/mod/common/config.h"
#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include "nat64/common/config.h"
#include "nat64/common/constants.h"
#include "nat64/mod/common/tags.h"
#include "nat64/mod/common/types.h"

static struct global_config __rcu *config;
static DEFINE_MUTEX(lock);

RCUTAG_USR
int config_init(bool is_disable)
{
	struct global_config *cfg;
	__u16 plateaus[] = DEFAULT_MTU_PLATEAUS;

	cfg = kmalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	cfg->jool_status = 0; /* This is never read, but whatever. */
	cfg->is_disable = (__u8) is_disable;
	cfg->reset_traffic_class = DEFAULT_RESET_TRAFFIC_CLASS;
	cfg->reset_tos = DEFAULT_RESET_TOS;
	cfg->new_tos = DEFAULT_NEW_TOS;

	cfg->atomic_frags.df_always_on = DEFAULT_DF_ALWAYS_ON;
	cfg->atomic_frags.build_ipv6_fh = DEFAULT_BUILD_IPV6_FH;
	cfg->atomic_frags.build_ipv4_id = DEFAULT_BUILD_IPV4_ID;
	cfg->atomic_frags.lower_mtu_fail = DEFAULT_LOWER_MTU_FAIL;

	cfg->nat64.ttl.udp = msecs_to_jiffies(1000 * UDP_DEFAULT);
	cfg->nat64.ttl.icmp = msecs_to_jiffies(1000 * ICMP_DEFAULT);
	cfg->nat64.ttl.tcp_est = msecs_to_jiffies(1000 * TCP_EST);
	cfg->nat64.ttl.tcp_trans = msecs_to_jiffies(1000 * TCP_TRANS);
	cfg->nat64.ttl.frag = msecs_to_jiffies(1000 * FRAGMENT_MIN);
	cfg->nat64.max_stored_pkts = DEFAULT_MAX_STORED_PKTS;
	cfg->nat64.src_icmp6errs_better = DEFAULT_SRC_ICMP6ERRS_BETTER;
	cfg->nat64.f_args = DEFAULT_F_ARGS;
	cfg->nat64.handle_rst_during_fin_rcv = DEFAULT_HANDLE_FIN_RCV_RST;
	cfg->nat64.drop_by_addr = DEFAULT_ADDR_DEPENDENT_FILTERING;
	cfg->nat64.drop_external_tcp = DEFAULT_DROP_EXTERNAL_CONNECTIONS;
	cfg->nat64.drop_icmp6_info = DEFAULT_FILTER_ICMPV6_INFO;
	cfg->nat64.bib_logging = DEFAULT_BIB_LOGGING;
	cfg->nat64.session_logging = DEFAULT_SESSION_LOGGING;

	cfg->siit.compute_udp_csum_zero = DEFAULT_COMPUTE_UDP_CSUM0;
	cfg->siit.eam_hairpin_mode = DEFAULT_EAM_HAIRPIN_MODE;
	cfg->siit.randomize_error_addresses = DEFAULT_RANDOMIZE_RFC6791;

	cfg->mtu_plateau_count = ARRAY_SIZE(plateaus);
	cfg->mtu_plateaus = kmalloc(sizeof(plateaus), GFP_ATOMIC);
	if (!cfg->mtu_plateaus) {
		log_err("Could not allocate memory to store the MTU plateaus.");
		kfree(cfg);
		return -ENOMEM;
	}
	memcpy(cfg->mtu_plateaus, &plateaus, sizeof(plateaus));

	mutex_lock(&lock);
	rcu_assign_pointer(config, cfg);
	mutex_unlock(&lock);

	return 0;
}

RCUTAG_USR
void config_destroy(void)
{
	config_replace(NULL);
}

RCUTAG_PKT
int config_clone(struct global_config *clone)
{
	struct global_config *tmp;
	size_t len;
	rcu_read_lock_bh();

	/* Clone the main structure. */
	tmp = rcu_dereference_bh(config);
	*clone = *tmp;

	/* Clone plateaus. */
	len = sizeof(*tmp->mtu_plateaus) * tmp->mtu_plateau_count;
	clone->mtu_plateaus = kmalloc(len, GFP_ATOMIC);
	if (!clone->mtu_plateaus) {
		rcu_read_unlock_bh();
		return -ENOMEM;
	}
	memcpy(clone->mtu_plateaus, tmp->mtu_plateaus, len);

	rcu_read_unlock_bh();
	return 0;
}

RCUTAG_USR
void config_replace(struct global_config *new)
{
	struct global_config *old;

	mutex_lock(&lock);
	old = rcu_dereference_protected(config, lockdep_is_held(&lock));
	rcu_assign_pointer(config, new);
	mutex_unlock(&lock);

	synchronize_rcu_bh();

	kfree(old->mtu_plateaus);
	kfree(old);
}

#define RCU_THINGY(type, field) \
	({ \
		type result; \
		rcu_read_lock_bh(); \
		result = rcu_dereference_bh(config)->field; \
		rcu_read_unlock_bh(); \
		result; \
	})

unsigned long config_get_ttl_udp(void)
{
	return RCU_THINGY(unsigned long, nat64.ttl.udp);
}

unsigned long config_get_ttl_tcpest(void)
{
	return RCU_THINGY(unsigned long, nat64.ttl.tcp_est);
}

unsigned long config_get_ttl_tcptrans(void)
{
	return RCU_THINGY(unsigned long, nat64.ttl.tcp_trans);
}

unsigned long config_get_ttl_icmp(void)
{
	return RCU_THINGY(unsigned long, nat64.ttl.icmp);
}

unsigned long config_get_ttl_frag(void)
{
	return RCU_THINGY(unsigned long, nat64.ttl.frag);
}

unsigned int config_get_max_pkts(void)
{
	return RCU_THINGY(unsigned int, nat64.max_stored_pkts);
}

bool config_get_src_icmp6errs_better(void)
{
	return RCU_THINGY(bool, nat64.src_icmp6errs_better);
}

unsigned int config_get_f_args(void)
{
	return RCU_THINGY(unsigned int, nat64.f_args);
}

bool config_handle_rst_during_fin_rcv(void)
{
	return RCU_THINGY(bool, nat64.handle_rst_during_fin_rcv);
}

bool config_get_bib_logging(void)
{
	return RCU_THINGY(bool, nat64.bib_logging);
}

bool config_get_session_logging(void)
{
	return RCU_THINGY(bool, nat64.session_logging);
}

bool config_get_filter_icmpv6_info(void)
{
	return RCU_THINGY(bool, nat64.drop_icmp6_info);
}

bool config_get_addr_dependent_filtering(void)
{
	return RCU_THINGY(bool, nat64.drop_by_addr);
}

bool config_get_drop_external_connections(void)
{
	return RCU_THINGY(bool, nat64.drop_external_tcp);
}

bool config_amend_zero_csum(void)
{
	return RCU_THINGY(bool, siit.compute_udp_csum_zero);
}

enum eam_hairpinning_mode config_eam_hairpin_mode(void)
{
	return RCU_THINGY(__u8, siit.eam_hairpin_mode);
}

bool config_randomize_rfc6791_pool(void)
{
	return RCU_THINGY(bool, siit.randomize_error_addresses);
}

bool config_get_reset_traffic_class(void)
{
	return RCU_THINGY(bool, reset_traffic_class);
}

void config_get_hdr4_config(bool *reset_tos, __u8 *new_tos, bool *build_ipv4_id,
		bool *df_always_on)
{
	struct global_config *tmp;

	rcu_read_lock_bh();
	tmp = rcu_dereference_bh(config);
	*reset_tos = tmp->reset_tos;
	*new_tos = tmp->new_tos;
	if (build_ipv4_id)
		*build_ipv4_id = tmp->atomic_frags.build_ipv4_id;
	if (df_always_on)
		*df_always_on = tmp->atomic_frags.df_always_on;
	rcu_read_unlock_bh();
}

bool config_get_build_ipv6_fh(void)
{
	return RCU_THINGY(bool, atomic_frags.build_ipv6_fh);
}

bool config_get_lower_mtu_fail(void)
{
	return RCU_THINGY(bool, atomic_frags.lower_mtu_fail);
}

/**
 * You need to call rcu_read_lock_bh() before calling this function,
 * and then rcu_read_unlock_bh() when you don't need plateaus & count anymore.
 */
void config_get_mtu_plateaus(__u16 **plateaus, __u16 *count)
{
	struct global_config *tmp;

	tmp = rcu_dereference_bh(config);
	*plateaus = tmp->mtu_plateaus;
	*count = tmp->mtu_plateau_count;
}

bool config_is_xlat_disabled(void)
{
	return RCU_THINGY(bool, is_disable);
}

RCUTAG_USR /* Only because of GFP_KERNEL. Can be easily upgraded to _FREE. */
int serialize_global_config(struct global_config *config, bool pools_empty,
		unsigned char **buffer_out, size_t *buffer_len_out)
{
	unsigned char *buffer;
	struct global_config *tmp;
	size_t mtus_len;
	bool disabled;

	mtus_len = config->mtu_plateau_count * sizeof(*config->mtu_plateaus);

	buffer = kmalloc(sizeof(*config) + mtus_len, GFP_KERNEL);
	if (!buffer) {
		log_debug("Could not allocate the configuration structure.");
		return -ENOMEM;
	}

	memcpy(buffer, config, sizeof(*config));
	memcpy(buffer + sizeof(*config), config->mtu_plateaus, mtus_len);
	tmp = (struct global_config *) buffer;

	tmp->nat64.ttl.udp = jiffies_to_msecs(config->nat64.ttl.udp);
	tmp->nat64.ttl.tcp_est = jiffies_to_msecs(config->nat64.ttl.tcp_est);
	tmp->nat64.ttl.tcp_trans = jiffies_to_msecs(config->nat64.ttl.tcp_trans);
	tmp->nat64.ttl.icmp = jiffies_to_msecs(config->nat64.ttl.icmp);
	tmp->nat64.ttl.frag = jiffies_to_msecs(config->nat64.ttl.frag);

	disabled = config->is_disable || pools_empty;
	tmp->jool_status = !disabled;

	*buffer_out = buffer;
	*buffer_len_out = sizeof(*config) + mtus_len;
	return 0;
}

/* TODO (issue164) this is not being used anywhere. Review after merging #164. */
int deserialize_global_config(void *buffer, __u16 buffer_len, struct global_config *target_out)
{
	size_t mtus_len;

	memcpy(target_out, buffer, sizeof(*target_out));

	target_out->mtu_plateaus = NULL;
	if (target_out->mtu_plateau_count) {
		mtus_len = target_out->mtu_plateau_count * sizeof(*target_out->mtu_plateaus);
		target_out->mtu_plateaus = kmalloc(mtus_len, GFP_ATOMIC);
		if (!target_out->mtu_plateaus) {
			log_debug("Could not allocate the config's plateaus.");
			return -ENOMEM;
		}
		memcpy(target_out->mtu_plateaus, buffer + sizeof(*target_out), mtus_len);
	}

	target_out->nat64.ttl.udp = msecs_to_jiffies(target_out->nat64.ttl.udp);
	target_out->nat64.ttl.tcp_est = msecs_to_jiffies(target_out->nat64.ttl.tcp_est);
	target_out->nat64.ttl.tcp_trans = msecs_to_jiffies(target_out->nat64.ttl.tcp_trans);
	target_out->nat64.ttl.icmp = msecs_to_jiffies(target_out->nat64.ttl.icmp);
	target_out->nat64.ttl.frag = msecs_to_jiffies(target_out->nat64.ttl.frag);

	return 0;
}
