/*
 * Copyright (c) 2001-2017 Mellanox Technologies, Ltd. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#include <infiniband/verbs.h>

#include "utils/bullseye.h"
#include "vlogger/vlogger.h"
#include <vma/util/verbs_extra.h>
#include <vma/util/sys_vars.h>
#include "vma/dev/ib_ctx_handler.h"
#include "vma/dev/time_converter_ib_ctx.h"
#include "vma/dev/time_converter_ptp.h"
#include "vma/util/verbs_extra.h"
#include "util/valgrind.h"
#include "vma/event/event_handler_manager.h"

#define MODULE_NAME             "ib_ctx_handler"

#define ibch_logpanic           __log_panic
#define ibch_logerr             __log_err
#define ibch_logwarn            __log_warn
#define ibch_loginfo            __log_info
#define ibch_logdbg             __log_info_dbg
#define ibch_logfunc            __log_info_func
#define ibch_logfuncall         __log_info_funcall


ib_ctx_handler::ib_ctx_handler(struct ibv_context* ctx, ts_conversion_mode_t ctx_time_converter_mode) :
	m_flow_tag_enabled(false)
	, m_removed(false)
	, m_conf_attr_rx_num_wre(0)
	, m_conf_attr_tx_num_to_signal(0)
	, m_conf_attr_tx_max_inline(0)
	, m_conf_attr_tx_num_wre(0)
	, m_p_ctx_time_converter(NULL)
{
	memset(&m_ibv_port_attr, 0, sizeof(m_ibv_port_attr));
	m_p_ibv_context = ctx;
	VALGRIND_MAKE_MEM_DEFINED(m_p_ibv_context, sizeof(ibv_context));
	m_p_ibv_device = ctx->device;

	BULLSEYE_EXCLUDE_BLOCK_START
	if (m_p_ibv_device == NULL) {
		ibch_logpanic("ibv_device is NULL! (ibv context %p)", m_p_ibv_context);
	}

#ifdef DEFINED_IBV_EXP_CQ_TIMESTAMP
	if (ctx_time_converter_mode != TS_CONVERSION_MODE_DISABLE) {
		struct ibv_exp_device_attr device_attr;
		memset(&device_attr, 0, sizeof(device_attr));
		device_attr.comp_mask = IBV_EXP_DEVICE_ATTR_WITH_HCA_CORE_CLOCK;
		if (!ibv_exp_query_device(m_p_ibv_context ,&device_attr)) {
			if (ctx_time_converter_mode == TS_CONVERSION_MODE_PTP) {
#ifdef DEFINED_IBV_EXP_VALUES_CLOCK_INFO
				struct ibv_exp_values ibv_exp_values_tmp;
				memset(&ibv_exp_values_tmp, 0, sizeof(ibv_exp_values_tmp));
				int ret = ibv_exp_query_values(m_p_ibv_context, IBV_EXP_VALUES_CLOCK_INFO, &ibv_exp_values_tmp);
				if (!ret) {
					m_p_ctx_time_converter = new time_converter_ptp(ctx);
				} else { // revert to mode TS_CONVERSION_MODE_SYNC
					m_p_ctx_time_converter = new time_converter_ib_ctx(ctx, TS_CONVERSION_MODE_SYNC, device_attr.hca_core_clock);
					ibch_logwarn("ibv_exp_query_values failure for clock_info, reverting to mode TS_CONVERSION_MODE_SYNC (ibv context %p) (return value=%d)",
								m_p_ibv_context, ret);
				}
#else
				m_p_ctx_time_converter = new time_converter_ib_ctx(ctx, TS_CONVERSION_MODE_SYNC, device_attr.hca_core_clock);
				ibch_logwarn("PTP is not supported by the underlying Infiniband verbs. IBV_EXP_VALUES_CLOCK_INFO not defined. reverting to mode TS_CONVERSION_MODE_SYNC");
#endif // DEFINED_IBV_EXP_VALUES_CLOCK_INFO
			} else {
				m_p_ctx_time_converter = new time_converter_ib_ctx(ctx, ctx_time_converter_mode, device_attr.hca_core_clock);
			}
		} else {
			m_p_ctx_time_converter = new time_converter_ib_ctx(ctx, TS_CONVERSION_MODE_DISABLE, 0);
			ibch_logwarn("device does not support hca_core_clock operations, reverting to mode TS_CONVERSION_MODE_DISABLE (ibv context %p) (hca_core_clock=%llu)",
											m_p_ibv_context, device_attr.hca_core_clock);
		}
	} else {
		m_p_ctx_time_converter = new time_converter_ib_ctx(ctx, TS_CONVERSION_MODE_DISABLE, 0);
	}
#else
	m_p_ctx_time_converter = new time_converter_ib_ctx(ctx, TS_CONVERSION_MODE_DISABLE, 0);
	if (ctx_time_converter_mode != TS_CONVERSION_MODE_DISABLE) {
		ibch_logwarn("time converter mode not applicable (configuration value=%d). set to TS_CONVERSION_MODE_DISABLE.",
				ctx_time_converter_mode);
	}
#endif // DEFINED_IBV_EXP_CQ_TIMESTAMP

	if (!m_p_ctx_time_converter) {
		ibch_logerr("Failed to allocate memory for time converter object");
		return;
	}

	// Create pd for this device
	m_p_ibv_pd = ibv_alloc_pd(m_p_ibv_context);
	if (m_p_ibv_pd == NULL) {
		ibch_logpanic("ibv device %p pd allocation failure (ibv context %p) (errno=%d %m)", 
			    m_p_ibv_device, m_p_ibv_context, errno);
	}

	memset(&m_ibv_device_attr, 0, sizeof(m_ibv_device_attr));
	vma_ibv_device_attr_comp_mask(m_ibv_device_attr);
	IF_VERBS_FAILURE(vma_ibv_query_device(m_p_ibv_context, &m_ibv_device_attr)) {
		ibch_logerr("ibv_query_device failed on ibv device %p (ibv context %p) (errno=%d %m)", 
			  m_p_ibv_device, m_p_ibv_context, errno);
		return;
	} ENDIF_VERBS_FAILURE;
	BULLSEYE_EXCLUDE_BLOCK_END

	ibch_logdbg("ibv device '%s' [%p] has %d port%s. Vendor Part Id: %d, FW Ver: %s, max_qp_wr=%d",
			m_p_ibv_device->name, m_p_ibv_device, m_ibv_device_attr.phys_port_cnt, ((m_ibv_device_attr.phys_port_cnt>1)?"s":""),
			m_ibv_device_attr.vendor_part_id, m_ibv_device_attr.fw_ver, m_ibv_device_attr.max_qp_wr);

	set_dev_configuration();

	g_p_event_handler_manager->register_ibverbs_event(m_p_ibv_context->async_fd, this, m_p_ibv_context, 0);
}

ib_ctx_handler::~ib_ctx_handler() {
	g_p_event_handler_manager->unregister_ibverbs_event(m_p_ibv_context->async_fd, this);
	// must delete ib_ctx_handler only after freeing all resources that
	// are still associated with the PD m_p_ibv_pd
	BULLSEYE_EXCLUDE_BLOCK_START
	if (ibv_dealloc_pd(m_p_ibv_pd))
		ibch_logdbg("pd deallocation failure (errno=%d %m)", errno);
	delete m_p_ctx_time_converter;
	BULLSEYE_EXCLUDE_BLOCK_END
}

ts_conversion_mode_t ib_ctx_handler::get_ctx_time_converter_status() {
	return m_p_ctx_time_converter->get_converter_status();
}

ibv_mr* ib_ctx_handler::mem_reg(void *addr, size_t length, uint64_t access)
{
	// Register the memory block with the HCA on this ibv_device
	ibch_logfunc("(dev=%p) addr=%p, length=%d, m_p_ibv_pd=%p on dev=%p", m_p_ibv_device, addr, length, m_p_ibv_pd, m_p_ibv_pd->context->device);
#ifdef DEFINED_IBV_EXP_ACCESS_ALLOCATE_MR
	struct ibv_exp_reg_mr_in in;
	memset(&in, 0 ,sizeof(in));
	in.exp_access = access;
	in.addr = addr;
	in.length = length;
	in.pd = m_p_ibv_pd;
	ibv_mr *mr = ibv_exp_reg_mr(&in);
#else
	ibv_mr *mr = ibv_reg_mr(m_p_ibv_pd, addr, length, access);
#endif
	VALGRIND_MAKE_MEM_DEFINED(mr, sizeof(ibv_mr));
	return mr;
}

void ib_ctx_handler::mem_dereg(ibv_mr *mr)
{
	if (is_removed()) {
		return;
	}
	IF_VERBS_FAILURE(ibv_dereg_mr(mr)) {
		ibch_logerr("failed de-registering a memory region "
				"(errno=%d %m)", errno);
	} ENDIF_VERBS_FAILURE;
	VALGRIND_MAKE_MEM_UNDEFINED(mr, sizeof(ibv_mr));
}

void ib_ctx_handler::set_flow_tag_capability(bool flow_tag_capability)
{
	m_flow_tag_enabled = flow_tag_capability;
}

bool ib_ctx_handler::update_port_attr(int port_num)
{
        IF_VERBS_FAILURE(ibv_query_port(m_p_ibv_context, port_num, &m_ibv_port_attr)) {
                ibch_logdbg("ibv_query_port failed on ibv device %p, port %d (errno=%d)", m_p_ibv_context, port_num, errno);
                return false;
        } ENDIF_VERBS_FAILURE;
        return true;
}

ibv_port_state ib_ctx_handler::get_port_state(int port_num)
{       
        update_port_attr(port_num);
        return m_ibv_port_attr.state;
}

#if _BullseyeCoverage
    #pragma BullseyeCoverage off
#endif

ibv_port_attr ib_ctx_handler::get_ibv_port_attr(int port_num)
{
        update_port_attr(port_num);
        return m_ibv_port_attr;
}

#if _BullseyeCoverage
    #pragma BullseyeCoverage on
#endif

void ib_ctx_handler::set_dev_configuration()
{
	ibch_logdbg("Setting configuration for the MLX card %s", m_p_ibv_device->name);
	m_conf_attr_rx_num_wre                  = safe_mce_sys().rx_num_wr;
	m_conf_attr_tx_max_inline               = safe_mce_sys().tx_max_inline;
	m_conf_attr_tx_num_wre                  = safe_mce_sys().tx_num_wr;
#ifdef DEFINED_VMAPOLL
	m_conf_attr_tx_num_to_signal = NUM_TX_WRE_TO_SIGNAL_MAX;
#else
	m_conf_attr_tx_num_to_signal = 	safe_mce_sys().tx_num_wr_to_signal;
#endif // DEFINED_VMAPOLL

	if (m_conf_attr_tx_num_wre < (m_conf_attr_tx_num_to_signal * 2)) {
		m_conf_attr_tx_num_wre = m_conf_attr_tx_num_to_signal * 2;
		ibch_loginfo("%s Setting the %s to %d according to the device specific configuration:",
			   m_p_ibv_device->name, SYS_VAR_TX_NUM_WRE, safe_mce_sys().tx_num_wr);
	}
}

void ib_ctx_handler::handle_event_ibverbs_cb(void *ev_data, void *ctx)
{
 	NOT_IN_USE(ctx);

	struct ibv_async_event *ibv_event = (struct ibv_async_event*)ev_data;
	ibch_logdbg("received ibv_event '%s' (%d)", priv_ibv_event_desc_str(ibv_event->event_type), ibv_event->event_type);
		
	if (ibv_event->event_type == IBV_EVENT_DEVICE_FATAL) {
		handle_event_DEVICE_FATAL();
	}
}

void ib_ctx_handler::handle_event_DEVICE_FATAL()
{
	m_removed = true;
	g_p_event_handler_manager->unregister_ibverbs_event(m_p_ibv_context->async_fd, this);
}
