/*
 * Copyright (C) 2012  Politecnico di Milano
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BBQUE_RESOURCE_MANAGER_H_
#define BBQUE_RESOURCE_MANAGER_H_

#include <atomic>

#include "bbque/config.h"
#include "bbque/application_manager.h"
#include "bbque/application_proxy.h"
#include "bbque/binding_manager.h"
#include "bbque/platform_manager.h"
#include "bbque/platform_services.h"
#include "bbque/plugin_manager.h"
#include "bbque/process_manager.h"
#include "bbque/scheduler_manager.h"
#include "bbque/synchronization_manager.h"
#include "bbque/profile_manager.h"
#include "bbque/resource_accounter.h"

#include "bbque/command_manager.h"

#include "bbque/data_manager.h"

#ifdef CONFIG_BBQUE_EM
#include "bbque/em/event.h"
#include "bbque/em/event_manager.h"
#endif

#include "bbque/utils/logging/logger.h"
#include "bbque/utils/timer.h"
#include "bbque/utils/deferrable.h"
#include "bbque/utils/metrics_collector.h"
#include "bbque/utils/worker.h"

#include <bitset>
#include <map>
#include <string>

using bbque::plugins::PluginManager;
using bbque::utils::MetricsCollector;
using bbque::utils::Deferrable;
using bbque::utils::Worker;
using bbque::CommandHandler;

namespace bu = bbque::utils;

namespace bbque {

/**
 * @class ResourceManager
 * @brief The class implementing the glue logic of Barbeque
 * @ingroup sec04_rm
 *
 * This class provides the glue logic of the Barbeque RTRM. Its 'Go'
 * method represents the entry point of the run-time manager and it is
 * called by main right after some playground preparation activities.
 * This class is in charge to load all needed modules and run the control
 * loop.
 */
class ResourceManager : public CommandHandler {

public:

	/**
	 * @brief Exit codes returned by methods of this class
	 */
	typedef enum ExitCode {
		OK = 0,
		SETUP_FAILED
	} ExitCode_t;

	typedef enum controlEvent {
		EXC_START = 0,
		EXC_STOP,

		BBQ_PLAT,
		BBQ_OPTS,
		BBQ_USR1,
		BBQ_USR2,

		BBQ_EXIT,
		BBQ_ABORT,

		EVENTS_COUNT
	} controlEvent_t;

	/**
	 * @brief Get a reference to the resource manager
	 * The ResourceManager is a singleton class providing the glue logic for
	 * the Barbeque run-time resource manager. An instance to this class is to
	 * be obtained by main in order to start grilling
	 * @return  a reference to the ResourceManager singleton instance
	 */
	static ResourceManager & GetInstance();

	/**
	 * @brief  Clean-up the grill by releasing current resource manager
	 * resources and modules.
	 */
	virtual ~ResourceManager() {};

	/**
	 * @brief Start managing resources
	 * This is the actual run-time manager entry-point, after the playground
	 * setup the main should call this method to start grilling applications.
	 * This will be in charge to load all the required modules and then start
	 * the control cycle.
	 */
	ExitCode_t Go();

	/**
	 * @brief Notify an event to the resource manager
	 *
	 * This method is used to notify the resource manager about events related
	 * to system activity (e.g. stopping/killing Barbeque), applications (e.g.
	 * starting a new execution context) and resources (e.g. changed resources
	 * availability). Notified events could trigger actions within the control
	 * loop, e.g. running the optimization policy to find a new resources
	 * schedule.
	 */
	void NotifyEvent(controlEvent_t evt);


	/**
	 * @brief Register a Worker to track
	 *
	 * @param name a string to identify the worker to register
	 * @param pw a pointer to the worker to be registered
	 */
	static void Register(std::string const & name, Worker *pw);

	/**
	 * @brief Unregister a tracked Worker
	 *
	 * @param name a string identifying the worker
	 */
	static void Unregister(std::string const & name);


	/**
	 * @brief Allows external module to wait for the RM to be ready
	 */
	void WaitForReady();

private:

	/**
	 * Set true when Barbeque should terminate
	 */
	bool done;

	std::unique_ptr<bu::Logger> logger;

	/**
	 * @brief Reference to supported platform services class.
	 * The platform services are a set of services exported by Barbeque to
	 * other modules (and plugins). The resource manager ensure an
	 * initialization of this core module before starting to grill.
	 */
	PlatformServices & ps;

	ApplicationManager & am;

	ApplicationProxy & ap;

	/**
	 * @brief Reference to the plugin manager module.
	 * The plugin manager is the required interface to load other modules. The
	 * resource manager ensure an initialization of this core module before
	 * starting to grill.
	 */
	PluginManager & pm;

	ResourceAccounter & ra;

	BindingManager & bdm;

	MetricsCollector & mc;

	PlatformManager & plm;

#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
	ProcessManager & prm;
#endif // CONFIG_BBQUE_LINUX_PROC_MANAGER

	CommandManager & cm;

	SchedulerManager & sm;

	SynchronizationManager & ym;

#ifdef CONFIG_BBQUE_DM
	DataManager & dm;
#endif

#ifdef CONFIG_BBQUE_SCHED_PROFILING
	ProfileManager & om;
#endif

#ifdef CONFIG_BBQUE_EM
	em::EventManager & em;
#endif

	/**
	 * @ brief Events to be managed
	 */
	std::bitset<EVENTS_COUNT> pendingEvts;

	std::mutex pendingEvts_mtx;

	std::condition_variable pendingEvts_cv;

	/**
	 * @ brief If not ready, an optimization is in progress
	 */
	bool is_ready = true;

	std::mutex status_mtx;

	std::condition_variable status_cv;

	/**
	 * @brief The map of regitered Worker
	 */
	static std::map<std::string, Worker*> workers_map;

	/**
	 * @brief A mutex controlling access to the workers map
	 */
	static std::mutex workers_mtx;

	/**
	 * @brief A conditional variable used by workers management code
	 */
	static std::condition_variable workers_cv;

	/**
	 * @brief The collection of metrics generated by this module
	 */
	typedef enum ResMgrMetrics {

		//----- Counting metrics
		RM_EVT_TOTAL = 0,
		RM_EVT_START,
		RM_EVT_STOP,
		RM_EVT_PLAT,
		RM_EVT_OPTS,
		RM_EVT_USR1,
		RM_EVT_USR2,

		RM_SCHED_TOTAL,
		RM_SCHED_FAILED,
		RM_SCHED_DELAYED,
		RM_SCHED_EMPTY,

		RM_SYNCH_TOTAL,
		RM_SYNCH_FAILED,

		//----- Sample statistics
		RM_EVT_TIME,
		RM_EVT_TIME_START,
		RM_EVT_TIME_STOP,
		RM_EVT_TIME_PLAT,
		RM_EVT_TIME_OPTS,
		RM_EVT_TIME_USR1,
		RM_EVT_TIME_USR2,

		RM_EVT_PERIOD,
		RM_EVT_PERIOD_START,
		RM_EVT_PERIOD_STOP,
		RM_EVT_PERIOD_PLAT,
		RM_EVT_PERIOD_OPTS,
		RM_EVT_PERIOD_USR1,
		RM_EVT_PERIOD_USR2,

		RM_SCHED_PERIOD,
		RM_SYNCH_PERIOD,

		RM_METRICS_COUNT
	} ResMgrMetrics_t;

	/** The High-Resolution timer used for profiling */
	Timer rm_tmr;

	/** The metrics collected by this module */
	static MetricsCollector::MetricsCollection_t metrics[RM_METRICS_COUNT];

	/**
	 * @brief   Build a new instance of the resource manager
	 */
	ResourceManager();

	/**
	 * @brief Resource optimizer deferrable
	 *
	 * This is usded to collect and aggregate optimization requests.
	 * The optimization will be performed by a call of the Optimize
	 * method.
	 */
	Deferrable optimize_dfr;

	/**
	 * @brief The interval [ms] of activation of the periodic optimization
	 *
	 * The time interval in [ms] for the periodic optimization is the
	 * minimum granted time period for the activation of a new
	 * optimization run. If this value is null, than the optimization is
	 * triggerted just by events.
	 */
	uint32_t opt_interval;

	/**
	 * @brief The optimization request has been triggered by a platform event?
	 *
	 * In case of events due to platform critical conditions (e.g., thermal threshold,
	 * fast discharging rate, offlining of resources, degradation notifications,...)
	 * the optimization must be executed independently from having active applications
	 * or not. This because generally the policy can include power management actions
	 * other than application scheduling.
	 */
	std::atomic<bool> plat_event;


	// By default we use an event based activation of optimizations
#define BBQUE_DEFAULT_RESOURCE_MANAGER_OPT_INTERVAL 0

	/**
	 * @brief Set to ready or not ready, depending on having an optimization in
	 * progress or not
	 */
	void SetReady(bool value);

	/**
	 * @brief   Run on optimization cycle (i.e. Schedule and Synchronization)
	 * Once an event happens which impacts on resources usage or availability
	 * an optimization cycle could be triggered by calling this method.
	 * An optimization cycles involve a run of the resource scheduler policy
	 * (e.g. YaMCA)  and an eventual Synchroniation of the active applications
	 * according to the currently loaded synchronization policy (e.g. SASB).
	 */
	void Optimize();

	/**
	 * @brief   The run-time resource manager setup routine
	 * This provides all the required playground setup to run the Barbeque RTRM.
	 */
	ExitCode_t Setup();

	/**
	 * @brief   The run-time resource manager control loop
	 * This provides the Barbeuqe applications and resources control logic.
	 * This is actually the entry point of the Barbeque state machine.
	 */
	void ControlLoop();

	/**
	 * @brief Process a EXC_START event
	 */
	void EvtExcStart();

	/**
	 * @brief Process a BBQ_PALT event
	 */
	void EvtBbqPlat();

	/**
	 * @brief Process a BBQ_OPTS event
	 */
	void EvtBbqOpts();

	/**
	 * @brief Process a BBQ_USR1 event
	 */
	void EvtBbqUsr1();

	/**
	 * @brief Process a BBQ_USR1 event
	 */
	void EvtBbqUsr2();

	/**
	 * @brief Process a EXC_STOP event
	 */
	void EvtExcStop();

	/**
	 * @brief Process a BBQ_EXIT event
	 */
	void EvtBbqExit();

	/**
	 * @brief The handler for commands defined by this module
	 */
	int CommandsCb(int argc, char *argv[]);

	/**
	 * @brief Terminate all the registered Worker
	 */
	static void TerminateWorkers();

};

} // namespace bbque

#endif // BBQUE_RESOURCE_MANAGER_H_
