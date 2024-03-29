/*
 * Copyright (C) 2016  Politecnico di Milano
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

#include "gridbalance_schedpol.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <iostream>

#include "bbque/modules_factory.h"
#include "bbque/utils/logging/logger.h"

#include "bbque/app/working_mode.h"
#include "bbque/res/binder.h"
#include "bbque/res/resource_utils.h"

#ifdef CONFIG_BBQUE_PM_CPU
#include "bbque/pm/power_manager.h"
#endif

#define MODULE_CONFIG SCHEDULER_POLICY_CONFIG "." SCHEDULER_POLICY_NAME

using namespace std::placeholders;

namespace bu = bbque::utils;
namespace po = boost::program_options;

namespace bbque { namespace plugins {

// :::::::::::::::::::::: Static plugin interface ::::::::::::::::::::::::::::

void * GridBalanceSchedPol::Create(PF_ObjectParams *) {
	return new GridBalanceSchedPol();
}

int32_t GridBalanceSchedPol::Destroy(void * plugin) {
	if (!plugin)
		return -1;
	delete (GridBalanceSchedPol *)plugin;
	return 0;
}

// ::::::::::::::::::::: Scheduler policy module interface :::::::::::::::::::

char const * GridBalanceSchedPol::Name() {
	return SCHEDULER_POLICY_NAME;
}

GridBalanceSchedPol::GridBalanceSchedPol():
		cm(ConfigurationManager::GetInstance()),
		ra(ResourceAccounter::GetInstance()) {
	logger = bu::Logger::GetLogger("bq.sp.gridbal");
	assert(logger);

	if (logger)
		logger->Info("gridbalance: Built a new dynamic object[%p]", this);
	else
		fprintf(stderr,
			FI("gridbalance: Built new dynamic object [%p]\n"), (void *)this);
}


GridBalanceSchedPol::~GridBalanceSchedPol() {
	entities.clear();
}


SchedulerPolicyIF::ExitCode_t GridBalanceSchedPol::Init() {
	// Build a string path for the resource state view
	std::string token_path(MODULE_NAMESPACE);
	++status_view_count;
	token_path.append(std::to_string(status_view_count));
	logger->Debug("Init: requiring a new resource state view [%s]",
		token_path.c_str());

	// A new resource status view
	auto ra_result = ra.GetView(token_path, sched_status_view);
	if (ra_result != ResourceAccounterStatusIF::RA_SUCCESS) {
		logger->Fatal("Init: cannot get a new resource state view");
		return SCHED_ERROR_VIEW;
	}
	logger->Debug("Init: resources state view token = %ld", sched_status_view);

	// Keep track of all the available CPU processing elements (cores)
	if (proc_elements.empty())
		proc_elements = sys->GetResources("sys.cpu.pe");

#ifdef CONFIG_BBQUE_PM_CPU
	// Sort processing elements (by temperature)
	SortProcessingElements();
#endif

	// Compute the number of slots for priority-proportional assignments
	if (sys->HasApplications(ApplicationStatusIF::READY))
		slots = GetSlots();
	logger->Debug("Init: number of assignable slots = %d", slots);

	return SCHED_OK;
}

#ifdef CONFIG_BBQUE_PM_CPU

void GridBalanceSchedPol::SortProcessingElements() {

	proc_elements.sort(bbque::res::CompareTemperature);
	for (auto & pe: proc_elements)
		logger->Debug("<%s> : %.0f C",
			pe->Path().c_str(),
			pe->GetPowerInfo(PowerManager::InfoType::TEMPERATURE));

	uint32_t temp_mean;
	PowerManager & pm(PowerManager::GetInstance());
	pm.GetTemperature(ra.GetPath("sys.cpu.pe"), temp_mean);
	logger->Debug("Init: <sys.cpu.pe> mean temperature = %d", temp_mean);
}

#endif // CONFIG_BBQUE_PM_CPU


SchedulerPolicyIF::ExitCode_t
GridBalanceSchedPol::AssignWorkingMode(bbque::app::AppCPtr_t papp) {
	logger->Debug("Assign: [%s] assigning resources...", papp->StrId());

	if (papp->Blocking()) {
		logger->Info("Assign: [%s] is being blocked", papp->StrId());
		return SCHED_OK;
	}

	if (papp->Running())
		papp->CurrentAWM()->ClearResourceRequests();

	// New AWM
	auto pawm = papp->CurrentAWM();
	if (pawm == nullptr)
		pawm = std::make_shared<ba::WorkingMode>(
				papp->WorkingModes().size(),"Run-time", 1, papp);

	// Amount of processing resources to assign
	std::string resource_path_str("sys.cpu.pe");
	uint64_t resource_slot_size = sys->ResourceTotal(resource_path_str) / slots;
	logger->Debug("Assign: <%s> total = %lu slot_size=%d",
		resource_path_str.c_str(), sys->ResourceTotal(resource_path_str),
		resource_slot_size);
	uint64_t resource_amount =
			(sys->ApplicationLowestPriority() - papp->Priority() + 1)
				* resource_slot_size;
	logger->Info("Assign: [%s] amount of <%s> assigned = %4d",
			papp->StrId(), resource_path_str.c_str(), resource_amount);

	if (resource_amount == 0) {
		logger->Warn("Assign: [%s] will have no resources", papp->StrId());
		return SCHED_OK;
	}

	// Assign...
	pawm->AddResourceRequest(resource_path_str, resource_amount,
		br::ResourceAssignment::Policy::BALANCED);
	logger->Debug("Assign: [%s] added resource request [#%d]",
		papp->StrId(), pawm->NumberOfResourceRequests());

	// Enqueue scheduling entity
	auto sched_entity = std::make_shared<SchedEntity_t>(papp, pawm, R_ID_ANY, 0);
	entities.push_back(sched_entity);

	return SCHED_OK;
}


SchedulerPolicyIF::ExitCode_t GridBalanceSchedPol::BindWorkingModesAndSched() {

	bbque::res::ResourcePtrList_t::const_iterator iter = proc_elements.begin();
	auto proc_path = ra.GetPath("sys.cpu.pe");

	for (auto & sched_entity: entities) {
		logger->Info("Bind: [%s] binding and scheduling...",
			sched_entity->papp->StrId());
		uint64_t req_amount = sched_entity->pawm->RequestedAmount(proc_path);
		size_t num_procs = req_amount > 100 ? ceil(float(req_amount) / 100.0) : 1;
		logger->Debug("Bind: [%s] <sys.cpu.pe>=%d => num_procs=%d",
			sched_entity->papp->StrId(), req_amount, num_procs);

		auto proc_mask =
			bbque::res::ResourceBinder::GetMaskInRange(
				proc_elements, iter, num_procs);
		logger->Debug("Bind: [%s] <sys.cpu.pe> mask = %s",
			sched_entity->papp->StrId(), proc_mask.ToString().c_str());

		if ((req_amount % 100 == 0) || ((*iter)->Available() == 0)) {
			logger->Debug("Bind: increment the iterator");
			iter++;
		}

		sched_entity->bind_refn =
			sched_entity->pawm->BindResource(proc_path, proc_mask);

		ApplicationManager & am(ApplicationManager::GetInstance());
		am.ScheduleRequest(
			sched_entity->papp, sched_entity->pawm,
			sched_status_view, sched_entity->bind_refn);
	}

	return SCHED_OK;
}



SchedulerPolicyIF::ExitCode_t
GridBalanceSchedPol::Schedule(
		System & system,
		RViewToken_t & status_view) {
	SchedulerPolicyIF::ExitCode_t result = SCHED_DONE;

	// Class providing query functions for applications and resources
	sys = &system;
	Init();

	// Resource (AWM) assignment
	auto assign_awm = std::bind(&GridBalanceSchedPol::AssignWorkingMode, this, _1);
	ForEachReadyAndRunningDo(assign_awm);

	// Resource binding and then scheduling
	BindWorkingModesAndSched();
	entities.clear();

	// Return the new resource status view according to the new resource
	// allocation performed
	status_view = sched_status_view;
	return result;
}

} // namespace plugins

} // namespace bbque
