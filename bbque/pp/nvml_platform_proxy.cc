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

#include <cstring>

#include "bbque/pp/nvml_platform_proxy.h"

#include "bbque/config.h"
#include "bbque/power_monitor.h"
#include "bbque/resource_accounter.h"
#include "bbque/res/binder.h"
#include "bbque/res/resource_path.h"
#include "bbque/app/working_mode.h"

#define MODULE_NAMESPACE "bq.pp.nvidia"

namespace br = bbque::res;
namespace po = boost::program_options;

namespace bbque {

NVMLPlatformProxy * NVMLPlatformProxy::GetInstance() {
	static NVMLPlatformProxy * instance;
	if (instance == nullptr)
		instance = new NVMLPlatformProxy();
	return instance;
}

NVMLPlatformProxy::NVMLPlatformProxy():
		cm(ConfigurationManager::GetInstance()),
#ifndef CONFIG_BBQUE_PM_NVIDIA
		cmm(CommandManager::GetInstance())
#else
		cmm(CommandManager::GetInstance()),
		pm(PowerManager::GetInstance())
#endif
{
	//---------- Get a logger module
	logger = bu::Logger::GetLogger(MODULE_NAMESPACE);
	assert(logger);
}

NVMLPlatformProxy::~NVMLPlatformProxy() {

}

PlatformProxy::ExitCode_t NVMLPlatformProxy::Setup(SchedPtr_t papp) {
    (void) papp;
    logger->Warn("NVML: No setup action implemented");
    return PlatformProxy::PLATFORM_OK;
}

PlatformProxy::ExitCode_t NVMLPlatformProxy::LoadPlatformData() {
	nvmlReturn_t result;

	//get nvml initialization
	result = nvmlInit();
	if (NVML_SUCCESS != result) {
		logger->Error("NVML: initialization error %s", nvmlErrorString(result));
		return PlatformProxy::PLATFORM_ENUMERATION_FAILED;
	}
	logger->Info("NVML: NVML initialized correctly");
	//Get devices
	result = nvmlDeviceGetCount(&device_count);
	if (NVML_SUCCESS != result) {
		logger->Error("NVML: Device error %s", nvmlErrorString(result));
		return PlatformProxy::PLATFORM_ENUMERATION_FAILED;
	}
	logger->Info("NVML: Number of device(s) found: %d", device_count);
	devices = new nvmlDevice_t[device_count];

	for (uint i = 0; i < device_count; ++i) {
		result = nvmlDeviceGetHandleByIndex(i, &devices[i]);
		if (NVML_SUCCESS != result) {
			logger->Debug("Skipping '%d' [Err:&d] ", i, nvmlErrorString(result));
			continue;
		}
	}

	// Register into Resource Accounter and Power Manager
	RegisterDevices();

	// Power management support
#ifdef CONFIG_BBQUE_PM_NVIDIA
	PrintGPUPowerInfo();
#endif
	return PlatformProxy::PLATFORM_OK;
}

PlatformProxy::ExitCode_t NVMLPlatformProxy::Refresh() {

    return PlatformProxy::PLATFORM_OK;
}

PlatformProxy::ExitCode_t NVMLPlatformProxy::Release(SchedPtr_t papp) {
    (void) papp;
    logger->Warn("NVML: No release action implemented");
    return PlatformProxy::PLATFORM_OK;
}

PlatformProxy::ExitCode_t NVMLPlatformProxy::ReclaimResources(SchedPtr_t papp) {
    (void) papp;
    logger->Warn("NVML: No reclaiming action implemented");
    return PlatformProxy::PLATFORM_OK;
}

PlatformProxy::ExitCode_t NVMLPlatformProxy::MapResources(
    ba::SchedPtr_t papp,
        br::ResourceAssignmentMapPtr_t assign_map,
        bool excl) {
    (void) papp;
    (void) assign_map;
    (void) excl;
    logger->Warn("NVML: No mapping action implemented");
    return PlatformProxy::PLATFORM_OK;
}


bool NVMLPlatformProxy::IsHighPerformance(
		bbque::res::ResourcePathPtr_t const & path) const {
	UNUSED(path)
	return true;
}


#ifdef CONFIG_BBQUE_PM_NVIDIA

void NVMLPlatformProxy::PrintGPUPowerInfo() {
	uint32_t min, max, step, s_min, s_max, ps_count;
	int  s_step;
	PowerManager::PMResult pm_result;

	ResourcePathListPtr_t pgpu_paths(
		GetDevicePaths(br::ResourceType::GPU));
	if (pgpu_paths == nullptr) return;

	for (auto gpu_rp: *(pgpu_paths.get())) {
		/*
		pm_result = pm.GetFanSpeedInfo(gpu_rp, min, max, step);
		if (pm_result == NVIDIAPowerManager::PMResult::OK)
			logger->Info("NVML: [%s] Fanspeed range: [%4d, %4d, s:%2d] RPM ",
				gpu_rp->ToString().c_str(), min, max, step);

		pm_result = pm.GetVoltageInfo(gpu_rp, min, max, step);
		if (pm_result == NVIDIAPowerManager::PMResult::OK)
			logger->Info("NVML: [%s] Voltage range:  [%4d, %4d, s:%2d] mV ",
				gpu_rp->ToString().c_str(), min, max, step);
		*/
		pm_result = pm.GetClockFrequencyInfo(gpu_rp, min, max, step);
		if (pm_result == NVIDIAPowerManager::PMResult::OK)
			logger->Info("NVML: [%s] ClkFreq range:  [%4d, %4d, s:%2d] MHz ",
				gpu_rp->ToString().c_str(),
				min/1000, max/1000, step/1000);

		std::vector<uint32_t> freqs;
		std::string freqs_str(" ");
		pm_result = pm.GetAvailableFrequencies(gpu_rp, freqs);
		if (pm_result == NVIDIAPowerManager::PMResult::OK) {
			for (auto & f: freqs)
				freqs_str += (std::to_string(f) + " ");
			logger->Info("NVML: [%s] ClkFrequencies:  [%s] MHz ",
					gpu_rp->ToString().c_str(), freqs_str.c_str());
		}

		pm_result = pm.GetPowerStatesInfo(gpu_rp, s_min,s_max, s_step);
		if (pm_result == NVIDIAPowerManager::PMResult::OK)
			logger->Info("NVML: [%s] Power states:   [%4d, %4d, s:%2d] ",
				gpu_rp->ToString().c_str(), s_min, s_max, s_step);

		pm_result = pm.GetPerformanceStatesCount(gpu_rp, ps_count);
		if (pm_result == NVIDIAPowerManager::PMResult::OK)
			logger->Info("NVML: [%s] Performance states: %2d",
				gpu_rp->ToString().c_str(), ps_count);
/*
		pm.SetFanSpeed(gpu_rp,PowerManager::FanSpeedType::PERCENT, 5);
		pm.ResetFanSpeed(gpu_rp);
*/
	}
	logger->Info("NVML: Monitoring %d GPU adapters", pgpu_paths->size());
}
#endif // CONFIG_BBQUE_PM


VectorUInt8Ptr_t NVMLPlatformProxy::GetDeviceIDs(br::ResourceType r_type) const {

	ResourceTypeIDMap_t::const_iterator d_it(GetDeviceConstIterator(r_type));
	if (d_it == device_ids.end()) {
		logger->Error("GetDeviceIDs: No NVML devices of type '%s'",
			br::GetResourceTypeString(r_type));
		return nullptr;
	}
	return d_it->second;
}


uint8_t NVMLPlatformProxy::GetDevicesNum(br::ResourceType r_type) const {

	ResourceTypeIDMap_t::const_iterator d_it(GetDeviceConstIterator(r_type));
	if (d_it == device_ids.end()) {
		logger->Error("GetDevicesNum: No NVML devices of type '%s'",
			br::GetResourceTypeString(r_type));
		return 0;
	}
	return d_it->second->size();
}

ResourcePathListPtr_t NVMLPlatformProxy::GetDevicePaths(
		br::ResourceType r_type) const {

	ResourceTypePathMap_t::const_iterator p_it(device_paths.find(r_type));
	if (p_it == device_paths.end()) {
		logger->Error("GetDevicePaths: No NVML devices of type  '%s'",
			br::GetResourceTypeString(r_type));
		return nullptr;
	}
	return p_it->second;
}


ResourceTypeIDMap_t::iterator
NVMLPlatformProxy::GetDeviceIterator(br::ResourceType r_type) {
	if (device_count == 0) {
		logger->Error("GetDeviceIterator: Missing NVML devices");
		return device_ids.end();
	}
	return	device_ids.find(r_type);
}

ResourceTypeIDMap_t::const_iterator
NVMLPlatformProxy::GetDeviceConstIterator(br::ResourceType r_type) const {
	if (device_count == 0) {
		logger->Error("GetDeviceConstIterator: Missing NVML devices");
		return device_ids.end();
	}
	return	device_ids.find(r_type);
}


PlatformProxy::ExitCode_t NVMLPlatformProxy::RegisterDevices() {
	nvmlReturn_t result;
	char dev_name[NVML_DEVICE_NAME_BUFFER_SIZE];
	char gpu_pe_path[]  = "sys0.gpu256.pe256";
	br::ResourceType r_type = br::ResourceType::UNDEFINED;
	ResourceAccounter &ra(ResourceAccounter::GetInstance());
#ifdef CONFIG_BBQUE_WM
	PowerMonitor & wm(PowerMonitor::GetInstance());
	//wm.Register(ra.GetPath(gpu_pe_path));
#endif
	for (uint16_t dev_id = 0; dev_id < device_count; ++dev_id) {
        logger->Debug("Looping over devices");

		result = nvmlDeviceGetName (devices[dev_id], dev_name, NVML_DEVICE_NAME_BUFFER_SIZE);
		if (NVML_SUCCESS != result) {
			logger->Warn("Failed to get name of device %i: %s", dev_id, nvmlErrorString(result));
		}

		snprintf(gpu_pe_path+5, 12, "gpu%hu.pe0", dev_id);
		ra.RegisterResource(gpu_pe_path, "", 100);
		r_type = br::ResourceType::GPU;
#ifdef CONFIG_BBQUE_WM
		wm.Register(ra.GetPath(gpu_pe_path));
		logger->Debug("InitPowerInfo: [%s] registered for monitoring in nvml", gpu_pe_path);
		//logger->Debug("gpu path %i: %s", dev_id, nvmlErrorString(result));
#endif

		// Keep track of NVIDIA device IDs and resource paths
		InsertDeviceID(r_type, dev_id);
		if (r_type == br::ResourceType::GPU)
			InsertDevicePath(r_type, gpu_pe_path);

		logger->Info("NVML: D[%d]: {%s}, type: [%s], path: [%s]",
			dev_id, dev_name,
			br::GetResourceTypeString(r_type),
			gpu_pe_path);
	}

	return PlatformProxy::PLATFORM_OK;
}


void NVMLPlatformProxy::InsertDeviceID(
		br::ResourceType r_type,
		uint8_t dev_id) {
	ResourceTypeIDMap_t::iterator d_it;
	VectorUInt8Ptr_t pdev_ids;

	logger->Debug("NVIDIA: Insert device %d of type %s",
			dev_id, br::GetResourceTypeString(r_type));
	d_it = GetDeviceIterator(r_type);
	if (d_it == device_ids.end()) {
		device_ids.insert(
			std::pair<br::ResourceType, VectorUInt8Ptr_t>
				(r_type, VectorUInt8Ptr_t(new VectorUInt8_t))
		);
	}

	pdev_ids = device_ids[r_type];
	pdev_ids->push_back(dev_id);
	if (r_type != br::ResourceType::GPU)
		return;

	// Resource path to GPU memory
	char gpu_mem_path[] = "sys0.gpu256.mem256";
	snprintf(gpu_mem_path+5, 12, "gpu%hu.mem0", dev_id);
	gpu_mem_paths.insert(std::pair<int, ResourcePathPtr_t>(
		dev_id, ResourcePathPtr_t(new br::ResourcePath(gpu_mem_path))));
	logger->Debug("NVML: GPU memory registered: %s", gpu_mem_path);
}

void NVMLPlatformProxy::InsertDevicePath(
		br::ResourceType r_type,
		std::string const & dev_path_str) {
	ResourceTypePathMap_t::iterator p_it;
	ResourcePathListPtr_t pdev_paths;

	logger->Debug("NVML: Insert device resource path  %s",
			dev_path_str.c_str());
	p_it = device_paths.find(r_type);
	if (p_it == device_paths.end()) {
		device_paths.insert(
			std::pair<br::ResourceType, ResourcePathListPtr_t>
				(r_type, ResourcePathListPtr_t(new ResourcePathList_t))
		);
	}

	ResourcePathPtr_t rp(new br::ResourcePath(dev_path_str));
	if (rp == nullptr) {
		logger->Error("NVML: Invalid resource path %s",
				dev_path_str.c_str());
		return;
	}

	pdev_paths = device_paths[r_type];
	pdev_paths->push_back(rp);
}

void NVMLPlatformProxy::Exit() {
	logger->Debug("Exiting the Nvml Proxy...");

	nvmlReturn_t result = nvmlShutdown();
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: Failed to shutdown NVML: [Err:%s]",
		             nvmlErrorString(result));
	}
	logger->Notice("NVML shutdownend correctly");

	delete devices;
	device_ids.clear();
	device_paths.clear();
}

} // namespace bbque
