#pragma once
#include "injectory/common.hpp"
#include "injectory/exception.hpp"
#include "injectory/process.hpp"

class ModuleKernel32;
class ModuleNtdll;

class Module : public Handle<HINSTANCE__>
{
	friend Module Process::isInjected(HMODULE);
	friend Module Process::isInjected(const Library&);
	friend Module Process::map(const File& file);
private:
	Process process;

private:
	Module(HMODULE handle, const Process& process)
		: Handle<HINSTANCE__>(handle)
		, process(process)
	{}

	template <class Deleter>
	Module(HMODULE handle, const Process& process, Deleter deleter)
		: Handle<HINSTANCE__>(handle, deleter)
		, process(process)
	{}

public:
	Module()
		: Module(nullptr, Process())
	{}

	Module(const fs::path& moduleName)
		: Module(GetModuleHandleW(moduleName.c_str()), Process::current)
	{
		if (!handle())
		{
			DWORD errcode = GetLastError();
			BOOST_THROW_EXCEPTION(ex_get_module_handle() << e_api_function("GetModuleHandle") << e_text("could not get handle to module '" + moduleName.string() + "'") << e_last_error(errcode));
		}
	}

	static Module load(const fs::path& moduleName, DWORD flags = 0, bool freeOnDestruction = true, bool throwing = true)
	{
		HMODULE handle_ = LoadLibraryExW(moduleName.c_str(), nullptr, flags);
		if (!handle_)
		{
			if (!throwing)
				return Module();

			DWORD errcode = GetLastError();
			BOOST_THROW_EXCEPTION(ex_get_module_handle() << e_api_function("LoadLibraryEx") << e_text("could not load module '" + moduleName.string() + "' locally") << e_last_error(errcode));
		}
		Module module;
		if (freeOnDestruction)
			module = Module(handle_, Process::current, FreeLibrary);
		else
			module = Module(handle_, Process::current);
		return module;
	}

	fs::path path() const;
	wstring mappedFilename(bool throwOnFail = true) const;
	void eject();

	IMAGE_DOS_HEADER dosHeader();
	IMAGE_NT_HEADERS ntHeader();

public:
	FARPROC getProcAddress(string procName, bool throwing = true) const
	{
		if (process != Process::current)
		{
			// load module locally without running it and calculate offset
			Module localModule = load(path(), DONT_RESOLVE_DLL_REFERENCES, true, throwing);

			if (!throwing && !localModule)
				return nullptr;

			LONG_PTR funcOffset = (DWORD_PTR)localModule.getProcAddress(procName) - (DWORD_PTR)localModule.handle();
			return (FARPROC)((DWORD_PTR)handle() + funcOffset);
		}
		else
		{
			FARPROC procAddress = GetProcAddress(handle(), procName.c_str());
			if (!procAddress)
			{
				if (!throwing)
					return nullptr;

				DWORD errcode = GetLastError();
				BOOST_THROW_EXCEPTION(ex_injection() << e_api_function("GetProcAddress") << e_text("could not get the address of '" + procName + "'") << e_last_error(errcode));
			}
			return procAddress;
		}
	}

private:
	template <typename T>
	struct TypeParser {};

	template <typename Ret, typename... Args>
	struct TypeParser<Ret(Args...)> {
		static function<Ret(Args...)> winapiFunction(const FARPROC lpfnGetProcessID) {
			return function<Ret(Args...)>(reinterpret_cast<Ret(WINAPI *)(Args...)>(lpfnGetProcessID));
		}
	};

public:
	template <typename T>
	function<T> getProcAddress(string procName, bool throwing = true) const
	{
		return TypeParser<T>::winapiFunction(getProcAddress(procName, throwing));
	}

public:
	static const Module& exe();
	static const ModuleKernel32& kernel32();
	static const ModuleNtdll& ntdll();
};



class ModuleKernel32 : public Module
{
public:
	// in 64bit systems, returns true for 32 bit processes.
	const function<BOOL(HANDLE, BOOL*)> isWow64Process_;
	// may be null
	const function<void(SYSTEM_INFO*)> getNativeSystemInfo_;

public:
	ModuleKernel32()
		: Module("kernel32")
		, isWow64Process_(getProcAddress<BOOL(HANDLE, PBOOL)>("IsWow64Process"))
		, getNativeSystemInfo_(getProcAddress<void(SYSTEM_INFO*)>("GetNativeSystemInfo", false))
	{}

	bool isWow64Process(const Process& proc) const
	{
		BOOL isWow64 = false;
		if (!isWow64Process_(proc.handle(), &isWow64))
		{
			DWORD errcode = GetLastError();
			BOOST_THROW_EXCEPTION(ex_injection() << e_api_function("IsWow64Process") << e_process(proc) << e_last_error(errcode));
		}
		return !isWow64;
	}
};



class ModuleNtdll : public Module
{
public:
	enum MY_THREAD_INFORMATION_CLASS
	{
		ThreadBasicInformation,
		ThreadTimes,
		ThreadPriority,
		ThreadBasePriority,
		ThreadAffinityMask,
		ThreadImpersonationToken,
		ThreadDescriptorTableEntry,
		ThreadEnableAlignmentFaultFixup,
		ThreadEventPair,
		ThreadQuerySetWin32StartAddress,
		ThreadZeroTlsCell,
		ThreadPerformanceCount,
		ThreadAmILastThread,
		ThreadIdealProcessor,
		ThreadPriorityBoost,
		ThreadSetTlsArrayAddress,
		ThreadIsIoPending,
		ThreadHideFromDebugger
	};

public:
	const function<NTSTATUS(HANDLE)> ntResumeProcess_;
	const function<NTSTATUS(HANDLE)> ntSuspendProcess_;
	const function<NTSTATUS(HANDLE, MY_THREAD_INFORMATION_CLASS, PVOID, ULONG)> ntSetInformationThread_;

public:
	ModuleNtdll()
		: Module("ntdll")
		, ntResumeProcess_(getProcAddress<NTSTATUS(HANDLE)>("NtResumeProcess"))
		, ntSuspendProcess_(getProcAddress<NTSTATUS(HANDLE)>("NtSuspendProcess"))
		, ntSetInformationThread_(getProcAddress<NTSTATUS(HANDLE, MY_THREAD_INFORMATION_CLASS, PVOID, ULONG)>("NtSetInformationThread"))
	{}


	static bool NT_SUCCESS(NTSTATUS status)
	{
		return status >= 0;
	}


	void ntResumeProcess(const Process& proc) const
	{
		NTSTATUS status = ntResumeProcess_(proc.handle());
		if (!NT_SUCCESS(status))
			BOOST_THROW_EXCEPTION(ex("could not resume process") << e_process(proc) << e_api_function("NtResumeProcess") << e_nt_status(status));
	}

	void ntSuspendProcess(const Process& proc) const
	{
		NTSTATUS status = ntSuspendProcess_(proc.handle());
		if (!NT_SUCCESS(status))
			BOOST_THROW_EXCEPTION(ex("could not suspend process") << e_process(proc) << e_api_function("NtSuspendProcess") << e_nt_status(status));
	}

	void ntSetInformationThread(const Thread& thread, MY_THREAD_INFORMATION_CLASS infoClass, void* info, unsigned long infoLength) const
	{
		NTSTATUS status = ntSetInformationThread_(thread.handle(), infoClass, info, infoLength);
		if (!NT_SUCCESS(status))
			BOOST_THROW_EXCEPTION(ex() << e_thread(thread) << e_api_function("NtSetInformationThread") << e_nt_status(status));
	}
};
