/* Copyright (C) 2007,2008 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the  GNU General Public License along
 * with this program; if not, write  to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#undef VERBOSE

#ifdef VERBOSE
#define VDBG(fmt, args...) pr_debug("[%s]  " fmt , \
				 __func__, ## args)
#else
#define VDBG(stuff...)	do {} while (0)
#endif

#ifdef VERBOSE
#define MPC_LOC printk("Current Location [%s]:[%d]\n", __FILE__, __LINE__)
#else
#define MPC_LOC do {} while (0)
#endif

#define PROTO_UNDEF	(0)
#define PROTO_HOST	(1)
#define PROTO_GADGET	(2)

enum otg_fsm_timer {
	A_WAIT_VRISE,
	A_WAIT_BCON,
	A_AIDL_BDIS,
	B_ASE0_BRST,
	B_SE0_SRP,
	B_SRP_FAIL,
	A_WAIT_ENUM,
	NUM_OTG_FSM_TIMERS,
};

/* OTG state machine according to the OTG spec */
struct otg_fsm {
	/* Input */
	int a_bus_resume;
	int a_bus_suspend;
	int a_conn;
	int a_sess_vld;
	int a_srp_det;
	int a_vbus_vld;
	int b_bus_resume;
	int b_bus_suspend;
	int b_conn;
	int b_se0_srp;
	int b_sess_end;
	int b_sess_vld;
	int id;

	/* Internal variables */
	int a_set_b_hnp_en;
	int b_srp_done;
	int b_hnp_enable;

	/* Timeout indicator for timers */
	int a_wait_vrise_tmout;
	int a_wait_bcon_tmout;
	int a_aidl_bdis_tmout;
	int b_ase0_brst_tmout;

	/* Informative variables */
	int a_bus_drop;
	int a_bus_req;
	int a_clr_err;
	int a_suspend_req;
	int b_bus_req;

	/* Output */
	int drv_vbus;
	int loc_conn;
	int loc_sof;

	struct otg_fsm_ops *ops;
	struct usb_otg *otg;

	/* Current usb protocol used: 0:undefine; 1:host; 2:client */
	int protocol;
	spinlock_t lock;
};

struct otg_fsm_ops {
	void	(*chrg_vbus)(struct otg_fsm *fsm, int on);
	void	(*drv_vbus)(struct otg_fsm *fsm, int on);
	void	(*loc_conn)(struct otg_fsm *fsm, int on);
	void	(*loc_sof)(struct otg_fsm *fsm, int on);
	void	(*start_pulse)(struct otg_fsm *fsm);
	void	(*add_timer)(struct otg_fsm *fsm, enum otg_fsm_timer timer);
	void	(*del_timer)(struct otg_fsm *fsm, enum otg_fsm_timer timer);
	int	(*start_host)(struct otg_fsm *fsm, int on);
	int	(*start_gadget)(struct otg_fsm *fsm, int on);
};


static inline int otg_chrg_vbus(struct otg_fsm *fsm, int on)
{
	if (!fsm->ops->chrg_vbus)
		return -EOPNOTSUPP;
	fsm->ops->chrg_vbus(fsm, on);
	return 0;
}

static inline int otg_drv_vbus(struct otg_fsm *fsm, int on)
{
	if (!fsm->ops->drv_vbus)
		return -EOPNOTSUPP;
	if (fsm->drv_vbus != on) {
		fsm->drv_vbus = on;
		fsm->ops->drv_vbus(fsm, on);
	}
	return 0;
}

static inline int otg_loc_conn(struct otg_fsm *fsm, int on)
{
	if (!fsm->ops->loc_conn)
		return -EOPNOTSUPP;
	if (fsm->loc_conn != on) {
		fsm->loc_conn = on;
		fsm->ops->loc_conn(fsm, on);
	}
	return 0;
}

static inline int otg_loc_sof(struct otg_fsm *fsm, int on)
{
	if (!fsm->ops->loc_sof)
		return -EOPNOTSUPP;
	if (fsm->loc_sof != on) {
		fsm->loc_sof = on;
		fsm->ops->loc_sof(fsm, on);
	}
	return 0;
}

static inline int otg_start_pulse(struct otg_fsm *fsm)
{
	if (!fsm->ops->start_pulse)
		return -EOPNOTSUPP;
	fsm->ops->start_pulse(fsm);
	return 0;
}

static inline int otg_add_timer(struct otg_fsm *fsm, enum otg_fsm_timer timer)
{
	if (!fsm->ops->add_timer)
		return -EOPNOTSUPP;
	fsm->ops->add_timer(fsm, timer);
	return 0;
}

static inline int otg_del_timer(struct otg_fsm *fsm, enum otg_fsm_timer timer)
{
	if (!fsm->ops->del_timer)
		return -EOPNOTSUPP;
	fsm->ops->del_timer(fsm, timer);
	return 0;
}

static inline int otg_start_host(struct otg_fsm *fsm, int on)
{
	if (!fsm->ops->start_host)
		return -EOPNOTSUPP;
	return fsm->ops->start_host(fsm, on);
}

static inline int otg_start_gadget(struct otg_fsm *fsm, int on)
{
	if (!fsm->ops->start_gadget)
		return -EOPNOTSUPP;
	return fsm->ops->start_gadget(fsm, on);
}

int otg_statemachine(struct otg_fsm *fsm);
