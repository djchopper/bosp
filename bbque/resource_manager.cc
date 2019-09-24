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

#include "bbque/resource_manager.h"

#include "bbque/application_manager.h"
#include "bbque/configuration_manager.h"
#include "bbque/signals_manager.h"
#include "bbque/utils/utility.h"

#ifdef CONFIG_BBQUE_WM
#include "bbque/power_monitor.h"
#endif

#define RESOURCE_MANAGER_NAMESPACE "bq.rm"
#define MODULE_NAMESPACE RESOURCE_MANAGER_NAMESPACE

/** Metrics (class COUNTER) declaration */
#define RM_COUNTER_METRIC(NAME, DESC)\
 {RESOURCE_MANAGER_NAMESPACE "." NAME, DESC, \
	 MetricsCollector::COUNTER, 0, NULL, 0}
/** Increase counter for the specified metric */
#define RM_COUNT_EVENT(METRICS, INDEX) \
	mc.Count(METRICS[INDEX].mh);
/** Increase counter for the specified metric */
#define RM_COUNT_EVENTS(METRICS, INDEX, AMOUNT) \
	mc.Count(METRICS[INDEX].mh, AMOUNT);

/** Metrics (class SAMPLE) declaration */
#define RM_SAMPLE_METRIC(NAME, DESC)\
 {RESOURCE_MANAGER_NAMESPACE "." NAME, DESC, \
	 MetricsCollector::SAMPLE, 0, NULL, 0}
/** Reset the timer used to evaluate metrics */
#define RM_RESET_TIMING(TIMER) \
	TIMER.start();
/** Acquire a new time sample */
#define RM_GET_TIMING(METRICS, INDEX, TIMER) \
	mc.AddSample(METRICS[INDEX].mh, TIMER.getElapsedTimeMs());
/** Acquire a new (generic) sample */
#define RM_ADD_SAMPLE(METRICS, INDEX, SAMPLE) \
	mc.AddSample(METRICS[INDEX].mh, SAMPLE);

/** Metrics (class PERIDO) declaration */
#define RM_PERIOD_METRIC(NAME, DESC)\
 {RESOURCE_MANAGER_NAMESPACE "." NAME, DESC, \
	 MetricsCollector::PERIOD, 0, NULL, 0}
/** Acquire a new time sample */
#define RM_GET_PERIOD(METRICS, INDEX, PERIOD) \
	mc.PeriodSample(METRICS[INDEX].mh, PERIOD);

namespace br = bbque::res;
namespace bu = bbque::utils;
namespace bp = bbque::plugins;
namespace po = boost::program_options;

using bp::PluginManager;
using bu::MetricsCollector;
using std::chrono::milliseconds;

namespace bbque {

/* Definition of metrics used by this module */
MetricsCollector::MetricsCollection_t
ResourceManager::metrics[RM_METRICS_COUNT] = {

	//----- Event counting metrics
	RM_COUNTER_METRIC("evt.tot",	"Total events"),
	RM_COUNTER_METRIC("evt.start",	"  START events"),
	RM_COUNTER_METRIC("evt.stop",	"  STOP  events"),
	RM_COUNTER_METRIC("evt.plat",	"  PALT  events"),
	RM_COUNTER_METRIC("evt.opts",	"  OPTS  events"),
	RM_COUNTER_METRIC("evt.usr1",	"  USR1  events"),
	RM_COUNTER_METRIC("evt.usr2",	"  USR2  events"),

	RM_COUNTER_METRIC("sch.tot",	"Total Scheduler activations"),
	RM_COUNTER_METRIC("sch.failed",	"  FAILED  schedules"),
	RM_COUNTER_METRIC("sch.delayed","  DELAYED schedules"),
	RM_COUNTER_METRIC("sch.empty",	"  EMPTY   schedules"),

	RM_COUNTER_METRIC("syn.tot",	"Total Synchronization activations"),
	RM_COUNTER_METRIC("syn.failed",	"  FAILED synchronizations"),

	//----- Sampling statistics
	RM_SAMPLE_METRIC("evt.avg.time",  "Avg events processing t[ms]"),
	RM_SAMPLE_METRIC("evt.avg.start", "  START events"),
	RM_SAMPLE_METRIC("evt.avg.stop",  "  STOP  events"),
	RM_SAMPLE_METRIC("evt.avg.plat",  "  PLAT  events"),
	RM_SAMPLE_METRIC("evt.avg.opts",  "  OPTS  events"),
	RM_SAMPLE_METRIC("evt.avg.usr1",  "  USR1  events"),
	RM_SAMPLE_METRIC("evt.avg.usr2",  "  USR2  events"),

	RM_PERIOD_METRIC("evt.per",		"Avg events period t[ms]"),
	RM_PERIOD_METRIC("evt.per.start",	"  START events"),
	RM_PERIOD_METRIC("evt.per.stop",	"  STOP  events"),
	RM_PERIOD_METRIC("evt.per.plat",	"  PLAT  events"),
	RM_PERIOD_METRIC("evt.per.opts",	"  OPTS  events"),
	RM_PERIOD_METRIC("evt.per.usr1",	"  USR1  events"),
	RM_PERIOD_METRIC("evt.per.usr2",	"  USR2  events"),

	RM_PERIOD_METRIC("sch.per",   "Avg Scheduler period t[ms]"),
	RM_PERIOD_METRIC("syn.per",   "Avg Synchronization period t[ms]"),

};


ResourceManager & ResourceManager::GetInstance() {
	static ResourceManager rtrm;

	return rtrm;
}

ResourceManager::ResourceManager() :
	ps(PlatformServices::GetInstance()),
	am(ApplicationManager::GetInstance()),
	ap(ApplicationProxy::GetInstance()),
	pm(PluginManager::GetInstance()),
	ra(ResourceAccounter::GetInstance()),
	bdm(BindingManager::GetInstance()),
	mc(MetricsCollector::GetInstance()),
	plm(PlatformManager::GetInstance()),
#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
	prm(ProcessManager::GetInstance()),
#endif // CONFIG_BBQUE_LINUX_PROC_MANAGER
	cm(CommandManager::GetInstance()),
	sm(SchedulerManager::GetInstance()),
	ym(SynchronizationManager::GetInstance()),
#ifdef CONFIG_BBQUE_DM
	dm(DataManager::GetInstance()),
#endif
#ifdef CONFIG_BBQUE_SCHED_PROFILING
	om(ProfileManager::GetInstance()),
#endif
#ifdef CONFIG_BBQUE_EM
	em(em::EventManager::GetInstance()),
#endif

	optimize_dfr("rm.opt", std::bind(&ResourceManager::Optimize, this)) {

	plat_event = false;

	//---------- Setup all the module metrics
	mc.Register(metrics, RM_METRICS_COUNT);

	//---------- Register commands
	CommandManager &cm = CommandManager::GetInstance();
#define CMD_STATUS_EXC ".exc_status"
	cm.RegisterCommand(MODULE_NAMESPACE CMD_STATUS_EXC, static_cast<CommandHandler*>(this),
			"Dump the status of each registered EXC");
#define CMD_STATUS_QUEUES ".que_status"
	cm.RegisterCommand(MODULE_NAMESPACE CMD_STATUS_QUEUES, static_cast<CommandHandler*>(this),
			"Dump the status of each Scheduling Queue");
#define CMD_STATUS_RESOURCES ".res_status"
	cm.RegisterCommand(MODULE_NAMESPACE CMD_STATUS_RESOURCES, static_cast<CommandHandler*>(this),
			"Dump the status of each registered Resource");
#define CMD_STATUS_SYNC ".syn_status"
	cm.RegisterCommand(MODULE_NAMESPACE CMD_STATUS_SYNC, static_cast<CommandHandler*>(this),
			"Dump the status of each Synchronization Queue");
#define CMD_OPT_FORCE ".opt_force"
	cm.RegisterCommand(MODULE_NAMESPACE CMD_OPT_FORCE, static_cast<CommandHandler*>(this),
			"Force a new scheduling event");

#ifdef CONFIG_BBQUE_EM
	em::Event event(true, "rm", "", "barbeque", "__startup", 1);
	em.InitializeArchive(event);
#endif

}

ResourceManager::ExitCode_t
ResourceManager::Setup() {

	//---------- Get a logger module
	logger = bu::Logger::GetLogger(RESOURCE_MANAGER_NAMESPACE);
	assert(logger);

	//---------- Loading configuration
	ConfigurationManager & cm = ConfigurationManager::GetInstance();
	po::options_description opts_desc("Resource Manager Options");
	opts_desc.add_options()
		("ResourceManager.opt_interval",
		 po::value<uint32_t>
		 (&opt_interval)->default_value(
			 BBQUE_DEFAULT_RESOURCE_MANAGER_OPT_INTERVAL),
		 "The interval [ms] of activation of the periodic optimization")
		;
	po::variables_map opts_vm;
	cm.ParseConfigurationFile(opts_desc, opts_vm);

	//---------- Dump list of registered plugins
	const bp::PluginManager::RegistrationMap & rm = pm.GetRegistrationMap();
	logger->Info("RM: Registered plugins:");
	bp::PluginManager::RegistrationMap::const_iterator i;
	for (i = rm.begin(); i != rm.end(); ++i)
		logger->Info(" * %s", (*i).first.c_str());

	//---------- Init Platform Integration Layer (PIL)
	PlatformManager::ExitCode_t result = plm.LoadPlatformConfig();
	if (result != PlatformManager::PLATFORM_OK) {
		logger->Fatal("Platform Configuration Loader FAILED!");
		return SETUP_FAILED;
	}

	result = plm.LoadPlatformData();
	if (result != PlatformManager::PLATFORM_OK) {
		logger->Fatal("Platform Integration Layer initialization FAILED!");
		return SETUP_FAILED;
	}

	// -------- Binding Manager initialization for the scheduling policy
	if (bdm.LoadBindingDomains() != BindingManager::OK) {
		logger->Fatal("Binding Manager initialization FAILED!");
		return SETUP_FAILED;
	}

#ifdef CONFIG_BBQUE_WM
	//----------- Start the Power Monitor
	PowerMonitor & wm(PowerMonitor::GetInstance());
	wm.Start();
#endif
	//---------- Start bbque services
	plm.Start();
	if (opt_interval)
		optimize_dfr.SetPeriodic(milliseconds(opt_interval));

	return OK;
}

void ResourceManager::NotifyEvent(controlEvent_t evt) {
	// Ensure we have a valid event
	logger->Debug("NotifyEvent: received event = %d", evt);
	assert(evt<EVENTS_COUNT);

	// Set the corresponding event flag
	std::unique_lock<std::mutex> pendingEvts_ul(pendingEvts_mtx, std::defer_lock);
	pendingEvts.set(evt);

	// Notify the control loop (just if it is sleeping)
	if (pendingEvts_ul.try_lock()) {
		logger->Debug("NotifyEvent: notifying %d", evt);
		pendingEvts_cv.notify_one();
	}
	else {
		logger->Debug("NotifyEvent: NOT notifying %d", evt);
	}
}

std::map<std::string, Worker*> ResourceManager::workers_map;
std::mutex ResourceManager::workers_mtx;
std::condition_variable ResourceManager::workers_cv;

void ResourceManager::Register(std::string const & name, Worker *pw) {
	std::unique_lock<std::mutex> workers_ul(workers_mtx);
	fprintf(stderr, FI("Registering Worker[%s]...\n"), name.c_str());
	workers_map[name] = pw;
}

void ResourceManager::Unregister(std::string const & name) {
	std::unique_lock<std::mutex> workers_ul(workers_mtx);
	fprintf(stderr, FI("Unregistering Worker[%s]...\n"), name.c_str());
	workers_map.erase(name);
	workers_cv.notify_one();
}

void ResourceManager::WaitForReady() {
	std::unique_lock<std::mutex> status_ul(status_mtx);
	while (!is_ready) {
		logger->Debug("WaitForReady: an optimization is in progress...");
		status_cv.wait(status_ul);
	}
}

void ResourceManager::SetReady(bool value) {
	logger->Debug("SetReady: %s", value ? "true": "false");
	std::unique_lock<std::mutex> status_ul(status_mtx);
	is_ready = value;
	if (value) {
		status_cv.notify_all();
		logger->Notice("SetReady: optimization terminated");
	}
}

void ResourceManager::TerminateWorkers() {
	std::unique_lock<std::mutex> workers_ul(workers_mtx);
	std::chrono::milliseconds timeout = std::chrono::milliseconds(300);

	// Waiting for all workers to be terminated
	for (uint8_t i = 3; i; --i) {

		// Signal all registered Workers to terminate
		for_each(workers_map.begin(), workers_map.end(),
				[=](std::pair<std::string, Worker*> entry) {
				fprintf(stderr, FI("Terminating Worker[%s]...\n"),
					entry.first.c_str());
				entry.second->Terminate();
				});

		// Wait up to 300[ms] for workers to terminate
		workers_cv.wait_for(workers_ul, timeout);
		if (workers_map.empty())
			break;

		DB(fprintf(stderr, FD("Waiting for [%lu] workers to terminate...\n"),
					workers_map.size()));

	}

	DB(fprintf(stderr, FD("All workers terminated\n")));
}

void ResourceManager::Optimize() {
	SynchronizationManager::ExitCode_t syncResult;
	SchedulerManager::ExitCode_t schedResult;
	static bu::Timer optimization_tmr;
	double period;
	bool active_apps = true;

	SetReady(false);

	// If the optimization has been triggered by a platform event (BBQ_PLAT) the policy must be
	// executed anyway. To the contrary, if it is an application event (BBQ_OPTS) check if
	// there are actually active applications
	if (!plat_event &&
		!am.HasApplications(Application::READY) &&
		!am.HasApplications(Application::RUNNING)
#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
		&&
		!prm.HasProcesses(Schedulable::READY) &&
		!prm.HasProcesses(Schedulable::RUNNING)
#endif // CONFIG_BBQUE_LINUX_PROC_MANAGER
	) {
		logger->Debug("Optimize: nothing to schedule...");
		active_apps = false;
	}

	plat_event = false;

	if (active_apps) {
		ra.PrintStatusReport();
		am.PrintStatus();
#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
		prm.PrintStatus(true);
#endif
		logger->Info("Optimize: lauching scheduler...");

		// Account for a new schedule activation
		RM_COUNT_EVENT(metrics, RM_SCHED_TOTAL);
		RM_GET_PERIOD(metrics, RM_SCHED_PERIOD, period);

		//--- Scheduling
		optimization_tmr.start();
		schedResult = sm.Schedule();
		optimization_tmr.stop();
		switch(schedResult) {
		case SchedulerManager::MISSING_POLICY:
		case SchedulerManager::FAILED:
			logger->Warn("Schedule FAILED (Error: scheduling policy failed)");
			RM_COUNT_EVENT(metrics, RM_SCHED_FAILED);
			SetReady(true);
			return;
		case SchedulerManager::DELAYED:
			logger->Error("Schedule DELAYED");
			RM_COUNT_EVENT(metrics, RM_SCHED_DELAYED);
			SetReady(true);
			return;
		default:
			assert(schedResult == SchedulerManager::DONE);
		}
		logger->Notice("Schedule Time: %11.3f[us]", optimization_tmr.getElapsedTimeUs());
		am.PrintStatus(true);
#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
		prm.PrintStatus(true);
#endif


	}

	// Check if there is at least one application to synchronize
	if (!am.HasApplications(Application::SYNC)
#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
		&&
		!prm.HasProcesses(Schedulable::SYNC)
#endif // CONFIG_BBQUE_LINUX_PROC_MANAGER
	) {
		logger->Debug("Optimize: no applications in SYNC state");
		RM_COUNT_EVENT(metrics, RM_SCHED_EMPTY);
	} else {
		// Account for a new synchronizaiton activation
		RM_COUNT_EVENT(metrics, RM_SYNCH_TOTAL);
		RM_GET_PERIOD(metrics, RM_SYNCH_PERIOD, period);
		if (period)
			logger->Notice("Schedule Run-time: %9.3f[ms]", period);

		//--- Synchronization
		optimization_tmr.start();
		syncResult = ym.SyncSchedule();
		optimization_tmr.stop();
		if (syncResult != SynchronizationManager::OK) {
			RM_COUNT_EVENT(metrics, RM_SYNCH_FAILED);
			// FIXME here we should implement some counter-meaure to
			// ensure consistency
		}
		ra.PrintStatusReport(0, true);
		am.PrintStatus(true);
#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
		prm.PrintStatus(true);
#endif
		logger->Notice("Sync Time: %11.3f[us]", optimization_tmr.getElapsedTimeUs());
	}

#ifdef CONFIG_BBQUE_SCHED_PROFILING
	//--- Profiling
	logger->Debug(LNPROB);
	optimization_tmr.start();
	ProfileManager::ExitCode_t profResult = om.ProfileSchedule();
	optimization_tmr.stop();
	if (profResult != ProfileManager::OK) {
		logger->Warn("Scheduler profiling FAILED");
	}
	logger->Debug(LNPROE);
	logger->Debug("Prof Time: %11.3f[us]", optimization_tmr.getElapsedTimeUs());
#else
	logger->Debug("Scheduling profiling disabled");
#endif
	SetReady(true);

#ifdef CONFIG_BBQUE_DM
	dm.NotifyUpdate(stat::EVT_SCHEDULING);
#endif
}

void ResourceManager::EvtExcStart() {
	uint32_t timeout = 0;

	logger->Info("EXC Enabled");

	// Reset timer for START event execution time collection
	RM_RESET_TIMING(rm_tmr);

	// This is a simple optimization triggering policy based on the
	// current priority level of READY applications.
	// When an application issue a Working Mode request it is expected to
	// be in ready state. The optimization will be triggered in a
	// timeframe which is _invese proportional_ to the highest priority
	// ready application.
	// This should allows to have short latencies for high priority apps
	// while still allowing for reduced rescheduling on applications
	// startup burst.
	// TODO: make this policy more tunable via the configuration file
	AppPtr_t papp = am.HighestPrio(ApplicationStatusIF::READY);
	if (!papp) {
		// In this case the application has exited before the start
		// event has had the change to be processed
		DB(logger->Warn("Overdue processing of a START event"));
		return;
	}
	timeout = BBQUE_RM_OPT_EXC_START_DEFER_MS;
	optimize_dfr.Schedule(milliseconds(timeout));

	// Collecing execution metrics
	RM_GET_TIMING(metrics, RM_EVT_TIME_START, rm_tmr);
}

void ResourceManager::EvtExcStop() {
	uint32_t timeout = 0;

	logger->Info("EXC Disabled");

	// Reset timer for START event execution time collection
	RM_RESET_TIMING(rm_tmr);

	// This is a simple optimization triggering policy
	timeout = BBQUE_RM_OPT_EXC_STOP_DEFER_MS;
	optimize_dfr.Schedule(milliseconds(timeout));

	// Collecing execution metrics
	RM_GET_TIMING(metrics, RM_EVT_TIME_STOP, rm_tmr);
}

void ResourceManager::EvtBbqPlat() {

	logger->Info("BarbequeRTRM Optimization Request for Platform Event");
	plat_event = true;

	// Reset timer for START event execution time collection
	RM_RESET_TIMING(rm_tmr);

	// Platform generated events triggers an immediate rescheduling.
	// TODO add a better policy which triggers immediate rescheduling only
	// on resources reduction. Perhaps such a policy could be plugged into
	// the PlatformProxy module.
	optimize_dfr.Schedule();

	// Collecing execution metrics
	RM_GET_TIMING(metrics, RM_EVT_TIME_PLAT, rm_tmr);
}

void ResourceManager::EvtBbqOpts() {
	uint32_t timeout = 0;

	logger->Info("BarbequeRTRM Optimization Request for Application Event");

	// Reset timer for START event execution time collection
	RM_RESET_TIMING(rm_tmr);

	// Explicit applications requests for optimization are delayed by
	// default just to increase the chance for aggregation of multiple
	// requests
	timeout = BBQUE_RM_OPT_REQUEST_DEFER_MS;
	optimize_dfr.Schedule(milliseconds(timeout));

	// Collecing execution metrics
	RM_GET_TIMING(metrics, RM_EVT_TIME_OPTS, rm_tmr);
}


void ResourceManager::EvtBbqUsr1() {

	// Reset timer for START event execution time collection
	RM_RESET_TIMING(rm_tmr);

	logger->Info("");
	logger->Info("==========[ Status Queues ]============"
			"========================================");
	logger->Info("");
	am.PrintStatusQ(true);

	logger->Info("");
	logger->Info("");
	logger->Info("==========[ Synchronization Queues ]==="
			"========================================");
	logger->Info("");
	am.PrintSyncQ(true);

	logger->Notice("");
	logger->Notice("");
	logger->Notice("==========[ Resources Status ]========="
			"========================================");
	logger->Notice("");
	ra.PrintStatusReport(0, true);

	logger->Notice("");
	logger->Notice("");
	logger->Notice("==========[ EXCs Status ]=============="
			"========================================");
	logger->Notice("");
	am.PrintStatus(true);
#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
	prm.PrintStatus(true);
#endif

	// Clear the corresponding event flag
	logger->Notice("");
	pendingEvts.reset(BBQ_USR1);

	// Collecing execution metrics
	RM_GET_TIMING(metrics, RM_EVT_TIME_USR1, rm_tmr);
}

void ResourceManager::EvtBbqUsr2() {

	// Reset timer for START event execution time collection
	RM_RESET_TIMING(rm_tmr);

	logger->Debug("Dumping metrics collection...");
	mc.DumpMetrics();

	// Clear the corresponding event flag
	pendingEvts.reset(BBQ_USR2);

	// Collecing execution metrics
	RM_GET_TIMING(metrics, RM_EVT_TIME_USR2, rm_tmr);
}

void ResourceManager::EvtBbqExit() {
	AppsUidMapIt apps_it;
	AppPtr_t papp;

	logger->Notice("Terminating BarbequeRTRM...");
	done = true;
	pendingEvts_cv.notify_one();

	// Dumping collected stats before termination
	EvtBbqUsr1();
	EvtBbqUsr2();

	// Stop applications
	papp = am.GetFirst(apps_it);
	for ( ; papp; papp = am.GetNext(apps_it)) {
		logger->Notice("Terminating application: %s", papp->StrId());
		ap.StopExecution(papp);
		am.DisableEXC(papp, true);
		am.DestroyEXC(papp);
	}

	// Terminate platforms
	logger->Notice("Terminating the platform supports...");
	plm.Exit();

	// Notify all workers
	logger->Notice("Stopping all workers...");
	ResourceManager::TerminateWorkers();
}

int ResourceManager::CommandsCb(int argc, char *argv[]) {
	uint8_t cmd_offset = ::strlen(MODULE_NAMESPACE) + 1;
	// Fix compiler warnings
	(void)argc;

	logger->Debug("Processing command [%s]", argv[0] + cmd_offset);

	bool exit_cmd_not_found = false;

	switch (argv[0][cmd_offset]) {
	case 'e':
		if (strcmp(argv[0], MODULE_NAMESPACE CMD_STATUS_EXC)) {
			exit_cmd_not_found = true;
			break;
		}

		logger->Notice("");
		logger->Notice("");
		logger->Notice("==========[ EXCs Status ]=============="
				"========================================");
		logger->Notice("");
		am.PrintStatus(true);
#ifdef CONFIG_BBQUE_LINUX_PROC_MANAGER
		prm.PrintStatus(true);
#endif
		break;

	case 'q':
		if (strcmp(argv[0], MODULE_NAMESPACE CMD_STATUS_QUEUES)) {
			exit_cmd_not_found = true;
			break;
		}

		logger->Info("");
		logger->Info("==========[ Status Queues ]============"
				"========================================");
		logger->Info("");
		am.PrintStatusQ(true);
		break;

	case 'r':
		if (strcmp(argv[0], MODULE_NAMESPACE CMD_STATUS_RESOURCES)) {
			exit_cmd_not_found = true;
			break;
		}

		logger->Notice("");
		logger->Notice("");
		logger->Notice("==========[ Resources Status ]========="
				"========================================");
		logger->Notice("");
		ra.PrintStatusReport(0, true);
		break;

	case 's':
		if (strcmp(argv[0], MODULE_NAMESPACE CMD_STATUS_SYNC)) {
			exit_cmd_not_found = true;
			break;
		}

		logger->Info("");
		logger->Info("");
		logger->Info("==========[ Synchronization Queues ]==="
				"========================================");
		logger->Info("");
		am.PrintSyncQ(true);
		break;
	case 'o':
		logger->Info("");
		logger->Info("");
		logger->Info("==========[ User Required Scheduling ]==="
				"===============================");
		logger->Info("");
		NotifyEvent(ResourceManager::BBQ_OPTS);
		break;
	}

	if ( ! exit_cmd_not_found ) {
		return 0;
	}

	logger->Error("Command [%s] not suppported by this moduel", argv[0]);
	return -1;
}


void ResourceManager::ControlLoop() {
	std::unique_lock<std::mutex> pendingEvts_ul(pendingEvts_mtx);
	double period;

	// Wait for a new event
	while (!pendingEvts.any()) {
	    for (int i = 0; i<pendingEvts.size(); ++i)
	        logger->Debug("pending events[%d] is: %d", i, pendingEvts.test(i));
	    //logger->Debug("pending events: %s", pendingEvts.to_string());
		logger->Debug("Control Loop: no events");
		pendingEvts_cv.wait(pendingEvts_ul);
	}

	if (done == true) {
		logger->Warn("Control Loop: returning");
		return;
	}

	// Checking for pending events, starting from higer priority ones.
	for(uint8_t evt=EVENTS_COUNT; evt; --evt) {

		logger->Debug("Checking events [%d:%s]",
				evt-1, pendingEvts[evt-1] ? "Pending" : "None");

		// Jump not pending events
		if (!pendingEvts[evt-1])
			continue;

		// Resetting event
		pendingEvts.reset(evt-1);

		// Account for a new event
		RM_COUNT_EVENT(metrics, RM_EVT_TOTAL);
		RM_GET_PERIOD(metrics, RM_EVT_PERIOD, period);

		// Dispatching events to handlers
		switch(evt-1) {
		case EXC_START:
			logger->Debug("Event [EXC_START]");
			EvtExcStart();
			RM_COUNT_EVENT(metrics, RM_EVT_START);
			RM_GET_PERIOD(metrics, RM_EVT_PERIOD_START, period);
			break;
		case EXC_STOP:
			logger->Debug("Event [EXC_STOP]");
			EvtExcStop();
			RM_COUNT_EVENT(metrics, RM_EVT_STOP);
			RM_GET_PERIOD(metrics, RM_EVT_PERIOD_STOP, period);
			break;
		// Platform reconfiguration or warning conditions requiring a policy execution
		case BBQ_PLAT:
			logger->Debug("Event [BBQ_PLAT]");
			EvtBbqPlat();
			RM_COUNT_EVENT(metrics, RM_EVT_PLAT);
			RM_GET_PERIOD(metrics, RM_EVT_PERIOD_PLAT, period);
			break;
		// Application-driven policy execution request
		case BBQ_OPTS:
			logger->Debug("Event [BBQ_OPTS]");
			EvtBbqOpts();
			RM_COUNT_EVENT(metrics, RM_EVT_OPTS);
			RM_GET_PERIOD(metrics, RM_EVT_PERIOD_OPTS, period);
			break;
		case BBQ_USR1:
			logger->Debug("Event [BBQ_USR1]");
			RM_COUNT_EVENT(metrics, RM_EVT_USR1);
			RM_GET_PERIOD(metrics, RM_EVT_PERIOD_USR1, period);
			EvtBbqUsr1();
			return;
		case BBQ_USR2:
			logger->Debug("Event [BBQ_USR2]");
			RM_COUNT_EVENT(metrics, RM_EVT_USR2);
			RM_GET_PERIOD(metrics, RM_EVT_PERIOD_USR2, period);
			EvtBbqUsr2();
			return;
		case BBQ_EXIT:
			logger->Debug("Event [BBQ_EXIT]");
			EvtBbqExit();
			return;
		case BBQ_ABORT:
			logger->Debug("Event [BBQ_ABORT]");
			logger->Fatal("Abortive quit");
			exit(EXIT_FAILURE);
		default:
			logger->Crit("Unhandled event [%d]", evt-1);
		}
	}
}

ResourceManager::ExitCode_t
ResourceManager::Go() {
	ExitCode_t result;

	result = Setup();
	if (result != OK)
		return result;

	while (!done) {
		ControlLoop();
	}

	return OK;
}

} // namespace bbque

