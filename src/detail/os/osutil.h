#pragma once

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
}
