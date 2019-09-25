#ifndef BBQUE_LINUX_NVIDIAPROXY_H_
#define BBQUE_LINUX_NVIDIAPROXY_H_

#include <fstream>
#include <list>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nvml.h"

#include "bbque/config.h"
#include "bbque/configuration_manager.h"
#include "bbque/command_manager.h"
#include "bbque/modules_factory.h"
#include "bbque/platform_proxy.h"
#include "bbque/app/application.h"
#include "bbque/pm/power_manager.h"
#include "bbque/pm/power_manager_nvidia.h"
#include "bbque/res/identifier.h"
#include "bbque/utils/worker.h"

namespace ba = bbque::app;
namespace br = bbque::res;
namespace bu = bbque::utils;

namespace bbque {

typedef std::list<ResourcePathPtr_t> ResourcePathList_t;
typedef std::shared_ptr<ResourcePathList_t> ResourcePathListPtr_t;
typedef std::vector<uint8_t> VectorUInt8_t;
typedef std::shared_ptr<VectorUInt8_t> VectorUInt8Ptr_t;
typedef std::map<br::ResourceType, VectorUInt8Ptr_t> ResourceTypeIDMap_t;
typedef std::map<br::ResourceType, ResourcePathListPtr_t> ResourceTypePathMap_t;
typedef std::map<int, std::ofstream *> DevFileMap_t;
typedef std::map<int, ResourcePathPtr_t> DevPathMap_t;


class NVMLPlatformProxy: public PlatformProxy {

public:

	static NVMLPlatformProxy * GetInstance();

	/**
	 * @brief Destructor
	 */
	virtual ~NVMLPlatformProxy();

	/**
	 * @brief Return the Platform specific string identifier
	 */
	const char * GetPlatformID(int16_t system_id = -1) const override final {
		(void) system_id;
		return BBQUE_OPENCL_PLATFORM;
	}

	/**
	 * @brief Return the Hardware identifier string
	 */
	const char * GetHardwareID(int16_t system_id = -1) const override final {
		(void) system_id;
		return "opencl";
	}

    /**
     * @brief Nvidia specific resource setup interface.
     */
    ExitCode_t Setup(SchedPtr_t papp);

	/**
	 * @brief Load NVML device data function
	 */
	ExitCode_t LoadPlatformData() override final;

    /**
     * @brief Nvidia specific resources refresh
     */
    ExitCode_t Refresh();

    /**
     * @brief Nvidia specific resources release interface.
     */
    ExitCode_t Release(SchedPtr_t papp);

    /**
     * @brief Nvidia specific resource claiming interface.
     */
    ExitCode_t ReclaimResources(SchedPtr_t papp);

    /**
     * @brief Nvidia resource assignment mapping
     */
    ExitCode_t MapResources(
            SchedPtr_t papp, ResourceAssignmentMapPtr_t pres, bool excl = true);

    /**
	 * nvml specific termination
	 */
	void Exit() override;


	bool IsHighPerformance(bbque::res::ResourcePathPtr_t const & path) const;


	// Class specific member functions

	/**
	 * @brief Number of nvml devices of a given resource type
	 */
	uint8_t GetDevicesNum(br::ResourceType r_type) const;

	/**
	 * @brief Set of NVML device IDs for a given resource type
	 */
	VectorUInt8Ptr_t GetDeviceIDs(br::ResourceType r_type) const;

	/**
	 * @brief Set of NVML device resource path for a given type
	 */
	ResourcePathListPtr_t GetDevicePaths(br::ResourceType r_type) const;

private:

	/*** Number of available GPU in the system */
	unsigned int device_count = 0;

	/*** Device descriptors   */
	nvmlDevice_t * devices = nullptr;

	/*** Configuration manager instance */
	ConfigurationManager & cm;

	/*** Command manager instance */
	CommandManager & cmm;

	/**
	 * @brief The logger used by the power manager.
	 */
	std::unique_ptr<bu::Logger> logger;

	/*** Map with all the device IDs for each type available   */
	ResourceTypeIDMap_t   device_ids;

	/*** Map with all the device paths for each type available */
	ResourceTypePathMap_t device_paths;

	/*** Map with the resource paths of GPUs memory */
	DevPathMap_t gpu_mem_paths;


	/*** Constructor */
	NVMLPlatformProxy();

	/** Retrieve the iterator for the vector of device IDs, given a type */
	ResourceTypeIDMap_t::iterator GetDeviceIterator(
	        br::ResourceType r_type);

	/** Retrieve the constant iterator for the vector of device IDs, given a type */
	ResourceTypeIDMap_t::const_iterator GetDeviceConstIterator(
	        br::ResourceType r_type) const;

#ifdef CONFIG_BBQUE_PM_NVIDIA

	/*** Power Manager instance */
	NVIDIAPowerManager & pm;

	/*** Print the GPU power information */
	void PrintGPUPowerInfo();

#endif // CONFIG_BBQUE_PM_NVIDIA

	/**
	 * @brief Append device ID per device type
	 *
	 * @param r_type The resource type (usually ResourceType::GPU)
	 * @param dev_id The NVIDIA device ID
	 */
	void InsertDeviceID(br::ResourceType r_type, uint8_t dev_id);

	/**
	 * @brief Append resource path per device type
	 *
	 * @param r_type The resource type (usually ResourceType::GPU)
	 * @param dev_p_str A resource path referencing a device of the type in the key
	 */
	void InsertDevicePath(
	        br::ResourceType r_type, std::string const & dev_rp_str);

	/**
	 * @brief Register device resources
	 */
	ExitCode_t RegisterDevices();

};

} // namespace bbque

#endif
