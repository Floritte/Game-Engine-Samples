#pragma once

#include <taskflow/taskflow/include/taskflow.hpp>

#ifdef FROSTIUM_SMOLENGINE_IMPL
namespace SmolEngine
#else
namespace Frostium
#endif
{
	class JobsSystemInstance
	{
	public:
		JobsSystemInstance();

		static void             BeginSubmition();
		static void             EndSubmition(bool wait = true);

		static uint32_t         GetNumWorkers();
		static uint32_t         GetNumRenderingTasks();
		static tf::Executor* GetExecutor();

		template<typename... F>
		static void             Schedule(F&&... f) { s_Instance->m_RenderingQueue.emplace(std::forward<F>(f)...); }

	private:

		static JobsSystemInstance* s_Instance;
		tf::Taskflow                    m_RenderingQueue{};
		tf::Executor                    m_Executor{};
	};
}