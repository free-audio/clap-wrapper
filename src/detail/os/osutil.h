#pragma once

/*
		a minimalistic OS layer

*/

#include<atomic>
#include <string>

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
}

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