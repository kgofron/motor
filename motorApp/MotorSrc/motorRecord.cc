/*
FILENAME...	motorRecord.cc
USAGE...	Motor Record Support.

Version:	$Revision: 1.18 $
Modified By:	$Author: rivers $
Last Modified:	$Date: 2004-07-28 18:16:06 $
*/

/*
 *	Original Author: Jim Kowalkowski
 *	Previous Author: Tim Mooney
 *	Current Author: Ron Sluiter
 *
 *	Experimental Physics and Industrial Control System (EPICS)
 *
 *	Copyright 1995, 1996 the University of Chicago Board of Governors.
 *
 *	This software was produced under U.S. Government contract:
 *	(W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *	Developed by
 *		The Beamline Controls and Data Acquisition Group
 *		Experimental Facilities Division
 *		Advanced Photon Source
 *		Argonne National Laboratory
 *
 *	Co-developed with
 *		The Controls and Computing Group
 *		Accelerator Systems Division
 *		Advanced Photon Source
 *		Argonne National Laboratory
 *
 *
 * Modification Log:
 * -----------------
 * .01 09-17-02 rls Joe Sullivan's port to R3.14.x and OSI.
 * .02 09-30-02 rls Bug fix for another "invalid state" scenario (i.e., new
 *			target position while MIP != DONE, see README).
 * .03 10-29-02 rls - NTM field added for Soft Channel device.
 *		    - Update "last" target position in do_work() when stop
 *			command is sent.
 * .04 03-21-03 rls - Elminate three redundant DMOV monitor postings.
 *		    - Consolidate do_work() backlash correction logic.
 * .05 04-16-03 rls - Home velocity field (HVEL) added.
 * .06 06-03-03 rls - Set DBE_LOG on all calls to db_post_events().
 * .07 10-29-03 rls - If move is in the preferred direction and the backlash
 *		      speed and acceleration are the same as the slew speed and
 *		      acceleration, then skip the backlash move and go directly
 *		      to the target position.  Bug fix for doing backlash in
 *		      wrong direction when MRES < 0.
 * .08 10-31-03 rls - Fix for bug introduced with R4.5.1; record locks-up when
 *			BDST != 0, DLY != 0 and new target position before
 *			backlash correction move.
 *		    - Update readback after DLY timeout.
 * .09 11-06-03 rls - Fix backlash after jog; added more state nodes to MIP so
 *			that commands can be broken up.
 * .10 12-11-03 rls - Bug fix for tweaks ignored. When TWV < MRES and user
 *			does TWF followed by TWR; then, a single TWF
 *			followed by a single TWR appear to be ignored.
 * .11 12-12-03 rls - Changed MSTA access to bit field.
 * .12 12-12-03 rls - Added status update field (STUP).
 * .13 12-23-03 rls - Prevent STUP from activating DLY or setting DMOV true.
 * .14 02-10-03 rls - Update lval in load_pos() if FOFF is set to FROZEN.
 * .15 02-12-03 rls - Allow sign(MRES) != sign(ERES).
 * .16 06-16-04 rls - JAR validity check.
 *
 */

#define VERSION 5.4

#include	<stdlib.h>
#include	<string.h>
#include	<alarm.h>
#include	<dbDefs.h>
#include	<callback.h>
#include	<dbAccess.h>
#include	<dbScan.h>
#include	<recGbl.h>
#include	<recSup.h>
#include	<dbEvent.h>
#include	<devSup.h>
#include	<math.h>

#define GEN_SIZE_OFFSET
#include	"motorRecord.h"
#undef GEN_SIZE_OFFSET

#include	"motor.h"
#include	"epicsExport.h"


/*----------------debugging-----------------*/
volatile int motorRecordDebug = 0;
epicsExportAddress(int, motorRecordDebug);
#ifdef __GNUG__
    #define Debug(l, f, args...) {if (l <= motorRecordDebug) printf(f, ## args);}
#else
    #define Debug()
#endif


/*** Forward references ***/

static RTN_STATUS do_work(motorRecord *);
static void alarm_sub(motorRecord *);
static void monitor(motorRecord *);
static void post_MARKed_fields(motorRecord *, unsigned short);
static void process_motor_info(motorRecord *, bool);
static void load_pos(motorRecord *);
static void check_speed_and_resolution(motorRecord *);
static void set_dial_highlimit(motorRecord *, struct motor_dset *);
static void set_dial_lowlimit(motorRecord *, struct motor_dset *);
static void set_userlimits(motorRecord *);
static void range_check(motorRecord *, float *, double, double);

/*** Record Support Entry Table (RSET) functions. ***/

static long init_record(dbCommon *, int);
static long process(dbCommon *);
static long special(DBADDR *, int);
static long get_units(const DBADDR *, char *);
static long get_precision(const DBADDR *, long *);
static long get_graphic_double(const DBADDR *, struct dbr_grDouble *);
static long get_control_double(const DBADDR *, struct dbr_ctrlDouble *);
static long get_alarm_double(const DBADDR  *, struct dbr_alDouble *);


rset motorRSET =
{
    RSETNUMBER,
    NULL,
    NULL,
    (RECSUPFUN) init_record,
    (RECSUPFUN) process,
    (RECSUPFUN) special,
    NULL,
    NULL,
    NULL,
    NULL,
    (RECSUPFUN) get_units,
    (RECSUPFUN) get_precision,
    NULL,
    NULL,
    NULL,
    (RECSUPFUN) get_graphic_double,
    (RECSUPFUN) get_control_double,
    (RECSUPFUN) get_alarm_double
};
epicsExportAddress(rset, motorRSET);


/*******************************************************************************
Support for tracking the progress of motor from one invocation of 'process()'
to the next.  The field 'pmr->mip' stores the motion in progress using these
fields.  ('pmr' is a pointer to motorRecord.)
*******************************************************************************/
#define MIP_DONE	0x0000	/* No motion is in progress. */
#define MIP_JOGF	0x0001	/* A jog-forward command is in progress. */
#define MIP_JOGR	0x0002	/* A jog-reverse command is in progress. */
#define MIP_JOG_BL1	0x0004	/* Done jogging; 1st phase take out backlash. */
#define MIP_JOG		(MIP_JOGF | MIP_JOGR | MIP_JOG_BL1 | MIP_JOG_BL2)
#define MIP_HOMF	0x0008	/* A home-forward command is in progress. */
#define MIP_HOMR	0x0010	/* A home-reverse command is in progress. */
#define MIP_HOME	(MIP_HOMF | MIP_HOMR)
#define MIP_MOVE	0x0020	/* A move not resulting from Jog* or Hom*. */
#define MIP_RETRY	0x0040	/* A retry is in progress. */
#define MIP_LOAD_P	0x0080	/* A load-position command is in progress. */
#define MIP_MOVE_BL	0x0100	/* Done moving; now take out backlash. */
#define MIP_STOP	0x0200	/* We're trying to stop.  When combined with */
/*                                 MIP_JOG* or MIP_HOM*, the jog or home     */
/*                                 command is performed after motor stops    */
#define MIP_DELAY_REQ	0x0400	/* We set the delay watchdog */
#define MIP_DELAY_ACK	0x0800	/* Delay watchdog is calling us back */
#define MIP_DELAY	(MIP_DELAY_REQ | MIP_DELAY_ACK)	/* Waiting for readback
							 * to settle */
#define MIP_JOG_REQ	0x1000	/* Jog Request. */
#define MIP_JOG_STOP	0x2000	/* Stop jogging. */
#define MIP_JOG_BL2	0x4000	/* 2nd phase take out backlash. */

/*******************************************************************************
Support for keeping track of which record fields have been changed, so we can
eliminate redundant db_post_events() without having to think, and without having
to keep lots of "last value of field xxx" fields in the record.  The idea is
to say...

	MARK(M_XXXX);

when you mean...

	db_post_events(pmr, &pmr->xxxx, monitor_mask);

Before leaving, you have to call post_MARKed_fields() to actually post the
field to all listeners.  monitor() does this.

	--- NOTE WELL ---
	The macros below assume that the variable "pmr" exists and points to a
	motor record, like so:
		motorRecord *pmr;
	No check is made in this code to ensure that this really is true.
*******************************************************************************/
/* Bit field for "mmap". */
typedef union
{
    unsigned long All;
    struct
    {
	unsigned int M_VAL	:1;
	unsigned int M_DVAL	:1;
	unsigned int M_HLM	:1;
	unsigned int M_LLM	:1;
	unsigned int M_DMOV	:1;
	unsigned int M_SPMG	:1;
	unsigned int M_RCNT	:1;
	unsigned int M_MRES	:1;
	unsigned int M_ERES	:1;
	unsigned int M_UEIP	:1;
	unsigned int M_URIP	:1;
	unsigned int M_LVIO	:1;
	unsigned int M_RVAL	:1;
	unsigned int M_RLV	:1;
	unsigned int M_OFF	:1;
	unsigned int M_RBV	:1;
	unsigned int M_DHLM	:1;
	unsigned int M_DLLM	:1;
	unsigned int M_DRBV	:1;
	unsigned int M_RDBD	:1;
	unsigned int M_MOVN	:1;
	unsigned int M_HLS	:1;
	unsigned int M_LLS	:1;
	unsigned int M_RRBV	:1;
	unsigned int M_RMP	:1;
	unsigned int M_REP	:1;
	unsigned int M_MSTA	:1;
	unsigned int M_ATHM	:1;
	unsigned int M_TDIR	:1;
	unsigned int M_MIP	:1;
	unsigned int M_DIFF	:1;
	unsigned int M_RDIF	:1;
    } Bits;
} mmap_field;

/* Bit field for "nmap". */
typedef union
{
    unsigned long All;
    struct
    {
	unsigned int M_S	:1;
	unsigned int M_SBAS	:1;
	unsigned int M_SBAK	:1;
	unsigned int M_SREV	:1;
	unsigned int M_UREV	:1;
	unsigned int M_VELO	:1;
	unsigned int M_VBAS	:1;
	unsigned int M_BVEL	:1;
	unsigned int M_MISS	:1;
	unsigned int M_ACCL	:1;
	unsigned int M_BACC	:1;
	unsigned int M_STUP	:1;
    } Bits;
} nmap_field;


#define MARK(FIELD) {mmap_field temp; temp.All = pmr->mmap; \
		    temp.Bits.FIELD = 1; pmr->mmap = temp.All;}
#define MARK_AUX(FIELD) {nmap_field temp; temp.All = pmr->nmap; \
		    temp.Bits.FIELD = 1; pmr->nmap = temp.All;}

#define UNMARK(FIELD) {mmap_field temp; temp.All = pmr->mmap; \
		    temp.Bits.FIELD = 0; pmr->mmap = temp.All;}
#define UNMARK_AUX(FIELD) {nmap_field temp; temp.All = pmr->nmap; \
		    temp.Bits.FIELD = 0; pmr->nmap = temp.All;}

/*
WARNING!!! The following macros assume that a variable (i.e., mmap_bits
	and/or nmap_bits) has been declared within the scope its' occurence
	AND initialized.
*/
		
#define MARKED(FIELD) (mmap_bits.Bits.FIELD)
#define MARKED_AUX(FIELD) (nmap_bits.Bits.FIELD)

#define UNMARK_ALL	pmr->mmap = pmr->nmap = 0

/*******************************************************************************
Device support allows us to string several motor commands into a single
"transaction", using the calls prototyped below:

	int start_trans(dbCommon *mr)
	int build_trans(int command, double *parms, dbCommon *mr)
	int end_trans(struct dbCommon *mr, int go)

For clarity and to avoid typo's, the macros defined below provide simplified
calls.

		--- NOTE WELL ---
	The following macros assume that the variable "pmr" points to a motor
	record, and that the variable "pdset" points to that motor record's device
	support entry table:
		motorRecord *pmr;
		struct motor_dset *pdset = (struct motor_dset *)(pmr->dset);

	No checks are made in this code to ensure that these conditions are met.
*******************************************************************************/
/* To begin a transaction... */
#define INIT_MSG()				(*pdset->start_trans)(pmr)

/* To send a single command... */
#define WRITE_MSG(cmd,parms)	(*pdset->build_trans)((cmd), (parms), pmr)

/* To end a transaction and send accumulated commands to the motor... */
#define SEND_MSG()				(*pdset->end_trans)(pmr)


/*
The DLY feature uses the OSI facility, callbackRequestDelayed(), to issue a
callbackRequest() on the structure below.  This structure is dynamically
allocated by init_record().  init_record() saves the pointer to this structure
in the motorRecord.  See process() for use of this structure when Done Moving
field (DMOV) is TRUE.
*/

struct callback		/* DLY feature callback structure. */
{
    CALLBACK dly_callback;
    struct motorRecord *precord;
};

static void callbackFunc(struct callback *pcb)
{
    motorRecord *pmr = pcb->precord;

    /*
     * It's possible user has requested stop, or in some other way rescinded
     * the delay request that resulted in this callback.  Check to make sure
     * this callback hasn't been orphaned by events occurring between the time
     * the watchdog was started and the time this function was invoked.
     */
    if (pmr->mip & MIP_DELAY_REQ)
    {
	pmr->mip &= ~MIP_DELAY_REQ;	/* Turn off REQ. */
	pmr->mip |= MIP_DELAY_ACK;	/* Turn on ACK. */
	scanOnce(pmr);
    }
}


/******************************************************************************
	enforceMinRetryDeadband()

Calculate minumum retry deadband (.rdbd) achievable under current
circumstances, and enforce this minimum value.
Make RDBD >= MRES.
******************************************************************************/
static void enforceMinRetryDeadband(motorRecord * pmr)
{
    float min_rdbd;

    min_rdbd = fabs(pmr->mres);

    if (pmr->rdbd < min_rdbd)
    {
	pmr->rdbd = min_rdbd;
	db_post_events(pmr, &pmr->rdbd, DBE_VAL_LOG);
    }
}


/******************************************************************************
	init_record()

Called twice after an EPICS database has been loaded, and then never called
again.

LOGIC:
    IF first call (pass == 0).
	Initialize VERS field to Motor Record version number.
	NORMAL RETURN.
    ENDIF
    ...
    ...
    ...
    Initialize Limit violation field false.
    IF (Software Travel limits are NOT disabled), AND,
	    (Dial readback violates dial high limit), OR,
	    (Dial readback violates dial low limit)
	Set Limit violation field true.
    ENDIF
    ...
    Call monitor().
    NORMAL RETURN.

*******************************************************************************/

static long init_record(dbCommon* arg, int pass)
{
    motorRecord *pmr = (motorRecord *) arg;
    struct motor_dset *pdset;
    long status;
    struct callback *pcallback;	/* v3.2 */
    const char errmsg[] = "motor:init_record()";

    if (pass == 0)
    {
	pmr->vers = VERSION;
	return(OK);
    }
    /* Check that we have a device-support entry table. */
    pdset = (struct motor_dset *) pmr->dset;
    if (pdset == NULL)
    {
	recGblRecordError(S_dev_noDSET, (void *) pmr, (char *) errmsg);
	return (S_dev_noDSET);
    }
    /* Check that DSET has pointers to functions we need. */
    if ((pdset->base.number < 8) ||
	(pdset->update_values == NULL) ||
	(pdset->start_trans == NULL) ||
	(pdset->build_trans == NULL) ||
	(pdset->end_trans == NULL))
    {
	recGblRecordError(S_dev_missingSup, (void *) pmr, (char *) errmsg);
	return (S_dev_missingSup);
    }

    /*** setup callback for readback settling time delay (v3.2) ***/
    pcallback = (struct callback *) (calloc(1, sizeof(struct callback)));
    pmr->cbak = (void *) pcallback;
    callbackSetCallback((void (*)(struct callbackPvt *)) callbackFunc,
			&pcallback->dly_callback);
    callbackSetPriority(pmr->prio, &pcallback->dly_callback);
    pcallback->precord = pmr;

    /*
     * Reconcile two different ways of specifying speed and resolution; make
     * sure things are sane.
     */
    check_speed_and_resolution(pmr);

    /* Call device support to initialize itself and the driver */
    if (pdset->base.init_record)
    {
	status = (*pdset->base.init_record) (pmr);
	if (status)
	{
	    pmr->card = -1;
	    return (status);
	}
	switch (pmr->out.type)
	{
	    case (VME_IO):
		pmr->card = pmr->out.value.vmeio.card;
		break;
	    case (CONSTANT):
	    case (PV_LINK):
	    case (DB_LINK):
	    case (CA_LINK):
		pmr->card = -1;
		break;
	    default:
		recGblRecordError(S_db_badField, (void *) pmr, (char *) errmsg);
		return(ERROR);
	}
    }
    /*
     * .dol (Desired Output Location) is a struct containing either a link to
     * some other field in this database, or a constant intended to initialize
     * the .val field.  If the latter, get that initial value and apply it.
     */
    if (pmr->dol.type == CONSTANT)
    {
	pmr->udf = FALSE;
	recGblInitConstantLink(&pmr->dol, DBF_DOUBLE, &pmr->val);
    }

    /*
     * Get motor position, encoder position, status, and readback-link value by
     * calling process_motor_info().
     * 
     * v3.2 Fix so that first call to process() doesn't appear to be a callback
     * from device support.  (Reset ptrans->callback_changed to NO in devSup).
     */
    (*pdset->update_values) (pmr);

    pmr->res = pmr->mres;	/* After R4.5, RES is always = MRES. */
    if (pmr->eres == 0.0)
    {
	pmr->eres = pmr->mres;
	MARK(M_ERES);
    }

    process_motor_info(pmr, true);
    enforceMinRetryDeadband(pmr);

    /*
     * If we're in closed-loop mode, initializing the user- and dial-coordinate
     * motor positions (.val and .dval) is someone else's job. Otherwise,
     * initialize them to the readback values (.rbv and .drbv) set by our
     * recent call to process_motor_info().
     */
    if (pmr->omsl != menuOmslclosed_loop)
    {
	pmr->val = pmr->rbv;
	MARK(M_VAL);
	pmr->dval = pmr->drbv;
	MARK(M_DVAL);
	pmr->rval = NINT(pmr->dval / pmr->mres);
	MARK(M_RVAL);
    }

    /* Reset limits in case database values are invalid. */
    set_dial_highlimit(pmr, pdset);
    set_dial_lowlimit(pmr, pdset);

    /* Initialize miscellaneous control fields. */
    pmr->dmov = TRUE;
    MARK(M_DMOV);
    pmr->movn = FALSE;
    MARK(M_MOVN);
    pmr->lspg = pmr->spmg = motorSPMG_Go;
    MARK(M_SPMG);
    pmr->diff = pmr->dval - pmr->drbv;
    MARK(M_DIFF);
    pmr->rdif = NINT(pmr->diff / pmr->mres);
    MARK(M_RDIF);
    pmr->lval = pmr->val;
    pmr->ldvl = pmr->dval;
    pmr->lrvl = pmr->rval;
    pmr->lvio = 0;		/* init limit-violation field */

    if ((pmr->dhlm == pmr->dllm) && (pmr->dllm == (float) 0.0))
	;
    else if ((pmr->drbv > pmr->dhlm + pmr->mres) || (pmr->drbv < pmr->dllm - pmr->mres))
    {
	pmr->lvio = 1;
	MARK(M_LVIO);
    }

    monitor(pmr);
    return(OK);
}


/******************************************************************************
	postProcess()

Post process a command or motion after motor has stopped. We do this for
any of several reasons:
	1) This is the first call to process()
	2) User hit a "Stop" button, and motor has stopped.
	3) User released a "Jog*" button and motor has stopped.
	4) Hom* command has completed.
	5) User hit Hom* or Jog* while motor was moving, causing a
		'stop' to be sent to the motor, and the motor has stopped.
	6) User caused a new value to be written to the motor hardware's
		position register.
	7) We hit a limit switch.
LOGIC:
    Clear post process command field; PP.
    IF Output Mode Select field set to CLOSED_LOOP, AND,
       NOT a "move", AND, NOT a "backlash move".
	Make drive values agree with readback value;
	    VAL  <- RBV
	    DVAL <- DRBV
	    RVAL <- DVAL converted to motor steps.
	    DIFF <- RDIF <- 0
    ENDIF
    IF done with either load-position or load-encoder-ratio commands.
	Clear MIP.
    ELSE IF done homing.
	...
	...
    ELSE IF done stopping after jog, OR, done with move.
	IF |backlash distance| > |motor resolution|.
	    Do backlasth correction.
	ELSE
	    Set MIP to DONE.
	    IF there is a jog request and the corresponding LS is off.
		Set jog requesst on in MIP.
	    ENDIF
	ENDIF
	...
	...
    ELSE IF done with jog or move backlash.
	Clear MIP.
	IF (JOGF field true, AND, Hard High limit false), OR,
		(JOGR field true, AND, Hard Low  limit false)
	    Set Jog request state true.
	ENDIF
    ENDIF
    
    
******************************************************************************/
static long postProcess(motorRecord * pmr)
{
    struct motor_dset *pdset = (struct motor_dset *) (pmr->dset);
#ifdef DMR_SOFTMOTOR_MODS
    int dir_positive = (pmr->dir == motorDIR_Pos);
    int dir = dir_positive ? 1 : -1;
#endif

    Debug(3, "postProcess: entry\n");

    pmr->pp = FALSE;

    if (pmr->omsl != menuOmslclosed_loop && !(pmr->mip & MIP_MOVE) &&
	!(pmr->mip & MIP_MOVE_BL) && !(pmr->mip & MIP_JOG_BL1) &&
	!(pmr->mip & MIP_JOG_BL2))
    {
	/* Make drive values agree with readback value. */
#ifdef DMR_SOFTMOTOR_MODS
    	/* Mark Rivers - make val and dval agree with rrbv, rather than rbv or
    	   drbv */
	pmr->val = (pmr->rrbv * pmr->mres) * dir + pmr->off;
	pmr->dval = pmr->rrbv * pmr->mres;
#else
	pmr->val = pmr->rbv;
	pmr->dval = pmr->drbv;
#endif
	MARK(M_VAL);
	MARK(M_DVAL);
	pmr->rval = NINT(pmr->dval / pmr->mres);
	MARK(M_RVAL);
	pmr->diff = 0.;
	MARK(M_DIFF);
	pmr->rdif = 0;
	MARK(M_RDIF);
    }

    if (pmr->mip & MIP_LOAD_P)
	pmr->mip = MIP_DONE;	/* We sent LOAD_POS, followed by GET_INFO. */
    else if (pmr->mip & MIP_HOME)
    {
	/* Home command */
	if (pmr->mip & MIP_STOP)
	{
	    /* Stopped and Hom* button still down.  Now do Hom*. */
	    double vbase = pmr->vbas / fabs(pmr->mres);
	    double hpos = 0;
	    double hvel =  pmr->hvel / fabs(pmr->mres);

	    pmr->mip &= ~MIP_STOP;
	    pmr->dmov = FALSE;
	    MARK(M_DMOV);
	    pmr->rcnt = 0;
	    MARK(M_RCNT);
	    INIT_MSG();
	    WRITE_MSG(SET_VEL_BASE, &vbase);
	    WRITE_MSG(SET_VELOCITY, &hvel);
	    WRITE_MSG((pmr->mip & MIP_HOMF) ? HOME_FOR : HOME_REV, &hpos);
	    WRITE_MSG(GO, NULL);
	    SEND_MSG();
	    pmr->pp = TRUE;
	}
	else
	{
	    if (pmr->mip & MIP_HOMF)
	    {
		pmr->mip &= ~MIP_HOMF;
		pmr->homf = 0;
		db_post_events(pmr, &pmr->homf, DBE_VAL_LOG);
	    }
	    else if (pmr->mip & MIP_HOMR)
	    {
    
		pmr->mip &= ~MIP_HOMR;
		pmr->homr = 0;
		db_post_events(pmr, &pmr->homr, DBE_VAL_LOG);
	    }
	}
    }
    else if (pmr->mip & MIP_JOG_STOP || pmr->mip & MIP_MOVE)
    {
	if (fabs(pmr->bdst) >  fabs(pmr->mres))
	{
	    msta_field msta;

	    /* First part of jog done. Do backlash correction. */
	    double vbase = pmr->vbas / fabs(pmr->mres);
	    double vel = pmr->velo / fabs(pmr->mres);
	    double bpos = (pmr->dval - pmr->bdst) / pmr->mres;

	    /* Use if encoder or ReadbackLink is in use. */
	    msta.All = pmr->msta;
	    int use_rel = (msta.Bits.EA_PRESENT && pmr->ueip) || pmr->urip;
	    double relpos = pmr->diff / pmr->mres;
	    double relbpos = ((pmr->dval - pmr->bdst) - pmr->drbv) / pmr->mres;

	    /* Restore DMOV to false and UNMARK it so it is not posted. */
	    pmr->dmov = FALSE;
	    UNMARK(M_DMOV);

	    INIT_MSG();

	    if (pmr->mip & MIP_JOG_STOP)
	    {
		double acc = vel / pmr->accl;

		WRITE_MSG(SET_VEL_BASE, &vbase);
		if (vel <= vbase)
		    vel = vbase + 1;
		WRITE_MSG(SET_VELOCITY, &vel);
		WRITE_MSG(SET_ACCEL, &acc);
		if (use_rel)
		    WRITE_MSG(MOVE_REL, &relbpos);
		else
		    WRITE_MSG(MOVE_ABS, &bpos);
		pmr->mip = MIP_JOG_BL1;
	    }
	    else
	    {
		double bvel = pmr->bvel / fabs(pmr->mres);
		double bacc = bvel / pmr->bacc;

		if (bvel <= vbase)
		    bvel = vbase + 1;
		WRITE_MSG(SET_VELOCITY, &bvel);
		WRITE_MSG(SET_ACCEL, &bacc);
		if (use_rel)
		{
		    relpos = (relpos - relbpos) * pmr->frac;
		    WRITE_MSG(MOVE_REL, &relpos);
		}
		else
		{
		    double currpos = pmr->dval / pmr->mres;
		    double newpos = bpos + pmr->frac * (currpos - bpos);
		    pmr->rval = NINT(newpos);
		    WRITE_MSG(MOVE_ABS, &newpos);
		}
		pmr->mip = MIP_MOVE_BL;
	    }
	    WRITE_MSG(GO, NULL);
	    SEND_MSG();
	    pmr->pp = TRUE;
	}
	else
	{
	    pmr->mip = MIP_DONE;	/* Backup distance = 0; skip backlash. */
	    if ((pmr->jogf && !pmr->hls) || (pmr->jogr && !pmr->lls))
		pmr->mip |= MIP_JOG_REQ;
	}
	pmr->mip &= ~MIP_JOG_STOP;
	pmr->mip &= ~MIP_MOVE;
    }
    else if (pmr->mip & MIP_JOG_BL1)
    {
	msta_field msta;
	
	/* First part of jog done. Do backlash correction. */
	double bvel = pmr->bvel / fabs(pmr->mres);
	double bacc = bvel / pmr->bacc;
	double vbase = pmr->vbas / fabs(pmr->mres);
	double bpos = (pmr->dval - pmr->bdst) / pmr->mres;

	/* Use if encoder or ReadbackLink is in use. */
	msta.All = pmr->msta;
	int use_rel = (msta.Bits.EA_PRESENT && pmr->ueip) || pmr->urip;
	double relpos = pmr->diff / pmr->mres;
	double relbpos = ((pmr->dval - pmr->bdst) - pmr->drbv) / pmr->mres;

	/* Restore DMOV to false and UNMARK it so it is not posted. */
	pmr->dmov = FALSE;
	UNMARK(M_DMOV);

	INIT_MSG();

	if (bvel <= vbase)
	    bvel = vbase + 1;
	WRITE_MSG(SET_VELOCITY, &bvel);
	WRITE_MSG(SET_ACCEL, &bacc);
	if (use_rel)
	{
	    relpos = (relpos - relbpos) * pmr->frac;
	    WRITE_MSG(MOVE_REL, &relpos);
	}
	else
	{
	    double currpos = pmr->dval / pmr->mres;
	    double newpos = bpos + pmr->frac * (currpos - bpos);
	    pmr->rval = NINT(newpos);
	    WRITE_MSG(MOVE_ABS, &newpos);
	}
	WRITE_MSG(GO, NULL);
	SEND_MSG();

	pmr->mip = MIP_JOG_BL2;
	pmr->pp = TRUE;
    }
    else if (pmr->mip & MIP_JOG_BL2 || pmr->mip & MIP_MOVE_BL)
    {
	/* Completed backlash part of jog command. */
	pmr->mip = MIP_DONE;
	if ((pmr->jogf && !pmr->hls) || (pmr->jogr && !pmr->lls))
	    pmr->mip |= MIP_JOG_REQ;
    }
    /* Save old values for next call. */
    pmr->lval = pmr->val;
    pmr->ldvl = pmr->dval;
    pmr->lrvl = pmr->rval;
    pmr->mip &= ~MIP_STOP;
    MARK(M_MIP);
    return(OK);
}


/******************************************************************************
	maybeRetry()

Compare target with actual position.  If retry is indicated, set variables so
that it will happen when we return.
******************************************************************************/
static void maybeRetry(motorRecord * pmr)
{
    if ((fabs(pmr->diff) > pmr->rdbd) && !pmr->hls && !pmr->lls)
    {
	/* No, we're not close enough.  Try again. */
	Debug(1, "maybeRetry: not close enough; diff = %f\n", pmr->diff);
	/* If max retry count is zero, retry is disabled */
	if (pmr->rtry == 0)
	{
	    pmr->mip &= MIP_JOG_REQ;/* Clear everything, except jog request; for
					jog reactivation in postProcess(). */
	    MARK(M_MIP);
	}
	else
	{
	    if (++(pmr->rcnt) > pmr->rtry)
	    {
		/* Too many retries. */
		/* pmr->spmg = motorSPMG_Pause; MARK(M_SPMG); */
		pmr->mip = MIP_DONE;
		MARK(M_MIP);
		pmr->lval = pmr->val;
		pmr->ldvl = pmr->dval;
		pmr->lrvl = pmr->rval;

		/* We should probably be triggering alarms here. */
		pmr->miss = 1;
		MARK_AUX(M_MISS);
	    }
	    else
	    {
		pmr->dmov = FALSE;
		MARK(M_DMOV);
		pmr->mip = MIP_RETRY;
		MARK(M_MIP);
	    }
	    MARK(M_RCNT);
	}
    }
    else
    {
	/* Yes, we're close enough to the desired value. */
	Debug(1, "maybeRetry: close enough; diff = %f\n", pmr->diff);
	pmr->mip &= MIP_JOG_REQ;/* Clear everything, except jog request; for
				    jog reactivation in postProcess(). */
	MARK(M_MIP);
	if (pmr->miss)
	{
	    pmr->miss = 0;
	    MARK_AUX(M_MISS);
	}

	/* If motion was initiated by "Move" button, pause. */
	if (pmr->spmg == motorSPMG_Move)
	{
	    pmr->spmg = motorSPMG_Pause;
	    MARK(M_SPMG);
	}
    }
}


/******************************************************************************
	process()

Called under many different circumstances for many different reasons.

1) Someone poked our .proc field, or some other field that is marked
'process-passive' in the motorRecord.ascii file.  In this case, we
determine which fields have changed since the last time we were invoked
and attempt to act accordingly.

2) Device support will call us periodically while a motor is moving, and
once after it stops.  In these cases, we infer that device support has
called us by looking at the flag it set, report the motor's state, and
fire off readback links.  If the motor has stopped, we fire off forward links
as well.

Note that this routine handles all motor records, and that several 'copies'
of this routine may execute 'simultaneously' (in the multitasking sense), as
long as they operate on different records.  This much is normal for an EPICS
record, and the normal mechanism for ensuring that a record does not get
processed by more than one 'simultaneous' copy of this routine (the .pact field)
works here as well.

However, it is normal for an EPICS record to be either 'synchronous' (runs
to completion at every invocation of process()) or 'asynchronous' (begins
processing at one invocation and forbids all further invocations except the
callback invocation from device support that completes processing).  This
record is worse than asynchronous because we can't forbid invocations while
a motor is moving (else a motor could not be stopped), nor can we complete
processing until a motor stops.

Backlash correction would complicate this picture further, since a motor
must stop before backlash correction starts and stops it again, but device
support and the Oregon Microsystems controller allow us to string two move
commands together--even with different velocities and accelerations.

Backlash-corrected jogs (move while user holds 'jog' button down) do
complicate the picture:  we can't string the jog command together with a
backlash correction because we don't know when the user is going to release
the jog button.  Worst of all, it is possible for the user to give us a
'jog' command while the motor is moving.  Then we have to do the following
in separate invocations of process():
	tell the motor to stop
	handle motor-in-motion callbacks while the motor slows down
	recognize the stopped-motor callback and begin jogging
	handle motor-in-motion callbacks while the motor jogs
	recognize when the user releases the jog button and tell the motor to stop
	handle motor-in-motion callbacks while the motor slows down
	recognize the stopped-motor callback and begin a backlash correction
	handle motor-in-motion callbacks while the motor is moving
	recognize the stopped-motor callback and fire off forward links
For this reason, a fair amount of code is devoted to keeping track of
where the motor is in a sequence of movements that comprise a single motion.

LOGIC:
    Initialize.
    IF this record is being processed by another task (i.e., PACT != 0).
    	NORMAL RETURN.
    ENDIF
    Set Processing Active indicator field (PACT) true.
    Call device support update_values().
    IF motor status field (MSTA) was modified.
    	Mark MSTA as changed.
    ENDIF
    IF function was invoked by a callback, OR, process delay acknowledged is true?
	Set process reason indicator to CALLBACK_DATA.
	Call process_motor_info().
	IF motor-in-motion indicator (MOVN) is true.
	    IF new target position in opposite direction of current motion.
	       [Sign of RDIF is NOT the same as sign of CDIR], AND,
	       [Dist. to target {DIFF} > 2 x (|Backlash Dist.| + Retry Deadband)], AND,
	       [MIP indicates this move is either (a result of a retry),OR,
	    		(not from a Jog* or Hom*)]
		Send Stop Motor command.
		Set STOP indicator in MIP true.
		Mark MIP as changed.
	    ENDIF
	ELSE
	    Set the Done Moving field (DMOV) TRUE and mark DMOV as changed.
	    IF the High or Low limit switch is TRUE.
		Set the Post Process field to TRUE.
	    ENDIF
	    IF the Post Process field is TRUE.
		IF target position has changed (VAL != LVAL).
		    Set MIP to DONE.
		ELSE
		    Call postProcess().
		ENDIF
	    ENDIF
	    IF the Done Moving field (DMOV) is TRUE.
		Initialize delay ticks.
		IF process delay acknowledged is true, OR, ticks <= 0.
		    Clear process delay request and ack. indicators in MIP field.
		    Mark MIP as changed.
		    Call maybeRetry().
		ELSE
		    Set process delay request indicator true in MIP field.
		    Mark MIP as changed.
		    Start WatchDog?
		    Set the Done Moving field (DMOV) to FALSE.
		    Set Processing Active indicator field (PACT) false.
		    NORMAL RETURN.
		ENDIF
	    ENDIF
	ENDIF
    ENDIF
    IF Software travel limits are disabled.
	Clear Limit violation field.
    ELSE
	IF Jog indicator is true in MIP field.
	    Update Limit violation (LVIO) based on Jog direction (JOGF/JOGR) and VELO.
	ELSE IF Homing indicator is true in MIP field.
	    Update Limit violation (LVIO) based on Home direction (HOMF/HOMR) and VELO.
	ELSE
	    Update Limit violation (LVIO).
	ENDIF
    ENDIF
    IF Limit violation (LVIO) has changed.
	Mark LVIO as changed.
	IF Limit violation (LVIO) is TRUE, AND, SET is false (i.e., Use/Set is Set).
	    Set STOP field true.
	    Clear JOGF and JOGR fields.
	ENDIF
    ENDIF
    IF STOP field is true, OR,
       SPMG field Stop indicator is true, OR,
       SPMG field Pause indicator is true, OR,
       function was NOT invoked by a callback, OR,
       Done Moving field (DMOV) is TRUE, OR,
       RETRY indicator is true in MIP field.
	Call do_work().
    ENDIF
    Update Readback output link (RLNK), call dbPutLink().
    IF Done Moving field (DMOV) is TRUE.
	Process the forward-scan-link record, call recGblFwdLink().
    ENDIF
Exit:
    Update record timestamp, call recGblGetTimeStamp().
    Process alarms, call alarm_sub().
    Monitor changes to record fields, call monitor().
    Set Processing Active indicator field (PACT) false.
    Exit.

*******************************************************************************/
static long process(dbCommon *arg)
{
    motorRecord *pmr = (motorRecord *) arg;
    long status = OK, process_reason;
    int old_lvio = pmr->lvio;
    unsigned int old_msta = pmr->msta;
    struct motor_dset *pdset = (struct motor_dset *) (pmr->dset);
    struct callback *pcallback = (struct callback *) pmr->cbak;	/* v3.2 */

    if (pmr->pact)
	return(OK);

    Debug(4, "process:---------------------- begin; motor \"%s\"\n", pmr->name);
    pmr->pact = 1;

    /*** Who called us? ***/
    /*
     * Call device support to get raw motor position/status and to see whether
     * this is a callback.
     */
    process_reason = (*pdset->update_values) (pmr);
    if (pmr->msta != old_msta)
	MARK(M_MSTA);

    if ((process_reason == CALLBACK_DATA) || (pmr->mip & MIP_DELAY_ACK))
    {
	/*
	 * This is, effectively, a callback from device support: a
	 * motor-in-motion update, some asynchronous acknowledgement of a
	 * command we sent in a previous life, or a callback thay we requested
	 * to delay while readback device settled.
	 */

	/*
	 * If we were invoked by the readback-delay callback, then this is just
	 * a continuation of the device-support callback.
	 */
	process_reason = CALLBACK_DATA;

	/*
	 * Get position and status from motor controller. Get readback-link
	 * value if link exists.
	 */
	process_motor_info(pmr, false);

	if (pmr->movn)
	{
	    int sign_rdif = (pmr->rdif < 0) ? 0 : 1;

	    /* Test for new target position in opposite direction of current
	       motion.
	     */	    
	    if (pmr->ntm == menuYesNoYES &&
		(sign_rdif != pmr->cdir) &&
		(fabs(pmr->diff) > 2 * (fabs(pmr->bdst) + pmr->rdbd)) &&
		(pmr->mip == MIP_RETRY || pmr->mip == MIP_MOVE))
	    {

		/* We're going in the wrong direction. Readback problem? */
		printf("%s:tdir = %d\n", pmr->name, pmr->tdir);
		INIT_MSG();
		WRITE_MSG(STOP_AXIS, NULL);
		SEND_MSG();
		pmr->mip |= MIP_STOP;
		MARK(M_MIP);
	    }
	    status = 0;
	}
	else if (pmr->stup != motorSTUP_BUSY)
	{
	    mmap_field mmap_bits;

	    /* Motor has stopped. */
	    /* Assume we're done moving until we find out otherwise. */
	    if (pmr->dmov != TRUE)
	    {
		pmr->dmov = TRUE;
		MARK(M_DMOV);
	    }

	    /* Do another update after LS error. */
	    if (pmr->mip != MIP_DONE && (pmr->rhls || pmr->rlls))
	    {
		INIT_MSG();
		WRITE_MSG(GET_INFO, NULL);
		SEND_MSG();
		pmr->pp = TRUE;
		pmr->mip = MIP_DONE;
		MARK(M_MIP);
		goto process_exit;
	    }
	    
	    if (pmr->pp)
	    {
		if (pmr->val != pmr->lval)
		{
		    pmr->mip = MIP_DONE;
		    /* Bug fix, record locks-up when BDST != 0, DLY != 0 and
		     * new target position before backlash correction move.*/
		    goto enter_do_work;
		}
		else
		    status = postProcess(pmr);
	    }

	    /* Are we "close enough" to desired position? */
	    if (pmr->dmov && !(pmr->rhls || pmr->rlls))
	    {
		mmap_bits.All = pmr->mmap; /* Initialize for MARKED. */

		if (pmr->mip & MIP_DELAY_ACK || (pmr->dly <= 0.0))
		{
		    if (pmr->mip & MIP_DELAY_ACK && !(pmr->mip & MIP_DELAY_REQ))
		    {
			pmr->mip |= MIP_DELAY;
			INIT_MSG();
			WRITE_MSG(GET_INFO, NULL);
			SEND_MSG();
			pmr->dmov = FALSE;
			goto process_exit;
		    }
		    else if (pmr->stup != motorSTUP_ON)
		    {
			pmr->mip &= ~MIP_DELAY;
			MARK(M_MIP);	/* done delaying */
			maybeRetry(pmr);
		    }
		}
		else if (MARKED(M_DMOV) && !(pmr->mip & MIP_DELAY_REQ))
		{
		    pmr->mip |= MIP_DELAY_REQ;
		    MARK(M_MIP);

                    callbackRequestDelayed(&pcallback->dly_callback, (double) pmr->dly);

		    pmr->dmov = FALSE;
		    pmr->pact = 0;
		    goto process_exit;
		}
	    }
	}
    }	/* END of (process_reason == CALLBACK_DATA). */

enter_do_work:

    /* check for soft-limit violation */
    if ((pmr->dhlm == pmr->dllm) && (pmr->dllm == (float) 0.0))
	pmr->lvio = false;
    else
    {
	if (pmr->mip & MIP_JOG)
	    pmr->lvio = (pmr->jogf && (pmr->drbv > pmr->dhlm - pmr->velo)) ||
			(pmr->jogr && (pmr->drbv < pmr->dllm + pmr->velo));
	else if(pmr->mip & MIP_HOME)
	    pmr->lvio = (pmr->homf && (pmr->drbv > pmr->dhlm - pmr->velo)) ||
			(pmr->homr && (pmr->drbv < pmr->dllm + pmr->velo));
	else
	    pmr->lvio = (pmr->drbv > pmr->dhlm + fabs(pmr->mres)) ||
			(pmr->drbv < pmr->dllm - fabs(pmr->mres));
    }

    if (pmr->lvio != old_lvio)
    {
	MARK(M_LVIO);
	if (pmr->lvio && !pmr->set)
	{
	    pmr->stop = 1;
	    /* Clear all the buttons that cause motion. */
	    pmr->jogf = pmr->jogr = pmr->homf = pmr->homr = 0;
	}
    }
    /* Do we need to examine the record to figure out what work to perform? */
    if (pmr->stop || (pmr->spmg == motorSPMG_Stop) ||
	(pmr->spmg == motorSPMG_Pause) ||
	(process_reason != CALLBACK_DATA) || pmr->dmov || pmr->mip & MIP_RETRY)
    {
	status = do_work(pmr);
    }

    /* Fire off readback link */
    status = dbPutLink(&(pmr->rlnk), DBR_DOUBLE, &(pmr->rbv), 1);

    if (pmr->dmov)
	recGblFwdLink(pmr);	/* Process the forward-scan-link record. */
    
process_exit:
    if (process_reason == CALLBACK_DATA && pmr->stup == motorSTUP_BUSY)
    {
	pmr->stup = motorSTUP_OFF;
	MARK_AUX(M_STUP);
    }

    /*** We're done.  Report the current state of the motor. ***/
    recGblGetTimeStamp(pmr);
    alarm_sub(pmr);			/* If we've violated alarm limits, yell. */
    monitor(pmr);		/* If values have changed, broadcast them. */
    pmr->pact = 0;
    Debug(4, "process:---------------------- end; motor \"%s\"\n", pmr->name);
    return (status);
}


/******************************************************************************
	do_work()
Here, we do the real work of processing the motor record.

The equations that transform between user and dial coordinates follow.
Note: if user and dial coordinates differ in sign, we have to reverse the
sense of the limits in going between user and dial.

Dial to User:
userVAL	= DialVAL * DIR + OFFset
userHLM	= (DIR==+) ? DialHLM + OFFset : -DialLLM + OFFset
userLLM = (DIR==+) ? DialLLM + OFFset : -DialHLM + OFFset

User to Dial:
DialVAL	= (userVAL - OFFset) / DIR
DialHLM	= (DIR==+) ? userHLM - OFFset : -userLLM + OFFset
DialLLM = (DIR==+) ? userLLM - OFFset : -userHLM + OFFset

Offset:
OFFset	= userVAL - DialVAL * DIR

DEFINITIONS:
    preferred direction - the direction in which the motor moves during the
			    backlash-takeout part of a motor motion.
LOGIC:
    Initialize.

    IF Stop button activated, AND, NOT processing a STOP request.
	Set MIP field to indicate processing a STOP request.
	Mark MIP field as changed.  Set Post process command field TRUE.
	Clear Jog forward and reverse request.  Clear Stop request.
	Send STOP_AXIS message to controller.
    	NORMAL RETURN.
    ENDIF

    IF Stop/Pause/Move/Go field has changed.
        Update Last Stop/Pause/Move/Go field.
	IF SPMG field set to STOP, OR, PAUSE.
	    IF SPMG field set to STOP.
		IF MIP state is DONE, STOP or RETRY.
		    Shouldn't be moving, but send a STOP command without
			changing to the STOP state.
		    NORMAL RETURN.
		ELSE IF Motor is moving (MOVN).
		    Set Post process command TRUE.
		ELSE
		    Set VAL <- RBV and mark as changed.
		    Set DVAL <- DRBV and mark as changed.
		    Set RVAL <- RRBV and mark as changed.
		ENDIF
	    ENDIF
	    Clear any possible Home request.
	    Set MIP field to indicate processing a STOP request.
	    Mark MIP field changed.
	    Send STOP_AXIS message to controller.
	    NORMAL RETURN.
	ELSE IF SPMG field set to GO.
	    IF either JOG request is true, AND, the corresponding limit is off.		
		Set MIP to JOG request (i.e., queue jog request).
	    ELSE IF MIP state is STOP.
		Set MIP to DONE.
	    ENDIF
	ELSE
	    Clear MIP and RCNT. Mark both as changed.
	ENDIF
    ENDIF

    IF MRES, OR, ERES, OR, UEIP are marked as changed.
	IF UEIP set to YES, AND, MSTA indicates an encoder is present.
	    IF |MRES| and/or |ERES| is very near zero.
		Set MRES and/or ERES to one (1.0).
	    ENDIF
	    Set sign of ERES to same sign as MRES.
	    .....
	    .....
	ELSE
	    Set the [encoder (ticks) / motor (steps)] ratio to unity (1).
	    Set RES <- MRES.
	ENDIF
	- call enforceMinRetryDeadband().
	IF MSTA indicates an encoder is present.
	    Send the ticks/steps ratio motor command.
	ENDIF
	IF the SET position field is true.
	    Set the PP field TRUE and send the update info. motor command.
	ELSE
	    - call load_pos().
	ENDIF
	NORMAL RETURN
    ENDIF

    IF OMSL set to CLOSED_LOOP, AND, DOL type set to DB_LINK.
	Use DOL field to get DB link - call dbGetLink().
	IF error return from dbGetLink().
	    Set Undefined Link indicator (UDF) TRUE.
	    ERROR RETURN.
	ENDIF
	Set Undefined Link indicator (UDF) FALSE.
    ELSE
	IF No Limit violation, AND, (Homing forward/OR/reverse request, AND,
		NOT processing Homing forward/OR/reverse, AND, NOT At
		High/OR/Low Limit Switch)
	    IF (STOPPED, OR, PAUSED)
		Set DMOV FALSE (Home command will be processed from
		    postProcess() when SPMG is set to GO).
	    ENDIF
	    IF (Software Travel limits are NOT disabled), AND,
		(Home Forward, AND, (DVAL > DHLM - VELO)), OR,
		(Home Reverse, AND, (DVAL < DLLM + VELO)))
		Set Limit violation field true.
		NORMAL RETURN.
	    ENDIF
	    ...
	    ...
	    NORMAL RETURN.
	ENDIF
	IF NOT currently jogging, AND, NOT (STOPPED, OR, PAUSED), AND,
		No Limit violation, AND, Jog Request is true.
	    IF (Forward jog, AND, DVAL > [DHLM - VELO]), OR,
	       (Reverse jog, AND, DVAL > [DLLM + VELO])
		Set limit violation (LVIO) true.
		NORMAL RETURN.
	    ENDIF
	    Set Jogging [forward/reverse] state true.
	    ...
	    ...
	    NORMAL RETURN
	ENDIF
	IF Jog request is false, AND, jog is active.
	    Set post process TRUE.
	    Send STOP_AXIS message to controller.
	ELSE IF process jog stop or backlash.
	    NORMAL RETURN.  NOTE: Don't want "DVAL has changed..." logic to
			    get processed.
	ENDIF
    ENDIF
    
    IF VAL field has changed.
	Mark VAL changed.
	IF the SET position field is true, AND, the FOFF field is "Variable".
	    ....
	ELSE
	    Calculate DVAL based on VAL, false and DIR.
	ENDIF
    ENDIF

    IF Software travel limits are disabled.
	Set LVIO false.
    ELSE
	Update LVIO field.
    ENDIF

    IF LVIO field has changed.
        Mark LVIO field.
    ENDIF

    IF Limit violation occurred.
	Restore VAL, DVAL and RVAL to previous, valid values.
	IF MIP state is DONE
	    Set DMOV TRUE.
	ENDIF
    ENDIF

    IF Stop/Pause/Move/Go field set to STOP, OR, PAUSE.
    	NORMAL RETURN.
    ENDIF

    IF Status Update request is YES.
	Send an INFO command.
    ENDIF

    IF DVAL field has changed, OR, NOT done moving.
	Mark DVAL as changed.
	Calculate new DIFF and RDIF fields and mark as changed.
	IF the SET position field is true.
	    Load new raw motor position w/out moving it - call load_pos().
	    NORMAL RETURN.
	ELSE
	    Calculate....
	    
	    IF (UEIP set to YES, AND, MSTA indicates an encoder is present),
			OR, ReadbackLink is in use (URIP).
		Set "use relative move" indicator (use_rel) to true.
	    ELSE
		Set "use relative move" indicator (use_rel) to false.
	    ENDIF

	    IF new raw commanded position = current raw feedback position.
		IF not done moving, AND, [either no motion-in-progress, OR,
					    retry-in-progress].
		    Set done moving TRUE.
		    NORMAL RETURN.
		    NOTE: maybeRetry() can send control here even though the
			move is to the same raw position.
		ENDIF
	    ENDIF

	    Set VAL and RVAL based on DVAL; mark DVAL, VAL and RVAL for
	    dbposting.

	    IF this is not a retry.
		Reset retry counter and mark RCNT for dbposting.
	    ENDIF
	    
	    IF (relative move indicator is OFF, AND, sign of absolute move
		matches sign of backlash distance), OR, (relative move indicator
		is ON, AND, sign of relative move matches sign of backlash
		distance)
		Set preferred direction indicator ON.
	    ELSE
		Set preferred direction indicator OFF.
	    ENDIF
	    
	    IF the dial DIFF is within the retry deadband.
		IF the move is in the "preferred direction".
		    Update last target positions.
		    Terminate move. Set DMOV TRUE.
		    NORMAL RETURN.
		ENDIF		    
	    ENDIF
	    ....
	    ....
	    IF motion in progress indicator is false.
		Set MIP MOVE indicator ON.
		.....
		.....
		.....
		Send message to controller.
	    ENDIF
	ENDIF
    ENDIF

    NORMAL RETURN.
    
    
*******************************************************************************/
static RTN_STATUS do_work(motorRecord * pmr)
{
    struct motor_dset *pdset = (struct motor_dset *) (pmr->dset);
    int dir_positive = (pmr->dir == motorDIR_Pos);
    int dir = dir_positive ? 1 : -1;
    int set = pmr->set;
    bool stop_or_pause = (pmr->spmg == motorSPMG_Stop ||
			     pmr->spmg == motorSPMG_Pause) ? true : false;
    int old_lvio = pmr->lvio;
    mmap_field mmap_bits;

    Debug(3, "do_work: begin\n");
    
    /*** Process Stop/Pause/Go_Pause/Go switch. ***
    *
    * STOP	means make the motor stop and, when it does, make the drive
    *       fields (e.g., .val) agree with the readback fields (e.g., .rbv)
    *       so the motor stays stopped until somebody gives it a new place
    *       to go and sets the switch to MOVE or GO.
    *
    * PAUSE	means stop the motor like the old steppermotorRecord stops
    *       a motor:  At the next call to process() the motor will continue
    *       moving to .val.
    *
    * MOVE	means Go to .val, but then wait for another explicit Go or
    *       Go_Pause before moving the motor, even if the .dval field
    *       changes.
    *
    * GO	means Go, and then respond to any field whose change causes
    *       .dval to change as if .dval had received a dbPut().
    *       (Implicit Go, as implemented in the old steppermotorRecord.)
    *       Note that a great many fields (.val, .rvl, .off, .twf, .homf,
    *       .jogf, etc.) can make .dval change.
    */
    if (pmr->spmg != pmr->lspg || pmr->stop != 0)
    {
	bool stop = (pmr->stop != 0) ? true : false;

	if (pmr->spmg != pmr->lspg)
	    pmr->lspg = pmr->spmg;
	else
	    pmr->stop = 0;

	if (stop_or_pause == true || stop == true)
	{
	    /*
	     * If STOP, make drive values agree with readback values (when the
	     * motor actually stops).
	     */
	    if (pmr->spmg == motorSPMG_Stop || stop == true)
	    {
		if (pmr->mip == MIP_DONE || pmr->mip == MIP_STOP || pmr->mip == MIP_RETRY)
		{
		    if (pmr->mip == MIP_RETRY)
		    {
			pmr->mip = MIP_DONE;
			MARK(M_MIP);
			pmr->dmov = TRUE;
			MARK(M_DMOV);
		    }
		    /* Send message (just in case), but don't put MIP in STOP state. */
		    INIT_MSG();
		    WRITE_MSG(STOP_AXIS, NULL);
		    SEND_MSG();
		    return(OK);
		}
		else if (pmr->movn)
		{
		    pmr->pp = TRUE;	/* Do when motor stops. */
		    pmr->jogf = pmr->jogr = 0;
		}
		else
		{
		    pmr->val  = pmr->lval = pmr->rbv;
		    MARK(M_VAL);
		    pmr->dval = pmr->ldvl = pmr->drbv;
		    MARK(M_DVAL);
		    pmr->rval = pmr->lrvl = NINT(pmr->dval / pmr->mres);
		    MARK(M_RVAL);
		}
	    }
	    /* Cancel any operations. */
	    if (pmr->mip & MIP_HOMF)
	    {
		pmr->homf = 0;
		db_post_events(pmr, &pmr->homf, DBE_VAL_LOG);
	    }
	    else if (pmr->mip & MIP_HOMR)
	    {
		pmr->homr = 0;
		db_post_events(pmr, &pmr->homr, DBE_VAL_LOG);
	    }
	    pmr->mip = MIP_STOP;
	    MARK(M_MIP);
	    INIT_MSG();
	    WRITE_MSG(STOP_AXIS, NULL);
	    SEND_MSG();
	    return(OK);
	}
	else if (pmr->spmg == motorSPMG_Go)
	{
	    /* Test for "queued" jog request. */
	    if ((pmr->jogf && !pmr->hls) || (pmr->jogr && !pmr->lls))
	    {
		pmr->mip = MIP_JOG_REQ;
		MARK(M_MIP);
	    }
	    else if (pmr->mip == MIP_STOP)
	    {
		pmr->mip = MIP_DONE;
		MARK(M_MIP);
	    }
	}
	else
	{
	    pmr->mip = MIP_DONE;
	    MARK(M_MIP);
	    pmr->rcnt = 0;
	    MARK(M_RCNT);
	}
    }

    /*** Handle changes in motor/encoder resolution, and in .ueip. ***/
    mmap_bits.All = pmr->mmap; /* Initialize for MARKED. */
    if (MARKED(M_MRES) || MARKED(M_ERES) || MARKED(M_UEIP))
    {
	/* encoder pulses, motor pulses */
	double ep_mp[2];
	long m;
	msta_field msta;

	if (MARKED(M_MRES))
	    pmr->res = pmr->mres;	/* After R4.5, RES is always = MRES. */

	/* Set the encoder ratio.  Note this is blatantly device dependent. */
	msta.All = pmr->msta;
	if (msta.Bits.EA_PRESENT && pmr->ueip)
	{
	    /* defend against divide by zero */
	    if (fabs(pmr->mres) < 1.e-9)
	    {
		pmr->mres = 1.;
		MARK(M_MRES);
	    }
	    if (pmr->eres == 0.0)
	    {
		pmr->eres = pmr->mres;
		MARK(M_ERES);
	    }
	    /* Calculate encoder ratio. */
	    for (m = 10000000; (m > 1) &&
		 (fabs(m / pmr->eres) > 1.e6 || fabs(m / pmr->mres) > 1.e6); m /= 10);
	    ep_mp[0] = fabs(m / pmr->eres);
	    ep_mp[1] = fabs(m / pmr->mres);
	}
	else
	{
	    ep_mp[0] = 1.;
	    ep_mp[1] = 1.;
	}

	/* Make sure retry deadband is achievable */
	enforceMinRetryDeadband(pmr);

	if (msta.Bits.EA_PRESENT)
	{
	    INIT_MSG();
	    WRITE_MSG(SET_ENC_RATIO, ep_mp);
	    SEND_MSG();
	}
	if (pmr->set)
	{
	    pmr->pp = TRUE;
	    INIT_MSG();
	    WRITE_MSG(GET_INFO, NULL);
	    SEND_MSG();
	}
	else
	    load_pos(pmr);

	return(OK);
    }
    /*** Collect .val (User value) changes from all sources. ***/
    if (pmr->omsl == menuOmslclosed_loop && pmr->dol.type == DB_LINK)
    {
	/** If we're in CLOSED_LOOP mode, get value from input link. **/
	long status;

	status = dbGetLink(&(pmr->dol), DBR_DOUBLE, &(pmr->val), NULL, NULL);
	if (!RTN_SUCCESS(status))
	{
	    pmr->udf = TRUE;
	    return(ERROR);
	}
	pmr->udf = FALSE;
	/* Later, we'll act on this new value of .val. */
    }
    else
    {
	/** Check out all the buttons and other sources of motion **/

	/* Send motor to home switch in forward direction. */
	if (!pmr->lvio &&
	    ((pmr->homf && !(pmr->mip & MIP_HOMF) && !pmr->hls) ||
	     (pmr->homr && !(pmr->mip & MIP_HOMR) && !pmr->lls)))
	{
	    if (stop_or_pause == true)
	    {
		pmr->dmov = FALSE;
		MARK(M_DMOV);
    		return(OK);
	    }
	    /* check for limit violation */
	    if ((pmr->dhlm == pmr->dllm) && (pmr->dllm == (float) 0.0))
		;
	    else if ((pmr->homf && (pmr->dval > pmr->dhlm - pmr->velo)) ||
		     (pmr->homr && (pmr->dval < pmr->dllm + pmr->velo)))
	    {
		pmr->lvio = 1;
		MARK(M_LVIO);
		return(OK);
	    }
	    pmr->mip = pmr->homf ? MIP_HOMF : MIP_HOMR;
	    MARK(M_MIP);
	    pmr->pp = TRUE;
	    if (pmr->movn)
	    {
		pmr->mip |= MIP_STOP;
		MARK(M_MIP);
		INIT_MSG();
		WRITE_MSG(STOP_AXIS, NULL);
		SEND_MSG();
	    }
	    else
	    {
		double vbase, hvel, hpos;

		/* defend against divide by zero */
		if (pmr->eres == 0.0)
		{
		    pmr->eres = pmr->mres;
		    MARK(M_ERES);
		}

		vbase = pmr->vbas / fabs(pmr->mres);
		hvel  = pmr->hvel / fabs(pmr->mres);
		hpos = 0;

		INIT_MSG();
		WRITE_MSG(SET_VEL_BASE, &vbase);
		WRITE_MSG(SET_VELOCITY, &hvel);
		WRITE_MSG((pmr->mip & MIP_HOMF) ? HOME_FOR : HOME_REV, &hpos);
		/*
		 * WRITE_MSG(SET_VELOCITY, &hvel); WRITE_MSG(MOVE_ABS, &hpos);
		 */
		WRITE_MSG(GO, NULL);
		SEND_MSG();
		pmr->dmov = FALSE;
		MARK(M_DMOV);
		pmr->rcnt = 0;
		MARK(M_RCNT);
	    }
	    return(OK);
	}
	/*
	 * Jog motor.  Move continuously until we hit a software limit or a
	 * limit switch, or until user releases button.
	 */
	if (!(pmr->mip & MIP_JOG) && stop_or_pause == false && !pmr->lvio &&
	    (pmr->mip & MIP_JOG_REQ))
	{
	    /* check for limit violation */
	    if ((pmr->dhlm == pmr->dllm) && (pmr->dllm == (float) 0.0))
		;
	    else if ((pmr->jogf && (pmr->dval > pmr->dhlm - pmr->velo)) ||
		     (pmr->jogr && (pmr->dval < pmr->dllm + pmr->velo)))
	    {
		pmr->lvio = 1;
		MARK(M_LVIO);
		return(OK);
	    }
	    pmr->mip = pmr->jogf ? MIP_JOGF : MIP_JOGR;
	    MARK(M_MIP);
	    if (pmr->movn)
	    {
		pmr->pp = TRUE;
		pmr->mip |= MIP_STOP;
		MARK(M_MIP);
		INIT_MSG();
		WRITE_MSG(STOP_AXIS, NULL);
		SEND_MSG();
	    }
	    else
	    {
		double jogv = (pmr->jvel * dir) / pmr->mres;
		double jacc = pmr->jar / fabs(pmr->mres);

		pmr->dmov = FALSE;
		MARK(M_DMOV);
		pmr->pp = TRUE;
		if (pmr->jogf)
		    pmr->cdir = 1;
		else
		{
		    pmr->cdir = 0;
		    jogv = -jogv;
		}

		if (pmr->mres < 0.0)
		    pmr->cdir = !pmr->cdir;

		INIT_MSG();
		WRITE_MSG(SET_ACCEL, &jacc);
		WRITE_MSG(JOG, &jogv);
		SEND_MSG();
	    }
	    return(OK);
	}
	/* Stop jogging. */
	if (((pmr->mip & MIP_JOG_REQ) == 0) && 
	    ((pmr->mip & MIP_JOGF) || (pmr->mip & MIP_JOGR)))
	{
	    /* Stop motor.  When stopped, process() will correct backlash. */
	    pmr->pp = TRUE;
	    pmr->mip |= MIP_JOG_STOP;
	    pmr->mip &= ~(MIP_JOGF | MIP_JOGR);
	    INIT_MSG();
	    WRITE_MSG(STOP_AXIS, NULL);
	    SEND_MSG();
	    return(OK);
	}
	else if (pmr->mip & (MIP_JOG_STOP | MIP_JOG_BL1 | MIP_JOG_BL2))
	    return(OK);	/* Normal return if process jog stop or backlash. */

	/*
	 * Tweak motor forward (reverse).  Increment motor's position by a
	 * value stored in pmr->twv.
	 */
	if (pmr->twf || pmr->twr)
	{
	    pmr->val += pmr->twv * (pmr->twf ? 1 : -1);
	    /* Later, we'll act on this. */
	    if (pmr->twf)
		pmr->twf = 0;
	    if (pmr->twr)
		pmr->twr = 0;
	}
	/*
	 * New relative value.  Someone has poked a value into the "move
	 * relative" field (just like the .val field, but relative instead of
	 * absolute.)
	 */
	if (pmr->rlv != pmr->lrlv)
	{
	    pmr->val += pmr->rlv;
	    /* Later, we'll act on this. */
	    pmr->rlv = 0.;
	    MARK(M_RLV);
	    pmr->lrlv = pmr->rlv;
	}
	/* New raw value.  Propagate to .dval and act later. */
	if (pmr->rval != pmr->lrvl)
	    pmr->dval = pmr->rval * pmr->mres;	/* Later, we'll act on this. */
    }

    /*** Collect .dval (Dial value) changes from all sources. ***
    * Now we either act directly on the .val change and return, or we
    * propagate it into a .dval change.
    */
    if (pmr->val != pmr->lval)
    {
	MARK(M_VAL);
	if (set && !pmr->foff)
	{
	    /*
	     * Act directly on .val. and return. User wants to redefine .val
	     * without moving the motor and without making a change to .dval.
	     * Adjust the offset and recalc user limits back into agreement
	     * with dial limits.
	     */
	    pmr->off = pmr->val - pmr->dval * dir;
	    pmr->rbv = pmr->drbv * dir + pmr->off;
	    MARK(M_OFF);
	    MARK(M_RBV);

	    set_userlimits(pmr);	/* Translate dial limits to user limits. */

	    pmr->lval = pmr->val;
	    pmr->mip = MIP_DONE;
	    MARK(M_MIP);
	    pmr->dmov = TRUE;
	    MARK(M_DMOV);
	    return(OK);
	}
	else
	    /*
	     * User wants to move the motor, or to recalibrate both user and
	     * dial.  Propagate .val to .dval.
	     */
	    pmr->dval = (pmr->val - pmr->off) / dir;	/* Later we'll act on this. */	    
    }

    /* Record limit violation */
    if ((pmr->dhlm == pmr->dllm) && (pmr->dllm == (float) 0.0))
	pmr->lvio = false;
    else
	pmr->lvio = (pmr->dval > pmr->dhlm) ||
		    (pmr->dval > pmr->dhlm + pmr->bdst) ||
		    (pmr->dval < pmr->dllm) ||
		    (pmr->dval < pmr->dllm + pmr->bdst);

    if (pmr->lvio != old_lvio)
	MARK(M_LVIO);
    if (pmr->lvio)
    {
	pmr->val = pmr->lval;
	MARK(M_VAL);
	pmr->dval = pmr->ldvl;
	MARK(M_DVAL);
	pmr->rval = pmr->lrvl;
	MARK(M_RVAL);
	if (pmr->mip == MIP_DONE)
	{
	    pmr->dmov = TRUE;
	    MARK(M_DMOV);
	}
	return(OK);
    }

    if (stop_or_pause == true)
	return(OK);
    
    if (pmr->stup == motorSTUP_ON)
    {
	pmr->stup = motorSTUP_BUSY;
	MARK_AUX(M_STUP);
	INIT_MSG();
	WRITE_MSG(GET_INFO, NULL);
	SEND_MSG();
    }

    /* IF DVAL field has changed, OR, NOT done moving. */
    if (pmr->dval != pmr->ldvl || !pmr->dmov)
    {
	if (pmr->dval != pmr->ldvl)
	    MARK(M_DVAL);
	pmr->diff = pmr->dval - pmr->drbv;
	MARK(M_DIFF);
	pmr->rdif = NINT(pmr->diff / pmr->mres);
	MARK(M_RDIF);
	if (set)
	{
	    load_pos(pmr);
	    /*
	     * device support will call us back when load is done.
	     */
	    return(OK);
	}
	else
	{
	    /** Calc new raw position, and do a (backlash-corrected?) move. **/
	    double rbvpos = pmr->drbv / pmr->mres;	/* where motor is  */
	    double currpos = pmr->ldvl / pmr->mres;	/* where we are    */
	    double newpos = pmr->dval / pmr->mres;	/* where to go     */
	    double vbase = pmr->vbas / fabs(pmr->mres);	/* base speed      */
	    double vel = pmr->velo / fabs(pmr->mres);	/* normal speed    */
	    double acc = vel / pmr->accl;	/* normal accel.   */
	    /*
	     * 'bpos' is one backlash distance away from 'newpos'.
	     */
	    double bpos = (pmr->dval - pmr->bdst) / pmr->mres;
	    double bvel = pmr->bvel / fabs(pmr->mres);	/* backlash speed  */
	    double bacc = bvel / pmr->bacc;	/* backlash accel. */
	    double slop = 0.95 * pmr->rdbd;
	    bool use_rel, preferred_dir;
	    double relpos = pmr->diff / pmr->mres;
	    double relbpos = ((pmr->dval - pmr->bdst) - pmr->drbv) / pmr->mres;
	    long rpos, npos;
	    /*
	     * Relative-move target positions with motor-resolution
	     * granularity. The hardware is going to convert encoder steps to
	     * motor steps by truncating any fractional part, instead of
	     * converting to nearest integer, so we prepare for that.
	     */
	    double mRelPos = NINT(relpos) + ((relpos > 0) ? .5 : -.5);
	    double mRelBPos = NINT(relbpos) + ((relbpos > 0) ? .5 : -.5);

	    msta_field msta;
	    msta.All = pmr->msta;

	    /*** Use if encoder or ReadbackLink is in use. ***/
	    if ((msta.Bits.EA_PRESENT && pmr->ueip) || pmr->urip)
		use_rel = true;
	    else
		use_rel = false;

	    /*
	     * Post new values, recalc .val to reflect the change in .dval. (We
	     * no longer know the origin of the .dval change.  If user changed
	     * .val, we're ok as we are, but if .dval was changed directly, we
	     * must make .val agree.)
	     */
	    pmr->val = pmr->dval * dir + pmr->off;
	    if (pmr->val != pmr->lval)
		MARK(M_VAL);
	    pmr->rval = NINT(pmr->dval / pmr->mres);
	    if (pmr->rval != pmr->lrvl)
		MARK(M_RVAL);

	    rpos = NINT(rbvpos);
	    npos = NINT(newpos);
	    if (npos == rpos)
	    {
		if (pmr->dmov == FALSE && (pmr->mip == MIP_DONE || pmr->mip == MIP_RETRY))
		{
		    pmr->dmov = TRUE;
		    MARK(M_DMOV);
		    if (pmr->mip != MIP_DONE)
		    {
			pmr->mip = MIP_DONE;
			MARK(M_MIP);
		    }
		}
		/* Update previous target positions. */
		pmr->ldvl = pmr->dval;
		pmr->lval = pmr->val;
		pmr->lrvl = pmr->rval;
		return(OK);
	    }

	    /* reset retry counter if this is not a retry */
	    if ((pmr->mip & MIP_RETRY) == 0)
	    {
		pmr->rcnt = 0;
		MARK(M_RCNT);
	    }

	    if (((use_rel == false) && ((pmr->dval > pmr->ldvl) == (pmr->bdst > 0))) ||
		((use_rel == true)  && ((pmr->diff > 0)         == (pmr->bdst > 0))))
		preferred_dir = true;
	    else
		preferred_dir = false;

	    /*
	     * If we're within retry deadband, move only in preferred dir.
	     */
	    if (fabs(pmr->diff) < slop)
	    {
		if (preferred_dir == false)
		{
		    if (pmr->mip == MIP_DONE)
		    {
			pmr->ldvl = pmr->dval;
			pmr->lval = pmr->val;
			pmr->lrvl = pmr->rval;
    
			pmr->dmov = TRUE;
			MARK(M_DMOV);
		    }
		    return(OK);
		}
	    }

	    if (pmr->mip == MIP_DONE || pmr->mip == MIP_RETRY)
	    {
		double velocity, position, accel;

		pmr->mip = MIP_MOVE;
		MARK(M_MIP);
		/* v1.96 Don't post dmov if special already did. */
		if (pmr->dmov)
		{
		    pmr->dmov = FALSE;
		    MARK(M_DMOV);
		}
		pmr->ldvl = pmr->dval;
		pmr->lval = pmr->val;
		pmr->lrvl = pmr->rval;
    
		INIT_MSG();

		/* Backlash disabled, OR, no need for seperate backlash move
		 * since move is in preferred direction (preferred_dir==ON),
		 * AND, backlash acceleration and velocity are the same as slew values
		 * (BVEL == VELO, AND, BACC == ACCL). */
		if ((fabs(pmr->bdst) <  fabs(pmr->mres)) ||
		    (preferred_dir == true && pmr->bvel == pmr->velo &&
		     pmr->bacc == pmr->accl))
		{
		    velocity = vel;
		    accel = acc;
		    if (use_rel == true)
			position = mRelPos * pmr->frac;
		    else
			position = currpos + pmr->frac * (newpos - currpos);
		}
		/* Is current position within backlash or retry range? */
		else if ((fabs(pmr->diff) < slop) ||
			 (use_rel == true  && ((relbpos < 0) == (relpos > 0))) ||
			 (use_rel == false && (((currpos + slop) > bpos) == (newpos > currpos))))
		{
/******************************************************************************
 * Backlash correction imposes a much larger penalty on overshoot than on
 * undershoot. Here, we allow user to specify (by .frac) the fraction of the
 * backlash distance to move as a first approximation. When the motor stops and
 * we're not yet at 'newpos', the callback will give us another chance, and
 * we'll go .frac of the remaining distance, and so on. This algorithm is
 * essential when the drive creeps after a move (e.g., piezo inchworm), and
 * helpful when the readback device has a latency problem (e.g., interpolated
 * encoder), or is a little nonlinear. (Blatantly nonlinear readback is not
 * handled by the motor record.)
 *****************************************************************************/
		    velocity = bvel;
		    accel = bacc;
		    if (use_rel == true)
			position = mRelPos * pmr->frac;
		    else
			position = currpos + pmr->frac * (newpos - currpos);
		}
		else
		{
		    velocity = vel;
		    accel = acc;
		    if (use_rel == true)
			position = mRelBPos;
		    else
			position = bpos;
		    pmr->pp = TRUE;	/* Do backlash from posprocess(). */
		}

		pmr->cdir = (pmr->rdif < 0.0) ? 0 : 1;
		WRITE_MSG(SET_VEL_BASE, &vbase);
		WRITE_MSG(SET_VELOCITY, &velocity);
		WRITE_MSG(SET_ACCEL, &accel);
		if (use_rel == true)
		    WRITE_MSG(MOVE_REL, &position);
		else
		    WRITE_MSG(MOVE_ABS, &position);
		WRITE_MSG(GO, NULL);
		SEND_MSG();
	    }
	}
    }
    return(OK);
}


/******************************************************************************
	special()
*******************************************************************************/
static long special(DBADDR *paddr, int after)
{
    motorRecord *pmr = (motorRecord *) paddr->precord;
    struct motor_dset *pdset = (struct motor_dset *) (pmr->dset);
    int dir_positive = (pmr->dir == motorDIR_Pos);
    int dir = dir_positive ? 1 : -1;
    bool changed = false;
    int fieldIndex = dbGetFieldIndex(paddr);
    double offset, tmp_raw, tmp_limit, fabs_urev;
    RTN_STATUS rtnval;
    motor_cmnd command;
    double temp_dbl;
    float *temp_flt;
    msta_field msta;

    msta.All = pmr->msta;

    Debug(3, "special: after = %d\n", after);

    /*
     * Someone wrote to drive field.  Blink .dmov unless record is disabled.
     */
    if (after == 0)
    {
	switch (fieldIndex)
	{
	    case motorRecordVAL:
	    case motorRecordDVAL:
	    case motorRecordRVAL:
	    case motorRecordRLV:
		if (pmr->disa == pmr->disv || pmr->disp)
		    return(OK);
		pmr->dmov = FALSE;
		db_post_events(pmr, &pmr->dmov, DBE_VAL_LOG);
		return(OK);

	    case motorRecordHOMF:
	    case motorRecordHOMR:
		if (pmr->mip & MIP_HOME)
		    return(ERROR);	/* Prevent record processing. */
		break;
	    case motorRecordSTUP:
		if (pmr->stup != motorSTUP_OFF)
		    return(ERROR);	/* Prevent record processing. */
	}
	return(OK);
    }

    fabs_urev = fabs(pmr->urev);

    switch (fieldIndex)
    {
	/* new vbas: make sbas agree */
    case motorRecordVBAS:
	if (pmr->vbas < 0.0)
	{
	    pmr->vbas = 0.0;	    
	    db_post_events(pmr, &pmr->vbas, DBE_VAL_LOG);
	}

	if ((pmr->urev != 0.0) && (pmr->sbas != (temp_dbl = pmr->vbas / fabs_urev)))
	{
	    pmr->sbas = temp_dbl;
	    db_post_events(pmr, &pmr->sbas, DBE_VAL_LOG);
	}
	break;

	/* new sbas: make vbas agree */
    case motorRecordSBAS:
	if (pmr->sbas < 0.0)
	{
	    pmr->sbas = 0.0;
	    db_post_events(pmr, &pmr->sbas, DBE_VAL_LOG);
	}

	if (pmr->vbas != (temp_dbl = fabs_urev * pmr->sbas))
	{
	    pmr->vbas = temp_dbl;
	    db_post_events(pmr, &pmr->vbas, DBE_VAL_LOG);
	}
	break;

	/* new vmax: make smax agree */
    case motorRecordVMAX:
	if (pmr->vmax < 0.0)
	{
	    pmr->vmax = 0.0;
	    db_post_events(pmr, &pmr->vmax, DBE_VAL_LOG);
	}

	if ((pmr->urev != 0.0) && (pmr->smax != (temp_dbl = pmr->vmax / fabs_urev)))
	{
	    pmr->smax = temp_dbl;
	    db_post_events(pmr, &pmr->smax, DBE_VAL_LOG);
	}
	break;

	/* new smax: make vmax agree */
    case motorRecordSMAX:
	if (pmr->smax < 0.0)
	{
	    pmr->smax = 0.0;
	    db_post_events(pmr, &pmr->smax, DBE_VAL_LOG);
	}

	if (pmr->vmax != (temp_dbl = fabs_urev * pmr->smax))
	{
	    pmr->vmax = temp_dbl;
	    db_post_events(pmr, &pmr->vmax, DBE_VAL_LOG);
	}
	break;

	/* new velo: make s agree */
    case motorRecordVELO:
	range_check(pmr, &pmr->velo, pmr->vbas, pmr->vmax);

	if ((pmr->urev != 0.0) && (pmr->s != (temp_dbl = pmr->velo / fabs_urev)))
	{
	    pmr->s = temp_dbl;
	    db_post_events(pmr, &pmr->s, DBE_VAL_LOG);
	}
	break;

	/* new s: make velo agree */
    case motorRecordS:
	range_check(pmr, &pmr->s, pmr->sbas, pmr->smax);

	if (pmr->velo != (temp_dbl = fabs_urev * pmr->s))
	{
	    pmr->velo = temp_dbl;
	    db_post_events(pmr, &pmr->velo, DBE_VAL_LOG);
	}
	break;

	/* new bvel: make sbak agree */
    case motorRecordBVEL:
	range_check(pmr, &pmr->bvel, pmr->vbas, pmr->vmax);

	if ((pmr->urev != 0.0) && (pmr->sbak != (temp_dbl = pmr->bvel / fabs_urev)))
	{
	    pmr->sbak = temp_dbl;
	    db_post_events(pmr, &pmr->sbak, DBE_VAL_LOG);
	}
	break;

	/* new sbak: make bvel agree */
    case motorRecordSBAK:
	range_check(pmr, &pmr->sbak, pmr->sbas, pmr->smax);

	if (pmr->bvel != (temp_dbl = fabs_urev * pmr->sbak))
	{
	    pmr->bvel = temp_dbl;
	    db_post_events(pmr, &pmr->bvel, DBE_VAL_LOG);
	}
	break;

	/* new accl */
    case motorRecordACCL:
	if (pmr->accl <= 0.0)
	{
	    pmr->accl = 0.1;
	    db_post_events(pmr, &pmr->accl, DBE_VAL_LOG);
	}
	break;

	/* new bacc */
    case motorRecordBACC:
	if (pmr->bacc <= 0.0)
	{
	    pmr->bacc = 0.1;
	    db_post_events(pmr, &pmr->bacc, DBE_VAL_LOG);
	}
	break;

	/* new rdbd */
    case motorRecordRDBD:
	enforceMinRetryDeadband(pmr);
	break;

	/* new dir */
    case motorRecordDIR:
	if (pmr->foff)
	{
	    pmr->val = pmr->dval * dir + pmr->off;
	    MARK(M_VAL);
	}
	else
	{
	    pmr->off = pmr->val - pmr->dval * dir;
	    MARK(M_OFF);
	}
	pmr->rbv = pmr->drbv * dir + pmr->off;
	MARK(M_RBV);
	set_userlimits(pmr);	/* Translate dial limits to user limits. */
	break;

	/* new offset */
    case motorRecordOFF:
	pmr->val = pmr->dval * dir + pmr->off;
	pmr->lval = pmr->ldvl * dir + pmr->off;
	pmr->rbv = pmr->drbv * dir + pmr->off;
	MARK(M_VAL);
	MARK(M_RBV);
	set_userlimits(pmr);	/* Translate dial limits to user limits. */
	break;

	/* new user high limit */
    case motorRecordHLM:
	offset = pmr->off;
	if (dir_positive)
	{
	    command = SET_HIGH_LIMIT;
	    tmp_limit = pmr->hlm - offset;
	    MARK(M_DHLM);
	}
	else
	{
	    command = SET_LOW_LIMIT;
	    tmp_limit = -(pmr->hlm) + offset;
	    MARK(M_DLLM);
	}

	tmp_raw = tmp_limit / pmr->mres;

	INIT_MSG();
	rtnval = (*pdset->build_trans)(command, &tmp_raw, pmr);
	if (rtnval != OK)
	{
	    /* If an error occured, build_trans() has reset
	     * dial high or low limit to controller's value. */

	    if (dir_positive)
		pmr->hlm = pmr->dhlm + offset;
	    else
		pmr->hlm = -(pmr->dllm) + offset;
	}
	else
	{
	    SEND_MSG();
	    if (dir_positive)
		pmr->dhlm = tmp_limit;
	    else
		pmr->dllm = tmp_limit;
	}
	MARK(M_HLM);
	break;

	/* new user low limit */
    case motorRecordLLM:
	offset = pmr->off;
	if (dir_positive)
	{
	    command = SET_LOW_LIMIT;
	    tmp_limit = pmr->llm - offset;
	    MARK(M_DLLM);
	}
	else
	{
	    command = SET_HIGH_LIMIT;
	    tmp_limit = -(pmr->llm) + offset;
	    MARK(M_DHLM);
	}

	tmp_raw = tmp_limit / pmr->mres;

	INIT_MSG();
	rtnval = (*pdset->build_trans)(command, &tmp_raw, pmr);
	if (rtnval != OK)
	{
	    /* If an error occured, build_trans() has reset
	     * dial high or low limit to controller's value. */

	    if (dir_positive)
		pmr->llm = pmr->dllm + offset;
	    else
		pmr->llm = -(pmr->dhlm) + offset;
	}
	else
	{
	    SEND_MSG();
	    if (dir_positive)
		pmr->dllm = tmp_limit;
	    else
		pmr->dhlm = tmp_limit;
	}
	MARK(M_LLM);
	break;

	/* new dial high limit */
    case motorRecordDHLM:
	set_dial_highlimit(pmr, pdset);
	break;

	/* new dial low limit */
    case motorRecordDLLM:
	set_dial_lowlimit(pmr, pdset);
	break;

	/* new frac (move fraction) */
    case motorRecordFRAC:
	/* enforce limit */
	if (pmr->frac < 0.1)
	{
	    pmr->frac = 0.1;
	    changed = true;
	}
	if (pmr->frac > 1.5)
	{
	    pmr->frac = 1.5;
	    changed = true;
	}
	if (changed == true)
	    db_post_events(pmr, &pmr->frac, DBE_VAL_LOG);
	break;

	/* new mres: make urev agree, and change (velo,bvel,vbas) to leave */
	/* (s,sbak,sbas) constant */
    case motorRecordMRES:
	MARK(M_MRES);		/* MARK it so we'll remember to tell device
				 * support */
	if (pmr->urev != (temp_dbl = pmr->mres * pmr->srev))
	{
	    pmr->urev = temp_dbl;
	    fabs_urev = fabs(pmr->urev);	/* Update local |UREV|. */
	    MARK_AUX(M_UREV);
	}
	goto velcheckB;

	/* new urev: make mres agree, and change (velo,bvel,vbas) to leave */
	/* (s,sbak,sbas) constant */

    case motorRecordUREV:
	if (pmr->mres != (temp_dbl = pmr->urev / pmr->srev))
	{
	    pmr->mres = temp_dbl;
	    MARK(M_MRES);
	}

velcheckB:
	if (pmr->velo != (temp_dbl = fabs_urev * pmr->s))
	{
	    pmr->velo = temp_dbl;
	    MARK_AUX(M_VELO);
	}
	if (pmr->vbas != (temp_dbl = fabs_urev * pmr->sbas))
	{
	    pmr->vbas = temp_dbl;
	    MARK_AUX(M_VBAS);
	}
	if (pmr->bvel != (temp_dbl = fabs_urev * pmr->sbak))
	{
	    pmr->bvel = temp_dbl;
	    MARK_AUX(M_BVEL);
	}
	if (pmr->vmax != (temp_dbl = fabs_urev * pmr->smax))
	{
	    pmr->vmax = temp_dbl;
	    db_post_events(pmr, &pmr->vmax, DBE_VAL_LOG);
	}
	break;

	/* new srev: make mres agree */
    case motorRecordSREV:
	if (pmr->srev <= 0)
	{
	    pmr->srev = 200;
	    MARK_AUX(M_SREV);
	}
	if (pmr->mres != pmr->urev / pmr->srev)
	{
	    pmr->mres = pmr->urev / pmr->srev;
	    MARK(M_MRES);
	}
	break;

	/* new eres (encoder resolution) */
    case motorRecordERES:
	if (pmr->eres == 0.0)	/* Don't allow ERES = 0. */
    	    pmr->eres = pmr->mres;
	MARK(M_ERES);
	break;

	/* new ueip flag */
    case motorRecordUEIP:
	MARK(M_UEIP);
	/* Ideally, we should be recalculating speeds, but at the moment */
	/* we don't know whether hardware even has an encoder. */
	break;

	/* new urip flag */
    case motorRecordURIP:
	break;

	/* Set to SET mode  */
    case motorRecordSSET:
	pmr->set = 1;
	db_post_events(pmr, &pmr->set, DBE_VAL_LOG);
	break;

	/* Set to USE mode  */
    case motorRecordSUSE:
	pmr->set = 0;
	db_post_events(pmr, &pmr->set, DBE_VAL_LOG);
	break;

	/* Set freeze-offset to freeze mode */
    case motorRecordFOF:
	pmr->foff = 1;
	db_post_events(pmr, &pmr->foff, DBE_VAL_LOG);
	break;

	/* Set freeze-offset to variable mode */
    case motorRecordVOF:
	pmr->foff = 0;
	db_post_events(pmr, &pmr->foff, DBE_VAL_LOG);
	break;

	/* New backlash distance.  Make sure retry deadband is achievable. */
    case motorRecordBDST:
	enforceMinRetryDeadband(pmr);
	break;

    case motorRecordPCOF:
	temp_flt = &pmr->pcof;
	command = SET_PGAIN;
	goto pidcof;
    case motorRecordICOF:
	temp_flt = &pmr->icof;
	command = SET_IGAIN;
	goto pidcof;
    case motorRecordDCOF:
	temp_flt = &pmr->dcof;
	command = SET_DGAIN;
pidcof:
	if (msta.Bits.GAIN_SUPPORT != 0)
	{
	    if (*temp_flt < 0.0)	/* Validity check;  0.0 <= gain <= 1.0 */
	    {
		*temp_flt = 0.0;
		changed = true;
	    }
	    else if (*temp_flt > 1.0)
	    {
		*temp_flt = 1.0;
		changed = true;
	    }

	    temp_dbl = *temp_flt;

	    INIT_MSG();
	    rtnval = (*pdset->build_trans)(command, &temp_dbl, pmr);
            /* If an error occured, build_trans() has reset the gain
	     * parameter to a valid value for this controller. */
	    if (rtnval != OK)
		changed = true;

	    SEND_MSG();
	    if (changed == 1)
		db_post_events(pmr, temp_flt, DBE_VAL_LOG);
	}
	break;

    case motorRecordCNEN:
	if (msta.Bits.GAIN_SUPPORT != 0)
	{
	    double tempdbl;

	    INIT_MSG();
	    tempdbl = pmr->cnen;
	    if (pmr->cnen != 0)
		WRITE_MSG(ENABLE_TORQUE, &tempdbl);
	    else
		WRITE_MSG(DISABL_TORQUE, &tempdbl);
	    SEND_MSG();
	}

    case motorRecordJOGF:
	if (pmr->jogf == 0)
	    pmr->mip &= ~MIP_JOG_REQ;
	else if (pmr->mip == MIP_DONE && !pmr->hls)
	    pmr->mip |= MIP_JOG_REQ;
	break;

    case motorRecordJOGR:
	if (pmr->jogr == 0)
	    pmr->mip &= ~MIP_JOG_REQ;
	else if (pmr->mip == MIP_DONE && !pmr->lls)
	    pmr->mip |= MIP_JOG_REQ;
	break;

    case motorRecordJVEL:
	range_check(pmr, &pmr->jvel, pmr->vbas, pmr->vmax);

	if ((pmr->mip & MIP_JOGF) || (pmr->mip & MIP_JOGR))
	{
	    double jogv = (pmr->jvel * dir) / pmr->mres;
	    double jacc = pmr->jar / fabs(pmr->mres);

	    if (pmr->jogr)
		jogv = -jogv;

	    INIT_MSG();
	    WRITE_MSG(SET_ACCEL, &jacc);
	    WRITE_MSG(JOG_VELOCITY, &jogv);
	    SEND_MSG();
	}
	break;

    case motorRecordJAR:
	// Valid JAR; 0 < JAR < JVEL [egu / sec] / 0.1 [sec]
	if (pmr->jar <= 0.0)
	    pmr->jar = pmr->jvel / 0.1;
	break;

    case motorRecordHVEL:
	range_check(pmr, &pmr->hvel, pmr->vbas, pmr->vmax);
	break;

    case motorRecordSTUP:
	if (pmr->stup != motorSTUP_ON)
	{
	    pmr->stup = motorSTUP_OFF;
	    db_post_events(pmr, &pmr->stup, DBE_VAL_LOG);
	    return(ERROR);	/* Prevent record processing. */
	}
	break;

    default:
	break;
    }

    switch (fieldIndex)	/* Re-check slew (VBAS) and backlash (VBAS) velocities. */
    {
	case motorRecordVMAX:
	case motorRecordSMAX:
	    if (pmr->vmax != 0.0 && pmr->vmax < pmr->vbas)
	    {
		pmr->vbas = pmr->vmax;
		MARK_AUX(M_VBAS);
		pmr->sbas = pmr->smax;
		MARK_AUX(M_SBAS);
	    }
	    goto velcheckA;

	case motorRecordVBAS:
	case motorRecordSBAS:
	    if (pmr->vmax != 0.0 && pmr->vbas > pmr->vmax)
	    {
		pmr->vmax = pmr->vbas;
		db_post_events(pmr, &pmr->vmax, DBE_VAL_LOG);
		pmr->smax = pmr->sbas;
		db_post_events(pmr, &pmr->smax, DBE_VAL_LOG);
	    }
velcheckA:
	    range_check(pmr, &pmr->velo, pmr->vbas, pmr->vmax);
    
	    if ((pmr->urev != 0.0) && (pmr->s != (temp_dbl = pmr->velo / fabs_urev)))
	    {
		pmr->s = temp_dbl;
		db_post_events(pmr, &pmr->s, DBE_VAL_LOG);
	    }
    
	    range_check(pmr, &pmr->bvel, pmr->vbas, pmr->vmax);
    
	    if ((pmr->urev != 0.0) && (pmr->sbak != (temp_dbl = pmr->bvel / fabs_urev)))
	    {
		pmr->sbak = temp_dbl;
		db_post_events(pmr, &pmr->sbak, DBE_VAL_LOG);
	    }

	    range_check(pmr, &pmr->jvel, pmr->vbas, pmr->vmax);
	    range_check(pmr, &pmr->hvel, pmr->vbas, pmr->vmax);
    }
    /* Do not process (i.e., clear) marked fields here.  PP fields (e.g., MRES) must remain marked. */
    return(OK);
}


/******************************************************************************
	get_units()
*******************************************************************************/
static long get_units(const DBADDR *paddr, char *units)
{
    motorRecord *pmr = (motorRecord *) paddr->precord;
    int siz = dbr_units_size - 1;	/* "dbr_units_size" from dbAccess.h */
    char s[30];
    int fieldIndex = dbGetFieldIndex(paddr);

    switch (fieldIndex)
    {

    case motorRecordVELO:
    case motorRecordBVEL:
    case motorRecordVBAS:
	strncpy(s, pmr->egu, DB_UNITS_SIZE);
	strcat(s, "/sec");
	break;

    case motorRecordACCL:
    case motorRecordBACC:
	strcpy(s, "sec");
	break;

    case motorRecordS:
    case motorRecordSBAS:
    case motorRecordSBAK:
	strcpy(s, "rev/sec");
	break;

    case motorRecordSREV:
	strcpy(s, "steps/rev");
	break;

    case motorRecordUREV:
	strncpy(s, pmr->egu, DB_UNITS_SIZE);
	strcat(s, "/rev");
	break;

    default:
	strncpy(s, pmr->egu, DB_UNITS_SIZE);
	break;
    }
    s[siz] = '\0';
    strncpy(units, s, siz + 1);
    return (0);
}

/******************************************************************************
	get_graphic_double()
*******************************************************************************/
static long get_graphic_double(const DBADDR *paddr, struct dbr_grDouble * pgd)
{
    motorRecord *pmr = (motorRecord *) paddr->precord;
    int fieldIndex = dbGetFieldIndex(paddr);

    switch (fieldIndex)
    {

    case motorRecordVAL:
    case motorRecordRBV:
	pgd->upper_disp_limit = pmr->hlm;
	pgd->lower_disp_limit = pmr->llm;
	break;

    case motorRecordDVAL:
    case motorRecordDRBV:
	pgd->upper_disp_limit = pmr->dhlm;
	pgd->lower_disp_limit = pmr->dllm;
	break;

    case motorRecordRVAL:
    case motorRecordRRBV:
	if (pmr->mres >= 0)
	{
	    pgd->upper_disp_limit = pmr->dhlm / pmr->mres;
	    pgd->lower_disp_limit = pmr->dllm / pmr->mres;
	}
	else
	{
	    pgd->upper_disp_limit = pmr->dllm / pmr->mres;
	    pgd->lower_disp_limit = pmr->dhlm / pmr->mres;
	}
	break;

    default:
	recGblGetGraphicDouble((dbAddr *) paddr, pgd);
	break;
    }

    return (0);
}

/******************************************************************************
	get_control_double()
*******************************************************************************/
static long
 get_control_double(const DBADDR *paddr, struct dbr_ctrlDouble * pcd)
{
    motorRecord *pmr = (motorRecord *) paddr->precord;
    int fieldIndex = dbGetFieldIndex(paddr);

    switch (fieldIndex)
    {

    case motorRecordVAL:
    case motorRecordRBV:
	pcd->upper_ctrl_limit = pmr->hlm;
	pcd->lower_ctrl_limit = pmr->llm;
	break;

    case motorRecordDVAL:
    case motorRecordDRBV:
	pcd->upper_ctrl_limit = pmr->dhlm;
	pcd->lower_ctrl_limit = pmr->dllm;
	break;

    case motorRecordRVAL:
    case motorRecordRRBV:
	if (pmr->mres >= 0)
	{
	    pcd->upper_ctrl_limit = pmr->dhlm / pmr->mres;
	    pcd->lower_ctrl_limit = pmr->dllm / pmr->mres;
	}
	else
	{
	    pcd->upper_ctrl_limit = pmr->dllm / pmr->mres;
	    pcd->lower_ctrl_limit = pmr->dhlm / pmr->mres;
	}
	break;

    default:
	recGblGetControlDouble((dbAddr *) paddr, pcd);
	break;
    }
    return (0);
}

/******************************************************************************
	get_precision()
*******************************************************************************/
static long get_precision(const DBADDR *paddr, long *precision)
{
    motorRecord *pmr = (motorRecord *) paddr->precord;
    int fieldIndex = dbGetFieldIndex(paddr);

    *precision = pmr->prec;
    switch (fieldIndex)
    {

    case motorRecordRRBV:
    case motorRecordRMP:
    case motorRecordREP:
	*precision = 0;
	break;

    case motorRecordVERS:
	*precision = 2;
	break;

    default:
	recGblGetPrec((dbAddr *) paddr, precision);
	break;
    }
    return (0);
}



/******************************************************************************
	get_alarm_double()
*******************************************************************************/
static long get_alarm_double(const DBADDR  *paddr, struct dbr_alDouble * pad)
{
    motorRecord *pmr = (motorRecord *) paddr->precord;
    int fieldIndex = dbGetFieldIndex(paddr);

    if (fieldIndex == motorRecordVAL || fieldIndex == motorRecordDVAL)
    {
	pad->upper_alarm_limit = pmr->hihi;
	pad->upper_warning_limit = pmr->high;
	pad->lower_warning_limit = pmr->low;
	pad->lower_alarm_limit = pmr->lolo;
    }
    else
    {
	recGblGetAlarmDouble((dbAddr *) paddr, pad);
    }
    return (0);
}


/******************************************************************************
	alarm_sub()
*******************************************************************************/
static void alarm_sub(motorRecord * pmr)
{
    msta_field msta;

    if (pmr->udf == TRUE)
    {
	recGblSetSevr((dbCommon *) pmr, UDF_ALARM, INVALID_ALARM);
	return;
    }
    /* limit-switch and soft-limit violations */
    if (pmr->hlsv && (pmr->hls || (pmr->dval > pmr->dhlm)))
    {
	recGblSetSevr((dbCommon *) pmr, HIGH_ALARM, pmr->hlsv);
	return;
    }
    if (pmr->hlsv && (pmr->lls || (pmr->dval < pmr->dllm)))
    {
	recGblSetSevr((dbCommon *) pmr, LOW_ALARM, pmr->hlsv);
	return;
    }
    
    msta.All = pmr->msta;

    if (msta.Bits.CNTRL_COMM_ERR != 0)
    {
	msta.Bits.CNTRL_COMM_ERR =  0;
	pmr->msta = msta.All;
	MARK(M_MSTA);
	recGblSetSevr((dbCommon *) pmr, COMM_ALARM, INVALID_ALARM);
    }
    return;
}


/******************************************************************************
	monitor()
*******************************************************************************/
static void monitor(motorRecord * pmr)
{
    unsigned short monitor_mask;

    monitor_mask = recGblResetAlarms(pmr);

    /* Catch all previous 'calls' to MARK(). */
    post_MARKed_fields(pmr, monitor_mask);
    return;
}


/******************************************************************************
	post_MARKed_fields()
*******************************************************************************/
static void post_MARKed_fields(motorRecord * pmr, unsigned short mask)
{
    unsigned short local_mask;
    mmap_field mmap_bits;
    nmap_field nmap_bits;
    msta_field msta;
    
    mmap_bits.All = pmr->mmap; /* Initialize for MARKED. */
    nmap_bits.All = pmr->nmap; /* Initialize for MARKED_AUX. */
    
    msta.All = pmr->msta;

    if ((local_mask = mask | (MARKED(M_RBV) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->rbv, local_mask);
	UNMARK(M_RBV);
    }
    
    if ((local_mask = mask | (MARKED(M_RRBV) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->rrbv, local_mask);
	UNMARK(M_RRBV);
    }
    
    if ((local_mask = mask | (MARKED(M_DRBV) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->drbv, local_mask);
	UNMARK(M_DRBV);
    }
    
    if ((local_mask = mask | (MARKED(M_RMP) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->rmp, local_mask);
	UNMARK(M_RMP);
    }
    
    if ((local_mask = mask | (MARKED(M_REP) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->rep, local_mask);
	UNMARK(M_REP);
    }
    
    if ((local_mask = mask | (MARKED(M_DIFF) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->diff, local_mask);
	UNMARK(M_DIFF);
    }
    
    if ((local_mask = mask | (MARKED(M_RDIF) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->rdif, local_mask);
	UNMARK(M_RDIF);
    }
    
    if ((local_mask = mask | (MARKED(M_MSTA) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->msta, local_mask);
	UNMARK(M_MSTA);
	if (msta.Bits.GAIN_SUPPORT)
	{
	    unsigned short pos_maint = (msta.Bits.EA_POSITION) ? 1 : 0;
	    if (pos_maint != pmr->cnen)
	    {
		pmr->cnen = pos_maint;
		db_post_events(pmr, &pmr->cnen, local_mask);
	    }
	}
    }

    if ((pmr->mmap == 0) && (pmr->nmap == 0))
	return;

    /* short circuit: less frequently posted PV's go below this line. */
    mmap_bits.All = pmr->mmap; /* Initialize for MARKED. */
    nmap_bits.All = pmr->nmap; /* Initialize for MARKED_AUX. */

    if ((local_mask = mask | (MARKED(M_VAL) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->val, local_mask);
    if ((local_mask = mask | (MARKED(M_DVAL) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->dval, local_mask);
    if ((local_mask = mask | (MARKED(M_RVAL) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->rval, local_mask);
    if ((local_mask = mask | (MARKED(M_TDIR) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->tdir, local_mask);
    if ((local_mask = mask | (MARKED(M_MIP) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->mip, local_mask);
    if ((local_mask = mask | (MARKED(M_HLM) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->hlm, local_mask);
    if ((local_mask = mask | (MARKED(M_LLM) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->llm, local_mask);
    if ((local_mask = mask | (MARKED(M_SPMG) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->spmg, local_mask);
    if ((local_mask = mask | (MARKED(M_RCNT) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->rcnt, local_mask);
    if ((local_mask = mask | (MARKED(M_RLV) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->rlv, local_mask);
    if ((local_mask = mask | (MARKED(M_OFF) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->off, local_mask);
    if ((local_mask = mask | (MARKED(M_DHLM) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->dhlm, local_mask);
    if ((local_mask = mask | (MARKED(M_DLLM) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->dllm, local_mask);
    if ((local_mask = mask | (MARKED(M_HLS) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->hls, local_mask);
	if ((pmr->dir == motorDIR_Pos) == (pmr->mres >= 0))
	    db_post_events(pmr, &pmr->rhls, local_mask);
	else
	    db_post_events(pmr, &pmr->rlls, local_mask);
    }
    if ((local_mask = mask | (MARKED(M_LLS) ? DBE_VAL_LOG : 0)))
    {
	db_post_events(pmr, &pmr->lls, local_mask);
	if ((pmr->dir == motorDIR_Pos) == (pmr->mres >= 0))
	    db_post_events(pmr, &pmr->rlls, local_mask);
	else
	    db_post_events(pmr, &pmr->rhls, local_mask);
    }
    if ((local_mask = mask | (MARKED(M_ATHM) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->athm, local_mask);
    if ((local_mask = mask | (MARKED(M_MRES) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->mres, local_mask);
    if ((local_mask = mask | (MARKED(M_ERES) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->eres, local_mask);
    if ((local_mask = mask | (MARKED(M_UEIP) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->ueip, local_mask);
    if ((local_mask = mask | (MARKED(M_URIP) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->urip, local_mask);
    if ((local_mask = mask | (MARKED(M_LVIO) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->lvio, local_mask);
    if ((local_mask = mask | (MARKED(M_RDBD) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->rdbd, local_mask);

    if ((local_mask = mask | (MARKED_AUX(M_S) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->s, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_SBAS) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->sbas, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_SBAK) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->sbak, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_SREV) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->srev, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_UREV) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->urev, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_VELO) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->velo, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_VBAS) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->vbas, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_BVEL) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->bvel, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_MISS) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->miss, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_ACCL) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->accl, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_BACC) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->bacc, local_mask);
    if ((local_mask = mask | (MARKED(M_MOVN) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->movn, local_mask);
    if ((local_mask = mask | (MARKED(M_DMOV) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->dmov, local_mask);
    if ((local_mask = mask | (MARKED_AUX(M_STUP) ? DBE_VAL_LOG : 0)))
	db_post_events(pmr, &pmr->stup, local_mask);

    UNMARK_ALL;
}


/******************************************************************************
	process_motor_info()
*******************************************************************************/
static void
 process_motor_info(motorRecord * pmr, bool initcall)
{
    double old_drbv = pmr->drbv;
    double old_rbv = pmr->rbv;
    long old_rrbv = pmr->rrbv;
    short old_tdir = pmr->tdir;
    short old_movn = pmr->movn;
    short old_hls = pmr->hls;
    short old_lls = pmr->lls;
    short old_athm = pmr->athm;
    int dir = (pmr->dir == motorDIR_Pos) ? 1 : -1;
    bool ls_active;
    msta_field msta;

    /*** Process record fields. ***/

    /* Calculate raw and dial readback values. */
    msta.All = pmr->msta;
    if (msta.Bits.EA_PRESENT && pmr->ueip)
    {
	/* An encoder is present and the user wants us to use it. */
	pmr->rrbv = pmr->rep;
	pmr->drbv = pmr->rrbv * pmr->eres;
    }
    else
    {
	pmr->rrbv = pmr->rmp;
	pmr->drbv = pmr->rrbv * pmr->mres;
    }

    MARK(M_RMP);
    MARK(M_REP);
    if (pmr->rrbv != old_rrbv)
	MARK(M_RRBV);
    if (pmr->drbv != old_drbv)
	MARK(M_DRBV);

    /* Calculate user readback value. */
    pmr->rbv = dir * pmr->drbv + pmr->off;
    if (pmr->rbv != old_rbv)
	MARK(M_RBV);

    /* Get current or most recent direction. */
    pmr->tdir = (msta.Bits.RA_DIRECTION) ? 1 : 0;
    if (pmr->tdir != old_tdir)
	MARK(M_TDIR);

    /* Get states of high, low limit switches. */
    pmr->rhls = (msta.Bits.RA_PLUS_LS)  &&  pmr->cdir;
    pmr->rlls = (msta.Bits.RA_MINUS_LS) && !pmr->cdir;

    ls_active = (pmr->rhls || pmr->rlls) ? true : false;
    
    pmr->hls = ((pmr->dir == motorDIR_Pos) == (pmr->mres >= 0)) ? pmr->rhls : pmr->rlls;
    pmr->lls = ((pmr->dir == motorDIR_Pos) == (pmr->mres >= 0)) ? pmr->rlls : pmr->rhls;
    if (pmr->hls != old_hls)
	MARK(M_HLS);
    if (pmr->lls != old_lls)
	MARK(M_LLS);

    /* Get motor-now-moving indicator. */
    if (ls_active == true || msta.Bits.RA_DONE || msta.Bits.RA_PROBLEM)
	pmr->movn = 0;
    else
	pmr->movn = 1;
    if (pmr->movn != old_movn)
	MARK(M_MOVN);
    
    /* Get state of motor's or encoder's home switch. */
    if (msta.Bits.EA_PRESENT && pmr->ueip)
	pmr->athm = (msta.Bits.EA_HOME) ? 1 : 0;
    else
	pmr->athm = (msta.Bits.RA_HOME) ? 1 : 0;

    if (pmr->athm != old_athm)
	MARK(M_ATHM);


    /*
     * If we've got an external readback device, get Dial readback from it, and
     * propagate to User readback. We do this after motor and encoder readbacks
     * have been read and propagated to .rbv in case .rdbl is a link involving
     * that field.
     */
    if (pmr->urip && initcall == false)
    {
	long rtnstat;

	old_drbv = pmr->drbv;
	rtnstat = dbGetLink(&(pmr->rdbl), DBR_DOUBLE, &(pmr->drbv), NULL, NULL);
	if (!RTN_SUCCESS(rtnstat))
	    pmr->drbv = old_drbv;
	else
	{
	    pmr->drbv *= pmr->rres;
	    pmr->rbv = pmr->drbv * dir + pmr->off;
	    if (pmr->drbv != old_drbv)
	    {
		MARK(M_DRBV);
		MARK(M_RBV);
	    }
	}
    }
    pmr->diff = pmr->dval - pmr->drbv;
    MARK(M_DIFF);
    pmr->rdif = NINT(pmr->diff / pmr->mres);
    MARK(M_RDIF);
}

/* Calc and load new raw position into motor w/out moving it. */
static void load_pos(motorRecord * pmr)
{
    struct motor_dset *pdset = (struct motor_dset *) (pmr->dset);
    double newpos = pmr->dval / pmr->mres;

    pmr->ldvl = pmr->dval;
    pmr->lval = pmr->val;
    pmr->lrvl = pmr->rval = (long) newpos;

    if (pmr->foff)
    {
	/* Translate dial value to user value. */
	if (pmr->dir == motorDIR_Pos)
	    pmr->val = pmr->off + pmr->dval;
	else
	    pmr->val = pmr->off - pmr->dval;
	MARK(M_VAL);
	pmr->lval = pmr->val;
    }
    else
    {
	/* Translate dial limits to user limits. */
	if (pmr->dir == motorDIR_Pos)
	    pmr->off = pmr->val - pmr->dval;
	else
	    pmr->off = pmr->val + pmr->dval;
	MARK(M_OFF);
	set_userlimits(pmr);	/* Translate dial limits to user limits. */
    }
    pmr->mip = MIP_LOAD_P;
    MARK(M_MIP);
    pmr->pp = TRUE;
    if (pmr->dmov == TRUE)
    {
	pmr->dmov = FALSE;
	MARK(M_DMOV);
    }

    /* Load pos. into motor controller.  Get new readback vals. */
    INIT_MSG();
    WRITE_MSG(LOAD_POS, &newpos);
    SEND_MSG();
    INIT_MSG();
    WRITE_MSG(GET_INFO, NULL);
    SEND_MSG();
}

/*
 * FUNCTION... static void check_speed_and_resolution(motorRecord *)
 *
 * INPUT ARGUMENTS...
 *	1 - motor record pointer
 *
 * RETRUN ARGUMENTS... None.
 *
 * LOGIC...
 *
 *  IF SREV negative.
 *	Set SREV <- 200.
 *  ENDIF
 *  IF UREV nonzero.
 *	Set MRES < - |UREV| / SREV.
 *  ENDIF
 *  IF MRES zero.
 *	Set MRES <- 1.0
 *  ENDIF
 *  IF UREV does not match MRES.
 *	Set UREV <- MRES * SREV.
 *  ENDIF
 *
 *  IF SMAX > 0.
 *	Set VMAX <- SMAX * |UREV|.
 *  ELSE IF VMAX > 0.
 *	Set SMAX <- VMAX / |UREV|.
 *  ELSE
 *	Set both SMAX and VMAX to zero.
 *  ENDIF
 *
 *  IF SBAS is nonzero.
 *	Range check; 0 < SBAS < SMAX.
 *	Set VBAS <- SBAS * |UREV|.
 *  ELSE
 *	Range check; 0 < VBAS < VMAX.
 *	Set SBAS <- VBAS / |UREV|.
 *  ENDIF
 *
 *  IF S is nonzero.
 *	Range check; SBAS < S < SMAX.
 *	VELO <- S * |UREV|.
 *  ELSE
 *	Range check; VBAS < VELO < VMAX.
 *	S < - VELO / |UREV|.
 *  ENDIF
 *
 *  IF SBAK is nonzero.
 *	Range check; SBAS < SBAK < SMAX.
 *	BVEL <- SBAK * |UREV|.
 *  ELSE
 *	Range check; VBAS < BVEL < VMAX.
 *	SBAK <- BVEL / |UREV|.
 *  ENDIF
 *
 *  IF ACCL or BACC is zero.
 *	Set ACCL/BACC to 0.1
 *  ENDIF
 *
 *  NORMAL RETURN.
 */

static void check_speed_and_resolution(motorRecord * pmr)
{
    double fabs_urev = fabs(pmr->urev);

    /*
     * Reconcile two different ways of specifying speed, resolution, and make
     * sure things are sane.
     */

    /* SREV (steps/revolution) must be sane. */
    if (pmr->srev <= 0)
    {
	pmr->srev = 200;
	MARK_AUX(M_SREV);
    }

    /* UREV (EGU/revolution) <--> MRES (EGU/step) */
    if (pmr->urev != 0.0)
    {
	pmr->mres = pmr->urev / pmr->srev;
	MARK(M_MRES);
    }
    if (pmr->mres == 0.0)
    {
	pmr->mres = 1.0;
	MARK(M_MRES);
    }
    if (pmr->urev != pmr->mres * pmr->srev)
    {
	pmr->urev = pmr->mres * pmr->srev;
        fabs_urev = fabs(pmr->urev);	/* Update local |UREV|. */
	MARK_AUX(M_UREV);
    }

    /* SMAX (revolutions/sec) <--> VMAX (EGU/sec) */
    if (pmr->smax > 0.0)
	pmr->vmax = pmr->smax * fabs_urev;
    else if (pmr->vmax > 0.0)
	pmr->smax = pmr->vmax / fabs_urev;
    else
	pmr->smax = pmr->vmax = 0.0;
    db_post_events(pmr, &pmr->vmax, DBE_VAL_LOG);
    db_post_events(pmr, &pmr->smax, DBE_VAL_LOG);

    /* SBAS (revolutions/sec) <--> VBAS (EGU/sec) */
    if (pmr->sbas != 0.0)
    {
	range_check(pmr, &pmr->sbas, 0.0, pmr->smax);
	pmr->vbas = pmr->sbas * fabs_urev;
    }
    else
    {
	range_check(pmr, &pmr->vbas, 0.0, pmr->vmax);
	pmr->sbas = pmr->vbas / fabs_urev;
    }
    db_post_events(pmr, &pmr->vbas, DBE_VAL_LOG);
    db_post_events(pmr, &pmr->sbas, DBE_VAL_LOG);

    
    /* S (revolutions/sec) <--> VELO (EGU/sec) */
    if (pmr->s != 0.0)
    {
	range_check(pmr, &pmr->s, pmr->sbas, pmr->smax);
	pmr->velo = pmr->s * fabs_urev;
    }
    else
    {
	range_check(pmr, &pmr->velo, pmr->vbas, pmr->vmax);
	pmr->s = pmr->velo / fabs_urev;
    }
    db_post_events(pmr, &pmr->velo, DBE_VAL_LOG);
    db_post_events(pmr, &pmr->s, DBE_VAL_LOG);

    /* SBAK (revolutions/sec) <--> BVEL (EGU/sec) */
    if (pmr->sbak != 0.0)
    {
	range_check(pmr, &pmr->sbak, pmr->sbas, pmr->smax);
	pmr->bvel = pmr->sbak * fabs_urev;
    }
    else
    {
	range_check(pmr, &pmr->bvel, pmr->vbas, pmr->vmax);
	pmr->sbak = pmr->bvel / fabs_urev;
    }
    db_post_events(pmr, &pmr->sbak, DBE_VAL_LOG);
    db_post_events(pmr, &pmr->bvel, DBE_VAL_LOG);

    /* Sanity check on acceleration time. */
    if (pmr->accl == 0.0)
    {
	pmr->accl = 0.1;
	MARK_AUX(M_ACCL);
    }
    if (pmr->bacc == 0.0)
    {
	pmr->bacc = 0.1;
	MARK_AUX(M_BACC);
    }
    /* Sanity check on jog velocity and acceleration rate. */
    if (pmr->jvel == 0.0)
	pmr->jvel = pmr->velo;
    else
	range_check(pmr, &pmr->jvel, pmr->vbas, pmr->vmax);

    if (pmr->jar == 0.0)
	pmr->jar = pmr->velo / pmr->accl;

    /* Sanity check on home velocity. */
    if (pmr->hvel == 0.0)
	pmr->hvel = pmr->vbas;
    else
	range_check(pmr, &pmr->hvel, pmr->vbas, pmr->vmax);
}

/*
FUNCTION... void set_dial_highlimit(motorRecord *)
USAGE... Set dial-coordinate high limit.
NOTES... This function sends a command to the device to set the raw dial high
limit.  This is done so that a device level function may do an error check on
the validity of the limit.  This is to support those devices (e.g., MM4000)
that have their own, read-only, travel limits.
*/
static void set_dial_highlimit(motorRecord *pmr, struct motor_dset *pdset)
{
    int dir_positive = (pmr->dir == motorDIR_Pos);
    double offset, tmp_raw;
    RTN_STATUS rtnval;

    tmp_raw = pmr->dhlm / pmr->mres;
    INIT_MSG();
    rtnval = (*pdset->build_trans)(SET_HIGH_LIMIT, &tmp_raw, pmr);
    offset = pmr->off;
    if (rtnval == OK)
	SEND_MSG();

    if (dir_positive)
    {
	pmr->hlm = pmr->dhlm + offset;
	MARK(M_HLM);
    }
    else
    {
	pmr->llm = -(pmr->dhlm) + offset;
	MARK(M_LLM);
    }
    MARK(M_DHLM);
}

/*
FUNCTION... void set_dial_lowlimit(motorRecord *)
USAGE... Set dial-coordinate low limit.
NOTES... This function sends a command to the device to set the raw dial low
limit.  This is done so that a device level function may do an error check on
the validity of the limit.  This is to support those devices (e.g., MM4000)
that have their own, read-only, travel limits.
*/
static void set_dial_lowlimit(motorRecord *pmr, struct motor_dset *pdset)
{
    int dir_positive = (pmr->dir == motorDIR_Pos);
    double offset, tmp_raw;
    RTN_STATUS rtnval;

    tmp_raw = pmr->dllm / pmr->mres;

    INIT_MSG();
    rtnval = (*pdset->build_trans)(SET_LOW_LIMIT, &tmp_raw, pmr);
    offset = pmr->off;
    if (rtnval == OK)
	SEND_MSG();

    if (dir_positive)
    {
	pmr->llm = pmr->dllm + offset;
	MARK(M_LLM);
    }
    else
    {
	pmr->hlm = -(pmr->dllm) + offset;
	MARK(M_HLM);
    }
    MARK(M_DLLM);
}

/*
FUNCTION... void set_userlimits(motorRecord *)
USAGE... Translate dial-coordinate limits to user-coordinate limits.
*/
static void set_userlimits(motorRecord *pmr)
{
    if (pmr->dir == motorDIR_Pos)
    {
	pmr->hlm = pmr->dhlm + pmr->off;
	pmr->llm = pmr->dllm + pmr->off;
    }
    else
    {
	pmr->hlm = -(pmr->dllm) + pmr->off;
	pmr->llm = -(pmr->dhlm) + pmr->off;
    }
    MARK(M_HLM);
    MARK(M_LLM);
}

/*
FUNCTION... void range_check(motorRecord *, float *, double, double)
USAGE... Limit parameter to valid range; i.e., min. <= parameter <= max.

INPUT...	parm - pointer to parameter to be range check.
		min  - minimum value.
		max  - 0 = max. range check disabled; !0 = maximum value.
*/
static void range_check(motorRecord *pmr, float *parm_ptr, double min, double max)
{
    bool changed = false;
    double parm_val = *parm_ptr;

    if (parm_val < min)
    {
	parm_val = min;
	changed = true;
    }
    if (max != 0.0 && parm_val > max)
    {
	parm_val = max;
	changed = true;
    }

    if (changed == true)
    {
	*parm_ptr = parm_val;
	db_post_events(pmr, parm_ptr, DBE_VAL_LOG);
    }
}

