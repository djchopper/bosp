/*
 * Copyright (C) 2019  Politecnico di Milano
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

#include "mangav2_schedpol.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>

#include "bbque/modules_factory.h"
#include "bbque/utils/logging/logger.h"

#include "bbque/app/working_mode.h"
#include "bbque/res/binder.h"
#include "tg/task_graph.h"


#define MODULE_CONFIG SCHEDULER_POLICY_CONFIG "." SCHEDULER_POLICY_NAME

namespace bu = bbque::utils;
namespace po = boost::program_options;

namespace bbque
{
namespace plugins
{

// :::::::::::::::::::::: Static plugin interface ::::::::::::::::::::::::::::

void * ManGAv2SchedPol::Create(PF_ObjectParams *)
{
	return new ManGAv2SchedPol();
}

int32_t ManGAv2SchedPol::Destroy(void * plugin)
{
	if (!plugin)
		return -1;
	delete (ManGAv2SchedPol *)plugin;
	return 0;
}

// ::::::::::::::::::::: Scheduler policy module interface :::::::::::::::::::

char const * ManGAv2SchedPol::Name()
{
	return SCHEDULER_POLICY_NAME;
}

ManGAv2SchedPol::ManGAv2SchedPol():
	cm(ConfigurationManager::GetInstance()),
	ra(ResourceAccounter::GetInstance())
{
	logger = bu::Logger::GetLogger(MODULE_NAMESPACE);
	assert(logger);
	if (logger)
		logger->Info("mangav2: Built a new dynamic object[%p]", this);
	else
		fprintf(stderr,
		        FI("mangav2: Built new dynamic object [%p]\n"), (void *)this);
}


ManGAv2SchedPol::~ManGAv2SchedPol()
{
	pe_per_acc.clear();
}


SchedulerPolicyIF::ExitCode_t ManGAv2SchedPol::Init()
{
	// Build a string path for the resource state view
	std::string token_path(MODULE_NAMESPACE);
	++status_view_count;
	token_path.append(std::to_string(status_view_count));
	logger->Debug("Init: getting new resource state view token from <%s>",
	              token_path.c_str());

	// Get a fresh resource status view
	ResourceAccounterStatusIF::ExitCode_t ra_result =
	    ra.GetView(token_path, sched_status_view);
	if (ra_result != ResourceAccounterStatusIF::RA_SUCCESS) {
		logger->Fatal("Init: cannot get a resource state view");
		return SCHED_ERROR_VIEW;
	}
	logger->Debug("Init: resources state view token: %ld", sched_status_view);

	// Load the application task graphs
	logger->Debug("Init: loading the applications task graphs");
	fut_tg = std::async(std::launch::async, &System::LoadTaskGraphs, sys);

	// Get the amount of processing elements from each accelerator
	if (pe_per_acc.empty()) {
		// Get the number of HN clusters (groups)
		auto clusters = sys->GetResources("sys.grp");
		logger->Debug("Init: # clusters: %d", clusters.size());
		for (auto & hn_cluster: clusters) {

			// Cluster info
			auto cluster_id = hn_cluster->ID();
			std::string cluster_path(
			    std::string("sys.grp") + std::to_string(cluster_id));

			// Nr. of accelerators
			std::string acc_path(cluster_path + std::string(".acc"));
			logger->Debug("Init: accelerators path: <%s>", acc_path.c_str());
			auto accs = sys->GetResources(acc_path);
			pe_per_acc.emplace(cluster_id, std::vector<uint32_t>(accs.size()));
			logger->Debug("Init: cluster=<%d>: # accelerators %d",
			              cluster_id, accs.size());

			// Nr. of cores per accelerator
			for (br::ResourcePtr_t const & acc: accs) {
				std::string pe_binding_path(
				    acc_path + std::to_string(acc->ID()) + std::string(".pe"));
				logger->Debug("Init: binding resource path: <%s>",
				              pe_binding_path.c_str());

				pe_per_acc[cluster_id][acc->ID()] =
				    sys->ResourceTotal(pe_binding_path) / 100 ;
				logger->Debug("Init: <%s> #proc_element=%d",
				              pe_binding_path.c_str(),
				              pe_per_acc[cluster_id][acc->ID()]);
			}
		}
	}

	return SCHED_OK;
}


SchedulerPolicyIF::ExitCode_t ManGAv2SchedPol::Schedule(
    System & system,
    RViewToken_t & status_view)
{
	// Class providing query functions for applications and resources
	sys = &system;
	Init();
	fut_tg.get();

	// Re-assing resources to running applications: no reconfiguration
	// supported by MANGO platform
	ExitCode_t result;
	bbque::app::AppCPtr_t papp;
	AppsUidMapIt app_it;

	papp = sys->GetFirstRunning(app_it);
	for (; papp; papp = sys->GetNextReady(app_it)) {
		logger->Debug("Schedule: [%s] is RUNNING -> rescheduling", papp->StrId());
		result = ReassignWorkingMode(papp);
		if (result == SCHED_ERROR) {
			logger->Crit("Schedule: [%s] unexpected error [err=%d]",
				     papp->StrId(), result);
			return SCHED_ERROR;
		}
	}

	for (AppPrio_t priority = 0; priority <= sys->ApplicationLowestPriority(); priority++) {
		if (!sys->HasApplications(priority))
			continue;

		logger->Debug("Schedule: serving applications with priority %d", priority);
		result = SchedulePriority(priority);
		if (result == SCHED_ERROR) {
			logger->Error("Schedule: error occurred while scheduling priority %d",
			              priority);
			return result;
		}
	}

	// Return the new resource status view according to the new resource
	// allocation performed
	status_view = sched_status_view;
	return SCHED_DONE;
}


SchedulerPolicyIF::ExitCode_t ManGAv2SchedPol::SchedulePriority(
    AppPrio_t priority)
{
	// Get all the applications @ this priority
	AppsUidMapIt app_iterator;
	ba::AppCPtr_t papp = sys->GetFirstWithPrio(priority, app_iterator);
	for (; papp; papp = sys->GetNextWithPrio(priority, app_iterator)) {
		logger->Debug("SchedulePriority: [%s] looking for resources...",
		              papp->StrId());

		// Skip disabled
		if (papp->Disabled()) {
			logger->Debug("SchedulePriority: [%s] DISABLED: skip...",
			              papp->StrId());
			continue;
		}

		// Skip already scheduled running applications
		if (papp->Running()) {
			logger->Debug("SchedulePriority: [%s] RUNNING: skip...",
			              papp->StrId());
			continue;
		}

		// Evaluate the mapping options
		ExitCode_t ret = EvalMappingAlternatives(papp);
		if (ret != SCHED_OK) {
			logger->Warn("SchedulePriority: [%s] not schedulable",
			             papp->StrId());
			// Do not schedule
			ApplicationManager & am(ApplicationManager::GetInstance());
			am.NoSchedule(papp);
			ret = SCHED_OK;
		}
	}

	return ExitCode_t::SCHED_OK;
}


SchedulerPolicyIF::ExitCode_t ManGAv2SchedPol::ReassignWorkingMode(
    bbque::app::AppCPtr_t papp)
{
	ApplicationManager & am(ApplicationManager::GetInstance());
	auto ret = am.ScheduleRequestAsPrev(papp, sched_status_view);
	if (ret != ApplicationManager::AM_SUCCESS) {
		logger->Error("ReassignWorkingMode: [%s] rescheduling failed", papp->StrId());
		return SCHED_SKIP_APP;
	}

	logger->Info("ReassignWorkingMode: [%s] rescheduling done", papp->StrId());
	return SCHED_OK;
}


SchedulerPolicyIF::ExitCode_t ManGAv2SchedPol::EvalMappingAlternatives(
    bbque::app::AppCPtr_t papp)
{
	auto recipe = papp->GetRecipe();
	auto task_graph = papp->GetTaskGraph();
	ExitCode_t ret = ExitCode_t::SCHED_OK;

	// Optionally add an ordering criteria here...
	logger->Debug("EvalMappingAlternatives: [%s] nr. mapping options = %d",
		papp->StrId(), recipe->GetTaskGraphMappingAll().size());

	for (auto & m: recipe->GetTaskGraphMappingAll()) {
		logger->Info("EvalMappingAlternatives: [%s] id=%d "
		             "exec_time=%d[ms] power=%d[mW] memory_bw=%d[Kbps] ",
		             papp->StrId(),
		             m.first,
		             m.second.exec_time_ms,
		             m.second.power_mw,
		             m.second.mem_bw);

		std::shared_ptr<Partition> partition = std::make_shared<Partition>(0);

		if (task_graph->Tasks().size() > m.second.tasks.size()) {
			logger->Error("EvalMappingAlternatives: [%s] "
			              "possible mismatch in recipe file!",
			              papp->StrId());
			return SCHED_ERROR;
		}

		// Task mapping
		for (auto const & task_mapping: m.second.tasks) {
			auto & task_id = task_mapping.first;
			auto & mapping_info = task_mapping.second;

			auto task = task_graph->GetTask(task_id);
			if (task == nullptr) {
				logger->Warn("EvalMappingAlternatives: [%s] task id=%d not found. "
				             "Possible mismatch in recipe file!",
				             papp->StrId(), task_id);
				continue;
			}


			if (mapping_info.type == bbque::res::ResourceType::ACCELERATOR) {
				logger->Debug("EvalMappingAlternatives: [%s] "
				              "task=%d => [acc=%d]",
				              papp->StrId(), task->Id(), mapping_info.id);
				task->SetMappedProcessor(mapping_info.id);
				partition->MapTask(task, mapping_info.id, 0, 0);
				// TODO: frequency setting ...
			}
		}

		// Buffer mapping
		for (auto const & buff_mapping: m.second.buffers) {
			auto & buff_id = buff_mapping.first;
			auto & mapping_info = buff_mapping.second;

			auto buffer = task_graph->GetBuffer(buff_id);
			if (buffer == nullptr) {
				logger->Warn("EvalMappingAlternatives: [%s] buffer id=%d not found. "
				             "Possible mismatch in recipe file!",
				             papp->StrId(), buff_id);
				continue;
			}

			if (mapping_info.type == bbque::res::ResourceType::MEMORY) {
				logger->Debug("EvalMappingAlternatives: [%s] "
				              "buffer=%d => [mem=%d]",
				              papp->StrId(), buffer->Id(), mapping_info.id);
				buffer->SetMemoryBank(mapping_info.id);
				partition->MapBuffer(buffer, mapping_info.id, 0);
			}
		}

		// Check if the mapping is feasible in at least one platform cluster
		ret = CheckMappingFeasibility(papp, partition);
		if (ret == SCHED_OK) {
			logger->Info("EvalMappingAlternatives: [%s] id=%d is feasible",
			             papp->StrId(),
			             m.first);
			//		papp->SetPartition(partition);
			return ret;
		} else if (ret == SCHED_ERROR) {
			logger->Error("EvalMappingAlternatives: [%s] id=%d interrupted",
			              papp->StrId(),
			              m.first);
			return ret;
		}
		// if here... try with the next mapping option
	}

	return SCHED_R_UNAVAILABLE;
}


SchedulerPolicyIF::ExitCode_t ManGAv2SchedPol::CheckMappingFeasibility(
    bbque::app::AppCPtr_t papp,
    std::shared_ptr<Partition> partition)
{
	// Iterate over all the HN clusters
	uint32_t curr_cluster_id = 0;
	uint32_t nr_clusters = pe_per_acc.size();

	for (; curr_cluster_id < nr_clusters; curr_cluster_id++) {
		logger->Debug("CheckMappingFeasibility: [%s] checking availability "
		              "in cluster=%d",
		              papp->StrId(), curr_cluster_id);

		partition->SetClusterId(curr_cluster_id);
		papp->GetTaskGraph()->SetCluster(curr_cluster_id);

		// Find memory space for kernels binaries and buffers...



		// Set the working mode
		int32_t ref_num;
		auto pawm = SelectWorkingMode(papp, partition, ref_num);
		if (pawm == nullptr) {
			logger->Crit("CheckMappingFeasibility [%s] null working mode",
			             papp->StrId());
			return ExitCode_t::SCHED_ERROR;
		}

		// Schedule request
		ExitCode_t ret = ScheduleApplication(papp, pawm, ref_num);
		if (ret != SCHED_R_UNAVAILABLE) {
			logger->Debug("CheckMappingFeasibility [%s] terminated",
			              papp->StrId());
			return ret;
		}
	}

	logger->Error("CheckMappingFeasibility [%s] no clusters on the platform",
	              papp->StrId());
	return SCHED_ERROR;
}


ba::AwmPtr_t ManGAv2SchedPol::SelectWorkingMode(
    ba::AppCPtr_t papp,
    std::shared_ptr<Partition> partition,
    int & ref_num)
{
	// Build a new working mode featuring assigned resources
	ba::AwmPtr_t pawm = papp->CurrentAWM();
	if (pawm == nullptr) {
		pawm = std::make_shared<ba::WorkingMode>(
		           papp->WorkingModes().size(),"Run-time", 1, papp);
	}
	pawm->ClearResourceBinding();
	logger->Info("SelectWorkingMode: [%s] partition %d mapping...",
	             papp->StrId(), partition->GetId());
	ref_num = -1;

	// Now we will update the Resource Accounter in order to trace the resource
	// allocation. This has no effect in the platform resource assignment, since
	// the actual assignment was performed (by the platform proxy) during
	// the PropagatePartition

	uint32_t nr_cores = 1;
	auto tg = papp->GetTaskGraph();
	uint32_t cluster_id = tg->GetCluster(); //partition->GetClusterId();

	for (auto task : tg->Tasks()) {
		// if thread count not specified, assign all the available cores
		logger->Debug("SelectWorkingMode: [%s] task=%d thread_count=%d",
		              papp->StrId(), task.first, task.second->GetThreadCount());

		int tile_id = task.second->GetMappedProcessor();
		//partition->GetUnit(task.second);
		if (tile_id < 0) {
			logger->Error("SelectWorkingMode: [%s] task=%d mapping missing",
			              papp->StrId(), task.first);
			return nullptr;
		}

		if (task.second->GetThreadCount() <= 0) {
			nr_cores = pe_per_acc[cluster_id][tile_id];
			logger->Debug("SelectWorkingMode: [%s] task=%d nr_cores=%d",
			              papp->StrId(), task.first, nr_cores);
		}

		// Resource request of processing resources
		logger->Debug("SelectWorkingMode: [%s] task=%d add accelerator request...",
		              papp->StrId(), task.first);
		pawm->AddResourceRequest("sys.grp.acc.pe", 100 * nr_cores,
		                         br::ResourceAssignment::Policy::BALANCED);

		// Resource binding: HN cluster (group)
		logger->Debug("SelectWorkingMode: [%s] task=%d bind accelerator request to "
		              "cluster=%d",
		              papp->StrId(), task.first, cluster_id);
		ref_num = pawm->BindResource(
		              br::ResourceType::GROUP, R_ID_ANY,
		              cluster_id,
		              ref_num);

		// Resource binding: HN tile (accelerator)
		logger->Debug("SelectWorkingMode: [%s] task=%d bind accelerator request to "
		              "tile=%d",
		              papp->StrId(), task.first, tile_id);
		ref_num = pawm->BindResource(
		              br::ResourceType::ACCELERATOR, R_ID_ANY,
		              tile_id,
		              ref_num);

		logger->Debug("SelectWorkingMode: [%s] task=%d -> cluster=<%d> tile=<%d>",
		              papp->StrId(), task.first,
		              cluster_id,
		              tile_id);
	}

	// Amount of memory to allocate per bank
	std::map<uint32_t, uint64_t> mem_req_amount;

	// Accouting of memory resources
	for (auto buff : tg->Buffers()) {
		//int mem_bank_id = partition->GetMemoryBank(buff.second);
		int mem_bank_id = buff.second->MemoryBank();
		if (mem_bank_id < 0) {
			logger->Error("SelectWorkingMode: [%s] buffer=%d mapping missing",
			              papp->StrId(), buff.first);
			return nullptr;
		}

		mem_req_amount[mem_bank_id] += buff.second->Size();
		logger->Debug("SelectWorkingMode: [%s] buffer=%d -> memory bank %d (id=%d)",
		              papp->StrId(),
		              buff.first,
		              partition->GetMemoryBank(buff.second),
		              mem_bank_id);
	}

	for (auto &mem_entry: mem_req_amount) {
		auto & mem_bank_id = mem_entry.first;
		auto & mem_amount  = mem_entry.second;

		// Resource request for memory (set the bank id yet)
		std::string mem_path("sys.grp.mem");
		mem_path += std::to_string(mem_bank_id);
		pawm->AddResourceRequest(mem_path, mem_amount);
		logger->Debug("SelectWorkingMode: [%s] memory bank %d (requested=%d)",
		              papp->StrId(), mem_bank_id, mem_amount);

		// Resource binding: HN cluster (group) needed only
		ref_num = pawm->BindResource(
		              br::ResourceType::GROUP, R_ID_ANY,
		              partition->GetClusterId(),
		              ref_num);
	}

	return pawm;
}


SchedulerPolicyIF::ExitCode_t ManGAv2SchedPol::ScheduleApplication(
    ba::AppCPtr_t papp,
    ba::AwmPtr_t pawm,
    int32_t ref_num)
{
	// Update the accounting of resources with a schedule request
	ApplicationManager & am(ApplicationManager::GetInstance());
	auto ret = am.ScheduleRequest(papp, pawm, sched_status_view, ref_num);
	if (ret == ApplicationManager::AM_SUCCESS) {
		logger->Info("ScheduleApplication: [%s] schedule request accepted",
		             papp->StrId());
		return SCHED_OK;
	} else if ((ret == ApplicationManager::AM_APP_BLOCKING) ||
	           (ret == ApplicationManager::AM_APP_DISABLED)) {
		logger->Debug("ScheduleApplication: [%s] schedule request for blocking/disabled",
		              papp->StrId());
		return SCHED_SKIP_APP;
	} else if (ret == ApplicationManager::AM_AWM_NOT_SCHEDULABLE) {
		logger->Debug("ScheduleApplication: [%s] schedule request not allocable",
		              papp->StrId());
		return SCHED_R_UNAVAILABLE;
	} else {
		logger->Error("ScheduleApplication: [%s] schedule request unexpected error",
		              papp->StrId());
		return SCHED_ERROR;
	}
}



} // namespace plugins

} // namespace bbque
