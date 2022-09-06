#pragma once

#include <clap/clap.h>

namespace Clap
{
	class IAutomation
	{
	public:
		virtual void beginEdit(clap_id id) = 0;
		virtual void performEdit(const clap_event_param_value_t* value) = 0;
		virtual void endEdit(clap_id id) = 0;
		virtual ~IAutomation() {}
	};
}