/* Interface handler for Sysmocom L1 */

/* (C) 2011 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/select.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/write_queue.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/lapdm.h>

#include <osmo-bts/logging.h>
#include <osmo-bts/bts.h>
#include <osmo-bts/oml.h>
#include <osmo-bts/gsm_data.h>
#include <osmo-bts/paging.h>
#include <osmo-bts/measurement.h>
#include <osmo-bts/pcu_if.h>
#include <osmo-bts/l1sap.h>

#include <sysmocom/femtobts/superfemto.h>
#include <sysmocom/femtobts/gsml1prim.h>
#include <sysmocom/femtobts/gsml1const.h>
#include <sysmocom/femtobts/gsml1types.h>

#include "femtobts.h"
#include "l1_if.h"
#include "l1_transp.h"
#include "hw_misc.h"

extern int pcu_direct;

#define MIN_QUAL_RACH	 5.0f	/* at least  5 dB C/I */
#define MIN_QUAL_NORM	-0.5f	/* at least -1 dB C/I */

struct wait_l1_conf {
	struct llist_head list;		/* internal linked list */
	struct osmo_timer_list timer;	/* timer for L1 timeout */
	unsigned int conf_prim_id;	/* primitive we expect in response */
	unsigned int is_sys_prim;	/* is this a system (1) or L1 (0) primitive */
	l1if_compl_cb *cb;
	void *cb_data;
};

static void release_wlc(struct wait_l1_conf *wlc)
{
	osmo_timer_del(&wlc->timer);
	talloc_free(wlc);
}

static void l1if_req_timeout(void *data)
{
	struct wait_l1_conf *wlc = data;

	if (wlc->is_sys_prim)
		LOGP(DL1C, LOGL_FATAL, "Timeout waiting for SYS primitive %s\n",
			get_value_string(femtobts_sysprim_names, wlc->conf_prim_id));
	else
		LOGP(DL1C, LOGL_FATAL, "Timeout waiting for L1 primitive %s\n",
			get_value_string(femtobts_l1prim_names, wlc->conf_prim_id));
	exit(23);
}

static int _l1if_req_compl(struct femtol1_hdl *fl1h, struct msgb *msg,
		   int is_system_prim, l1if_compl_cb *cb)
{
	struct wait_l1_conf *wlc;
	struct osmo_wqueue *wqueue;
	unsigned int timeout_secs;

	/* allocate new wsc and store reference to mutex and conf_id */
	wlc = talloc_zero(fl1h, struct wait_l1_conf);
	wlc->cb = cb;
	wlc->cb_data = NULL;

	/* Make sure we actually have received a REQUEST type primitive */
	if (is_system_prim == 0) {
		GsmL1_Prim_t *l1p = msgb_l1prim(msg);

		LOGP(DL1P, LOGL_INFO, "Tx L1 prim %s\n",
			get_value_string(femtobts_l1prim_names, l1p->id));

		if (femtobts_l1prim_type[l1p->id] != L1P_T_REQ) {
			LOGP(DL1C, LOGL_ERROR, "L1 Prim %s is not a Request!\n",
				get_value_string(femtobts_l1prim_names, l1p->id));
			talloc_free(wlc);
			return -EINVAL;
		}
		wlc->is_sys_prim = 0;
		wlc->conf_prim_id = femtobts_l1prim_req2conf[l1p->id];
		wqueue = &fl1h->write_q[MQ_L1_WRITE];
		timeout_secs = 30;
	} else {
		SuperFemto_Prim_t *sysp = msgb_sysprim(msg);

		LOGP(DL1C, LOGL_INFO, "Tx SYS prim %s\n",
			get_value_string(femtobts_sysprim_names, sysp->id));

		if (femtobts_sysprim_type[sysp->id] != L1P_T_REQ) {
			LOGP(DL1C, LOGL_ERROR, "SYS Prim %s is not a Request!\n",
				get_value_string(femtobts_sysprim_names, sysp->id));
			talloc_free(wlc);
			return -EINVAL;
		}
		wlc->is_sys_prim = 1;
		wlc->conf_prim_id = femtobts_sysprim_req2conf[sysp->id];
		wqueue = &fl1h->write_q[MQ_SYS_WRITE];
		timeout_secs = 30;
	}

	/* enqueue the message in the queue and add wsc to list */
	osmo_wqueue_enqueue(wqueue, msg);
	llist_add(&wlc->list, &fl1h->wlc_list);

	/* schedule a timer for timeout_secs seconds. If DSP fails to respond, we terminate */
	wlc->timer.data = wlc;
	wlc->timer.cb = l1if_req_timeout;
	osmo_timer_schedule(&wlc->timer, timeout_secs, 0);

	return 0;
}

/* send a request primitive to the L1 and schedule completion call-back */
int l1if_req_compl(struct femtol1_hdl *fl1h, struct msgb *msg,
		   l1if_compl_cb *cb)
{
	return _l1if_req_compl(fl1h, msg, 1, cb);
}

int l1if_gsm_req_compl(struct femtol1_hdl *fl1h, struct msgb *msg,
		   l1if_compl_cb *cb)
{
	return _l1if_req_compl(fl1h, msg, 0, cb);
}

/* allocate a msgb containing a GsmL1_Prim_t */
struct msgb *l1p_msgb_alloc(void)
{
	struct msgb *msg = msgb_alloc(sizeof(GsmL1_Prim_t), "l1_prim");

	if (msg)
		msg->l1h = msgb_put(msg, sizeof(GsmL1_Prim_t));

	return msg;
}

/* allocate a msgb containing a SuperFemto_Prim_t */
struct msgb *sysp_msgb_alloc(void)
{
	struct msgb *msg = msgb_alloc(sizeof(SuperFemto_Prim_t), "sys_prim");

	if (msg)
		msg->l1h = msgb_put(msg, sizeof(SuperFemto_Prim_t));

	return msg;
}

static GsmL1_PhDataReq_t *
data_req_from_rts_ind(GsmL1_Prim_t *l1p,
		const GsmL1_PhReadyToSendInd_t *rts_ind)
{
	GsmL1_PhDataReq_t *data_req = &l1p->u.phDataReq;

	l1p->id = GsmL1_PrimId_PhDataReq;

	/* copy fields from PH-RSS.ind */
	data_req->hLayer1	= rts_ind->hLayer1;
	data_req->u8Tn 		= rts_ind->u8Tn;
	data_req->u32Fn		= rts_ind->u32Fn;
	data_req->sapi		= rts_ind->sapi;
	data_req->subCh		= rts_ind->subCh;
	data_req->u8BlockNbr	= rts_ind->u8BlockNbr;

	return data_req;
}

static GsmL1_PhEmptyFrameReq_t *
empty_req_from_rts_ind(GsmL1_Prim_t *l1p,
			const GsmL1_PhReadyToSendInd_t *rts_ind)
{
	GsmL1_PhEmptyFrameReq_t *empty_req = &l1p->u.phEmptyFrameReq;

	l1p->id = GsmL1_PrimId_PhEmptyFrameReq;

	empty_req->hLayer1 = rts_ind->hLayer1;
	empty_req->u8Tn = rts_ind->u8Tn;
	empty_req->u32Fn = rts_ind->u32Fn;
	empty_req->sapi = rts_ind->sapi;
	empty_req->subCh = rts_ind->subCh;
	empty_req->u8BlockNbr = rts_ind->u8BlockNbr;

	return empty_req;
}

static const uint8_t fill_frame[GSM_MACBLOCK_LEN] = {
	0x03, 0x03, 0x01, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B,
	0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B,
	0x2B, 0x2B, 0x2B
};

static void dump_meas_res(int ll, GsmL1_MeasParam_t *m)
{
	LOGPC(DL1C, ll, ", Meas: RSSI %-3.2f dBm,  Qual %-3.2f dB,  "
		"BER %-3.2f,  Timing %d\n", m->fRssi, m->fLinkQuality,
		m->fBer, m->i16BurstTiming);
}

static int process_meas_res(struct gsm_bts_trx *trx, uint8_t chan_nr,
				GsmL1_MeasParam_t *m)
{
	struct osmo_phsap_prim l1sap;

	memset(&l1sap, 0, sizeof(l1sap));
	osmo_prim_init(&l1sap.oph, SAP_GSM_PH, PRIM_MPH_INFO,
		PRIM_OP_INDICATION, NULL);
	l1sap.u.info.type = PRIM_INFO_MEAS;
	l1sap.u.info.u.meas_ind.chan_nr = chan_nr;
	l1sap.u.info.u.meas_ind.ta_offs_qbits = m->i16BurstTiming;
	l1sap.u.info.u.meas_ind.ber10k = (unsigned int) (m->fBer * 100);
	l1sap.u.info.u.meas_ind.inv_rssi = (uint8_t) (m->fRssi * -1);

	return l1sap_up(trx, &l1sap);
}

/* primitive from common part */
int bts_model_l1sap_down(struct gsm_bts_trx *trx, struct osmo_phsap_prim *l1sap)
{
	struct femtol1_hdl *fl1 = trx_femtol1_hdl(trx);
	struct msgb *msg = l1sap->oph.msg;
	uint32_t u32Fn;
	uint8_t u8Tn, subCh, u8BlockNbr = 0, sapi, ss;
	uint8_t chan_nr, link_id;
	int rc = 0;
	struct msgb *nmsg = NULL;
	GsmL1_Prim_t *l1p;
	struct gsm_lchan *lchan;

	switch (OSMO_PRIM_HDR(&l1sap->oph)) {
	case OSMO_PRIM(PRIM_PH_DATA, PRIM_OP_REQUEST):
		if (!msg) {
			LOGP(DL1C, LOGL_FATAL, "PH-DATA.req without msg. "
				"Please fix!\n");
			abort();
		}
		chan_nr = l1sap->u.data.chan_nr;
		link_id = l1sap->u.data.link_id;
		u32Fn = l1sap->u.data.fn;
		u8Tn = L1SAP_CHAN2TS(chan_nr);
		subCh = 0x1f;
		if (L1SAP_IS_LINK_SACCH(link_id)) {
			sapi = GsmL1_Sapi_Sacch;
			if (!L1SAP_IS_CHAN_TCHF(chan_nr))
				subCh = l1sap_chan2ss(chan_nr);
		} else if (L1SAP_IS_CHAN_TCHF(chan_nr)) {
			if (trx->ts[u8Tn].pchan == GSM_PCHAN_PDCH) {
				if (L1SAP_IS_PTCCH(u32Fn)) {
					sapi = GsmL1_Sapi_Ptcch;
					u8BlockNbr = L1SAP_FN2PTCCHBLOCK(u32Fn);
				} else {
					sapi = GsmL1_Sapi_Pdtch;
					u8BlockNbr = L1SAP_FN2MACBLOCK(u32Fn);
				}
			} else {
				sapi = GsmL1_Sapi_FacchF;
				u8BlockNbr = (u32Fn % 13) >> 2;
			}
		} else if (L1SAP_IS_CHAN_TCHH(chan_nr)) {
			subCh = L1SAP_CHAN2SS_TCHH(chan_nr);
			sapi = GsmL1_Sapi_FacchH;
			u8BlockNbr = (u32Fn % 26) >> 3;
		} else if (L1SAP_IS_CHAN_SDCCH4(chan_nr)) {
			subCh = L1SAP_CHAN2SS_SDCCH4(chan_nr);
			sapi = GsmL1_Sapi_Sdcch;
		} else if (L1SAP_IS_CHAN_SDCCH8(chan_nr)) {
			subCh = L1SAP_CHAN2SS_SDCCH8(chan_nr);
			sapi = GsmL1_Sapi_Sdcch;
		} else if (L1SAP_IS_CHAN_BCCH(chan_nr)) {
			sapi = GsmL1_Sapi_Bcch;
		} else if (L1SAP_IS_CHAN_AGCH_PCH(chan_nr)) {
#warning Set BS_AG_BLKS_RES
			/* The sapi depends on DSP configuration, not
			 * on the actual SYSTEM INFORMATION 3. */
			u8BlockNbr = L1SAP_FN2CCCHBLOCK(u32Fn);
			if (u8BlockNbr >= 1)
				sapi = GsmL1_Sapi_Pch;
			else
				sapi = GsmL1_Sapi_Agch;
		} else {
			LOGP(DL1C, LOGL_NOTICE, "unknown prim %d op %d "
				"chan_nr %d link_id %d\n", l1sap->oph.primitive,
				l1sap->oph.operation, chan_nr, link_id);
			rc = -EINVAL;
			goto done;
		}

		msgb_pull(msg, sizeof(*l1sap));

		/* create new message */
		nmsg = l1p_msgb_alloc();
		l1p = msgb_l1prim(nmsg);
		if (msg->len) {
			/* data request */
			GsmL1_PhDataReq_t *data_req = &l1p->u.phDataReq;
			GsmL1_MsgUnitParam_t *msu_param;

			l1p->id = GsmL1_PrimId_PhDataReq;

			data_req->hLayer1 = fl1->hLayer1;
			data_req->u8Tn = u8Tn;
			data_req->u32Fn = u32Fn;
			data_req->sapi = sapi;
			data_req->subCh = subCh;
			data_req->u8BlockNbr = u8BlockNbr;
			msu_param = &data_req->msgUnitParam;
			msu_param->u8Size = msg->len;
			memcpy(msu_param->u8Buffer, msg->data, msg->len);
		} else {
			/* empty frame */
			GsmL1_PhEmptyFrameReq_t *empty_req =
							&l1p->u.phEmptyFrameReq;

			l1p->id = GsmL1_PrimId_PhEmptyFrameReq;

			empty_req->hLayer1 = fl1->hLayer1;
			empty_req->u8Tn = u8Tn;
			empty_req->u32Fn = u32Fn;
			empty_req->sapi = sapi;
			empty_req->subCh = subCh;
			empty_req->u8BlockNbr = u8BlockNbr;
		}

		/* send message to DSP's queue */
		osmo_wqueue_enqueue(&fl1->write_q[MQ_L1_WRITE], nmsg);
		break;
	case OSMO_PRIM(PRIM_TCH, PRIM_OP_REQUEST):
		chan_nr = l1sap->u.tch.chan_nr;
		u32Fn = l1sap->u.tch.fn;
		u8Tn = L1SAP_CHAN2TS(chan_nr);
		u8BlockNbr = (u32Fn % 13) >> 2;
		if (L1SAP_IS_CHAN_TCHH(chan_nr)) {
			ss = subCh = L1SAP_CHAN2SS_TCHH(chan_nr);
			sapi = GsmL1_Sapi_TchH;
		} else {
			subCh = 0x1f;
			ss = 0;
			sapi = GsmL1_Sapi_TchF;
		}

		lchan = &trx->ts[u8Tn].lchan[ss];

		/* create new message and fill data */
		if (msg) {
			msgb_pull(msg, sizeof(*l1sap));
			/* create new message */
			nmsg = l1p_msgb_alloc();
			if (!nmsg) {
				rc = -ENOMEM;
				goto done;
			}
			l1p = msgb_l1prim(nmsg);
			l1if_tch_encode(lchan,
				l1p->u.phDataReq.msgUnitParam.u8Buffer,
				&l1p->u.phDataReq.msgUnitParam.u8Size,
				msg->data, msg->len);
		}

		/* no message/data, we generate an empty traffic msg */
		if (!nmsg)
			nmsg = gen_empty_tch_msg(lchan);

		/* no traffic message, we generate an empty msg */
		if (!nmsg) {
			nmsg = l1p_msgb_alloc();
			if (!nmsg) {
				rc = -ENOMEM;
				goto done;
			}
		}

		l1p = msgb_l1prim(nmsg);

		/* if we provide data, or if data is already in nmsg */
		if (l1p->u.phDataReq.msgUnitParam.u8Size) {
			/* data request */
			GsmL1_PhDataReq_t *data_req = &l1p->u.phDataReq;

			l1p->id = GsmL1_PrimId_PhDataReq;

			data_req->hLayer1 = fl1->hLayer1;
			data_req->u8Tn = u8Tn;
			data_req->u32Fn = u32Fn;
			data_req->sapi = sapi;
			data_req->subCh = subCh;
			data_req->u8BlockNbr = u8BlockNbr;
		} else {
			/* empty frame */
			GsmL1_PhEmptyFrameReq_t *empty_req =
							&l1p->u.phEmptyFrameReq;

			l1p->id = GsmL1_PrimId_PhEmptyFrameReq;

			empty_req->hLayer1 = fl1->hLayer1;
			empty_req->u8Tn = u8Tn;
			empty_req->u32Fn = u32Fn;
			empty_req->sapi = sapi;
			empty_req->subCh = subCh;
			empty_req->u8BlockNbr = u8BlockNbr;
		}
		/* send message to DSP's queue */
		osmo_wqueue_enqueue(&fl1->write_q[MQ_L1_WRITE], nmsg);
		break;
	case OSMO_PRIM(PRIM_MPH_INFO, PRIM_OP_REQUEST):
		switch (l1sap->u.info.type) {
		case PRIM_INFO_ACT_CIPH:
			chan_nr = l1sap->u.info.u.ciph_req.chan_nr;
			u8Tn = L1SAP_CHAN2TS(chan_nr);
			ss = l1sap_chan2ss(chan_nr);
			lchan = &trx->ts[u8Tn].lchan[ss];
			if (l1sap->u.info.u.ciph_req.uplink) {
				l1if_set_ciphering(fl1, lchan, 1);
				lchan->ciph_state = LCHAN_CIPH_RX_REQ;
			}
			if (l1sap->u.info.u.ciph_req.downlink) {
				l1if_set_ciphering(fl1, lchan, 0);
				lchan->ciph_state = LCHAN_CIPH_RX_CONF_TX_REQ;
			}
			if (l1sap->u.info.u.ciph_req.downlink
			 && l1sap->u.info.u.ciph_req.uplink)
				lchan->ciph_state = LCHAN_CIPH_RXTX_REQ;
			break;
		case PRIM_INFO_ACTIVATE:
		case PRIM_INFO_DEACTIVATE:
		case PRIM_INFO_MODIFY:
			chan_nr = l1sap->u.info.u.act_req.chan_nr;
			u8Tn = L1SAP_CHAN2TS(chan_nr);
			ss = l1sap_chan2ss(chan_nr);
			lchan = &trx->ts[u8Tn].lchan[ss];
			if (l1sap->u.info.type == PRIM_INFO_ACTIVATE)
				l1if_rsl_chan_act(lchan);
			else if (l1sap->u.info.type == PRIM_INFO_MODIFY)
				l1if_rsl_mode_modify(lchan);
			else if (l1sap->u.info.u.act_req.sacch_only)
				l1if_rsl_deact_sacch(lchan);
			else
				l1if_rsl_chan_rel(lchan);
			break;
		default:
			LOGP(DL1C, LOGL_NOTICE, "unknown MPH-INFO.req %d\n",
				l1sap->u.info.type);
			rc = -EINVAL;
			goto done;
		}
		break;
	default:
		LOGP(DL1C, LOGL_NOTICE, "unknown prim %d op %d\n",
			l1sap->oph.primitive, l1sap->oph.operation);
		rc = -EINVAL;
		goto done;
	}

done:
	if (msg)
		msgb_free(msg);
	return rc;
}

static int handle_mph_time_ind(struct femtol1_hdl *fl1,
				GsmL1_MphTimeInd_t *time_ind)
{
	struct gsm_bts_trx *trx = fl1->priv;
	struct gsm_bts *bts = trx->bts;
	struct osmo_phsap_prim l1sap;
	uint32_t fn;

	/* increment the primitive count for the alive timer */
	fl1->alive_prim_cnt++;

	/* ignore every time indication, except for c0 */
	if (trx != bts->c0) {
		return 0;
	}

	fn = time_ind->u32Fn;

	memset(&l1sap, 0, sizeof(l1sap));
	osmo_prim_init(&l1sap.oph, SAP_GSM_PH, PRIM_MPH_INFO,
		PRIM_OP_INDICATION, NULL);
	l1sap.u.info.type = PRIM_INFO_TIME;
	l1sap.u.info.u.time_ind.fn = fn;

	return l1sap_up(trx, &l1sap);
}

static uint8_t chan_nr_by_sapi(enum gsm_phys_chan_config pchan,
			       GsmL1_Sapi_t sapi, GsmL1_SubCh_t subCh,
			       uint8_t u8Tn, uint32_t u32Fn)
{
	uint8_t cbits = 0;
	switch (sapi) {
	case GsmL1_Sapi_Bcch:
		cbits = 0x10;
		break;
	case GsmL1_Sapi_Sacch:
		switch(pchan) {
		case GSM_PCHAN_TCH_F:
			cbits = 0x01;
			break;
		case GSM_PCHAN_TCH_H:
			cbits = 0x02 + subCh;
			break;
		case GSM_PCHAN_CCCH_SDCCH4:
			cbits = 0x04 + subCh;
			break;
		case GSM_PCHAN_SDCCH8_SACCH8C:
			cbits = 0x08 + subCh;
			break;
		default:
			LOGP(DL1C, LOGL_ERROR, "SACCH for pchan %d?\n",
				pchan);
			return 0;
		}
		break;
	case GsmL1_Sapi_Sdcch:
		switch(pchan) {
		case GSM_PCHAN_CCCH_SDCCH4:
			cbits = 0x04 + subCh;
			break;
		case GSM_PCHAN_SDCCH8_SACCH8C:
			cbits = 0x08 + subCh;
			break;
		default:
			LOGP(DL1C, LOGL_ERROR, "SDCCH for pchan %d?\n",
				pchan);
			return 0;
		}
		break;
	case GsmL1_Sapi_Agch:
	case GsmL1_Sapi_Pch:
		cbits = 0x12;
		break;
	case GsmL1_Sapi_TchF:
		cbits = 0x01;
		break;
	case GsmL1_Sapi_TchH:
		cbits = 0x02 + subCh;
		break;
	case GsmL1_Sapi_FacchF:
		cbits = 0x01;
		break;
	case GsmL1_Sapi_FacchH:
		cbits = 0x02 + subCh;
		break;
	case GsmL1_Sapi_Pdtch:
	case GsmL1_Sapi_Pacch:
		switch(pchan) {
		case GSM_PCHAN_PDCH:
			cbits = 0x01;
			break;
		default:
			LOGP(DL1C, LOGL_ERROR, "PDTCH for pchan %d?\n",
				pchan);
			return 0;
		}
		break;
	case GsmL1_Sapi_Ptcch:
		if (!L1SAP_IS_PTCCH(u32Fn)) {
			LOGP(DL1C, LOGL_FATAL, "Not expecting PTCCH at frame "
				"number other than 12, got it at %u (%u). "
				"Please fix!\n", u32Fn % 52, u32Fn);
			abort();
		}
		switch(pchan) {
		case GSM_PCHAN_PDCH:
			cbits = 0x01;
			break;
		default:
			LOGP(DL1C, LOGL_ERROR, "PTCCH for pchan %d?\n",
				pchan);
			return 0;
		}
		break;
	default:
		return 0;
	}

	return (cbits << 3) | u8Tn;
}

static int handle_ph_readytosend_ind(struct femtol1_hdl *fl1,
				     GsmL1_PhReadyToSendInd_t *rts_ind,
				     struct msgb *l1p_msg)
{
	struct gsm_bts_trx *trx = fl1->priv;
	struct gsm_bts *bts = trx->bts;
	struct osmo_phsap_prim *l1sap;
	struct gsm_time g_time;
	uint8_t chan_nr, link_id;
	uint32_t fn;
	int rc;
	struct msgb *resp_msg;
	GsmL1_PhDataReq_t *data_req;
	GsmL1_MsgUnitParam_t *msu_param;
	uint32_t t3p;

	/* in case we need to forward primitive to common part*/
	chan_nr = chan_nr_by_sapi(trx->ts[rts_ind->u8Tn].pchan, rts_ind->sapi,
		rts_ind->subCh, rts_ind->u8Tn, rts_ind->u32Fn);
	if (chan_nr) {
		fn = rts_ind->u32Fn;
		if (rts_ind->sapi == GsmL1_Sapi_Sacch)
			link_id = 0x40;
		else
			link_id = 0;
		rc = msgb_trim(l1p_msg, sizeof(*l1sap));
		if (rc < 0)
			MSGB_ABORT(l1p_msg, "No room for primitive\n");
		l1sap = msgb_l1sap_prim(l1p_msg);
		if (rts_ind->sapi == GsmL1_Sapi_TchF
		 || rts_ind->sapi == GsmL1_Sapi_TchH) {
			osmo_prim_init(&l1sap->oph, SAP_GSM_PH, PRIM_TCH_RTS,
				PRIM_OP_INDICATION, l1p_msg);
			l1sap->u.tch.chan_nr = chan_nr;
			l1sap->u.tch.fn = fn;
		} else {
			osmo_prim_init(&l1sap->oph, SAP_GSM_PH, PRIM_PH_RTS,
				PRIM_OP_INDICATION, l1p_msg);
			l1sap->u.data.link_id = link_id;
			l1sap->u.data.chan_nr = chan_nr;
			l1sap->u.data.fn = fn;
		}

		return l1sap_up(trx, l1sap);
	}

	gsm_fn2gsmtime(&g_time, rts_ind->u32Fn);

	DEBUGP(DL1P, "Rx PH-RTS.ind %02u/%02u/%02u SAPI=%s\n",
		g_time.t1, g_time.t2, g_time.t3,
		get_value_string(femtobts_l1sapi_names, rts_ind->sapi));

	/* in all other cases, we need to allocate a new PH-DATA.ind
	 * primitive msgb and start to fill it */
	resp_msg = l1p_msgb_alloc();
	data_req = data_req_from_rts_ind(msgb_l1prim(resp_msg), rts_ind);
	msu_param = &data_req->msgUnitParam;

	/* set default size */
	msu_param->u8Size = GSM_MACBLOCK_LEN;

	switch (rts_ind->sapi) {
	case GsmL1_Sapi_Sch:
		gsm_fn2gsmtime(&g_time, rts_ind->u32Fn);
		/* compute T3prime */
		t3p = (g_time.t3 - 1) / 10;
		/* fill SCH burst with data */
		msu_param->u8Size = 4;
		msu_param->u8Buffer[0] = (bts->bsic << 2) | (g_time.t1 >> 9);
		msu_param->u8Buffer[1] = (g_time.t1 >> 1);
		msu_param->u8Buffer[2] = (g_time.t1 << 7) | (g_time.t2 << 2) | (t3p >> 1);
		msu_param->u8Buffer[3] = (t3p & 1);
		break;
	case GsmL1_Sapi_Prach:
		goto empty_frame;
		break;
	default:
		memcpy(msu_param->u8Buffer, fill_frame, GSM_MACBLOCK_LEN);
		break;
	}

tx:
	/* transmit */
	osmo_wqueue_enqueue(&fl1->write_q[MQ_L1_WRITE], resp_msg);

	msgb_free(l1p_msg);
	return 0;

empty_frame:
	/* in case we decide to send an empty frame... */
	empty_req_from_rts_ind(msgb_l1prim(resp_msg), rts_ind);

	goto tx;
}

static int handle_ph_data_ind(struct femtol1_hdl *fl1, GsmL1_PhDataInd_t *data_ind,
			      struct msgb *l1p_msg)
{
	struct gsm_bts_trx *trx = fl1->priv;
	struct osmo_phsap_prim *l1sap;
	uint8_t chan_nr, link_id;
	uint32_t fn;
	uint8_t *data, len;
	int8_t rssi;
	int rc;

	/* chan_nr and link_id */
	chan_nr = chan_nr_by_sapi(trx->ts[data_ind->u8Tn].pchan, data_ind->sapi,
		data_ind->subCh, data_ind->u8Tn, data_ind->u32Fn);
	if (!chan_nr) {
		LOGP(DL1C, LOGL_ERROR, "PH-DATA-INDICATION for unknown sapi "
			"%d\n", data_ind->sapi);
		return ENOTSUP;
	}
	fn = data_ind->u32Fn;
	if (data_ind->sapi == GsmL1_Sapi_Sacch)
		link_id = 0x40;
	else
		link_id = 0;

	/* uplink measurement */
	process_meas_res(trx, chan_nr, &data_ind->measParam);

	if (data_ind->measParam.fLinkQuality < fl1->min_qual_norm
	 && data_ind->msgUnitParam.u8Size != 0) {
		msgb_free(l1p_msg);
 		return 0;
	}

	DEBUGP(DL1C, "Rx PH-DATA.ind %s (hL2 %08x): %s",
		get_value_string(femtobts_l1sapi_names, data_ind->sapi),
		data_ind->hLayer2,
		osmo_hexdump(data_ind->msgUnitParam.u8Buffer,
			     data_ind->msgUnitParam.u8Size));
	dump_meas_res(LOGL_DEBUG, &data_ind->measParam);

	/* check for TCH */
	if (data_ind->sapi == GsmL1_Sapi_TchF
	 || data_ind->sapi == GsmL1_Sapi_TchH) {
 		/* TCH speech frame handling */
		return l1if_tch_rx(trx, chan_nr, l1p_msg);
 	}

	/* get rssi */
	rssi = (int8_t) (data_ind->measParam.fRssi);
	/* get data pointer and length */
	data = data_ind->msgUnitParam.u8Buffer;
	len = data_ind->msgUnitParam.u8Size;
	/* pull lower header part before data */
	msgb_pull(l1p_msg, data - l1p_msg->data);
	/* trim remaining data to it's size, to get rid of upper header part */
	rc = msgb_trim(l1p_msg, len);
	if (rc < 0)
		MSGB_ABORT(l1p_msg, "No room for primitive data\n");
	l1p_msg->l2h = l1p_msg->data;
	/* push new l1 header */
	l1p_msg->l1h = msgb_push(l1p_msg, sizeof(*l1sap));
	/* fill header */
	l1sap = msgb_l1sap_prim(l1p_msg);
	osmo_prim_init(&l1sap->oph, SAP_GSM_PH, PRIM_PH_DATA, PRIM_OP_INDICATION,
		l1p_msg);
	l1sap->u.data.link_id = link_id;
	l1sap->u.data.chan_nr = chan_nr;
	l1sap->u.data.fn = fn;
	l1sap->u.data.rssi = rssi;

	return l1sap_up(trx, l1sap);
}


static int handle_ph_ra_ind(struct femtol1_hdl *fl1, GsmL1_PhRaInd_t *ra_ind,
			    struct msgb *l1p_msg)
{
	struct gsm_bts_trx *trx = fl1->priv;
	struct gsm_bts *bts = trx->bts;
	struct gsm_bts_role_bts *btsb = bts->role;
	struct osmo_phsap_prim *l1sap;
	uint32_t fn;
	uint8_t ra, acc_delay;
	int rc;

	/* increment number of busy RACH slots, if required */
	if (trx == bts->c0 &&
	    ra_ind->measParam.fRssi >= btsb->load.rach.busy_thresh)
		btsb->load.rach.busy++;

	if (ra_ind->measParam.fLinkQuality < fl1->min_qual_rach) {
		msgb_free(l1p_msg);
		return 0;
	}

	dump_meas_res(LOGL_DEBUG, &ra_ind->measParam);

	if (ra_ind->msgUnitParam.u8Size != 1) {
		LOGP(DL1C, LOGL_ERROR, "PH-RACH-INDICATION has %d bits\n",
			ra_ind->sapi);
		msgb_free(l1p_msg);
		return 0;
	}

	fn = ra_ind->u32Fn;
	ra = ra_ind->msgUnitParam.u8Buffer[0];
	/* check for under/overflow / sign */
	if (ra_ind->measParam.i16BurstTiming < 0)
		acc_delay = 0;
	else
		acc_delay = ra_ind->measParam.i16BurstTiming >> 2;
	rc = msgb_trim(l1p_msg, sizeof(*l1sap));
	if (rc < 0)
		MSGB_ABORT(l1p_msg, "No room for primitive data\n");
	l1sap = msgb_l1sap_prim(l1p_msg);
	osmo_prim_init(&l1sap->oph, SAP_GSM_PH, PRIM_PH_RACH, PRIM_OP_INDICATION,
		l1p_msg);
	l1sap->u.rach_ind.ra = ra;
	l1sap->u.rach_ind.acc_delay = acc_delay;
	l1sap->u.rach_ind.fn = fn;

	return l1sap_up(trx, l1sap);
}

/* handle any random indication from the L1 */
static int l1if_handle_ind(struct femtol1_hdl *fl1, struct msgb *msg)
{
	GsmL1_Prim_t *l1p = msgb_l1prim(msg);
	int rc = 0;

	switch (l1p->id) {
	case GsmL1_PrimId_MphTimeInd:
		rc = handle_mph_time_ind(fl1, &l1p->u.mphTimeInd);
		break;
	case GsmL1_PrimId_MphSyncInd:
		break;
	case GsmL1_PrimId_PhConnectInd:
		break;
	case GsmL1_PrimId_PhReadyToSendInd:
		return handle_ph_readytosend_ind(fl1, &l1p->u.phReadyToSendInd, msg);
		break;
	case GsmL1_PrimId_PhDataInd:
		return handle_ph_data_ind(fl1, &l1p->u.phDataInd, msg);
		break;
	case GsmL1_PrimId_PhRaInd:
		return handle_ph_ra_ind(fl1, &l1p->u.phRaInd, msg);
		break;
	default:
		break;
	}

	msgb_free(msg);

	return rc;
}

static inline int is_prim_compat(GsmL1_Prim_t *l1p, struct wait_l1_conf *wlc)
{
	/* the limitation here is that we cannot have multiple callers
	 * sending the same primitive */
	if (wlc->is_sys_prim != 0)
		return 0;
	if (l1p->id != wlc->conf_prim_id)
		return 0;
	return 1;
}

int l1if_handle_l1prim(int wq, struct femtol1_hdl *fl1h, struct msgb *msg)
{
	GsmL1_Prim_t *l1p = msgb_l1prim(msg);
	struct wait_l1_conf *wlc;
	int rc;

	switch (l1p->id) {
	case GsmL1_PrimId_MphTimeInd:
		/* silent, don't clog the log file */
		break;
	default:
		LOGP(DL1P, LOGL_DEBUG, "Rx L1 prim %s on queue %d\n",
			get_value_string(femtobts_l1prim_names, l1p->id), wq);
	}

	/* check if this is a resposne to a sync-waiting request */
	llist_for_each_entry(wlc, &fl1h->wlc_list, list) {
		if (is_prim_compat(l1p, wlc)) {
			llist_del(&wlc->list);
			if (wlc->cb)
				rc = wlc->cb(fl1h->priv, msg);
			else {
				rc = 0;
				msgb_free(msg);
			}
			release_wlc(wlc);
			return rc;
		}
	}

	/* if we reach here, it is not a Conf for a pending Req */
	return l1if_handle_ind(fl1h, msg);
}

int l1if_handle_sysprim(struct femtol1_hdl *fl1h, struct msgb *msg)
{
	SuperFemto_Prim_t *sysp = msgb_sysprim(msg);
	struct wait_l1_conf *wlc;
	int rc;

	LOGP(DL1P, LOGL_DEBUG, "Rx SYS prim %s\n",
		get_value_string(femtobts_sysprim_names, sysp->id));

	/* check if this is a resposne to a sync-waiting request */
	llist_for_each_entry(wlc, &fl1h->wlc_list, list) {
		/* the limitation here is that we cannot have multiple callers
		 * sending the same primitive */
		if (wlc->is_sys_prim && sysp->id == wlc->conf_prim_id) {
			llist_del(&wlc->list);
			if (wlc->cb)
				rc = wlc->cb(fl1h->priv, msg);
			else {
				rc = 0;
				msgb_free(msg);
			}
			release_wlc(wlc);
			return rc;
		}
	}
	/* if we reach here, it is not a Conf for a pending Req */
	return l1if_handle_ind(fl1h, msg);
}

#if 0
/* called by RSL if the BCCH SI has been modified */
int sysinfo_has_changed(struct gsm_bts *bts, int si)
{
	/* FIXME: Determine BS_AG_BLKS_RES and 
	 *  	* set cfgParams.u.agch.u8NbrOfAgch
	 *	* determine implications on paging
	 */
	/* FIXME: Check for Extended BCCH presence */
	/* FIXME: Check for CCCH_CONF */
	/* FIXME: Check for BS_PA_MFRMS: update paging */

	return 0;
}
#endif

static int activate_rf_compl_cb(struct gsm_bts_trx *trx, struct msgb *resp)
{
	SuperFemto_Prim_t *sysp = msgb_sysprim(resp);
	GsmL1_Status_t status;
	int on = 0;
	unsigned int i;

	if (sysp->id == SuperFemto_PrimId_ActivateRfCnf)
		on = 1;

	if (on)
		status = sysp->u.activateRfCnf.status;
	else
		status = sysp->u.deactivateRfCnf.status;

	LOGP(DL1C, LOGL_INFO, "Rx RF-%sACT.conf (status=%s)\n", on ? "" : "DE",
		get_value_string(femtobts_l1status_names, status));


	if (on) {
		if (status != GsmL1_Status_Success) {
			LOGP(DL1C, LOGL_FATAL, "RF-ACT.conf with status %s\n",
				get_value_string(femtobts_l1status_names, status));
			bts_shutdown(trx->bts, "RF-ACT failure");
		} else
			sysmobts_led_set(LED_RF_ACTIVE, 1);

		/* signal availability */
		oml_mo_state_chg(&trx->mo, NM_OPSTATE_DISABLED, NM_AVSTATE_OK);
		oml_mo_tx_sw_act_rep(&trx->mo);
		oml_mo_state_chg(&trx->bb_transc.mo, -1, NM_AVSTATE_OK);
		oml_mo_tx_sw_act_rep(&trx->bb_transc.mo);

		for (i = 0; i < ARRAY_SIZE(trx->ts); i++)
			oml_mo_state_chg(&trx->ts[i].mo, NM_OPSTATE_DISABLED, NM_AVSTATE_DEPENDENCY);
	} else {
		sysmobts_led_set(LED_RF_ACTIVE, 0);
		oml_mo_state_chg(&trx->mo, NM_OPSTATE_DISABLED, NM_AVSTATE_OFF_LINE);
		oml_mo_state_chg(&trx->bb_transc.mo, NM_OPSTATE_DISABLED, NM_AVSTATE_OFF_LINE);
	}

	msgb_free(resp);

	return 0;
}

/* activate or de-activate the entire RF-Frontend */
int l1if_activate_rf(struct femtol1_hdl *hdl, int on)
{
	struct msgb *msg = sysp_msgb_alloc();
	SuperFemto_Prim_t *sysp = msgb_sysprim(msg);

	if (on) {
		sysp->id = SuperFemto_PrimId_ActivateRfReq;
#ifdef HW_SYSMOBTS_V1
		sysp->u.activateRfReq.u12ClkVc = hdl->clk_cal;
#else
#if SUPERFEMTO_API_VERSION >= SUPERFEMTO_API(0,2,0)
		sysp->u.activateRfReq.timing.u8TimSrc = 1; /* Master */
#endif /* 0.2.0 */
		sysp->u.activateRfReq.msgq.u8UseTchMsgq = 0;
		sysp->u.activateRfReq.msgq.u8UsePdtchMsgq = pcu_direct;
		/* Use clock from OCXO or whatever source is configured */
#if SUPERFEMTO_API_VERSION < SUPERFEMTO_API(2,1,0)
		sysp->u.activateRfReq.rfTrx.u8ClkSrc = hdl->clk_src;
#else
		sysp->u.activateRfReq.rfTrx.clkSrc = hdl->clk_src;
#endif /* 2.1.0 */
		sysp->u.activateRfReq.rfTrx.iClkCor = hdl->clk_cal;
#if SUPERFEMTO_API_VERSION < SUPERFEMTO_API(2,4,0)
#if SUPERFEMTO_API_VERSION < SUPERFEMTO_API(2,1,0)
		sysp->u.activateRfReq.rfRx.u8ClkSrc = hdl->clk_src;
#else
		sysp->u.activateRfReq.rfRx.clkSrc = hdl->clk_src;
#endif /* 2.1.0 */
		sysp->u.activateRfReq.rfRx.iClkCor = hdl->clk_cal;
#endif /* API 2.4.0 */
#endif /* !HW_SYSMOBTS_V1 */
	} else {
		sysp->id = SuperFemto_PrimId_DeactivateRfReq;
	}

	return l1if_req_compl(hdl, msg, activate_rf_compl_cb);
}

/* call-back on arrival of DSP+FPGA version + band capability */
static int info_compl_cb(struct gsm_bts_trx *trx, struct msgb *resp)
{
	SuperFemto_Prim_t *sysp = msgb_sysprim(resp);
	SuperFemto_SystemInfoCnf_t *sic = &sysp->u.systemInfoCnf;
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(trx);
	int rc;

	fl1h->hw_info.dsp_version[0] = sic->dspVersion.major;
	fl1h->hw_info.dsp_version[1] = sic->dspVersion.minor;
	fl1h->hw_info.dsp_version[2] = sic->dspVersion.build;

	fl1h->hw_info.fpga_version[0] = sic->fpgaVersion.major;
	fl1h->hw_info.fpga_version[1] = sic->fpgaVersion.minor;
	fl1h->hw_info.fpga_version[2] = sic->fpgaVersion.build;

	LOGP(DL1C, LOGL_INFO, "DSP v%u.%u.%u, FPGA v%u.%u.%u\n",
		sic->dspVersion.major, sic->dspVersion.minor,
		sic->dspVersion.build, sic->fpgaVersion.major,
		sic->fpgaVersion.minor, sic->fpgaVersion.build);

#ifdef HW_SYSMOBTS_V1
	if (sic->rfBand.gsm850)
		fl1h->hw_info.band_support |= GSM_BAND_850;
	if (sic->rfBand.gsm900)
		fl1h->hw_info.band_support |= GSM_BAND_900;
	if (sic->rfBand.dcs1800)
		fl1h->hw_info.band_support |= GSM_BAND_1800;
	if (sic->rfBand.pcs1900)
		fl1h->hw_info.band_support |= GSM_BAND_1900;
#else
	fl1h->hw_info.band_support |= GSM_BAND_850 | GSM_BAND_900 | GSM_BAND_1800 | GSM_BAND_1900;
#endif

	if (!(fl1h->hw_info.band_support & trx->bts->band))
		LOGP(DL1C, LOGL_FATAL, "BTS band %s not supported by hw\n",
		     gsm_band_name(trx->bts->band));

#if SUPERFEMTO_API_VERSION >= SUPERFEMTO_API(2,4,0)
	/* load calibration tables (if we know their path) */
	rc = calib_load(fl1h);
	if (rc < 0)
		LOGP(DL1C, LOGL_ERROR, "Operating without calibration; "
			"unable to load tables!\n");
#else
	LOGP(DL1C, LOGL_NOTICE, "Operating without calibration "
		"as software was compiled against old header files\n");
#endif

	msgb_free(resp);

	/* FIXME: clock related */
	return 0;
}

/* request DSP+FPGA code versions + band capability */
static int l1if_get_info(struct femtol1_hdl *hdl)
{
	struct msgb *msg = sysp_msgb_alloc();
	SuperFemto_Prim_t *sysp = msgb_sysprim(msg);

	sysp->id = SuperFemto_PrimId_SystemInfoReq;

	return l1if_req_compl(hdl, msg, info_compl_cb);
}

static int reset_compl_cb(struct gsm_bts_trx *trx, struct msgb *resp)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(trx);
	SuperFemto_Prim_t *sysp = msgb_sysprim(resp);
	GsmL1_Status_t status = sysp->u.layer1ResetCnf.status;

	LOGP(DL1C, LOGL_NOTICE, "Rx L1-RESET.conf (status=%s)\n",
		get_value_string(femtobts_l1status_names, status));

	msgb_free(resp);

	/* If we're coming out of reset .. */
	if (status != GsmL1_Status_Success) {
		LOGP(DL1C, LOGL_FATAL, "L1-RESET.conf with status %s\n",
			get_value_string(femtobts_l1status_names, status));
		bts_shutdown(trx->bts, "L1-RESET failure");
	}

	/* as we cannot get the current DSP trace flags, we simply
	 * set them to zero (or whatever dsp_trace_f has been initialized to */
	l1if_set_trace_flags(fl1h, fl1h->dsp_trace_f);

	/* obtain version information on DSP/FPGA and band capabilities */
	l1if_get_info(fl1h);

	/* otherwise, request activation of RF board */
	l1if_activate_rf(fl1h, 1);

	return 0;
}

int l1if_reset(struct femtol1_hdl *hdl)
{
	struct msgb *msg = sysp_msgb_alloc();
	SuperFemto_Prim_t *sysp = msgb_sysprim(msg);
	sysp->id = SuperFemto_PrimId_Layer1ResetReq;

	return l1if_req_compl(hdl, msg, reset_compl_cb);
}

/* set the trace flags within the DSP */
int l1if_set_trace_flags(struct femtol1_hdl *hdl, uint32_t flags)
{
	struct msgb *msg = sysp_msgb_alloc();
	SuperFemto_Prim_t *sysp = msgb_sysprim(msg);

	LOGP(DL1C, LOGL_INFO, "Tx SET-TRACE-FLAGS.req (0x%08x)\n",
		flags);

	sysp->id = SuperFemto_PrimId_SetTraceFlagsReq;
	sysp->u.setTraceFlagsReq.u32Tf = flags;

	hdl->dsp_trace_f = flags;

	/* There is no confirmation we could wait for */
	return osmo_wqueue_enqueue(&hdl->write_q[MQ_SYS_WRITE], msg);
}

struct femtol1_hdl *l1if_open(void *priv)
{
	struct femtol1_hdl *fl1h;
	int rc;

#ifndef HW_SYSMOBTS_V1
	LOGP(DL1C, LOGL_INFO, "sysmoBTSv2 L1IF compiled against API headers "
			"v%u.%u.%u\n", SUPERFEMTO_API_VERSION >> 16,
			(SUPERFEMTO_API_VERSION >> 8) & 0xff,
			 SUPERFEMTO_API_VERSION & 0xff);
#else
	LOGP(DL1C, LOGL_INFO, "sysmoBTSv1 L1IF compiled against API headers "
			"v%u.%u.%u\n", FEMTOBTS_API_VERSION >> 16,
			(FEMTOBTS_API_VERSION >> 8) & 0xff,
			 FEMTOBTS_API_VERSION & 0xff);
#endif

	fl1h = talloc_zero(priv, struct femtol1_hdl);
	if (!fl1h)
		return NULL;
	INIT_LLIST_HEAD(&fl1h->wlc_list);

	fl1h->priv = priv;
	fl1h->clk_cal = 0;
	fl1h->ul_power_target = -75;	/* dBm default */
	fl1h->min_qual_rach = MIN_QUAL_RACH;
	fl1h->min_qual_norm = MIN_QUAL_NORM;
	/* default clock source: OCXO */
#if SUPERFEMTO_API_VERSION >= SUPERFEMTO_API(2,1,0)
	fl1h->clk_src = SuperFemto_ClkSrcId_Ocxo;
#else
	fl1h->clk_src = SF_CLKSRC_OCXO;
#endif

	rc = l1if_transport_open(MQ_SYS_WRITE, fl1h);
	if (rc < 0) {
		talloc_free(fl1h);
		return NULL;
	}

	rc = l1if_transport_open(MQ_L1_WRITE, fl1h);
	if (rc < 0) {
		l1if_transport_close(MQ_SYS_WRITE, fl1h);
		talloc_free(fl1h);
		return NULL;
	}

	return fl1h;
}

int l1if_close(struct femtol1_hdl *fl1h)
{
	l1if_transport_close(MQ_L1_WRITE, fl1h);
	l1if_transport_close(MQ_SYS_WRITE, fl1h);
	return 0;
}

