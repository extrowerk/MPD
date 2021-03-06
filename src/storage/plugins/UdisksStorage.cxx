/*
 * Copyright 2003-2018 The Music Player Daemon Project
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

#include "UdisksStorage.hxx"
#include "LocalStorage.hxx"
#include "storage/StoragePlugin.hxx"
#include "storage/StorageInterface.hxx"
#include "storage/FileInfo.hxx"
#include "lib/dbus/Glue.hxx"
#include "lib/dbus/AsyncRequest.hxx"
#include "lib/dbus/Message.hxx"
#include "lib/dbus/PendingCall.hxx"
#include "lib/dbus/AppendIter.hxx"
#include "lib/dbus/ReadIter.hxx"
#include "lib/dbus/ObjectManager.hxx"
#include "lib/dbus/UDisks2.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "thread/SafeSingleton.hxx"
#include "event/Call.hxx"
#include "event/DeferEvent.hxx"
#include "fs/AllocatedPath.hxx"
#include "util/StringCompare.hxx"
#include "util/RuntimeError.hxx"
#include "Log.hxx"

#include <stdexcept>

class UdisksStorage final : public Storage {
	const std::string base_uri;
	const std::string id;

	std::string dbus_path;

	SafeSingleton<ODBus::Glue> dbus_glue;
	ODBus::AsyncRequest list_request;
	ODBus::AsyncRequest mount_request;

	mutable Mutex mutex;
	Cond cond;

	bool want_mount = false;

	std::unique_ptr<Storage> mounted_storage;

	std::exception_ptr mount_error;

	DeferEvent defer_mount, defer_unmount;

public:
	template<typename B, typename I>
	UdisksStorage(EventLoop &_event_loop, B &&_base_uri, I &&_id)
		:base_uri(std::forward<B>(_base_uri)),
		 id(std::forward<I>(_id)),
		 dbus_glue(_event_loop),
		 defer_mount(_event_loop, BIND_THIS_METHOD(DeferredMount)),
		 defer_unmount(_event_loop, BIND_THIS_METHOD(DeferredUnmount)) {}

	~UdisksStorage() noexcept override {
		if (list_request || mount_request)
			BlockingCall(GetEventLoop(), [this](){
					if (list_request)
						list_request.Cancel();
					if (mount_request)
						mount_request.Cancel();
				});

		try {
			UnmountWait();
		} catch (...) {
			FormatError(std::current_exception(),
				    "Failed to unmount '%s'",
				    base_uri.c_str());
		}
	}

	EventLoop &GetEventLoop() noexcept {
		return defer_mount.GetEventLoop();
	}

	/* virtual methods from class Storage */
	StorageFileInfo GetInfo(const char *uri_utf8, bool follow) override {
		MountWait();
		return mounted_storage->GetInfo(uri_utf8, follow);
	}

	std::unique_ptr<StorageDirectoryReader> OpenDirectory(const char *uri_utf8) override {
		MountWait();
		return mounted_storage->OpenDirectory(uri_utf8);
	}

	std::string MapUTF8(const char *uri_utf8) const noexcept override;

	AllocatedPath MapFS(const char *uri_utf8) const noexcept override {
		try {
			const_cast<UdisksStorage *>(this)->MountWait();
		} catch (...) {
			return nullptr;
		}

		return mounted_storage->MapFS(uri_utf8);
	}

	const char *MapToRelativeUTF8(const char *uri_utf8) const noexcept override;

private:
	void OnListReply(ODBus::Message reply) noexcept;

	void MountWait();
	void DeferredMount() noexcept;
	void OnMountNotify(ODBus::Message reply) noexcept;

	void UnmountWait();
	void DeferredUnmount() noexcept;
	void OnUnmountNotify(ODBus::Message reply) noexcept;
};

void
UdisksStorage::OnListReply(ODBus::Message reply) noexcept
{
	using namespace UDisks2;

	try {
		ParseObjects(reply, [this](Object &&o) {
				if (o.IsId(id))
					dbus_path = std::move(o.path);
			});

		if (dbus_path.empty())
			throw FormatRuntimeError("No such UDisks2 object: %s",
						 id.c_str());
	} catch (...) {
		const std::lock_guard<Mutex> lock(mutex);
		mount_error = std::current_exception();
		want_mount = false;
		cond.broadcast();
		return;
	}

	DeferredMount();
}

void
UdisksStorage::MountWait()
{
	const std::lock_guard<Mutex> lock(mutex);

	if (mounted_storage)
		/* already mounted */
		return;

	if (!want_mount) {
		want_mount = true;
		defer_mount.Schedule();
	}

	while (want_mount)
		cond.wait(mutex);

	if (mount_error)
		std::rethrow_exception(mount_error);
}

void
UdisksStorage::DeferredMount() noexcept
try {
	using namespace ODBus;

	auto &connection = dbus_glue->GetConnection();

	if (dbus_path.empty()) {
		auto msg = Message::NewMethodCall(UDISKS2_INTERFACE,
						  UDISKS2_PATH,
						  DBUS_OM_INTERFACE,
						  "GetManagedObjects");
		list_request.Send(connection, *msg.Get(),
				  std::bind(&UdisksStorage::OnListReply,
					    this, std::placeholders::_1));
		return;
	}

	auto msg = Message::NewMethodCall(UDISKS2_INTERFACE,
					  dbus_path.c_str(),
					  UDISKS2_FILESYSTEM_INTERFACE,
					  "Mount");
	AppendMessageIter(*msg.Get()).AppendEmptyArray<DictEntryTypeTraits<StringTypeTraits, VariantTypeTraits>>();

	mount_request.Send(connection, *msg.Get(),
			   std::bind(&UdisksStorage::OnMountNotify,
				     this, std::placeholders::_1));
} catch (...) {
	const std::lock_guard<Mutex> lock(mutex);
	mount_error = std::current_exception();
	want_mount = false;
	cond.broadcast();
}

void
UdisksStorage::OnMountNotify(ODBus::Message reply) noexcept
try {
	using namespace ODBus;
	reply.CheckThrowError();

	ReadMessageIter i(*reply.Get());
	if (i.GetArgType() != DBUS_TYPE_STRING)
		throw std::runtime_error("Malformed 'Mount' response");

	const char *mount_path = i.GetString();

	const std::lock_guard<Mutex> lock(mutex);
	mounted_storage = CreateLocalStorage(Path::FromFS(mount_path));
	mount_error = {};
	want_mount = false;
	cond.broadcast();
} catch (...) {
	const std::lock_guard<Mutex> lock(mutex);
	mount_error = std::current_exception();
	want_mount = false;
	cond.broadcast();
}

void
UdisksStorage::UnmountWait()
{
	const std::lock_guard<Mutex> lock(mutex);

	if (!mounted_storage)
		/* not mounted */
		return;

	defer_unmount.Schedule();

	while (mounted_storage)
		cond.wait(mutex);

	if (mount_error)
		std::rethrow_exception(mount_error);
}

void
UdisksStorage::DeferredUnmount() noexcept
try {
	using namespace ODBus;

	auto &connection = dbus_glue->GetConnection();
	auto msg = Message::NewMethodCall(UDISKS2_INTERFACE,
					  dbus_path.c_str(),
					  UDISKS2_FILESYSTEM_INTERFACE,
					  "Unmount");
	AppendMessageIter(*msg.Get()).AppendEmptyArray<DictEntryTypeTraits<StringTypeTraits, VariantTypeTraits>>();

	mount_request.Send(connection, *msg.Get(),
			   std::bind(&UdisksStorage::OnUnmountNotify,
				     this, std::placeholders::_1));
} catch (...) {
	const std::lock_guard<Mutex> lock(mutex);
	mount_error = std::current_exception();
	mounted_storage.reset();
	cond.broadcast();
}

void
UdisksStorage::OnUnmountNotify(ODBus::Message reply) noexcept
try {
	using namespace ODBus;
	reply.CheckThrowError();

	const std::lock_guard<Mutex> lock(mutex);
	mount_error = {};
	mounted_storage.reset();
	cond.broadcast();
} catch (...) {
	const std::lock_guard<Mutex> lock(mutex);
	mount_error = std::current_exception();
	mounted_storage.reset();
	cond.broadcast();
}

std::string
UdisksStorage::MapUTF8(const char *uri_utf8) const noexcept
{
	assert(uri_utf8 != nullptr);

	try {
		const_cast<UdisksStorage *>(this)->MountWait();

		return mounted_storage->MapUTF8(uri_utf8);
	} catch (...) {
		/* fallback - not usable but the best we can do */

		if (StringIsEmpty(uri_utf8))
			return base_uri;

		return PathTraitsUTF8::Build(base_uri.c_str(), uri_utf8);
	}
}

const char *
UdisksStorage::MapToRelativeUTF8(const char *uri_utf8) const noexcept
{
	return PathTraitsUTF8::Relative(base_uri.c_str(), uri_utf8);
}

static std::unique_ptr<Storage>
CreateUdisksStorageURI(EventLoop &event_loop, const char *base_uri)
{
	const char *id_begin = StringAfterPrefix(base_uri, "udisks://");
	if (id_begin == nullptr)
		return nullptr;

	std::string id;

	const char *relative_path = strchr(id_begin, '/');
	if (relative_path == nullptr) {
		id = id_begin;
		relative_path = "";
	} else {
		id = {id_begin, relative_path};
		++relative_path;
	}

	// TODO: use relative_path

	return std::make_unique<UdisksStorage>(event_loop, base_uri,
					       std::move(id));
}

const StoragePlugin udisks_storage_plugin = {
	"udisks",
	CreateUdisksStorageURI,
};
