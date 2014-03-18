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

#ifndef BBQUE_YAMCA_SCHEDPOL_H_
#define BBQUE_YAMCA_SCHEDPOL_H_

#include <cstdint>

#include "bbque/scheduler_manager.h"
#include "bbque/plugins/scheduler_policy.h"
#include "bbque/plugins/plugin.h"
#include "bbque/utils/logging/logger.h"

#define SCHEDULER_POLICY_NAME "yamca"
#define MODULE_NAMESPACE SCHEDULER_POLICY_NAMESPACE "." SCHEDULER_POLICY_NAME
#define MODULE_CONFIG SCHEDULER_POLICY_CONFIG "." SCHEDULER_POLICY_NAME

#define RSRC_CLUSTER "sys.cpu"

namespace bu = bbque::utils;

using bbque::res::RViewToken_t;
using bbque::utils::Timer;
using bbque::utils::MetricsCollector;

// These are the parameters received by the PluginManager on create calls
struct PF_ObjectParams;

namespace bbque { namespace plugins {

/**
 * @brief The YaMCA resource scheduler heuristic
 *
 * A dynamic C++ plugin which implements the YaMCA resource scheduler heuristic.
 */
class YamcaSchedPol: public SchedulerPolicyIF {

public:

	/** The scheduling entity*/
	typedef std::pair<AppCPtr_t, AwmPtr_t> SchedEntity_t;

	/** Map for ordering the scheduling entities */
	typedef std::multimap<float, SchedEntity_t> SchedEntityMap_t;

//----- static plugin interface

	/**
	 *
	 */
	static void * Create(PF_ObjectParams *);

	/**
	 *
	 */
	static int32_t Destroy(void *);

	/**
	 * @brief Destructor
	 */
	virtual ~YamcaSchedPol();

//----- Scheduler Policy module interface

	/**
	 * @see SchedulerPolicyIF
	 */
	char const * Name();

	/**
	 * @see ScheduerPolicyIF
	 */
	SchedulerPolicyIF::ExitCode_t
		Schedule(bbque::System & sv, RViewToken_t & rav);

private:

	/** System logger instance */
	std::unique_ptr<bu::Logger> logger;

	/** Resource accounter instance */
	ResourceAccounter & rsrc_acct;

	/** Token for accessing a resources view */
	RViewToken_t rsrc_view_token = 0;

	/** A counter used for getting always a new clean resources view */
	uint32_t tok_counter = 0;

	/** Number of clusters on the platform	 */
	uint64_t num_clusters = 0;

	/** Keep track the clusters without available PEs */
	std::vector<bool> clusters_full;

	/** Metric collector instance */
	MetricsCollector & mc;

	std::mutex sched_mtx;

	/**
	 * @brief The collection of metrics generated by this module
	 */
	typedef enum SchedPolMetrics {
		//----- Value metrics
		YAMCA_SCHEDMAP_SIZE = 0,
		YAMCA_NUM_ENTITY,
		//----- Timing metrics
		YAMCA_ORDER_TIME,
		YAMCA_METCOMP_TIME,
		YAMCA_SELECT_TIME,

		YAMCA_METRICS_COUNT
	} SchedPolMetrics_t;

	/** The High-Resolution timer used for profiling */
	Timer yamca_tmr;

	static MetricsCollector::MetricsCollection_t
		coll_metrics[YAMCA_METRICS_COUNT];

	/**
	 * @brief The plugins constructor
	 *
	 * Plugins objects could be build only by using the "create" method.
	 * Usually the PluginManager acts as object.
	 */
	YamcaSchedPol();

	/**
	 * @brief Get a token for accessing a clean resource view
	 * @return The token returned by ResourceAccounter.
	 */
	ExitCode_t InitResourceView();

	/**
	 * @brief Schedule applications from a priority queue
	 *
	 * @param sv the System interfaces
	 * @param prio The priority queue to schedule
	 *
	 * @return @see ExitCode_t
	 */
	ExitCode_t SchedulePrioQueue(bbque::System & sv,
			AppPrio_t prio);

	/**
	 * @brief Scheduling entities ordering
	 *
	 * For each application create a scheduling entity made by the pair
	 * Application-WorkingMode and place it in a map (ordered by the metrics
	 * value computed)
	 *
	 * @param sched_map Multimap for scheduling entities ordering
	 * @param sv the System interfaces
	 * @param prio The priority queue to schedule
	 * @param cl_id The current cluster for the clustered resources
	 */
	ExitCode_t OrderSchedEntity(SchedEntityMap_t & sched_map,
			bbque::System & sv,
			AppPrio_t prio,
			int cl_id);

	/**
	 * @brief Metrics of all the AWMs of an Application
	 *
	 * @param sched_map Multimap for scheduling entities ordering
	 * @param papp Application to evaluate
	 * @param cl_id The current cluster for the clustered resources
	 */
	ExitCode_t InsertWorkingModes(SchedEntityMap_t & sched_map,
			AppCPtr_t const & papp, int cl_id);

	ExitCode_t EvalWorkingMode(SchedEntityMap_t * sched_map,
			AppCPtr_t const & papp, AwmPtr_t const & wm,
			int cl_id);
	/**
	 * @brief Schedule the entities
	 *
	 * For each application pick the next working mode to schedule
	 *
	 * @param sched_map Multimap for scheduling entities ordering
	 */
	void SelectWorkingModes(SchedEntityMap_t & sched_map);

	/**
	 * @brief Check if an application/EXC must be skipped
	 *
	 * Whenever we are in the ordering or the selecting step, the
	 * application/EXC must be skipped if some conditions are verified.
	 *
	 * @param papp Application/EXC pointer
	 * @return true if the Application/EXC must be skipped, false otherwise
	 */
	bool CheckSkipConditions(AppCPtr_t const & papp);

	/**
	 * @brief Metrics computation
	 *
	 * For each scheduling entity computes a metrics used to define an order.
	 * Such order is used to pick the working mode to set to the application.
	 *
	 * @param papp Application to evaluate
	 * @param wm Working mode to evaluate
	 * @param cl_id The current cluster for the clustered resources
	 * @param metrics Metrics value to return
	 * @return @see ExitCode_t
	 */
	ExitCode_t MetricsComputation(AppCPtr_t const & papp,
			AwmPtr_t const & wm, int cl_id, float & metrics);

	/**
	 * @brief Get resources contention level
	 *
	 * Return a parameter for the evaluation of the scheduling metrics. In
	 * particuar it catches the impact of resource request on the total
	 * availability.
	 *
	 * @param papp Shared pointer to the application to schedule
	 * @param wm The working mode containing the resource requests
	 * @param cl_id The current cluster for the clustered resources
	 * @param cont_level The contention level value to return
	 * @return @see ExitCode_t
	 */
	ExitCode_t GetContentionLevel(AppCPtr_t const & papp, AwmPtr_t const & wm,
			int cl_id, float & cont_level);

	/**
	 * @brief Compute the resource contention level
	 *
	 * @param papp Shared pointer to the application to schedule
	 * @param rsrc_usages Map of resource usages to bind
	 * @param cont_level The contention level value to return
	 * @return @see ExitCode_t
	 */
	ExitCode_t ComputeContentionLevel(AppCPtr_t const & papp,
			UsagesMapPtr_t const & rsrc_usages, float & cont_level);

};

} // namespace plugins

} // namespace bbque

#endif // BBQUE_YAMCA_SCHEDPOL_H_
