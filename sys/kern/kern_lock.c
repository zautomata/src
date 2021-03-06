/*	$OpenBSD: kern_lock.c,v 1.52 2017/12/04 09:51:03 mpi Exp $	*/

/* 
 * Copyright (c) 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code contains ideas from software contributed to Berkeley by
 * Avadis Tevanian, Jr., Michael Wayne Young, and the Mach Operating
 * System project at Carnegie-Mellon University.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_lock.c	8.18 (Berkeley) 5/21/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sched.h>
#include <sys/atomic.h>
#include <sys/witness.h>

#include <ddb/db_output.h>

#ifdef MP_LOCKDEBUG
#ifndef DDB
#error "MP_LOCKDEBUG requires DDB"
#endif

/* CPU-dependent timing, this needs to be settable from ddb. */
int __mp_lock_spinout = 200000000;
#endif /* MP_LOCKDEBUG */

#if defined(MULTIPROCESSOR) || defined(WITNESS)
struct __mp_lock kernel_lock;
#endif

#ifdef MULTIPROCESSOR

/*
 * Functions for manipulating the kernel_lock.  We put them here
 * so that they show up in profiles.
 */

void
_kernel_lock_init(void)
{
	__mp_lock_init(&kernel_lock);
}

/*
 * Acquire/release the kernel lock.  Intended for use in the scheduler
 * and the lower half of the kernel.
 */

void
_kernel_lock(const char *file, int line)
{
	SCHED_ASSERT_UNLOCKED();
#ifdef WITNESS
	___mp_lock(&kernel_lock, file, line);
#else
	__mp_lock(&kernel_lock);
#endif
}

void
_kernel_unlock(void)
{
	__mp_unlock(&kernel_lock);
}

int
_kernel_lock_held(void)
{
	if (panicstr)
		return 1;
	return (__mp_lock_held(&kernel_lock, curcpu()));
}

#ifdef __USE_MI_MPLOCK

/* Ticket lock implementation */
/*
 * Copyright (c) 2014 David Gwynne <dlg@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <machine/cpu.h>

void
___mp_lock_init(struct __mp_lock *mpl, struct lock_type *type)
{
	memset(mpl->mpl_cpus, 0, sizeof(mpl->mpl_cpus));
	mpl->mpl_users = 0;
	mpl->mpl_ticket = 1;

#ifdef WITNESS
	mpl->mpl_lock_obj.lo_name = type->lt_name;
	mpl->mpl_lock_obj.lo_type = type;
	if (mpl == &kernel_lock)
		mpl->mpl_lock_obj.lo_flags = LO_WITNESS | LO_INITIALIZED |
		    LO_SLEEPABLE | (LO_CLASS_KERNEL_LOCK << LO_CLASSSHIFT);
	else if (mpl == &sched_lock)
		mpl->mpl_lock_obj.lo_flags = LO_WITNESS | LO_INITIALIZED |
		    LO_RECURSABLE | (LO_CLASS_SCHED_LOCK << LO_CLASSSHIFT);
	WITNESS_INIT(&mpl->mpl_lock_obj, type);
#endif
}

static __inline void
__mp_lock_spin(struct __mp_lock *mpl, u_int me)
{
#ifndef MP_LOCKDEBUG
	while (mpl->mpl_ticket != me)
		CPU_BUSY_CYCLE();
#else
	int nticks = __mp_lock_spinout;

	while (mpl->mpl_ticket != me) {
		CPU_BUSY_CYCLE();

		if (--nticks <= 0) {
			db_printf("__mp_lock(%p): lock spun out", mpl);
			db_enter();
			nticks = __mp_lock_spinout;
		}
	}
#endif
}

void
___mp_lock(struct __mp_lock *mpl LOCK_FL_VARS)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[cpu_number()];
	unsigned long s;

#ifdef WITNESS
	if (!__mp_lock_held(mpl, curcpu()))
		WITNESS_CHECKORDER(&mpl->mpl_lock_obj,
		    LOP_EXCLUSIVE | LOP_NEWORDER, file, line, NULL);
#endif

	s = intr_disable();
	if (cpu->mplc_depth++ == 0)
		cpu->mplc_ticket = atomic_inc_int_nv(&mpl->mpl_users);
	intr_restore(s);

	__mp_lock_spin(mpl, cpu->mplc_ticket);
	membar_enter_after_atomic();

	WITNESS_LOCK(&mpl->mpl_lock_obj, LOP_EXCLUSIVE, file, line);
}

void
___mp_unlock(struct __mp_lock *mpl LOCK_FL_VARS)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[cpu_number()];
	unsigned long s;

#ifdef MP_LOCKDEBUG
	if (!__mp_lock_held(mpl, curcpu())) {
		db_printf("__mp_unlock(%p): not held lock\n", mpl);
		db_enter();
	}
#endif

	WITNESS_UNLOCK(&mpl->mpl_lock_obj, LOP_EXCLUSIVE, file, line);

	s = intr_disable();
	if (--cpu->mplc_depth == 0) {
		membar_exit();
		mpl->mpl_ticket++;
	}
	intr_restore(s);
}

int
___mp_release_all(struct __mp_lock *mpl LOCK_FL_VARS)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[cpu_number()];
	unsigned long s;
	int rv;
#ifdef WITNESS
	int i;
#endif

	s = intr_disable();
	rv = cpu->mplc_depth;
#ifdef WITNESS
	for (i = 0; i < rv; i++)
		WITNESS_UNLOCK(&mpl->mpl_lock_obj, LOP_EXCLUSIVE, file, line);
#endif
	cpu->mplc_depth = 0;
	membar_exit();
	mpl->mpl_ticket++;
	intr_restore(s);

	return (rv);
}

int
___mp_release_all_but_one(struct __mp_lock *mpl LOCK_FL_VARS)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[cpu_number()];
	int rv = cpu->mplc_depth - 1;
#ifdef WITNESS
	int i;

	for (i = 0; i < rv; i++)
		WITNESS_UNLOCK(&mpl->mpl_lock_obj, LOP_EXCLUSIVE, file, line);
#endif

#ifdef MP_LOCKDEBUG
	if (!__mp_lock_held(mpl, curcpu())) {
		db_printf("__mp_release_all_but_one(%p): not held lock\n", mpl);
		db_enter();
	}
#endif

	cpu->mplc_depth = 1;

	return (rv);
}

void
___mp_acquire_count(struct __mp_lock *mpl, int count LOCK_FL_VARS)
{
	while (count--)
		___mp_lock(mpl LOCK_FL_ARGS);
}

int
__mp_lock_held(struct __mp_lock *mpl, struct cpu_info *ci)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[CPU_INFO_UNIT(ci)];

	return (cpu->mplc_ticket == mpl->mpl_ticket && cpu->mplc_depth > 0);
}

#endif /* __USE_MI_MPLOCK */

#endif /* MULTIPROCESSOR */
