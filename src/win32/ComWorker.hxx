/*
 * Copyright 2020 The Music Player Daemon Project
 * http://www.musicpd.org
#include "thread/Thread.hxx"
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

#ifndef MPD_WIN32_COM_WORKER_HXX
#define MPD_WIN32_COM_WORKER_HXX

#include <boost/lockfree/spsc_queue.hpp>
#include <condition_variable>
#include <mutex>
#include <optional>

#include "thread/Future.hxx"
#include "thread/Thread.hxx"
#include "win32/WinEvent.hxx"
#include <objbase.h>
#include <windows.h>

// Worker thread for all COM operation
class COMWorker {
private:
	class COMWorkerThread : public Thread {
	public:
		COMWorkerThread() : Thread{BIND_THIS_METHOD(Work)} {}

	private:
		friend class COMWorker;
		void Work() noexcept;
		void Finish() noexcept {
			running_flag.clear();
			event.Set();
		}
		void Push(const std::function<void()> &function) {
			spsc_buffer.push(function);
			event.Set();
		}

		boost::lockfree::spsc_queue<std::function<void()>> spsc_buffer{32};
		std::atomic_flag running_flag = true;
		WinEvent event{};
	};

public:
	static void Aquire() {
		std::unique_lock locker(mutex);
		if (reference_count == 0) {
			thread.emplace();
			thread->Start();
		}
		++reference_count;
	}
	static void Release() noexcept {
		std::unique_lock locker(mutex);
		--reference_count;
		if (reference_count == 0) {
			thread->Finish();
			thread->Join();
			thread.reset();
		}
	}

	template <typename Function, typename... Args>
	static auto Async(Function &&function, Args &&...args) {
		using R = std::invoke_result_t<std::decay_t<Function>,
					       std::decay_t<Args>...>;
		auto promise = std::make_shared<Promise<R>>();
		auto future = promise->get_future();
		thread->Push([function = std::forward<Function>(function),
			      args = std::make_tuple(std::forward<Args>(args)...),
			      promise = std::move(promise)]() mutable {
			try {
				if constexpr (std::is_void_v<R>) {
					std::apply(std::forward<Function>(function),
						   std::move(args));
					promise->set_value();
				} else {
					promise->set_value(std::apply(
						std::forward<Function>(function),
						std::move(args)));
				}
			} catch (...) {
				promise->set_exception(std::current_exception());
			}
		});
		return future;
	}

private:
	static std::mutex mutex;
	static unsigned int reference_count;
	static std::optional<COMWorkerThread> thread;
};

#endif
