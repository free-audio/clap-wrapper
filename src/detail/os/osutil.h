#pragma once

/*
		a minimalistic OS layer

*/

#include <atomic>
#include <string>
#define FMT_HEADER_ONLY 1
#include "fmt/format.h"
#include "fmt/ranges.h"


namespace os
{
	class IPlugObject
	{
	public:
		virtual void onIdle() = 0;
		virtual ~IPlugObject() {}
	};
	void attach(IPlugObject* plugobject);
	void detach(IPlugObject* plugobject);
	uint64_t getTickInMS();
	std::string getParentFolderName();
	std::string getBinaryName();

	void log(const char* text);

	template <typename... Args>
	void log(const char* format_str, Args&&... args) {
		fmt::memory_buffer buf;
		fmt::format_to(std::back_inserter(buf), format_str, args...);
		buf.push_back(0);
		log((const char*)buf.data());
	};
}

#ifndef CLAP_WRAPPER_LOGLEVEL
#define CLAP_WRAPPER_LOGLEVEL 2
#endif

#if (CLAP_WRAPPER_LOGLEVEL == 0)
#define LOGINFO(...) (void(0))
#define LOGDETAIL(...) (void(0))
#endif

#if (CLAP_WRAPPER_LOGLEVEL == 1)
#define LOGINFO os::log
#define LOGDETAIL(...) (void(0))
#endif

#if (CLAP_WRAPPER_LOGLEVEL == 2)
#define LOGINFO os::log
#define LOGDETAIL os::log
#endif

namespace util
{

	template<typename T, uint32_t Q>
	class fixedqueue
	{
	public:
		inline void push(const T& val)
		{
			push(&val);
		}
		inline void push(const T* val)
		{			
			_elements[_head] = *val;
			_head = (_head + 1) % Q;
		}
		inline bool pop(T& out)
		{
			if (_head == _tail)
			{
				return false;
			}
			out = _elements[_tail];
			_tail = (_tail + 1) % Q;
			return true;
		}
	private:
		T _elements[Q] = { };
		std::atomic_uint32_t _head = 0u;
		std::atomic_uint32_t _tail = 0u;
	};
};