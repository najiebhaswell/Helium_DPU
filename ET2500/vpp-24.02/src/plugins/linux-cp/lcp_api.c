/*
 * Copyright 2020 Rubicon Communications, LLC.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/socket.h>
#include <linux/if.h>

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>
#include <vpp/app/version.h>
#include <vnet/format_fns.h>

#include <linux-cp/lcp_interface.h>
#include <linux-cp/lcp.api_enum.h>
#include <linux-cp/lcp.api_types.h>

extern u16 *bpdu_drop;




static u16 lcp_msg_id_base;
#define REPLY_MSG_ID_BASE lcp_msg_id_base
#include <vlibapi/api_helper_macros.h>

static lip_host_type_t
api_decode_host_type (vl_api_lcp_itf_host_type_t type)
{
  if (type == LCP_API_ITF_HOST_TUN)
    return LCP_ITF_HOST_TUN;

  return LCP_ITF_HOST_TAP;
}

static vl_api_lcp_itf_host_type_t
api_encode_host_type (lip_host_type_t type)
{
  if (type == LCP_ITF_HOST_TUN)
    return LCP_API_ITF_HOST_TUN;

  return LCP_API_ITF_HOST_TAP;
}

static int
vl_api_lcp_itf_pair_add (u32 phy_sw_if_index, lip_host_type_t lip_host_type,
			 u8 *mp_host_if_name, size_t sizeof_host_if_name,
			 u8 *mp_namespace, size_t sizeof_mp_namespace,
			 u32 *host_sw_if_index_p, u32 *vif_index_p)
{
  u8 *host_if_name, *netns;
  int host_len, netns_len, rv;

  host_if_name = netns = 0;

  /* lcp_itf_pair_create expects vec of u8 */
  host_len = clib_strnlen ((char *) mp_host_if_name, sizeof_host_if_name - 1);
  vec_add (host_if_name, mp_host_if_name, host_len);
  vec_add1 (host_if_name, 0);

  netns_len = clib_strnlen ((char *) mp_namespace, sizeof_mp_namespace - 1);
  vec_add (netns, mp_namespace, netns_len);
  vec_add1 (netns, 0);

  rv = lcp_itf_pair_create (phy_sw_if_index, host_if_name, lip_host_type,
			    netns, host_sw_if_index_p);

  if (!rv && (vif_index_p != NULL))
    {
      lcp_itf_pair_t *pair =
	lcp_itf_pair_get (lcp_itf_pair_find_by_phy (phy_sw_if_index));
      *vif_index_p = pair->lip_vif_index;
    }

  vec_free (host_if_name);
  vec_free (netns);

  return rv;
}

static void
vl_api_lcp_itf_pair_add_del_t_handler (vl_api_lcp_itf_pair_add_del_t *mp)
{
  u32 phy_sw_if_index;
  vl_api_lcp_itf_pair_add_del_reply_t *rmp;
  lip_host_type_t lip_host_type;
  int rv;

  VALIDATE_SW_IF_INDEX_END (mp);

  phy_sw_if_index = mp->sw_if_index;
  lip_host_type = api_decode_host_type (mp->host_if_type);
  if (mp->is_add)
    {
      rv = vl_api_lcp_itf_pair_add (
	phy_sw_if_index, lip_host_type, mp->host_if_name,
	sizeof (mp->host_if_name), mp->netns, sizeof (mp->netns), NULL, NULL);
    }
  else
    {
      rv = lcp_itf_pair_delete (phy_sw_if_index);
    }

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO_END (VL_API_LCP_ITF_PAIR_ADD_DEL_REPLY);
}

static void
vl_api_lcp_itf_pair_add_del_v2_t_handler (vl_api_lcp_itf_pair_add_del_v2_t *mp)
{
  u32 phy_sw_if_index, host_sw_if_index = ~0;
  vl_api_lcp_itf_pair_add_del_v2_reply_t *rmp;
  lip_host_type_t lip_host_type;
  int rv;

  VALIDATE_SW_IF_INDEX_END (mp);

  phy_sw_if_index = mp->sw_if_index;
  lip_host_type = api_decode_host_type (mp->host_if_type);
  if (mp->is_add)
    {
      rv = vl_api_lcp_itf_pair_add (
	phy_sw_if_index, lip_host_type, mp->host_if_name,
	sizeof (mp->host_if_name), mp->netns, sizeof (mp->netns),
	&host_sw_if_index, NULL);
    }
  else
    {
      rv = lcp_itf_pair_delete (phy_sw_if_index);
    }

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO2_END (VL_API_LCP_ITF_PAIR_ADD_DEL_V2_REPLY,
		    { rmp->host_sw_if_index = host_sw_if_index; });
}

static void
vl_api_lcp_itf_pair_add_del_v3_t_handler (vl_api_lcp_itf_pair_add_del_v3_t *mp)
{
  u32 phy_sw_if_index, host_sw_if_index = ~0, vif_index = ~0;
  vl_api_lcp_itf_pair_add_del_v3_reply_t *rmp;
  lip_host_type_t lip_host_type;
  int rv;

  VALIDATE_SW_IF_INDEX_END (mp);

  phy_sw_if_index = mp->sw_if_index;
  lip_host_type = api_decode_host_type (mp->host_if_type);
  if (mp->is_add)
    {
      rv = vl_api_lcp_itf_pair_add (
	phy_sw_if_index, lip_host_type, mp->host_if_name,
	sizeof (mp->host_if_name), mp->netns, sizeof (mp->netns),
	&host_sw_if_index, &vif_index);
    }
  else
    {
      rv = lcp_itf_pair_delete (phy_sw_if_index);
    }

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO2_END (VL_API_LCP_ITF_PAIR_ADD_DEL_V3_REPLY, ({
		      rmp->host_sw_if_index = host_sw_if_index;
		      rmp->vif_index = vif_index;
		    }));
}

static void
send_lcp_itf_pair_details (index_t lipi, vl_api_registration_t *rp,
			   u32 context)
{
  vl_api_lcp_itf_pair_details_t *rmp;
  lcp_itf_pair_t *lcp_pair = lcp_itf_pair_get (lipi);

  REPLY_MACRO_DETAILS4_END (
    VL_API_LCP_ITF_PAIR_DETAILS, rp, context, ({
      rmp->phy_sw_if_index = lcp_pair->lip_phy_sw_if_index;
      rmp->host_sw_if_index = lcp_pair->lip_host_sw_if_index;
      rmp->vif_index = lcp_pair->lip_vif_index;
      rmp->host_if_type = api_encode_host_type (lcp_pair->lip_host_type);

      memcpy_s (rmp->host_if_name, sizeof (rmp->host_if_name),
		lcp_pair->lip_host_name, vec_len (lcp_pair->lip_host_name));
      rmp->host_if_name[vec_len (lcp_pair->lip_host_name)] = 0;

      memcpy_s (rmp->netns, sizeof (rmp->netns), lcp_pair->lip_namespace,
		vec_len (lcp_pair->lip_namespace));
      rmp->netns[vec_len (lcp_pair->lip_namespace)] = 0;
    }));
}

static void
vl_api_lcp_itf_pair_get_t_handler (vl_api_lcp_itf_pair_get_t *mp)
{
  vl_api_lcp_itf_pair_get_reply_t *rmp;
  i32 rv = 0;

  REPLY_AND_DETAILS_MACRO_END (
    VL_API_LCP_ITF_PAIR_GET_REPLY, lcp_itf_pair_pool,
    ({ send_lcp_itf_pair_details (cursor, rp, mp->context); }));
}

static void
vl_api_lcp_itf_pair_get_v2_t_handler (vl_api_lcp_itf_pair_get_v2_t *mp)
{
  vl_api_lcp_itf_pair_get_v2_reply_t *rmp;
  i32 rv = 0;

  if (mp->sw_if_index == ~0)
    {
      REPLY_AND_DETAILS_MACRO_END (
	VL_API_LCP_ITF_PAIR_GET_REPLY, lcp_itf_pair_pool,
	({ send_lcp_itf_pair_details (cursor, rp, mp->context); }));
    }
  else
    {
      VALIDATE_SW_IF_INDEX_END (mp);
      send_lcp_itf_pair_details (
	lcp_itf_pair_find_by_phy (mp->sw_if_index),
	vl_api_client_index_to_registration (mp->client_index), mp->context);

      BAD_SW_IF_INDEX_LABEL;
      REPLY_MACRO2_END (VL_API_LCP_ITF_PAIR_GET_V2_REPLY,
			({ rmp->cursor = ~0; }));
    }
}

static void
vl_api_lcp_default_ns_set_t_handler (vl_api_lcp_default_ns_set_t *mp)
{
  vl_api_lcp_default_ns_set_reply_t *rmp;
  int rv;

  mp->netns[LCP_NS_LEN - 1] = 0;
  rv = lcp_set_default_ns (mp->netns);

  REPLY_MACRO (VL_API_LCP_DEFAULT_NS_SET_REPLY);
}

static void
vl_api_lcp_default_ns_get_t_handler (vl_api_lcp_default_ns_get_t *mp)
{
  vl_api_lcp_default_ns_get_reply_t *rmp;

  REPLY_MACRO_DETAILS2 (VL_API_LCP_DEFAULT_NS_GET_REPLY, ({
			  char *ns = (char *) lcp_get_default_ns ();
			  if (ns)
			    clib_strncpy ((char *) rmp->netns, ns,
					  LCP_NS_LEN - 1);
			}));
}

static void
vl_api_lcp_itf_pair_replace_begin_t_handler (
  vl_api_lcp_itf_pair_replace_begin_t *mp)
{
  vl_api_lcp_itf_pair_replace_begin_reply_t *rmp;
  int rv;

  rv = lcp_itf_pair_replace_begin ();

  REPLY_MACRO (VL_API_LCP_ITF_PAIR_REPLACE_BEGIN_REPLY);
}

static void
vl_api_lcp_itf_pair_replace_end_t_handler (
  vl_api_lcp_itf_pair_replace_end_t *mp)
{
  vl_api_lcp_itf_pair_replace_end_reply_t *rmp;
  int rv = 0;

  rv = lcp_itf_pair_replace_end ();

  REPLY_MACRO (VL_API_LCP_ITF_PAIR_REPLACE_END_REPLY);
}

static void
vl_api_lcp_set_interface_punt_feature_t_handler (
  vl_api_lcp_set_interface_punt_feature_t *mp)
{
  vl_api_lcp_set_interface_punt_feature_reply_t *rmp;
  int rv = 0;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  u8 punt_on = mp->punt_on ? 1 : 0;

  VALIDATE_SW_IF_INDEX (mp);

  /* ospf punt for phy interfaces */
  vnet_feature_enable_disable("ip4-multicast", "linux-cp-ospfv2-phy",
          sw_if_index, punt_on, NULL, 0);
  vnet_feature_enable_disable("ip6-multicast", "linux-cp-ospfv3-phy",
          sw_if_index, punt_on, NULL, 0);

  /* dhcp/dhcpv6 punt for interfaces */
  vnet_feature_enable_disable("ip4-unicast", "linux-cp-dhcp-phy",
          sw_if_index, punt_on, NULL, 0);
  vnet_feature_enable_disable("ip4-multicast", "linux-cp-dhcp-phy",
          sw_if_index, punt_on, NULL, 0);
  vnet_feature_enable_disable("ip6-unicast", "linux-cp-dhcpv6-phy",
          sw_if_index, punt_on, NULL, 0);
  vnet_feature_enable_disable("ip6-multicast", "linux-cp-dhcpv6-phy",
          sw_if_index, punt_on, NULL, 0);

  /* icmpv6-ndp nd punt for interfaces */
  vnet_feature_enable_disable("ip6-unicast", "linux-cp-ndp-phy",
          sw_if_index, punt_on, NULL, 0);
  vnet_feature_enable_disable("ip6-multicast", "linux-cp-ndp-phy",
          sw_if_index, punt_on, NULL, 0);
  vnet_feature_enable_disable ("arp", "linux-cp-arp-phy",
		  sw_if_index, punt_on, NULL, 0);

  vnet_feature_enable_disable("ip4-multicast", "linux-cp-vrrp",
          sw_if_index, punt_on, NULL, 0);
  vnet_feature_enable_disable("ip6-multicast", "linux-cp-vrrp6",
          sw_if_index, punt_on, NULL, 0);

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_LCP_SET_INTERFACE_PUNT_FEATURE_REPLY);
}



static void
vl_api_lcp_set_interface_bpdu_drop_t_handler (
  vl_api_lcp_set_interface_bpdu_drop_t *mp)
{
  vl_api_lcp_set_interface_bpdu_drop_reply_t *rmp;
  int rv = 0;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  u8 is_drop = mp->is_drop ? 1 : 0;

  VALIDATE_SW_IF_INDEX (mp);

  vec_validate(bpdu_drop, sw_if_index);

  bpdu_drop[sw_if_index] = is_drop;

  
  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_LCP_SET_INTERFACE_BPDU_DROP_REPLY);
}

/*
 * Set up the API message handling tables
 */
#include <linux-cp/lcp.api.c>

static clib_error_t *
lcp_api_init (vlib_main_t *vm)
{
  /* Ask for a correctly-sized block of API message decode slots */
  lcp_msg_id_base = setup_message_id_table ();

  return (NULL);
}

VLIB_INIT_FUNCTION (lcp_api_init);

#include <vpp/app/version.h>
VLIB_PLUGIN_REGISTER () = {
  .version = VPP_BUILD_VER,
  .description = "Linux Control Plane - Interface Mirror",
  .default_disabled = 1,
};

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */

