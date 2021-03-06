#include "Common.h"

extern "C" {
	DRIVER_INITIALIZE DriverEntry;
}

using _LoadLibrary = PVOID(__stdcall*)(LPCWSTR);
_LoadLibrary LoadLibrary;


VOID
ApcInjectionRoutine(
	PKAPC KApc,
	PKNORMAL_ROUTINE*,
	PVOID*, PVOID*, PVOID*
)
{
	ExFreePoolWithTag(KApc, KAPC_TAG);

	dprintf("LoadLibrary() 0x%p\n", LoadLibrary);

	wchar_t* DllBuffer {};
	ULONG_PTR DllLen { 512 };

	if (NT_SUCCESS(ZwAllocateVirtualMemory(ZwCurrentProcess(), 
	                                       reinterpret_cast<PVOID*>(&DllBuffer),
					       0, 
					       &DllLen,
					       MEM_COMMIT,
					       PAGE_READWRITE))) 
	{
		KAPC_STATE ApcState;
		KeStackAttachProcess(IoGetCurrentProcess(), &ApcState);
		RtlStringCbCopyW(DllBuffer, DllLen, L"ntoskrnl.exe");
		KeUnstackDetachProcess(&ApcState);

		auto Apc = static_cast<PKAPC>(ExAllocatePoolWithTag(NonPagedPool, 
								    sizeof(KAPC), 
								    KAPC_TAG));
		auto ApcCleanup = [](PKAPC Apc, PKNORMAL_ROUTINE*,
					PVOID*, PVOID*, PVOID*)
		{
			ExFreePoolWithTag(Apc, KAPC_TAG);
		};

		KeInitializeApc(Apc,
				KeGetCurrentThread(),
				OriginalApcEnvironment,
				ApcCleanup,
				nullptr,
				reinterpret_cast<PKNORMAL_ROUTINE>(LoadLibrary),
				UserMode,
				DllBuffer);
		if (!KeInsertQueueApc(Apc, 
		                      nullptr,
				      nullptr,
				      IO_NO_INCREMENT)) 
		{
			ExFreePoolWithTag(Apc, KAPC_TAG);
			dprintf("Failed to queue UserApc\n");
		}
	}
}

/* INJECTION used to inject ntoskrnl into notepad, otherwise
   APC test conducted to see where each routine gets called*/
#define INJECTION
VOID
ApcImageCallback(
	PUNICODE_STRING ImageName,
	HANDLE Pid,
	PIMAGE_INFO ImageInfo)
{
	if (!Pid)
		return;

	UNICODE_STRING k32 = RTL_CONSTANT_STRING(L"\\Windows\\System32\\kernel32.dll");
	if (RtlCompareUnicodeString(ImageName, &k32, TRUE) == 0) {
		
		if (strcmp(PsGetProcessImageFileName(IoGetCurrentProcess()), "notepad.exe") != 0)
			return;
		ULONG Rva;
		auto Status = Injection::KGetRoutineAddressFromModule(L"\\SystemRoot\\System32\\Kernel32.dll",
								      "LoadLibraryW",
								      &Rva);
		if (!NT_SUCCESS(Status))
			return;

		LoadLibrary = (_LoadLibrary) ((ULONG_PTR) ImageInfo->ImageBase + Rva);
		dprintf("LoadLibrary: 0x%p\n", LoadLibrary);
#if defined(INJECTION)
		auto Apc = (PKAPC) ExAllocatePoolWithTag(NonPagedPool,
							 sizeof(KAPC),
							 KAPC_TAG);
		KeInitializeApc(Apc,
				KeGetCurrentThread(),
				OriginalApcEnvironment,
				(PKKERNEL_ROUTINE) ApcInjectionRoutine,
				nullptr,
				nullptr,
				KernelMode,
				nullptr);
		if (!KeInsertQueueApc(Apc, nullptr, nullptr, IO_NO_INCREMENT)) {
			ExFreePoolWithTag(Apc, KAPC_TAG);
			return;
		}
#else
		/* APC test to see where the NormalRoutine will get executed */
		auto kApc = (PKAPC) ExAllocatePoolWithTag(NonPagedPool,
							  sizeof(KAPC),
							  KAPC_TAG);

		auto KernelRoutine = [](PKAPC Apc, PKNORMAL_ROUTINE*, PVOID*, PVOID*, PVOID*)
		{
			ExFreePoolWithTag(Apc, KAPC_TAG);
			dprintf("KernelRoutine IRQL = %d APC_LEVEL\n", KeGetCurrentIrql());
		};
		auto NormalRoutine = [](PVOID, PVOID, PVOID)
		{
			dprintf("NormalRoutine IRQL = %d PASSIVE_LEVEL\n", KeGetCurrentIrql());
		};

		KeInitializeApc(kApc,
				KeGetCurrentThread(),
				OriginalApcEnvironment,
				KernelRoutine,
				nullptr,
				(PKNORMAL_ROUTINE) LoadLibrary,
				UserMode,
				nullptr);
		KeInsertQueueApc(kApc, 0, 0, IO_NO_INCREMENT);
#endif
	}
}





NTSTATUS
DriverEntry(
	PDRIVER_OBJECT DriverObj,
	PUNICODE_STRING Registry
)
{
	UNREFERENCED_PARAMETER(Registry);

	auto Status = PsSetLoadImageNotifyRoutine(ApcImageCallback);
	if (!NT_SUCCESS(Status))
		return Status;

	DriverObj->DriverUnload = [](PDRIVER_OBJECT DriverObj)
	{
		UNREFERENCED_PARAMETER(DriverObj);
		PsRemoveLoadImageNotifyRoutine(ApcImageCallback);
	};

	return Status;
}
