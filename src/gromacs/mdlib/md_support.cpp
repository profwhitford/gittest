/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team.
 * Copyright (c) 2013,2014,2015,2016,2017,2018,2019,2020, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */

#include "gmxpre.h"

#include "md_support.h"

#include <climits>
#include <cmath>

#include <algorithm>

#include "gromacs/domdec/domdec.h"
#include "gromacs/gmxlib/network.h"
#include "gromacs/gmxlib/nrnb.h"
#include "gromacs/math/utilities.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdlib/dispersioncorrection.h"
#include "gromacs/mdlib/simulationsignal.h"
#include "gromacs/mdlib/stat.h"
#include "gromacs/mdlib/tgroup.h"
#include "gromacs/mdlib/update.h"
#include "gromacs/mdlib/vcm.h"
#include "gromacs/mdrunutility/multisim.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/mdtypes/df_history.h"
#include "gromacs/mdtypes/enerdata.h"
#include "gromacs/mdtypes/energyhistory.h"
#include "gromacs/mdtypes/forcerec.h"
#include "gromacs/mdtypes/group.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/pulling/pull.h"
#include "gromacs/timing/wallcycle.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/trajectory/trajectoryframe.h"
#include "gromacs/utility/arrayref.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/logger.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/snprintf.h"

// TODO move this to multi-sim module
bool multisim_int_all_are_equal(const gmx_multisim_t* ms, int64_t value)
{
    bool     allValuesAreEqual = true;
    int64_t* buf;

    GMX_RELEASE_ASSERT(ms, "Invalid use of multi-simulation pointer");

    snew(buf, ms->nsim);
    /* send our value to all other master ranks, receive all of theirs */
    buf[ms->sim] = value;
    gmx_sumli_sim(ms->nsim, buf, ms);

    for (int s = 0; s < ms->nsim; s++)
    {
        if (buf[s] != value)
        {
            allValuesAreEqual = false;
            break;
        }
    }

    sfree(buf);

    return allValuesAreEqual;
}

int multisim_min(const gmx_multisim_t* ms, int nmin, int n)
{
    int*     buf;
    gmx_bool bPos, bEqual;
    int      s, d;

    snew(buf, ms->nsim);
    buf[ms->sim] = n;
    gmx_sumi_sim(ms->nsim, buf, ms);
    bPos   = TRUE;
    bEqual = TRUE;
    for (s = 0; s < ms->nsim; s++)
    {
        bPos   = bPos && (buf[s] > 0);
        bEqual = bEqual && (buf[s] == buf[0]);
    }
    if (bPos)
    {
        if (bEqual)
        {
            nmin = std::min(nmin, buf[0]);
        }
        else
        {
            /* Find the least common multiple */
            for (d = 2; d < nmin; d++)
            {
                s = 0;
                while (s < ms->nsim && d % buf[s] == 0)
                {
                    s++;
                }
                if (s == ms->nsim)
                {
                    /* We found the LCM and it is less than nmin */
                    nmin = d;
                    break;
                }
            }
        }
    }
    sfree(buf);

    return nmin;
}

/* TODO Specialize this routine into init-time and loop-time versions?
   e.g. bReadEkin is only true when restoring from checkpoint */
void compute_globals(gmx_global_stat*          gstat,
                     t_commrec*                cr,
                     const t_inputrec*         ir,
                     t_forcerec*               fr,
                     gmx_ekindata_t*           ekind,
                     const rvec*               x,
                     const rvec*               v,
                     const matrix              box,
                     real                      vdwLambda,
                     const t_mdatoms*          mdatoms,
                     t_nrnb*                   nrnb,
                     t_vcm*                    vcm,
                     gmx_wallcycle_t           wcycle,
                     gmx_enerdata_t*           enerd,
                     tensor                    force_vir,
                     tensor                    shake_vir,
                     tensor                    total_vir,
                     tensor                    pres,
                     rvec                      mu_tot,
                     gmx::Constraints*         constr,
                     gmx::SimulationSignaller* signalCoordinator,
                     const matrix              lastbox,
                     int*                      totalNumberOfBondedInteractions,
                     gmx_bool*                 bSumEkinhOld,
                     const int                 flags)
{
    gmx_bool bEner, bPres, bTemp;
    gmx_bool bStopCM, bGStat, bReadEkin, bEkinAveVel, bScaleEkin, bConstrain;
    gmx_bool bCheckNumberOfBondedInteractions;
    real     dvdl_ekin;

    /* translate CGLO flags to gmx_booleans */
    bStopCM                          = ((flags & CGLO_STOPCM) != 0);
    bGStat                           = ((flags & CGLO_GSTAT) != 0);
    bReadEkin                        = ((flags & CGLO_READEKIN) != 0);
    bScaleEkin                       = ((flags & CGLO_SCALEEKIN) != 0);
    bEner                            = ((flags & CGLO_ENERGY) != 0);
    bTemp                            = ((flags & CGLO_TEMPERATURE) != 0);
    bPres                            = ((flags & CGLO_PRESSURE) != 0);
    bConstrain                       = ((flags & CGLO_CONSTRAINT) != 0);
    bCheckNumberOfBondedInteractions = ((flags & CGLO_CHECK_NUMBER_OF_BONDED_INTERACTIONS) != 0);

    /* we calculate a full state kinetic energy either with full-step velocity verlet
       or half step where we need the pressure */

    bEkinAveVel = (ir->eI == eiVV || (ir->eI == eiVVAK && bPres) || bReadEkin);

    /* in initalization, it sums the shake virial in vv, and to
       sums ekinh_old in leapfrog (or if we are calculating ekinh_old) for other reasons */

    /* ########## Kinetic energy  ############## */

    if (bTemp)
    {
        /* Non-equilibrium MD: this is parallellized, but only does communication
         * when there really is NEMD.
         */

        if (PAR(cr) && (ekind->bNEMD))
        {
            accumulate_u(cr, &(ir->opts), ekind);
        }
        if (!bReadEkin)
        {
            calc_ke_part(x, v, box, &(ir->opts), mdatoms, ekind, nrnb, bEkinAveVel);
        }
    }

    /* Calculate center of mass velocity if necessary, also parallellized */
    if (bStopCM)
    {
        calc_vcm_grp(*mdatoms, x, v, vcm);
    }

    if (bTemp || bStopCM || bPres || bEner || bConstrain || bCheckNumberOfBondedInteractions)
    {
        if (!bGStat)
        {
            /* We will not sum ekinh_old,
             * so signal that we still have to do it.
             */
            *bSumEkinhOld = TRUE;
        }
        else
        {
            gmx::ArrayRef<real> signalBuffer = signalCoordinator->getCommunicationBuffer();
            if (PAR(cr))
            {
                wallcycle_start(wcycle, ewcMoveE);
                global_stat(gstat, cr, enerd, force_vir, shake_vir, mu_tot, ir, ekind, constr,
                            bStopCM ? vcm : nullptr, signalBuffer.size(), signalBuffer.data(),
                            totalNumberOfBondedInteractions, *bSumEkinhOld, flags);
                wallcycle_stop(wcycle, ewcMoveE);
            }
            signalCoordinator->finalizeSignals();
            *bSumEkinhOld = FALSE;
        }
    }

    if (bEner)
    {
        /* Calculate the amplitude of the cosine velocity profile */
        ekind->cosacc.vcos = ekind->cosacc.mvcos / mdatoms->tmass;
    }

    if (bTemp)
    {
        /* Sum the kinetic energies of the groups & calc temp */
        /* compute full step kinetic energies if vv, or if vv-avek and we are computing the pressure with inputrecNptTrotter */
        /* three maincase:  VV with AveVel (md-vv), vv with AveEkin (md-vv-avek), leap with AveEkin (md).
           Leap with AveVel is not supported; it's not clear that it will actually work.
           bEkinAveVel: If TRUE, we simply multiply ekin by ekinscale to get a full step kinetic energy.
           If FALSE, we average ekinh_old and ekinh*ekinscale_nhc to get an averaged half step kinetic energy.
         */
        enerd->term[F_TEMP] = sum_ekin(&(ir->opts), ekind, &dvdl_ekin, bEkinAveVel, bScaleEkin);
        enerd->dvdl_lin[efptMASS] = static_cast<double>(dvdl_ekin);

        enerd->term[F_EKIN] = trace(ekind->ekin);
    }

    /* ########## Now pressure ############## */
    // TODO: For the VV integrator bConstrain is needed in the conditional. This is confusing, so get rid of this.
    if (bPres || bConstrain)
    {
        m_add(force_vir, shake_vir, total_vir);

        /* Calculate pressure and apply LR correction if PPPM is used.
         * Use the box from last timestep since we already called update().
         */

        enerd->term[F_PRES] = calc_pres(fr->ePBC, ir->nwall, lastbox, ekind->ekin, total_vir, pres);
    }

    /* ##########  Long range energy information ###### */
    if ((bEner || bPres) && fr->dispersionCorrection)
    {
        /* Calculate long range corrections to pressure and energy */
        /* this adds to enerd->term[F_PRES] and enerd->term[F_ETOT],
           and computes enerd->term[F_DISPCORR].  Also modifies the
           total_vir and pres tensors */

        const DispersionCorrection::Correction correction =
                fr->dispersionCorrection->calculate(lastbox, vdwLambda);

        if (bEner)
        {
            enerd->term[F_DISPCORR] = correction.energy;
            enerd->term[F_EPOT] += correction.energy;
            enerd->term[F_DVDL_VDW] += correction.dvdl;
        }
        if (bPres)
        {
            correction.correctVirial(total_vir);
            correction.correctPressure(pres);
            enerd->term[F_PDISPCORR] = correction.pressure;
            enerd->term[F_PRES] += correction.pressure;
        }
    }
}

void setCurrentLambdasRerun(int64_t           step,
                            const t_lambda*   fepvals,
                            const t_trxframe* rerun_fr,
                            const double* /*lam0*/,
                            t_state* globalState)
{
    GMX_RELEASE_ASSERT(globalState != nullptr,
                       "setCurrentLambdasGlobalRerun should be called with a valid state object");

    if (rerun_fr->bLambda)
    {
        if (fepvals->delta_lambda == 0)
        {
            globalState->lambda[efptFEP] = rerun_fr->lambda;
        }
        else
        {
            /* find out between which two value of lambda we should be */
            const real fracSimulationLambda = step * fepvals->delta_lambda;

            // Set initial lambda value for the simulation either from initialFEPStateIndex or,
            // if not set, from the initial lambda.
            double initialGlobalLambda = 0;
            if (fepvals->init_fep_state > -1)
            {
                if (fepvals->n_lambda > 1)
                {
                    initialGlobalLambda =
                            static_cast<double>(fepvals->init_fep_state) / (fepvals->n_lambda - 1);
                }
            }
            else
            {
                if (fepvals->init_lambda > -1)
                {
                    initialGlobalLambda = fepvals->init_lambda;
                }
            }

            const double globalLambda = initialGlobalLambda + fracSimulationLambda;

            // when there is no lambda value array, set all lambdas to steps * deltaLambdaPerStep
            if (fepvals->n_lambda <= 0)
            {
                std::fill(std::begin(globalState->lambda), std::end(globalState->lambda), globalLambda);
                return;
            }

            GMX_ASSERT(
                    globalLambda <= 1 || gmx_within_tol(globalLambda, 1, 1e-5),
                    "Lambda may not be larger than one when interpolating an array of multi-lambda "
                    "values.");
            GMX_ASSERT(globalLambda >= 0 || gmx_within_tol(globalLambda, 0, 1e-5),
                       "Lambda may not be negative when interpolating an array of multi-lambda "
                       "values.");

            // find out between which two value lambda array elements to interpolate
            // at the boundary of the lambda array, return the boundary array values
            const int fepStateLeft =
                    std::max(0, static_cast<int>(std::floor(globalLambda * (fepvals->n_lambda - 1))));

            const int fepStateRight = std::min(fepvals->n_lambda - 1, fepStateLeft + 1);

            // interpolate between this state and the next
            const double fracBetween = globalLambda * (fepvals->n_lambda - 1) - fepStateLeft;
            for (int i = 0; i < efptNR; i++)
            {
                globalState->lambda[i] = fepvals->all_lambda[i][fepStateLeft]
                                         + fracBetween
                                                   * (fepvals->all_lambda[i][fepStateRight]
                                                      - fepvals->all_lambda[i][fepStateLeft]);
            }
        }
    }
    else if (rerun_fr->bFepState)
    {
        globalState->fep_state = rerun_fr->fep_state;
        for (int i = 0; i < efptNR; i++)
        {
            globalState->lambda[i] = fepvals->all_lambda[i][globalState->fep_state];
        }
    }
}

void setCurrentLambdasLocal(const int64_t   step,
                            const t_lambda* fepvals,
                            const double* /*lam0*/,
                            gmx::ArrayRef<real> lambda,
                            const int           currentFEPState)
/* find the current lambdas.  If rerunning, we either read in a state, or a lambda value,
   requiring different logic. */
{
    if (fepvals->delta_lambda != 0)
    {
        /* find out between which two value of lambda we should be */
        const real fracSimulationLambda = step * fepvals->delta_lambda;

        // Set initial lambda value for the simulation either from initialFEPStateIndex or,
        // if not set, from the initial lambda.
        double initialGlobalLambda = 0;
        if (fepvals->init_fep_state > -1)
        {
            if (fepvals->n_lambda > 1)
            {
                initialGlobalLambda =
                        static_cast<double>(fepvals->init_fep_state) / (fepvals->n_lambda - 1);
            }
        }
        else
        {
            if (fepvals->init_lambda > -1)
            {
                initialGlobalLambda = fepvals->init_lambda;
            }
        }

        const double globalLambda = initialGlobalLambda + fracSimulationLambda;

        // when there is no lambda value array, set all lambdas to steps * deltaLambdaPerStep
        if (fepvals->n_lambda <= 0)
        {
            std::fill(std::begin(lambda), std::end(lambda), globalLambda);
            return;
        }

        GMX_ASSERT(globalLambda <= 1 || gmx_within_tol(globalLambda, 1, 1e-5),
                   "Lambda may not be larger than one when interpolating an array of multi-lambda "
                   "values.");
        GMX_ASSERT(
                globalLambda >= 0 || gmx_within_tol(globalLambda, 0, 1e-5),
                "Lambda may not be negative when interpolating an array of multi-lambda values.");

        // find out between which two value lambda array elements to interpolate
        // at the boundary of the lambda array, return the boundary array values
        const int fepStateLeft =
                std::max(0, static_cast<int>(std::floor(globalLambda * (fepvals->n_lambda - 1))));

        const int fepStateRight = std::min(fepvals->n_lambda - 1, fepStateLeft + 1);

        // interpolate between this state and the next
        const double fracBetween = globalLambda * (fepvals->n_lambda - 1) - fepStateLeft;
        for (int i = 0; i < efptNR; i++)
        {
            lambda[i] = fepvals->all_lambda[i][fepStateLeft]
                        + fracBetween
                                  * (fepvals->all_lambda[i][fepStateRight]
                                     - fepvals->all_lambda[i][fepStateLeft]);
        }
    }
    else
    {
        /* if < 0, fep_state was never defined, and we should not set lambda from the state */
        if (currentFEPState > -1)
        {
            GMX_ASSERT(currentFEPState < fepvals->n_lambda,
                       "Current FEP state may not be larger than number of given states.");
            for (int i = 0; i < efptNR; i++)
            {
                lambda[i] = fepvals->all_lambda[i][currentFEPState];
            }
            return;
        }

        if (fepvals->init_fep_state > -1)
        {
            GMX_ASSERT(fepvals->init_fep_state < fepvals->n_lambda,
                       "Initial FEP state may not be larger than number of given states.");
            for (int i = 0; i < efptNR; i++)
            {
                lambda[i] = fepvals->all_lambda[i][fepvals->init_fep_state];
            }
            return;
        }
        std::fill(std::begin(lambda), std::end(lambda), fepvals->init_lambda);
    }
}

static void min_zero(int* n, int i)
{
    if (i > 0 && (*n == 0 || i < *n))
    {
        *n = i;
    }
}

static int lcd4(int i1, int i2, int i3, int i4)
{
    int nst;

    nst = 0;
    min_zero(&nst, i1);
    min_zero(&nst, i2);
    min_zero(&nst, i3);
    min_zero(&nst, i4);
    if (nst == 0)
    {
        gmx_incons("All 4 inputs for determining nstglobalcomm are <= 0");
    }

    while (nst > 1
           && ((i1 > 0 && i1 % nst != 0) || (i2 > 0 && i2 % nst != 0) || (i3 > 0 && i3 % nst != 0)
               || (i4 > 0 && i4 % nst != 0)))
    {
        nst--;
    }

    return nst;
}

int computeGlobalCommunicationPeriod(const gmx::MDLogger& mdlog, t_inputrec* ir, const t_commrec* cr)
{
    int nstglobalcomm;
    {
        // Set up the default behaviour
        if (!(ir->nstcalcenergy > 0 || ir->nstlist > 0 || ir->etc != etcNO || ir->epc != epcNO))
        {
            /* The user didn't choose the period for anything
               important, so we just make sure we can send signals and
               write output suitably. */
            nstglobalcomm = 10;
            if (ir->nstenergy > 0 && ir->nstenergy < nstglobalcomm)
            {
                nstglobalcomm = ir->nstenergy;
            }
        }
        else
        {
            /* The user has made a choice (perhaps implicitly), so we
             * ensure that we do timely intra-simulation communication
             * for (possibly) each of the four parts that care.
             *
             * TODO Does the Verlet scheme (+ DD) need any
             * communication at nstlist steps? Is the use of nstlist
             * here a leftover of the twin-range scheme? Can we remove
             * nstlist when we remove the group scheme?
             */
            nstglobalcomm = lcd4(ir->nstcalcenergy, ir->nstlist, ir->etc != etcNO ? ir->nsttcouple : 0,
                                 ir->epc != epcNO ? ir->nstpcouple : 0);
        }
    }

    // TODO change this behaviour. Instead grompp should print
    // a (performance) note and mdrun should not change ir.
    if (ir->comm_mode != ecmNO && ir->nstcomm < nstglobalcomm)
    {
        GMX_LOG(mdlog.warning)
                .asParagraph()
                .appendTextFormatted("WARNING: Changing nstcomm from %d to %d", ir->nstcomm, nstglobalcomm);
        ir->nstcomm = nstglobalcomm;
    }

    if (cr->nnodes > 1)
    {
        GMX_LOG(mdlog.info)
                .appendTextFormatted("Intra-simulation communication will occur every %d steps.\n",
                                     nstglobalcomm);
    }
    return nstglobalcomm;
}

void rerun_parallel_comm(t_commrec* cr, t_trxframe* fr, gmx_bool* bLastStep)
{
    rvec *xp, *vp;

    if (MASTER(cr) && *bLastStep)
    {
        fr->natoms = -1;
    }
    xp = fr->x;
    vp = fr->v;
    gmx_bcast(sizeof(*fr), fr, cr);
    fr->x = xp;
    fr->v = vp;

    *bLastStep = (fr->natoms < 0);
}

// TODO Most of this logic seems to belong in the respective modules
void set_state_entries(t_state* state, const t_inputrec* ir, bool useModularSimulator)
{
    /* The entries in the state in the tpx file might not correspond
     * with what is needed, so we correct this here.
     */
    state->flags = 0;
    if (ir->efep != efepNO || ir->bExpanded)
    {
        state->flags |= (1 << estLAMBDA);
        state->flags |= (1 << estFEPSTATE);
    }
    state->flags |= (1 << estX);
    GMX_RELEASE_ASSERT(state->x.size() == state->natoms,
                       "We should start a run with an initialized state->x");
    if (EI_DYNAMICS(ir->eI))
    {
        state->flags |= (1 << estV);
    }

    state->nnhpres = 0;
    if (ir->ePBC != epbcNONE)
    {
        state->flags |= (1 << estBOX);
        if (inputrecPreserveShape(ir))
        {
            state->flags |= (1 << estBOX_REL);
        }
        if ((ir->epc == epcPARRINELLORAHMAN) || (ir->epc == epcMTTK))
        {
            state->flags |= (1 << estBOXV);
            if (!useModularSimulator)
            {
                state->flags |= (1 << estPRES_PREV);
            }
        }
        if (inputrecNptTrotter(ir) || (inputrecNphTrotter(ir)))
        {
            state->nnhpres = 1;
            state->flags |= (1 << estNHPRES_XI);
            state->flags |= (1 << estNHPRES_VXI);
            state->flags |= (1 << estSVIR_PREV);
            state->flags |= (1 << estFVIR_PREV);
            state->flags |= (1 << estVETA);
            state->flags |= (1 << estVOL0);
        }
        if (ir->epc == epcBERENDSEN)
        {
            state->flags |= (1 << estBAROS_INT);
        }
    }

    if (ir->etc == etcNOSEHOOVER)
    {
        state->flags |= (1 << estNH_XI);
        state->flags |= (1 << estNH_VXI);
    }

    if (ir->etc == etcVRESCALE || ir->etc == etcBERENDSEN)
    {
        state->flags |= (1 << estTHERM_INT);
    }

    init_gtc_state(state, state->ngtc, state->nnhpres,
                   ir->opts.nhchainlength); /* allocate the space for nose-hoover chains */
    init_ekinstate(&state->ekinstate, ir);

    if (ir->bExpanded)
    {
        snew(state->dfhist, 1);
        init_df_history(state->dfhist, ir->fepvals->n_lambda);
    }

    if (ir->pull && ir->pull->bSetPbcRefToPrevStepCOM)
    {
        state->flags |= (1 << estPULLCOMPREVSTEP);
    }
}