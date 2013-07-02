/* (C) 2011 by Harald Welte <laforge@gnumonks.org>
 * (C) 2013 by Holger Hans Peter Freyther
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
#include <errno.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

#include <sysmocom/femtobts/gsml1prim.h>
#include <sysmocom/femtobts/gsml1const.h>
#include <sysmocom/femtobts/gsml1types.h>

#include <osmo-bts/gsm_data.h>
#include <osmo-bts/logging.h>
#include <osmo-bts/oml.h>
#include <osmo-bts/rsl.h>
#include <osmo-bts/amr.h>
#include <osmo-bts/bts.h>
#include <osmo-bts/bts_model.h>
#include <osmo-bts/l1sap.h>

#include "l1_if.h"
#include "femtobts.h"
#include "utils.h"

static int mph_info_chan_confirm(struct gsm_lchan *lchan,
			enum osmo_mph_info_type type, uint8_t cause)
{
	struct osmo_phsap_prim l1sap;

	memset(&l1sap, 0, sizeof(l1sap));
	osmo_prim_init(&l1sap.oph, SAP_GSM_PH, PRIM_MPH_INFO, PRIM_OP_CONFIRM,
		NULL);
	l1sap.u.info.type = type;
	l1sap.u.info.u.act_cnf.chan_nr = gsm_lchan2chan_nr(lchan);
	l1sap.u.info.u.act_cnf.cause = cause;

	return l1sap_up(lchan->ts->trx, &l1sap);
}

enum sapi_cmd_type {
	SAPI_CMD_ACTIVATE,
	SAPI_CMD_CONFIG_CIPHERING,
	SAPI_CMD_CONFIG_LOGCH_PARAM,
	SAPI_CMD_SACCH_REL_MARKER,
	SAPI_CMD_REL_MARKER,
	SAPI_CMD_DEACTIVATE,
};

struct sapi_cmd {
	struct llist_head entry;
	GsmL1_Sapi_t sapi;
	GsmL1_Dir_t dir;
	enum sapi_cmd_type type;
	int (*callback)(struct gsm_lchan *lchan, int status);
};

static const enum GsmL1_LogChComb_t pchan_to_logChComb[_GSM_PCHAN_MAX] = {
	[GSM_PCHAN_NONE]		= GsmL1_LogChComb_0,
	[GSM_PCHAN_CCCH]		= GsmL1_LogChComb_IV,
	[GSM_PCHAN_CCCH_SDCCH4] 	= GsmL1_LogChComb_V,
	[GSM_PCHAN_TCH_F]		= GsmL1_LogChComb_I,
	[GSM_PCHAN_TCH_H]		= GsmL1_LogChComb_II,
	[GSM_PCHAN_SDCCH8_SACCH8C]	= GsmL1_LogChComb_VII,
	[GSM_PCHAN_PDCH]		= GsmL1_LogChComb_XIII,
	//[GSM_PCHAN_TCH_F_PDCH]		= FIXME,
	[GSM_PCHAN_UNKNOWN]		= GsmL1_LogChComb_0,
};

static void *prim_init(GsmL1_Prim_t *prim, GsmL1_PrimId_t id, struct femtol1_hdl *gl1)
{
	prim->id = id;

	/* for some reason the hLayer1 field is not always at the same position
	 * in the GsmL1_Prim_t, so we have to have this ugly case statement here... */
	switch (id) {
	case GsmL1_PrimId_MphInitReq:
		//prim->u.mphInitReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_MphCloseReq:
		prim->u.mphCloseReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_MphConnectReq:
		prim->u.mphConnectReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_MphDisconnectReq:
		prim->u.mphDisconnectReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_MphActivateReq:
		prim->u.mphActivateReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_MphDeactivateReq:
		prim->u.mphDeactivateReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_MphConfigReq:
		prim->u.mphConfigReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_MphMeasureReq:
		prim->u.mphMeasureReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_MphInitCnf:
	case GsmL1_PrimId_MphCloseCnf:
	case GsmL1_PrimId_MphConnectCnf:
	case GsmL1_PrimId_MphDisconnectCnf:
	case GsmL1_PrimId_MphActivateCnf:
	case GsmL1_PrimId_MphDeactivateCnf:
	case GsmL1_PrimId_MphConfigCnf:
	case GsmL1_PrimId_MphMeasureCnf:
		break;
	case GsmL1_PrimId_MphTimeInd:
		break;
	case GsmL1_PrimId_MphSyncInd:
		break;
	case GsmL1_PrimId_PhEmptyFrameReq:
		prim->u.phEmptyFrameReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_PhDataReq:
		prim->u.phDataReq.hLayer1 = gl1->hLayer1;
		break;
	case GsmL1_PrimId_PhConnectInd:
		break;
	case GsmL1_PrimId_PhReadyToSendInd:
		break;
	case GsmL1_PrimId_PhDataInd:
		break;
	case GsmL1_PrimId_PhRaInd:
		break;
	default:
		LOGP(DL1C, LOGL_ERROR, "unknown L1 primitive %u\n", id);
		break;
	}
	return &prim->u;
}

GsmL1_Status_t prim_status(GsmL1_Prim_t *prim)
{
	/* for some reason the Status field is not always at the same position
	 * in the GsmL1_Prim_t, so we have to have this ugly case statement here... */
	switch (prim->id) {
	case GsmL1_PrimId_MphInitCnf:
		return prim->u.mphInitCnf.status;
	case GsmL1_PrimId_MphCloseCnf:
		return prim->u.mphCloseCnf.status;
	case GsmL1_PrimId_MphConnectCnf:
		return prim->u.mphConnectCnf.status;
	case GsmL1_PrimId_MphDisconnectCnf:
		return prim->u.mphDisconnectCnf.status;
	case GsmL1_PrimId_MphActivateCnf:
		return prim->u.mphActivateCnf.status;
	case GsmL1_PrimId_MphDeactivateCnf:
		return prim->u.mphDeactivateCnf.status;
	case GsmL1_PrimId_MphConfigCnf:
		return prim->u.mphConfigCnf.status;
	case GsmL1_PrimId_MphMeasureCnf:
		return prim->u.mphMeasureCnf.status;
	default:
		break;
	}
	return GsmL1_Status_Success;
}

#if 0
static int compl_cb_send_oml_msg(struct msgb *l1_msg, void *data)
{
	struct msgb *resp_msg = data;
	GsmL1_Prim_t *l1p = msgb_l1prim(l1_msg);

	if (prim_status(l1p) != GsmL1_Status_Success) {
		LOGP(DL1C, LOGL_ERROR, "Rx %s, status: %s\n",
			get_value_string(femtobts_l1prim_names, l1p->id),
			get_value_string(femtobts_l1status_names, cc->status));
		return 0;
	}

	msgb_free(l1_msg);

	return abis_nm_sendmsg(msg);
}
#endif

int lchan_activate(struct gsm_lchan *lchan, enum gsm_lchan_state lchan_state);

static int opstart_compl(struct gsm_abis_mo *mo, struct msgb *l1_msg)
{
	GsmL1_Prim_t *l1p = msgb_l1prim(l1_msg);
	GsmL1_Status_t status = prim_status(l1p);

	if (status != GsmL1_Status_Success) {
		LOGP(DL1C, LOGL_ERROR, "Rx %s, status: %s\n",
			get_value_string(femtobts_l1prim_names, l1p->id),
			get_value_string(femtobts_l1status_names, status));
		msgb_free(l1_msg);
		return oml_mo_opstart_nack(mo, NM_NACK_CANT_PERFORM);
	}

	msgb_free(l1_msg);

	/* Set to Operational State: Enabled */
	oml_mo_state_chg(mo, NM_OPSTATE_ENABLED, NM_AVSTATE_OK);

	/* ugly hack to auto-activate all SAPIs for the BCCH/CCCH on TS0 */
	if (mo->obj_class == NM_OC_CHANNEL && mo->obj_inst.trx_nr == 0 &&
	    mo->obj_inst.ts_nr == 0) {
		DEBUGP(DL1C, "====> trying to activate lchans of BCCH\n");
		lchan_activate(&mo->bts->c0->ts[0].lchan[4], LCHAN_S_NONE);
	}

	/* Send OPSTART ack */
	return oml_mo_opstart_ack(mo);
}

static int opstart_compl_cb(struct gsm_bts_trx *trx, struct msgb *l1_msg)
{
	struct gsm_abis_mo *mo;
	GsmL1_Prim_t *l1p = msgb_l1prim(l1_msg);
	GsmL1_MphConnectCnf_t *cnf = &l1p->u.mphConnectCnf;

	mo = &trx->ts[cnf->u8Tn].mo;
	return opstart_compl(mo, l1_msg);
}

static int trx_init_compl_cb(struct gsm_bts_trx *trx, struct msgb *l1_msg)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(trx);

	GsmL1_Prim_t *l1p = msgb_l1prim(l1_msg);
	GsmL1_MphInitCnf_t *ic = &l1p->u.mphInitCnf;

	LOGP(DL1C, LOGL_INFO, "Rx MPH-INIT.conf (status=%s)\n",
		get_value_string(femtobts_l1status_names, ic->status));

	/* store layer1 handle */
	if (ic->status != GsmL1_Status_Success) {
		LOGP(DL1C, LOGL_FATAL, "Rx MPH-INIT.conf status=%s\n",
			get_value_string(femtobts_l1status_names, ic->status));
		bts_shutdown(trx->bts, "MPH-INIT failure");
	}

	fl1h->hLayer1 = ic->hLayer1;

	return opstart_compl(&trx->mo, l1_msg);
}

int gsm_abis_mo_check_attr(const struct gsm_abis_mo *mo, const uint8_t *attr_ids,
			   unsigned int num_attr_ids)
{
	unsigned int i;

	if (!mo->nm_attr)
		return 0;

	for (i = 0; i < num_attr_ids; i++) {
		if (!TLVP_PRESENT(mo->nm_attr, attr_ids[i]))
			return 0;
	}
	return 1;
}

static const uint8_t trx_rqd_attr[] = { NM_ATT_RF_MAXPOWR_R };

/* initialize the layer1 */
static int trx_init(struct gsm_bts_trx *trx)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(trx);
	struct msgb *msg;
	GsmL1_MphInitReq_t *mi_req;
	GsmL1_DeviceParam_t *dev_par;
	int femto_band;

	if (!gsm_abis_mo_check_attr(&trx->mo, trx_rqd_attr,
				    ARRAY_SIZE(trx_rqd_attr))) {
		/* HACK: spec says we need to decline, but openbsc
		 * doesn't deal with this very well */
		return oml_mo_opstart_ack(&trx->mo);
		//return oml_mo_opstart_nack(&trx->mo, NM_NACK_CANT_PERFORM);
	}

	femto_band = sysmobts_select_femto_band(trx->bts, trx->arfcn);
	if (femto_band < 0) {
		LOGP(DL1C, LOGL_ERROR, "Unsupported GSM band %s\n",
			gsm_band_name(trx->bts->band));
	}

	msg = l1p_msgb_alloc();
	mi_req = prim_init(msgb_l1prim(msg), GsmL1_PrimId_MphInitReq, fl1h);
	dev_par = &mi_req->deviceParam;
	dev_par->devType = GsmL1_DevType_TxdRxu;
	dev_par->freqBand = femto_band;
	dev_par->u16Arfcn = trx->arfcn;
	dev_par->u16BcchArfcn = trx->bts->c0->arfcn;
	dev_par->u8NbTsc = trx->bts->bsic & 7;
	dev_par->fRxPowerLevel = fl1h->ul_power_target;
	dev_par->fTxPowerLevel = trx->nominal_power - trx->max_power_red;
	LOGP(DL1C, LOGL_NOTICE, "Init TRX (ARFCN %u, TSC %u, RxPower % 2f dBm, "
		"TxPower % 2.2f dBm\n", dev_par->u16Arfcn, dev_par->u8NbTsc,
		dev_par->fRxPowerLevel, dev_par->fTxPowerLevel);
	
	/* send MPH-INIT-REQ, wait for MPH-INIT-CNF */
	return l1if_gsm_req_compl(fl1h, msg, trx_init_compl_cb);
}

uint32_t trx_get_hlayer1(struct gsm_bts_trx *trx)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(trx);

	return fl1h->hLayer1;
}

static int trx_close_compl_cb(struct gsm_bts_trx *trx, struct msgb *l1_msg)
{
	msgb_free(l1_msg);
	return 0;
}

int bts_model_trx_close(struct gsm_bts_trx *trx)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(trx);
	struct msgb *msg;

	msg = l1p_msgb_alloc();
	prim_init(msgb_l1prim(msg), GsmL1_PrimId_MphCloseReq, fl1h);
	LOGP(DL1C, LOGL_NOTICE, "Close TRX %u\n", trx->nr);

	return l1if_gsm_req_compl(fl1h, msg, trx_close_compl_cb);
}

static int ts_connect(struct gsm_bts_trx_ts *ts)
{
	struct msgb *msg = l1p_msgb_alloc();
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(ts->trx);
	GsmL1_MphConnectReq_t *cr;

	cr = prim_init(msgb_l1prim(msg), GsmL1_PrimId_MphConnectReq, fl1h);
	cr->u8Tn = ts->nr;
	cr->logChComb = pchan_to_logChComb[ts->pchan];
	
	return l1if_gsm_req_compl(fl1h, msg, opstart_compl_cb);
}

GsmL1_Sapi_t lchan_to_GsmL1_Sapi_t(const struct gsm_lchan *lchan)
{
	switch (lchan->type) {
	case GSM_LCHAN_TCH_F:
		return GsmL1_Sapi_TchF;
	case GSM_LCHAN_TCH_H:
		return GsmL1_Sapi_TchH;
	default:
		LOGP(DL1C, LOGL_NOTICE, "%s cannot determine L1 SAPI\n",
			gsm_lchan_name(lchan));
		break;
	}
	return GsmL1_Sapi_Idle;
}

GsmL1_SubCh_t lchan_to_GsmL1_SubCh_t(const struct gsm_lchan *lchan)
{
	switch (lchan->ts->pchan) {
	case GSM_PCHAN_CCCH_SDCCH4:
		if (lchan->type == GSM_LCHAN_CCCH)
			return GsmL1_SubCh_NA;
		/* fall-through */
	case GSM_PCHAN_TCH_H:
	case GSM_PCHAN_SDCCH8_SACCH8C:
		return lchan->nr;
	case GSM_PCHAN_NONE:
	case GSM_PCHAN_CCCH:
	case GSM_PCHAN_TCH_F:
	case GSM_PCHAN_PDCH:
	case GSM_PCHAN_UNKNOWN:
	default:
		return GsmL1_SubCh_NA;
	}

	return GsmL1_SubCh_NA;
}

struct sapi_dir {
	GsmL1_Sapi_t sapi;
	GsmL1_Dir_t dir;
};

static const struct sapi_dir ccch_sapis[] = {
	{ GsmL1_Sapi_Fcch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Sch, 	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Bcch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Agch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Pch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Rach,	GsmL1_Dir_RxUplink },
};

static const struct sapi_dir tchf_sapis[] = {
	{ GsmL1_Sapi_TchF,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_TchF,	GsmL1_Dir_RxUplink },
	{ GsmL1_Sapi_FacchF,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_FacchF,	GsmL1_Dir_RxUplink },
	{ GsmL1_Sapi_Sacch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Sacch,	GsmL1_Dir_RxUplink },
};

static const struct sapi_dir tchh_sapis[] = {
	{ GsmL1_Sapi_TchH,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_TchH,	GsmL1_Dir_RxUplink },
	{ GsmL1_Sapi_FacchH, 	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_FacchH, 	GsmL1_Dir_RxUplink },
	{ GsmL1_Sapi_Sacch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Sacch,	GsmL1_Dir_RxUplink },
};

static const struct sapi_dir sdcch_sapis[] = {
	{ GsmL1_Sapi_Sdcch, 	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Sdcch, 	GsmL1_Dir_RxUplink },
	{ GsmL1_Sapi_Sacch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Sacch,	GsmL1_Dir_RxUplink },
};

static const struct sapi_dir pdtch_sapis[] = {
	{ GsmL1_Sapi_Pdtch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Pdtch,	GsmL1_Dir_RxUplink },
	{ GsmL1_Sapi_Ptcch,	GsmL1_Dir_TxDownlink },
	{ GsmL1_Sapi_Prach,	GsmL1_Dir_RxUplink },
#if 0
	{ GsmL1_Sapi_Ptcch,	GsmL1_Dir_RxUplink },
	{ GsmL1_Sapi_Pacch,	GsmL1_Dir_TxDownlink },
#endif
};

struct lchan_sapis {
	const struct sapi_dir *sapis;
	unsigned int num_sapis;
};

static const struct lchan_sapis sapis_for_lchan[_GSM_LCHAN_MAX] = {
	[GSM_LCHAN_SDCCH] = {
		.sapis = sdcch_sapis,
		.num_sapis = ARRAY_SIZE(sdcch_sapis),
	},
	[GSM_LCHAN_TCH_F] = {
		.sapis = tchf_sapis,
		.num_sapis = ARRAY_SIZE(tchf_sapis),
	},
	[GSM_LCHAN_TCH_H] = {
		.sapis = tchh_sapis,
		.num_sapis = ARRAY_SIZE(tchh_sapis),
	},
	[GSM_LCHAN_CCCH] = {
		.sapis = ccch_sapis,
		.num_sapis = ARRAY_SIZE(ccch_sapis),
	},
	[GSM_LCHAN_PDTCH] = {
		.sapis = pdtch_sapis,
		.num_sapis = ARRAY_SIZE(pdtch_sapis),
	},
};

static int mph_send_activate_req(struct gsm_lchan *lchan, struct sapi_cmd *cmd);
static int mph_send_deactivate_req(struct gsm_lchan *lchan, struct sapi_cmd *cmd);
static int mph_send_config_ciphering(struct gsm_lchan *lchan, struct sapi_cmd *cmd);
static int mph_send_config_logchpar(struct gsm_lchan *lchan, struct sapi_cmd *cmd);

static int check_sapi_release(struct gsm_lchan *lchan, int sapi, int dir);
static int lchan_deactivate_sapis(struct gsm_lchan *lchan);

/**
 * Execute the first SAPI command of the queue. In case of the markers
 * this method is re-entrant so we need to make sure to remove a command
 * from the list before calling a function that will queue a command.
 *
 * \return 0 in case no Gsm Request was sent, 1 otherwise
 */
static int sapi_queue_exeute(struct gsm_lchan *lchan)
{
	int res;
	struct sapi_cmd *cmd;

	cmd = llist_entry(lchan->sapi_cmds.next, struct sapi_cmd, entry);

	switch (cmd->type) {
	case SAPI_CMD_ACTIVATE:
		mph_send_activate_req(lchan, cmd);
		res = 1;
		break;
	case SAPI_CMD_CONFIG_CIPHERING:
		mph_send_config_ciphering(lchan, cmd);
		res = 1;
		break;
	case SAPI_CMD_CONFIG_LOGCH_PARAM:
		mph_send_config_logchpar(lchan, cmd);
		res = 1;
		break;
	case SAPI_CMD_SACCH_REL_MARKER:
		llist_del(&cmd->entry);
		talloc_free(cmd);
		res = check_sapi_release(lchan, GsmL1_Sapi_Sacch,
					GsmL1_Dir_TxDownlink);
		res |= check_sapi_release(lchan, GsmL1_Sapi_Sacch,
					GsmL1_Dir_RxUplink);
		break;
	case SAPI_CMD_REL_MARKER:
		llist_del(&cmd->entry);
		talloc_free(cmd);
		res = lchan_deactivate_sapis(lchan);
		break;
	case SAPI_CMD_DEACTIVATE:
		mph_send_deactivate_req(lchan, cmd);
		res = 1;
		break;
	default:
		LOGP(DL1C, LOGL_NOTICE,
			"Unimplemented command type %d\n", cmd->type);
		llist_del(&cmd->entry);
		talloc_free(cmd);
		res = 0;
		abort();
		break;
	}

	return res;
}

static void sapi_queue_send(struct gsm_lchan *lchan)
{
	int res;

	do {
		res = sapi_queue_exeute(lchan);
	} while (res == 0 && !llist_empty(&lchan->sapi_cmds));
}

static void sapi_queue_dispatch(struct gsm_lchan *lchan, int status)
{
	int end;
	struct sapi_cmd *cmd = llist_entry(lchan->sapi_cmds.next,
			struct sapi_cmd, entry);
	llist_del(&cmd->entry);
	end = llist_empty(&lchan->sapi_cmds);

	if (cmd->callback)
		cmd->callback(lchan, status);
	talloc_free(cmd);

	if (end || llist_empty(&lchan->sapi_cmds)) {
		LOGP(DL1C, LOGL_NOTICE,
			"%s End of queue encountered. Now empty? %d\n",
			gsm_lchan_name(lchan), llist_empty(&lchan->sapi_cmds));
		return;
	}

	sapi_queue_send(lchan);
}

/**
 * Queue and possible execute a SAPI command. Return 1 in case the command was
 * already executed and 0 in case if it was only put into the queue
 */
static int queue_sapi_command(struct gsm_lchan *lchan, struct sapi_cmd *cmd)
{
	int start = llist_empty(&lchan->sapi_cmds);
	llist_add_tail(&cmd->entry, &lchan->sapi_cmds);

	if (!start)
		return 0;

	sapi_queue_send(lchan);
	return 1;
}

static int lchan_act_compl_cb(struct gsm_bts_trx *trx, struct msgb *l1_msg)
{
	enum lchan_sapi_state status;
	struct sapi_cmd *cmd;
	struct gsm_lchan *lchan;
	GsmL1_Prim_t *l1p = msgb_l1prim(l1_msg);
	GsmL1_MphActivateCnf_t *ic = &l1p->u.mphActivateCnf;

	/* get the lchan from the information we supplied */
	lchan = l1if_hLayer_to_lchan(trx, ic->hLayer3);
	if (!lchan) {
		LOGP(DL1C, LOGL_ERROR,
			"Failed to find lchan for hLayer3=0x%x\n", ic->hLayer3);
		goto err;
	}

	LOGP(DL1C, LOGL_INFO, "%s MPH-ACTIVATE.conf (%s ",
		gsm_lchan_name(lchan),
		get_value_string(femtobts_l1sapi_names, ic->sapi));
	LOGPC(DL1C, LOGL_INFO, "%s)\n",
		get_value_string(femtobts_dir_names, ic->dir));

	if (ic->status == GsmL1_Status_Success) {
		DEBUGP(DL1C, "Successful activation of L1 SAPI %s on TS %u\n",
			get_value_string(femtobts_l1sapi_names, ic->sapi), ic->u8Tn);
		status = LCHAN_SAPI_S_ASSIGNED;
	} else {
		LOGP(DL1C, LOGL_ERROR, "Error activating L1 SAPI %s on TS %u: %s\n",
			get_value_string(femtobts_l1sapi_names, ic->sapi), ic->u8Tn,
			get_value_string(femtobts_l1status_names, ic->status));
		status = LCHAN_SAPI_S_ERROR;
	}

	if (ic->dir & GsmL1_Dir_TxDownlink)
		lchan->sapis_dl[ic->sapi] = status;
	if (ic->dir & GsmL1_Dir_RxUplink)
		lchan->sapis_ul[ic->sapi] = status;

	if (llist_empty(&lchan->sapi_cmds)) {
		LOGP(DL1C, LOGL_ERROR,
				"%s Got activation confirmation with empty queue\n",
				gsm_lchan_name(lchan));
		goto err;
	}

	cmd = llist_entry(lchan->sapi_cmds.next, struct sapi_cmd, entry);
	if (cmd->sapi != ic->sapi || cmd->dir != ic->dir ||
			cmd->type != SAPI_CMD_ACTIVATE) {
		LOGP(DL1C, LOGL_ERROR,
				"%s Confirmation mismatch (%d, %d) (%d, %d)\n",
				gsm_lchan_name(lchan), cmd->sapi, cmd->dir,
				ic->sapi, ic->dir);
		goto err;
	}

	sapi_queue_dispatch(lchan, ic->status);

err:
	msgb_free(l1_msg);

	return 0;
}

uint32_t l1if_lchan_to_hLayer(struct gsm_lchan *lchan)
{
	return (lchan->nr << 8) | (lchan->ts->nr << 16) | (lchan->ts->trx->nr << 24);
}

/* obtain a ptr to the lapdm_channel for a given hLayer */
struct gsm_lchan *
l1if_hLayer_to_lchan(struct gsm_bts_trx *trx, uint32_t hLayer2)
{
	uint8_t ts_nr = (hLayer2 >> 16) & 0xff;
	uint8_t lchan_nr = (hLayer2 >> 8)& 0xff;
	struct gsm_bts_trx_ts *ts;

	/* FIXME: if we actually run on the BTS, the 32bit field is large
	 * enough to simply put a pointer inside. */
	if (ts_nr >= ARRAY_SIZE(trx->ts))
		return NULL;

	ts = &trx->ts[ts_nr];

	if (lchan_nr >= ARRAY_SIZE(ts->lchan))
		return NULL;

	return &ts->lchan[lchan_nr];
}

/* we regularly check if the DSP L1 is still sending us primitives.
 * if not, we simply stop the BTS program (and be re-spawned) */
static void alive_timer_cb(void *data)
{
	struct femtol1_hdl *fl1h = data;

	if (fl1h->alive_prim_cnt == 0) {
		LOGP(DL1C, LOGL_FATAL, "DSP L1 is no longer sending primitives!\n");
		exit(23);
	}
	fl1h->alive_prim_cnt = 0;
	osmo_timer_schedule(&fl1h->alive_timer, 5, 0);
}

static void clear_amr_params(GsmL1_LogChParam_t *lch_par)
{
	int j;
	/* common for the SIGN, V1 and EFR: */
	lch_par->tch.amrCmiPhase = GsmL1_AmrCmiPhase_NA;
	lch_par->tch.amrInitCodecMode = GsmL1_AmrCodecMode_Unset;
	for (j = 0; j < ARRAY_SIZE(lch_par->tch.amrActiveCodecSet); j++)
		lch_par->tch.amrActiveCodecSet[j] = GsmL1_AmrCodec_Unset;
}

static void set_payload_format(GsmL1_LogChParam_t *lch_par)
{
#ifdef L1_HAS_RTP_MODE
#ifdef USE_L1_RTP_MODE
	lch_par->tch.tchPlFmt = GsmL1_TchPlFmt_Rtp;
#else
	lch_par->tch.tchPlFmt = GsmL1_TchPlFmt_If2;
#endif /* USE_L1_RTP_MODE */
#endif /* L1_HAS_RTP_MODE */
}

static void lchan2lch_par(GsmL1_LogChParam_t *lch_par, struct gsm_lchan *lchan)
{
	int j;

	LOGP(DL1C, LOGL_INFO, "%s: %s tch_mode=0x%02x\n",
		gsm_lchan_name(lchan), __FUNCTION__, lchan->tch_mode);

	switch (lchan->tch_mode) {
	case GSM48_CMODE_SIGN:
		/* we have to set some TCH payload type even if we don't
		 * know yet what codec we will use later on */
		if (lchan->type == GSM_LCHAN_TCH_F)
			lch_par->tch.tchPlType = GsmL1_TchPlType_Fr;
		else
			lch_par->tch.tchPlType = GsmL1_TchPlType_Hr;
		clear_amr_params(lch_par);
		break;
	case GSM48_CMODE_SPEECH_V1:
		if (lchan->type == GSM_LCHAN_TCH_F)
			lch_par->tch.tchPlType = GsmL1_TchPlType_Fr;
		else
			lch_par->tch.tchPlType = GsmL1_TchPlType_Hr;
		set_payload_format(lch_par);
		clear_amr_params(lch_par);
		break;
	case GSM48_CMODE_SPEECH_EFR:
		lch_par->tch.tchPlType = GsmL1_TchPlType_Efr;
		set_payload_format(lch_par);
		clear_amr_params(lch_par);
		break;
	case GSM48_CMODE_SPEECH_AMR:
		lch_par->tch.tchPlType = GsmL1_TchPlType_Amr;
		set_payload_format(lch_par);
		lch_par->tch.amrCmiPhase = GsmL1_AmrCmiPhase_Odd; /* FIXME? */
		lch_par->tch.amrInitCodecMode = amr_get_initial_mode(lchan);

		/* initialize to clean state */
		for (j = 0; j < ARRAY_SIZE(lch_par->tch.amrActiveCodecSet); j++)
			lch_par->tch.amrActiveCodecSet[j] = GsmL1_AmrCodec_Unset;

		j = 0;
		if (lchan->mr_conf.m4_75)
			lch_par->tch.amrActiveCodecSet[j++] = GsmL1_AmrCodec_4_75;
		if (j >= ARRAY_SIZE(lch_par->tch.amrActiveCodecSet))
			break;

		if (lchan->mr_conf.m5_15)
			lch_par->tch.amrActiveCodecSet[j++] = GsmL1_AmrCodec_5_15;
		if (j >= ARRAY_SIZE(lch_par->tch.amrActiveCodecSet))
			break;

		if (lchan->mr_conf.m5_90)
			lch_par->tch.amrActiveCodecSet[j++] = GsmL1_AmrCodec_5_9;
		if (j >= ARRAY_SIZE(lch_par->tch.amrActiveCodecSet))
			break;

		if (lchan->mr_conf.m6_70)
			lch_par->tch.amrActiveCodecSet[j++] = GsmL1_AmrCodec_6_7;
		if (j >= ARRAY_SIZE(lch_par->tch.amrActiveCodecSet))
			break;

		if (lchan->mr_conf.m7_40)
			lch_par->tch.amrActiveCodecSet[j++] = GsmL1_AmrCodec_7_4;
		if (j >= ARRAY_SIZE(lch_par->tch.amrActiveCodecSet))
			break;

		if (lchan->mr_conf.m7_95)
			lch_par->tch.amrActiveCodecSet[j++] = GsmL1_AmrCodec_7_95;
		if (j >= ARRAY_SIZE(lch_par->tch.amrActiveCodecSet))
			break;

		if (lchan->mr_conf.m10_2)
			lch_par->tch.amrActiveCodecSet[j++] = GsmL1_AmrCodec_10_2;
		if (j >= ARRAY_SIZE(lch_par->tch.amrActiveCodecSet))
			break;
		if (lchan->mr_conf.m12_2)
			lch_par->tch.amrActiveCodecSet[j++] = GsmL1_AmrCodec_12_2;
		break;
	case GSM48_CMODE_DATA_14k5:
	case GSM48_CMODE_DATA_12k0:
	case GSM48_CMODE_DATA_6k0:
	case GSM48_CMODE_DATA_3k6:
		LOGP(DL1C, LOGL_ERROR, "%s: CSD not supported!\n",
			gsm_lchan_name(lchan));
		break;
	}
}

static int mph_send_activate_req(struct gsm_lchan *lchan, struct sapi_cmd *cmd)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(lchan->ts->trx);
	struct msgb *msg = l1p_msgb_alloc();
	int sapi = cmd->sapi;
	int dir = cmd->dir;
	GsmL1_MphActivateReq_t *act_req;
	GsmL1_LogChParam_t *lch_par;

	act_req = prim_init(msgb_l1prim(msg), GsmL1_PrimId_MphActivateReq, fl1h);
	lch_par = &act_req->logChPrm;
	act_req->u8Tn = lchan->ts->nr;
	act_req->subCh = lchan_to_GsmL1_SubCh_t(lchan);
	act_req->dir = dir;
	act_req->sapi = sapi;
	act_req->hLayer2 = l1if_lchan_to_hLayer(lchan);
	act_req->hLayer3 = act_req->hLayer2;

	switch (act_req->sapi) {
	case GsmL1_Sapi_Rach:
		lch_par->rach.u8Bsic = lchan->ts->trx->bts->bsic;
		break;
	case GsmL1_Sapi_Agch:
#warning Set BS_AG_BLKS_RES
		lch_par->agch.u8NbrOfAgch = 1;
		break;
	case GsmL1_Sapi_TchH:
	case GsmL1_Sapi_TchF:
		lchan2lch_par(lch_par, lchan);
		break;
	case GsmL1_Sapi_Ptcch:
		lch_par->ptcch.u8Bsic = lchan->ts->trx->bts->bsic;
		break;
	case GsmL1_Sapi_Prach:
		lch_par->prach.u8Bsic = lchan->ts->trx->bts->bsic;
		break;
	case GsmL1_Sapi_Pdtch:
	case GsmL1_Sapi_Pacch:
	case GsmL1_Sapi_Sacch:
		/*
		 * TODO: For the SACCH we need to set the u8MsPowerLevel when
		 * doing manual MS power control. */
		/*
		 * Be sure that every packet is received, even if it
		 * fails. In this case the length might be lower or 0.
		 */
		act_req->fBFILevel = -200.0;
		break;
	default:
		break;
	}

	LOGP(DL1C, LOGL_INFO, "%s MPH-ACTIVATE.req (hL2=0x%08x, %s ",
		gsm_lchan_name(lchan), act_req->hLayer2,
		get_value_string(femtobts_l1sapi_names, act_req->sapi));
	LOGPC(DL1C, LOGL_INFO, "%s)\n",
		get_value_string(femtobts_dir_names, act_req->dir));

	/* send the primitive for all GsmL1_Sapi_* that match the LCHAN */
	return l1if_gsm_req_compl(fl1h, msg, lchan_act_compl_cb);
}

static void sapi_clear_queue(struct llist_head *queue)
{
	struct sapi_cmd *next, *tmp;

	llist_for_each_entry_safe(next, tmp, queue, entry) {
		llist_del(&next->entry);
		talloc_free(next);
	}
}

static int sapi_activate_cb(struct gsm_lchan *lchan, int status)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(lchan->ts->trx);

	/* FIXME: Error handling */
	if (status != GsmL1_Status_Success) {
		lchan_set_state(lchan, LCHAN_S_BROKEN);
		sapi_clear_queue(&lchan->sapi_cmds);
		mph_info_chan_confirm(lchan, PRIM_INFO_ACTIVATE, RSL_ERR_EQUIPMENT_FAIL);
		return -1;
	}

	if (!llist_empty(&lchan->sapi_cmds))
		return 0;

	if (lchan->state != LCHAN_S_ACT_REQ)
		return 0;

	lchan_set_state(lchan, LCHAN_S_ACTIVE);
	mph_info_chan_confirm(lchan, PRIM_INFO_ACTIVATE, 0);

	/* set the initial ciphering parameters for both directions */
	l1if_set_ciphering(fl1h, lchan, 1);
	l1if_set_ciphering(fl1h, lchan, 0);
	if (lchan->encr.alg_id)
		lchan->ciph_state = LCHAN_CIPH_RXTX_REQ;
	else
		lchan->ciph_state = LCHAN_CIPH_NONE;

	return 0;
}

static void enqueue_sapi_act_cmd(struct gsm_lchan *lchan, int sapi, int dir)
{
	struct sapi_cmd *cmd = talloc_zero(lchan->ts->trx, struct sapi_cmd);

	cmd->sapi = sapi;
	cmd->dir = dir;
	cmd->type = SAPI_CMD_ACTIVATE;
	cmd->callback = sapi_activate_cb;
	queue_sapi_command(lchan, cmd);
}

int lchan_activate(struct gsm_lchan *lchan, enum gsm_lchan_state lchan_state)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(lchan->ts->trx);
	const struct lchan_sapis *s4l = &sapis_for_lchan[lchan->type];
	unsigned int i;

	lchan_set_state(lchan, lchan_state);

	if (!llist_empty(&lchan->sapi_cmds))
		LOGP(DL1C, LOGL_ERROR,
			"%s Trying to activate lchan, but commands in queue\n",
			gsm_lchan_name(lchan));

	for (i = 0; i < s4l->num_sapis; i++) {
		int sapi = s4l->sapis[i].sapi;
		int dir = s4l->sapis[i].dir;

		if (sapi == GsmL1_Sapi_Sch) {
			/* once we activate the SCH, we should get MPH-TIME.ind */
			fl1h->alive_timer.cb = alive_timer_cb;
			fl1h->alive_timer.data = fl1h;
			fl1h->alive_prim_cnt = 0;
			osmo_timer_schedule(&fl1h->alive_timer, 5, 0);
		}
		enqueue_sapi_act_cmd(lchan, sapi, dir);
	}

#warning "FIXME: Should this be in sapi_activate_cb?"
	lchan_init_lapdm(lchan);

	return 0;
}

const struct value_string femtobts_l1cfgt_names[] = {
	{ GsmL1_ConfigParamId_SetNbTsc, 	"Set NB TSC" },
	{ GsmL1_ConfigParamId_SetTxPowerLevel,	"Set Tx power level" },
	{ GsmL1_ConfigParamId_SetLogChParams,	"Set logical channel params" },
	{ GsmL1_ConfigParamId_SetCipheringParams,"Configure ciphering params" },
	{ 0, NULL }
};

static void dump_lch_par(int logl, GsmL1_LogChParam_t *lch_par, GsmL1_Sapi_t sapi)
{
	int i;

	switch (sapi) {
	case GsmL1_Sapi_Rach:
		LOGPC(DL1C, logl, "BSIC=0x%08x", lch_par->rach.u8Bsic);
		break;
	case GsmL1_Sapi_Agch:
		LOGPC(DL1C, logl, "BS_AG_BLKS_RES=%u ",
		      lch_par->agch.u8NbrOfAgch);
		break;
	case GsmL1_Sapi_Sacch:
		LOGPC(DL1C, logl, "MS Power Level 0x%02x",
			lch_par->sacch.u8MsPowerLevel);
		break;
	case GsmL1_Sapi_TchF:
	case GsmL1_Sapi_TchH:
		LOGPC(DL1C, logl, "amrCmiPhase=0x%02x amrInitCodec=0x%02x (",
			lch_par->tch.amrCmiPhase,
			lch_par->tch.amrInitCodecMode);
		for (i = 0; i < ARRAY_SIZE(lch_par->tch.amrActiveCodecSet); i++) {
			LOGPC(DL1C, logl, "%x ",
			lch_par->tch.amrActiveCodecSet[i]);
		}
		break;
	/* FIXME: PRACH / PTCCH */
	default:
		break;
	}
	LOGPC(DL1C, logl, ")\n");
}

static int chmod_txpower_compl_cb(struct gsm_bts_trx *trx, struct msgb *l1_msg)
{
	GsmL1_Prim_t *l1p = msgb_l1prim(l1_msg);
	GsmL1_MphConfigCnf_t *cc = &l1p->u.mphConfigCnf;

	LOGP(DL1C, LOGL_INFO, "%s MPH-CONFIG.conf (%s) ",
		gsm_trx_name(trx),
		get_value_string(femtobts_l1cfgt_names, cc->cfgParamId));

	LOGPC(DL1C, LOGL_INFO, "setTxPower %f dBm\n",
		cc->cfgParams.setTxPowerLevel.fTxPowerLevel);

	msgb_free(l1_msg);

	return 0;
}

static int chmod_modif_compl_cb(struct gsm_bts_trx *trx, struct msgb *l1_msg)
{
	struct gsm_lchan *lchan;
	GsmL1_Prim_t *l1p = msgb_l1prim(l1_msg);
	GsmL1_MphConfigCnf_t *cc = &l1p->u.mphConfigCnf;

	/* get the lchan from the information we supplied */
	lchan = l1if_hLayer_to_lchan(trx, cc->hLayer3);
	if (!lchan) {
		LOGP(DL1C, LOGL_ERROR,
			"Failed to find lchan for hLayer3=0x%x\n", cc->hLayer3);
		goto err;
	}

	LOGP(DL1C, LOGL_INFO, "%s MPH-CONFIG.conf (%s) ",
		gsm_lchan_name(lchan),
		get_value_string(femtobts_l1cfgt_names, cc->cfgParamId));

	switch (cc->cfgParamId) {
	case GsmL1_ConfigParamId_SetLogChParams:
		dump_lch_par(LOGL_INFO,
			     &cc->cfgParams.setLogChParams.logChParams,
			     cc->cfgParams.setLogChParams.sapi);

		sapi_queue_dispatch(lchan, cc->status);
		break;
	case GsmL1_ConfigParamId_SetCipheringParams:
		switch (lchan->ciph_state) {
		case LCHAN_CIPH_RX_REQ:
			LOGPC(DL1C, LOGL_INFO, "RX_REQ -> RX_CONF\n");
			lchan->ciph_state = LCHAN_CIPH_RX_CONF;
			break;
		case LCHAN_CIPH_RX_CONF_TX_REQ:
			LOGPC(DL1C, LOGL_INFO, "RX_CONF_TX_REQ -> RXTX_CONF\n");
			lchan->ciph_state = LCHAN_CIPH_RXTX_CONF;
			break;
		case LCHAN_CIPH_RXTX_REQ:
			LOGPC(DL1C, LOGL_INFO, "RXTX_REQ -> RX_CONF_TX_REQ\n");
			lchan->ciph_state = LCHAN_CIPH_RX_CONF_TX_REQ;
			break;
		case LCHAN_CIPH_NONE:
			LOGPC(DL1C, LOGL_INFO, "\n");
			break;
		default:
			LOGPC(DL1C, LOGL_INFO, "unhandled state %u\n", lchan->ciph_state);
			break;
		}
		if (llist_empty(&lchan->sapi_cmds)) {
			LOGP(DL1C, LOGL_ERROR,
				"%s Got ciphering conf with empty queue\n",
				gsm_lchan_name(lchan));
			goto err;
		}

		sapi_queue_dispatch(lchan, cc->status);
		break;
	case GsmL1_ConfigParamId_SetNbTsc:
	default:
		LOGPC(DL1C, LOGL_INFO, "\n");
		break;
	}

err:
	msgb_free(l1_msg);

	return 0;
}

static int mph_send_config_logchpar(struct gsm_lchan *lchan, struct sapi_cmd *cmd)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(lchan->ts->trx);
	struct msgb *msg = l1p_msgb_alloc();
	GsmL1_MphConfigReq_t *conf_req;
	GsmL1_LogChParam_t *lch_par;

	/* channel mode, encryption and/or multirate have changed */

	/* update multi-rate config */
	conf_req = prim_init(msgb_l1prim(msg), GsmL1_PrimId_MphConfigReq, fl1h);
	conf_req->cfgParamId = GsmL1_ConfigParamId_SetLogChParams;
	conf_req->cfgParams.setLogChParams.sapi = lchan_to_GsmL1_Sapi_t(lchan);
	conf_req->cfgParams.setLogChParams.u8Tn = lchan->ts->nr;
	conf_req->cfgParams.setLogChParams.subCh = lchan_to_GsmL1_SubCh_t(lchan);
	conf_req->cfgParams.setLogChParams.dir = cmd->dir;
	conf_req->hLayer3 = l1if_lchan_to_hLayer(lchan);

	lch_par = &conf_req->cfgParams.setLogChParams.logChParams;
	lchan2lch_par(lch_par, lchan);

	/* FIXME: update encryption */

	LOGP(DL1C, LOGL_INFO, "%s MPH-CONFIG.req (%s) ",
		gsm_lchan_name(lchan),
		get_value_string(femtobts_l1sapi_names,
				 conf_req->cfgParams.setLogChParams.sapi));
	LOGPC(DL1C, LOGL_INFO, "cfgParams Tn=%u, subCh=%u, dir=0x%x ",
			conf_req->cfgParams.setLogChParams.u8Tn,
			conf_req->cfgParams.setLogChParams.subCh,
			conf_req->cfgParams.setLogChParams.dir);
	dump_lch_par(LOGL_INFO,
			&conf_req->cfgParams.setLogChParams.logChParams,
			conf_req->cfgParams.setLogChParams.sapi);

	return l1if_gsm_req_compl(fl1h, msg, chmod_modif_compl_cb);
}

static void enqueue_sapi_logchpar_cmd(struct gsm_lchan *lchan, int dir)
{
	struct sapi_cmd *cmd = talloc_zero(lchan->ts->trx, struct sapi_cmd);

	cmd->dir = dir;
	cmd->type = SAPI_CMD_CONFIG_LOGCH_PARAM;
	queue_sapi_command(lchan, cmd);
}

static int tx_confreq_logchpar(struct gsm_lchan *lchan, uint8_t direction)
{
	enqueue_sapi_logchpar_cmd(lchan, direction);

	return 0;
}

int l1if_set_txpower(struct femtol1_hdl *fl1h, float tx_power)
{
	struct msgb *msg = l1p_msgb_alloc();
	GsmL1_MphConfigReq_t *conf_req;

	conf_req = prim_init(msgb_l1prim(msg), GsmL1_PrimId_MphConfigReq, fl1h);
	conf_req->cfgParamId = GsmL1_ConfigParamId_SetTxPowerLevel;
	conf_req->cfgParams.setTxPowerLevel.fTxPowerLevel = tx_power;

	return l1if_gsm_req_compl(fl1h, msg, chmod_txpower_compl_cb);
}

const enum GsmL1_CipherId_t rsl2l1_ciph[] = {
	[0]	= GsmL1_CipherId_A50,
	[1]	= GsmL1_CipherId_A50,
	[2]	= GsmL1_CipherId_A51,
	[3]	= GsmL1_CipherId_A52,
	[4]	= GsmL1_CipherId_A53,
};

static int mph_send_config_ciphering(struct gsm_lchan *lchan, struct sapi_cmd *cmd)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(lchan->ts->trx);
	struct msgb *msg = l1p_msgb_alloc();
	struct GsmL1_MphConfigReq_t *cfgr;

	cfgr = prim_init(msgb_l1prim(msg), GsmL1_PrimId_MphConfigReq, fl1h);

	cfgr->cfgParamId = GsmL1_ConfigParamId_SetCipheringParams;
	cfgr->cfgParams.setCipheringParams.u8Tn = lchan->ts->nr;
	cfgr->cfgParams.setCipheringParams.subCh = lchan_to_GsmL1_SubCh_t(lchan);
	cfgr->cfgParams.setCipheringParams.dir = cmd->dir;
	cfgr->hLayer3 = l1if_lchan_to_hLayer(lchan);

	if (lchan->encr.alg_id >= ARRAY_SIZE(rsl2l1_ciph))
		return -EINVAL;
	cfgr->cfgParams.setCipheringParams.cipherId = rsl2l1_ciph[lchan->encr.alg_id];

	LOGP(DL1C, LOGL_NOTICE, "%s SET_CIPHERING (ALG=%u %s)\n",
		gsm_lchan_name(lchan),
		cfgr->cfgParams.setCipheringParams.cipherId,
		get_value_string(femtobts_dir_names,
				 cfgr->cfgParams.setCipheringParams.dir));

	memcpy(cfgr->cfgParams.setCipheringParams.u8Kc,
	       lchan->encr.key, lchan->encr.key_len);

	return l1if_gsm_req_compl(fl1h, msg, chmod_modif_compl_cb);
}

static void enqueue_sapi_ciphering_cmd(struct gsm_lchan *lchan, int dir)
{
	struct sapi_cmd *cmd = talloc_zero(lchan->ts->trx, struct sapi_cmd);

	cmd->dir = dir;
	cmd->type = SAPI_CMD_CONFIG_CIPHERING;
	queue_sapi_command(lchan, cmd);
}

int l1if_set_ciphering(struct femtol1_hdl *fl1h,
			  struct gsm_lchan *lchan,
			  int dir_downlink)
{
	int dir;

	/* ignore the request when the channel is not active */
	if (lchan->state != LCHAN_S_ACTIVE)
		return -1;

	if (dir_downlink)
		dir = GsmL1_Dir_RxUplink;
	else
		dir = GsmL1_Dir_TxDownlink;

	enqueue_sapi_ciphering_cmd(lchan, dir);

	return 0;
}

int l1if_rsl_mode_modify(struct gsm_lchan *lchan)
{
	if (lchan->state != LCHAN_S_ACTIVE)
		return -1;

	/* channel mode, encryption and/or multirate have changed */

	/* update multi-rate config */
	tx_confreq_logchpar(lchan, GsmL1_Dir_RxUplink);
	tx_confreq_logchpar(lchan, GsmL1_Dir_TxDownlink);

	/* FIXME: update encryption */

	return 0;
}

static int lchan_deact_compl_cb(struct gsm_bts_trx *trx, struct msgb *l1_msg)
{
	enum lchan_sapi_state status;
	struct sapi_cmd *cmd;
	struct gsm_lchan *lchan;
	GsmL1_Prim_t *l1p = msgb_l1prim(l1_msg);
	GsmL1_MphDeactivateCnf_t *ic = &l1p->u.mphDeactivateCnf;

	lchan = l1if_hLayer_to_lchan(trx, ic->hLayer3);
	if (!lchan) {
		LOGP(DL1C, LOGL_ERROR,
			"Failed to find lchan for hLayer3=0x%x\n", ic->hLayer3);
		goto err;
	}

	LOGP(DL1C, LOGL_INFO, "%s MPH-DEACTIVATE.conf (%s ",
		gsm_lchan_name(lchan),
		get_value_string(femtobts_l1sapi_names, ic->sapi));
	LOGPC(DL1C, LOGL_INFO, "%s)\n",
		get_value_string(femtobts_dir_names, ic->dir));

	if (ic->status == GsmL1_Status_Success) {
		DEBUGP(DL1C, "Successful deactivation of L1 SAPI %s on TS %u\n",
			get_value_string(femtobts_l1sapi_names, ic->sapi), ic->u8Tn);
		status = LCHAN_SAPI_S_NONE;
	} else {
		LOGP(DL1C, LOGL_ERROR, "Error deactivating L1 SAPI %s on TS %u: %s\n",
			get_value_string(femtobts_l1sapi_names, ic->sapi), ic->u8Tn,
			get_value_string(femtobts_l1status_names, ic->status));
		status = LCHAN_SAPI_S_ERROR;
	}

	if (ic->dir & GsmL1_Dir_TxDownlink)
		lchan->sapis_dl[ic->sapi] = status;
	if (ic->dir & GsmL1_Dir_RxUplink)
		lchan->sapis_ul[ic->sapi] = status;


	if (llist_empty(&lchan->sapi_cmds)) {
		LOGP(DL1C, LOGL_ERROR,
				"%s Got de-activation confirmation with empty queue\n",
				gsm_lchan_name(lchan));
		goto err;
	}

	cmd = llist_entry(lchan->sapi_cmds.next, struct sapi_cmd, entry);
	if (cmd->sapi != ic->sapi || cmd->dir != ic->dir ||
			cmd->type != SAPI_CMD_DEACTIVATE) {
		LOGP(DL1C, LOGL_ERROR,
				"%s Confirmation mismatch (%d, %d) (%d, %d)\n",
				gsm_lchan_name(lchan), cmd->sapi, cmd->dir,
				ic->sapi, ic->dir);
		goto err;
	}

	sapi_queue_dispatch(lchan, ic->status);

err:
	msgb_free(l1_msg);
	return 0;
}

static int mph_send_deactivate_req(struct gsm_lchan *lchan, struct sapi_cmd *cmd)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(lchan->ts->trx);
	struct msgb *msg = l1p_msgb_alloc();
	GsmL1_MphDeactivateReq_t *deact_req;

	deact_req = prim_init(msgb_l1prim(msg), GsmL1_PrimId_MphDeactivateReq, fl1h);
	deact_req->u8Tn = lchan->ts->nr;
	deact_req->subCh = lchan_to_GsmL1_SubCh_t(lchan);
	deact_req->dir = cmd->dir;
	deact_req->sapi = cmd->sapi;
	deact_req->hLayer3 = l1if_lchan_to_hLayer(lchan);

	LOGP(DL1C, LOGL_INFO, "%s MPH-DEACTIVATE.req (%s ",
		gsm_lchan_name(lchan),
		get_value_string(femtobts_l1sapi_names, deact_req->sapi));
	LOGPC(DL1C, LOGL_INFO, "%s)\n",
		get_value_string(femtobts_dir_names, deact_req->dir));

	/* send the primitive for all GsmL1_Sapi_* that match the LCHAN */
	return l1if_gsm_req_compl(fl1h, msg, lchan_deact_compl_cb);
}

static int sapi_deactivate_cb(struct gsm_lchan *lchan, int status)
{
	/* FIXME: Error handling. There is no NACK... */
	if (status != GsmL1_Status_Success && lchan->state == LCHAN_S_REL_REQ) {
		LOGP(DL1C, LOGL_ERROR, "%s is now broken. Stopping the release.\n",
			gsm_lchan_name(lchan));
		lchan_set_state(lchan, LCHAN_S_BROKEN);
		sapi_clear_queue(&lchan->sapi_cmds);
		mph_info_chan_confirm(lchan, PRIM_INFO_DEACTIVATE, 0);
		return -1;
	}

	if (!llist_empty(&lchan->sapi_cmds))
		return 0;

	/* Don't send an REL ACK on SACCH deactivate */
	if (lchan->state != LCHAN_S_REL_REQ)
		return 0;

	lchan_set_state(lchan, LCHAN_S_NONE);
	mph_info_chan_confirm(lchan, PRIM_INFO_DEACTIVATE, 0);
	return 0;
}

static int enqueue_sapi_deact_cmd(struct gsm_lchan *lchan, int sapi, int dir)
{
	struct sapi_cmd *cmd = talloc_zero(lchan->ts->trx, struct sapi_cmd);

	cmd->sapi = sapi;
	cmd->dir = dir;
	cmd->type = SAPI_CMD_DEACTIVATE;
	cmd->callback = sapi_deactivate_cb;
	return queue_sapi_command(lchan, cmd);
}

/*
 * Release the SAPI if it was allocated. E.g. the SACCH might be already
 * deactivated or during a hand-over the TCH was not allocated yet.
 */
static int check_sapi_release(struct gsm_lchan *lchan, int sapi, int dir)
{
	/* check if we should schedule a release */
	if (dir & GsmL1_Dir_TxDownlink) {
		if (lchan->sapis_dl[sapi] != LCHAN_SAPI_S_ASSIGNED)
			return 0;
		lchan->sapis_dl[sapi] = LCHAN_SAPI_S_REL;
	} else if (dir & GsmL1_Dir_RxUplink) {
		if (lchan->sapis_ul[sapi] != LCHAN_SAPI_S_ASSIGNED)
			return 0;
		lchan->sapis_ul[sapi] = LCHAN_SAPI_S_REL;
	}

	/* now schedule the command and maybe dispatch it */
	return enqueue_sapi_deact_cmd(lchan, sapi, dir);
}


static int lchan_deactivate_sapis(struct gsm_lchan *lchan)
{
	struct femtol1_hdl *fl1h = trx_femtol1_hdl(lchan->ts->trx);
	const struct lchan_sapis *s4l = &sapis_for_lchan[lchan->type];
	int i, res;

	res = 0;

	/* The order matters.. the Facch needs to be released first */
	for (i = s4l->num_sapis-1; i >= 0; i--) {
		/* Stop the alive timer once we deactivate the SCH */
		if (s4l->sapis[i].sapi == GsmL1_Sapi_Sch)
			osmo_timer_del(&fl1h->alive_timer);

		/* Release if it was allocated */
		res |= check_sapi_release(lchan, s4l->sapis[i].sapi, s4l->sapis[i].dir);
	}

	/* nothing was queued */
	if (res == 0) {
		LOGP(DL1C, LOGL_ERROR, "%s all SAPIs already released?\n",
			gsm_lchan_name(lchan));
		lchan_set_state(lchan, LCHAN_S_BROKEN);
		mph_info_chan_confirm(lchan, PRIM_INFO_DEACTIVATE, 0);
	}

	return res;
}

static void enqueue_rel_marker(struct gsm_lchan *lchan)
{
	struct sapi_cmd *cmd;

	/* remember we need to release all active SAPIs */
	cmd = talloc_zero(lchan->ts->trx, struct sapi_cmd);
	cmd->type = SAPI_CMD_REL_MARKER;
	queue_sapi_command(lchan, cmd);
}

int lchan_deactivate(struct gsm_lchan *lchan)
{
	lchan_set_state(lchan, LCHAN_S_REL_REQ);
	lchan->ciph_state = 0; /* FIXME: do this in common/\*.c */
	enqueue_rel_marker(lchan);
	return 0;
}

static void enqueue_sacch_rel_marker(struct gsm_lchan *lchan)
{
	struct sapi_cmd *cmd;

	/* remember we need to check if the SACCH is allocated */
	cmd = talloc_zero(lchan->ts->trx, struct sapi_cmd);
	cmd->type = SAPI_CMD_SACCH_REL_MARKER;
	queue_sapi_command(lchan, cmd);
}

static int lchan_deactivate_sacch(struct gsm_lchan *lchan)
{
	enqueue_sacch_rel_marker(lchan);
	return 0;
}

/* callback from OML */
int bts_model_check_oml(struct gsm_bts *bts, uint8_t msg_type,
			struct tlv_parsed *old_attr, struct tlv_parsed *new_attr,
			void *obj)
{
	/* FIXME: check if the attributes are valid */
	return 0;
}

/* callback from OML */
int bts_model_apply_oml(struct gsm_bts *bts, struct msgb *msg,
			struct tlv_parsed *new_attr, void *obj)
{
	/* FIXME: we actaully need to send a ACK or NACK for the OML message */
	return oml_fom_ack_nack(msg, 0);
}

/* callback from OML */
int bts_model_opstart(struct gsm_bts *bts, struct gsm_abis_mo *mo,
		      void *obj)
{
	int rc;

	switch (mo->obj_class) {
	case NM_OC_RADIO_CARRIER:
		rc = trx_init(obj);
		break;
	case NM_OC_CHANNEL:
		rc = ts_connect(obj);
		break;
	case NM_OC_BTS:
	case NM_OC_SITE_MANAGER:
	case NM_OC_BASEB_TRANSC:
	case NM_OC_GPRS_NSE:
	case NM_OC_GPRS_CELL:
	case NM_OC_GPRS_NSVC:
		oml_mo_state_chg(mo, NM_OPSTATE_ENABLED, -1);
		rc = oml_mo_opstart_ack(mo);
		break;
	default:
		rc = oml_mo_opstart_nack(mo, NM_NACK_OBJCLASS_NOTSUPP);
	}
	return rc;
}

int bts_model_chg_adm_state(struct gsm_bts *bts, struct gsm_abis_mo *mo,
			    void *obj, uint8_t adm_state)
{
	/* blindly accept all state changes */
	mo->nm_state.administrative = adm_state;
	return oml_mo_statechg_ack(mo);
}

int l1if_rsl_chan_act(struct gsm_lchan *lchan)
{
	//uint8_t mode = *TLVP_VAL(tp, RSL_IE_CHAN_MODE);
	//uint8_t type = *TLVP_VAL(tp, RSL_IE_ACT_TYPE);

	lchan_activate(lchan, LCHAN_S_ACT_REQ);
	return 0;
}

int l1if_rsl_chan_rel(struct gsm_lchan *lchan)
{
	/* A duplicate RF Release Request, ignore it */
	if (lchan->state == LCHAN_S_REL_REQ)
		return 0;
	lchan_deactivate(lchan);
	return 0;
}

int l1if_rsl_deact_sacch(struct gsm_lchan *lchan)
{
	/* Only de-activate the SACCH if the lchan is active */
	if (lchan->state != LCHAN_S_ACTIVE)
		return 0;
	return lchan_deactivate_sacch(lchan);
}

int bts_model_trx_deact_rf(struct gsm_bts_trx *trx)
{
	struct femtol1_hdl *fl1 = trx_femtol1_hdl(trx);

	return l1if_activate_rf(fl1, 0);
}
