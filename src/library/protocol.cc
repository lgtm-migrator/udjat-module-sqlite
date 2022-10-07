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
 #include <udjat/sqlite/protocol.h>
 #include <udjat/tools/mainloop.h>
 #include <udjat/tools/timestamp.h>
 #include <udjat/tools/systemservice.h>
 #include <udjat/tools/logger.h>
 #include <udjat/tools/threadpool.h>
 #include <udjat/tools/intl.h>
 #include <string>

#ifndef _WIN32
	#include <unistd.h>
#endif // _WIN32

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

	/*
	Udjat::Value & SQLite::Protocol::get(Udjat::Value &value) const {
		value.set(this->value);
		return value;
	}
	*/

	static const Udjat::ModuleInfo moduleinfo{"SQLite " SQLITE_VERSION " custom protocol module"};

	SQLite::Protocol::Protocol(const pugi::xml_node &node) : Udjat::Protocol(Quark(node,"name","sql",false).c_str(),moduleinfo), ins(child_value(node,"insert")), del(child_value(node,"delete")), select(child_value(node,"select")), pending(child_value(node,"pending",false)) {

		send_delay = Object::getAttribute(node, "sqlite", "retry-delay", (unsigned int) send_delay);

		for(pugi::xml_node child = node.child("init"); child; child = child.next_sibling("init")) {

			String sql{child.child_value()};
			sql.strip();
			sql.expand(child);

#ifdef DEBUG
			cout << sql << endl;
#endif // DEBUG

			Database::getInstance().exec(sql.c_str());

		}

	}

	SQLite::Protocol::~Protocol() {
		if(busy) {
			info() << "Waiting for workers" << endl;
			ThreadPool::getInstance().wait();
		}
		info() << "Disabling " << (busy ? "an active" : "inactive") << " protocol handler" << endl;
	}

	std::shared_ptr<Abstract::State> SQLite::Protocol::state() const {

		if(pending && *pending) {

			//
			// Create default states.
			//
			std::shared_ptr<Abstract::State> state;

			if(!value) {

				state = make_shared<Abstract::State>("empty", Level::unimportant, _( "Output queue is empty" ) );

			} else if(value == 1) {

				state = make_shared<Abstract::State>("pending", Level::warning, _( "One pending request in the output queue") );

			} else {

				class State : public Abstract::State {
				private:
					std::string message;

				public:
					State(uint64_t val) : Abstract::State("pending",Level::warning) {
						message = Logger::Message( _( "{} pending requests in the output queue" ), val);
						Object::properties.summary = message.c_str();
					}
				};

				state = make_shared<State>(value);

			}

			info() << state->summary() << endl;

			return state;

		}

		return make_shared<Abstract::State>("none", Level::unimportant, _( "No pending requests") );
	}

	bool SQLite::Protocol::retry() {

#ifdef DEBUG
		trace(__FUNCTION__);
#endif // DEBUG

		try {

			send();

		} catch(const std::exception &e) {

			error() << e.what() << endl;

		}

#ifdef DEBUG
		trace(__FUNCTION__);
#endif // DEBUG
		return false;

	}

	void SQLite::Protocol::send() {

#ifdef DEBUG
		trace(__FUNCTION__," start");
#endif // DEBUG

		static mutex guard;

		{
			lock_guard<mutex> lock(guard);
			if(busy) {
				return;
			}
			busy = true;
		}

		try {

			Statement del(this->del);
			Statement select(this->select);
			MainLoop &mainloop = MainLoop::getInstance();

			while(select.step() == SQLITE_ROW && mainloop && Protocol::verify(this)) {

				int64_t id;
				Udjat::URL url;
				string action, payload;

				select.get(0,id);
				select.get(1,url);
				select.get(2,action);
				select.get(3,payload);

				info() << "Sending " << action << " " << url << " (" << id << ")" << endl;
				Logger::write(Logger::Trace,Protocol::c_str(),payload.c_str());

				HTTP::Client client(url);

				switch(HTTP::MethodFactory(action.c_str())) {
				case HTTP::Get:
					{
						auto response = client.get();
						info() << url << endl;
						Logger::write(Logger::Trace,response);
					}
					break;

				case HTTP::Post:
					{
						auto response = client.post(payload.c_str());
						Logger::write(Logger::Trace,response);
					}
					break;

				default:
					error() << "Unexpected verb '" << action << "' sending queued request, ignoring" << endl;
				}

				info() << "Removing request '" << id << "' from URL queue" << endl;
				del.bind(1,id).exec();

				del.reset();
				select.reset();

#ifdef DEBUG
				trace("Waiting for ",send_delay," seconds");
#endif // DEBUG

#ifdef _WIN32
				Sleep(send_delay * 100);
#else
				sleep(send_delay);
#endif // _WIN32

			}

		} catch(const std::exception &e) {

			warning() << "Error sending queued message: " << e.what() << endl;

		} catch(...) {

			warning() << "Unexpected error sending queued messages" << endl;

		}

		{
			lock_guard<mutex> lock(guard);
			busy = false;
		}
#ifdef DEBUG
		trace(__FUNCTION__," finishes");
#endif // DEBUG

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

				if(MainLoop::getInstance()) {
					Protocol *protocol = const_cast<Protocol *>(this->protocol);
					ThreadPool::getInstance().push("sqlite-worker",[protocol]() {
						if(Udjat::Protocol::verify(protocol)) {
							protocol->send();
						}
					});
				}

				// Force as complete.
				progress(1,1);
				return "";
			}

		};

		return make_shared<Worker>(this,ins);
	}


 }

