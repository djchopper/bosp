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

#ifndef BBQUE_POWER_MANAGER_NVIDIA_H_
#define BBQUE_POWER_MANAGER_NVIDIA_H_

#include <map>

#include "nvml.h"

#include "bbque/pm/power_manager.h"
#include "bbque/res/resource_type.h"
#include "bbque/res/resource_path.h"

#define NVIDIA_VENDOR     "NVIDIA"
#define NVML_NAME         "libnvidia-ml.so"


namespace br = bbque::res;

namespace bbque {


/**
 * @class NVIDIAPowerManager
 *
 * @brief Provide power management related API for NVIDIA GPU devices,
 * by extending @ref PowerManager class.
 */
class NVIDIAPowerManager: public PowerManager {

public:

	NVIDIAPowerManager();

	~NVIDIAPowerManager();

    static NVIDIAPowerManager & GetInstance();


    PMResult GetLoad(br::ResourcePathPtr_t const & rp, uint32_t & perc);


	PMResult GetTemperature(br::ResourcePathPtr_t const & rp, uint32_t & celsius);


	PMResult GetClockFrequency(br::ResourcePathPtr_t const & rp, uint32_t & khz);


	PMResult SetClockFrequency(br::ResourcePathPtr_t const & rp, uint32_t khz);


	PMResult GetClockFrequencyInfo(br::ResourcePathPtr_t const & rp,
	                               uint32_t & khz_min, uint32_t & khz_max,
	                               uint32_t & khz_step);


	PMResult GetAvailableFrequencies(br::ResourcePathPtr_t const & rp,
	                                 std::vector<uint32_t> & freqs);


	PMResult GetFanSpeed(br::ResourcePathPtr_t const & rp, FanSpeedType fs_type,
	                     uint32_t & value);


	PMResult GetPowerUsage(br::ResourcePathPtr_t const & rp, uint32_t & mwatt);


	PMResult GetPowerState(br::ResourcePathPtr_t const & rp, uint32_t & state);


	PMResult GetPowerInfo(br::ResourcePathPtr_t const & rp, uint32_t & mwatt_min,
	                      uint32_t & mwatt_max);


	PMResult GetPowerStatesInfo(br::ResourcePathPtr_t const & rp, uint32_t & min,
	                            uint32_t & max, int & step);


	PMResult GetPerformanceState(br::ResourcePathPtr_t const & rp,
	                             uint32_t & state);

	PMResult GetPerformanceStatesCount(br::ResourcePathPtr_t const & rp,
	                                   uint32_t & count);

private:

	bool initialized = false;

	struct DeviceInfo {
		char name[NVML_DEVICE_NAME_BUFFER_SIZE];
		nvmlPciInfo_t pci;
		nvmlComputeMode_t compute_mode;
		unsigned int id_num;
	};


	/***  Pointer to the NVIDIA Display Library  */
	void * nvmlib;

	/*** Number of available GPU in the system */
	unsigned int device_count;

	/*** Mapping BBQ resource id -> NVML device id */
	std::map<BBQUE_RID_TYPE, nvmlDevice_t> devices_map;

	/*** Information retreived for each device */
	std::map<nvmlDevice_t, DeviceInfo> info_map;


	/**
	 * @brief Load devices information
	 */
	void LoadDevicesInfo();

	/**
	 * @brief Get the platform device id (returned from NVML)
	 *
	 * @param rp Resource path of the GPU (including '.pe')
	 * @param device is the variable where will be placed the device ID
	 * @return PMResult::OK if success
	 */
	int GetDeviceId(br::ResourcePathPtr_t const & rp, nvmlDevice_t & device) const;

};

}

#endif // BBQUE_POWER_MANAGER_NVIDIA_H_

