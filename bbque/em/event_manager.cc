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

#include "bbque/em/event_manager.h"
#include "bbque/utils/utility.h"

#include <fstream>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/serialization/serialization.hpp>
//#include <boost/archive/binary_oarchive.hpp>
//#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

using namespace std::chrono;

namespace bbque {

std::string EventManager::TimeToString(time_t t) {
   std::stringstream strm;
   strm << t;
   return strm.str();
}

EventManager::EventManager() {
	// Get a logger module
	logger = bu::Logger::GetLogger(EVENT_MANAGER_NAMESPACE);
	assert(logger);

	std::time_t timestamp = std::time(nullptr);

	archive_path = ARCHIVE_FOLDER + std::string("events_") + TimeToString(timestamp) + std::string(".txt");

	// Create events folder if it doesn't exist
	const char* folder_path = std::string(ARCHIVE_FOLDER).c_str();
    boost::filesystem::path dir(folder_path);
    if(boost::filesystem::create_directory(dir))
    {
        logger->Info("Create events Archive folder...");
    }

	std::ofstream ofs(archive_path);
}

EventManager::~EventManager() {

}

EventManager & EventManager::GetInstance() {
	static EventManager instance;
	return instance;
}

void EventManager::Serialize(EventWrapper ew) {

	// create and open the archive for output
	std::ofstream ofs(archive_path);

	if (ofs.good()) {
		try {
			boost::archive::text_oarchive oa(ofs, boost::archive::no_header);
			oa << ew;
		} catch (...) {
			logger->Error("Cannot write the archive");
		}
	} else {
		logger->Error("Cannot open archive for writing");
    }
}

EventWrapper EventManager::Deserialize() {

	EventWrapper ew;

	// open the archive for input
    std::ifstream ifs(archive_path);

    if (ifs.good()) {
    	try {
    		boost::archive::text_iarchive ia(ifs, boost::archive::no_header);
	        ia >> ew;
	    } catch (...) {
			logger->Error("Cannot read the archive");
		}
	} else {
		logger->Error("Cannot open archive for reading");
    }

    return ew;
}

void EventManager::InitializeArchive(Event event) {
	logger->Info("Initialize Archive...");

	milliseconds timestamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
	event.SetTimestamp(timestamp);

	EventWrapper ew;
    ew.AddEvent(event);
    Serialize(ew);
}

void EventManager::Push(Event event) {
	logger->Info("Push Event...");

	milliseconds timestamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
	event.SetTimestamp(timestamp);

	EventWrapper ew;
	
    ew = Deserialize();
    ew.AddEvent(event);
    Serialize(ew);
}

} // namespace bbque