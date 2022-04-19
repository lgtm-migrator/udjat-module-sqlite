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

 #include <config.h>
 #include <pugixml.hpp>
 #include <udjat/sqlite/sql.h>
 #include <udjat/tools/quark.h>
 #include <udjat/tools/http/client.h>
 #include <udjat/moduleinfo.h>
 #include <udjat/sqlite/database.h>
 #include <udjat/sqlite/statement.h>
 #include <udjat/tools/mainloop.h>
 #include <udjat/tools/threadpool.h>
 #include <udjat/tools/timestamp.h>
 #include <udjat/tools/systemservice.h>
 #include <string>

#ifndef _WIN32
	#include <unistd.h>
#endif // _WIN32

 #include "private.h"

 using namespace std;

 namespace Udjat {

	static const char * child_value(const pugi::xml_node &node, const char *name, bool required = true) {
		auto child = node.child(name);
		if(!child) {
			if(required) {
				throw runtime_error(string{"Required child '"} + name + "' not found");
			}
			return "";
		}
		String sql{child.child_value()};
		sql.strip();
		sql.expand(node);

		return Quark(sql).c_str();
	}

	int64_t SQLite::Protocol::count() const {
		int64_t pending_messages = 0;
		if(pending && *pending) {
			Statement sql{pending};
			sql.step();
			sql.get(0,pending_messages);
		}
		return pending_messages;
	}

	SQLite::Protocol::Protocol(const pugi::xml_node &node) : Udjat::Protocol(Quark(node,"name","sql",false).c_str(),SQLite::Module::moduleinfo), ins(child_value(node,"insert")), del(child_value(node,"delete")), select(child_value(node,"select")), pending(child_value(node,"pending",false)) {

		retry.delay = Object::getAttribute(node, "sqlite", "retry-delay", (unsigned int) retry.delay);
		retry.interval = Object::getAttribute(node, "sqlite", "retry-interval", (unsigned int) retry.interval) * 1000;
		retry.when_busy = Object::getAttribute(node, "sqlite", "retry-when-busy", (unsigned int) retry.when_busy) * 1000;
		retry.notify = Object::getAttribute(node, "sqlite", "notify", pending && *pending);

		if(retry.interval) {

			Udjat::Protocol::info() << "URL retry timer set to " << (retry.interval/1000) << " seconds" << endl;

			MainLoop::getInstance().insert(this, retry.interval, [this]() {

				MainLoop::getInstance().reset(this,retry.when_busy);
				if(!busy) {
					busy = true;
					ThreadPool::getInstance().push([this](){

						try {

							send();

						} catch(const std::exception &e) {

							warning() << "Error '" << e.what() << "' while sending queued requests" << endl;

						} catch(...) {

							warning() << "Unexpected error while sending queued requests" << endl;

						}

						// Do we need to change state?
						if(retry.notify && pending && *pending) {

							class SQLState : public Udjat::State<int64_t> {
							private:
								string msg;

							public:
								SQLState(int64_t value) : Udjat::State<int64_t>("SQLite",value,Level::ready) {

									if(value == 0) {
										msg = "No pending messages";
									} else if(value == 1) {
										msg = "1 pending message";
									} else {
										msg = std::to_string(value);
										msg += " pending messages";
									}

									Object::properties.summary = msg.c_str();

								}
							};

							auto systemservice = SystemService::getInstance();

							if(systemservice) {

								if(retry.state) {
									systemservice->deactivate(retry.state);
								}

								retry.state = make_shared<SQLState>(count());

								systemservice->activate(retry.state);

							}

						}


						// Schedule next retry.
						cout << "Scheduling retry to " << TimeStamp(time(0)+(retry.interval /1000)) << endl;
						MainLoop::getInstance().reset(this,retry.interval);
						busy = false;
					});
				}

				return true;

			});

		}

		for(pugi::xml_node child = node.child("init"); child; child = child.next_sibling("init")) {

			String sql{child.child_value()};
			sql.strip();
			sql.expand(child);

#ifdef DEBUG
			cout << sql << endl;
#endif // DEBUG

			Database::getInstance().exec(sql.c_str());

		}

		if(pending && *pending) {

			try {

				uint64_t pending_messages = count();

				if(!pending_messages) {
					info() << "No pending requests" << endl;
				} else if(pending_messages == 1) {
					warning() << "1 pending request" << endl;
				} else {
					warning() << pending_messages << " pending requests" << endl;
				}

			} catch(const std::exception &e) {

				error() << "Error '" << e.what() << "' counting pending requests" << endl;

			}

		}

	}

	SQLite::Protocol::~Protocol() {
		MainLoop::getInstance().remove(this);
	}

	void SQLite::Protocol::send() const {

		Statement del(this->del);
		Statement select(this->select);
		MainLoop &mainloop = MainLoop::getInstance();

		while(select.step() == SQLITE_ROW && mainloop) {

			int64_t id;
			Udjat::URL url;
			string action, payload;

			select.get(0,id);
			select.get(1,url);
			select.get(2,action);
			select.get(3,payload);

			info() << "Sending " << action << " " << url << " (" << id << ")" << endl << payload << endl;

			HTTP::Client client(url);

			switch(HTTP::MethodFactory(action.c_str())) {
			case HTTP::Get:
				cout << client.get() << endl;
				break;
			case HTTP::Post:
				cout << client.post(payload.c_str()) << endl;
				break;

			default:
				error() << "Unexpected verb '" << action << "' sending queued request, ignoring" << endl;
			}

			info() << "Removing request '" << id << "' from URL queue" << endl;
			del.bind(1,id).exec();

			del.reset();
			select.reset();

#ifdef _WIN32
			Sleep(retry.delay * 100);
#else
			sleep(retry.delay);
#endif // _WIN32

		}

	}

	std::shared_ptr<Protocol::Worker> SQLite::Protocol::WorkerFactory() const {

		class Worker : public Udjat::Protocol::Worker {
		private:
			const char *sql;
			const Protocol *protocol = nullptr;

		public:
			Worker(const Protocol *p, const char *s) : sql(s), protocol(p) {
			}

			virtual ~Worker() {
			}

			String get(const std::function<bool(double current, double total)> &progress) override {

//#ifdef DEBUG
//				cout << "Inserting " << method() << " '" << url() << "'" << endl;
//#endif // DEBUG

				progress(0,0);

				// Get SQL
				String sql{this->sql};
				sql.expand(true,true);

				// Prepare.
				Statement stmt(sql.c_str());

				// Arguments: URL, VERB, Payload
				stmt.bind(
					url().c_str(),
					std::to_string(method()),
					payload(),
					nullptr
				);

				stmt.exec();

				// Reset timer.
				MainLoop::getInstance().reset(protocol,100);

				// Force as complete.
				progress(1,1);
				return "";
			}

		};

		return make_shared<Worker>(this,ins);
	}


 }


