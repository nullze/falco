/*
Copyright (C) 2023 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define __STDC_FORMAT_MACROS

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <atomic>
#include <unordered_map>

#include "falco_utils.h"
#include "token_bucket.h"

#include "actions.h"
#include "helpers.h"
#include "../options.h"
#include "../signals.h"
#include "../../semaphore.h"
#include "../../stats_writer.h"
#include "../../falco_outputs.h"
#include "../../event_drops.h"
#ifndef MINIMAL_BUILD
#include "../../webserver.h"
#endif

#include <plugin_manager.h>

using namespace falco::app;
using namespace falco::app::actions;

class source_sync_context
{
public:
	source_sync_context(falco::semaphore& s)
		: m_finished(false), m_joined(false), m_semaphore(s) { }
	source_sync_context(source_sync_context&&) = default;
	source_sync_context& operator = (source_sync_context&&) = default;
	source_sync_context(const source_sync_context&) = delete;
	source_sync_context& operator = (const source_sync_context&) = delete;

	inline void finish()
	{
		bool v = false;
		while (!m_finished.compare_exchange_weak(
				v, true, 
				std::memory_order_seq_cst,
				std::memory_order_seq_cst))
		{
			if (v)
			{
				throw falco_exception("source_sync_context has been finished twice");
			}
		}
		m_semaphore.release();
	}

	inline void join()
	{
		bool v = false;
		while (!m_joined.compare_exchange_weak(
				v, true, 
				std::memory_order_seq_cst,
				std::memory_order_seq_cst))
		{
			if (v)
			{
				throw falco_exception("source_sync_context has been joined twice");
			}
		}
	}

	inline bool joined()
	{
		return m_joined.load(std::memory_order_seq_cst);
	}

	inline bool finished()
	{
		return m_finished.load(std::memory_order_seq_cst);
	}
	
private:
	// set to true when the event processing loop finishes
	std::atomic<bool> m_finished;
	// set to true when the result has been collected after finishing
	std::atomic<bool> m_joined;
	// used to notify the waiting thread when finished gets set to true
	falco::semaphore& m_semaphore;
};

struct live_context
{
	live_context() = default;
	live_context(live_context&&) = default;
	live_context& operator = (live_context&&) = default;
	live_context(const live_context&) = default;
	live_context& operator = (const live_context&) = default;

	// the name of the source of which events are processed
	std::string source;
	// the result of the event processing loop
	run_result res;
	// if non-null, the thread on which events are processed
	std::unique_ptr<std::thread> thread;
	// used for thread synchronization purposes
	std::unique_ptr<source_sync_context> sync;
};

//
// Event processing loop
//
static falco::app::run_result do_inspect(
		falco::app::state& s,
		std::shared_ptr<sinsp> inspector,
		const std::string& source, // an empty source represents capture mode
		std::shared_ptr<stats_writer> statsw,
		syscall_evt_drop_mgr &sdropmgr,
		bool check_drops_and_timeouts,
		uint64_t duration_to_tot_ns,
		uint64_t &num_evts)
{
	int32_t rc = 0;
	sinsp_evt* ev = NULL;
	stats_writer::collector stats_collector(statsw);
	uint64_t duration_start = 0;
	uint32_t timeouts_since_last_success_or_msg = 0;
	token_bucket rate_limiter;
	bool rate_limiter_enabled = s.config->m_notifications_rate > 0;
	bool source_engine_idx_found = false;
	bool is_capture_mode = source.empty();
	bool syscall_source_engine_idx = s.source_infos.at(falco_common::syscall_source)->engine_idx;
	std::size_t source_engine_idx = 0;
	std::vector<std::string> source_names = inspector->get_plugin_manager()->sources();
	source_names.push_back(falco_common::syscall_source);
	if (!is_capture_mode)
	{
		source_engine_idx = s.source_infos.at(source)->engine_idx;
	}

	// if enabled, init rate limiter
	if (rate_limiter_enabled)
	{
		rate_limiter.init(
			s.config->m_notifications_rate,
			s.config->m_notifications_max_burst);
	}

	// reset event counter
	num_evts = 0;

	// init drop manager if we are inspecting syscalls
	if (check_drops_and_timeouts)
	{
		sdropmgr.init(inspector,
				s.outputs, // drop manager has its own rate limiting logic
				s.config->m_syscall_evt_drop_actions,
				s.config->m_syscall_evt_drop_threshold,
				s.config->m_syscall_evt_drop_rate,
				s.config->m_syscall_evt_drop_max_burst,
				s.config->m_syscall_evt_simulate_drops);
	}

	//
	// Start capture
	//
	inspector->start_capture();

	//
	// Loop through the events
	//
	while(1)
	{
		rc = inspector->next(&ev);

		if (should_reopen_outputs())
		{
			falco::app::reopen_outputs([&s]()
				{
					if(s.outputs != nullptr)
					{
						s.outputs->reopen_outputs();
					}
				});
		}

		if(should_terminate())
		{
			falco::app::terminate();
			break;
		}
		else if(should_restart())
		{
			falco::app::restart();
			break;
		}
		else if(rc == SCAP_TIMEOUT)
		{
			if(unlikely(ev == nullptr))
			{
				timeouts_since_last_success_or_msg++;
				if(timeouts_since_last_success_or_msg > s.config->m_syscall_evt_timeout_max_consecutives
					&& check_drops_and_timeouts)
				{
					std::string rule = "Falco internal: timeouts notification";
					std::string msg = rule + ". " + std::to_string(s.config->m_syscall_evt_timeout_max_consecutives) + " consecutive timeouts without event.";
					std::string last_event_time_str = "none";
					if(duration_start > 0)
					{
						sinsp_utils::ts_to_string(duration_start, &last_event_time_str, false, true);
					}
					std::map<std::string, std::string> o = {
						{"last_event_time", last_event_time_str},
					};
					auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
					s.outputs->handle_msg(now, falco_common::PRIORITY_DEBUG, msg, rule, o);
					// Reset the timeouts counter, Falco alerted
					timeouts_since_last_success_or_msg = 0;
				}
			}

			continue;
		}
		else if(rc == SCAP_FILTERED_EVENT)
		{
			continue;
		}
		else if(rc == SCAP_EOF)
		{
			break;
		}
		else if(rc != SCAP_SUCCESS)
		{
			//
			// Event read error.
			//
			return run_result::fatal(inspector->getlasterr());
		}

		// if we are in live mode, we already have the right source engine idx
		if (is_capture_mode)
		{
			source_engine_idx = syscall_source_engine_idx;
			if (ev->get_type() == PPME_PLUGINEVENT_E)
			{
				// note: here we can assume that the source index will be the same
				// in both the falco engine and the sinsp plugin manager. See the
				// comment in init_falco_engine.cpp for more details.
				source_engine_idx = inspector->get_plugin_manager()->source_idx_by_plugin_id(*(int32_t *)ev->get_param(0)->m_val, source_engine_idx_found);
				if (!source_engine_idx_found)
				{
					return run_result::fatal("Unknown plugin ID in inspector: " + std::to_string(*(int32_t *)ev->get_param(0)->m_val));
				}
			}

			// for capture mode, the source name can change at every event
			stats_collector.collect(inspector, source_names[source_engine_idx]);
		}
		else
		{
			// for live mode, the source name is constant
			stats_collector.collect(inspector, source);
		}

		// Reset the timeouts counter, Falco successfully got an event to process
		timeouts_since_last_success_or_msg = 0;
		if(duration_start == 0)
		{
			duration_start = ev->get_ts();
		}
		else if(duration_to_tot_ns > 0)
		{
			if(ev->get_ts() - duration_start >= duration_to_tot_ns)
			{
				break;
			}
		}

		if(check_drops_and_timeouts && !sdropmgr.process_event(inspector, ev))
		{
			return run_result::fatal("Drop manager internal error");
		}

		// As the inspector has no filter at its level, all
		// events are returned here. Pass them to the falco
		// engine, which will match the event against the set
		// of rules. If a match is found, pass the event to
		// the outputs.
		std::unique_ptr<falco_engine::rule_result> res = s.engine->process_event(source_engine_idx, ev);
		if(res)
		{
			if (!rate_limiter_enabled || rate_limiter.claim())
			{
				s.outputs->handle_event(res->evt, res->rule, res->source, res->priority_num, res->format, res->tags);
			}
			else
			{
				falco_logger::log(LOG_DEBUG, "Skipping rate-limited notification for rule " + res->rule + "\n");
			}
		}

		num_evts++;
	}

	return run_result::ok();
}

static void process_inspector_events(
		falco::app::state& s,
		std::shared_ptr<sinsp> inspector,
		std::shared_ptr<stats_writer> statsw,
		std::string source, // an empty source represents capture mode
		source_sync_context* sync,
		run_result* res) noexcept
{
	try
	{
		double duration;
		scap_stats cstats;
		uint64_t num_evts = 0;
		syscall_evt_drop_mgr sdropmgr;
		bool is_capture_mode = source.empty();
		bool check_drops_timeouts = is_capture_mode
			|| (source == falco_common::syscall_source && !s.is_gvisor_enabled());

		duration = ((double)clock()) / CLOCKS_PER_SEC;

		*res = do_inspect(s, inspector, source, statsw, sdropmgr, check_drops_timeouts,
						uint64_t(s.options.duration_to_tot*ONE_SECOND_IN_NS),
						num_evts);

		duration = ((double)clock()) / CLOCKS_PER_SEC - duration;

		inspector->get_capture_stats(&cstats);

		if(s.options.verbose)
		{
			if (source == falco_common::syscall_source)
			{
				fprintf(stderr, "Driver Events:%" PRIu64 "\nDriver Drops:%" PRIu64 "\n",
				cstats.n_evts,
				cstats.n_drops);
			}

			fprintf(stderr, "%sElapsed time: %.3lf, Captured Events: %" PRIu64 ", %.2lf eps\n",
				(is_capture_mode ? "" : ("("+source+") ").c_str()),
				duration,
				num_evts,
				num_evts / duration);
		}

		if (check_drops_timeouts)
		{
			sdropmgr.print_stats();
		}
	}
	catch(const std::exception& e)
	{
		*res = run_result::fatal(e.what());
	}

	if (sync)
	{
		sync->finish();
	}
}

static std::shared_ptr<stats_writer> init_stats_writer(const options& opts)
{
	auto statsw = std::make_shared<stats_writer>();
	if (!opts.stats_filename.empty())
	{
		std::string err;
		if (!stats_writer::init_ticker(opts.stats_interval, err))
		{
			throw falco_exception(err);
		}
		statsw.reset(new stats_writer(opts.stats_filename));
	}
	return statsw;
}

falco::app::run_result falco::app::actions::process_events(falco::app::state& s)
{
	run_result res = run_result::ok();
	bool termination_forced = false;

	// Notify engine that we finished loading and enabling all rules
	s.engine->complete_rule_loading();

	// Initialize stats writer
	auto statsw = init_stats_writer(s.options);

	// Start processing events
	if(s.is_capture_mode())
	{
		res = open_offline_inspector(s);
		if (!res.success)
		{
			return res;
		}

		process_inspector_events(s, s.offline_inspector, statsw, "", nullptr, &res);
		s.offline_inspector->close();

		// Honor -M also when using a trace file.
		// Since inspection stops as soon as all events have been consumed
		// just await the given duration is reached, if needed.
		if(s.options.duration_to_tot > 0)
		{
			std::this_thread::sleep_for(std::chrono::seconds(s.options.duration_to_tot));
		}
	}
	else
	{
		print_enabled_event_sources(s);

		// start event processing for all enabled sources
		falco::semaphore termination_sem(s.enabled_sources.size());
		std::vector<live_context> ctxs;
		ctxs.reserve(s.enabled_sources.size());
		for (const auto& source : s.enabled_sources)
		{
			ctxs.emplace_back();
			auto& ctx = ctxs[ctxs.size() - 1];
			ctx.source = source;
			ctx.sync.reset(new source_sync_context(termination_sem));
			auto src_info = s.source_infos.at(source);

			try
			{
				falco_logger::log(LOG_DEBUG, "Opening event source '" + source + "'\n");
				termination_sem.acquire();
				res = open_live_inspector(s, src_info->inspector, source);
				if (!res.success)
				{
					// note: we don't return here because we need to reach
					// the thread termination loop below to make sure all
					// already-spawned threads get terminated gracefully
					ctx.sync->finish();
					break;
				}

				if (s.enabled_sources.size() == 1)
				{
					// optimization: with only one source we don't spawn additional threads
					process_inspector_events(s, src_info->inspector, statsw, source, ctx.sync.get(), &ctx.res);
				}
				else
				{
					auto res_ptr = &ctx.res;
					auto sync_ptr = ctx.sync.get();
					ctx.thread.reset(new std::thread([&s, src_info, &statsw, source, sync_ptr, res_ptr](){
						process_inspector_events(s, src_info->inspector, statsw, source, sync_ptr, res_ptr);
					}));
				}
			}
			catch (std::exception &e)
			{
				// note: we don't return here because we need to reach
				// the thread termination loop below to make sure all
				// already-spawned threads get terminated gracefully
				ctx.res = run_result::fatal(e.what());
				ctx.sync->finish();
				break;
			}
		}

		// wait for event processing to terminate for all sources
		// if a thread terminates with an error, we trigger the app termination
		// to force all other event streams to termiante too.
		// We accomulate the errors in a single run_result.
		size_t closed_count = 0;
		while (closed_count < ctxs.size())
		{
			// This is shared across all running event source threads an
			// keeps the main thread sleepy until one of the parallel
			// threads terminates and invokes release(). At that point,
			// we know that at least one thread finished running and we can
			// attempt joining it. Not that this also works when only one
			// event source is enabled, in which we have no additional threads.
			termination_sem.acquire();

			if (!res.success && !termination_forced)
			{
				falco_logger::log(LOG_INFO, "An error occurred in an event source, forcing termination...\n");
				terminate(false);
				termination_forced = true;
			}

			for (auto &ctx : ctxs)
			{
				if (ctx.sync->finished() && !ctx.sync->joined())
				{
					if (ctx.thread)
					{
						if (!ctx.thread->joinable())
						{
							// thread has finished executing but
							// we already joined it, so we skip to the next one.
							// technically, we should never get here because
							// ctx.joined should already be true at this point
							continue;
						}
						ctx.thread->join();
					}

					falco_logger::log(LOG_DEBUG, "Closing event source '" + ctx.source + "'\n");
					s.source_infos.at(ctx.source)->inspector->close();

					res = run_result::merge(res, ctx.res);
					ctx.sync->join();
					closed_count++;
				}
			}
		}
	}

	s.engine->print_stats();

	return res;
}
