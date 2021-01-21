/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "Discovery.hxx"
#include "ContentDirectoryService.hxx"
#include "Log.hxx"
#include "lib/curl/Global.hxx"
#include "event/Call.hxx"
#include "util/DeleteDisposer.hxx"
#include "util/ScopeExit.hxx"
#include "util/RuntimeError.hxx"

#include <upnptools.h>

#include <stdlib.h>
#include <string.h>

UPnPDeviceDirectory::Downloader::Downloader(UPnPDeviceDirectory &_parent,
					    const UpnpDiscovery &disco)
	:defer_start_event(_parent.GetEventLoop(),
			   BIND_THIS_METHOD(OnDeferredStart)),
	 parent(_parent),
	 id(UpnpDiscovery_get_DeviceID_cstr(&disco)),
	 url(UpnpDiscovery_get_Location_cstr(&disco)),
	 expires(std::chrono::seconds(UpnpDiscovery_get_Expires(&disco))),
	 request(*parent.curl, url.c_str(), *this)
{
	const std::lock_guard<Mutex> protect(parent.mutex);
	parent.downloaders.push_back(*this);
}

void
UPnPDeviceDirectory::Downloader::Destroy() noexcept
{
	const std::lock_guard<Mutex> protect(parent.mutex);
	parent.downloaders.erase_and_dispose(parent.downloaders.iterator_to(*this),
					     DeleteDisposer());
}

void
UPnPDeviceDirectory::Downloader::OnHeaders(unsigned status,
					   std::multimap<std::string, std::string> &&)
{
	if (status != 200) {
		Destroy();
		return;
	}
}

void
UPnPDeviceDirectory::Downloader::OnData(ConstBuffer<void> _data)
{
	data.append(static_cast<const char *>(_data.data), _data.size);
}

void
UPnPDeviceDirectory::Downloader::OnEnd()
{
	AtScopeExit(this) { Destroy(); };

	ContentDirectoryDescriptor d(std::move(id),
				     std::chrono::steady_clock::now(),
				     expires);

	try {
		d.Parse(url, data.c_str());
	} catch (...) {
		LogError(std::current_exception());
	}

	parent.LockAdd(std::move(d));
}

void
UPnPDeviceDirectory::Downloader::OnError(std::exception_ptr e) noexcept
{
	LogError(e);
	Destroy();
}

// The service type string we are looking for.
static constexpr char ContentDirectorySType[] = "urn:schemas-upnp-org:service:ContentDirectory:1";

// We don't include a version in comparisons, as we are satisfied with
// version 1
gcc_pure
static bool
isCDService(const char *st) noexcept
{
	constexpr size_t sz = sizeof(ContentDirectorySType) - 3;
	return strncmp(ContentDirectorySType, st, sz) == 0;
}

// The type of device we're asking for in search
static constexpr char MediaServerDType[] = "urn:schemas-upnp-org:device:MediaServer:1";

gcc_pure
static bool
isMSDevice(const char *st) noexcept
{
	constexpr size_t sz = sizeof(MediaServerDType) - 3;
	return strncmp(MediaServerDType, st, sz) == 0;
}

static void
AnnounceFoundUPnP(UPnPDiscoveryListener &listener, const UPnPDevice &device)
{
	for (const auto &service : device.services)
		if (isCDService(service.serviceType.c_str()))
			listener.FoundUPnP(ContentDirectoryService(device,
								   service));
}

static void
AnnounceLostUPnP(UPnPDiscoveryListener &listener, const UPnPDevice &device)
{
	for (const auto &service : device.services)
		if (isCDService(service.serviceType.c_str()))
			listener.LostUPnP(ContentDirectoryService(device,
								  service));
}

inline void
UPnPDeviceDirectory::LockAdd(ContentDirectoryDescriptor &&d)
{
	const std::lock_guard<Mutex> protect(mutex);

	for (auto &i : directories) {
		if (i.id == d.id) {
			i = std::move(d);
			return;
		}
	}

	directories.emplace_back(std::move(d));

	if (listener != nullptr)
		AnnounceFoundUPnP(*listener, directories.back().device);
}

inline void
UPnPDeviceDirectory::LockRemove(const std::string &id)
{
	const std::lock_guard<Mutex> protect(mutex);

	for (auto i = directories.begin(), end = directories.end();
	     i != end; ++i) {
		if (i->id == id) {
			if (listener != nullptr)
				AnnounceLostUPnP(*listener, i->device);

			directories.erase(i);
			break;
		}
	}
}

inline int
UPnPDeviceDirectory::OnAlive(const UpnpDiscovery *disco) noexcept
{
	if (isMSDevice(UpnpDiscovery_get_DeviceType_cstr(disco)) ||
	    isCDService(UpnpDiscovery_get_ServiceType_cstr(disco))) {
		try {
			auto *downloader = new Downloader(*this, *disco);

			try {
				downloader->Start();
			} catch (...) {
				BlockingCall(GetEventLoop(), [downloader](){
						downloader->Destroy();
					});

				throw;
			}
		} catch (...) {
			LogError(std::current_exception());
			return UPNP_E_SUCCESS;
		}
	}

	return UPNP_E_SUCCESS;
}

inline int
UPnPDeviceDirectory::OnByeBye(const UpnpDiscovery *disco) noexcept
{
	if (isMSDevice(UpnpDiscovery_get_DeviceType_cstr(disco)) ||
	    isCDService(UpnpDiscovery_get_ServiceType_cstr(disco))) {
		// Device signals it is going off.
		LockRemove(UpnpDiscovery_get_DeviceID_cstr(disco));
	}

	return UPNP_E_SUCCESS;
}

// This gets called for all libupnp asynchronous events, in a libupnp
// thread context.
// Example: ContentDirectories appearing and disappearing from the network
// We queue a task for our worker thread(s)
int
UPnPDeviceDirectory::Invoke(Upnp_EventType et, const void *evp) noexcept
{
	switch (et) {
	case UPNP_DISCOVERY_SEARCH_RESULT:
	case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		{
			auto *disco = static_cast<const UpnpDiscovery *>(evp);
			return OnAlive(disco);
		}

	case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
		{
			auto *disco = static_cast<const UpnpDiscovery *>(evp);
			return OnByeBye(disco);
		}

	default:
		// Ignore other events for now
		break;
	}

	return UPNP_E_SUCCESS;
}

void
UPnPDeviceDirectory::ExpireDevices()
{
	const auto now = std::chrono::steady_clock::now();
	bool didsomething = false;

	directories.remove_if([now, &didsomething](const ContentDirectoryDescriptor &d){
			bool expired = now > d.expires;
			if (expired)
				didsomething = true;
			return expired;
		});

	if (didsomething)
		Search();
}

UPnPDeviceDirectory::UPnPDeviceDirectory(EventLoop &event_loop,
					 UpnpClient_Handle _handle,
					 UPnPDiscoveryListener *_listener)
	:curl(event_loop), handle(_handle),
	 listener(_listener)
{
}

UPnPDeviceDirectory::~UPnPDeviceDirectory() noexcept
{
	BlockingCall(GetEventLoop(), [this](){
			const std::lock_guard<Mutex> protect(mutex);
			downloaders.clear_and_dispose(DeleteDisposer());
		});
}

inline EventLoop &
UPnPDeviceDirectory::GetEventLoop() const noexcept
{
	return curl->GetEventLoop();
}

void
UPnPDeviceDirectory::Start()
{
	Search();
}

void
UPnPDeviceDirectory::Search()
{
	const auto now = std::chrono::steady_clock::now();
	if (now - last_search < std::chrono::seconds(10))
		return;
	last_search = now;

	// We search both for device and service just in case.
	int code = UpnpSearchAsync(handle, search_timeout,
				   ContentDirectorySType, GetUpnpCookie());
	if (code != UPNP_E_SUCCESS)
		throw FormatRuntimeError("UpnpSearchAsync() failed: %s",
					 UpnpGetErrorMessage(code));

	code = UpnpSearchAsync(handle, search_timeout,
			       MediaServerDType, GetUpnpCookie());
	if (code != UPNP_E_SUCCESS)
		throw FormatRuntimeError("UpnpSearchAsync() failed: %s",
					 UpnpGetErrorMessage(code));
}

std::vector<ContentDirectoryService>
UPnPDeviceDirectory::GetDirectories()
{
	const std::lock_guard<Mutex> protect(mutex);

	ExpireDevices();

	std::vector<ContentDirectoryService> out;
	for (const auto &descriptor : directories) {
		for (const auto &service : descriptor.device.services) {
			if (isCDService(service.serviceType.c_str())) {
				out.emplace_back(descriptor.device, service);
			}
		}
	}

	return out;
}

ContentDirectoryService
UPnPDeviceDirectory::GetServer(std::string_view friendly_name)
{
	const std::lock_guard<Mutex> protect(mutex);

	ExpireDevices();

	for (const auto &i : directories) {
		const auto &device = i.device;

		if (device.friendlyName != friendly_name)
			continue;

		for (const auto &service : device.services)
			if (isCDService(service.serviceType.c_str()))
				return ContentDirectoryService(device,
							       service);
	}

	throw std::runtime_error("Server not found");
}
