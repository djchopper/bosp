/*
 * Copyright (C) 2014  Politecnico di Milano
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

#include "bbque/pm/power_manager.h"
#include "bbque/res/resource_path.h"
#include "bbque/config.h"

#define MODULE_CONFIG    "PowerManager"
#define MODULE_NAMESPACE "bq.pm"

#ifdef CONFIG_BBQUE_PM_AMD
# include "bbque/pm/power_manager_amd.h"
#endif
#ifdef CONFIG_BBQUE_PM_GPU_ARM_MALI
# include "bbque/pm/power_manager_gpu_arm_mali.h"
#endif

#ifdef CONFIG_BBQUE_PM_CPU
# include "bbque/pm/power_manager_cpu.h"
#ifdef CONFIG_TARGET_ODROID_XU // Odroid XU-3
# include "bbque/pm/power_manager_cpu_odroidxu.h"
#endif // CONFIG_TARGET_ODROID_XU
#ifdef CONFIG_TARGET_ARM_CORTEX_A9
# include "bbque/pm/power_manager_cpu_arm_cortexa9.h"
#endif // CONFIG_TARGET_FREESCALE_IMX6Q
#endif

#ifdef CONFIG_BBQUE_PM_MANGO
# include "bbque/pm/power_manager_mango.h"
#endif // CONFIG_BBQUE_PM_MANGO

namespace bw = bbque::pm;

namespace bbque {

std::array<
	PowerManager::InfoType,
	int(PowerManager::InfoType::COUNT)> PowerManager::InfoTypeIndex = {{
		InfoType::LOAD        ,
		InfoType::TEMPERATURE ,
		InfoType::FREQUENCY   ,
		InfoType::POWER       ,
		InfoType::CURRENT     ,
		InfoType::VOLTAGE     ,
		InfoType::PERF_STATE  ,
		InfoType::POWER_STATE
	}};

std::array<
	const char *,
	int(PowerManager::InfoType::COUNT)> PowerManager::InfoTypeStr = {{
		"load"        ,
		"temperature" ,
		"frequency"   ,
		"power"       ,
		"current"     ,
		"voltage"     ,
		"performance state",
		"power state"
	}};

PowerManager & PowerManager::GetInstance() {
	static PowerManager instance;
	return instance;
}


PowerManager::PowerManager() {
	static bool initialized = false;
	cm = &(CommandManager::GetInstance());
	mm = &(bw::ModelManager::GetInstance());

	// Get a logger module
	logger = bu::Logger::GetLogger(MODULE_NAMESPACE);
	assert(logger);

	// Block recursion on vendor specific construction
	if (initialized) return;
	logger->Info("Initialize PowerManager...");
	initialized = true;

	// Register command to set device fan speed
#define CMD_FANSPEED_SET "fanspeed_set"
	cm->RegisterCommand(MODULE_NAMESPACE "." CMD_FANSPEED_SET,
		static_cast<CommandHandler*>(this),
		"Set the speed of the fan (percentage value)");

	// GPU
#ifdef CONFIG_BBQUE_PM_AMD
	logger->Notice("Using AMD provider for GPUs power management");
	device_managers[br::ResourceType::GPU] =
		std::shared_ptr<PowerManager>(new AMDPowerManager());
#endif
#ifdef CONFIG_BBQUE_PM_NVIDIA
    logger->Notice("Using NVIDIA provider for GPUs power management");
	device_managers[br::ResourceType::GPU] =
		std::shared_ptr<PowerManager>(new NVIDIAPowerManager());
#endif
#ifdef CONFIG_BBQUE_PM_GPU_ARM_MALI
	logger->Notice("Using ARM Mali provider for GPUs power management");
	device_managers[br::ResourceType::GPU] =
		std::shared_ptr<PowerManager>(new ARM_Mali_GPUPowerManager());
#endif

	// CPU
#ifdef CONFIG_BBQUE_PM_CPU
# ifdef CONFIG_TARGET_ODROID_XU
	logger->Notice("Using ODROID-XU CPU power management module");
	device_managers[br::ResourceType::CPU] =
		std::shared_ptr<PowerManager>(new ODROID_XU_CPUPowerManager());
	return;
# endif
# ifdef CONFIG_TARGET_ARM_CORTEX_A9
	logger->Notice("Using ARM Cortex A9 CPU power management module");
	device_managers[br::ResourceType::CPU] =
		std::shared_ptr<PowerManager>(new ARM_CortexA9_CPUPowerManager());
	return;
# endif
	// Generic
	logger->Notice("Using generic CPU power management module");
	device_managers[br::ResourceType::CPU] =
		std::shared_ptr<PowerManager>(new CPUPowerManager());
#endif // CONFIG_BBQUE_PM_CPU

	// MANGO accelerators
#ifdef CONFIG_BBQUE_PM_MANGO
	logger->Notice("Using MANGO platform power management module");
	device_managers[br::ResourceType::ACCELERATOR] =
		std::shared_ptr<PowerManager>(new MangoPowerManager());
#endif // CONFIG_BBQUE_PM_MANGO

}


PowerManager::~PowerManager() {
	device_managers.clear();
}


PowerManager::PMResult PowerManager::GetLoad(
		br::ResourcePathPtr_t const & rp, uint32_t &perc) {
	auto dm = GetDeviceManager(rp, "GetLoad");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetLoad(rp, perc);
}


PowerManager::PMResult PowerManager::GetTemperature(
		br::ResourcePathPtr_t const & rp, uint32_t &celsius) {
	auto dm = GetDeviceManager(rp, "GetTemperature");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetTemperature(rp, celsius);
}


PowerManager::PMResult
PowerManager::GetClockFrequency(br::ResourcePathPtr_t const & rp, uint32_t &khz) {
	auto dm = GetDeviceManager(rp, "GetClockFrequency");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetClockFrequency(rp, khz);
}

PowerManager::PMResult
PowerManager::GetClockFrequencyInfo(
		br::ResourcePathPtr_t const & rp,
		uint32_t &khz_min,
		uint32_t &khz_max,
		uint32_t &khz_step) {
	auto dm = GetDeviceManager(rp, "GetClockFrequencyInfo");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetClockFrequencyInfo(rp, khz_min, khz_max, khz_step);
}

PowerManager::PMResult
PowerManager::GetAvailableFrequencies(
		br::ResourcePathPtr_t const & rp,
		std::vector<uint32_t> & freqs) {
	auto dm = GetDeviceManager(rp, "GetAvailableFrequencies");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetAvailableFrequencies(rp, freqs);
}

PowerManager::PMResult
PowerManager::SetClockFrequency(br::ResourcePathPtr_t const & rp, uint32_t khz) {
	auto dm = GetDeviceManager(rp, "SetClockFrequency");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->SetClockFrequency(rp, khz);
}

PowerManager::PMResult
PowerManager::SetClockFrequency(
		br::ResourcePathPtr_t const & rp,
		uint32_t min_khz, uint32_t max_khz) {
	auto dm = GetDeviceManager(rp, "SetClockFrequency");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->SetClockFrequency(rp, min_khz, max_khz);
}

std::vector<std::string> const &
PowerManager::GetAvailableFrequencyGovernors(br::ResourcePathPtr_t const & rp) {
	auto dm = GetDeviceManager(rp, "GetAvailableFrequencyGovernors");
	if (dm != nullptr)
		dm->GetAvailableFrequencyGovernors(rp);
	return cpufreq_governors;
}

PowerManager::PMResult
PowerManager::GetClockFrequencyGovernor(
		br::ResourcePathPtr_t const & rp,
		std::string & governor) {
	auto dm = GetDeviceManager(rp, "GetClockFrequencyGovernor");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetClockFrequencyGovernor(rp, governor);
}

PowerManager::PMResult
PowerManager::SetClockFrequencyGovernor(
		br::ResourcePathPtr_t const & rp,
		std::string const & governor) {
	auto dm = GetDeviceManager(rp, "SetClockFrequencyGovernor");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->SetClockFrequencyGovernor(rp, governor);
}


PowerManager::PMResult
PowerManager::GetVoltage(br::ResourcePathPtr_t const & rp, uint32_t &volt) {
	auto dm = GetDeviceManager(rp, "GetVoltage");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetVoltage(rp, volt);
}

PowerManager::PMResult
PowerManager::GetVoltageInfo(
		br::ResourcePathPtr_t const & rp,
		uint32_t &volt_min,
		uint32_t &volt_max,
		uint32_t &volt_step) {
	auto dm = GetDeviceManager(rp, "GetVoltageInfo");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetVoltageInfo(rp, volt_min, volt_max, volt_step);
}

PowerManager::PMResult
PowerManager::SetOn(br::ResourcePathPtr_t const & rp) {
	auto dm = GetDeviceManager(rp, "SetOn");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->SetOn(rp);
}

PowerManager::PMResult
PowerManager::SetOff(br::ResourcePathPtr_t const & rp) {
	auto dm = GetDeviceManager(rp, "SetOff");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->SetOff(rp);
}

bool PowerManager::IsOn(br::ResourcePathPtr_t const & rp) const {
	auto dm = GetDeviceManager(rp, "IsOn");
	if (dm == nullptr)
		return true;
	return dm->IsOn(rp);
}

PowerManager::PMResult
PowerManager::GetFanSpeed(
		br::ResourcePathPtr_t const & rp,
		FanSpeedType fs_type,
		uint32_t &value) {
	auto dm = GetDeviceManager(rp, "GetFanSpeed");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetFanSpeed(rp, fs_type, value);
}

PowerManager::PMResult
PowerManager::GetFanSpeedInfo(
		br::ResourcePathPtr_t const & rp,
		uint32_t &rpm_min,
		uint32_t &rpm_max,
		uint32_t &rpm_step) {
	auto dm = GetDeviceManager(rp, "GetFanSpeedInfo");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetFanSpeedInfo(rp, rpm_min, rpm_max, rpm_step);
}

PowerManager::PMResult
PowerManager::SetFanSpeed(
		br::ResourcePathPtr_t const & rp,
		FanSpeedType fs_type,
		uint32_t value) {
	auto dm = GetDeviceManager(rp, "SetFanSpeed");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->SetFanSpeed(rp, fs_type, value);
}

PowerManager::PMResult
PowerManager::ResetFanSpeed(br::ResourcePathPtr_t const & rp) {
	auto dm = GetDeviceManager(rp, "ResetFanSpeed");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->ResetFanSpeed(rp);
}


PowerManager::PMResult
PowerManager::GetPowerUsage(br::ResourcePathPtr_t const & rp, uint32_t &mwatt) {
	auto dm = GetDeviceManager(rp, "GetPowerUsage");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetPowerUsage(rp, mwatt);
}

PowerManager::PMResult
PowerManager::GetPowerInfo(
		br::ResourcePathPtr_t const & rp,
		uint32_t &mwatt_min,
		uint32_t &mwatt_max) {
	auto dm = GetDeviceManager(rp, "GetPowerUsageInfo");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetPowerInfo(rp, mwatt_min, mwatt_max);
}

PowerManager::PMResult
PowerManager::GetPowerState(br::ResourcePathPtr_t const & rp, uint32_t &state) {
	auto dm = GetDeviceManager(rp, "GetPowerState");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetPowerState(rp, state);
}

PowerManager::PMResult
PowerManager::GetPowerStatesInfo(
		br::ResourcePathPtr_t const & rp,
		uint32_t & min, uint32_t & max, int & step) {
	auto dm = GetDeviceManager(rp, "GetPowerStatesInfo");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetPowerStatesInfo(rp, min, max, step);
}

PowerManager::PMResult
PowerManager::SetPowerState(br::ResourcePathPtr_t const & rp, uint32_t state) {
	auto dm = GetDeviceManager(rp, "SetPowerState");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->SetPowerState(rp, state);
}


PowerManager::PMResult
PowerManager::GetPerformanceState(br::ResourcePathPtr_t const & rp, uint32_t &state) {
	auto pm_iter = device_managers.find(rp->ParentType(rp->Type()));
	if (pm_iter == device_managers.end()) {
		logger->Warn("(PM) GetPerformanceState not supported for [%s]",
			br::GetResourceTypeString(rp->ParentType(rp->Type())));
		return PMResult::ERR_API_NOT_SUPPORTED;
	}
	return pm_iter->second->GetPerformanceState(rp, state);
}

PowerManager::PMResult
PowerManager::GetPerformanceStatesCount(br::ResourcePathPtr_t const & rp, uint32_t &count) {
	auto dm = GetDeviceManager(rp, "GetPerfStatesCount");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->GetPerformanceStatesCount(rp, count);
}

PowerManager::PMResult
PowerManager::SetPerformanceState(
		br::ResourcePathPtr_t const & rp,
		uint32_t state) {
	auto dm = GetDeviceManager(rp, "SetPerformanceState");
	if (dm == nullptr)
		return PMResult::ERR_API_NOT_SUPPORTED;
	return dm->SetPerformanceState(rp, state);
}


int PowerManager::CommandsCb(int argc, char *argv[]) {
	uint8_t cmd_offset = ::strlen(MODULE_NAMESPACE) + 1;
	char * command_id  = argv[0] + cmd_offset;

	logger->Info("Processing command [%s]", command_id);
	if (argc < 2) {
		logger->Error("(PM) Invalid command format");
		return 1;
	}

	br::ResourcePathPtr_t rp(new br::ResourcePath(argv[1]));
	if (rp == nullptr) {
		logger->Error("(PM) Please specify a valid resource path");
		return 2;
	}

	// Set fan speed
	if (!strncmp(CMD_FANSPEED_SET, command_id, strlen(CMD_FANSPEED_SET))) {
		if (argc != 3) {
			logger->Error("(PM) Usage: "
				"'%s.%s  sys[0-9].gpu[0-9] [0..100]' ",
				MODULE_NAMESPACE, CMD_FANSPEED_SET);
			return 3;
		}

		uint8_t speed_perc = atoi(argv[2]);
		return FanSpeedSetHandler(rp, speed_perc);
	}

	logger->Error("Unexpected command: %s", command_id);
	return 5;
}

int PowerManager::FanSpeedSetHandler(
		br::ResourcePathPtr_t const & rp,
		uint8_t speed_perc) {
	if (speed_perc > 100) {
		logger->Error("(PM) Invalid value (%d). Expected [0..100]");
		return 4;
	}

	logger->Notice("Setting fan speed of %s to %d %", rp->ToString().c_str(), speed_perc);
	return (int) this->SetFanSpeed(rp, FanSpeedType::PERCENT, speed_perc);
}

} // namespace bbque

