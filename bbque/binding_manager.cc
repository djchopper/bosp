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
#include <string>

#include "bbque/binding_manager.h"
#include "bbque/configuration_manager.h"
#include "bbque/res/resource_path.h"

#define MODULE_NAMESPACE   "bq.bdm"
#define MODULE_CONFIG      "BindingManager"


namespace bbque {


BindingManager & BindingManager::GetInstance() {
	static BindingManager instance;
	return instance;
}

BindingManager::BindingManager():
		logger(bbque::utils::Logger::GetLogger(MODULE_NAMESPACE)),
		ra(ResourceAccounter::GetInstance()) {}


void BindingManager::InitBindingDomains() {

	// Binding domain resource path
	ConfigurationManager & config_manager(ConfigurationManager::GetInstance());
	std::string domains_str;
	boost::program_options::options_description opts_desc("BindingManager options (domains)");
	opts_desc.add_options()
		(MODULE_CONFIG ".domains",
		 boost::program_options::value<std::string>(&domains_str)->default_value("cpu"),
		"Resource binding domain");
	boost::program_options::variables_map opts_vm;
	config_manager.ParseConfigurationFile(opts_desc, opts_vm);
	logger->Info("Binding domains: %s", domains_str.c_str());

	// Parse each binding domain string
	size_t end_pos = 0;
	std::string binding_str;
	br::ResourceType type;
	while (end_pos != std::string::npos) {
		end_pos     = domains_str.find(',');
		binding_str = domains_str.substr(0, end_pos);
		domains_str.erase(0, end_pos + 1);

		// Binding domain resource path
		br::ResourcePathPtr_t base_path =
			std::make_shared<br::ResourcePath>(ra.GetPrefixPath());
		base_path->Concat(binding_str);

		// Binding domain resource type check
		type = base_path->Type();
		if (type == br::ResourceType::UNDEFINED) {
			logger->Error("Binding: Invalid domain type <%s>",
					binding_str.c_str());
			continue;
		}

#ifndef CONFIG_BBQUE_OPENCL
		if (type == br::ResourceType::GPU) {
			logger->Info("Binding: OpenCL support disabled."
					" Discarding <GPU> binding type");
			continue;
		}
#endif
		// New binding info structure
		domains.emplace(type, std::make_shared<BindingInfo_t>());
		domains[type]->base_path = base_path;
		logger->Info("Resource binding domain: '%s' Type:<%s>",
				domains[type]->base_path->ToString().c_str(),
				br::GetResourceTypeString(type));
	}
}


BindingManager::ExitCode_t BindingManager::LoadBindingDomains() {

	InitBindingDomains();
	if (domains.empty()) {
		logger->Fatal("Missing binding domains");
		return ERR_MISSING_OPTIONS;
	}

	// Set information for each binding domain
	for (auto & bd_entry: domains) {
		BindingInfo & binding(*(bd_entry.second));
		binding.resources = ra.GetResources(binding.base_path);
		binding.r_ids.clear();

		// Skip missing resource bindings
		if (binding.resources.empty()) {
			logger->Warn("Init: No bindings R<%s> available",
					binding.base_path->ToString().c_str());
		}

		// Get all the possible resource binding IDs
		for (br::ResourcePtr_t & rsrc: binding.resources) {
			binding.r_ids.emplace(rsrc->ID());
			logger->Info("Init: R<%s> ID: %d",
					binding.base_path->ToString().c_str(), rsrc->ID());
		}
		logger->Info("Init: R<%s>: %d possible bindings",
				binding.base_path->ToString().c_str(),
				binding.resources.size());
	}

	return OK;
}

} // namespace bbque

