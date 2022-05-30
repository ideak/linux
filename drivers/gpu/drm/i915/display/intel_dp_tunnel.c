#include <linux/types.h>

#include <drm/drm_atomic_state_helper.h>

#include "i915_drv.h"

#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_tunnel.h"
#include "intel_link_bw.h"

#define MAX_DP_TUNNELS_PER_GROUP 3

#define to_group(__private_obj) \
	container_of(__private_obj, struct intel_dp_tunnel_group, base)

#define to_group_state(__private_state) \
	container_of(__private_state, struct intel_dp_tunnel_group_state, base)

#define to_tunnel_group(__tunnel) \
	container_of(__tunnel, struct intel_dp_tunnel_group, tunnels[(__tunnel)->idx])

#define to_tunnel_state(__group_state, __tunnel) \
	(&(__group_state)->tunnel_states[(__tunnel)->idx])

#define for_each_new_group_in_state(__state, __group, __new_group_state, __i) \
	for ((__i) = 0; \
	     (__i) < (__state)->base.num_private_objs; \
	     (__i)++) \
		for_each_if ((__state)->base.private_objs[__i].ptr && \
			     (__state)->base.private_objs[__i].ptr->funcs == &tunnel_group_funcs && \
			     ((__group) = to_group((__state)->base.private_objs[__i].ptr), \
			      (__new_group_state) = to_group_state((__state)->base.private_objs[__i].new_state), 1))

#define for_each_old_group_in_state(__state, __group, __old_group_state, __i) \
	for ((__i) = 0; \
	     (__i) < (__state)->base.num_private_objs; \
	     (__i)++) \
		for_each_if ((__state)->base.private_objs[__i].ptr && \
			     (__state)->base.private_objs[__i].ptr->funcs == &tunnel_group_funcs && \
			     ((__group) = to_group((__state)->base.private_objs[__i].ptr), \
			      (__old_group_state) = to_group_state((__state)->base.private_objs[__i].old_state), 1))

#define for_each_tunnel_in_group(__group, __tunnel) \
	for ((__tunnel) = &(__group)->tunnels[0]; \
	     (__tunnel) - (__group)->tunnels < ARRAY_SIZE((__group)->tunnels); \
	     (__tunnel)++)

#define for_each_bw_alloc_tunnel_in_group(__group, __group_state, __tunnel, __tunnel_state) \
	for_each_tunnel_in_group(__group, __tunnel) \
		for_each_if ((__tunnel)->bw_alloc_enabled && \
			     ((__tunnel_state) = to_tunnel_state(__group_state, __tunnel), 1))

#define DPTUN_BW_ARG(__bw) ((__bw) / 100)

#define __tun_prn(__tunnel, __level, __type, __fmt, ...) \
	drm_##__level##__type(&to_tunnel_group(__tunnel)->mgr->i915->drm, \
			      "[%s][DPTUN %d:%d:%d]: " __fmt, \
			      (__tunnel)->aux->name, \
			      tunnel_group_drv_id(to_tunnel_group(__tunnel)), \
			      tunnel_group_id(to_tunnel_group(__tunnel)), \
			      (__tunnel)->adapter_id, ## \
			      __VA_ARGS__)

#define tun_dbg(__tunnel, __fmt, ...) \
	__tun_prn(__tunnel, dbg, _kms, __fmt, ## __VA_ARGS__)

#define tun_dbg_atomic(__tunnel, __fmt, ...) \
	__tun_prn(__tunnel, dbg, _atomic, __fmt, ## __VA_ARGS__)

#define tun_err(__tunnel, __fmt, ...) \
	__tun_prn(__tunnel, err,, __fmt, ## __VA_ARGS__)

#define tun_grp_dbg(__group, __fmt, ...) \
	drm_dbg_kms(&(__group)->mgr->i915->drm, \
		    "[DPTUN %d:%d:*]: " __fmt, \
		    tunnel_group_drv_id(__group), \
		    tunnel_group_id(__group), ## \
		    __VA_ARGS__)

struct intel_dp_tunnel_group;
struct intel_dp_tunnel_mgr;

struct intel_dp_tunnel_info {
	u8 dev_ids[12];
	u8 dev_info[3];
	u8 dev_status[6];
	u8 dprx_caps[2];
	u8 drv_status[2];
};

struct intel_dp_tunnel {
	struct drm_dp_aux *aux;

	int bw_granularity;
	int allocated_bw;

	int max_dprx_rate;
	u8 max_dprx_lane_count;

	u8 adapter_id;

	u8 idx:order_base_2(MAX_DP_TUNNELS_PER_GROUP);
	u8 active:1;

	u8 bw_alloc_supported:1;
	u8 bw_alloc_enabled:1;
	u8 bw_alloc_suspended:1;
	u8 bw_alloc_has_errors:1;
};

struct intel_dp_tunnel_state {
	int required_bw;
};

struct intel_dp_tunnel_group_state {
	struct drm_private_state base;

	struct intel_dp_tunnel_state tunnel_states[MAX_DP_TUNNELS_PER_GROUP];
};

struct intel_dp_tunnel_group {
	struct drm_private_obj base;
	struct intel_dp_tunnel_mgr *mgr;

	int tunnel_count;
	struct intel_dp_tunnel tunnels[MAX_DP_TUNNELS_PER_GROUP];

	int available_bw;
	int drv_group_id;
};

struct intel_dp_tunnel_mgr {
	struct drm_i915_private *i915;

	int group_count;
	int active_group_count;
	struct intel_dp_tunnel_group *groups;
	wait_queue_head_t bw_req_queue;
};

static u8 tunnel_info_dev_id_reg(const struct intel_dp_tunnel_info *info, int reg)
{
	return info->dev_ids[reg - DP_TUNNELING_OUI];
}

static u8 tunnel_info_dev_status_reg(const struct intel_dp_tunnel_info *info, int reg)
{
	return info->dev_status[reg - DP_USB4_DRIVER_BW_CAPABILITY];
}

static u8 tunnel_info_dev_info_reg(const struct intel_dp_tunnel_info *info, int reg)
{
	return info->dev_info[reg - DP_TUNNELING_CAPABILITIES];
}

static int tunnel_info_dprx_cap_reg(const struct intel_dp_tunnel_info *info, int reg)
{
	return info->dprx_caps[reg - DP_TUNNELING_MAX_LINK_RATE];
}

static int tunnel_info_drv_status_reg(const struct intel_dp_tunnel_info *info, int reg)
{
	return info->drv_status[reg - DP_DPTX_BW_ALLOCATION_MODE_CONTROL];
}

static const u8 *tunnel_info_dev_oui(const struct intel_dp_tunnel_info *info)
{
	return info->dev_ids;
}

static const u8 *tunnel_info_dev_id(const struct intel_dp_tunnel_info *info)
{
	return &info->dev_ids[DP_TUNNELING_DEV_ID - DP_TUNNELING_OUI];
}

static u8 tunnel_info_dev_hw_rev_major(const struct intel_dp_tunnel_info *info)
{
	return (tunnel_info_dev_id_reg(info, DP_TUNNELING_HW_REV) &
	        DP_TUNNELING_HW_REV_MAJOR_MASK) >> DP_TUNNELING_HW_REV_MAJOR_SHIFT;
}

static u8 tunnel_info_dev_hw_rev_minor(const struct intel_dp_tunnel_info *info)
{
	return (tunnel_info_dev_id_reg(info, DP_TUNNELING_HW_REV) &
	        DP_TUNNELING_HW_REV_MINOR_MASK) >> DP_TUNNELING_HW_REV_MINOR_SHIFT;
}

static u8 tunnel_info_dev_sw_rev_major(const struct intel_dp_tunnel_info *info)
{
	return tunnel_info_dev_id_reg(info, DP_TUNNELING_SW_REV_MAJOR);
}

static u8 tunnel_info_dev_sw_rev_minor(const struct intel_dp_tunnel_info *info)
{
	return tunnel_info_dev_id_reg(info, DP_TUNNELING_SW_REV_MINOR);
}

static int tunnel_info_drv_id(const struct intel_dp_tunnel_info *info)
{
	return tunnel_info_dev_info_reg(info, DP_USB4_DRIVER_ID) &
	       DP_USB4_DRIVER_ID_MASK;
}

static int tunnel_info_group_id(const struct intel_dp_tunnel_info *info)
{
	return tunnel_info_dev_status_reg(info, DP_IN_ADAPTER_TUNNEL_INFORMATION) &
	       DP_GROUP_ID_MASK;
}

static int tunnel_info_drv_group_id(const struct intel_dp_tunnel_info *info)
{
	int group_id = tunnel_info_group_id(info);

	if (!group_id)
		return 0;

	return (tunnel_info_drv_id(info) << DP_GROUP_ID_BITS) | group_id;
}

static int tunnel_info_adapter_id(const struct intel_dp_tunnel_info *info)
{
	return tunnel_info_dev_info_reg(info, DP_IN_ADAPTER_INFO) &
	       DP_IN_ADAPTER_NUMBER_MASK;
}

static int tunnel_info_bw_granularity(const struct intel_dp_tunnel_info *info)
{
	int gr = tunnel_info_dev_status_reg(info, DP_BW_GRANULARITY) &
		 DP_BW_GRANULARITY_MASK;

	WARN_ON(gr > 2);

	return 25000 * (1 << gr);
}

static int tunnel_info_estimated_bw(const struct intel_dp_tunnel_info *info)
{
	return tunnel_info_dev_status_reg(info, DP_ESTIMATED_BW) *
	       tunnel_info_bw_granularity(info);
}

static int tunnel_info_allocated_bw(const struct intel_dp_tunnel_info *info)
{
	return tunnel_info_dev_status_reg(info, DP_ALLOCATED_BW) *
	       tunnel_info_bw_granularity(info);
}

static int tunnel_info_max_dprx_rate(const struct intel_dp_tunnel_info *info)
{
	u8 bw_code = tunnel_info_dprx_cap_reg(info, DP_TUNNELING_MAX_LINK_RATE);

	return drm_dp_bw_code_to_link_rate(bw_code);
}

static int tunnel_info_max_dprx_lane_count(const struct intel_dp_tunnel_info *info)
{
	u8 lane_count = tunnel_info_dprx_cap_reg(info, DP_TUNNELING_MAX_LANE_COUNT) &
			DP_TUNNELING_MAX_LANE_COUNT_MASK;

	return lane_count;
}

static bool tunnel_info_bw_alloc_supported(const struct intel_dp_tunnel_info *info)
{
	u8 cap_mask = DP_TUNNELING_SUPPORT | DP_IN_BW_ALLOCATION_MODE_SUPPORT;

	if ((tunnel_info_dev_info_reg(info, DP_TUNNELING_CAPABILITIES) & cap_mask) != cap_mask)
		return false;

	return tunnel_info_dev_status_reg(info, DP_USB4_DRIVER_BW_CAPABILITY) &
	       DP_USB4_DRIVER_BW_ALLOCATION_MODE_SUPPORT;
}

static bool tunnel_info_bw_alloc_enabled(const struct intel_dp_tunnel_info *info)
{
	return tunnel_info_drv_status_reg(info, DP_DPTX_BW_ALLOCATION_MODE_CONTROL) &
	       DP_DISPLAY_DRIVER_BW_ALLOCATION_MODE_ENABLE;
}

static bool read_tunnel_info(struct drm_dp_aux *aux, struct intel_dp_tunnel_info *info_ret)
{
	struct intel_dp_tunnel_info info = {};

	if (drm_dp_dpcd_read(aux, DP_TUNNELING_OUI,
			     info.dev_ids, sizeof(info.dev_ids)) < 0)
		return false;

	if (drm_dp_dpcd_read(aux, DP_TUNNELING_CAPABILITIES,
			     &info.dev_info, sizeof(info.dev_info)) < 0)
		return false;

	if (drm_dp_dpcd_read(aux, DP_USB4_DRIVER_BW_CAPABILITY,
			     &info.dev_status, sizeof(info.dev_status)) < 0)
		return false;

	if (drm_dp_dpcd_read(aux, DP_TUNNELING_MAX_LINK_RATE,
			     &info.dprx_caps, sizeof(info.dprx_caps)) < 0)
		return false;

	if (drm_dp_dpcd_read(aux, DP_DPTX_BW_ALLOCATION_MODE_CONTROL,
			     &info.drv_status, sizeof(info.drv_status)) < 0)
		return false;

	*info_ret = info;

	return true;
}

static int tunnel_group_drv_id(struct intel_dp_tunnel_group *group)
{
	return group->drv_group_id >> DP_GROUP_ID_BITS;
}

static int tunnel_group_id(struct intel_dp_tunnel_group *group)
{
	return group->drv_group_id & DP_GROUP_ID_MASK;
}

static struct intel_dp_tunnel_group *
lookup_or_alloc_group(struct intel_dp_tunnel_mgr *mgr, int drv_group_id)
{
	struct intel_dp_tunnel_group *group = NULL;
	int i;

	drm_WARN_ON(&mgr->i915->drm, !drv_group_id);

	for (i = 0; i < mgr->group_count; i++) {
		if (mgr->groups[i].drv_group_id == drv_group_id)
			return &mgr->groups[i];

		if (!group && !mgr->groups[i].drv_group_id)
			group = &mgr->groups[i];
	}

	if (!group ||
	    drm_WARN_ON(&mgr->i915->drm, mgr->active_group_count == mgr->group_count)) {
		drm_dbg_kms(&mgr->i915->drm, "Can't allocate more tunnel groups\n");
		return NULL;
	}

	mgr->active_group_count++;
	group->drv_group_id = drv_group_id;

	return group;
}

static void free_group(struct intel_dp_tunnel_group *group)
{
	struct intel_dp_tunnel_mgr *mgr = group->mgr;

	if (drm_WARN_ON(&mgr->i915->drm, !mgr->active_group_count))
		return;

	mgr->active_group_count--;

	group->drv_group_id = 0;
}

static struct intel_dp_tunnel *alloc_tunnel(struct intel_dp_tunnel_mgr *mgr, int drv_group_id)
{
	struct intel_dp_tunnel_group *group = lookup_or_alloc_group(mgr, drv_group_id);
	struct intel_dp_tunnel *tunnel;
	int i;

	if (!group)
		return NULL;

	if (group->tunnel_count == ARRAY_SIZE(group->tunnels))
		return NULL;

	for (i = 0; i < ARRAY_SIZE(group->tunnels); i++) {
		tunnel = &group->tunnels[i];

		if (!tunnel->active)
			break;
	}

	if (drm_WARN_ON(&group->mgr->i915->drm, !tunnel))
		return NULL;

	tunnel->idx = i;
	tunnel->active = true;

	group->tunnel_count++;

	return tunnel;
}

static void free_tunnel(struct intel_dp_tunnel *tunnel)
{
	struct intel_dp_tunnel_group *group = to_tunnel_group(tunnel);

	if (drm_WARN_ON(&group->mgr->i915->drm, !group->tunnel_count))
		return;

	if (!--group->tunnel_count)
		free_group(group);

	memset(tunnel, 0, sizeof(*tunnel));
}

static void set_bw_alloc_error(struct intel_dp_tunnel *tunnel)
{
	tunnel->bw_alloc_has_errors = true;
}

static int group_allocated_bw(struct intel_dp_tunnel_group *group)
{
	struct intel_dp_tunnel *tunnel;
	struct intel_dp_tunnel_state *tunnel_state;
	int bw = 0;

	for_each_bw_alloc_tunnel_in_group(group, to_group_state(group->base.state),
					  tunnel, tunnel_state) {
		(void)tunnel_state; /* suppress variable not-used warn */
		bw += tunnel->allocated_bw;
	}

	return bw;
}

static struct intel_dp_tunnel_state *
tunnel_state(struct intel_dp_tunnel *tunnel)
{
	struct intel_dp_tunnel_group *group = to_tunnel_group(tunnel);

	return to_tunnel_state(to_group_state(group->base.state), tunnel);
}

static bool update_group_available_bw(struct intel_dp_tunnel *tunnel,
				      int tunnel_available_bw)
{
	struct intel_dp_tunnel_group *group = to_tunnel_group(tunnel);
	int available_bw = group_allocated_bw(group) - tunnel->allocated_bw +
			   tunnel_available_bw;

	if (group->available_bw != available_bw) {
		group->available_bw = available_bw;
		return true;
	}

	return false;
}

static struct intel_dp_tunnel *
add_tunnel(struct intel_dp_tunnel_mgr *mgr,
	   struct drm_dp_aux *aux,
	   struct intel_dp_tunnel_info *info)
{
	struct intel_dp_tunnel *tunnel = alloc_tunnel(mgr, tunnel_info_drv_group_id(info));

	if (!tunnel) {
		drm_dbg_kms(&mgr->i915->drm, "Can't allocate more tunnels\n");
		return NULL;
	}

	tunnel->aux = aux;

	tunnel->adapter_id = tunnel_info_adapter_id(info);

	tunnel->bw_alloc_supported = tunnel_info_bw_alloc_supported(info);
	tunnel->bw_granularity = tunnel_info_bw_granularity(info);
	tunnel->max_dprx_rate = tunnel_info_max_dprx_rate(info);
	tunnel->max_dprx_lane_count = tunnel_info_max_dprx_lane_count(info);
	tunnel->allocated_bw = tunnel_info_allocated_bw(info);

	tunnel_state(tunnel)->required_bw = tunnel->allocated_bw;

	return tunnel;
}

static void remove_tunnel(struct intel_dp_tunnel *tunnel)
{
	free_tunnel(tunnel);
}

static bool set_bw_alloc_mode(struct intel_dp_tunnel *tunnel, bool enable)
{
	u8 mask = DP_DISPLAY_DRIVER_BW_ALLOCATION_MODE_ENABLE | DP_UNMASK_BW_ALLOCATION_IRQ;
	u8 val;

	if (drm_dp_dpcd_readb(tunnel->aux, DP_DPTX_BW_ALLOCATION_MODE_CONTROL, &val) < 0)
		return false;

	if (enable)
		val |= mask;
	else
		val &= ~mask;

	if (drm_dp_dpcd_writeb(tunnel->aux, DP_DPTX_BW_ALLOCATION_MODE_CONTROL, val) < 0)
		return false;

	tunnel->bw_alloc_enabled = enable;

	return true;
}

static bool check_tunnel_info(struct intel_dp_tunnel *tunnel, struct intel_dp_tunnel_info *info)
{
	if (tunnel_info_drv_group_id(info) != to_tunnel_group(tunnel)->drv_group_id)
		return false;

	if (tunnel_info_bw_granularity(info) != tunnel->bw_granularity) {
		tun_dbg(tunnel, "BW granularity mismatch: %d/%d\n",
			tunnel_info_bw_granularity(info),
			tunnel->bw_granularity);
		return false;
	}

	if (hweight8(tunnel_info_max_dprx_lane_count(info)) != 1) {
		tun_dbg(tunnel, "Invalid DPRX lane count: %d\n",
			tunnel_info_max_dprx_lane_count(info));
		return false;
	}

	if (!tunnel_info_max_dprx_rate(info)) {
		tun_dbg(tunnel, "DPRX rate is 0\n");
		return false;
	}

	if (tunnel_info_allocated_bw(info) != tunnel->allocated_bw) {
		tun_dbg(tunnel, "Allocate BW mismatch: %d/%d\n",
			tunnel_info_allocated_bw(info),
			tunnel->allocated_bw);
		return false;
	}

	if (tunnel_info_estimated_bw(info) < tunnel->allocated_bw) {
		tun_dbg(tunnel, "Estimated BW < allocated BW: %d/%d\n",
			tunnel_info_estimated_bw(info),
			tunnel->allocated_bw);
		return false;
	}

	if (tunnel->bw_alloc_supported && !tunnel_info_bw_alloc_supported(info)) {
		tun_dbg(tunnel, "BW alloc support mismatch: %d/%d\n",
			tunnel_info_bw_alloc_supported(info),
			tunnel->bw_alloc_supported);
		return false;
	}

	return true;
}

static bool
read_and_check_tunnel_info(struct intel_dp_tunnel *tunnel, struct intel_dp_tunnel_info *info)
{
	if (!read_tunnel_info(tunnel->aux, info))
		return false;

	return check_tunnel_info(tunnel, info);
}

static bool update_tunnel_state(struct intel_dp_tunnel *tunnel, struct intel_dp_tunnel_info *info)
{
	bool changed = false;

	if (tunnel_info_bw_alloc_supported(info) != tunnel->bw_alloc_supported) {
		tunnel->bw_alloc_supported = tunnel_info_bw_alloc_supported(info);
		changed = true;
	}

	if (tunnel_info_max_dprx_rate(info) != tunnel->max_dprx_rate) {
		tunnel->max_dprx_rate = tunnel_info_max_dprx_rate(info);
		changed = true;
	}

	if (tunnel_info_max_dprx_lane_count(info) != tunnel->max_dprx_lane_count) {
		tunnel->max_dprx_lane_count = tunnel_info_max_dprx_lane_count(info);
		changed = true;
	}

	if (update_group_available_bw(tunnel, tunnel_info_estimated_bw(info)))
		changed = true;

	return changed;
}

static int dev_id_len(const u8 *dev_id, int max_len)
{
	while (max_len && dev_id[max_len - 1] == '\0')
		max_len--;

	return max_len;
}

static int get_max_dprx_bw(const struct intel_dp_tunnel *tunnel)
{
	/*
	 * Convert the link rate to 10 kbit/s, removing the 8b/10b / 128/132b encoding
	 * overhead. (The TBT DP-in/DP-out adapters removes/restore this encoding, so
	 * the BW allocation requests are expected to exclude the overhead as well.)
	 */
	if (drm_dp_is_uhbr_rate(tunnel->max_dprx_rate))
		return tunnel->max_dprx_rate * tunnel->max_dprx_lane_count * 128 / 132;
	else
		return tunnel->max_dprx_rate * tunnel->max_dprx_lane_count * 8 / 10;
}

static int get_max_tunnel_bw(const struct intel_dp_tunnel *tunnel,
			     const struct intel_dp_tunnel_info *info)
{
	int max_bw;

	max_bw = min(get_max_dprx_bw(tunnel), tunnel_info_estimated_bw(info));
	max_bw = roundup(max_bw, tunnel->bw_granularity);

	return max_bw;
}

/**
 * intel_dp_tunnel_detect - Detect DP tunnel on the link
 * @mgr: Tunnel manager
 * @aux: DP AUX on which the tunnel will be detected
 *
 * Detect if there is any DP tunnel on the link and add it to the tunnel topology.
 *
 * Returns a pointer to the new tunnel object or NULL if no tunnel was detected
 * or adding the tunnel to the tunnel topology failed.
 */
struct intel_dp_tunnel *
intel_dp_tunnel_detect(struct intel_dp_tunnel_mgr *mgr, struct drm_dp_aux *aux)
{
	struct intel_dp_tunnel *tunnel;
	struct intel_dp_tunnel_info info;
	int max_tunnel_bw;

	if (!read_tunnel_info(aux, &info))
		return NULL;

	if (!tunnel_info_bw_alloc_supported(&info))
		return NULL;

	tunnel = add_tunnel(mgr, aux, &info);
	if (!tunnel)
		return NULL;

	max_tunnel_bw = get_max_tunnel_bw(tunnel, &info);
	tun_dbg(tunnel,
		"OUI:%*phD DevID:%*pE Rev-HW:%d.%d SW:%d.%d DPRX:%dx%d Mb/s BW-Sup:%c En:%c Alloc tunnel:%d/%d Group: %d/%d Mb/s\n",
		DP_TUNNELING_OUI_BYTES,
			tunnel_info_dev_oui(&info),
		dev_id_len(tunnel_info_dev_id(&info), DP_TUNNELING_DEV_ID_BYTES),
			tunnel_info_dev_id(&info),
		tunnel_info_dev_hw_rev_major(&info), tunnel_info_dev_hw_rev_minor(&info),
		tunnel_info_dev_sw_rev_major(&info), tunnel_info_dev_sw_rev_minor(&info),
		DPTUN_BW_ARG(tunnel->max_dprx_rate), tunnel->max_dprx_lane_count,
		tunnel->bw_alloc_supported ? 'Y' : 'N',
		tunnel_info_bw_alloc_enabled(&info) ? 'Y' : 'N',
		DPTUN_BW_ARG(tunnel->allocated_bw),
		DPTUN_BW_ARG(max_tunnel_bw),
		DPTUN_BW_ARG(group_allocated_bw(to_tunnel_group(tunnel))),
		DPTUN_BW_ARG(to_tunnel_group(tunnel)->available_bw));

	return tunnel;
}

/**
 * intel_dp_tunnel_destroy - Destroy tunnel object
 * @tunnel: Tunnel object
 *
 * Remove the tunnel from the tunnel topology and destroy it.
 */
void intel_dp_tunnel_destroy(struct intel_dp_tunnel *tunnel)
{
	remove_tunnel(tunnel);
}

static bool bw_req_complete(struct drm_dp_aux *aux, bool *req_succeeded)
{
	u8 mask = DP_BW_REQUEST_SUCCEEDED | DP_BW_REQUEST_FAILED;
	u8 val;

	*req_succeeded = false;

	if (drm_dp_dpcd_readb(aux, DP_TUNNELING_STATUS, &val) < 0)
		return true;

	val &= mask;

	if (!val)
		return false;

	if (drm_dp_dpcd_writeb(aux, DP_TUNNELING_STATUS, val) < 0)
		return true;

	*req_succeeded = val == DP_BW_REQUEST_SUCCEEDED;

	return true;
}

static bool intel_dp_tunnel_allocate_bw(struct intel_dp_tunnel *tunnel, int bw)
{
	struct intel_dp_tunnel_mgr *mgr = to_tunnel_group(tunnel)->mgr;
	int request_bw = DIV_ROUND_UP(bw, tunnel->bw_granularity);
	bool req_succeeded = false;
	unsigned long wait_expires;
	int req_complete;
	DEFINE_WAIT(wait);

	if (!tunnel->bw_alloc_enabled)
		return false;

	if (request_bw > 255) {
		tun_dbg(tunnel,
			"Can't allocate %d Mb/s with BW granularity %d Mb/s\n",
			DPTUN_BW_ARG(bw),
			DPTUN_BW_ARG(tunnel->bw_granularity));
		return false;
	}

	if (drm_dp_dpcd_writeb(tunnel->aux, DP_REQUEST_BW, request_bw) < 0)
		goto out;

	wait_expires = jiffies + msecs_to_jiffies_timeout(5000);

	for (;;) {
		req_complete = bw_req_complete(tunnel->aux, &req_succeeded);
		if (req_complete)
			break;

		if (time_after(jiffies, wait_expires))
			break;

		prepare_to_wait(&mgr->bw_req_queue, &wait, TASK_UNINTERRUPTIBLE);
		schedule_timeout(msecs_to_jiffies(50));
	};

	finish_wait(&mgr->bw_req_queue, &wait);

	if (!req_succeeded) {
		if (!req_complete || bw < tunnel->allocated_bw)
			set_bw_alloc_error(tunnel);
		goto out;
	}

	tunnel->allocated_bw = bw;
out:
	tun_dbg(tunnel, "Allocating %d Mb/s: %s (Group allocated: %d/%d Mb/s)\n",
		DPTUN_BW_ARG(bw),
		req_succeeded ? "Ok" : "Failed",
		DPTUN_BW_ARG(group_allocated_bw(to_tunnel_group(tunnel))),
		DPTUN_BW_ARG(to_tunnel_group(tunnel)->available_bw));

	return req_succeeded;
}

/**
 * intel_dp_tunnel_enable_bw_alloc: Enable DP tunnel BW allocation mode
 * @tunnel: Tunnel object
 *
 * Enable the DP tunnel BW allocation mode on @tunnel if it supports it.
 *
 * Returns true if the tunnel supports the BW allocation mode and it was
 * successfully enabled, false otherwise.
 */
bool intel_dp_tunnel_enable_bw_alloc(struct intel_dp_tunnel *tunnel)
{
	struct intel_dp_tunnel_info info;
	int max_tunnel_bw;

	if (!tunnel->bw_alloc_supported)
		return false;

	if (!set_bw_alloc_mode(tunnel, true))
		return false;

	if (!read_and_check_tunnel_info(tunnel, &info)) {
		set_bw_alloc_mode(tunnel, false);
		return false;
	}

	update_tunnel_state(tunnel, &info);

	max_tunnel_bw = get_max_tunnel_bw(tunnel, &info);
	if (!intel_dp_tunnel_allocate_bw(tunnel, max_tunnel_bw)) {
		set_bw_alloc_mode(tunnel, false);
		return false;
	}

	tun_dbg(tunnel,
		"BW allocation mode enabled: DPRX:%dx%d Alloc tunnel:%d/%d Group:%d/%d Mb/s\n",
		DPTUN_BW_ARG(tunnel->max_dprx_rate), tunnel->max_dprx_lane_count,
		DPTUN_BW_ARG(tunnel->allocated_bw),
		DPTUN_BW_ARG(max_tunnel_bw),
		DPTUN_BW_ARG(group_allocated_bw(to_tunnel_group(tunnel))),
		DPTUN_BW_ARG(to_tunnel_group(tunnel)->available_bw));

	return true;
}

/**
 * intel_dp_tunnel_disable_bw_alloc: Disable DP tunnel BW allocation mode
 * @tunnel: Tunnel object
 *
 * Disable the DP tunnel BW allocation mode on @tunnel.
 */
void intel_dp_tunnel_disable_bw_alloc(struct intel_dp_tunnel *tunnel)
{
	if (!tunnel->bw_alloc_enabled)
		return;

	(void)set_bw_alloc_mode(tunnel, false);
	tunnel->allocated_bw = 0;
	update_group_available_bw(tunnel, 0);
}

void intel_dp_tunnel_suspend(struct intel_dp_tunnel *tunnel)
{
	if (!tunnel->bw_alloc_enabled)
		return;

	intel_dp_tunnel_disable_bw_alloc(tunnel);
	tunnel->bw_alloc_suspended = 1;
}

/* Return true if BW alloc mode is enabled */
bool intel_dp_tunnel_resume(struct intel_dp_tunnel *tunnel,
			    bool sink_connected)
{
	if (!tunnel->bw_alloc_suspended)
		return tunnel->bw_alloc_enabled;

	tunnel->bw_alloc_suspended = 0;

	/*
	 * Re-enable the BW allocation mode once the sink reconnects, avoiding
	 * setting the BW allocation error flag here (which prevents reenabling
	 * the BW allocation mode).
	 */
	if (!sink_connected)
		return false;

	return intel_dp_tunnel_enable_bw_alloc(tunnel);
}

static bool check_and_clear_status_change(struct intel_dp_tunnel *tunnel, u8 *changed)
{
	u8 mask = DP_BW_ALLOCATION_CAPABILITY_CHANGED | DP_ESTIMATED_BW_CHANGED;

	if (drm_dp_dpcd_readb(tunnel->aux, DP_TUNNELING_STATUS, changed) < 0)
		return false;

	*changed &= mask;

	if (!*changed)
		return true;

	return drm_dp_dpcd_writeb(tunnel->aux, DP_TUNNELING_STATUS, *changed) == 1;
}

/**
 * intel_dp_tunnel_update_state: Update DP tunnel SW state with the HW state
 * @tunnel: Tunnel object
 *
 * Update the SW state of @tunnel with the HW state.
 *
 * Returns %true if there wasn't any change requiring an update or the
 * updating was successful, %false in case of a failure during the HW state
 * readout, or in case of an inconsistent HW state.
 */
bool intel_dp_tunnel_update_state(struct intel_dp_tunnel *tunnel)
{
	struct intel_dp_tunnel_info info;
	u8 changed;

	if (!check_and_clear_status_change(tunnel, &changed))
		return false;

	if (!changed)
		return true;

	if (!read_and_check_tunnel_info(tunnel, &info))
		return false;

	if (!update_tunnel_state(tunnel, &info))
		return true;

	tun_dbg(tunnel,
		"Status changed: DPRX:%dx%d BW-Sup:%c Alloc tunnel:%d/%d Group:%d/%d Mb/s\n",
		DPTUN_BW_ARG(tunnel->max_dprx_rate), tunnel->max_dprx_lane_count,
		tunnel->bw_alloc_supported ? 'Y' : 'N',
		DPTUN_BW_ARG(tunnel->allocated_bw),
		DPTUN_BW_ARG(tunnel_info_estimated_bw(&info)),
		DPTUN_BW_ARG(group_allocated_bw(to_tunnel_group(tunnel))),
		DPTUN_BW_ARG(to_tunnel_group(tunnel)->available_bw));

	return true;
}

/* Return true if reprobe is needed. */
bool intel_dp_tunnel_handle_irq(struct intel_dp_tunnel_mgr *mgr, struct drm_dp_aux *aux)
{
	u8 val;

	if (drm_dp_dpcd_readb(aux, DP_TUNNELING_STATUS, &val) < 0)
		return false;

	if (val & (DP_BW_REQUEST_SUCCEEDED | DP_BW_REQUEST_FAILED))
		wake_up_all(&mgr->bw_req_queue);

	return val & (DP_BW_ALLOCATION_CAPABILITY_CHANGED | DP_ESTIMATED_BW_CHANGED);
}

/**
 * intel_dp_tunnel_has_bw_alloc_errors - Query for DP tunnel BW allocation errors
 * @tunnel: Tunnel object
 *
 * The function is used to query if there was any BW allocation error on
 * @tunnel. The error state can be only cleared by destroying and re-detecting
 * the tunnel.
 *
 * Returns true if any BW allocation error occured on @tunnel.
 */
bool intel_dp_tunnel_has_bw_alloc_errors(struct intel_dp_tunnel *tunnel)
{
	return tunnel->bw_alloc_has_errors;
}

/**
 * intel_dp_tunnel_max_dprx_rate - Query the maximum rate of the tunnel's DPRX
 * @tunnel: Tunnel object
 *
 * The function is used to query the maximum link rate of the DPRX connected
 * to @tunnel. Note that this rate will not be limited by the BW limit of the
 * tunnel, as opposed to the standard and extended DP_MAX_LINK_RATE DPCD
 * registers.
 *
 * Returns the maximum link rate in 10 kbit/s units.
 */
int intel_dp_tunnel_max_dprx_rate(struct intel_dp_tunnel *tunnel)
{
	return tunnel->max_dprx_rate;
}

/**
 * intel_dp_tunnel_max_dprx_lane_count - Query the maximum lane count of the tunnel's DPRX
 * @tunnel: Tunnel object
 *
 * The function is used to query the maximum lane count of the DPRX connected
 * to @tunnel. Note that this lane count will not be limited by the BW limit of
 * the tunnel, as opposed to the standard and extended DP_MAX_LANE_COUNT DPCD
 * registers.
 *
 * Returns the maximum lane count.
 */
int intel_dp_tunnel_max_dprx_lane_count(struct intel_dp_tunnel *tunnel)
{
	return tunnel->max_dprx_lane_count;
}

/**
 * intel_dp_tunnel_available_bw - Query the estimated total available BW of the tunnel
 * @tunnel: Tunnel object
 *
 * This function is used to query the estimated total available BW of the
 * tunnel. This includes the currently allocated and free BW for all the
 * tunnels in @tunnel's group.
 *
 * Returns the @tunnel group's estimated total available bandwidth in 10 kbit/s
 * units.
 */
int intel_dp_tunnel_available_bw(struct intel_dp_tunnel *tunnel)
{
	return to_tunnel_group(tunnel)->available_bw;
}

static struct drm_private_state *
tunnel_group_duplicate_state(struct drm_private_obj *obj)
{
	struct intel_dp_tunnel_group_state *group_state;

	group_state = kzalloc(sizeof(*group_state), GFP_KERNEL);
	if (!group_state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &group_state->base);

	memcpy(group_state->tunnel_states,
	       to_group_state(obj->state)->tunnel_states,
	       sizeof(group_state->tunnel_states));

	return &group_state->base;
}

static void tunnel_group_destroy_state(struct drm_private_obj *obj, struct drm_private_state *state)
{
	kfree(to_group_state(state));
}

static const struct drm_private_state_funcs tunnel_group_funcs = {
	.atomic_duplicate_state = tunnel_group_duplicate_state,
	.atomic_destroy_state = tunnel_group_destroy_state,
};

static bool init_group(struct intel_dp_tunnel_mgr *mgr, struct intel_dp_tunnel_group *group)
{
	struct intel_dp_tunnel_group_state *group_state = kzalloc(sizeof(*group_state), GFP_KERNEL);

	if (!group_state)
		return false;

	group->mgr = mgr;

	drm_atomic_private_obj_init(&mgr->i915->drm, &group->base, &group_state->base, &tunnel_group_funcs);

	return true;
}

static void cleanup_group(struct intel_dp_tunnel_group *group)
{
	drm_atomic_private_obj_fini(&group->base);
}

static struct intel_dp_tunnel_mgr *
create_mgr(struct drm_i915_private *i915, int group_count)
{
	struct intel_dp_tunnel_mgr *mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
	int i;

	if (!mgr)
		return NULL;

	mgr->i915 = i915;
	mgr->group_count = group_count;
	init_waitqueue_head(&mgr->bw_req_queue);

	mgr->groups = kcalloc(group_count, sizeof(*mgr->groups), GFP_KERNEL);
	if (!mgr->groups) {
		kfree(mgr);
		return NULL;
	}

	for (i = 0; i < group_count; i++) {
		if (init_group(mgr, &mgr->groups[i]))
			continue;

		kfree(mgr->groups);
		kfree(mgr);

		return NULL;
	}

	return mgr;
}

static void destroy_mgr(struct intel_dp_tunnel_mgr *mgr)
{
	int i;

	for (i = 0; i < mgr->group_count; i++)
		cleanup_group(&mgr->groups[i]);

	kfree(mgr->groups);
	kfree(mgr);
}

/**
 * intel_dp_tunnel_mgr_create - Create a DP tunnel manager
 * @i915: i915 driver object
 *
 * Creates a DP tunnel manager.
 *
 * Returns a pointer to the tunnel manager if created successfully or NULL in
 * case of an error.
 */
struct intel_dp_tunnel_mgr *
intel_dp_tunnel_mgr_create(struct drm_i915_private *i915)
{
	struct drm_connector_list_iter connector_list_iter;
	struct intel_connector *connector;
	int dp_connectors = 0;

	drm_connector_list_iter_begin(&i915->drm, &connector_list_iter);
	for_each_intel_connector_iter(connector, &connector_list_iter) {
		if (connector->base.connector_type != DRM_MODE_CONNECTOR_DisplayPort)
			continue;

		dp_connectors++;
	}
	drm_connector_list_iter_end(&connector_list_iter);

	return create_mgr(i915, dp_connectors);
}

/**
 * intel_dp_tunnel_mgr_destroy - Destroy DP tunnel manager
 * @mgr: Tunnel manager object
 *
 * Destroy the tunnel manager.
 */
void intel_dp_tunnel_mgr_destroy(struct intel_dp_tunnel_mgr *mgr)
{
	destroy_mgr(mgr);
}

/**
 * intel_dp_tunnel_atomic_add_state - Add all atomic state for a tunnel group
 * @state: Atomic state
 * @tunnel: Tunnel object
 *
 * Add the atomic state of all tunnels in the group of @tunnel and the the
 * connectors using these tunnels.
 *
 * Returns 0 if the state was added successfully, a negative error code
 * otherwise.
 */
int intel_dp_tunnel_atomic_add_state(struct intel_atomic_state *state,
				     struct intel_dp_tunnel *tunnel)
{
	struct intel_connector *connector;
	struct drm_private_state *_group_state;
	struct drm_connector_list_iter connector_list_iter;
	int ret = 0;

	if (!tunnel)
		return 0;

	_group_state = drm_atomic_get_private_obj_state(&state->base,
							&to_tunnel_group(tunnel)->base);
	if (IS_ERR(_group_state))
		return PTR_ERR(_group_state);

	/* Add state for all connectors in the tunnel's group. */
	drm_connector_list_iter_begin(state->base.dev, &connector_list_iter);
	for_each_intel_connector_iter(connector, &connector_list_iter) {
		struct intel_dp_tunnel *connector_tunnel = NULL;
		struct drm_connector_state *_conn_state;

		if (connector->get_dp_tunnel)
			connector_tunnel = connector->get_dp_tunnel(connector);

		if (!connector_tunnel)
			continue;

		if (to_tunnel_group(connector_tunnel) != to_tunnel_group(tunnel))
			continue;

		tun_dbg_atomic(connector_tunnel,
			       "Adding DP tunnel group state for [CONNECTOR:%d:%s]\n",
			       connector->base.base.id, connector->base.name);

		_conn_state = drm_atomic_get_connector_state(&state->base, &connector->base);
		if (IS_ERR(_conn_state)) {
			ret = PTR_ERR(_conn_state);
			break;
		}
	}
	drm_connector_list_iter_end(&connector_list_iter);

	return ret;
}

static int get_required_tunnel_bw(struct intel_atomic_state *state,
				  struct intel_dp_tunnel *tunnel)
{
	struct intel_connector *connector;
	struct intel_digital_connector_state *conn_state;
	int required_rate = 0;
	int i;

	for_each_new_intel_connector_in_state(state, connector, conn_state, i) {
		int connector_rate;

		(void)conn_state; /* suppress variable not-used warn */

		if (!connector->get_dp_tunnel)
			continue;

		if (connector->get_dp_tunnel(connector) != tunnel)
			continue;

		connector_rate = connector->get_dp_link_rate(state, connector);
		/* Convert kByte/s -> 10 kbit/s */
		connector_rate = connector_rate * 8 / 10;
		tun_dbg(tunnel,
			"Required %d Mb/s for [CONNECTOR:%d:%s]\n",
			DPTUN_BW_ARG(connector_rate),
			connector->base.base.id, connector->base.name);

		required_rate += connector_rate;
	}

	return required_rate;
}

static u8 get_tunnel_pipe_mask(struct intel_atomic_state *state,
			       struct intel_dp_tunnel *tunnel)
{
	struct intel_connector *connector;
	struct intel_digital_connector_state *conn_state;

	u8 mask = 0;
	int i;

	for_each_new_intel_connector_in_state(state, connector, conn_state, i) {
		struct intel_crtc *crtc;

		(void)conn_state; /* suppress variable not-used warn */

		if (!connector->get_dp_tunnel)
			continue;

		if (connector->get_dp_tunnel(connector) != tunnel)
			continue;

		if (!conn_state->base.crtc)
			continue;

		crtc = to_intel_crtc(conn_state->base.crtc);
		mask |= BIT(crtc->pipe);
	}

	return mask;
}

static int intel_dp_tunnel_check_group_bw(struct intel_atomic_state *state,
					  struct intel_link_bw_limits *limits,
					  struct intel_dp_tunnel_group *group,
					  struct intel_dp_tunnel_group_state *new_group_state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_dp_tunnel *tunnel;
	struct intel_dp_tunnel_state *new_tunnel_state;
	int group_required_bw = 0;
	u8 group_pipes = 0;
	int ret;

	for_each_bw_alloc_tunnel_in_group(group, new_group_state, tunnel, new_tunnel_state) {
		int max_dprx_bw = get_max_dprx_bw(tunnel);
		u8 tunnel_pipes;

		new_tunnel_state->required_bw = roundup(get_required_tunnel_bw(state, tunnel),
							tunnel->bw_granularity);

		tun_dbg(tunnel,
			"%sRequired %d/%d Mb/s total for tunnel.\n",
			new_tunnel_state->required_bw > max_dprx_bw ? "Not enough BW: " : "",
			DPTUN_BW_ARG(new_tunnel_state->required_bw),
			DPTUN_BW_ARG(max_dprx_bw));

		group_required_bw += new_tunnel_state->required_bw;

		tunnel_pipes = get_tunnel_pipe_mask(state, tunnel);

		if (new_tunnel_state->required_bw > max_dprx_bw) {
			ret = intel_link_bw_reduce_bpp(state, limits,
						       tunnel_pipes, "DP tunnel link BW");

			return ret ? : -EAGAIN;
		}

		group_pipes |= tunnel_pipes;
	}

	tun_grp_dbg(group,
		    "%sRequired %d/%d Mb/s total for tunnel group.\n",
		    group_required_bw > group->available_bw ? "Not enough BW: " : "",
		    DPTUN_BW_ARG(group_required_bw),
		    DPTUN_BW_ARG(group->available_bw));

	if (group_required_bw <= group->available_bw)
		return 0;

	drm_dbg_kms(&i915->drm, "group pipes: %02x\n", group_pipes);

	ret = intel_link_bw_reduce_bpp(state, limits,
				       group_pipes, "DP tunnel group link BW");

	return ret ? : -EAGAIN;
}

/**
 * intel_dp_tunnel_atomic_check_link - Check the DP tunnel atomic state
 * @state: intel atomic state
 * @limits: link BW limits
 *
 * Check the link configuration for all DP tunnels in @state. If the
 * configuration is invalid @limits will be updated if possible to
 * reduce the total BW, after which the configuration for all CRTCs in
 * @state must be recomputed with the updated @limits.
 *
 * Returns:
 *   - 0 if the confugration is valid
 *   - %-EAGAIN, if the configuration is invalid and @limits got updated
 *     with fallback values with which the configuration of all CRTCs in
 *     @state must be recomputed
 *   - Other negative error, if the configuration is invalid without a
 *     fallback possibility, or the check failed for another reason
 */
int intel_dp_tunnel_atomic_check_link(struct intel_atomic_state *state,
				      struct intel_link_bw_limits *limits)
{
	struct intel_dp_tunnel_group *group;
	struct intel_dp_tunnel_group_state *new_group_state;
	int i;

	for_each_new_group_in_state(state, group, new_group_state, i) {
		int ret;

		ret = intel_dp_tunnel_check_group_bw(state, limits,
						     group, new_group_state);
		if (ret)
			return ret;
	}

	return 0;
}

static void restore_tunnel_bw(struct intel_dp_tunnel *tunnel, int bw)
{
	if (intel_dp_tunnel_allocate_bw(tunnel, bw))
		return;

	tun_dbg(tunnel,
		"Can't restore original %d Mb/s, disabling tunnel BW allocation mode.\n",
		DPTUN_BW_ARG(bw));
}

/**
 * intel_dp_tunnel_atomic_cancel_reservations - Cancel all BW reservations in an atomic state
 * @state: Atomic state
 *
 * Cancel all BW reservations of a previous successful call to
 * intel_dp_tunnel_atomic_reserve().
 */
void intel_dp_tunnel_atomic_cancel_reservations(struct intel_atomic_state *state)
{
	struct intel_dp_tunnel_group *group;
	struct intel_dp_tunnel_group_state *old_group_state;
	int i;

	for_each_old_group_in_state(state, group, old_group_state, i) {
		struct intel_dp_tunnel *tunnel;
		struct intel_dp_tunnel_state *old_tunnel_state;

		for_each_bw_alloc_tunnel_in_group(group, old_group_state, tunnel, old_tunnel_state) {
			if (old_tunnel_state->required_bw >= tunnel->allocated_bw)
				continue;

			restore_tunnel_bw(tunnel, old_tunnel_state->required_bw);
		}

		for_each_bw_alloc_tunnel_in_group(group, old_group_state, tunnel, old_tunnel_state) {
			if (old_tunnel_state->required_bw <= tunnel->allocated_bw)
				continue;

			restore_tunnel_bw(tunnel, old_tunnel_state->required_bw);
		}
	}
}

static bool reserve_tunnel_bw(struct intel_dp_tunnel *tunnel, int bw)
{
	if (intel_dp_tunnel_allocate_bw(tunnel, bw))
		return true;

	tun_dbg(tunnel,
		"Can't reserve %d Mb/s\n",
		DPTUN_BW_ARG(bw));

	return false;
}

/**
 * intel_dp_tunnel_atomic_reserve - Reserve required BW for all DP tunnels
 * @state: Atomic state
 *
 * Reserve the required BW for all DP tunnels in @state. The BW must be
 * commited by a subsequent call to intel_dp_tunnel_atomic_commit().
 *
 * Returns 0 if the BW was reserved successfully, or a negative error code
 * otherwise.
 */
int intel_dp_tunnel_atomic_reserve(struct intel_atomic_state *state)
{
	struct intel_dp_tunnel_group *group;
	struct intel_dp_tunnel_group_state *new_group_state;
	int i;

	for_each_new_group_in_state(state, group, new_group_state, i) {
		struct intel_dp_tunnel *tunnel;
		struct intel_dp_tunnel_state *new_tunnel_state;
		int free_bw = group->available_bw - group_allocated_bw(group);

		for_each_bw_alloc_tunnel_in_group(group, new_group_state, tunnel, new_tunnel_state) {
			int reserved_bw;

			if (new_tunnel_state->required_bw <= tunnel->allocated_bw)
				continue;

			reserved_bw = min(new_tunnel_state->required_bw,
					  tunnel->allocated_bw + free_bw);
			free_bw -= reserved_bw - tunnel->allocated_bw;

			if (reserved_bw == tunnel->allocated_bw)
				continue;

			if (!reserve_tunnel_bw(tunnel, reserved_bw)) {
				intel_dp_tunnel_atomic_cancel_reservations(state);

				return -EINVAL;
			}
		}
	}

	return 0;
}

static void commit_tunnel_bw(struct intel_dp_tunnel *tunnel, int bw)
{
	if (intel_dp_tunnel_allocate_bw(tunnel, bw))
		return;

	tun_err(tunnel,
		"Can't commit %d Mb/s, disabling tunnel BW allocation mode.\n",
		DPTUN_BW_ARG(bw));

	set_bw_alloc_error(tunnel);
}

/**
 * intel_dp_tunnel_atomic_commit - Commit required BW for all DP tunnels
 * @state: Atomic state
 *
 * Commit the required BW for all DP tunnels in @state. The BW must have
 * been reserved by a successful preceding call to
 * intel_dp_tunnel_atomic_reserve().
 *
 * A BW allocation failures will be recorded, which can be queried by a
 * subsequent call to intel_dp_has_bw_alloc_errors().
 */
void intel_dp_tunnel_atomic_commit(struct intel_atomic_state *state)
{
	struct intel_dp_tunnel_group *group;
	struct intel_dp_tunnel_group_state *new_group_state;
	int i;

	for_each_new_group_in_state(state, group, new_group_state, i) {
		struct intel_dp_tunnel *tunnel;
		struct intel_dp_tunnel_state *new_tunnel_state;

		for_each_bw_alloc_tunnel_in_group(group, new_group_state, tunnel, new_tunnel_state) {
			if (new_tunnel_state->required_bw >= tunnel->allocated_bw)
				continue;

			commit_tunnel_bw(tunnel, new_tunnel_state->required_bw);
		}

		for_each_bw_alloc_tunnel_in_group(group, new_group_state, tunnel, new_tunnel_state) {
			if (new_tunnel_state->required_bw <= tunnel->allocated_bw)
				continue;

			commit_tunnel_bw(tunnel, new_tunnel_state->required_bw);
		}
	}
}
