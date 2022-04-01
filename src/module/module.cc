/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * Copyright (C) 2021 Perry Werneck <perry.werneck@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

 #include "private.h"
 #include <pugixml.hpp>
 #include <udjat/tools/object.h>
 #include <udjat/tools/string.h>
 #include <udjat/tools/configuration.h>
 #include <udjat/tools/application.h>
 #include <udjat/moduleinfo.h>
 #include <udjat/sqlite/database.h>
 #include <udjat/module.h>

 using namespace std;

 namespace Udjat {

	/// @brief Get attribute from configuration file.
	static String getAttribute(const char *name, const char *def) {
		return String{Config::Value<string>("sql",name,def)};
	}

	/// @brief Get attribute from xml with fallback to configuration file.
	static String getAttribute(const pugi::xml_node &node, const char *name, const char *def) {
		auto attribute = Object::getAttribute(node, name, false);
		if(attribute) {
			return String{attribute.as_string(def)}.expand(node);
		}
		return getAttribute(name,def).expand(node);
	}

	/// @brief Create module from configuration file.
	static const Udjat::ModuleInfo moduleinfo{"SQLite " SQLITE_VERSION " module"};

	SQLite::Module::Module() : Udjat::Module("sqlite",moduleinfo), Udjat::Factory("sql",moduleinfo) {

		// Open SQLite database
#ifdef DEBUG
		open(Application::DataFile(getAttribute("dbname","./sqlite.db").c_str()).c_str());
#else
		open(Application::DataFile(getAttribute("dbname","sqlite.db").c_str()).c_str());
#endif // DEBUG

	}

	/// @brief Create module from XML definition with fallback to configuration file.
	SQLite::Module::Module(const pugi::xml_node &node) : Udjat::Module("sqlite",moduleinfo), Udjat::Factory("sql",moduleinfo) {

		// Open SQLite database
		open(Application::DataFile(getAttribute(node,"dbname","sqlite.db").c_str()).c_str());

	}

	SQLite::Module::~Module() {
	}

 }