/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "i2-base.h"

using namespace icinga;

ScriptInterpreter::ScriptInterpreter(const Script::Ptr& script)
{ }

ScriptInterpreter::~ScriptInterpreter(void)
{
	Stop();
}

void ScriptInterpreter::Start(void)
{
	/* We can't start the thread in the constructor because
	 * the worker thread might end up calling one of the virtual
	 * methods before the object is fully constructed. */

	m_Thread = boost::thread(&ScriptInterpreter::ThreadWorkerProc, this);
}

void ScriptInterpreter::Stop(void)
{
	assert(Application::IsMainThread());

	{
		boost::mutex::scoped_lock lock(m_Mutex);

		if (m_Shutdown)
			return;

		m_Shutdown = true;
		m_CallAvailable.notify_all();
	}

	BOOST_FOREACH(const String& function, m_SubscribedFunctions) {
		ScriptFunction::Unregister(function);
	}

	m_Thread.join();
}

void ScriptInterpreter::ThreadWorkerProc(void)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	for (;;) {
		while (m_Calls.empty() && !m_Shutdown)
			m_CallAvailable.wait(lock);

		if (m_Shutdown)
			break;

		ScriptCall call = m_Calls.front();
		m_Calls.pop_front();

		ProcessCall(call.Task, call.Function, call.Arguments);
	}
}

void ScriptInterpreter::ScriptFunctionThunk(const ScriptTask::Ptr& task,
    const String& function, const vector<Value>& arguments)
{
	ScriptCall call;
	call.Task = task;
	call.Function = function;
	call.Arguments = arguments;

	{
		boost::mutex::scoped_lock lock(m_Mutex);
		m_Calls.push_back(call);
		m_CallAvailable.notify_all();
	}
}

void ScriptInterpreter::SubscribeFunction(const String& name)
{
	assert(Application::IsMainThread());

	m_SubscribedFunctions.insert(name);

	ScriptFunction::Ptr sf = boost::make_shared<ScriptFunction>(boost::bind(&ScriptInterpreter::ScriptFunctionThunk, this, _1, name, _2));
	ScriptFunction::Register(name, sf);
}

void ScriptInterpreter::UnsubscribeFunction(const String& name)
{
	assert(Application::IsMainThread());

	m_SubscribedFunctions.erase(name);
	ScriptFunction::Unregister(name);
}
