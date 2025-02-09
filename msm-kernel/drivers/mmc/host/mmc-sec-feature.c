// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Specific feature
 *
 * Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *
 * Authors:
 *      Storage Driver <storage.sec@samsung.com>
 */

#include <linux/of.h>
#include <linux/sched/clock.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/slot-gpio.h>

#include "mmc-sec-feature.h"
#include "mmc-sec-sysfs.h"

struct mmc_sd_sec_device_info sdi;

static int sd_sec_sdcard_uevent(struct device *dev,
		struct kobj_uevent_env *env)
{
	struct mmc_host *host = dev_get_drvdata(dev);
	int retval;
	bool card_exist = false;
	struct mmc_sd_sec_status_err_info *status_err = &sdi.status_err;

	add_uevent_var(env, "DEVNAME=%s", dev->kobj.name);

	if (host->card)
		card_exist = true;

	retval = add_uevent_var(env, "IOERROR=%s", card_exist ? (
			((status_err->ge_cnt && !(status_err->ge_cnt % 1000)) ||
			 (status_err->ecc_cnt && !(status_err->ecc_cnt % 1000)) ||
			 (status_err->wp_cnt && !(status_err->wp_cnt % 100)) ||
			 (status_err->oor_cnt && !(status_err->oor_cnt % 100)))
			? "YES" : "NO") : "NoCard");

	return retval;
}

static void sd_sec_sdcard_noti_work(struct work_struct *work)
{
	struct mmc_sd_sec_device_info *cdi;
	struct mmc_sd_sec_status_err_info *status_err = &sdi.status_err;
	int ret;

	cdi = container_of(work, struct mmc_sd_sec_device_info, noti_work);
	if (!cdi->mmc->card)
		return;

	status_err->noti_cnt++;
	pr_info("%s: Send notification for SD card IO error. cnt(%d)\n",
			mmc_hostname(cdi->mmc), status_err->noti_cnt);

	ret = kobject_uevent(&sec_sdcard_cmd_dev->kobj, KOBJ_CHANGE);
	if (ret)
		pr_err("%s: Failed to send uevent with err %d\n", __func__, ret);
}

static struct device_type sdcard_type = {
	.uevent = sd_sec_sdcard_uevent,
};

static void mmc_sd_sec_inc_status_err(struct mmc_card *card, u32 status)
{
	struct mmc_sd_sec_status_err_info *status_err = &sdi.status_err;
	bool noti = false;

	if (status & R1_ERROR) {
		status_err->ge_cnt++;
		if (!(status_err->ge_cnt % 1000))
			noti = true;
	}
	if (status & R1_CC_ERROR)
		status_err->cc_cnt++;
	if (status & R1_CARD_ECC_FAILED) {
		status_err->ecc_cnt++;
		if (!(status_err->ecc_cnt % 1000))
			noti = true;
	}
	if (status & R1_WP_VIOLATION) {
		status_err->wp_cnt++;
		if (!(status_err->wp_cnt % 100))
			noti = true;
	}
	if (status & R1_OUT_OF_RANGE) {
		status_err->oor_cnt++;
		if (!(status_err->oor_cnt % 100))
			noti = true;
	}
	/*
	 * Make notification for SD card errors
	 *
	 * Condition :
	 *	 GE, ECC : Every 1000 error
	 *	 WP, OOR : Every  100 error
	 */
	if (noti && mmc_card_sd(card) && sec_sdcard_cmd_dev)
		schedule_work(&sdi.noti_work);
}

static void mmc_sd_sec_inc_err_count(int index, int error, u32 status)
{
	int i = 0;
	int cpu = raw_smp_processor_id();
	struct mmc_sd_sec_err_info *err_log = &sdi.err_info[0];

	if (!error)
		return;

	/*
	 * Storage error count's policy handles only both EILSEQ and ETIMEDOUT.
	 * There is possible to detect minor error cases(e.g. ENOMEIDUM, EIO)
	 * In this case, it should be handled as -ETIMEDOUT error.
	 */
	if (error != -EILSEQ)
		error = -ETIMEDOUT;

	for (i = 0; i < MAX_ERR_TYPE_INDEX; i++) {
		if (err_log[index + i].err_type == error) {
			index += i;
			break;
		}
	}

	if (i >= MAX_ERR_TYPE_INDEX)
		return;

	/* log device status and time if this is the first error  */
	if (!err_log[index].status || !(R1_CURRENT_STATE(status) & R1_STATE_TRAN))
		err_log[index].status = status;
	if (!err_log[index].first_issue_time)
		err_log[index].first_issue_time = cpu_clock(cpu);

	err_log[index].last_issue_time = cpu_clock(cpu);
	err_log[index].count++;
}

#define MMC_BLK_TIMEOUT_MS (9 * 1000)
static bool mmc_sd_sec_check_busy_stuck(u32 status)
{
	if (time_before(jiffies,
		sdi.tstamp_last_cmd + msecs_to_jiffies(MMC_BLK_TIMEOUT_MS)))
		return false;

	if (status && (!(status & R1_READY_FOR_DATA) ||
			(R1_CURRENT_STATE(status) == R1_STATE_PRG)))
		return true;

	return false;
}

static void mmc_sd_sec_log_err_count(struct mmc_card *card,
		struct mmc_request *mrq)
{
	u32 status = (mrq->sbc ? mrq->sbc->resp[0] : 0) |
		(mrq->stop ? mrq->stop->resp[0] : 0) |
		(mrq->cmd ? mrq->cmd->resp[0] : 0);

	if (status & STATUS_MASK)
		mmc_sd_sec_inc_status_err(card, status);

	if (mrq->cmd->error)
		mmc_sd_sec_inc_err_count(SD_CMD_OFFSET,
				mrq->cmd->error, status);
	if (mrq->sbc && mrq->sbc->error)
		mmc_sd_sec_inc_err_count(SD_SBC_OFFSET,
				mrq->sbc->error, status);
	if (mrq->data && mrq->data->error)
		mmc_sd_sec_inc_err_count(SD_DATA_OFFSET,
				mrq->data->error, status);
	if (mrq->stop && mrq->stop->error)
		mmc_sd_sec_inc_err_count(SD_STOP_OFFSET,
				mrq->stop->error, status);

	/*
	 * in block.c
	 *    #define MMC_BLK_TIMEOUT_MS  (10 * 1000)
	 *    refer to card_busy_detect()
	 * so, check CMD13's response(status)
	 *    if there is no other CMD for 9 secs or more.
	 */
	if (mrq->cmd->opcode != MMC_SEND_STATUS)
		return;

	if (mmc_sd_sec_check_busy_stuck(status)) {
		/* card stuck in prg state */
		mmc_sd_sec_inc_err_count(SD_BUSY_OFFSET, -ETIMEDOUT, status);
		/* not to check card busy again */
		sdi.tstamp_last_cmd = jiffies;
	}
}

static void mmc_sd_sec_clear_err_count(void)
{
	struct mmc_sd_sec_err_info *err_log = &sdi.err_info[0];
	struct mmc_sd_sec_status_err_info *status_err = &sdi.status_err;
	int i = 0;

	for (i = 0; i < MAX_LOG_INDEX; i++) {
		err_log[i].status = 0;
		err_log[i].first_issue_time = 0;
		err_log[i].last_issue_time = 0;
		err_log[i].count = 0;
	}

	memset(status_err, 0, sizeof(struct mmc_sd_sec_status_err_info));
}

static void mmc_sd_sec_init_err_count(void)
{
	static const char *const req_types[] = {
		"sbc  ", "cmd  ", "data ", "stop ", "busy "
	};
	struct mmc_sd_sec_err_info *err_log = &sdi.err_info[0];
	int i;

	/*
	 * err_log[0].type = "sbc  "
	 * err_log[0].err_type = -EILSEQ;
	 * err_log[1].type = "sbc  "
	 * err_log[1].err_type = -ETIMEDOUT;
	 * ...
	 */
	for (i = 0; i < MAX_LOG_INDEX; i++) {
		snprintf(err_log[i].type, sizeof(char) * 5, "%s",
				req_types[i / MAX_ERR_TYPE_INDEX]);

		err_log[i].err_type =
			(i % MAX_ERR_TYPE_INDEX == 0) ?	-EILSEQ : -ETIMEDOUT;
	}
}

void sd_sec_card_event(struct mmc_host *host)
{
	bool status;

	if (!host)
		return;

	status = mmc_gpio_get_cd(host) ? true : false;

	if (status ^ sdi.tray_status) {
		pr_info("%s: slot status change detected (%d -> %d), GPIO_ACTIVE_%s\n",
				mmc_hostname(host), sdi.tray_status, status,
				(host->caps2 & MMC_CAP2_CD_ACTIVE_HIGH) ?
				"HIGH" : "LOW");
		sdi.tray_status = status;
		if (sdi.card_detect_cnt < UINT_MAX)
			sdi.card_detect_cnt++;

		host->unused = 0;
		mmc_sd_sec_clear_err_count();
	}
}

static bool mmc_sd_sec_check_cmd_type(struct mmc_request *mrq)
{
	/*
	 * cmd->flags info
	 * MMC_CMD_AC	 (0b00 << 5) : Addressed commands
	 * MMC_CMD_ADTC  (0b01 << 5) : Addressed data transfer commands
	 * MMC_CMD_BC	 (0b10 << 5) : Broadcast commands
	 * MMC_CMD_BCR	 (0b11 << 5) : Broadcast commands with response
	 *
	 * Log the errors only for AC or ADTC type
	 */
	if (!(mrq->cmd->flags & MMC_RSP_PRESENT))
		return false;

	if (mrq->cmd->flags & MMC_CMD_BC)
		return false;
	/*
	 * No need to check if MMC_RSP_136 set or cmd MMC_APP_CMD.
	 * CMD55 is sent with MMC_CMD_AC flag but no need to log.
	 */
	if ((mrq->cmd->flags & MMC_RSP_136) ||
			(mrq->cmd->opcode == MMC_APP_CMD))
		return false;

	return true;
}

void mmc_sd_sec_check_req_err(struct mmc_host *host, struct mmc_request *mrq)
{
	if (!host->card || !mrq || !mrq->cmd)
		return;

	/* return if the cmd is tuning block */
	if ((mrq->cmd->opcode == MMC_SEND_TUNING_BLOCK) ||
			(mrq->cmd->opcode == MMC_SEND_TUNING_BLOCK_HS200))
		return;

	/* set CMD(except CMD13) timestamp to check card stuck */
	if (mrq->cmd->opcode != MMC_SEND_STATUS)
		sdi.tstamp_last_cmd = jiffies;

	if (mmc_sd_sec_check_cmd_type(mrq))
		mmc_sd_sec_log_err_count(host->card, mrq);
}

void sd_sec_set_features(struct mmc_host *host, struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;

	host->caps &= ~MMC_CAP_AGGRESSIVE_PM;
	host->caps2 |= MMC_CAP2_NO_PRESCAN_POWERUP;

	sdi.tray_status = mmc_gpio_get_cd(host) ? true : false;
	/* skip init without SD card detect irq */
	host->trigger_card_event = true;

	if (of_property_read_u32(np, "sec-sd-slot-type", &sdi.sd_slot_type)) {
		if (mmc_gpio_get_cd(host) < 0)
			sdi.sd_slot_type = SEC_NO_DET_SD_SLOT;
		else
			sdi.sd_slot_type = SEC_INVALID_SD_SLOT;
	}

	sdi.card_detect_cnt = 0;

	sd_sec_init_sysfs(host);
	mmc_sd_sec_init_err_count();

	/* Register sd uevent . */
	sdi.mmc = host;
	sec_sdcard_cmd_dev->type = &sdcard_type;
	INIT_WORK(&sdi.noti_work, sd_sec_sdcard_noti_work);
}

