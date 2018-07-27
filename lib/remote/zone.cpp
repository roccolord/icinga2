/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2018 Icinga Development Team (https://www.icinga.com/)  *
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

#include "remote/zone.hpp"
#include "remote/zone-ti.cpp"
#include "remote/jsonrpcconnection.hpp"
#include "base/perfdatavalue.hpp"
#include "base/objectlock.hpp"
#include "base/logger.hpp"
#include "base/statsfunction.hpp"
#include <limits>

using namespace icinga;

REGISTER_TYPE(Zone);

REGISTER_STATSFUNCTION(Zone, &Zone::StatsFunc);

void Zone::OnAllConfigLoaded()
{
	ObjectImpl<Zone>::OnAllConfigLoaded();

	m_Parent = Zone::GetByName(GetParentRaw());

	if (m_Parent && m_Parent->IsGlobal())
		BOOST_THROW_EXCEPTION(ScriptError("Zone '" + GetName() + "' can not have a global zone as parent.", GetDebugInfo()));

	Zone::Ptr zone = m_Parent;
	int levels = 0;

	Array::Ptr endpoints = GetEndpointsRaw();

	if (endpoints) {
		ObjectLock olock(endpoints);
		for (const String& endpoint : endpoints) {
			Endpoint::Ptr ep = Endpoint::GetByName(endpoint);

			if (ep)
				ep->SetCachedZone(this);
		}
	}

	while (zone) {
		m_AllParents.push_back(zone);

		zone = Zone::GetByName(zone->GetParentRaw());
		levels++;

		if (levels > 32)
			BOOST_THROW_EXCEPTION(ScriptError("Infinite recursion detected while resolving zone graph. Check your zone hierarchy.", GetDebugInfo()));
	}
}

Zone::Ptr Zone::GetParent() const
{
	return m_Parent;
}

std::set<Endpoint::Ptr> Zone::GetEndpoints() const
{
	std::set<Endpoint::Ptr> result;

	Array::Ptr endpoints = GetEndpointsRaw();

	if (endpoints) {
		ObjectLock olock(endpoints);

		for (const String& name : endpoints) {
			Endpoint::Ptr endpoint = Endpoint::GetByName(name);

			if (!endpoint)
				continue;

			result.insert(endpoint);
		}
	}

	return result;
}

std::vector<Zone::Ptr> Zone::GetAllParents() const
{
	return m_AllParents;
}

bool Zone::CanAccessObject(const ConfigObject::Ptr& object)
{
	Zone::Ptr object_zone;

	if (object->GetReflectionType() == Zone::TypeInstance)
		object_zone = static_pointer_cast<Zone>(object);
	else
		object_zone = static_pointer_cast<Zone>(object->GetZone());

	if (!object_zone)
		object_zone = Zone::GetLocalZone();

	if (object_zone->GetGlobal())
		return true;

	return object_zone->IsChildOf(this);
}

bool Zone::IsChildOf(const Zone::Ptr& zone)
{
	Zone::Ptr azone = this;

	while (azone) {
		if (azone == zone)
			return true;

		azone = azone->GetParent();
	}

	return false;
}

bool Zone::IsGlobal() const
{
	return GetGlobal();
}

bool Zone::IsSingleInstance() const
{
	Array::Ptr endpoints = GetEndpointsRaw();
	return !endpoints || endpoints->GetLength() < 2;
}

Zone::Ptr Zone::GetLocalZone()
{
	Endpoint::Ptr local = Endpoint::GetLocalEndpoint();

	if (!local)
		return nullptr;

	return local->GetZone();
}

static std::set<String> l_StatsFuncAggregateSum ({
	"messages_sent_per_second", "messages_received_per_second", "bytes_sent_per_second", "bytes_received_per_second"
});

static std::set<String> l_StatsFuncAggregateCount ({
	"connecting", "syncing", "connected"
});

static std::set<String> l_StatsFuncAggregateMin ({
	"last_message_sent", "last_message_received"
});

void Zone::StatsFunc(const Dictionary::Ptr& status, const Array::Ptr& perfdata)
{
	Dictionary::Ptr ourStatus = new Dictionary;
	auto localEndpoint (Endpoint::GetLocalEndpoint());

	for (auto& zone : ConfigType::GetObjectsByType<Zone>()) {
		Dictionary::Ptr endpointStats = new Dictionary({
			{"local_log_position", (Array::Ptr)new Array},
			{"remote_log_position", (Array::Ptr)new Array},
			{"connecting", (Array::Ptr)new Array},
			{"syncing", (Array::Ptr)new Array},
			{"connected", (Array::Ptr)new Array},
			{"last_message_sent", (Array::Ptr)new Array},
			{"last_message_received", (Array::Ptr)new Array},
			{"messages_sent_per_second", (Array::Ptr)new Array},
			{"messages_received_per_second", (Array::Ptr)new Array},
			{"bytes_sent_per_second", (Array::Ptr)new Array},
			{"bytes_received_per_second", (Array::Ptr)new Array}
		});

		auto endpoints (zone->GetEndpoints());

		endpoints.erase(localEndpoint);

		for (auto& endpoint : endpoints) {
			((Array::Ptr)endpointStats->Get("local_log_position"))->Add(endpoint->GetLocalLogPosition());
			((Array::Ptr)endpointStats->Get("remote_log_position"))->Add(endpoint->GetRemoteLogPosition());
			((Array::Ptr)endpointStats->Get("connecting"))->Add(endpoint->GetConnecting());
			((Array::Ptr)endpointStats->Get("syncing"))->Add(endpoint->GetSyncing());
			((Array::Ptr)endpointStats->Get("connected"))->Add(endpoint->GetConnected());
			((Array::Ptr)endpointStats->Get("last_message_sent"))->Add(endpoint->GetLastMessageSent());
			((Array::Ptr)endpointStats->Get("last_message_received"))->Add(endpoint->GetLastMessageReceived());
			((Array::Ptr)endpointStats->Get("messages_sent_per_second"))->Add(endpoint->GetMessagesSentPerSecond());
			((Array::Ptr)endpointStats->Get("messages_received_per_second"))->Add(endpoint->GetMessagesReceivedPerSecond());
			((Array::Ptr)endpointStats->Get("bytes_sent_per_second"))->Add(endpoint->GetBytesSentPerSecond());
			((Array::Ptr)endpointStats->Get("bytes_received_per_second"))->Add(endpoint->GetBytesReceivedPerSecond());
		}

		for (auto& label : l_StatsFuncAggregateSum) {
			auto sum (0.0);
			Array::Ptr values = endpointStats->Get(label);
			ObjectLock valuesLock (values);

			for (auto& value : values) {
				sum += value.Get<double>();
			}

			endpointStats->Set(label, sum);
		}

		for (auto& label : l_StatsFuncAggregateCount) {
			uintmax_t count = 0;
			Array::Ptr values = endpointStats->Get(label);
			ObjectLock valuesLock (values);

			for (auto& value : values) {
				if (value.Get<bool>()) {
					++count;
				}
			}

			endpointStats->Set(label, count);
		}

		for (auto& label : l_StatsFuncAggregateMin) {
			auto min (std::numeric_limits<double>::infinity());
			auto hasAny (false);
			Array::Ptr values = endpointStats->Get(label);
			ObjectLock valuesLock (values);

			for (auto& value : values) {
				auto number (value.Get<double>());

				if (number < min) {
					min = number;
				}

				hasAny = true;
			}

			endpointStats->Set(label, hasAny ? min : 0.0);
		}

		{
			auto maxDiff (-std::numeric_limits<double>::infinity());
			Array::Ptr remoteLogPositions = endpointStats->Get("remote_log_position");
			ObjectLock remoteLogPositionLock (remoteLogPositions);
			auto remoteLogPosition (begin(remoteLogPositions));
			auto hasAny (false);
			Array::Ptr localLogPositions = endpointStats->Get("local_log_position");
			ObjectLock localLogPositionLock (localLogPositions);

			for (auto& localLogPosition : localLogPositions) {
				auto diff (localLogPosition - *remoteLogPosition);

				if (diff > maxDiff) {
					maxDiff = diff;
				}

				hasAny = true;

				++remoteLogPosition;
			}

			endpointStats->Set("client_log_lag", hasAny ? maxDiff : 0.0);
			endpointStats->Remove("local_log_position");
			endpointStats->Remove("remote_log_position");
		}

		ourStatus->Set(zone->GetName(), endpointStats);
	}

	{
		ObjectLock ourStatusLock (ourStatus);

		for (auto& nameZoneStatus : ourStatus) {
			Dictionary::Ptr zoneStatus = nameZoneStatus.second;
			ObjectLock zoneStatusLock (zoneStatus);
			auto labelPrefix ("zone_" + nameZoneStatus.first + "_");

			for (auto& labelValue : zoneStatus) {
				perfdata->Add(new PerfdataValue(labelPrefix + labelValue.first, labelValue.second));
			}
		}
	}

	status->Set("zone", ourStatus);
}

void Zone::ValidateEndpointsRaw(const Lazy<Array::Ptr>& lvalue, const ValidationUtils& utils)
{
	ObjectImpl<Zone>::ValidateEndpointsRaw(lvalue, utils);

	if (lvalue() && lvalue()->GetLength() > 2) {
		Log(LogWarning, "Zone")
			<< "The Zone object '" << GetName() << "' has more than two endpoints."
			<< " Due to a known issue this type of configuration is strongly"
			<< " discouraged and may cause Icinga to use excessive amounts of CPU time.";
	}
}
