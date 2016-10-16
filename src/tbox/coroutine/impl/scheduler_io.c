/*!The Treasure Box Library
 * 
 * TBox is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * TBox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with TBox; 
 * If not, see <a href="http://www.gnu.org/licenses/"> http://www.gnu.org/licenses/</a>
 * 
 * Copyright (C) 2009 - 2017, ruki All rights reserved.
 *
 * @author      ruki
 * @file        scheduler_io.c
 * @ingroup     coroutine
 *
 */

/* //////////////////////////////////////////////////////////////////////////////////////
 * trace
 */
#define TB_TRACE_MODULE_NAME            "scheduler_io"
#define TB_TRACE_MODULE_DEBUG           (1)

/* //////////////////////////////////////////////////////////////////////////////////////
 * includes
 */
#include "scheduler_io.h"
#include "coroutine.h"

/* //////////////////////////////////////////////////////////////////////////////////////
 * macros
 */

// the ltimer grow
#ifdef __tb_small__
#   define TB_SCHEDULER_IO_LTIMER_GROW      (64)
#else
#   define TB_SCHEDULER_IO_LTIMER_GROW      (4096)
#endif

// the timer grow
#define TB_SCHEDULER_IO_TIMER_GROW          (TB_SCHEDULER_IO_LTIMER_GROW >> 4)

/* //////////////////////////////////////////////////////////////////////////////////////
 * private implementation
 */
static tb_void_t tb_co_scheduler_io_timeout(tb_bool_t killed, tb_cpointer_t priv)
{
    // check
    tb_coroutine_t* coroutine = (tb_coroutine_t*)priv;
    tb_assert(coroutine);

    // get scheduler
    tb_co_scheduler_t* scheduler = (tb_co_scheduler_t*)tb_coroutine_scheduler(coroutine);
    tb_assert(scheduler);

    // get io scheduler
    tb_co_scheduler_io_ref_t scheduler_io = tb_co_scheduler_io(scheduler);
    tb_assert(scheduler_io && scheduler_io->poller);

    // trace
    tb_trace_d("coroutine(%p): timer %s", coroutine, killed? "killed" : "timeout");

    // remove this socket from poller
    tb_socket_ref_t sock = (tb_socket_ref_t)coroutine->io_priv[2];
    if (sock) 
    {
        // remove it
        tb_poller_remove(scheduler_io->poller, sock);
        coroutine->io_priv[2] = tb_null;
    }

    // resume the coroutine of this timer task
    tb_co_scheduler_resume(scheduler, coroutine, tb_null);
}
static tb_void_t tb_co_scheduler_io_events(tb_poller_ref_t poller, tb_socket_ref_t sock, tb_size_t events, tb_cpointer_t priv)
{
    // check
    tb_coroutine_t* coroutine = (tb_coroutine_t*)priv;
    tb_assert(coroutine && poller && sock && priv);

    // get scheduler
    tb_co_scheduler_t* scheduler = (tb_co_scheduler_t*)tb_coroutine_scheduler(coroutine);
    tb_assert(scheduler);

    // get io scheduler
    tb_co_scheduler_io_ref_t scheduler_io = tb_co_scheduler_io(scheduler);
    tb_assert(scheduler_io && scheduler_io->poller);

    // trace
    tb_trace_d("coroutine(%p): socket: %p, events %lu", coroutine, sock, events);

    // exists the timer task? remove it
    tb_cpointer_t task = coroutine->io_priv[0];
    if (task) 
    {
        // is low-precision timer?
        tb_cpointer_t is_ltimer = coroutine->io_priv[1];

        // remove the timer task
        if (is_ltimer) tb_ltimer_task_exit(scheduler_io->ltimer, (tb_ltimer_task_ref_t)task);
        else tb_timer_task_exit(scheduler_io->timer, (tb_timer_task_ref_t)task);
        coroutine->io_priv[0] = tb_null;
    }

    // remove this socket from poller
    tb_poller_remove(scheduler_io->poller, sock);

    // resume the coroutine of this socket and pass the events to suspend()
    tb_co_scheduler_resume(scheduler, coroutine, (tb_cpointer_t)events);
}
static tb_bool_t tb_co_scheduler_io_timer_spak(tb_co_scheduler_io_ref_t scheduler_io)
{
    // check
    tb_assert(scheduler_io && scheduler_io->timer && scheduler_io->ltimer);

    // spak ctime
    tb_cache_time_spak();

    // spak timer
    if (!tb_timer_spak(scheduler_io->timer)) return tb_false;

    // spak ltimer
    if (!tb_ltimer_spak(scheduler_io->ltimer)) return tb_false;

    // pk
    return tb_true;
}
static tb_void_t tb_co_scheduler_io_loop(tb_cpointer_t priv)
{
    // check
    tb_co_scheduler_io_ref_t scheduler_io = (tb_co_scheduler_io_ref_t)priv;
    tb_assert_and_check_return(scheduler_io && scheduler_io->timer && scheduler_io->ltimer);

    // the scheduler
    tb_co_scheduler_t* scheduler = scheduler_io->scheduler;
    tb_assert_and_check_return(scheduler);

    // the poller
    tb_poller_ref_t poller = scheduler_io->poller;
    tb_assert_and_check_return(poller);

    // loop
    while (!scheduler->stopped)
    {
        // finish all other ready coroutines first
        while (tb_co_scheduler_yield(scheduler)) 
        {
            // spar timer
            if (!tb_co_scheduler_io_timer_spak(scheduler_io)) break;
        }

        // no more suspended coroutines? loop end
        tb_check_break(tb_co_scheduler_suspend_count(scheduler));

        // the delay
        tb_size_t delay = tb_timer_delay(scheduler_io->timer);

        // the ldelay
        tb_size_t ldelay = tb_ltimer_delay(scheduler_io->ltimer);

        // trace
        tb_trace_d("loop: wait %lu ms ..", tb_min(delay, ldelay));

        // no more ready coroutines? wait io events and timers
        if (tb_poller_wait(poller, tb_co_scheduler_io_events, tb_min(delay, ldelay)) < 0) break;

        // spar timer
        if (!tb_co_scheduler_io_timer_spak(scheduler_io)) break;
    }
}

/* //////////////////////////////////////////////////////////////////////////////////////
 * implementation
 */
tb_co_scheduler_io_ref_t tb_co_scheduler_io_init(tb_co_scheduler_t* scheduler)
{
    // done
    tb_bool_t                   ok = tb_false;
    tb_co_scheduler_io_ref_t    scheduler_io = tb_null;
    do
    {
        // init io scheduler
        scheduler_io = tb_malloc0_type(tb_co_scheduler_io_t);
        tb_assert_and_check_break(scheduler_io);

        // save scheduler
        scheduler_io->scheduler = (tb_co_scheduler_t*)scheduler;

        // init timer and using cache time
        scheduler_io->timer = tb_timer_init(TB_SCHEDULER_IO_TIMER_GROW, tb_true);
        tb_assert_and_check_break(scheduler_io->timer);

        // init ltimer and using cache time
        scheduler_io->ltimer = tb_ltimer_init(TB_SCHEDULER_IO_LTIMER_GROW, TB_LTIMER_TICK_S, tb_true);
        tb_assert_and_check_break(scheduler_io->ltimer);

        // init poller
        scheduler_io->poller = tb_poller_init(tb_null);
        tb_assert_and_check_break(scheduler_io->poller);

        // start the io loop coroutine
        if (!tb_co_scheduler_start(scheduler_io->scheduler, tb_co_scheduler_io_loop, scheduler_io, 0)) break;

        // ok
        ok = tb_true;

    } while (0);

    // failed?
    if (!ok)
    {
        // exit io scheduler
        if (scheduler_io) tb_co_scheduler_io_exit(scheduler_io);
        scheduler_io = tb_null;
    }

    // ok?
    return scheduler_io;
}
tb_void_t tb_co_scheduler_io_exit(tb_co_scheduler_io_ref_t scheduler_io)
{
    // check
    tb_assert_and_check_return(scheduler_io);

    // exit poller
    if (scheduler_io->poller) tb_poller_exit(scheduler_io->poller);
    scheduler_io->poller = tb_null;

    // exit timer
    if (scheduler_io->timer) tb_timer_exit(scheduler_io->timer);
    scheduler_io->timer = tb_null;

    // exit ltimer
    if (scheduler_io->ltimer) tb_ltimer_exit(scheduler_io->ltimer);
    scheduler_io->ltimer = tb_null;

    // clear scheduler
    scheduler_io->scheduler = tb_null;

    // exit it
    tb_free(scheduler_io);
}
tb_void_t tb_co_scheduler_io_kill(tb_co_scheduler_io_ref_t scheduler_io)
{
    // check
    tb_assert_and_check_return(scheduler_io);

    // trace
    tb_trace_d("kill: ..");

    // kill timer
    if (scheduler_io->timer) tb_timer_kill(scheduler_io->timer);

    // kill ltimer
    if (scheduler_io->ltimer) tb_ltimer_kill(scheduler_io->ltimer);

    // kill poller
    if (scheduler_io->poller) tb_poller_kill(scheduler_io->poller);
}
tb_cpointer_t tb_co_scheduler_io_sleep(tb_co_scheduler_io_ref_t scheduler_io, tb_size_t interval)
{
    // check
    tb_assert_and_check_return_val(scheduler_io && scheduler_io->poller && scheduler_io->scheduler, tb_null);

    // get the current coroutine
    tb_coroutine_t* coroutine = tb_co_scheduler_running(scheduler_io->scheduler);
    tb_assert(coroutine);

    // trace
    tb_trace_d("coroutine(%p): sleep %lu ms ..", coroutine, interval);

    // high-precision interval?
    if (interval % 1000)
    {
        // post task to timer
        tb_timer_task_post(scheduler_io->timer, interval, tb_false, tb_co_scheduler_io_timeout, coroutine);
    }
    // low-precision interval?
    else
    {
        // post task to ltimer (faster)
        tb_ltimer_task_post(scheduler_io->ltimer, interval, tb_false, tb_co_scheduler_io_timeout, coroutine);
    }

    // clear the timer task 
    coroutine->io_priv[0] = tb_null;
    coroutine->io_priv[1] = tb_null;

    // clear the socket data
    coroutine->io_priv[2] = tb_null;

    // suspend it
    return tb_co_scheduler_suspend(scheduler_io->scheduler);
}
tb_long_t tb_co_scheduler_io_wait(tb_co_scheduler_io_ref_t scheduler_io, tb_socket_ref_t sock, tb_size_t events, tb_long_t timeout)
{
    // check
    tb_assert_and_check_return_val(scheduler_io && scheduler_io->poller && scheduler_io->scheduler, -1);

    // get the current coroutine
    tb_coroutine_t* coroutine = tb_co_scheduler_running(scheduler_io->scheduler);
    tb_assert(coroutine);

    // trace
    tb_trace_d("coroutine(%p): wait events(%lu) with %ld ms for socket(%p) ..", coroutine, events, timeout, sock);

    // enable edge-trigger mode if be supported
    if (tb_poller_support(scheduler_io->poller, TB_POLLER_EVENT_CLEAR))
        events |= TB_POLLER_EVENT_CLEAR;

    // insert socket to poller for waiting events
    if (!tb_poller_insert(scheduler_io->poller, sock, events, coroutine))
    {
        // trace
        tb_trace_e("failed to insert sock(%p) to poller on coroutine(%p)!", sock, coroutine);

        // failed
        return tb_false;
    }

    // exists timeout?
    tb_cpointer_t   task = tb_null;
    tb_bool_t       is_ltimer = tb_false;
    if (timeout >= 0)
    {
        // high-precision interval?
        if (timeout % 1000)
        {
            // init task for timer
            task = tb_timer_task_init(scheduler_io->timer, timeout, tb_false, tb_co_scheduler_io_timeout, coroutine);
            tb_assert_and_check_return_val(task, tb_false);
        }
        // low-precision interval?
        else
        {
            // init task for ltimer (faster)
            task = tb_ltimer_task_init(scheduler_io->ltimer, timeout, tb_false, tb_co_scheduler_io_timeout, coroutine);
            tb_assert_and_check_return_val(task, tb_false);

            // mark as low-precision timer
            is_ltimer = tb_true;
        }
    }

    // bind the timer task to coroutine
    coroutine->io_priv[0] = task;
    coroutine->io_priv[1] = (tb_cpointer_t)(tb_size_t)is_ltimer;

    // bind the socket to coroutine for the timer function
    coroutine->io_priv[2] = sock;

    // suspend the current coroutine and return the waited result
    return (tb_long_t)tb_co_scheduler_suspend(scheduler_io->scheduler);
}
