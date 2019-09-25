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

#include <dlfcn.h>

#include "bbque/pm/power_manager_nvidia.h"

#define numFreq 1000

#define  GET_DEVICE_ID(rp, device) \
 	nvmlDevice_t device; \
 	unsigned int id_num; \
	id_num = GetDeviceId(rp, device); \
	if (device == NULL) { \
		logger->Error("NVML: The path does not resolve a resource"); \
		return PMResult::ERR_RSRC_INVALID_PATH; \
	}


namespace br = bbque::res;


namespace bbque {


const char * convertToComputeModeString(nvmlComputeMode_t mode)
{
	switch (mode) {
	case NVML_COMPUTEMODE_DEFAULT:
		return "Default";
	case NVML_COMPUTEMODE_EXCLUSIVE_THREAD:
		return "Exclusive_Thread";
	case NVML_COMPUTEMODE_PROHIBITED:
		return "Prohibited";
	case NVML_COMPUTEMODE_EXCLUSIVE_PROCESS:
		return "Exclusive Process";
	default:
		return "Unknown";
	}
}

NVIDIAPowerManager & NVIDIAPowerManager::GetInstance() {
        static NVIDIAPowerManager instance;
        return instance;
    }


NVIDIAPowerManager::NVIDIAPowerManager()
{
	// Retrieve information about the GPU(s) of the system
	LoadDevicesInfo();
}

void NVIDIAPowerManager::LoadDevicesInfo()
{
	nvmlReturn_t result;
	result = nvmlInit();

	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: Control initialization failed [Err:%s]",
		             nvmlErrorString(result) );
		return;
	}
	logger->Info("NVML: Nvml initialized correctly");

	// Devices enumeration
	result = nvmlDeviceGetCount(&device_count);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: No device(s) available on the system [Err:%s]",
		             nvmlErrorString(result));
		return;
	}
	logger->Info("NVML: Number of device(s) count = %d", device_count);

	for (uint i = 0; i < device_count; ++i) {
		DeviceInfo device_info;
		nvmlDevice_t device;

		device_info.id_num = i;
		// Query for device handle to perform operations on a device
		result = nvmlDeviceGetHandleByIndex(i, &device);
		if (NVML_SUCCESS != result) {
			logger->Debug("Skipping '%d' [Err:%d] ", i, nvmlErrorString(result));
			continue;
		}

		// Devices ID mapping and resouce path
		devices_map.insert(std::pair<BBQUE_RID_TYPE, nvmlDevice_t>(devices_map.size(),
		                   device));

		result = nvmlDeviceGetName(device, device_info.name,
		                           NVML_DEVICE_NAME_BUFFER_SIZE);
		if (NVML_SUCCESS != result) {
			logger->Warn("Failed to get name of device %i: %s", i, nvmlErrorString(result));
		}

		// pci.busId is very useful to know which device physically you're talking to
		// Using PCI identifier you can also match nvmlDevice handle to CUDA device.
		result = nvmlDeviceGetPciInfo(device, &device_info.pci);
		if (NVML_SUCCESS != result) {
			logger->Warn("Failed to get pci info for device %i: %s", i,
			             nvmlErrorString(result));
		}

		logger->Debug("%d. %s [%s] %d", i, device_info.name, device_info.pci.busId,
		              device);

		// Power control capabilities
		result = nvmlDeviceGetComputeMode(device, &device_info.compute_mode);
		if (NVML_ERROR_NOT_SUPPORTED == result)
			logger->Warn("This is not CUDA capable device");
		else if (NVML_SUCCESS != result) {
			logger->Warn("Failed to get compute mode for device %i: %s", i,
			             nvmlErrorString(result));
			continue;
		} else {
			// try to change compute mode
			logger->Debug("Changing device's compute mode from '%s' to '%s'",
			              convertToComputeModeString(device_info.compute_mode),
			              convertToComputeModeString(NVML_COMPUTEMODE_PROHIBITED));

			result = nvmlDeviceSetComputeMode(device, NVML_COMPUTEMODE_PROHIBITED);
			if (NVML_ERROR_NO_PERMISSION == result)
				logger->Warn("Need root privileges to do that: %s", nvmlErrorString(result));
			else if (NVML_ERROR_NOT_SUPPORTED == result)
				logger->Warn("Compute mode prohibited not supported. You might be running on"
				             "windows in WDDM driver model or on non-CUDA capable GPU.");

			else if (NVML_SUCCESS != result) {
				logger->Warn("Failed to set compute mode for device %i: %s", i,
				             nvmlErrorString(result));
			} else {
				//All is gone correctly
				logger->Debug("Device initialized correctly");
				// Mapping information Devices per devices
				info_map.insert(std::pair<nvmlDevice_t, DeviceInfo>(device, device_info));
			}
		}
	}
	initialized = true;
	logger->Notice("NVIDIA: Devices [#=%d] information initialized",
	               devices_map.size());

	std::map<BBQUE_RID_TYPE, nvmlDevice_t>::iterator it = devices_map.begin();
	for (; it != devices_map.end(); ++it) {
		auto it2 = info_map.find(it->second);
		if (it2 != info_map.end()) {
			logger->Debug("Restoring device's compute mode back to '%s'",
			              convertToComputeModeString(it2->second.compute_mode));
			result = nvmlDeviceSetComputeMode(it->second, it2->second.compute_mode);
			if (NVML_SUCCESS != result) {
				logger->Warn("Failed to restore compute mode for device %s: %s",
				             it2->second.name, nvmlErrorString(result));
			}
			logger->Debug("Restored correctly");
		} else
			logger->Warn("Error inside the matching between device_map and info_map");

	}
}

NVIDIAPowerManager::~NVIDIAPowerManager()
{
	nvmlReturn_t result;

	std::map<BBQUE_RID_TYPE, nvmlDevice_t>::iterator it = devices_map.begin();
	for (; it != devices_map.end(); ++it) {
		auto it2 = info_map.find(it->second);
		if (it2 != info_map.end()) {
			logger->Debug("Restoring device's compute mode back to '%s'",
			              convertToComputeModeString(it2->second.compute_mode));
			result = nvmlDeviceSetComputeMode(it->second, it2->second.compute_mode);
			if (NVML_SUCCESS != result) {
				logger->Warn("Failed to restore compute mode for device %s: %s",
				             it2->second.name, nvmlErrorString(result));
			}
			logger->Debug("Restored correctly");
		} else
			logger->Warn("Error inside the matching between device_map and info_map");

	}

	result = nvmlShutdown();
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: Failed to shutdown NVML: [Err:%s]",
		             nvmlErrorString(result));
	}
	logger->Notice("NVML shutdownend correctly");

	devices_map.clear();
	info_map.clear();
}

int NVIDIAPowerManager::GetDeviceId(br::ResourcePathPtr_t const & rp,
                                    nvmlDevice_t & device) const
{
	std::map<BBQUE_RID_TYPE, nvmlDevice_t>::const_iterator it;
	std::map<nvmlDevice_t, DeviceInfo>::const_iterator it2;
	if (rp == nullptr) {
		logger->Debug("NVML: Null resource path");
		return -1;
	}

	it = devices_map.find(rp->GetID(br::ResourceType::GPU));
	it2 = info_map.find(it->second);
	if (it == devices_map.end()) {
		logger->Warn("NVML: Missing GPU id=%d", rp->GetID(br::ResourceType::GPU));
		return -2;
	}
	if (it2 == info_map.end()) {
		logger->Warn("NVML: Missing GPU id=%d information",
		             rp->GetID(br::ResourceType::GPU));
		return -2;
	}

	device = it->second;
	return it2->second.id_num;
}

PowerManager::PMResult
NVIDIAPowerManager::GetLoad(br::ResourcePathPtr_t const & rp, uint32_t & perc)
{
	nvmlReturn_t result;
	nvmlUtilization_t utilization;

	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetUtilizationRates(device, &utilization);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: Cannot get GPU (Device %d) load", device);
		logger->Warn("NVML: Failed to to query the utilization rate for device %i: %s",
		             device, nvmlErrorString(result));
	}

	logger->Debug("NVML: [GPU-%d] utilization rate %u ", id_num, utilization.gpu);
	logger->Debug("NVML: [GPU-%d] memory utilization rate %u", id_num,
	              utilization.memory);
	perc = utilization.gpu;
	return PMResult::OK;
}

PowerManager::PMResult
NVIDIAPowerManager::GetTemperature(br::ResourcePathPtr_t const & rp,
                                   uint32_t & celsius)
{
	nvmlReturn_t result;
	celsius = 0;
	unsigned int temp;

	if (!initialized) {
		logger->Warn("NVML: Cannot get GPU(s) temperature");
		return PMResult::ERR_API_NOT_SUPPORTED;
	}

	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: Failed to to query the temperature: %s",
		             nvmlErrorString(result));
		logger->Warn("NVML: [GPU-%d] Temperature not available [%d]", id_num,
		             nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}
	celsius = temp;
	logger->Debug("NVML: [GPU-%d] Temperature : %d C", id_num, temp);

	return PMResult::OK;
}


/* Clock frequency */

PowerManager::PMResult
NVIDIAPowerManager::GetAvailableFrequencies(br::ResourcePathPtr_t const & rp,
                std::vector<uint32_t> & freqs)
{
	nvmlReturn_t result;
	unsigned int memoryClockMHz;
	unsigned int clockMhz[numFreq], count = numFreq;
	std::vector<uint32_t>::iterator it;

	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetClockInfo (device, NVML_CLOCK_MEM , &memoryClockMHz);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: [GPU-%d] Failed to query the graphic clock inside GetAvailableFrequencies: %s",
		             id_num, nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}

	result = nvmlDeviceGetSupportedGraphicsClocks (device, memoryClockMHz , &count,
	                clockMhz);
	//result = nvmlDeviceGetSupportedGraphicsClocks(device, NVML_CLOCK_GRAPHICS, &count, clockMhz);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: [GPU-%d] Failed to query the supported graphic clock: %s",
		             id_num, nvmlErrorString(result));
		//freqs[0] = -1;
		return PMResult::ERR_API_INVALID_VALUE;
	}

	it = freqs.begin();

	for (int i = 0; it != freqs.end(); ++i, it++) {
		freqs.insert(it, (uint32_t)clockMhz[i]);
		logger->Debug("NVML: [GPU-%d] possible clock frequency: %d Mhz", id_num,
		              clockMhz[i]);

	}

	return PMResult::OK;
}

PowerManager::PMResult
NVIDIAPowerManager::GetClockFrequency(br::ResourcePathPtr_t const & rp,
                                      uint32_t & khz)
{
	nvmlReturn_t result;
	unsigned int var;
	khz = 0;

	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetClockInfo (device, NVML_CLOCK_GRAPHICS , &var);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: Failed to to query the graphic clock: %s",
		             nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}

	khz = var;
	logger->Debug("NVML: [GPU-%d] clock frequency: %d Mhz", id_num, var);

	return PMResult::OK;
}

PowerManager::PMResult
NVIDIAPowerManager::SetClockFrequency(br::ResourcePathPtr_t const & rp,
                                      uint32_t khz)
{
	nvmlReturn_t result;
	unsigned int memClockMHz;
	khz = khz * 1000;

	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetClockInfo (device, NVML_CLOCK_MEM , &memClockMHz);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: Failed to to query the memory graphic clock inside method SetClockFrequency: %s",
		             nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}

	result = nvmlDeviceSetApplicationsClocks(device, memClockMHz, khz);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: Failed to to set the graphic clock: %s",
		             nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}

	logger->Debug("NVML: [GPU-%d] clock setted at frequency: %d Mhz", id_num, khz);

	return PMResult::OK;
}


PowerManager::PMResult
NVIDIAPowerManager::GetClockFrequencyInfo(br::ResourcePathPtr_t const & rp,
                uint32_t & khz_min, uint32_t & khz_max, uint32_t & khz_step)
{
	nvmlReturn_t result;
	unsigned int var;
	khz_min = khz_max = khz_step = 0;
	br::ResourceType r_type = rp->Type();

	GET_DEVICE_ID(rp, device);

	if (r_type == br::ResourceType::PROC_ELEMENT) {
		result = nvmlDeviceGetDefaultApplicationsClock(device, NVML_CLOCK_GRAPHICS,
		                &var);
		if (NVML_SUCCESS != result) {
			logger->Warn("NVML: [GPU-%d] PROC_ELEMENT Failed to to query the default graphic clock: %s",
			             id_num, nvmlErrorString(result));
			return PMResult::ERR_API_INVALID_VALUE;
		}
		khz_min  = var;
		logger->Debug("NVML: [GPU-%d] Min GPU freq %d Mhz", id_num, khz_min);
		result = nvmlDeviceGetMaxClockInfo(device, NVML_CLOCK_GRAPHICS, &var);
		if (NVML_SUCCESS != result) {
			logger->Warn("NVML: [GPU-%d] Failed to to query the max graphic clock: %s",
			             id_num, nvmlErrorString(result));
			return PMResult::ERR_API_INVALID_VALUE;
		}
		khz_max  = var;
		khz_step = 1;
		logger->Debug("NVML: [GPU-%d] Max GPU freq %d Mhz", id_num, khz_max);
		logger->Debug("NVML: [GPU-%d] Step  freq %d Mhz", id_num, khz_step);
	} else if (r_type == br::ResourceType::MEMORY) {
		result = nvmlDeviceGetDefaultApplicationsClock(device, NVML_CLOCK_MEM, &var);
		if (NVML_SUCCESS != result) {
			logger->Warn("NVML: [GPU-%d] MEMORY Failed to to query the default memory clock: %s",
			             id_num, nvmlErrorString(result));
			return PMResult::ERR_API_INVALID_VALUE;
		}
		khz_min  = var;
		logger->Debug("NVML: [GPU-%d] Min Mem freq %d Mhz", id_num, khz_min);
		result = nvmlDeviceGetMaxClockInfo (device, NVML_CLOCK_MEM , &var);
		if (NVML_SUCCESS != result) {
			printf("NVML: [GPU-%d] Failed to to query the max memory clock: %s", id_num,
			       nvmlErrorString(result));
			return PMResult::ERR_API_INVALID_VALUE;
		}
		khz_max  = var;
		khz_step = 1;
		logger->Debug("NVML: [GPU-%d] Max Mem freq %d Mhz", id_num, khz_max);
		logger->Debug("NVML: [GPU-%d] Step freq %d Mhz", id_num, khz_step);
	}

	return PMResult::OK;
}


/* Fan */

PowerManager::PMResult
NVIDIAPowerManager::GetFanSpeed(br::ResourcePathPtr_t const & rp,
                                FanSpeedType fs_type,	uint32_t & value)
{
	nvmlReturn_t result;
	unsigned int var;
	value = 0;

	GET_DEVICE_ID(rp, device);

	// Fan speed type
	if (fs_type == FanSpeedType::PERCENT) {
		result = nvmlDeviceGetFanSpeed (device, &var);
		if (NVML_SUCCESS != result) {
			logger->Warn("NVML: [GPU-%d] Failed to to query the fan speed: %s", id_num,
			             nvmlErrorString(result));
			return PMResult::ERR_API_INVALID_VALUE;
		}
		logger->Debug("NVML: [GPU-%d] Fan speed: %d%c ", id_num, var, '%');
		value = (uint32_t) var;
	} else if (fs_type == FanSpeedType::RPM) {
		logger->Warn("NVML: RPM fan speed is not supported for NVIDIA GPUs");
		value = 0;
	}

	return PMResult::OK;
}


/* Power */

PowerManager::PMResult
NVIDIAPowerManager::GetPowerUsage(br::ResourcePathPtr_t const & rp,
                                  uint32_t & mwatt)
{
	nvmlReturn_t result;
	unsigned int var;

	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetPowerUsage (device, &var);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: [GPU-%d] Failed to to query the power usage: %s", id_num,
		             nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}
	logger->Debug("NVML: [GPU-%d] the power usage of GPU is %d mW [+/-5%c]", id_num,
	              var, '%');
	mwatt = var;

	return PMResult::OK;
}

PowerManager::PMResult
NVIDIAPowerManager::GetPowerInfo(br::ResourcePathPtr_t const & rp,
                                 uint32_t & mwatt_min, uint32_t & mwatt_max)
{
	nvmlReturn_t result;
	unsigned int min, max;

	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetPowerManagementLimitConstraints(device, &min, &max);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: [GPU-%d]  Failed to to query the power information: %s",
		             id_num, nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}

	mwatt_min = min;
	mwatt_max = max;

	return PMResult::OK;
}

PowerManager::PMResult
NVIDIAPowerManager::GetPowerState(br::ResourcePathPtr_t const & rp,
                                  uint32_t & state)
{
	nvmlPstates_t pState;
	nvmlReturn_t result;
	state = 0;
	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetPerformanceState(device, &pState);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: [GPU-%d] Failed to to query the power state: %s", id_num,
		             nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}

	state = (uint32_t) pState;

	return PMResult::OK;
}

/* States */


#define NVIDIA_GPU_PSTATE_MAX    0
#define NVIDIA_GPU_PSTATE_MIN   15


PowerManager::PMResult
NVIDIAPowerManager::GetPowerStatesInfo(br::ResourcePathPtr_t const & rp,
                                       uint32_t & min, uint32_t & max, int & step)
{
	(void) rp;
	min  = NVIDIA_GPU_PSTATE_MIN;
	max  = NVIDIA_GPU_PSTATE_MAX;
	step = 1;

	return PMResult::OK;
}


PowerManager::PMResult
NVIDIAPowerManager::GetPerformanceState(br::ResourcePathPtr_t const & rp,
                                        uint32_t & state)
{
	nvmlPstates_t pState;
	nvmlReturn_t result;
	state = 0;
	GET_DEVICE_ID(rp, device);

	result = nvmlDeviceGetPerformanceState(device, &pState);
	if (NVML_SUCCESS != result) {
		logger->Warn("NVML: [GPU-%d] Failed to to query the performance state: %s",
		             id_num, nvmlErrorString(result));
		return PMResult::ERR_API_INVALID_VALUE;
	}

	logger->Debug("Power state has an interval [%d-%d]+{32}:",
		NVIDIA_GPU_PSTATE_MAX, NVIDIA_GPU_PSTATE_MIN);
	logger->Debug("*) %d for Maximum Performance", NVIDIA_GPU_PSTATE_MAX);
	logger->Debug("*) %d for Minimum Performance", NVIDIA_GPU_PSTATE_MIN);
	logger->Debug("*) 32 Unknown performance state");
	logger->Debug("NVML: [GPU-%d] PerformanceState: %u ", id_num, pState);
	state = (uint32_t) pState;

	return PMResult::OK;
}

PowerManager::PMResult
NVIDIAPowerManager::GetPerformanceStatesCount(br::ResourcePathPtr_t const & rp,
                uint32_t & count)
{
	(void) rp;

	count = NVIDIA_GPU_PSTATE_MIN - NVIDIA_GPU_PSTATE_MAX;

	return PMResult::OK;
}



} // namespace bbque

