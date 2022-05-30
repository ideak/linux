#ifndef __INTEL_DP_TUNNEL_H__
#define __INTEL_DP_TUNNEL_H__

#include <linux/types.h>

struct drm_dp_aux;

struct drm_i915_private;

struct intel_atomic_state;
struct intel_dp_tunnel;
struct intel_dp_tunnel_mgr;
struct intel_link_bw_limits;

struct intel_dp_tunnel *
intel_dp_tunnel_detect(struct intel_dp_tunnel_mgr *mgr, struct drm_dp_aux *aux);
void intel_dp_tunnel_destroy(struct intel_dp_tunnel *tunnel);
bool intel_dp_tunnel_update_state(struct intel_dp_tunnel *tunnel);

bool intel_dp_tunnel_enable_bw_alloc(struct intel_dp_tunnel *tunnel);
void intel_dp_tunnel_disable_bw_alloc(struct intel_dp_tunnel *tunnel);

void intel_dp_tunnel_suspend(struct intel_dp_tunnel *tunnel);
bool intel_dp_tunnel_resume(struct intel_dp_tunnel *tunnel,
			    bool sink_connected);

bool intel_dp_tunnel_has_bw_alloc_errors(struct intel_dp_tunnel *tunnel);

bool intel_dp_tunnel_handle_irq(struct intel_dp_tunnel_mgr *mgr, struct drm_dp_aux *aux);

int intel_dp_tunnel_max_dprx_rate(struct intel_dp_tunnel *tunnel);
int intel_dp_tunnel_max_dprx_lane_count(struct intel_dp_tunnel *tunnel);
int intel_dp_tunnel_available_bw(struct intel_dp_tunnel *tunnel);

struct intel_dp_tunnel_mgr *
intel_dp_tunnel_mgr_create(struct drm_i915_private *i915);
void intel_dp_tunnel_mgr_destroy(struct intel_dp_tunnel_mgr *mgr);

int intel_dp_tunnel_atomic_add_state(struct intel_atomic_state *state,
				     struct intel_dp_tunnel *tunnel);
int intel_dp_tunnel_atomic_check_link(struct intel_atomic_state *state,
				      struct intel_link_bw_limits *limits);
int intel_dp_tunnel_atomic_reserve(struct intel_atomic_state *state);
void intel_dp_tunnel_atomic_cancel_reservations(struct intel_atomic_state *state);
void intel_dp_tunnel_atomic_commit(struct intel_atomic_state *state);

#endif
